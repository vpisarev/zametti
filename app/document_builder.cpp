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

#include "marker.h"
#include "settings.h"

#include <QColor>
#include <QPalette>
#include <QWidget>
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

#include <algorithm>
#include <vector>

namespace zametti {
namespace {

// Перевод строки внутри блока IR — это перенос внутри того же абзаца, а не
// новый абзац. QChar::LineSeparator даёт ровно это и, в отличие от '\n', не
// разрывает QTextBlock. Длина в кодовых единицах та же, так что смещения спанов
// остаются верными.
QString toQt(const std::string& utf8) {
    QString s = QString::fromUtf8(utf8.data(), static_cast<qsizetype>(utf8.size()));
    s.replace(QLatin1Char('\n'), QChar::LineSeparator);
    return s;
}

QString withoutTrailingNewline(const std::string& text) {
    std::string copy = text;
    if (!copy.empty() && copy.back() == '\n') copy.pop_back();
    return toQt(copy);
}

// Байтовое смещение в UTF-8 → индекс в QString. Вызывается по возрастанию
// смещений, поэтому каждый кусок текста пересчитывается ровно один раз.
class OffsetMap {
public:
    explicit OffsetMap(const std::string& text) : text_(text) {}

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
    const std::string& text_;
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
void enlargeFallbackGlyphs(QTextDocument& doc, int textStart, const QString& text,
                           qreal pointSize, const QRawFont& primary) {
    QTextCursor cursor(&doc);
    int i = 0;
    while (i < text.size()) {
        const bool pair = text[i].isHighSurrogate() && i + 1 < text.size() &&
                          text[i + 1].isLowSurrogate();
        const char32_t cp = pair ? QChar::surrogateToUcs4(text[i], text[i + 1])
                                 : char32_t(text[i].unicode());
        if (primary.supportsCharacter(cp)) {
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
            if (primary.supportsCharacter(c)) break;
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

void applySpans(QTextDocument& doc, int textStart, const Block& b, qreal linePoint,
                qreal zoom) {
    OffsetMap map(b.text);
    QTextCursor cursor(&doc);
    for (const Span& s : b.inlines) {
        if (s.length <= 0) continue;
        const int from = map.at(static_cast<size_t>(s.offset));
        const int to = map.at(static_cast<size_t>(s.offset) + static_cast<size_t>(s.length));
        if (to <= from) continue;

        QTextCharFormat fmt;
        if (s.bold) fmt.setFontWeight(QFont::Bold);
        if (s.italic) fmt.setFontItalic(true);
        if (s.strike) fmt.setFontStrikeOut(true);
        if (s.code) {
            fmt.setBackground(appearance().codeBackground);
            fmt.setFontPointSize(codePoint(linePoint, zoom));
            if (!appearance().codeFamily.isEmpty())
                fmt.setFontFamilies({QString(appearance().codeFamily)});
        }
        if (!s.href.empty()) {
            fmt.setAnchor(true);
            fmt.setAnchorHref(
                QString::fromUtf8(s.href.data(), static_cast<qsizetype>(s.href.size())));
            fmt.setForeground(appearance().linkColor);
            fmt.setFontUnderline(true);
        }
        cursor.setPosition(textStart + from);
        cursor.setPosition(textStart + to, QTextCursor::KeepAnchor);
        cursor.mergeCharFormat(fmt);
    }
}

// Прогон списка на каждом уровне вложенности. Ключ — не сам Kind, а его проекция
// (level, isOrdered): Bullet, TaskUnchecked и TaskChecked принадлежат одному
// семейству и прогон не рвут. Прогон переживает вложенный подсписок, иначе
// нумерация начнётся заново — "1. 2. 1." вместо "1. 2. 3.".
struct ListState {
    struct Level {
        int ordinal = 0;
        bool ordered = false;
        bool alive = false;
        qreal contentCol = 0.0;   // где начинается текст пункта этого уровня
    };
    std::vector<Level> levels;

    void reset() {
        for (Level& l : levels) l.alive = false;
    }

    int nextOrdinal(size_t level, bool ordered) {
        if (levels.size() <= level + 1) levels.resize(level + 2);
        Level& l = levels[level];
        l.ordinal = (l.alive && l.ordered == ordered) ? l.ordinal + 1 : 1;
        l.alive = true;
        l.ordered = ordered;
        for (size_t k = level + 1; k < levels.size(); ++k) levels[k].alive = false;
        return l.ordinal;
    }
};

}  // namespace

void applyPalette(QWidget& view) {
    QPalette palette = view.palette();
    palette.setColor(QPalette::Base, appearance().pageBackground);
    palette.setColor(QPalette::Highlight, appearance().selectionBackground);
    // Выделение светлое, поэтому текст в нём остаётся тёмным: белый по
    // умолчанию на таком фоне просто пропал бы.
    palette.setColor(QPalette::HighlightedText, palette.color(QPalette::Text));
    view.setPalette(palette);
}

void buildDocument(const Document& doc, QTextDocument& target, qreal zoom) {
    target.setUndoRedoEnabled(false);
    target.clear();
    // Поля задаются рамкой корневого фрейма, а не documentMargin: тот кладёт
    // одинаковый отступ со всех сторон, а по бокам нужно заметно больше.
    target.setDocumentMargin(0);

    const qreal basePoint = appearance().baseFontPoint * zoom;

    QFont base{QString(appearance().fontFamily)};
    base.setPointSizeF(basePoint);
    base.setStyleHint(QFont::Monospace);
    target.setDefaultFont(base);

    const QFontMetricsF metrics(base);
    const QRawFont primaryFont = QRawFont::fromFont(base);

    QTextFrameFormat rootFormat = target.rootFrame()->frameFormat();
    rootFormat.setLeftMargin(appearance().sideMargin * zoom);
    rootFormat.setRightMargin(appearance().sideMargin * zoom);
    rootFormat.setTopMargin(appearance().verticalMargin * zoom);
    rootFormat.setBottomMargin(appearance().verticalMargin * zoom);
    target.rootFrame()->setFrameFormat(rootFormat);

    QTextCursor cursor(&target);
    cursor.beginEditBlock();

    // Свежий QTextDocument уже содержит один пустой блок: для первого блока
    // формат ставится на него, иначе сверху появится пустой абзац.
    bool first = true;
    bool prevList = false;
    bool prevOrdered = false;
    int prevLevel = 0;
    ListState lists;

    for (const Block& b : doc) {
        const bool raw = !b.rawSource.empty();
        const bool list = !raw && isList(b.kind);

        QTextBlockFormat blockFmt;
        if (!raw) {
            blockFmt.setProperty(KindProperty, static_cast<int>(b.kind));
            if (list) blockFmt.setProperty(LevelProperty, b.level);
        }

        // Отбивку целиком держит верхнее поле, нижнее всегда нулевое. Qt между
        // соседями берёт максимум из двух полей, и при полях с обеих сторон
        // зазор нельзя сделать разным для разных пар блоков: список, идущий за
        // абзацем, отбивался бы от него ровно как второй абзац.
        // Пункты одного списка стоят вплотную; разной нумерации на одном уровне
        // — это уже два списка подряд, и они должны разделяться, иначе
        // "- буллет" и "1. пункт" сливаются в одну лесенку.
        const bool sameList = list && prevList &&
                              (b.level != prevLevel || isOrdered(b.kind) == prevOrdered);
        qreal topMargin = appearance().blockSpacing;
        if (sameList) topMargin = 0;
        else if (list && prevList)
            topMargin = std::max(appearance().listSpacingBefore, appearance().listSpacingAfter);
        else if (list) topMargin = appearance().listSpacingBefore;
        else if (prevList) topMargin = appearance().listSpacingAfter;

        // Высота строки задаётся явно, а не долей от самого высокого знака в
        // ней: иначе знак из запасного шрифта растягивал бы свою строку, и
        // пункты одного списка стояли бы с разным шагом.
        qreal linePoint = basePoint;

        QTextCharFormat charFmt;
        charFmt.setFontPointSize(basePoint);

        QString text;
        if (raw) {
            text = withoutTrailingNewline(b.rawSource);
            charFmt.setForeground(appearance().rawColor);
        } else {
            switch (b.kind) {
                case Kind::Heading:
                    blockFmt.setHeadingLevel(b.headingLevel);
                    topMargin = appearance().blockSpacing * appearance().headingSpacingFactor;
                    charFmt.setFontWeight(QFont::Bold);
                    linePoint = basePoint * appearance().headingScale[b.headingLevel - 1];
                    charFmt.setFontPointSize(linePoint);
                    break;

                case Kind::Code:
                    // Отступа у блока кода нет: подложка идёт во всю колонку,
                    // как в остальных программах для заметок.
                    text = withoutTrailingNewline(b.text);
                    blockFmt.setBackground(appearance().codeBackground);
                    linePoint = codePoint(basePoint, zoom);
                    charFmt.setFontPointSize(linePoint);
                    if (!appearance().codeFamily.isEmpty())
                        charFmt.setFontFamilies({QString(appearance().codeFamily)});
                    break;

                case Kind::Quote:
                    // Курсивом цитату не выделяем: тогда настоящий _курсив_
                    // внутри неё стал бы неотличим от остального текста.
                    blockFmt.setLeftMargin(metrics.horizontalAdvance(QLatin1Char(' ')) * 3);
                    charFmt.setForeground(appearance().quoteColor);
                    break;

                default:
                    break;
            }
            if (b.kind != Kind::Code) text = toQt(b.text);
        }

        blockFmt.setTopMargin(first ? 0 : topMargin * zoom);
        blockFmt.setBottomMargin(0);

        if (list) {
            const size_t level = static_cast<size_t>(b.level);
            const int ordinal = lists.nextOrdinal(level, isOrdered(b.kind));

            // Под маркер отводится поле слева; сам он в текст не попадает и
            // рисуется по геометрии строки (см. marker.h). Поэтому продолжения
            // пункта выравниваются по его тексту сами, без висячего отступа.
            const qreal indent = lists.levels[level].contentCol;
            const qreal cell = markerColumn(b.kind, ordinal, base);
            lists.levels[level + 1].contentCol = indent + cell;
            blockFmt.setLeftMargin(indent + cell);
        } else {
            lists.reset();
        }

        QFont lineFont = base;
        lineFont.setPointSizeF(linePoint);
        const qreal lineFactor = list ? appearance().listLineHeightFactor : appearance().lineHeightFactor;
        blockFmt.setLineHeight(QFontMetricsF(lineFont).height() * lineFactor,
                               QTextBlockFormat::FixedHeight);

        if (first) {
            cursor.setBlockFormat(blockFmt);
            cursor.setBlockCharFormat(charFmt);
            first = false;
        } else {
            cursor.insertBlock(blockFmt, charFmt);
        }

        const int textStart = cursor.position();
        cursor.insertText(text, charFmt);
        if (!raw && !b.inlines.empty()) applySpans(target, textStart, b, linePoint, zoom);
        enlargeFallbackGlyphs(target, textStart, text, linePoint, primaryFont);
        prevList = list;
        prevOrdered = list && isOrdered(b.kind);
        prevLevel = list ? b.level : 0;
    }

    cursor.endEditBlock();
}

}  // namespace zametti
