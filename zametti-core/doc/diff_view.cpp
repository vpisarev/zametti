#include "diff_view.h"

#include "doc_model.h"
#include "document_builder.h"

namespace zametti::diff {

QString gapLabel(int lines) {
    // Английские формы: «removed: 1 line», «removed: 5 lines».
    const QString word = lines == 1 ? QStringLiteral("line") : QStringLiteral("lines");
    return QStringLiteral("removed: %1 %2").arg(lines).arg(word);
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


}  // namespace zametti::diff
