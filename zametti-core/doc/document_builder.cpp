// Логические блоки → QTextDocument.
//
// АБСОЛЮТНЫХ КЕГЛЕЙ ЗДЕСЬ НЕТ НИ ОДНОГО. Размер знака задаётся ступенью от
// шрифта документа (лестница в doc_model.h), поэтому Ctrl+= это один
// setDefaultFont: форматы не трогаются, в стек отмены масштаб не попадает,
// пересборки нет. По той же причине сборщику не передают zoom — ему нечего с
// ним делать, а тому, кто масштаб всё-таки меняет, достаточно шрифта
// документа.
//
// Геометрия (поля блоков, отступы, плашка кода) остаётся в пикселях и строится
// один раз. Это осознанный выбор: em-единиц у QTextBlockFormat нет вовсе.
//
// Здесь и только здесь UTF-8 ядра превращается в UTF-16 Qt. Смещения кусков
// строки заданы в байтах, индексы QString — в кодовых единицах UTF-16;
// приравнивать их нельзя, ошибка проявится только на не-ASCII. Пересчёт идёт
// накопительно по кускам, идущим по порядку.
//
// Маркеры списков не попадают в документ вовсе: здесь под них только
// резервируется левое поле, а рисует их NoteView по геометрии строки (см.
// marker.h). Причина та же, по которой отвергнут QTextList: его маркер нельзя
// ни покрасить, ни увеличить, ни сдвинуть, а чекбоксу всё это нужно. Заодно
// уходит навязанный Qt отступ — пункт верхнего уровня встаёт вровень с абзацем,
// как и в самом файле. Плата — нумерацию приходится вести самим, ровно тем же
// правилом, что и в сериализаторе: прогон живёт на каждом уровне и переживает
// вложенный подсписок.

#include "document_builder.h"
#include "table.h"

#include "doc_model.h"

#include "editor_ops.h"
#include "marker.h"
#include "settings.h"

#include <QColor>
#include <QFont>
#include <QFontMetricsF>
#include <QRawFont>
#include <QString>
#include <QTextBlockFormat>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QVariant>
#include <QTextFrame>
#include <QTextFrameFormat>
#include <QTextOption>


#include <algorithm>
#include <vector>

namespace zametti {
namespace {

// Перевод строки внутри блока IR — это перенос внутри того же абзаца, а не
// новый абзац. QChar::LineSeparator даёт ровно это и, в отличие от '\n', не
// разрывает QTextBlock. Длина в кодовых единицах та же, так что смещения спанов
// остаются верными.
// Позиция знака, заменённого разделителем строк, и то, чем он был.
struct Break {
    int at;
    int source;
};

// Текст блока — в документ: три знака, на которых Qt рвёт блок ('\n', '\r',
// U+2029), становятся разделителем строк U+2028, а какой знак был — помнит
// пометка (см. BreakSource в doc_model.h).
QString toQt(const QString& text, std::vector<Break>& breaks) {
    QString s = text;
    for (qsizetype i = 0; i < s.size(); ++i) {
        int source = 0;
        if (s[i] == QLatin1Char('\n')) source = BreakNewline;
        else if (s[i] == QLatin1Char('\r')) source = BreakCarriageReturn;
        else if (s[i] == QChar::ParagraphSeparator) source = BreakParagraph;
        if (source == 0) continue;
        s[i] = QChar::LineSeparator;
        breaks.push_back({static_cast<int>(i), source});
    }
    return s;
}

// Помечает подменённые разделители. Пометка ложится на один знак, поэтому он
// становится отдельным куском блока — читателю только это и нужно.
void markBreaks(QTextDocument& doc, int textStart, const std::vector<Break>& breaks) {
    QTextCursor cursor(&doc);
    for (const Break& b : breaks) {
        QTextCharFormat fmt;
        fmt.setProperty(BreakSourceProperty, b.source);
        cursor.setPosition(textStart + b.at);
        cursor.setPosition(textStart + b.at + 1, QTextCursor::KeepAnchor);
        cursor.mergeCharFormat(fmt);
    }
}

// Символы, которых нет в основной гарнитуре, рисуются запасным шрифтом — это
// прежде всего эмодзи. Опознаём их не по диапазонам кодов, а по факту:
// «нет глифа в основном шрифте». Так правило не устареет вместе с Unicode.
//
// Спрашивать надо именно QRawFont: он описывает одну физическую гарнитуру.
// QFontMetrics отвечает за целую цепочку с запасными шрифтами и на эмодзи
// говорит «есть», отчего увеличение не срабатывало вовсе.
// Разделители строк и абзацев глифа не имеют вовсе, и увеличивать их незачем.
// Шрифт про них не знает, поэтому без этой оговорки они попадали под правило для
// эмодзи — а текст, набранный сразу после переноса, наследовал увеличенный
// формат и выходил крупнее соседей.
bool needsFallback(char32_t cp, const QRawFont& primary) {
    if (cp == 0x2028 || cp == 0x2029) return false;
    return !primary.supportsCharacter(cp);
}

void enlargeFallbackGlyphs(QTextDocument& doc, int textStart, const QString& text,
                           int surroundingStep, const QRawFont& primary,
                           const ZDocStyle& style) {
    QTextCursor cursor(&doc);
    int i = 0;
    while (i < text.size()) {
        const bool pair = text[i].isHighSurrogate() && i + 1 < text.size() &&
                          text[i + 1].isLowSurrogate();
        const char32_t cp = pair ? QChar::surrogateToUcs4(text[i], text[i + 1])
                                 : char32_t(text[i].unicode());
        if (!needsFallback(cp, primary)) {
            i += pair ? 2 : 1;
            continue;
        }

        // Составные эмодзи (модификаторы тона, склейка через ZWJ) берём одним
        // куском: разный кегль внутри последовательности её бы разорвал.
        const int begin = i;
        while (i < text.size()) {
            const bool p = text[i].isHighSurrogate() && i + 1 < text.size() &&
                           text[i + 1].isLowSurrogate();
            const char32_t c = p ? QChar::surrogateToUcs4(text[i], text[i + 1])
                                 : char32_t(text[i].unicode());
            if (!needsFallback(c, primary)) break;
            i += p ? 2 : 1;
        }

        // Ступень отсчитывается ОТ ОКРУЖАЮЩЕГО текста, а не от шрифта
        // документа: эмодзи внутри заголовка обязан ехать вместе с заголовком.
        QTextCharFormat fmt;
        setFontStep(fmt, surroundingStep + style.fallbackStep());
        cursor.setPosition(textStart + begin);
        cursor.setPosition(textStart + i, QTextCursor::KeepAnchor);
        cursor.mergeCharFormat(fmt);
    }
}

// Ступень кода — от ступени окружающего текста. В обычном абзаце окружение
// нулевое, и код получает ровно settings().style().codeStep(); в заголовке он едет
// вместе с заголовком.
int codeStepIn(int surroundingStep, const ZDocStyle& style) {
    return surroundingStep + style.codeStep();
}

void applySpans(QTextDocument& doc, int textStart, const Piece& b, int lineStep,
                const ZDocStyle& style) {
    QTextCursor cursor(&doc);
    // Смещения кусков — единицы UTF-16 от начала текста блока, те же, что и в
    // документе: пересчитывать нечего.
    const int size = int(b.text.size());
    for (const Run& s : b.runs) {
        if (s.empty()) continue;
        const int from = qBound(0, int(s.start), size);
        const int to = qBound(0, int(s.end), size);
        if (to <= from) continue;

        // Стиль записывается свойством, а не выводится обратно из оформления:
        // заголовок набран жирным целиком, и «жирный» внутри него по весу
        // шрифта было бы не отличить от самого заголовка.
        int bits = 0;
        if (s.bold()) bits |= SpanBold;
        if (s.italic()) bits |= SpanItalic;
        if (s.strike()) bits |= SpanStrike;
        if (s.code()) bits |= SpanCode;
        if (s.image()) bits |= SpanImage;
        if (s.math()) bits |= SpanMath;
        if (s.comment()) bits |= SpanComment;

        QTextCharFormat fmt;
        if (bits != 0) fmt.setProperty(SpanStyleProperty, bits);
        if (s.bold()) fmt.setFontWeight(QFont::Bold);
        if (s.italic()) fmt.setFontItalic(true);
        if (s.strike()) fmt.setFontStrikeOut(true);
        if (s.code()) {
            fmt.setBackground(style.codeBackground());
            setFontStep(fmt, codeStepIn(lineStep, style));
            if (!style.codeFamily().isEmpty())
                fmt.setFontFamilies({QString(style.codeFamily())});
        }
        if (!s.href.isEmpty()) {
            fmt.setAnchor(true);
            fmt.setAnchorHref(s.href);
            fmt.setForeground(style.linkColor());
            fmt.setFontUnderline(true);
        }
        if (s.comment()) fmt.setForeground(style.rawColor());
        if (!s.title.isEmpty()) fmt.setProperty(SpanTitleProperty, s.title);
        cursor.setPosition(textStart + from);
        cursor.setPosition(textStart + to, QTextCursor::KeepAnchor);
        cursor.mergeCharFormat(fmt);
    }
}

}  // namespace

bool pieceIsImageObject(const Piece& piece) {
    if (piece.raw || piece.kind != Kind::Paragraph) return false;
    // Image-спан целым абзацем: один кусок, помеченный картинкой, покрывающий
    // текст блока без остатка. Картинка в середине текста объектом не бывает —
    // она живёт внутри строки и показывается стилем.
    const QString& text = piece.text;
    if (piece.runs.size() == 1 && piece.runs[0].image() && piece.runs[0].start == 0 &&
        piece.runs[0].end == text.size())
        return true;
    // Вики-вложение Obsidian: строка целиком "![[путь]]". Разметки у неё нет —
    // это дословный текст абзаца, и объектом он становится целиком.
    return text.startsWith(QLatin1String("![[")) && text.size() > 5 &&
           text.endsWith(QLatin1String("]]"));
}

bool pieceIsFormulaObject(const Piece& piece) {
    return !piece.raw && piece.kind == Kind::Math && !piece.text.isEmpty();
}

bool pieceIsTableObject(const Piece& piece) {
    // Признак ставит разбор (md4c) или обход живого документа (объект был);
    // разбор таблиц для показа обязан согласиться — иначе показывать нечего,
    // и кусок остаётся дословным текстом.
    return piece.raw && piece.table && parseTable(piece.text).valid;
}

qreal blockTopMargin(Kind kind, bool raw, bool previousIsVSpace, bool first,
                     const ZDocStyle& style) {
    // У первого блока отбивке сверху взяться неоткуда: над ним поле страницы.
    if (first) return 0.0;

    // Прогон пустых строк обрамляется своими полями: сверху перед первой, снизу
    // после последней. Внутри прогона — ничего, иначе высота разделителя из n
    // строк перестала бы быть n высотами строки.
    if (!raw && kind == Kind::VSpace)
        return previousIsVSpace ? 0.0 : style.separatorSpacingBefore();
    if (previousIsVSpace) return style.separatorSpacingAfter();

    // Своего воздуха у заголовка нет. Он был — «заголовок отделяет куски текста»,
    // — но выглядел ровно как пустая строка, которой в файле нет, и читался как
    // ошибка: в редакторе строка есть, в markdown её нет. Отбивку задаёт только
    // сам файл.
    return 0.0;
}

qreal blockTopMarginPx(Kind kind, bool raw, bool previousIsVSpace, bool first,
                       qreal lineUnit, const ZDocStyle& style) {
    qreal margin = blockTopMargin(kind, raw, previousIsVSpace, first, style) * lineUnit;
    // Сверху у плашки только воздух под скругление: полоска с языком и кнопкой
    // висит снизу, в нижнем поле последней строки блока.
    if (!raw && kind == Kind::Code) margin += codePlate(style).padTop;
    return margin;
}

QTextBlockFormat vspaceBlockFormat(bool previousIsVSpace, bool first, const ZDocStyle& style) {
    // МЕРА — БАЗОВЫЙ ШРИФТ ОБЛИКА, а не шрифт документа.
    //
    // Раньше здесь стоял doc.defaultFont(), и разницы не было: его никто не
    // двигал. Теперь им задаётся масштаб показа — и пустая строка, заведённая
    // операцией на 200 %, получила бы вдвое большее поле, чем такая же строка у
    // сборщика. Вся прочая геометрия документа печётся в единице (layoutLineUnit,
    // layoutCharUnit); пустая строка не имеет права быть исключением.
    const QFont base = layoutBaseFont(style);
    QTextBlockFormat format;
    format.setProperty(KindProperty, int(Kind::VSpace));
    format.setTopMargin(blockTopMargin(Kind::VSpace, false, previousIsVSpace, first, style) *
                        layoutLineUnit(style));
    format.setBottomMargin(0);
    applyLineHeight(format, style.lineHeightFactor(), base.pointSizeF(), base, style);
    return format;
}

QFont layoutBaseFont(const ZDocStyle& style) {
    QFont base{QString(style.fontFamily())};
    base.setPointSizeF(style.baseFontPoint());
    base.setStyleHint(QFont::Monospace);
    return base;
}

qreal layoutLineUnit(const ZDocStyle& style) {
    return QFontMetricsF(layoutBaseFont(style)).height();
}

void applyLineHeight(QTextBlockFormat& format, qreal factor, qreal linePoint,
                     const QFont& base, const ZDocStyle& style) {
    if (factor <= 0.0) return;
    switch (style.lineHeightMode()) {
        case ZDocStyle::LineHeight::Proportional:
            format.setLineHeight(factor * 100.0, QTextBlockFormat::ProportionalHeight);
            return;
        case ZDocStyle::LineHeight::Natural:
            return;
        case ZDocStyle::LineHeight::Pixels: {
            QFont line = base;
            line.setPointSizeF(linePoint);
            format.setLineHeight(std::round(QFontMetricsF(line).height() * factor),
                                 QTextBlockFormat::FixedHeight);
            return;
        }
    }
}

qreal layoutCharUnit(const ZDocStyle& style) {
    return QFontMetricsF(layoutBaseFont(style)).horizontalAdvance(QLatin1Char('A'));
}

namespace {

// Всё, что у сборки одно на весь документ: единицы ритма и шрифты. Считается
// один раз — и полной сборкой, и заплаткой, одинаково.
//
// Масштаба здесь нет. Шрифт документа сборщик ставит базовый (1.0), а зум
// потом кладёт поверх свой setDefaultFont — кегли ступенчатые и поедут за ним
// сами. Единицы ритма (высота строки, ширина "A") сняты с базового шрифта: они
// задают ГЕОМЕТРИЮ, а она в пикселях и строится один раз.
struct BuildContext {
    // Стиль, которым собирается документ: параметр сборки, а не глобальное.
    const ZDocStyle* style = nullptr;
    qreal basePoint = 0.0;
    QFont base;
    QRawFont primaryFont;
    qreal lineUnit = 0.0;
    qreal charUnit = 0.0;
    CodePlate plate;
    // Опыт просмотрщика, см. BuildOptions в заголовке.
};

BuildContext contextFor(const ZDocStyle& style) {
    BuildContext ctx;
    ctx.style = &style;
    ctx.basePoint = style.baseFontPoint();
    ctx.base = layoutBaseFont(style);
    ctx.primaryFont = QRawFont::fromFont(ctx.base);
    const QFontMetricsF metrics(ctx.base);
    ctx.lineUnit = metrics.height();
    ctx.charUnit = metrics.horizontalAdvance(QLatin1Char('A'));
    Q_ASSERT(qFuzzyCompare(ctx.lineUnit, layoutLineUnit(style)));
    ctx.plate = codePlate(style);
    return ctx;
}

// Один блок IR — в один QTextBlock (у литерального в несколько, по строке).
//
// documentStart — блок стоит в самом начале документа: над ним поле страницы,
// и своей отбивки сверху у него нет. reuse — писать в блок, на котором стоит
// курсор, а не заводить новый: и у свежего QTextDocument, и после выреза под
// заплатку остаётся ровно один пустой блок, который надо занять.
// СТРОЧНАЯ КАРТИНКА БЕЗ ПОДПИСИ ПОЛУЧАЕТ ИМЯ. В документе картинка посреди
// текста живёт спаном — форматом на знаках подписи; у «до ![](x.png) после»
// знаков нет, и держаться ей не на чем: она пропадала при сборке (файл после
// записи: «до  после»). Владелец: «![](…) мы в любом случае обязаны
// сохранять… смело бы писали ![image 1](…)». Имя безымянное (см.
// isNonameCaption), номер — порядковый среди картинок блока: номера не обязаны
// идти по порядку (решение владельца). Картинка целым абзацем сюда не
// попадает: она объект, и её пустая подпись живёт в свойстве.
bool hasBareInlineImage(const Piece& piece) {
    if (piece.raw || pieceIsImageObject(piece)) return false;
    for (const Run& run : piece.runs)
        if (run.image() && run.empty()) return true;
    return false;
}

const Piece& withNamedBareImages(const Piece& piece, Piece& storage) {
    if (!hasBareInlineImage(piece)) return piece;
    storage = piece;
    int ordinal = 0;
    for (size_t i = 0; i < storage.runs.size(); ++i) {
        if (storage.runs[i].image()) ++ordinal;
        if (!storage.runs[i].image() || !storage.runs[i].empty()) continue;
        const QString name = QStringLiteral("image %1").arg(ordinal);
        const int32_t at = storage.runs[i].start;
        const int32_t len = int32_t(name.size());
        storage.text.insert(at, name);
        storage.runs[i].end = at + len;
        for (size_t k = 0; k < storage.runs.size(); ++k) {
            if (k == i) continue;
            Run& other = storage.runs[k];
            // Всё, что начинается за вставкой (или на ней же, но идёт позже),
            // едет целиком; что накрывает вставку — растёт; что кончилось до
            // неё — стоит на месте.
            if (other.start > at || (other.start == at && k > i)) {
                other.start += len;
                other.end += len;
            } else if (other.end > at) {
                other.end += len;
            }
        }
    }
    return storage;
}

void emitBlock(QTextCursor& cursor, QTextDocument& target, const BuildContext& ctx,
               const Piece& piece, bool documentStart, bool& reuse, bool& prevVSpace) {
    Piece named;
    const Piece& b = withNamedBareImages(piece, named);
    const ZDocStyle& style = *ctx.style;
    const bool first = documentStart && reuse;
    const bool raw = b.raw;
    const bool list = !raw && isList(b.kind);

    QTextBlockFormat blockFmt;
    if (raw) {
        blockFmt.setProperty(RawProperty, true);
    } else {
        blockFmt.setProperty(KindProperty, static_cast<int>(b.kind));
        if (list) {
            blockFmt.setProperty(MarkerProperty, static_cast<int>(b.marker));
            blockFmt.setProperty(CheckedProperty, b.checked);
        }
    }
    // Уровень — у всякого блока, стоящего внутри пункта, а не только у самого
    // пункта: второй абзац пункта, блок кода, формула, дословный кусок
    // (таблица) — все на уровне (объекты внутри пунктов любой глубины —
    // решение владельца, сессия 5).
    if (b.level >= 0) blockFmt.setProperty(LevelProperty, b.level);

    // Отбивку целиком держит верхнее поле, нижнее всегда нулевое. Qt между
    // соседями берёт максимум из двух полей, и при полях с обеих сторон
    // зазор нельзя сделать разным для разных пар блоков: список, идущий за
    // абзацем, отбивался бы от него ровно как второй абзац.
    // Пункты одного списка стоят вплотную; два списка подряд разделяются,
    // иначе "- буллет" и "1. пункт" сливаются в одну лесенку. Сравнивать с
    // родом предыдущего блока нельзя: после вложенного подсписка предыдущий
    // блок лежит на другом уровне, и о списке текущего уровня не говорит
    // ничего. Спрашиваем состояние прогонов.
    // Пустая строка — сама блок, поэтому автоматических отбивок между
    // блоками нет вовсе: сколько пустых строк в файле, столько и на экране.
    // Полями обрамляется только прогон пустых строк — сверху перед первой,
    // снизу после последней.
    const bool vspace = !raw && b.kind == Kind::VSpace;

    // Ступень кегля этого блока. Ноль — вровень со шрифтом документа.
    int lineStep = 0;

    QTextCharFormat charFmt;
    setFontStep(charFmt, 0);

    // ЛИТЕРАЛЬНЫЙ БЛОК — ОДИН QTextBlock: и блок кода, и дословный кусок
    // (решение владельца, сессия refactor2: «1 блок markdown == 1 QTextBlock»).
    // Строки внутри разделяет U+2028 с пометкой BreakSourceProperty — тот же
    // разделитель, каким живёт мягкий перенос в абзаце, — и читатель вернёт из
    // него перевод строки. Замер (zametti-bench big loop type-mid): набор внутри
    // блока кода на 100 / 620 / 3000 строк — 2.5 / 2.7 / 8.3 мс одним блоком
    // против 7.2 / 13 / 85 мс построчно: построчный путь пересобирал логический
    // блок целиком на каждое нажатие. Дословный кусок лежал построчно дольше
    // (ContinuationProperty), пока таблица не стала объектом (сессия 5): теперь
    // раскрытая таблица — такой же один блок, и флип у неё 1 блок ↔ 1 блок.
    const bool literal = raw || b.kind == Kind::Code;
    const QString& source = b.text;
    // Один завершающий перевод строки снимаем: иначе внизу висела бы лишняя
    // пустая строка. По виду документа его не восстановить — пустой блок
    // кода и блок из одной пустой строки выглядят одинаково.
    const bool trailingNewline = literal && b.trailingNewline;

    QString text;
    std::vector<Break> breaks;
    if (raw) {
        charFmt.setForeground(style.rawColor());
    } else {
        switch (b.kind) {
            case Kind::Heading:
                blockFmt.setHeadingLevel(b.headingLevel);
                charFmt.setFontWeight(QFont::Bold);
                lineStep = style.headingStep()[size_t(b.headingLevel - 1)];
                setFontStep(charFmt, lineStep);
                break;

            case Kind::Code:
                // Поле блока — это отступ САМОГО КОДА, то есть плашка плюс её
                // внутреннее поле. Левый край плашки отрисовка находит,
                // вычитая padLeft обратно (см. codePlate в settings.h).
                blockFmt.setLeftMargin(ctx.plate.indent + ctx.plate.padLeft);
                blockFmt.setProperty(InfoProperty, b.info);
                lineStep = codeStepIn(0, style);
                setFontStep(charFmt, lineStep);
                if (!style.codeFamily().isEmpty())
                    charFmt.setFontFamilies({QString(style.codeFamily())});
                break;

            case Kind::Quote:
                // Курсивом цитату не выделяем: тогда настоящий _курсив_
                // внутри неё стал бы неотличим от остального текста.
                blockFmt.setLeftMargin(style.quoteIndent() * ctx.charUnit);
                charFmt.setForeground(style.quoteColor());
                break;

            // Ветки default в switch по роду не место: новый род обязан
            // сам всплыть здесь ошибкой сборки, а не молча собраться
            // обычным абзацем.
            case Kind::Paragraph:
            case Kind::VSpace:
            case Kind::ListItem:
                break;

            case Kind::Divider:
                // Черту рисует NoteView по геометрии блока — как маркеры.
                // В документе это пустой блок: текста у черты не бывает.
                break;

            case Kind::Math:
                // ФОРМУЛА ЛИТЕРАЛЬНА, как код: текст блока — её исходник вместе
                // с долларами, разметки внутри нет.
                //
                // И ОН НЕ РИСУЕТСЯ: вместо него вид кладёт вёрстку. Цвет ставит
                // СБОРЩИК, а не вид, и это важнее, чем кажется. Сперва гасил
                // вид — и цвет, будучи правкой живого документа, переживал
                // кэш заметок: вернулся человек на заметку, а прозрачность уже
                // растеклась на соседний блок, и под последней формулой
                // пропадали ссылка и абзац (владелец увидел это ровно так:
                // «сразу после загрузки видно, после возврата — нет»).
                // Поставленный сборщиком, цвет одинаков при каждой сборке.
                // Гасить исходник больше не нужно: его в тексте блока нет
                // вовсе — там стоит объект, а исходник живёт в свойстве его
                // формата. Прозрачный цвет был записью ВИДА в живой документ и
                // ровно из-за неё формулы и были выключены.
                break;

            case Kind::Html:
                // Комментарий: в тексте — внутренность без скобок, скобки
                // — структура. Рисуется тем же серым, что и дословные
                // куски, но правится как обычный текст.
                charFmt.setForeground(style.rawColor());
                break;
        }
        if (b.kind != Kind::Code) text = toQt(b.text, breaks);
    }

    // ФОТОГРАФИЯ — ОБЪЕКТ, А НЕ ТЕКСТ. В документе она занимает один знак
    // U+FFFC, размер которого Qt спрашивает у QTextObjectInterface, а исходник
    // едет рядом, в свойствах формата: обход документа кладёт обратно ровно
    // его, и U+FFFC не покидает QTextDocument никогда.
    // ФОРМУЛА — ОБЪЕКТ, как и фотография: один знак U+FFFC, исходник рядом.
    const bool formulaObject = pieceIsFormulaObject(b);
    if (formulaObject) {
        breaks.clear();
        charFmt.setObjectType(FormulaObject);
        charFmt.setProperty(ObjectSourceProperty, b.text);
        text = QString(QChar::ObjectReplacementCharacter);
    }
    // ТАБЛИЦА — ОБЪЕКТ (сессия 5): дословный кусок с одним знаком U+FFFC;
    // исходник в свойстве БЕЗ хвостового перевода строки — тот, как у кода,
    // держит TrailingNewlineProperty, и обход вернёт его писателю сам.
    const bool tableObject = pieceIsTableObject(b);
    if (tableObject) {
        breaks.clear();
        QStringView body = b.text;
        if (body.endsWith(u'\n')) body.chop(1);
        charFmt.setObjectType(TableObject);
        charFmt.setProperty(ObjectSourceProperty, body.toString());
        charFmt.clearForeground();
        text = QString(QChar::ObjectReplacementCharacter);
    }

    const bool imageObject = pieceIsImageObject(b);
    if (imageObject) {
        breaks.clear();
        const Run& run = b.runs.empty() ? Run{} : b.runs.front();
        charFmt.setObjectType(ImageObject);
        charFmt.setProperty(SpanStyleProperty, int(SpanImage));
        charFmt.setProperty(ObjectSourceProperty, b.text);
        BlockImageRef ref;
        if (!b.runs.empty()) {
            const QString& alt = b.text;
            const QString& href = run.href;
            charFmt.setProperty(ObjectAltProperty, alt);
            charFmt.setAnchorHref(href);
            if (!run.title.isEmpty()) charFmt.setProperty(SpanTitleProperty, run.title);
            ref = imageRefOfSpan(href, alt);
        } else {
            ref = imageRefOfWiki(b.text);
        }
        // ВЫРАВНИВАНИЕМ БЛОКА ФОТОГРАФИЮ НЕ ДВИГАЕМ, и это решение владельца:
        // объект занимает ВСЮ ширину колонки, а где внутри этой полосы встанет
        // снимок — дело вида. Тогда и подпись, и уголки, и попадание мышью
        // считаются от одной геометрии, а не от того, куда Qt поставила знак.
        (void)ref;
        text = QString(QChar::ObjectReplacementCharacter);
    }

    // Полоска с языком живёт НЕ в тексте, а в поле блока: резерв под неё —
    // НИЖНЕЕ поле последней строки блока кода (ставится в цикле по строкам), а
    // верхнее поле первой — воздух под скругление. Рисует в этом резерве
    // note_view.cpp теми же величинами. Резерв не зависит от того, задан язык
    // или нет: пустая полоска — это ряд, в котором стоит кнопка копирования.
    const bool code = !raw && b.kind == Kind::Code;
    // Первому блоку документа отбивка не нужна (над ним поле страницы), а вот
    // воздух над плашкой нужен и ему — это и делает blockTopMarginPx.
    blockFmt.setTopMargin(blockTopMarginPx(b.kind, raw, prevVSpace, first, ctx.lineUnit, style));
    blockFmt.setBottomMargin(0);


    // ВЫСОТА СТРОКИ — ДОЛЕЙ, А НЕ ПИКСЕЛЯМИ. Пиксели были бы абсолютными и на
    // зуме остались бы на месте: текст вырос, а строки — нет. Доля же считается
    // от естественной высоты строки, то есть от шрифта, и едет вместе с ним.
    //
    // Плата известна и записана: пиксельная высота была ЦЕЛОЙ нарочно — дробная
    // копилась от строки к строке, и Qt красил выделение с разбегом (между
    // полосами оставался незакрашенный ряд). С долей округления нет; если
    // разбег вернётся, лечить его надо подложкой выделения, а не абсолютной
    // высотой.
    // У БЛОКА С ОБЪЕКТОМ ВЫСОТА СТРОКИ РОВНО ЕГО СОБСТВЕННАЯ. Доля здесь
    // множится на высоту объекта, а не на высоту буквы: под фотографией в 340
    // точек 140 % оставляли полосу пустоты в полторы сотни точек. Объект сам
    // назвал свой размер — добавлять к нему ритм текста нечего.
    const qreal lineFactor = (imageObject || formulaObject || tableObject)
                                 ? 1.0
                                 : (list ? style.listLineHeightFactor()
                                         : style.lineHeightFactor());
    applyLineHeight(blockFmt, lineFactor, ctx.basePoint * fontStepFactor(lineStep), ctx.base, style);

    // ОДИН QTextBlock на любой блок заметки. Завершающий перевод строки у
    // литерального держится признаком, а не байтом (TrailingNewlineProperty):
    // пустой блок кода и блок из одной пустой строки выглядят одинаково.
    if (trailingNewline) blockFmt.setProperty(TrailingNewlineProperty, true);
    // Полоска блока кода — это НИЖНЕЕ ПОЛЕ блока. Qt между соседями берёт из
    // двух полей максимум, и меньше полоски зазор стать не может: следующий
    // блок в неё не въедет.
    if (code) blockFmt.setBottomMargin(ctx.plate.strip);

    // У литерального блока текст — исходник целиком, строки через U+2028; у
    // обычного текст и его разметка переносов посчитаны выше, и трогать их
    // нельзя. Тот же завершающий перевод строки, что помечен признаком, из
    // текста снимается: он не начинает новую строку, а завершает последнюю.
    // Таблица-объект — исключение: её текст уже один знак.
    if (literal && !tableObject) {
        breaks.clear();
        QStringView body = source;
        if (body.endsWith(u'\n')) body.chop(1);
        text = toQt(body.toString(), breaks);
    }

    if (reuse) {
        cursor.setBlockFormat(blockFmt);
        cursor.setBlockCharFormat(charFmt);
        reuse = false;
    } else {
        cursor.insertBlock(blockFmt, charFmt);
    }

    const int textStart = cursor.position();
    cursor.insertText(text, charFmt);
    markBreaks(target, textStart, breaks);
    const bool object = imageObject || formulaObject || tableObject;
    if (!literal && !object && !b.runs.empty())
        applySpans(target, textStart, b, lineStep, style);
    if (!object)
        enlargeFallbackGlyphs(target, textStart, text, lineStep, ctx.primaryFont, style);
    prevVSpace = vspace;
}

}  // namespace

namespace {
const char* const kStyleProperty = "zametti.docStyle";
}

std::shared_ptr<const ZDocStyle> attachedStyle(const QTextDocument& doc) {
    const QVariant held = doc.property(kStyleProperty);
    if (!held.isValid()) return nullptr;
    return held.value<std::shared_ptr<const ZDocStyle>>();
}

const ZDocStyle& styleOf(const QTextDocument& doc) {
    const std::shared_ptr<const ZDocStyle> own = attachedStyle(doc);
    return own ? *own : settings().style();
}

void attachStyle(QTextDocument& doc, std::shared_ptr<const ZDocStyle> style) {
    if (style == nullptr) doc.setProperty(kStyleProperty, QVariant());
    else doc.setProperty(kStyleProperty, QVariant::fromValue(std::move(style)));
}

qreal displayScaleOf(const QTextDocument& doc) {
    const qreal base = styleOf(doc).baseFontPoint();
    if (base <= 0.0) return 1.0;
    const qreal shown = doc.defaultFont().pointSizeF();
    return shown > 0.0 ? shown / base : 1.0;
}

void buildDocument(const std::vector<Piece>& blocks, QTextDocument& target,
                   BuildOptions options) {
    if (!options.keepUndo) target.setUndoRedoEnabled(false);
    // Стиль сборки прикрепляется к документу: свой — если дали, иначе тот, что
    // уже прикреплён (пересборка живой заметки), иначе из настроек.
    if (options.style != nullptr) attachStyle(target, options.style);
    target.clear();
    // Поля задаются рамкой корневого фрейма, а не documentMargin: тот кладёт
    // одинаковый отступ со всех сторон, а по бокам нужно заметно больше.
    target.setDocumentMargin(0);

    const ZDocStyle& style = styleOf(target);
    BuildContext ctx = contextFor(style);
    target.setDefaultFont(ctx.base);
    // Стоп табуляции — тот же, которым Tab ставит пробелы (editor.codeTabWidth).
    // Иначе набранное нами и литеральные табы из старых файлов рисовались бы
    // по-разному, и одинаковый на вид отступ оказывался бы разным.
    //
    // Стоп задаётся на весь документ, а не блокам кода: в QTextBlockFormat
    // стопы задаются списком положений, а список — уникальное значение на
    // блок, и QTextFormatCollection завела бы отдельный формат на каждую
    // строку (см. про интернирование в doc_model.h).
    {
        QFont codeLine = ctx.base;
        codeLine.setPointSizeF(ctx.basePoint * fontStepFactor(codeStepIn(0, style)));
        QTextOption option = target.defaultTextOption();
        option.setTabStopDistance(settings().editor().codeTabWidth() *
                                  QFontMetricsF(codeLine).horizontalAdvance(QLatin1Char(' ')));
        target.setDefaultTextOption(option);
    }

    QTextFrameFormat rootFormat = target.rootFrame()->frameFormat();
    rootFormat.setLeftMargin(style.sideMargin() * ctx.charUnit);
    rootFormat.setRightMargin(style.sideMargin() * ctx.charUnit);
    rootFormat.setTopMargin(style.verticalMargin() * ctx.lineUnit);
    rootFormat.setBottomMargin(style.verticalMargin() * ctx.lineUnit);
    target.rootFrame()->setFrameFormat(rootFormat);

    QTextCursor cursor(&target);
    cursor.beginEditBlock();

    // Свежий QTextDocument уже содержит один пустой блок: для первого блока
    // формат ставится на него, иначе сверху появится пустой абзац.
    bool first = true;
    bool prevVSpace = false;

    for (const Piece& b : blocks)
        emitBlock(cursor, target, ctx, b, true, first, prevVSpace);

    // Пустой документ: блоков не было, и единственный блок остался без формата
    // вовсе. Каретка в нём выходила кеглем по умолчанию и в самом углу окна —
    // человек её попросту не находил. Род блоку не назначаем: по его отсутствию
    // читатель и отличает пустой документ от пустого абзаца.
    if (first) {
        QTextCharFormat charFmt;
        setFontStep(charFmt, 0);
        QTextBlockFormat blockFmt;
        applyLineHeight(blockFmt, style.lineHeightFactor(), ctx.basePoint, ctx.base, style);
        cursor.setBlockFormat(blockFmt);
        cursor.setBlockCharFormat(charFmt);
    }

    // Под маркер отводится поле слева; сам он в текст не попадает и рисуется по
    // геометрии строки (см. marker.h). Поэтому продолжения пункта выравниваются
    // по его тексту сами, без висячего отступа.
    applyListGeometry(target, {0, target.blockCount() - 1});

    cursor.endEditBlock();
}

namespace {

// Одинаковы ли блоки настолько, что сборщик выдал бы за них одно и то же.
// Сравнивается всё, что блок о себе знает, а не только то, что видно на
// экране: род, разметка, текст, спаны с их адресами.
bool sameBlock(const Piece& x, const Piece& y) {
    if (x.kind != y.kind || x.marker != y.marker || x.checked != y.checked || x.raw != y.raw ||
        x.headingLevel != y.headingLevel || x.html != y.html || x.level != y.level)
        return false;
    if (x.text != y.text || x.info != y.info || x.trailingNewline != y.trailingNewline)
        return false;
    if (x.runs.size() != y.runs.size()) return false;
    for (size_t i = 0; i < x.runs.size(); ++i) {
        const Run& a = x.runs[i];
        const Run& b = y.runs[i];
        if (a.flags != b.flags || a.href != b.href || a.title != b.title) return false;
        if (x.view(a) != y.view(b)) return false;
    }
    return true;
}

}  // namespace

#ifndef NDEBUG
// ДОКУМЕНТ ОБЯЗАН СОВПАДАТЬ С ТЕМ, ЧТО СОБРАЛ БЫ СБОРЩИК, — до последнего
// свойства формата. Вопрос один и тот же у двоих: у заплатки («я дала то же,
// что дала бы полная сборка?») и у базиса правки («замена куска не оставила
// состояния, которого разбор не породил бы?»). Значит и проверка одна.
//
// Работает в отладочной сборке после каждой заплатки и после каждой замены
// куска, то есть на каждой операции всех фаззеров: свойство, а не отдельный
// случай.
void checkMatchesBuild(const std::vector<Piece>& to, const QTextDocument& target) {
    QTextDocument reference;
    buildDocument(to, reference);
    // ШРИФТ ДОКУМЕНТА В СРАВНЕНИИ НЕ УЧАСТВУЕТ — по той же причине, что и поля
    // рамки ниже: им теперь задаётся МАСШТАБ ПОКАЗА, и держит его вид
    // (NoteView::setZoom), а не сборщик. Сборщик ставит базовый кегль и о
    // масштабе не знает вовсе, поэтому у живого документа на 200 % здесь 22, а
    // у только что собранного эталона — 11, и сравнивать их значит ловить не
    // расхождение заплатки, а сам факт зума.
    //
    // Цена названа вслух: кегль базового шрифта из-под проверки ушёл. Он один
    // на весь документ и берётся из облика константой, так что испортить его
    // поблочно заплатка не может; ступени же кегля живут в форматах знаков и
    // сверяются по-прежнему.
    reference.setDefaultFont(target.defaultFont());
    // РЕЗЕРВЫ ПОКАЗА В СРАВНЕНИИ НЕ УЧАСТВУЮТ: их держит ВИД, а не сборщик, и
    // заплатка их не трогает вовсе — сверять тут нечего.
    //   - нижнее поле блока: в нём живёт высота фотографии (syncImageSpace);
    //   - высота строки: ею вид ужимает исходник выключной формулы под её
    //     вёрстку (note_view, syncFormulas). Сборщик ставит долю от строки
    //     (ProportionalHeight), вид — точные пиксели (FixedHeight), и на
    //     формуле они расходятся всегда;
    //   - поля рамки: их пересчитывает applyContentWidth под ширину окна —
    //     колонка в широком окне центрируется, и левое поле у живого документа
    //     87 против 55.99 у только что собранного. Сборщик ставит начальные
    //     значения, вид тут же ставит свои.
    // Сравнивать чужое — значит ловить не расхождение заплатки, а порядок
    // вызовов. Ровно на этом проверка и падала: у всех тестовых окон колонка
    // уже колонки не была, центрирование не включалось, и разница не всплывала.
    //
    // ЦЕНА НАЗВАНА ВСЛУХ: вместе с чужим из-под проверки уходит и своё —
    // высота строки, которую ставит сам сборщик. Заплатка, испортившая её на
    // обычном блоке, здесь больше не покраснеет. Долг снимается вместе с
    // переводом объектов на QTextObjectInterface: тогда вид перестанет писать
    // в документ вовсе, и исключение станет ненужным.
    const QList<int> skip{QTextFormat::BlockBottomMargin, QTextFormat::LineHeight,
                          QTextFormat::LineHeightType,    QTextFormat::FrameTopMargin,
                          QTextFormat::FrameBottomMargin, QTextFormat::FrameLeftMargin,
                          QTextFormat::FrameRightMargin};
    const QString want = documentFingerprint(reference, skip);
    const QString got = documentFingerprint(target, skip);
    if (want == got) return;
    const QStringList wantLines = want.split(QLatin1Char('\n'));
    const QStringList gotLines = got.split(QLatin1Char('\n'));
    qWarning().noquote() << "заплатка разошлась со сборкой: строк у сборки" << wantLines.size()
                         << "у заплатки" << gotLines.size();
    for (qsizetype i = 0; i < qMax(wantLines.size(), gotLines.size()); ++i) {
        const QString a = i < wantLines.size() ? wantLines.at(i) : QStringLiteral("<нет строки>");
        const QString b = i < gotLines.size() ? gotLines.at(i) : QStringLiteral("<нет строки>");
        if (a == b) continue;
        qWarning().noquote() << "строка" << i << "\n  сборка:  " << a.left(400)
                             << "\n  заплатка:" << b.left(400);
    }
    Q_ASSERT(!"документ разошёлся с полной сборкой");
}
#endif

bool patchDocument(const std::vector<Piece>& built, const std::vector<Piece>& now,
                   const std::vector<Piece>& to, QTextDocument& target) {
    const ZDocStyle& style = styleOf(target);
    const int builtCount = static_cast<int>(built.size());
    const int nowCount = static_cast<int>(now.size());
    const int newCount = static_cast<int>(to.size());
    // Пустой документ собирается особым путём (единственный блок без рода), и
    // выкраивать в нём нечего.
    if (builtCount == 0 || nowCount == 0 || newCount == 0) return false;

    // Общая голова — там, где ВСЕ трое согласны: и оформление верное, и
    // содержимое на месте, и номер блока в документе тот же самый.
    const int limit = qMin(builtCount, qMin(nowCount, newCount));
    int head = 0;
    while (head < limit && sameBlock(built[head], to[head]) && sameBlock(now[head], to[head]))
        ++head;
    if (head == builtCount && head == nowCount && head == newCount) {
#ifndef NDEBUG
        checkMatchesBuild(to, target);
#endif
        return true;   // не изменилось ничего
    }

    int tail = 0;
    while (tail < builtCount - head && tail < nowCount - head && tail < newCount - head &&
           sameBlock(built[size_t(builtCount - 1 - tail)], to[size_t(newCount - 1 - tail)]) &&
           sameBlock(now[size_t(nowCount - 1 - tail)], to[size_t(newCount - 1 - tail)]))
        ++tail;

    // Соседа с каждой стороны берём в заплатку, хотя он и не менялся: верхнее
    // поле блока зависит от того, пустая ли строка перед ним, и правка на
    // границе меняет отбивку у соседа, а не у себя. Дальше первого соседа это
    // не расходится: он в заплатке пересчитается по верному предшественнику, а
    // его собственная «пустота» не изменилась — значит и следующему за ним
    // считать нечего.
    head = qMax(0, head - 1);
    tail = qMax(0, tail - 1);
    // Вырезаем по номерам now: в документе лежит именно он.
    const int oldLast = nowCount - 1 - tail;
    // ЗАПЛАТКА БЕРЁТСЯ И ЗА ВЕСЬ ДОКУМЕНТ. Раньше здесь стоял отказ: «поменялось
    // всё — пусть собирает сборщик». С тех пор как отмена стала штатной, отказ
    // оказался ровно неверным: полная сборка начинается с clear(), а он в стек
    // отмены не ложится — один Ctrl+Z оставлял от заметки пустой лист, да и
    // каретка после отмены уезжала в начало. Заплатка же режет и вставляет
    // курсором, то есть отменяется как обычная правка.
    //
    // Шрифт и поля рамки она при этом не трогает, и это тоже стало плюсом: в
    // шрифте документа живёт масштаб показа, и сборщик, поставив базовый кегль,
    // сбрасывал бы его на каждой такой правке.

    // Границы выреза в самом документе. Один проход: искать блок IR по номеру —
    // это проход по документу, а их нужно два, начало и конец.
    // Блок заметки == QTextBlock (дословные куски и код — одним блоком), и
    // номер блока IR — это номер блока документа.
    const QTextBlock startBlock = target.findBlockByNumber(head);
    const QTextBlock endBlock = target.findBlockByNumber(oldLast);
    // Не нашлись — значит now описывает не этот документ, и резать наугад
    // нельзя. Пусть собирает целиком.
    if (!startBlock.isValid() || !endBlock.isValid()) return false;

    const BuildContext ctx = contextFor(style);
    const int firstNumber = startBlock.blockNumber();

    QTextCursor cursor(&target);
    cursor.beginEditBlock();
    // Вырезаем текст блоков, но не разделитель после последнего: от всего
    // выреза остаётся ровно один пустой блок, и следующий за ним — прежний.
    cursor.setPosition(startBlock.position());
    cursor.setPosition(endBlock.position() + endBlock.length() - 1, QTextCursor::KeepAnchor);
    cursor.removeSelectedText();

    bool reuse = true;
    bool prevVSpace = head > 0 && !to[size_t(head - 1)].raw &&
                      to[size_t(head - 1)].kind == Kind::VSpace;
    for (int i = head; i <= newCount - 1 - tail; ++i)
        emitBlock(cursor, target, ctx, to[size_t(i)], head == 0, reuse, prevVSpace);

    // Геометрия списка считается по прогону целиком, а не по блоку: ширину
    // колонки задаёт самый широкий маркер прогона. Диапазон до прогонов
    // расширяет сама applyListGeometry.
    applyListGeometry(target, {firstNumber, cursor.blockNumber()});
    cursor.endEditBlock();
#ifndef NDEBUG
    checkMatchesBuild(to, target);
#endif
    return true;
}

}  // namespace zametti
