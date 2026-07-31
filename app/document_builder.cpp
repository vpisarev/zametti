// IR → QTextDocument.
//
// Здесь и только здесь UTF-8 ядра превращается в UTF-16 Qt. Смещения Span
// заданы в байтах, индексы QString — в кодовых единицах UTF-16; приравнивать их
// нельзя, ошибка проявится только на не-ASCII. Пересчёт идёт накопительно по
// спанам, идущим по порядку.
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

#include <QDebug>

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
                           qreal pointSize, const QRawFont& primary) {
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

        QTextCharFormat fmt;
        fmt.setFontPointSize(pointSize * appearance().fallbackScale);
        cursor.setPosition(textStart + begin);
        cursor.setPosition(textStart + i, QTextCursor::KeepAnchor);
        cursor.mergeCharFormat(fmt);
    }
}

// Кегль кода: величина абсолютная, в пунктах, и от кегля окружающего текста не
// зависит — только от масштаба окна. Ноль — кода не отличать от текста.
qreal codePoint(qreal surrounding, qreal zoom) {
    if (appearance().codePointSize <= 0.0) return surrounding;
    return appearance().codePointSize * zoom;
}

void applySpans(QTextDocument& doc, int textStart, const Document& ir, const Block& b,
                qreal linePoint, qreal zoom) {
    OffsetMap map(ir.text(b));
    QTextCursor cursor(&doc);
    for (const Inline& s : ir.inlines(b)) {
        if (s.text.size() <= 0) continue;
        const int from = map.at(static_cast<size_t>(s.text.start));
        const int to = map.at(static_cast<size_t>(s.text.end));
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
        if (s.comment()) style |= SpanComment;

        QTextCharFormat fmt;
        if (style != 0) fmt.setProperty(SpanStyleProperty, style);
        if (s.bold()) fmt.setFontWeight(QFont::Bold);
        if (s.italic()) fmt.setFontItalic(true);
        if (s.strike()) fmt.setFontStrikeOut(true);
        if (s.code()) {
            fmt.setBackground(appearance().codeBackground);
            fmt.setFontPointSize(codePoint(linePoint, zoom));
            if (!appearance().codeFamily.isEmpty())
                fmt.setFontFamilies({QString(appearance().codeFamily)});
        }
        if (!s.href.empty()) {
            const std::string_view href = ir.href(s);
            fmt.setAnchor(true);
            fmt.setAnchorHref(QString::fromUtf8(href.data(), static_cast<qsizetype>(href.size())));
            fmt.setForeground(appearance().linkColor);
            fmt.setFontUnderline(true);
        }
        if (s.comment()) fmt.setForeground(appearance().rawColor);
        if (!s.title.empty()) {
            const std::string_view title = ir.title(s);
            fmt.setProperty(SpanTitleProperty,
                            QString::fromUtf8(title.data(), static_cast<qsizetype>(title.size())));
        }
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

QTextBlockFormat vspaceBlockFormat(const QTextDocument& doc, bool previousIsVSpace, bool first) {
    const QFontMetricsF metrics(doc.defaultFont());
    QTextBlockFormat format;
    format.setProperty(KindProperty, int(Kind::VSpace));
    format.setTopMargin(blockTopMargin(Kind::VSpace, false, previousIsVSpace, first) *
                        metrics.height());
    format.setBottomMargin(0);
    format.setLineHeight(std::round(metrics.height() * appearance().lineHeightFactor),
                         QTextBlockFormat::FixedHeight);
    return format;
}

namespace {

// Всё, что у сборки одно на весь документ: единицы ритма и шрифты. Считается
// один раз — и полной сборкой, и заплаткой, одинаково.
struct BuildContext {
    qreal zoom = 1.0;
    qreal basePoint = 0.0;
    QFont base;
    QRawFont primaryFont;
    qreal lineUnit = 0.0;
    qreal charUnit = 0.0;
};

BuildContext contextFor(qreal zoom) {
    BuildContext ctx;
    ctx.zoom = zoom;
    ctx.basePoint = appearance().baseFontPoint * zoom;
    ctx.base = QFont{QString(appearance().fontFamily)};
    ctx.base.setPointSizeF(ctx.basePoint);
    ctx.base.setStyleHint(QFont::Monospace);
    ctx.primaryFont = QRawFont::fromFont(ctx.base);
    // Единицы ритма страницы: высота строки по вертикали, ширина "A" по
    // горизонтали. Метрики сняты с уже отмасштабированного шрифта, поэтому зум
    // сюда входит сам собой.
    const QFontMetricsF metrics(ctx.base);
    ctx.lineUnit = metrics.height();
    ctx.charUnit = metrics.horizontalAdvance(QLatin1Char('A'));
    return ctx;
}

// Один блок IR — в один QTextBlock (у литерального в несколько, по строке).
//
// documentStart — блок стоит в самом начале документа: над ним поле страницы,
// и своей отбивки сверху у него нет. reuse — писать в блок, на котором стоит
// курсор, а не заводить новый: и у свежего QTextDocument, и после выреза под
// заплатку остаётся ровно один пустой блок, который надо занять.
void emitBlock(QTextCursor& cursor, QTextDocument& target, const BuildContext& ctx,
               const Document& doc, const Block& b, bool documentStart, bool& reuse,
               bool& prevVSpace) {
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
    const qreal topMargin = blockTopMargin(b.kind, raw, prevVSpace, first);

    // Высота строки задаётся явно, а не долей от самого высокого знака в
    // ней: иначе знак из запасного шрифта растягивал бы свою строку, и
    // пункты одного списка стояли бы с разным шагом.
    qreal linePoint = ctx.basePoint;

    QTextCharFormat charFmt;
    charFmt.setFontPointSize(ctx.basePoint);

    // Литеральное содержимое режется построчно, по QTextBlock на строку:
    // Qt переразмечает целиком тот блок, в который пишут, и длинный блок
    // кода делал набор внутри себя ощутимо медленным.
    const bool literal = raw || b.kind == Kind::Code;
    const std::string_view source = doc.text(b);
    // Один завершающий перевод строки снимаем: иначе внизу висела бы лишняя
    // пустая строка. По виду документа его не восстановить — пустой блок
    // кода и блок из одной пустой строки выглядят одинаково.
    const bool trailingNewline = literal && !source.empty() && source.back() == '\n';

    QString text;
    std::vector<Break> breaks;
    if (raw) {
        charFmt.setForeground(appearance().rawColor);
    } else {
        switch (b.kind) {
            case Kind::Heading:
                blockFmt.setHeadingLevel(b.headingLevel);
                charFmt.setFontWeight(QFont::Bold);
                linePoint = ctx.basePoint * appearance().headingScale[b.headingLevel - 1];
                charFmt.setFontPointSize(linePoint);
                break;

            case Kind::Code:
                // Отступ маленький: подложка идёт почти во всю колонку, как
                // в остальных программах для заметок.
                blockFmt.setLeftMargin(appearance().codeIndent * ctx.charUnit);
                blockFmt.setProperty(
                    InfoProperty,
                    QString::fromUtf8(doc.info(b).data(), qsizetype(doc.info(b).size())));
                linePoint = codePoint(ctx.basePoint, ctx.zoom);
                charFmt.setFontPointSize(linePoint);
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

            case Kind::Html:
                // Комментарий: в тексте — внутренность без скобок, скобки
                // — структура. Рисуется тем же серым, что и дословные
                // куски, но правится как обычный текст.
                charFmt.setForeground(appearance().rawColor);
                break;
        }
        if (b.kind != Kind::Code) text = toQt(doc.text(b), breaks);
    }

    blockFmt.setTopMargin(first ? 0 : topMargin * ctx.lineUnit);
    blockFmt.setBottomMargin(0);

    QFont lineFont = ctx.base;
    lineFont.setPointSizeF(linePoint);
    const qreal lineFactor = list ? appearance().listLineHeightFactor : appearance().lineHeightFactor;
    // Высоту строки округляем до целого пикселя. Дробная копилась от строки
    // к строке, и Qt красил выделение с разбегом: между полосами оставался
    // незакрашенный ряд, а на укороченной строке он читался сколом на углу.
    // С целой высотой полосы сходятся вплотную сами, и подложку выделения
    // рисовать не надо.
    blockFmt.setLineHeight(std::round(QFontMetricsF(lineFont).height() * lineFactor),
                           QTextBlockFormat::FixedHeight);

    // Один QTextBlock у обычного блока и по одному на строку у литерального.
    // Все, кроме первого, помечены продолжением: без этого разрезанный блок
    // кода из двух строк не отличить от двух блоков кода подряд.
    const std::vector<std::string_view> lines =
        literal ? splitLiteralLines(source) : std::vector<std::string_view>{};
    const size_t count = literal ? lines.size() : 1;

    for (size_t line = 0; line < count; ++line) {
        QTextBlockFormat lineFmt = blockFmt;
        if (line > 0) {
            lineFmt.setProperty(ContinuationProperty, true);
            lineFmt.setTopMargin(0);
        }
        if (line + 1 == count && trailingNewline)
            lineFmt.setProperty(TrailingNewlineProperty, true);
        // Язык стоит на каждой строке, хотя читатель берёт его с первой:
        // иначе удаление первой строки роняло бы язык всего блока. Лишних
        // форматов это не плодит — значение у всех строк одно, а
        // QTextFormatCollection их объединяет.

        // У литерального блока каждая строка своя; у обычного текст и его
        // разметка переносов посчитаны один раз выше, и трогать их нельзя.
        if (literal) {
            breaks.clear();
            text = toQt(lines[line], breaks);
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
        if (!literal && !b.inlines.empty())
            applySpans(target, textStart, doc, b, linePoint, ctx.zoom);
        enlargeFallbackGlyphs(target, textStart, text, linePoint, ctx.primaryFont);
    }
    prevVSpace = vspace;
}

}  // namespace

void buildDocument(const Document& doc, QTextDocument& target, qreal zoom) {
    target.setUndoRedoEnabled(false);
    target.clear();
    // Поля задаются рамкой корневого фрейма, а не documentMargin: тот кладёт
    // одинаковый отступ со всех сторон, а по бокам нужно заметно больше.
    target.setDocumentMargin(0);

    const BuildContext ctx = contextFor(zoom);
    target.setDefaultFont(ctx.base);

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

    for (const Block& b : doc.blocks)
        emitBlock(cursor, target, ctx, doc, b, true, first, prevVSpace);

    // Пустой документ: блоков не было, и единственный блок остался без формата
    // вовсе. Каретка в нём выходила кеглем по умолчанию и в самом углу окна —
    // человек её попросту не находил. Род блоку не назначаем: по его отсутствию
    // читатель и отличает пустой документ от пустого абзаца.
    if (first) {
        QTextCharFormat charFmt;
        charFmt.setFontPointSize(ctx.basePoint);
        QTextBlockFormat blockFmt;
        blockFmt.setLineHeight(QFontMetricsF(ctx.base).height() * appearance().lineHeightFactor,
                               QTextBlockFormat::FixedHeight);
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
bool sameBlock(const Document& a, const Block& x, const Document& b, const Block& y) {
    if (x.kind != y.kind || x.marker != y.marker || x.checked != y.checked || x.raw != y.raw ||
        x.headingLevel != y.headingLevel || x.html != y.html || x.level != y.level)
        return false;
    if (a.text(x) != b.text(y) || a.info(x) != b.info(y)) return false;
    const std::span<const Inline> xs = a.inlines(x);
    const std::span<const Inline> ys = b.inlines(y);
    if (xs.size() != ys.size()) return false;
    for (size_t i = 0; i < xs.size(); ++i) {
        if (xs[i].flags != ys[i].flags) return false;
        if (a.text(x, xs[i]) != b.text(y, ys[i])) return false;
        if (a.href(xs[i]) != b.href(ys[i]) || a.title(xs[i]) != b.title(ys[i])) return false;
    }
    return true;
}

#ifndef NDEBUG
// Заплатка обязана давать ровно то же, что и полная сборка, — до последнего
// свойства формата. Проверяется в отладочной сборке после каждой заплатки, то
// есть на каждой операции всех фаззеров: свойство, а не отдельный случай.
void checkPatchMatchesBuild(const Document& to, const QTextDocument& target, qreal zoom) {
    QTextDocument reference;
    buildDocument(to, reference, zoom);
    // Нижнее поле блока в сравнении не участвует: сборщик всегда пишет туда
    // ноль, а ненулевое значение ставит вид — в нём живёт высота фотографии
    // (см. note_view.cpp). Полная сборка его затирает, и вид ставит заново;
    // заплатка нетронутые блоки не трогает, и поле остаётся стоять. Разница
    // не в сборке, а в том, что сборка сбивает чужое.
    const QList<int> skip{QTextFormat::BlockBottomMargin};
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

bool patchDocument(const Document& built, const Document& now, const Document& to,
                   QTextDocument& target, qreal zoom) {
    const int builtCount = static_cast<int>(built.blocks.size());
    const int nowCount = static_cast<int>(now.blocks.size());
    const int newCount = static_cast<int>(to.blocks.size());
    // Пустой документ собирается особым путём (единственный блок без рода), и
    // выкраивать в нём нечего.
    if (builtCount == 0 || nowCount == 0 || newCount == 0) return false;

    // Общая голова — там, где ВСЕ трое согласны: и оформление верное, и
    // содержимое на месте, и номер блока в документе тот же самый.
    const int limit = qMin(builtCount, qMin(nowCount, newCount));
    int head = 0;
    while (head < limit && sameBlock(built, built.blocks[head], to, to.blocks[head]) &&
           sameBlock(now, now.blocks[head], to, to.blocks[head]))
        ++head;
    if (head == builtCount && head == nowCount && head == newCount) {
#ifndef NDEBUG
        checkPatchMatchesBuild(to, target, zoom);
#endif
        return true;   // не изменилось ничего
    }

    int tail = 0;
    while (tail < builtCount - head && tail < nowCount - head && tail < newCount - head &&
           sameBlock(built, built.blocks[builtCount - 1 - tail], to,
                     to.blocks[newCount - 1 - tail]) &&
           sameBlock(now, now.blocks[nowCount - 1 - tail], to, to.blocks[newCount - 1 - tail]))
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
    // Меняется весь документ — заплатка вырождается в сборку, но без установки
    // шрифта и рамки. Пусть собирает тот, чья это работа.
    if (head == 0 && oldLast == nowCount - 1) return false;

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

    const BuildContext ctx = contextFor(zoom);
    const int firstNumber = startBlock.blockNumber();

    QTextCursor cursor(&target);
    cursor.beginEditBlock();
    // Вырезаем текст блоков, но не разделитель после последнего: от всего
    // выреза остаётся ровно один пустой блок, и следующий за ним — прежний.
    cursor.setPosition(startBlock.position());
    cursor.setPosition(endBlock.position() + endBlock.length() - 1, QTextCursor::KeepAnchor);
    cursor.removeSelectedText();

    bool reuse = true;
    bool prevVSpace = head > 0 && !to.blocks[head - 1].raw &&
                      to.blocks[head - 1].kind == Kind::VSpace;
    for (int i = head; i <= newCount - 1 - tail; ++i)
        emitBlock(cursor, target, ctx, to, to.blocks[i], head == 0, reuse, prevVSpace);

    // Геометрия списка считается по прогону целиком, а не по блоку: ширину
    // колонки задаёт самый широкий маркер прогона. Диапазон до прогонов
    // расширяет сама applyListGeometry.
    applyListGeometry(target, {firstNumber, cursor.blockNumber()});
    cursor.endEditBlock();
#ifndef NDEBUG
    checkPatchMatchesBuild(to, target, zoom);
#endif
    return true;
}

}  // namespace zametti
