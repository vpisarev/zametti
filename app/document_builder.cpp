// IR → QTextDocument.
//
// Здесь и только здесь UTF-8 ядра превращается в UTF-16 Qt. Смещения Span
// заданы в байтах, индексы QString — в кодовых единицах UTF-16; приравнивать их
// нельзя, ошибка проявится только на не-ASCII. Пересчёт идёт накопительно по
// спанам, идущим по порядку.

#include "document_builder.h"

#include <QColor>
#include <QString>
#include <QTextBlockFormat>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextList>
#include <QTextListFormat>

#include <vector>

namespace zametti {
namespace {

constexpr qreal kHeadingScale[6] = {1.7, 1.45, 1.25, 1.1, 1.0, 0.95};
constexpr qreal kBlockSpacing = 8.0;
constexpr int kIndentPerLevel = 24;

// Перевод строки внутри блока IR — это перенос внутри того же абзаца, а не
// новый абзац. QChar::LineSeparator даёт ровно это и, в отличие от '\n', не
// разрывает QTextBlock. Длина в кодовых единицах та же, так что смещения спанов
// остаются верными.
QString toQt(const std::string& utf8) {
    QString s = QString::fromUtf8(utf8.data(), static_cast<qsizetype>(utf8.size()));
    s.replace(QLatin1Char('\n'), QChar::LineSeparator);
    return s;
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

QTextCharFormat monospace(const QTextCharFormat& base) {
    QTextCharFormat fmt = base;
    fmt.setFontFamilies({QStringLiteral("monospace")});
    return fmt;
}

void applySpans(QTextCursor& cursor, int blockStart, const Block& b,
                const QTextCharFormat& base) {
    OffsetMap map(b.text);
    QTextCursor fmtCursor(cursor.document());
    for (const Span& s : b.inlines) {
        if (s.length <= 0) continue;
        int from = map.at(static_cast<size_t>(s.offset));
        int to = map.at(static_cast<size_t>(s.offset) + static_cast<size_t>(s.length));
        if (to <= from) continue;

        QTextCharFormat fmt;
        if (s.bold) fmt.setFontWeight(QFont::Bold);
        if (s.italic) fmt.setFontItalic(true);
        if (s.strike) fmt.setFontStrikeOut(true);
        if (s.code) {
            fmt.setFontFamilies({QStringLiteral("monospace")});
            fmt.setBackground(QColor(0, 0, 0, 18));
        }
        if (!s.href.empty()) {
            fmt.setAnchor(true);
            fmt.setAnchorHref(QString::fromUtf8(s.href.data(),
                                                static_cast<qsizetype>(s.href.size())));
            fmt.setForeground(QColor(0x1a, 0x5f, 0xb4));
            fmt.setFontUnderline(true);
        }
        fmtCursor.setPosition(blockStart + from);
        fmtCursor.setPosition(blockStart + to, QTextCursor::KeepAnchor);
        fmtCursor.mergeCharFormat(fmt);
    }
    (void)base;
}

QString withoutTrailingNewline(const std::string& text) {
    std::string copy = text;
    if (!copy.empty() && copy.back() == '\n') copy.pop_back();
    return toQt(copy);
}

}  // namespace

void buildDocument(const Document& doc, QTextDocument& target) {
    target.setUndoRedoEnabled(false);
    target.clear();
    target.setDocumentMargin(24);

    QTextCursor cursor(&target);
    cursor.beginEditBlock();

    const QTextCharFormat baseChar = cursor.charFormat();
    const qreal basePointSize = target.defaultFont().pointSizeF() > 0
                                    ? target.defaultFont().pointSizeF()
                                    : 11.0;

    // Свежий QTextDocument уже содержит один пустой блок: для первого блока
    // формат ставится на него, иначе сверху появится пустой абзац.
    bool first = true;

    // Прогон списка на каждом уровне вложенности. Ключ — не сам Kind, а его
    // проекция (level, isOrdered): Bullet, TaskUnchecked и TaskChecked
    // принадлежат одному семейству и прогон не рвут.
    //
    // Хранить один «текущий» список нельзя: после вложенного подсписка мы
    // возвращаемся на внешний уровень, и если завести там второй QTextList,
    // нумерация начнётся заново — "1. 2. 1." вместо "1. 2. 3.". Поэтому список
    // помнится по уровням, а вложение лишь гасит прогоны глубже.
    struct ListRun {
        QTextList* list = nullptr;
        bool ordered = false;
        bool alive = false;
    };
    std::vector<ListRun> runs;

    for (const Block& b : doc) {
        QTextBlockFormat blockFmt;
        blockFmt.setTopMargin(kBlockSpacing);
        blockFmt.setBottomMargin(kBlockSpacing);

        QTextCharFormat charFmt = baseChar;
        charFmt.setFontPointSize(basePointSize);

        const bool raw = !b.rawSource.empty();
        const bool list = !raw && isList(b.kind);

        QString text;
        if (raw) {
            text = withoutTrailingNewline(b.rawSource);
            charFmt = monospace(charFmt);
            charFmt.setForeground(QColor(0x88, 0x88, 0x88));
        } else {
            switch (b.kind) {
                case Kind::Heading:
                    blockFmt.setHeadingLevel(b.headingLevel);
                    charFmt.setFontWeight(QFont::Bold);
                    charFmt.setFontPointSize(basePointSize *
                                             kHeadingScale[b.headingLevel - 1]);
                    blockFmt.setTopMargin(kBlockSpacing * 2);
                    break;
                case Kind::Code:
                    text = withoutTrailingNewline(b.text);
                    charFmt = monospace(charFmt);
                    blockFmt.setBackground(QColor(0, 0, 0, 12));
                    break;
                case Kind::Quote:
                    // Курсивом цитату не выделяем: тогда настоящий _курсив_
                    // внутри неё стал бы неотличим от остального текста.
                    blockFmt.setLeftMargin(kIndentPerLevel);
                    charFmt.setForeground(QColor(0x55, 0x55, 0x55));
                    break;
                default:
                    break;
            }
            if (b.kind != Kind::Code) text = toQt(b.text);
        }

        if (b.kind == Kind::TaskUnchecked && !raw)
            blockFmt.setMarker(QTextBlockFormat::MarkerType::Unchecked);
        else if (b.kind == Kind::TaskChecked && !raw)
            blockFmt.setMarker(QTextBlockFormat::MarkerType::Checked);

        if (list) {
            blockFmt.setTopMargin(0);
            blockFmt.setBottomMargin(0);
        }

        if (first) {
            cursor.setBlockFormat(blockFmt);
            cursor.setBlockCharFormat(charFmt);
            first = false;
        } else {
            cursor.insertBlock(blockFmt, charFmt);
        }

        if (list) {
            const size_t level = static_cast<size_t>(b.level);
            if (runs.size() <= level) runs.resize(level + 1);
            ListRun& r = runs[level];

            if (r.alive && r.list != nullptr && r.ordered == isOrdered(b.kind)) {
                r.list->add(cursor.block());
            } else {
                QTextListFormat listFmt;
                listFmt.setStyle(isOrdered(b.kind) ? QTextListFormat::ListDecimal
                                                   : QTextListFormat::ListDisc);
                listFmt.setIndent(b.level + 1);
                r.list = cursor.createList(listFmt);
                r.ordered = isOrdered(b.kind);
                r.alive = true;
            }
            for (size_t k = level + 1; k < runs.size(); ++k) runs[k].alive = false;
        } else {
            for (ListRun& r : runs) r.alive = false;
        }

        const int blockStart = cursor.position();
        cursor.insertText(text, charFmt);
        if (!raw && !b.inlines.empty()) applySpans(cursor, blockStart, b, charFmt);
    }

    cursor.endEditBlock();
}

}  // namespace zametti
