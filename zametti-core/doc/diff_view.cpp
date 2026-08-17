#include "diff_view.h"

#include "doc_model.h"

#include "settings.h"

#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>

namespace zametti::diff {
namespace {

// Цвет метки. Одинаковый во всех видах: полоска на поле, заливка строки и
// заглушка обязаны говорить одно и то же.
QColor colorOf(Mark mark) {
    const Appearance& look = appearance();
    switch (mark) {
        case Mark::Added: return look.diffAdded;
        case Mark::Removed: return look.diffRemoved;
        case Mark::Changed: return look.diffChanged;
        case Mark::Same: break;
    }
    return QColor();
}

}  // namespace

QString gapLabel(int lines) {
    // Безличная форма («удалено: 1 строка», «удалено: 5 строк») — единственная,
    // которая живёт со всеми тремя окончаниями сразу.
    const int tens = lines % 100;
    const int ones = lines % 10;
    QString word = QStringLiteral("строк");
    if (tens < 11 || tens > 14) {
        if (ones == 1) word = QStringLiteral("строка");
        else if (ones >= 2 && ones <= 4) word = QStringLiteral("строки");
    }
    return QStringLiteral("удалено: %1 %2").arg(lines).arg(word);
}

namespace {

// Слипнутся ли два блока в файле, стой они подряд без пустой строки. Правило
// одно на всех — оно живёт в block_kind.h, — и спрашивается здесь потому, что
// иллюстрированная копия собирается тем же сборщиком, что и заметка: жить по
// общим правилам ей дешевле, чем объясняться.
bool merges(const Piece& previous, const Piece& next) {
    return wouldMerge(previous.kind, previous.raw, previous.isClosedHtmlComment(), next.kind,
                      next.raw, next.level);
}

Piece vspacePiece() {
    Piece out;
    out.kind = Kind::VSpace;
    return out;
}

}  // namespace

Illustrated illustrate(const std::vector<Piece>& snapshot, const BlockMarks& marks) {
    Illustrated out;
    out.blocks.reserve(snapshot.size() + size_t(marks.gapBefore.size()) + 1);

    // Вспомогательная строка курсивом: она не текст заметки, и выглядеть как
    // текст заметки не должна.
    const auto addLabel = [&](int lines) {
        Piece label;
        label.text = gapLabel(lines);
        Run italic;
        italic.start = 0;
        italic.end = int32_t(label.text.size());
        italic.set(InlineItalic, true);
        label.runs.push_back(italic);
        if (!out.blocks.empty() && merges(out.blocks.back(), label)) {
            out.blocks.push_back(vspacePiece());
            out.blockMark.append(Mark::Same);
            out.sourceBlock.append(-1);
        }
        out.blocks.push_back(std::move(label));
        out.blockMark.append(Mark::Removed);
        out.sourceBlock.append(-1);
    };

    for (size_t i = 0; i < snapshot.size(); ++i) {
        const auto gap = marks.gapBefore.constFind(int(i));
        if (gap != marks.gapBefore.constEnd() && gap.value() > 0) addLabel(gap.value());
        const Piece& block = snapshot[i];
        if (!out.blocks.empty() && merges(out.blocks.back(), block)) {
            out.blocks.push_back(vspacePiece());
            out.blockMark.append(Mark::Same);
            out.sourceBlock.append(-1);
        }
        out.blocks.push_back(block);
        out.blockMark.append(int(i) < marks.blocks.size() ? marks.blocks[int(i)] : Mark::Same);
        out.sourceBlock.append(int(i));
    }
    if (marks.gapAtEnd > 0) addLabel(marks.gapAtEnd);
    return out;
}

void buildPlainDocument(const Result& result, QTextDocument& target,
                        QVector<Mark>* markOfBlock) {
    const Appearance& look = appearance();
    target.clear();
    if (markOfBlock != nullptr) markOfBlock->clear();

    // Поле слева — под полоски разности: без него они легли бы прямо на первый
    // знак строки (видно на снимке приёмки).
    target.setDocumentMargin(look.diffBarWidth * 4);

    QTextCharFormat text;
    text.setFontFamilies({look.codeFamily});
    setFontStep(text, look.codeStep);

    QTextCursor caret(&target);
    bool first = true;
    for (const Row& row : result.rows) {
        QTextBlockFormat block;
        // Заливка строки — едва заметная: моноширинный markdown должен
        // читаться как markdown, а не как светофор. Само же «сюда смотреть»
        // говорит полоска на поле, она сплошная.
        if (row.mark != Mark::Same) {
            QColor tint = colorOf(row.mark);
            tint.setAlpha(qBound(0, look.diffTint, 255));
            block.setBackground(tint);
        }
        if (first) {
            caret.setBlockFormat(block);
            caret.setCharFormat(text);
            first = false;
        } else {
            caret.insertBlock(block, text);
        }
        // Строки, которой на этой стороне нет, не показываем пустой строкой с
        // выдумкой — она и есть пустая. Место под неё остаётся, и в этом весь
        // смысл: по Alt текст в ней появляется, а всё вокруг стоит намертво.
        caret.insertText(row.text());
        if (markOfBlock != nullptr) markOfBlock->append(row.mark);
    }
    if (result.rows.isEmpty() && markOfBlock != nullptr) markOfBlock->append(Mark::Same);
}

}  // namespace zametti::diff
