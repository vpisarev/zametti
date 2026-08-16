// ВОРОТА К ZDocument: что происходит с документом, когда человек жмёт Ctrl+=.
//
// Вопрос владельца, и от ответа зависит вся дальнейшая работа. Правильный
// ответ — «ничего»: документ хранит СЕМАНТИКУ, а облик накладывает вид. Из
// нарушения этого правила выросла половина мелких бед этапа 16 («состояние
// показа, записанное в живой документ»).
//
// Но механизма для такого ответа у QTextDocument нет даром: он держит
// разрешённые шрифты прямо в QTextCharFormat. Сегодня зум делает ПОЛНУЮ
// пересборку (applyZoom → refreshAppearance → buildDocument), и довод записан
// в шапке сборщика: «кегль задан явно в каждом формате».
//
// Владелец принёс ответ — docs/zametti-zoom.md, свойство FontSizeAdjustment.
// Обещано: Qt держит семь ступеней и умножает их на defaultFont, а
// markdown-импортёр самого Qt так и делает для заголовков. Тогда документ
// можно построить БЕЗ единого абсолютного кегля, и весь шрифтовой зум — это
// один setDefaultFont.
//
// Пробник проверяет пять вещей, и все с числами:
//
//   1. те ли семь коэффициентов и вправду ли они от defaultFont;
//   2. ТРОГАЕТ ЛИ setDefaultFont СТЕК ОТМЕНЫ — главный вопрос: от него зависит,
//      может ли отмена стать родной или останется на снимках IR;
//   3. зовётся ли intrinsicSize заново — поедут ли формулы и маркеры вместе;
//   4. сколько стоит смена масштаба против 151 мс полной сборки;
//   5. что делается с геометрией, если её НЕ масштабировать.

#include "test_util.h"

#include <QElapsedTimer>
#include <QFontMetricsF>
#include <QTextBlock>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFrame>
#include <QTextFrameFormat>
#include <QAbstractTextDocumentLayout>
#include <QImage>
#include <QPainter>
#include <QTextLayout>
#include <QTextLine>
#include <cmath>
#include <string>
#include <QTextObjectInterface>

#include <algorithm>

#include <cstdio>

namespace {

// Кегль знака, как его в итоге видит вёрстка.
qreal shownSize(const QTextDocument& doc, int blockNumber) {
    const QTextBlock block = doc.findBlockByNumber(blockNumber);
    if (!block.isValid()) return 0;
    for (auto it = block.begin(); it != block.end(); ++it) {
        if (it.fragment().isValid()) {
            const QFont f = it.fragment().charFormat().font();
            return QFontMetricsF(f).height();
        }
    }
    return QFontMetricsF(doc.defaultFont()).height();
}

// Объект, который считает свой размер от defaultFont. Ровно так должны вести
// себя формулы и маркеры списка, если зум делается сменой шрифта.
class SizeFromFont : public QObject, public QTextObjectInterface {
    Q_OBJECT
    Q_INTERFACES(QTextObjectInterface)

public:
    int asked = 0;

    QSizeF intrinsicSize(QTextDocument* doc, int, const QTextFormat&) override {
        ++asked;
        const qreal unit = QFontMetricsF(doc->defaultFont()).height();
        return QSizeF(unit * 3.0, unit * 1.5);
    }
    void drawObject(QPainter*, const QRectF&, QTextDocument*, int, const QTextFormat&) override {}
};

constexpr int kObjectType = QTextFormat::UserObject + 17;

}  // namespace

int ztZoomProbe(int argc, char** argv) {
    (void)argc;
    (void)argv;

    // --- 1. FontSizeAdjustment: те ли ступени и от чего они считаются -------
    {
        QTextDocument doc;
        doc.setLayoutEnabled(false);
        QFont base = doc.defaultFont();
        base.setPointSizeF(12.0);
        doc.setDefaultFont(base);

        QTextCursor cursor(&doc);
        for (int step = -1; step <= 5; ++step) {
            if (step != -1) cursor.insertBlock();
            QTextCharFormat fmt;
            fmt.setProperty(QTextFormat::FontSizeAdjustment, step);
            cursor.insertText(QStringLiteral("текст"), fmt);
        }

        std::printf("\n1. FontSizeAdjustment при defaultFont 12 pt\n");
        std::printf("   %-10s %-12s %-12s\n", "ступень", "высота", "во сколько раз");
        const qreal unit = QFontMetricsF(base).height();
        for (int i = 0; i < 7; ++i) {
            const qreal h = shownSize(doc, i);
            std::printf("   %-10d %-12.2f %-12.3f\n", i - 1, h, unit > 0 ? h / unit : 0.0);
        }

        // И то же самое после смены defaultFont: если ступени и правда
        // относительные, все семь поедут вместе.
        QFont bigger = base;
        bigger.setPointSizeF(18.0);
        doc.setDefaultFont(bigger);
        std::printf("   после setDefaultFont(18 pt):\n");
        const qreal unitBig = QFontMetricsF(bigger).height();
        bool allMoved = true;
        for (int i = 0; i < 7; ++i) {
            const qreal h = shownSize(doc, i);
            const qreal ratio = unitBig > 0 ? h / unitBig : 0.0;
            std::printf("   %-10d %-12.2f %-12.3f\n", i - 1, h, ratio);
            if (h <= 0) allMoved = false;
        }
        ZT_TRUE("все ступени пережили смену шрифта", allMoved);
    }

    // --- 2. ГЛАВНЫЙ ВОПРОС: трогает ли setDefaultFont стек отмены -----------
    {
        QTextDocument doc;
        doc.setUndoRedoEnabled(true);
        QTextCursor cursor(&doc);
        cursor.insertText(QStringLiteral("мама мыла раму"));

        const bool canUndoBefore = doc.isUndoAvailable();
        const int stepsBefore = doc.availableUndoSteps();

        QFont f = doc.defaultFont();
        f.setPointSizeF(f.pointSizeF() * 1.5);
        doc.setDefaultFont(f);

        const bool canUndoAfter = doc.isUndoAvailable();
        const int stepsAfter = doc.availableUndoSteps();

        std::printf("\n2. Стек отмены и setDefaultFont\n");
        std::printf("   до:    отмена доступна %s, шагов %d\n",
                    canUndoBefore ? "да" : "нет", stepsBefore);
        std::printf("   после: отмена доступна %s, шагов %d\n",
                    canUndoAfter ? "да" : "нет", stepsAfter);

        // Сама проверка: отмена возвращает ТЕКСТ, а не масштаб.
        doc.undo();
        const QString afterUndo = doc.toPlainText();
        const qreal sizeAfterUndo = doc.defaultFont().pointSizeF();
        std::printf("   Ctrl+Z вернул текст [%s], кегль остался %.1f\n",
                    qPrintable(afterUndo), sizeAfterUndo);

        ZT_EQ("шагов отмены не прибавилось", std::to_string(stepsBefore),
              std::to_string(stepsAfter));
        ZT_EQ("Ctrl+Z убрал ТЕКСТ, а не масштаб", std::string(), afterUndo.toStdString());
        ZT_TRUE("масштаб пережил отмену", sizeAfterUndo > 11.0);
    }

    // --- 3. Зовётся ли intrinsicSize заново --------------------------------
    {
        QTextDocument doc;
        SizeFromFont handler;
        doc.documentLayout()->registerHandler(kObjectType, &handler);

        QTextCursor cursor(&doc);
        QTextCharFormat fmt;
        fmt.setObjectType(kObjectType);
        cursor.insertText(QString(QChar::ObjectReplacementCharacter), fmt);
        doc.setTextWidth(400);
        const int askedFirst = handler.asked;

        QFont f = doc.defaultFont();
        f.setPointSizeF(f.pointSizeF() * 2.0);
        doc.setDefaultFont(f);
        doc.setTextWidth(400);
        const int askedAfter = handler.asked;

        std::printf("\n3. Объект, считающий размер от defaultFont\n");
        std::printf("   спрошен до смены шрифта: %d раз, после: %d раз\n",
                    askedFirst, askedAfter);
        ZT_TRUE("после смены шрифта объект спросили заново", askedAfter > askedFirst);
    }

    // --- 4. Цена смены масштаба -------------------------------------------
    {
        QTextDocument doc;
        QTextCursor cursor(&doc);
        // Заметка размером с самую большую у владельца: 4000 блоков.
        for (int i = 0; i < 4000; ++i) {
            if (i) cursor.insertBlock();
            cursor.insertText(QStringLiteral("Строка номер %1 с некоторым количеством текста, "
                                             "чтобы вёрстка была не пустой").arg(i));
        }
        doc.setTextWidth(800);
        (void)doc.documentLayout()->documentSize();

        double best = 1e18;
        for (int run = 0; run < 5; ++run) {
            QFont f = doc.defaultFont();
            f.setPointSizeF(11.0 + double(run % 2));
            QElapsedTimer t;
            t.start();
            doc.setDefaultFont(f);
            (void)doc.documentLayout()->documentSize();
            best = std::min(best, double(t.nsecsElapsed()) / 1e6);
        }
        std::printf("\n4. Смена масштаба на 4000 блоках: %.1f мс\n", best);
        std::printf("   для сравнения: полная сборка заметки в 239 КБ — 151 мс\n");
    }

    // --- 5. Что будет с геометрией, если её не трогать ---------------------
    {
        QTextDocument doc;
        QTextCursor cursor(&doc);
        QTextBlockFormat block;
        block.setTopMargin(8);
        block.setBottomMargin(8);
        cursor.setBlockFormat(block);
        cursor.insertText(QStringLiteral("абзац"));

        const qreal marginBefore = doc.begin().blockFormat().topMargin();
        QFont f = doc.defaultFont();
        f.setPointSizeF(f.pointSizeF() * 2.0);
        doc.setDefaultFont(f);
        const qreal marginAfter = doc.begin().blockFormat().topMargin();

        std::printf("\n5. Поля блока при удвоении шрифта: было %.1f, стало %.1f\n",
                    marginBefore, marginAfter);
        std::printf("   то есть геометрия НЕ едет — её пришлось бы либо объявить\n");
        std::printf("   неизменной (как текстовый зум в браузере), либо проходить\n");
        std::printf("   по документу отдельно.\n");
        ZT_EQ("поля остались прежними", std::to_string(int(marginBefore)),
              std::to_string(int(marginAfter)));
    }


    // --- 6. КРАСИТ ЛИ Qt ВЫДЕЛЕНИЕ ПО ВСЕЙ ОТВЕДЁННОЙ ПОЛОСЕ ----------------
    //
    // Вопрос владельца: отрисовка текста отдана Qt, значит и выделение без
    // разрывов — его работа. Проверяем прямо: один и тот же документ с одной и
    // той же высотой строки, заданной ТРЕМЯ способами, красится выделением — и
    // считаем ряды, оставшиеся незакрашенными.
    {
        struct Case {
            const char* name;
            QTextBlockFormat::LineHeightTypes type;
            qreal value;
        };

        // Естественную высоту строки узнаём заранее: от неё считается доля.
        qreal natural = 0.0;
        {
            QTextDocument probe;
            probe.setTextWidth(400);
            QTextCursor c(&probe);
            c.insertText(QStringLiteral("Ы"));
            probe.documentLayout()->documentSize();   // заставляем разметить
            const QTextLayout* text = probe.begin().layout();
            if (text != nullptr && text->lineCount() > 0)
                natural = text->lineAt(0).height();
            if (natural <= 0.0) natural = QFontMetricsF(probe.defaultFont()).height();
        }

        const Case cases[] = {
            {"естественная (без свойства)", QTextBlockFormat::SingleHeight, 0},
            {"доля 115 %", QTextBlockFormat::ProportionalHeight, 115},
            {"пиксели, столько же", QTextBlockFormat::FixedHeight, std::round(natural * 1.15)},
            {"пиксели, ровно естественная", QTextBlockFormat::FixedHeight, natural},
        };

        std::printf("\n6. Разрывы в выделении. Естественная высота строки %.3f\n", natural);
        std::printf("   %-30s %-10s %-12s %-8s\n", "чем задана", "назначено", "полоса", "пусто");

        for (const Case& one : cases) {
            QTextDocument doc;
            QTextCursor cursor(&doc);
            for (int i = 0; i < 4; ++i) {
                QTextBlockFormat block;
                block.setLineHeight(one.value, one.type);
                if (i > 0) cursor.insertBlock(block);
                else cursor.setBlockFormat(block);
                cursor.insertText(QStringLiteral("строка выделения"));
            }
            doc.setTextWidth(400);

            QImage shot(400, 240, QImage::Format_ARGB32);
            shot.fill(Qt::white);
            {
                QPainter painter(&shot);
                QAbstractTextDocumentLayout::PaintContext ctx;
                QAbstractTextDocumentLayout::Selection selection;
                QTextCursor whole(&doc);
                whole.movePosition(QTextCursor::End, QTextCursor::KeepAnchor);
                selection.cursor = whole;
                selection.format.setBackground(QColor(0, 0, 255));
                selection.format.setForeground(QColor(255, 255, 255));
                ctx.selections.append(selection);
                doc.documentLayout()->draw(&painter, ctx);
            }

            int first = -1;
            int last = -1;
            for (int y = 0; y < shot.height(); ++y)
                for (int x = 0; x < shot.width(); ++x)
                    if (qBlue(shot.pixel(x, y)) > 200 && qRed(shot.pixel(x, y)) < 100) {
                        if (first < 0) first = y;
                        last = y;
                        break;
                    }
            int empty = 0;
            for (int y = first; y >= 0 && y <= last; ++y) {
                bool any = false;
                for (int x = 0; x < shot.width() && !any; ++x)
                    any = qBlue(shot.pixel(x, y)) > 200 && qRed(shot.pixel(x, y)) < 100;
                if (!any) ++empty;
            }
            const qreal assigned = doc.begin().blockFormat().lineHeight(natural, 1.0);
            std::printf("   %-30s %-10.2f %-12s %-8d\n", one.name, assigned,
                        (std::to_string(first) + ".." + std::to_string(last)).c_str(), empty);
        }
        std::printf("   Пусто > 0 значит: Qt оставляет ряды незакрашенными сам.\n");

        // От ЧЕГО Qt считает долю. Спрашивать QTextLine::height() мало: он
        // отдаёт округлённое вверх, а шаг разметки идёт по неокруглённому.
        {
            QTextDocument doc;
            QTextCursor cursor(&doc);
            for (int i = 0; i < 3; ++i) {
                QTextBlockFormat block;
                block.setLineHeight(115, QTextBlockFormat::ProportionalHeight);
                if (i > 0) cursor.insertBlock(block);
                else cursor.setBlockFormat(block);
                cursor.insertText(QStringLiteral("строка"));
            }
            doc.setTextWidth(400);
            doc.documentLayout()->documentSize();   // заставляем разметить
            const QTextBlock first = doc.begin();
            if (first.layout() == nullptr || first.layout()->lineCount() == 0) {
                std::printf("   от чего доля: разметки нет, замер не вышел\n");
                return zt::freshFailures();
            }
            const QTextLine line = first.layout()->lineAt(0);
            const QFontMetricsF metrics(first.charFormat().font());
            const qreal step =
                doc.documentLayout()->blockBoundingRect(first.next()).top() -
                doc.documentLayout()->blockBoundingRect(first).top();
            std::printf("   от чего доля: шаг %.3f; line.height %.3f, ascent+descent %.3f,\n"
                        "                 leading %.3f, метрики height %.3f, lineSpacing %.3f\n",
                        step, line.height(), line.ascent() + line.descent(), line.leading(),
                        metrics.height(), metrics.lineSpacing());
            std::printf("                 шаг / 1.15 = %.3f\n", step / 1.15);
        }
    }

    // --- 7. ЧТО ИЗ НАШИХ ЗАПИСЕЙ ПОПАДАЕТ В СТЕК ОТМЕНЫ ---------------------
    //
    // Отмена у нас своя (снимки логических блоков), а штатный стек выключен, и
    // довод записан один: «мы его загрязняем». Довод не проверен ни разу.
    // Проверяем поимённо — по одной записи на строку, каждая на чистом
    // документе, и смотрим, прибавилось ли шагов.
    //
    // Список взят не с потолка: это ровно те места, где ВИД пишет в документ,
    // и каждое из них уже помечено в коде флагом changingLayout_.
    {
        std::printf("\n7. Что попадает в стек отмены (шагов было → стало)\n");

        struct Probe {
            const char* name;
            void (*apply)(QTextDocument&);
        };

        const Probe probes[] = {
            {"setDefaultFont (зум)",
             [](QTextDocument& d) {
                 QFont f = d.defaultFont();
                 f.setPointSizeF(f.pointSizeF() * 1.5);
                 d.setDefaultFont(f);
             }},
            {"setTextWidth (ширина колонки)", [](QTextDocument& d) { d.setTextWidth(300); }},
            {"markContentsDirty",
             [](QTextDocument& d) { d.markContentsDirty(0, d.characterCount()); }},
            {"setIndentWidth", [](QTextDocument& d) { d.setIndentWidth(40); }},
            {"setDocumentMargin (поле документа)",
             [](QTextDocument& d) { d.setDocumentMargin(40); }},
            {"rootFrame setFrameFormat (поля рамки, applyContentWidth)",
             [](QTextDocument& d) {
                 QTextFrameFormat f = d.rootFrame()->frameFormat();
                 f.setLeftMargin(f.leftMargin() + 10);
                 d.rootFrame()->setFrameFormat(f);
             }},
            {"setBlockFormat, ДРУГОЕ поле (syncImageSpace, syncGaps)",
             [](QTextDocument& d) {
                 QTextCursor c(d.firstBlock());
                 QTextBlockFormat f = c.blockFormat();
                 f.setBottomMargin(f.bottomMargin() + 10);
                 c.setBlockFormat(f);
             }},
            {"setBlockFormat, ТО ЖЕ значение",
             [](QTextDocument& d) {
                 QTextCursor c(d.firstBlock());
                 c.setBlockFormat(c.blockFormat());
             }},
            {"три setBlockFormat в одной скобке begin/endEditBlock",
             [](QTextDocument& d) {
                 QTextCursor c(&d);
                 c.beginEditBlock();
                 for (QTextBlock b = d.begin(); b.isValid(); b = b.next()) {
                     QTextCursor at(b);
                     QTextBlockFormat f = at.blockFormat();
                     f.setTopMargin(f.topMargin() + 3);
                     at.setBlockFormat(f);
                 }
                 c.endEditBlock();
             }},
        };

        for (const Probe& one : probes) {
            QTextDocument doc;
            doc.setUndoRedoEnabled(true);
            QTextCursor cursor(&doc);
            cursor.insertText(QStringLiteral("первый абзац"));
            cursor.insertBlock();
            cursor.insertText(QStringLiteral("второй абзац"));
            cursor.insertBlock();
            cursor.insertText(QStringLiteral("третий абзац"));
            doc.setTextWidth(400);
            (void)doc.documentLayout()->documentSize();

            const QString whole = doc.toPlainText();
            const int before = doc.availableUndoSteps();
            one.apply(doc);
            const int after = doc.availableUndoSteps();

            // ГЛАВНОЕ ЧИСЛО — не длина стека, а сколько раз человеку придётся
            // нажать Ctrl+Z, прежде чем отмена доберётся до его собственного
            // текста. Длина стека у Qt считает команды, а человек считает
            // нажатия, и это разные числа.
            int wasted = 0;
            while (wasted < 20 && doc.isUndoAvailable()) {
                doc.undo();
                if (doc.toPlainText() != whole) break;   // добрались до текста
                ++wasted;
            }

            std::printf("   %-56s %d → %-3d  %s\n", one.name, before, after,
                        after > before
                            ? (std::string("ЗАСОРИЛ: ") + std::to_string(wasted) +
                               " нажатий Ctrl+Z впустую").c_str()
                            : "чисто");
        }
        std::printf("   «чисто» значит: запись в стек не попала, Ctrl+Z её не видит.\n");
    }

    std::printf("\n");
    return zt::freshFailures();
}

#include "zoom_probe.moc"
