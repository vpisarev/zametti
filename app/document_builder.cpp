// IR → QTextDocument.
//
// Здесь и только здесь UTF-8 ядра превращается в UTF-16 Qt. Смещения Span
// заданы в байтах, индексы QString — в кодовых единицах UTF-16; приравнивать их
// нельзя, ошибка проявится только на не-ASCII. Пересчёт идёт накопительно по
// спанам, идущим по порядку.
//
// Маркеры списков рисуются текстом, а не через QTextList. Причина простая:
// маркер QTextList нельзя ни покрасить, ни увеличить, ни сдвинуть по базовой
// линии, а чекбоксу всё это нужно. Заодно уходит и навязанный Qt отступ:
// пункт верхнего уровня встаёт вровень с абзацем, как и в самом файле.
// Плата — нумерацию приходится вести самим, ровно тем же правилом, что и в
// сериализаторе: прогон живёт на каждом уровне и переживает вложенный подсписок.

#include "document_builder.h"

#include "checkbox_object.h"

#include <QAbstractTextDocumentLayout>
#include <QColor>
#include <QFont>
#include <QFontMetricsF>
#include <QString>
#include <QTextBlockFormat>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextOption>

#include <vector>

namespace zametti {
namespace {

constexpr char kFontFamily[] = "IBM Plex Mono";
// В IBM Plex Mono нет ни U+2610, ни U+2611 — чекбоксы берём из DejaVu Sans Mono,
// он тоже моноширинный и стоит в системе по умолчанию.
constexpr char kSymbolFamily[] = "DejaVu Sans Mono";

constexpr qreal kBaseFontPoint = 11.0;
constexpr qreal kHeadingScale[6] = {1.7, 1.45, 1.25, 1.1, 1.0, 0.95};
// У IBM Plex Mono собственный межстрочный просвет уже приличный, поэтому
// множитель нужен маленький. Пункты списка ставим плотно: список читается как
// один объект. Расстояние между абзацами держат поля блока, а не интерлиньяж —
// иначе, ужимая строки, мы бы заодно сплющили и абзацы.
constexpr qreal kLineHeightFactor = 1.15;
constexpr qreal kListLineHeightFactor = 1.05;
constexpr qreal kBlockSpacing = 13.0;
// Чем рисовать чекбокс. Шрифтовые варианты просты, но размер, толщина линий и
// положение по базовой линии в них заданы шрифтом и не настраиваются.
enum class CheckboxStyle {
    Glyph,   // ☐ / ☑ из DejaVu Sans Mono
    Ascii,   // [ ] / [x] основной гарнитурой
    Drawn,   // рисуем сами, см. checkbox_object.cpp
};
constexpr CheckboxStyle kCheckboxStyle = CheckboxStyle::Drawn;
constexpr qreal kCheckboxScale = 1.8;

const QColor kMarkerColor(0x7a, 0x82, 0x8c);
const QColor kCheckedColor(0x1f, 0x8b, 0x3d);
const QColor kUncheckedColor(0x6b, 0x77, 0x85);
const QColor kLinkColor(0x1a, 0x5f, 0xb4);
const QColor kQuoteColor(0x5a, 0x62, 0x6a);
const QColor kRawColor(0x99, 0x9f, 0xa6);
const QColor kCodeBackground(0, 0, 0, 14);

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

void applySpans(QTextDocument& doc, int textStart, const Block& b) {
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
        if (s.code) fmt.setBackground(kCodeBackground);
        if (!s.href.empty()) {
            fmt.setAnchor(true);
            fmt.setAnchorHref(
                QString::fromUtf8(s.href.data(), static_cast<qsizetype>(s.href.size())));
            fmt.setForeground(kLinkColor);
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

bool isTask(Kind kind) {
    return kind == Kind::TaskUnchecked || kind == Kind::TaskChecked;
}

// Сам знак маркера, без отбивки: текст ставится по табуляции, а не встык.
// У нарисованного чекбокса знака нет — вместо него в текст идёт заполнитель,
// который лэйаут отдаёт нашему обработчику.
QString markerGlyph(Kind kind, int ordinal) {
    switch (kind) {
        case Kind::Bullet:  return QStringLiteral("•");
        case Kind::Ordered: return QString::number(ordinal) + QStringLiteral(".");
        case Kind::TaskUnchecked:
        case Kind::TaskChecked:
            switch (kCheckboxStyle) {
                case CheckboxStyle::Glyph:
                    return kind == Kind::TaskChecked ? QStringLiteral("☑")
                                                     : QStringLiteral("☐");
                case CheckboxStyle::Ascii:
                    return kind == Kind::TaskChecked ? QStringLiteral("[x]")
                                                     : QStringLiteral("[ ]");
                case CheckboxStyle::Drawn:
                    return QString(QChar::ObjectReplacementCharacter);
            }
            return {};
        default: return {};
    }
}

// Ширина колонки маркера — в знакоместах базового шрифта, как в самом файле:
// под "- " содержимое идёт со второй колонки, под "1. " — с третьей. Чекбокс
// шире буквы, поэтому ему нужна своя колонка.
int markerCells(Kind kind, int ordinal) {
    if (kind == Kind::Ordered) return QString::number(ordinal).size() + 2;
    if (isTask(kind)) return kCheckboxStyle == CheckboxStyle::Ascii ? 4 : 3;
    return 2;
}

}  // namespace

void buildDocument(const Document& doc, QTextDocument& target) {
    target.setUndoRedoEnabled(false);
    target.clear();
    target.setDocumentMargin(28);

    QFont base(QString::fromLatin1(kFontFamily));
    base.setPointSizeF(kBaseFontPoint);
    base.setStyleHint(QFont::Monospace);
    target.setDefaultFont(base);

    const QFontMetricsF metrics(base);

    if (kCheckboxStyle == CheckboxStyle::Drawn &&
        target.documentLayout()->handlerForObject(CheckboxObject::Type) == nullptr) {
        target.documentLayout()->registerHandler(CheckboxObject::Type,
                                                 new CheckboxObject(&target));
    }

    QTextCursor cursor(&target);
    cursor.beginEditBlock();

    // Свежий QTextDocument уже содержит один пустой блок: для первого блока
    // формат ставится на него, иначе сверху появится пустой абзац.
    bool first = true;
    ListState lists;

    for (const Block& b : doc) {
        const bool raw = !b.rawSource.empty();
        const bool list = !raw && isList(b.kind);

        QTextBlockFormat blockFmt;
        blockFmt.setTopMargin(kBlockSpacing);
        blockFmt.setBottomMargin(kBlockSpacing);

        // Высота строки задаётся явно, а не долей от самого высокого знака в
        // ней: иначе увеличенный чекбокс растягивал бы строку задачи, и пункты
        // одного списка стояли бы с разным шагом.
        qreal linePoint = kBaseFontPoint;

        QTextCharFormat charFmt;
        charFmt.setFontPointSize(kBaseFontPoint);

        QString marker;
        QTextCharFormat markerFmt;

        QString text;
        if (raw) {
            text = withoutTrailingNewline(b.rawSource);
            charFmt.setForeground(kRawColor);
        } else {
            switch (b.kind) {
                case Kind::Heading:
                    blockFmt.setHeadingLevel(b.headingLevel);
                    blockFmt.setTopMargin(kBlockSpacing * 2.2);
                    charFmt.setFontWeight(QFont::Bold);
                    linePoint = kBaseFontPoint * kHeadingScale[b.headingLevel - 1];
                    charFmt.setFontPointSize(linePoint);
                    break;

                case Kind::Code:
                    text = withoutTrailingNewline(b.text);
                    blockFmt.setBackground(kCodeBackground);
                    blockFmt.setLeftMargin(metrics.horizontalAdvance(QLatin1Char(' ')) * 2);
                    break;

                case Kind::Quote:
                    // Курсивом цитату не выделяем: тогда настоящий _курсив_
                    // внутри неё стал бы неотличим от остального текста.
                    blockFmt.setLeftMargin(metrics.horizontalAdvance(QLatin1Char(' ')) * 3);
                    charFmt.setForeground(kQuoteColor);
                    break;

                default:
                    break;
            }
            if (b.kind != Kind::Code) text = toQt(b.text);
        }

        if (list) {
            const size_t level = static_cast<size_t>(b.level);
            const int ordinal = lists.nextOrdinal(level, isOrdered(b.kind));
            marker = markerGlyph(b.kind, ordinal) + QLatin1Char('\t');

            markerFmt = charFmt;
            if (isTask(b.kind)) {
                const bool checked = b.kind == Kind::TaskChecked;
                markerFmt.setForeground(checked ? kCheckedColor : kUncheckedColor);
                switch (kCheckboxStyle) {
                    case CheckboxStyle::Glyph:
                        markerFmt.setFontFamilies({QString::fromLatin1(kSymbolFamily),
                                                   QString::fromLatin1(kFontFamily)});
                        markerFmt.setFontPointSize(kBaseFontPoint * kCheckboxScale);
                        break;
                    case CheckboxStyle::Ascii:
                        break;
                    case CheckboxStyle::Drawn:
                        markerFmt.setObjectType(CheckboxObject::Type);
                        markerFmt.setProperty(CheckboxObject::CheckedProperty, checked);
                        break;
                }
            } else {
                markerFmt.setForeground(kMarkerColor);
            }

            // Висячий отступ: первая строка начинается с маркера, продолжения
            // выравниваются по тексту. Колонка текста задана табуляцией, а не
            // шириной самого знака: чекбокс крупнее остальных маркеров, и без
            // этого текст задач съезжал бы вправо относительно обычных пунктов.
            const qreal indent = lists.levels[level].contentCol;
            const qreal cell = metrics.horizontalAdvance(QLatin1Char(' ')) *
                               markerCells(b.kind, ordinal);
            lists.levels[level + 1].contentCol = indent + cell;

            blockFmt.setLeftMargin(indent + cell);
            blockFmt.setTextIndent(-cell);
            blockFmt.setTabPositions({QTextOption::Tab(cell, QTextOption::LeftTab)});
            blockFmt.setTopMargin(0);
            blockFmt.setBottomMargin(0);
        } else {
            lists.reset();
        }

        QFont lineFont = base;
        lineFont.setPointSizeF(linePoint);
        const qreal lineFactor = list ? kListLineHeightFactor : kLineHeightFactor;
        blockFmt.setLineHeight(QFontMetricsF(lineFont).height() * lineFactor,
                               QTextBlockFormat::FixedHeight);

        if (first) {
            cursor.setBlockFormat(blockFmt);
            cursor.setBlockCharFormat(charFmt);
            first = false;
        } else {
            cursor.insertBlock(blockFmt, charFmt);
        }

        if (!marker.isEmpty()) cursor.insertText(marker, markerFmt);

        const int textStart = cursor.position();
        cursor.insertText(text, charFmt);
        if (!raw && !b.inlines.empty()) applySpans(target, textStart, b);
    }

    cursor.endEditBlock();
}

}  // namespace zametti
