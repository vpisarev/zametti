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

QString toQt(std::string_view utf8, std::vector<Break>& breaks) {
    QString s = QString::fromUtf8(utf8.data(), static_cast<qsizetype>(utf8.size()));
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

// Литеральный текст по строкам. Один завершающий перевод строки снимается —
// он не начинает новую строку, а завершает последнюю. Пустой текст даёт одну
// пустую строку: блок в документе есть всегда, пустых блоков не бывает.
std::vector<std::string_view> splitLiteralLines(std::string_view text) {
    std::string_view body = text;
    if (!body.empty() && body.back() == '\n') body.remove_suffix(1);

    std::vector<std::string_view> lines;
    size_t start = 0;
    for (;;) {
        const size_t end = body.find('\n', start);
        if (end == std::string_view::npos) {
            lines.push_back(body.substr(start));
            break;
        }
        lines.push_back(body.substr(start, end - start));
        start = end + 1;
    }
    return lines;
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

// Байтовое смещение в UTF-8 → индекс в QString. Вызывается по возрастанию
// смещений, поэтому каждый кусок текста пересчитывается ровно один раз.
class OffsetMap {
public:
    explicit OffsetMap(std::string_view text) : text_(text) {}

    int at(size_t byteOffset) {
        if (byteOffset < byte_) {   // мусорный спан: назад не отматываем
            byte_ = 0;
            utf16_ = 0;
        }
        if (byteOffset > text_.size()) byteOffset = text_.size();
        utf16_ += QString::fromUtf8(text_.data() + byte_,
                                    static_cast<qsizetype>(byteOffset - byte_))
                      .size();
        byte_ = byteOffset;
        return utf16_;
    }

private:
    const std::string_view text_;
    size_t byte_ = 0;
    int utf16_ = 0;
};

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
                           int surroundingStep, const QRawFont& primary) {
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
        setFontStep(fmt, surroundingStep + appearance().fallbackStep);
        cursor.setPosition(textStart + begin);
        cursor.setPosition(textStart + i, QTextCursor::KeepAnchor);
        cursor.mergeCharFormat(fmt);
    }
}

// Ступень кода — от ступени окружающего текста. В обычном абзаце окружение
// нулевое, и код получает ровно appearance().codeStep; в заголовке он едет
// вместе с заголовком.
int codeStepIn(int surroundingStep) { return surroundingStep + appearance().codeStep; }

void applySpans(QTextDocument& doc, int textStart, const Piece& b, int lineStep) {
    OffsetMap map(b.text);
    QTextCursor cursor(&doc);
    for (const Run& s : b.runs) {
        if (s.empty()) continue;
        const int from = map.at(static_cast<size_t>(s.start));
        const int to = map.at(static_cast<size_t>(s.end));
        if (to <= from) continue;

        // Стиль записывается свойством, а не выводится обратно из оформления:
        // заголовок набран жирным целиком, и «жирный» внутри него по весу
        // шрифта было бы не отличить от самого заголовка.
        int style = 0;
        if (s.bold()) style |= SpanBold;
        if (s.italic()) style |= SpanItalic;
        if (s.strike()) style |= SpanStrike;
        if (s.code()) style |= SpanCode;
        if (s.image()) style |= SpanImage;
        if (s.math()) style |= SpanMath;
        if (s.comment()) style |= SpanComment;

        QTextCharFormat fmt;
        if (style != 0) fmt.setProperty(SpanStyleProperty, style);
        if (s.bold()) fmt.setFontWeight(QFont::Bold);
        if (s.italic()) fmt.setFontItalic(true);
        if (s.strike()) fmt.setFontStrikeOut(true);
        if (s.code()) {
            fmt.setBackground(appearance().codeBackground);
            setFontStep(fmt, codeStepIn(lineStep));
            if (!appearance().codeFamily.isEmpty())
                fmt.setFontFamilies({QString(appearance().codeFamily)});
        }
        if (!s.href.empty()) {
            fmt.setAnchor(true);
            fmt.setAnchorHref(QString::fromStdString(s.href));
            fmt.setForeground(appearance().linkColor);
            fmt.setFontUnderline(true);
        }
        if (s.comment()) fmt.setForeground(appearance().rawColor);
        if (!s.title.empty())
            fmt.setProperty(SpanTitleProperty, QString::fromStdString(s.title));
        cursor.setPosition(textStart + from);
        cursor.setPosition(textStart + to, QTextCursor::KeepAnchor);
        cursor.mergeCharFormat(fmt);
    }
}

}  // namespace

qreal blockTopMargin(Kind kind, bool raw, bool previousIsVSpace, bool first) {
    // У первого блока отбивке сверху взяться неоткуда: над ним поле страницы.
    if (first) return 0.0;

    // Прогон пустых строк обрамляется своими полями: сверху перед первой, снизу
    // после последней. Внутри прогона — ничего, иначе высота разделителя из n
    // строк перестала бы быть n высотами строки.
    if (!raw && kind == Kind::VSpace)
        return previousIsVSpace ? 0.0 : appearance().separatorSpacingBefore;
    if (previousIsVSpace) return appearance().separatorSpacingAfter;

    // Своего воздуха у заголовка нет. Он был — «заголовок отделяет куски текста»,
    // — но выглядел ровно как пустая строка, которой в файле нет, и читался как
    // ошибка: в редакторе строка есть, в markdown её нет. Отбивку задаёт только
    // сам файл.
    return 0.0;
}

qreal blockTopMarginPx(Kind kind, bool raw, bool previousIsVSpace, bool first,
                       bool continuation, qreal lineUnit) {
    // Строки одного литерального блока стоят вплотную: воздух и отбивка есть
    // только у первой.
    if (continuation) return 0.0;
    qreal margin = blockTopMargin(kind, raw, previousIsVSpace, first) * lineUnit;
    // Сверху у плашки только воздух под скругление: полоска с языком и кнопкой
    // висит снизу, в нижнем поле последней строки блока.
    if (!raw && kind == Kind::Code) margin += codePlate().padTop;
    return margin;
}

QTextBlockFormat vspaceBlockFormat(bool previousIsVSpace, bool first) {
    // МЕРА — БАЗОВЫЙ ШРИФТ ОБЛИКА, а не шрифт документа.
    //
    // Раньше здесь стоял doc.defaultFont(), и разницы не было: его никто не
    // двигал. Теперь им задаётся масштаб показа — и пустая строка, заведённая
    // операцией на 200 %, получила бы вдвое большее поле, чем такая же строка у
    // сборщика. Вся прочая геометрия документа печётся в единице (layoutLineUnit,
    // layoutCharUnit); пустая строка не имеет права быть исключением.
    const QFont base = layoutBaseFont();
    QTextBlockFormat format;
    format.setProperty(KindProperty, int(Kind::VSpace));
    format.setTopMargin(blockTopMargin(Kind::VSpace, false, previousIsVSpace, first) *
                        layoutLineUnit());
    format.setBottomMargin(0);
    applyLineHeight(format, appearance().lineHeightFactor, base.pointSizeF(), base);
    return format;
}

QFont layoutBaseFont() {
    QFont base{QString(appearance().fontFamily)};
    base.setPointSizeF(appearance().baseFontPoint);
    base.setStyleHint(QFont::Monospace);
    return base;
}

qreal layoutLineUnit() { return QFontMetricsF(layoutBaseFont()).height(); }

void applyLineHeight(QTextBlockFormat& format, qreal factor, qreal linePoint,
                     const QFont& base) {
    if (factor <= 0.0) return;
    switch (appearance().lineHeightMode) {
        case Appearance::LineHeight::Proportional:
            format.setLineHeight(factor * 100.0, QTextBlockFormat::ProportionalHeight);
            return;
        case Appearance::LineHeight::Natural:
            return;
        case Appearance::LineHeight::Pixels: {
            QFont line = base;
            line.setPointSizeF(linePoint);
            format.setLineHeight(std::round(QFontMetricsF(line).height() * factor),
                                 QTextBlockFormat::FixedHeight);
            return;
        }
    }
}

qreal layoutCharUnit() {
    return QFontMetricsF(layoutBaseFont()).horizontalAdvance(QLatin1Char('A'));
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
    qreal basePoint = 0.0;
    QFont base;
    QRawFont primaryFont;
    qreal lineUnit = 0.0;
    qreal charUnit = 0.0;
    CodePlate plate;
    // Опыт просмотрщика, см. BuildOptions в заголовке.
    bool codeAsOneBlock = false;
};

BuildContext contextFor() {
    BuildContext ctx;
    ctx.basePoint = appearance().baseFontPoint;
    ctx.base = layoutBaseFont();
    ctx.primaryFont = QRawFont::fromFont(ctx.base);
    const QFontMetricsF metrics(ctx.base);
    ctx.lineUnit = metrics.height();
    ctx.charUnit = metrics.horizontalAdvance(QLatin1Char('A'));
    Q_ASSERT(qFuzzyCompare(ctx.lineUnit, layoutLineUnit()));
    ctx.plate = codePlate();
    return ctx;
}

// Один блок IR — в один QTextBlock (у литерального в несколько, по строке).
//
// documentStart — блок стоит в самом начале документа: над ним поле страницы,
// и своей отбивки сверху у него нет. reuse — писать в блок, на котором стоит
// курсор, а не заводить новый: и у свежего QTextDocument, и после выреза под
// заплатку остаётся ровно один пустой блок, который надо занять.
void emitBlock(QTextCursor& cursor, QTextDocument& target, const BuildContext& ctx,
               const Piece& b, bool documentStart, bool& reuse, bool& prevVSpace) {
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
        // Уровень — у всякого блока, стоящего внутри пункта, а не только у
        // самого пункта: второй абзац пункта тоже на уровне.
        if (b.level >= 0) blockFmt.setProperty(LevelProperty, b.level);
    }

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

    // Литеральное содержимое режется построчно, по QTextBlock на строку:
    // Qt переразмечает целиком тот блок, в который пишут, и длинный блок
    // кода делал набор внутри себя ощутимо медленным.
    //
    // Ключ опыта снимает это ТОЛЬКО с блока кода: дословный кусок остаётся
    // построчным. Внутри одного блока строки разделяет U+2028, и это тот же
    // разделитель, каким живёт мягкий перенос в абзаце, — читатель вернёт из
    // него перевод строки по пометке BreakSourceProperty.
    const bool literal = raw || b.kind == Kind::Code;
    const bool wholeCode = ctx.codeAsOneBlock && !raw && b.kind == Kind::Code;
    const std::string_view source = b.text;
    // Один завершающий перевод строки снимаем: иначе внизу висела бы лишняя
    // пустая строка. По виду документа его не восстановить — пустой блок
    // кода и блок из одной пустой строки выглядят одинаково.
    const bool trailingNewline = literal && b.trailingNewline;

    QString text;
    std::vector<Break> breaks;
    if (raw) {
        charFmt.setForeground(appearance().rawColor);
    } else {
        switch (b.kind) {
            case Kind::Heading:
                blockFmt.setHeadingLevel(b.headingLevel);
                charFmt.setFontWeight(QFont::Bold);
                lineStep = appearance().headingStep[size_t(b.headingLevel - 1)];
                setFontStep(charFmt, lineStep);
                break;

            case Kind::Code:
                // Поле блока — это отступ САМОГО КОДА, то есть плашка плюс её
                // внутреннее поле. Левый край плашки отрисовка находит,
                // вычитая padLeft обратно (см. codePlate в settings.h).
                blockFmt.setLeftMargin(ctx.plate.indent + ctx.plate.padLeft);
                blockFmt.setProperty(InfoProperty, QString::fromStdString(b.info));
                lineStep = codeStepIn(0);
                setFontStep(charFmt, lineStep);
                if (!appearance().codeFamily.isEmpty())
                    charFmt.setFontFamilies({QString(appearance().codeFamily)});
                break;

            case Kind::Quote:
                // Курсивом цитату не выделяем: тогда настоящий _курсив_
                // внутри неё стал бы неотличим от остального текста.
                blockFmt.setLeftMargin(appearance().quoteIndent * ctx.charUnit);
                charFmt.setForeground(appearance().quoteColor);
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
                // Пока объекты показаны исходником (kObjectsShown), гасить его
                // нечем: вёрстки поверх не будет, и прозрачный текст означал бы
                // пустое место вместо формулы.
                if (kObjectsShown) charFmt.setForeground(QColor(Qt::transparent));
                break;

            case Kind::Html:
                // Комментарий: в тексте — внутренность без скобок, скобки
                // — структура. Рисуется тем же серым, что и дословные
                // куски, но правится как обычный текст.
                charFmt.setForeground(appearance().rawColor);
                break;
        }
        if (b.kind != Kind::Code) text = toQt(b.text, breaks);
    }

    // Полоска с языком живёт НЕ в тексте, а в поле блока: резерв под неё —
    // НИЖНЕЕ поле последней строки блока кода (ставится в цикле по строкам), а
    // верхнее поле первой — воздух под скругление. Рисует в этом резерве
    // note_view.cpp теми же величинами. Резерв не зависит от того, задан язык
    // или нет: пустая полоска — это ряд, в котором стоит кнопка копирования.
    const bool code = !raw && b.kind == Kind::Code;
    // Первому блоку документа отбивка не нужна (над ним поле страницы), а вот
    // воздух над плашкой нужен и ему — это и делает blockTopMarginPx.
    blockFmt.setTopMargin(blockTopMarginPx(b.kind, raw, prevVSpace, first, false, ctx.lineUnit));
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
    const qreal lineFactor = list ? appearance().listLineHeightFactor : appearance().lineHeightFactor;
    applyLineHeight(blockFmt, lineFactor, ctx.basePoint * fontStepFactor(lineStep), ctx.base);

    // Один QTextBlock у обычного блока и по одному на строку у литерального.
    // Все, кроме первого, помечены продолжением: без этого разрезанный блок
    // кода из двух строк не отличить от двух блоков кода подряд.
    const std::vector<std::string_view> lines =
        (literal && !wholeCode) ? splitLiteralLines(source) : std::vector<std::string_view>{};
    const size_t count = (literal && !wholeCode) ? lines.size() : 1;

    for (size_t line = 0; line < count; ++line) {
        QTextBlockFormat lineFmt = blockFmt;
        if (line > 0) {
            lineFmt.setProperty(ContinuationProperty, true);
            lineFmt.setTopMargin(0);
        }
        if (line + 1 == count && trailingNewline)
            lineFmt.setProperty(TrailingNewlineProperty, true);
        // Полоска блока кода — это НИЖНЕЕ ПОЛЕ последней его строки. Qt между
        // соседями берёт из двух полей максимум, и меньше полоски зазор стать
        // не может: следующий блок в неё не въедет.
        if (code && line + 1 == count) lineFmt.setBottomMargin(ctx.plate.strip);
        // Язык стоит на каждой строке, хотя читатель берёт его с первой:
        // иначе удаление первой строки роняло бы язык всего блока. Лишних
        // форматов это не плодит — значение у всех строк одно, а
        // QTextFormatCollection их объединяет.

        // У литерального блока каждая строка своя; у обычного текст и его
        // разметка переносов посчитаны один раз выше, и трогать их нельзя.
        if (literal) {
            breaks.clear();
            if (wholeCode) {
                // Тот же завершающий перевод строки, что снимает
                // splitLiteralLines: он не начинает новую строку, а завершает
                // последнюю, и держится признаком, а не байтом.
                std::string_view body = source;
                if (!body.empty() && body.back() == '\n') body.remove_suffix(1);
                text = toQt(body, breaks);
            } else {
                text = toQt(lines[line], breaks);
            }
        }

        if (reuse) {
            cursor.setBlockFormat(lineFmt);
            cursor.setBlockCharFormat(charFmt);
            reuse = false;
        } else {
            cursor.insertBlock(lineFmt, charFmt);
        }

        const int textStart = cursor.position();
        cursor.insertText(text, charFmt);
        markBreaks(target, textStart, breaks);
        if (!literal && !b.runs.empty()) applySpans(target, textStart, b, lineStep);
        enlargeFallbackGlyphs(target, textStart, text, lineStep, ctx.primaryFont);
    }
    prevVSpace = vspace;
}

}  // namespace

void buildDocument(const std::vector<Piece>& blocks, QTextDocument& target,
                   BuildOptions options) {
    if (!options.keepUndo) target.setUndoRedoEnabled(false);
    target.clear();
    // Поля задаются рамкой корневого фрейма, а не documentMargin: тот кладёт
    // одинаковый отступ со всех сторон, а по бокам нужно заметно больше.
    target.setDocumentMargin(0);

    BuildContext ctx = contextFor();
    ctx.codeAsOneBlock = options.codeAsOneBlock;
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
        codeLine.setPointSizeF(ctx.basePoint * fontStepFactor(codeStepIn(0)));
        QTextOption option = target.defaultTextOption();
        option.setTabStopDistance(appearance().codeTabWidth *
                                  QFontMetricsF(codeLine).horizontalAdvance(QLatin1Char(' ')));
        target.setDefaultTextOption(option);
    }

    QTextFrameFormat rootFormat = target.rootFrame()->frameFormat();
    rootFormat.setLeftMargin(appearance().sideMargin * ctx.charUnit);
    rootFormat.setRightMargin(appearance().sideMargin * ctx.charUnit);
    rootFormat.setTopMargin(appearance().verticalMargin * ctx.lineUnit);
    rootFormat.setBottomMargin(appearance().verticalMargin * ctx.lineUnit);
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
        applyLineHeight(blockFmt, appearance().lineHeightFactor, ctx.basePoint, ctx.base);
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

#ifndef NDEBUG
// Заплатка обязана давать ровно то же, что и полная сборка, — до последнего
// свойства формата. Проверяется в отладочной сборке после каждой заплатки, то
// есть на каждой операции всех фаззеров: свойство, а не отдельный случай.
void checkPatchMatchesBuild(const std::vector<Piece>& to, const QTextDocument& target) {
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
        qWarning().noquote() << "строка" << i << "\n  сборка:  " << a.left(90)
                             << "\n  заплатка:" << b.left(90);
    }
    Q_ASSERT(!"заплатка разошлась с полной сборкой");
}
#endif

}  // namespace

bool patchDocument(const std::vector<Piece>& built, const std::vector<Piece>& now,
                   const std::vector<Piece>& to, QTextDocument& target) {
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
        checkPatchMatchesBuild(to, target);
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
    QTextBlock startBlock;
    QTextBlock endBlock;
    {
        int index = -1;
        for (QTextBlock block = target.begin(); block.isValid(); block = block.next()) {
            if (!isContinuationBlock(block)) {
                ++index;
                if (index == head) startBlock = block;
                if (index > oldLast) break;
            }
            if (index == oldLast) endBlock = block;   // последняя строка блока IR
        }
    }
    // Не нашлись — значит now описывает не этот документ, и резать наугад
    // нельзя. Пусть собирает целиком.
    if (!startBlock.isValid() || !endBlock.isValid()) return false;

    const BuildContext ctx = contextFor();
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
    checkPatchMatchesBuild(to, target);
#endif
    return true;
}

}  // namespace zametti
