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
#include <QAbstractTextDocumentLayout>
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

    std::printf("\n");
    return zt::g_failures == 0 ? 0 : 1;
}

#include "zoom_probe.moc"
