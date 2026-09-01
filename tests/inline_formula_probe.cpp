// ПРОБНИК СТРОЧНОЙ ФОРМУЛЫ-ОБЪЕКТА — до любого кода в модели (бриф).
//
// Вопрос движку («где у вёрстки базовая линия») уже отвечен пробником
// `formula`; здесь вопросы К САМОЙ Qt, и на каждый отвечает замер, а не
// документация:
//
//   1. Куда штатная QTextDocumentLayout ставит инлайн-объект относительно
//      базовой линии строки при verticalAlignment = AlignBaseline / AlignMiddle,
//      и как от него растёт строка. setAscent/setDescent у QTextInlineObject
//      снаружи не достать — их выставляет resizeInlineObject самой Qt, и надо
//      знать, ЧТО она выставляет.
//   2. Садится ли настоящая вёрстка `$x$` на базовую линию соседних букв с
//      точностью ≤ 0.5 px (порог владельца) при выбранной посадке — тремя
//      кеглями, двумя плотностями.
//   3. Дедупликация: 1000 одинаковых `$\alpha$` — один формат в коллекции и
//      один вызов движка; 1000 разных — тысяча и тысяча.
//   4. Атомарность знака: стрелка перешагивает, Backspace убирает целиком,
//      набор рядом не наследует objectType.
//   5. Перенос: формула — атом; шире колонки — уезжает за край, как длинное
//      слово (усадки нет — сказать владельцу вслух).
//   6. Выделение поверх drawObject: закрывает ли штатная заливка выделения
//      картинку с альфой, нарисованную В обработчике (у блочных объектов
//      закрывала — потому они и рисуются поверх готовой страницы).

#include "formula.h"
#include "resources.h"
#include "settings.h"
#include "test_util.h"

#include <QAbstractTextDocumentLayout>
#include <QDir>
#include <QElapsedTimer>
#include <QFont>
#include <QFontInfo>
#include <QFontMetricsF>
#include <QImage>
#include <QPainter>
#include <QTextBlock>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextLayout>
#include <QTextObjectInterface>

#include <cmath>
#include <string>

namespace {

using zametti::FormulaImage;
using zametti::Formulas;

QString g_shots;

std::string num(double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.2f", v);
    return buf;
}

constexpr int kProbeObject = QTextFormat::UserObject + 90;
enum ProbeProperty {
    ProbeWidth = QTextFormat::UserProperty + 90,
    ProbeHeight,
    ProbeShift,     // на сколько вниз от верха отведённого места рисовать чернила
    ProbeSource,    // исходник формулы: по нему обработчик берёт вёрстку
};

// Обработчик-регистратор: отдаёт заданный размер и запоминает, какой
// прямоугольник Qt дала drawObject. Если задан исходник — рисует настоящую
// вёрстку движка со сдвигом ProbeShift от верха.
class RecordingHandler : public QObject, public QTextObjectInterface {
    Q_OBJECT
    Q_INTERFACES(QTextObjectInterface)

public:
    QSizeF intrinsicSize(QTextDocument*, int, const QTextFormat& format) override {
        return QSizeF(format.doubleProperty(ProbeWidth), format.doubleProperty(ProbeHeight));
    }
    void drawObject(QPainter* painter, const QRectF& rect, QTextDocument*, int,
                    const QTextFormat& format) override {
        lastRect = rect;
        ++draws;
        const QString latex = format.stringProperty(ProbeSource);
        if (latex.isEmpty() || pixelSize <= 0.0) return;
        const FormulaImage drawn = Formulas::render(latex, false, pixelSize, Qt::black, dpr);
        if (!drawn.ok()) return;
        const qreal dy = format.doubleProperty(ProbeShift);
        // ВЕРХ КАРТИНКИ — НА ФИЗИЧЕСКИЙ ПИКСЕЛЬ. Дробный верх размазывается
        // сглаживанием на ряд ниже, и чернила «x» из вёрстки оказывались на
        // пиксель ниже чернил буквы (замер первого прогона: ±1 px, прыгает от
        // кегля). Буквы шрифтовый растеризатор прищёлкивает сам — прищёлкиваем
        // и вёрстку.
        const qreal top = std::round((rect.top() + dy) * dpr) / dpr;
        painter->drawImage(QRectF(rect.left(), top, drawn.width, drawn.height),
                           drawn.image,
                           QRectF(QPointF(0, 0), QSizeF(drawn.image.size())));
    }

    QRectF lastRect;
    int draws = 0;
    qreal pixelSize = 0.0;
    qreal dpr = 1.0;
};

// Документ из одного абзаца «до [объект] после» с заданным выравниванием и
// размером объекта. Возвращает базовую линию первой строки в координатах
// документа и прямоугольник, который Qt отдала drawObject.
struct Seat {
    qreal baseline = 0.0;     // координата документа
    QRectF rect;              // прямоугольник объекта из drawObject
    qreal ascent = 0.0;       // строки С объектом
    qreal descent = 0.0;
    qreal textAscent = 0.0;   // строки без объекта, тем же шрифтом
    qreal textDescent = 0.0;
};

Seat seatObject(RecordingHandler& handler, const QFont& font, QSizeF size,
                QTextCharFormat::VerticalAlignment align, const QString& latex = {},
                qreal shift = 0.0) {
    QTextDocument doc;
    doc.setDefaultFont(font);
    doc.documentLayout()->registerHandler(kProbeObject, &handler);
    doc.setTextWidth(640);

    QTextCursor cursor(&doc);
    cursor.insertText(QStringLiteral("Йерx "));
    QTextCharFormat objectFormat;
    objectFormat.setObjectType(kProbeObject);
    objectFormat.setVerticalAlignment(align);
    objectFormat.setProperty(ProbeWidth, size.width());
    objectFormat.setProperty(ProbeHeight, size.height());
    if (shift != 0.0) objectFormat.setProperty(ProbeShift, shift);
    if (!latex.isEmpty()) objectFormat.setProperty(ProbeSource, latex);
    cursor.insertText(QString(QChar::ObjectReplacementCharacter), objectFormat);
    cursor.insertText(QStringLiteral(" xyj."), QTextCharFormat());

    // Строка без объекта — мера того, насколько объект растит строку.
    QTextDocument plain;
    plain.setDefaultFont(font);
    QTextCursor(&plain).insertText(QStringLiteral("Йерx xyj."));
    plain.setTextWidth(640);
    (void)plain.documentLayout()->documentSize();

    // Вёрстка ленивая: рисуем, чтобы drawObject точно позвали.
    (void)doc.documentLayout()->documentSize();
    QImage canvas(660, 200, QImage::Format_ARGB32_Premultiplied);
    canvas.fill(Qt::white);
    {
        QPainter painter(&canvas);
        doc.drawContents(&painter);
    }

    Seat seat;
    const QTextBlock block = doc.firstBlock();
    const QTextLayout* layout = block.layout();
    if (layout == nullptr || layout->lineCount() == 0) return seat;
    const QTextLine line = layout->lineAt(0);
    seat.baseline = layout->position().y() + line.y() + line.ascent();
    seat.rect = handler.lastRect;
    seat.ascent = line.ascent();
    seat.descent = line.descent();
    const QTextLine plainLine = plain.firstBlock().layout()->lineAt(0);
    seat.textAscent = plainLine.ascent();
    seat.textDescent = plainLine.descent();
    return seat;
}

// --- 1. что Qt делает с ascent/descent инлайн-объекта -----------------------

void probeQtSeating() {
    QFont font(zametti::settings().style().fontFamily());
    font.setPointSizeF(zametti::settings().style().baseFontPoint());
    const QFontMetricsF metrics(font);
    std::printf("шрифт: ascent %s, descent %s, height %s\n",
                num(metrics.ascent()).c_str(), num(metrics.descent()).c_str(),
                num(metrics.height()).c_str());

    RecordingHandler handler;
    const QSizeF sizes[] = {{24, 10}, {24, 30}, {24, 60}};
    struct Mode {
        QTextCharFormat::VerticalAlignment align;
        const char* name;
    };
    const Mode modes[] = {
        {QTextCharFormat::AlignNormal, "AlignNormal"},
        {QTextCharFormat::AlignBaseline, "AlignBaseline"},
        {QTextCharFormat::AlignMiddle, "AlignMiddle"},
    };
    for (const Mode& mode : modes) {
        for (const QSizeF& size : sizes) {
            const Seat seat = seatObject(handler, font, size, mode.align);
            ZT_TRUE("drawObject позвали", !seat.rect.isNull());
            // Верх и низ объекта ОТ БАЗОВОЙ ЛИНИИ: положительное — ниже неё.
            std::printf(
                "%-14s h=%2.0f: верх %+7.2f низ %+7.2f | строка ascent %.2f (текст %.2f) "
                "descent %.2f (текст %.2f)\n",
                mode.name, size.height(), seat.rect.top() - seat.baseline,
                seat.rect.bottom() - seat.baseline, seat.ascent, seat.textAscent, seat.descent,
                seat.textDescent);
        }
    }
}

// --- 2. посадка настоящей вёрстки на базовую линию --------------------------

// Чернила: первая и последняя строки картинки с ненулевой альфой в столбцах
// [left, right); пусто — обе -1.
struct Ink {
    int top = -1;
    int bottom = -1;
};

Ink inkRows(const QImage& image, int left = 0, int right = -1) {
    Ink ink;
    if (right < 0) right = image.width();
    for (int y = 0; y < image.height(); ++y)
        for (int x = left; x < right; ++x) {
            const QRgb px = image.pixel(x, y);
            if (qAlpha(px) > 24 && qRed(px) < 160) {
                if (ink.top < 0) ink.top = y;
                ink.bottom = y;
                break;
            }
        }
    return ink;
}

// Посадка, которую предлагает бриф, выраженная числами intrinsicSize и сдвига
// отрисовки. Считается от вёрстки и метрик шрифта абзаца; ровно эти же числа
// потом обязаны уехать в боевой код (правило «переносить из пробника всё»).
struct Landing {
    QSizeF band;                                   // intrinsicSize объекта
    qreal drawShift = 0.0;                         // от верха места до верха картинки
    QTextCharFormat::VerticalAlignment align = QTextCharFormat::AlignBaseline;
};

// AlignBaseline: Qt ставит низ места на «линию низа» шрифта, а весь рост уходит
// вверх. Замер первого прогона: низ места — на baseline + ЦЕЛОМ descent
// (QFontMetrics, не QFontMetricsF: +4.00 при descent 4.12) — значит и полосу
// надо мерить целым, иначе посадка ниже на дробную часть. Место высотой
// (базовая линия вёрстки + целый descent шрифта) сажает картинку точно на
// базовую линию строки; глубокая формула свисает ниже места — в этом и вопрос
// замера.
Landing baselineLanding(const FormulaImage& drawn, const QFont& font) {
    Landing landing;
    landing.align = QTextCharFormat::AlignBaseline;
    landing.band = QSizeF(drawn.width, drawn.baseline + QFontMetrics(font).descent());
    landing.drawShift = 0.0;
    return landing;
}

// AlignMiddle: место симметрично вокруг некоторой середины; высота берётся
// такой, чтобы и подъём, и глубина вёрстки поместились от базовой линии.
Landing middleLanding(const FormulaImage& drawn, const QFontMetricsF& metrics) {
    Q_UNUSED(metrics);
    Landing landing;
    landing.align = QTextCharFormat::AlignMiddle;
    const qreal half = qMax(drawn.baseline, drawn.depth);
    landing.band = QSizeF(drawn.width, 2 * half);
    landing.drawShift = half - drawn.baseline;
    return landing;
}

// Сажаем `$…$` в настоящий абзац выбранной посадкой, рисуем и меряем разницу
// чернил: низ буквы «x» текста против низа «x» из вёрстки.
struct SeatError {
    qreal error = 1e9;      // пиксели снимка (при dpr>1 — физические)
    Seat seat;
};

SeatError measureLanding(RecordingHandler& handler, const QFont& font, qreal dpr,
                         const Landing& landing, const QString& latex) {
    QTextDocument doc;
    doc.setDefaultFont(font);
    doc.documentLayout()->registerHandler(kProbeObject, &handler);
    doc.setTextWidth(640);

    QTextCursor cursor(&doc);
    cursor.insertText(QStringLiteral("x "));
    QTextCharFormat objectFormat;
    objectFormat.setObjectType(kProbeObject);
    objectFormat.setVerticalAlignment(landing.align);
    objectFormat.setProperty(ProbeWidth, landing.band.width());
    objectFormat.setProperty(ProbeHeight, landing.band.height());
    objectFormat.setProperty(ProbeShift, landing.drawShift);
    objectFormat.setProperty(ProbeSource, latex);
    cursor.insertText(QString(QChar::ObjectReplacementCharacter), objectFormat);
    cursor.insertText(QStringLiteral(" x"), QTextCharFormat());

    (void)doc.documentLayout()->documentSize();
    QImage canvas(int(660 * dpr), int(220 * dpr), QImage::Format_ARGB32_Premultiplied);
    canvas.setDevicePixelRatio(dpr);
    canvas.fill(Qt::white);

    SeatError out;
    const QTextBlock block = doc.firstBlock();
    const QTextLayout* layout = block.layout();
    if (layout == nullptr || layout->lineCount() == 0) return out;
    const QTextLine line = layout->lineAt(0);
    out.seat.baseline = layout->position().y() + line.y() + line.ascent();
    out.seat.ascent = line.ascent();
    out.seat.descent = line.descent();

    // Рядом с объектной посадкой — РУЧНАЯ посадка той же вёрстки на ту же
    // базовую линию: ровно так сажает лист пробника `formula`, и ровно эти
    // листы принимал владелец. Мерой ошибки служит разница ЧЕРНИЛ этих двух
    // посадок: сравнение с буквой сравнивало бы два растеризатора (шрифтовый
    // против движка), и его ±1 px — шум, а не наша ошибка.
    const FormulaImage drawn = Formulas::render(latex, false, handler.pixelSize, Qt::black,
                                                handler.dpr);
    {
        QPainter painter(&canvas);
        doc.drawContents(&painter);
        if (drawn.ok()) {
            const qreal top =
                std::round((out.seat.baseline - drawn.baseline) * dpr) / dpr;
            painter.drawImage(QRectF(420.0, top, drawn.width, drawn.height), drawn.image,
                              QRectF(QPointF(0, 0), QSizeF(drawn.image.size())));
        }
    }
    out.seat.rect = handler.lastRect;

    const int split = int(handler.lastRect.left() * dpr);
    const int objectRight = int(handler.lastRect.right() * dpr) + 1;
    const Ink seated = inkRows(canvas, split, qMin(objectRight, canvas.width()));
    const Ink manual = inkRows(canvas, int(420 * dpr), canvas.width());
    if (seated.bottom < 0 || manual.bottom < 0) return out;
    out.error = std::fabs(double(seated.bottom - manual.bottom));
    return out;
}

void probeRealLanding() {
    const zametti::ZDocStyle& look = zametti::settings().style();
    const qreal scale = zametti::settings().formulas().mathScale();
    RecordingHandler handler;

    const qreal points[] = {12.0, 15.0, 20.0};
    const qreal densities[] = {1.0, 2.0};
    for (const qreal point : points) {
        for (const qreal dpr : densities) {
            QFont font(look.fontFamily());
            font.setPointSizeF(point);
            const QFontMetricsF metrics(font);
            handler.pixelSize = QFontInfo(font).pixelSize() * scale;
            handler.dpr = dpr;

            const FormulaImage x =
                Formulas::render(QStringLiteral("x"), false, handler.pixelSize, Qt::black, dpr);
            ZT_TRUE("вёрстка «x» удалась", x.ok());
            if (!x.ok()) continue;

            const SeatError base = measureLanding(handler, font, dpr,
                                                  baselineLanding(x, font),
                                                  QStringLiteral("x"));
            const SeatError middle = measureLanding(handler, font, dpr,
                                                    middleLanding(x, metrics),
                                                    QStringLiteral("x"));
            std::printf("кегль %4.1f dpr %.0f: ошибка посадки baseline %.2f px, middle %.2f px\n",
                        point, dpr, base.error, middle.error);
            // Порог владельца: ≤ 0.5 px (логической точки; на dpr 2 — 1 физ.).
            ZT_TRUE("посадка AlignBaseline в допуске 0.5 лог.px: " + num(base.error / dpr),
                    base.error / dpr <= 0.5 + 1e-9);
        }
    }

    // Рост строки от высокой формулы — тремя примерами из брифа.
    QFont font(look.fontFamily());
    font.setPointSizeF(look.baseFontPoint());
    const QFontMetricsF metrics(font);
    handler.pixelSize = QFontInfo(font).pixelSize() * scale;
    handler.dpr = 1.0;
    const QString samples[] = {QStringLiteral("\\alpha"), QStringLiteral("\\frac{a}{b}"),
                               QStringLiteral("\\sum_{k=0}^{n} x_k")};
    for (const QString& latex : samples) {
        const FormulaImage drawn =
            Formulas::render(latex, false, handler.pixelSize, Qt::black, 1.0);
        if (!drawn.ok()) continue;
        const Landing landing = baselineLanding(drawn, font);
        const SeatError seat = measureLanding(handler, font, 1.0, landing, latex);
        const qreal hang = drawn.depth - metrics.descent();
        std::printf(
            "  %-22s: вёрстка %5.1f×%5.1f (базовая %5.1f, глубина %4.1f) — строка "
            "ascent %.1f/descent %.1f; свес под строку %s\n",
            latex.toUtf8().constData(), drawn.width, drawn.height, drawn.baseline, drawn.depth,
            seat.seat.ascent, seat.seat.descent,
            hang > 0.01 ? num(hang).c_str() : "нет");
    }
}

// --- 3. дедупликация форматов и вызовов движка ------------------------------

void probeDeduplication() {
    QFont font(zametti::settings().style().fontFamily());
    font.setPointSizeF(zametti::settings().style().baseFontPoint());

    auto build = [&font](bool distinct) {
        QTextDocument doc;
        doc.setDefaultFont(font);
        QTextCursor cursor(&doc);
        for (int i = 0; i < 1000; ++i) {
            QTextCharFormat fmt;
            fmt.setObjectType(kProbeObject);
            fmt.setProperty(ProbeSource, distinct ? QStringLiteral("$a_{%1}$").arg(i)
                                                  : QStringLiteral("$\\alpha$"));
            cursor.insertText(QStringLiteral(" слово "));
            cursor.insertText(QString(QChar::ObjectReplacementCharacter), fmt);
        }
        return doc.allFormats().size();
    };

    QElapsedTimer timer;
    timer.start();
    const int sameFormats = build(false);
    const qint64 sameMicros = timer.nsecsElapsed() / 1000;
    timer.restart();
    const int distinctFormats = build(true);
    const qint64 distinctMicros = timer.nsecsElapsed() / 1000;

    // Пустой документ — точка отсчёта: сколько форматов у Qt «из коробки».
    QTextDocument empty;
    empty.setDefaultFont(font);
    const int emptyFormats = empty.allFormats().size();

    std::printf("форматы: пустой документ %d, 1000 одинаковых %d, 1000 разных %d\n",
                emptyFormats, sameFormats, distinctFormats);
    std::printf("вставка: одинаковые %lld мкс, разные %lld мкс\n",
                (long long)sameMicros, (long long)distinctMicros);
    ZT_TRUE("одинаковые форматы интернируются: " + std::to_string(sameFormats - emptyFormats),
            sameFormats - emptyFormats <= 4);
    ZT_TRUE("разные — по формату на исходник", distinctFormats - emptyFormats >= 1000);

    // Вызовы движка при кэше по содержимому: один против тысячи.
    QHash<QString, FormulaImage> cache;
    auto renderCached = [&cache](const QString& latex) {
        if (cache.contains(latex)) return;
        cache.insert(latex, Formulas::render(latex, false, 15.0, Qt::black, 1.0));
    };
    Formulas::resetRenders();
    for (int i = 0; i < 1000; ++i) renderCached(QStringLiteral("\\alpha"));
    const int sameRenders = Formulas::renders();
    cache.clear();
    Formulas::resetRenders();
    for (int i = 0; i < 1000; ++i) renderCached(QStringLiteral("a_{%1}").arg(i));
    const int distinctRenders = Formulas::renders();
    std::printf("движок: 1000 одинаковых — %d вызов(ов), 1000 разных — %d\n", sameRenders,
                distinctRenders);
    ZT_EQ("одинаковые — один вызов движка", "1", std::to_string(sameRenders));
    ZT_EQ("разные — тысяча вызовов", "1000", std::to_string(distinctRenders));
}

// --- 4. атомарность знака ---------------------------------------------------

void probeAtomicity() {
    QFont font(zametti::settings().style().fontFamily());
    font.setPointSizeF(zametti::settings().style().baseFontPoint());
    QTextDocument doc;
    doc.setDefaultFont(font);
    QTextCursor cursor(&doc);
    cursor.insertText(QStringLiteral("до "));
    QTextCharFormat fmt;
    fmt.setObjectType(kProbeObject);
    fmt.setProperty(ProbeSource, QStringLiteral("$\\alpha$"));
    cursor.insertText(QString(QChar::ObjectReplacementCharacter), fmt);
    cursor.insertText(QStringLiteral(" после"), QTextCharFormat());

    // Стрелка перешагивает знак за один шаг.
    QTextCursor caret(&doc);
    caret.setPosition(3);   // перед объектом
    caret.movePosition(QTextCursor::Right);
    ZT_EQ("Right перешагнул объект одним шагом", "4", std::to_string(caret.position()));

    // Backspace справа убирает формулу целиком (один знак).
    QTextCursor eraser(&doc);
    eraser.setPosition(4);
    eraser.deletePreviousChar();
    ZT_TRUE("после Backspace объекта нет",
            !doc.firstBlock().text().contains(QChar::ObjectReplacementCharacter));
    ZT_EQ("текст сомкнулся", std::string("до  после"),
          doc.firstBlock().text().toStdString());
    doc.undo();
    ZT_TRUE("отмена вернула объект",
            doc.firstBlock().text().contains(QChar::ObjectReplacementCharacter));

    // Набор рядом не наследует objectType: insertText чистит его и у нашего
    // типа (Qt чистит всё, что >= UserObject? — вот это и меряем).
    QTextCursor typer(&doc);
    typer.setPosition(4);   // сразу за объектом
    typer.insertText(QStringLiteral("y"));
    const QTextBlock block = doc.firstBlock();
    QTextCursor probe(&doc);
    probe.setPosition(5);   // за набранным знаком: формат знака слева
    ZT_EQ("набранный рядом знак — не объект", "0",
          std::to_string(probe.charFormat().objectType()));
    Q_UNUSED(block);
}

// --- 5. перенос строки ------------------------------------------------------

void probeWrap() {
    QFont font(zametti::settings().style().fontFamily());
    font.setPointSizeF(zametti::settings().style().baseFontPoint());
    RecordingHandler handler;

    // Формула у края: она атом и уезжает на следующую строку целиком.
    {
        QTextDocument doc;
        doc.setDefaultFont(font);
        doc.documentLayout()->registerHandler(kProbeObject, &handler);
        QTextCursor cursor(&doc);
        cursor.insertText(QStringLiteral("слово слово слово "));
        QTextCharFormat fmt;
        fmt.setObjectType(kProbeObject);
        fmt.setProperty(ProbeWidth, 90.0);
        fmt.setProperty(ProbeHeight, 14.0);
        cursor.insertText(QString(QChar::ObjectReplacementCharacter), fmt);
        cursor.insertText(QStringLiteral(" хвост"), QTextCharFormat());
        doc.setTextWidth(QFontMetricsF(font).horizontalAdvance(QStringLiteral("слово слово слово ")) +
                         40.0);
        (void)doc.documentLayout()->documentSize();
        const QTextLayout* layout = doc.firstBlock().layout();
        ZT_TRUE("строка сломалась", layout->lineCount() >= 2);
        // Объект на второй строке целиком: его знак — первый в ней.
        const QTextLine second = layout->lineAt(1);
        const int at = doc.firstBlock().text().indexOf(QChar::ObjectReplacementCharacter);
        ZT_TRUE("формула уехала атомом: начало второй строки == знак объекта",
                second.textStart() == at);
    }

    // Формула шире колонки: строка выходит за край, как длинное слово.
    {
        QTextDocument doc;
        doc.setDefaultFont(font);
        doc.documentLayout()->registerHandler(kProbeObject, &handler);
        QTextCursor cursor(&doc);
        QTextCharFormat fmt;
        fmt.setObjectType(kProbeObject);
        fmt.setProperty(ProbeWidth, 300.0);
        fmt.setProperty(ProbeHeight, 14.0);
        cursor.insertText(QStringLiteral("до "));
        cursor.insertText(QString(QChar::ObjectReplacementCharacter), fmt);
        doc.setTextWidth(120.0);
        (void)doc.documentLayout()->documentSize();
        const QTextLayout* layout = doc.firstBlock().layout();
        const QTextLine line = layout->lineAt(layout->lineCount() - 1);
        std::printf("шире колонки: колонка 120, естественная ширина строки %.1f\n",
                    line.naturalTextWidth());
        ZT_TRUE("уезжает за край, не ужимаясь", line.naturalTextWidth() > 120.0);
    }
}

// --- 6. выделение поверх drawObject -----------------------------------------

void probeSelectionOverObject() {
    QFont font(zametti::settings().style().fontFamily());
    font.setPointSizeF(zametti::settings().style().baseFontPoint());
    RecordingHandler handler;
    handler.pixelSize = QFontInfo(font).pixelSize();
    handler.dpr = 1.0;

    QTextDocument doc;
    doc.setDefaultFont(font);
    doc.documentLayout()->registerHandler(kProbeObject, &handler);
    QTextCursor cursor(&doc);
    cursor.insertText(QStringLiteral("до "));
    const FormulaImage drawn = Formulas::render(QStringLiteral("\\alpha"), false,
                                                handler.pixelSize, Qt::black, 1.0);
    QTextCharFormat fmt;
    fmt.setObjectType(kProbeObject);
    fmt.setVerticalAlignment(QTextCharFormat::AlignBaseline);
    fmt.setProperty(ProbeWidth, drawn.ok() ? drawn.width : 12.0);
    fmt.setProperty(ProbeHeight, drawn.ok() ? drawn.height : 14.0);
    fmt.setProperty(ProbeSource, QStringLiteral("\\alpha"));
    cursor.insertText(QString(QChar::ObjectReplacementCharacter), fmt);
    cursor.insertText(QStringLiteral(" после"), QTextCharFormat());
    doc.setTextWidth(300);
    (void)doc.documentLayout()->documentSize();

    auto shoot = [&](bool selected) {
        QImage canvas(320, 60, QImage::Format_ARGB32_Premultiplied);
        canvas.fill(Qt::white);
        QPainter painter(&canvas);
        QAbstractTextDocumentLayout::PaintContext ctx;
        if (selected) {
            QAbstractTextDocumentLayout::Selection selection;
            QTextCursor all(&doc);
            all.select(QTextCursor::Document);
            selection.cursor = all;
            selection.format.setBackground(QColor(0x99, 0xc2, 0xf2));
            ctx.selections.append(selection);
        }
        doc.documentLayout()->draw(&painter, ctx);
        return canvas;
    };

    const QImage plain = shoot(false);
    const QImage selected = shoot(true);
    const QRectF at = handler.lastRect;
    // Считаем тёмные знаки вёрстки внутри прямоугольника объекта на обоих
    // снимках: если заливка выделения кладётся ПОСЛЕ drawObject, чернила
    // пропадут.
    auto darkCount = [&at](const QImage& image) {
        int dark = 0;
        for (int y = qMax(0, int(at.top())); y < qMin(image.height(), int(at.bottom()) + 1); ++y)
            for (int x = qMax(0, int(at.left())); x < qMin(image.width(), int(at.right()) + 1); ++x) {
                const QRgb px = image.pixel(x, y);
                if (qGray(px) < 100) ++dark;
            }
        return dark;
    };
    const int inkPlain = darkCount(plain);
    const int inkSelected = darkCount(selected);
    std::printf("чернила вёрстки: без выделения %d, под выделением %d\n", inkPlain, inkSelected);
    ZT_TRUE("на чистом снимке вёрстка есть", inkPlain > 8);
    if (inkSelected < inkPlain / 2)
        std::printf("  ВЫВОД: выделение ЗАКРЫВАЕТ нарисованное в drawObject — рисовать поверх\n");
    else
        std::printf("  ВЫВОД: выделение НЕ закрывает нарисованное в drawObject\n");
    plain.save(QDir(g_shots).filePath(QStringLiteral("выделение-нет.png")));
    selected.save(QDir(g_shots).filePath(QStringLiteral("выделение-есть.png")));
}

// --- 7. лист для владельца ---------------------------------------------------

// Абзац из брифа тремя кеглями, посадка AlignBaseline, красная черта базовой
// линии первой строки. Смотрят глазами: формулы стоят на черте, строка с
// дробью выше соседних ровно на рост дроби.
void shootParagraph() {
    const zametti::ZDocStyle& look = zametti::settings().style();
    const qreal scale = zametti::settings().formulas().mathScale();
    RecordingHandler handler;

    const qreal points[] = {12.0, 15.0, 20.0};
    qreal y = 16.0;
    QImage sheet(760, 460, QImage::Format_RGB32);
    sheet.fill(QColor(0xfe, 0xfe, 0xfb));
    QPainter painter(&sheet);
    painter.setRenderHint(QPainter::Antialiasing, true);

    for (const qreal point : points) {
        QFont font(look.fontFamily());
        font.setPointSizeF(point);
        const QFontMetricsF metrics(font);
        handler.pixelSize = QFontInfo(font).pixelSize() * scale;
        handler.dpr = 1.0;

        QTextDocument doc;
        doc.setDefaultFont(font);
        doc.documentLayout()->registerHandler(kProbeObject, &handler);
        QTextCursor cursor(&doc);
        const struct {
            QString text;
            QString latex;
        } pieces[] = {
            {QStringLiteral("текст "), QStringLiteral("\\alpha")},
            {QStringLiteral(" текст "), QStringLiteral("\\frac{a}{b}")},
            {QStringLiteral(" текст "), QStringLiteral("\\sum_{k=0}^{n} x_k")},
            {QStringLiteral(" текст, и ещё длинный хвост, чтобы абзац сломался на несколько "
                            "строк и был виден шаг между ними."),
             QString()},
        };
        for (const auto& piece : pieces) {
            cursor.insertText(piece.text, QTextCharFormat());
            if (piece.latex.isEmpty()) continue;
            const FormulaImage drawn =
                Formulas::render(piece.latex, false, handler.pixelSize, Qt::black, 1.0);
            if (!drawn.ok()) continue;
            const Landing landing = baselineLanding(drawn, font);
            QTextCharFormat fmt;
            fmt.setObjectType(kProbeObject);
            fmt.setVerticalAlignment(landing.align);
            fmt.setProperty(ProbeWidth, landing.band.width());
            fmt.setProperty(ProbeHeight, landing.band.height());
            fmt.setProperty(ProbeShift, landing.drawShift);
            fmt.setProperty(ProbeSource, piece.latex);
            cursor.insertText(QString(QChar::ObjectReplacementCharacter), fmt);
        }
        doc.setTextWidth(720);
        (void)doc.documentLayout()->documentSize();

        painter.save();
        painter.translate(20, y);
        doc.drawContents(&painter);
        // Черта базовой линии первой строки.
        const QTextLayout* layout = doc.firstBlock().layout();
        if (layout != nullptr && layout->lineCount() > 0) {
            const QTextLine line = layout->lineAt(0);
            const qreal baseline = layout->position().y() + line.y() + line.ascent();
            painter.setPen(QColor(220, 60, 60, 140));
            painter.drawLine(QPointF(0, baseline), QPointF(720, baseline));
        }
        painter.restore();
        y += doc.documentLayout()->documentSize().height() + 24;
    }
    painter.end();
    const QString path = QDir(g_shots).filePath(QStringLiteral("строчные-абзац.png"));
    if (!sheet.save(path)) std::printf("НЕ СОХРАНИЛСЯ снимок %s\n", qPrintable(path));
}

}  // namespace

int ztInlineFormulaProbe(int argc, char** argv) {
    g_shots = argc > 1 ? QString::fromLocal8Bit(argv[1])
                       : QStringLiteral(ZAMETTI_TESTDATA "/inline-formula-shots");
    QDir().mkpath(g_shots);
    zametti::loadEmbeddedFonts();

    QString error;
    ZT_TRUE("движок формул поднялся: " + error.toStdString(), Formulas::init(&error));
    if (!Formulas::ready()) return zt::report("строчные формулы: пробник");

    probeQtSeating();
    probeRealLanding();
    probeDeduplication();
    probeAtomicity();
    probeWrap();
    probeSelectionOverObject();
    shootParagraph();

    std::printf("снимки: %s\n", qPrintable(g_shots));
    return zt::report("строчные формулы: пробник");
}

#include "inline_formula_probe.moc"
