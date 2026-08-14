// ВРЕМЕННЫЙ МОСТИК К УМИРАЮЩЕМУ ПРЕДСТАВЛЕНИЮ. Зачем он — см. ir_bridge.h.
//
// Направление обмена здесь ровно одно на каждую сторону:
//   parse()     — новый разбор даёт логические блоки, мостик перекладывает их
//                 в арену представления;
//   serialize() — представление превращается в живой документ, и заметка
//                 записывает себя новым писателем;
//   buildDocument()/patchDocument() — представление превращается в те же
//                 логические блоки, которые ждёт новая сборка.
//
// Файл умрёт вместе с ir.h, и ничего, кроме этих подписей, в нём нет.

#include "ir_bridge.h"

#include "document_impl.h"
#include "ir.h"
#include "parser.h"
#include "serializer.h"

namespace zametti {

// --- представление ← логические блоки --------------------------------------

Document parse(std::string_view markdown) {
    std::vector<Piece> blocks;
    NoteHeader header;
    parsePieces(markdown, blocks, header);

    Document ir;
    ir.meta.present = header.present();
    ir.meta.blankAfter = header.blankAfter();
    ir.meta.lines = header.lines();
    ir.chars.reserve(draftReserveFor(markdown.size()));
    ir.blocks.reserve(blocks.size());

    std::vector<Inline> spans;
    for (const Piece& piece : blocks) {
        Block block;
        block.kind = piece.kind;
        block.marker = piece.marker;
        block.checked = piece.checked;
        block.raw = piece.raw;
        block.headingLevel = int8_t(piece.headingLevel);
        block.html = piece.html;
        block.level = int16_t(piece.level);
        block.text = ir.append(piece.text);
        block.info = ir.append(piece.info);

        spans.clear();
        spans.reserve(piece.runs.size());
        for (const Run& run : piece.runs) {
            Inline span;
            span.text = Range{run.start, run.end};
            span.href = ir.append(run.href);
            span.title = ir.append(run.title);
            span.flags = run.flags;
            spans.push_back(span);
        }
        block.inlines = ir.appendInlines(spans);
        ir.blocks.push_back(block);
    }
    ir.validate();
    return ir;
}

// --- логические блоки ← представление --------------------------------------

std::vector<Piece> piecesOf(const Document& ir) {
    std::vector<Piece> blocks;
    blocks.reserve(ir.blocks.size());
    for (const Block& b : ir.blocks) {
        Piece piece;
        piece.kind = b.kind;
        piece.marker = b.marker;
        piece.html = b.html;
        piece.level = b.level;
        piece.headingLevel = b.headingLevel;
        piece.checked = b.checked;
        piece.raw = b.raw;
        piece.info = std::string(ir.info(b));
        piece.text = std::string(ir.text(b));
        piece.trailingNewline = !piece.text.empty() && piece.text.back() == '\n';
        piece.runs.reserve(size_t(b.inlines.size()));
        for (const Inline& s : ir.inlines(b)) {
            Run run;
            run.start = s.text.start;
            run.end = s.text.end;
            run.flags = s.flags;
            run.href = std::string(ir.href(s));
            run.title = std::string(ir.title(s));
            piece.runs.push_back(std::move(run));
        }
        blocks.push_back(std::move(piece));
    }
    return blocks;
}

void buildDocument(const Document& ir, QTextDocument& target) {
    buildDocument(piecesOf(ir), target);
}

bool patchDocument(const Document& built, const Document& now, const Document& to,
                   QTextDocument& target) {
    return patchDocument(piecesOf(built), piecesOf(now), piecesOf(to), target);
}

// --- запись ----------------------------------------------------------------

namespace {

// ZDocument, наполненный из представления. Ходить внутрь класса можно только
// его же частям — этот файл ею и является.
class IrBridge : public ZDocument {
public:
    explicit IrBridge(const Document& ir) {
        d_->header.setLines(ir.meta.lines);
        d_->header.setPresent(ir.meta.present);
        d_->header.setBlankAfter(ir.meta.blankAfter);
        buildDocument(piecesOf(ir), d_->text);
    }
};

}  // namespace

std::string serialize(const Document& doc) { return IrBridge(doc).toMarkdown(); }

// Та же умирающая подпись с картой строк. Карту теперь даёт заметка
// (ZDocument::sourceLines), а здесь она лишь переводится обратно в вид «блок →
// первая строка и сколько их», который ждут старые потребители.
std::string serialize(const Document& doc, std::vector<BlockLines>* map) {
    const IrBridge bridge(doc);
    const std::vector<SourceLine> lines = bridge.sourceLines();

    std::string out;
    for (size_t i = 0; i + 1 < lines.size() || (i < lines.size() && !lines[i].text.isEmpty());
         ++i) {
        const QByteArray utf8 = lines[i].text.toUtf8();
        out.append(utf8.constData(), size_t(utf8.size()));
        out.push_back('\n');
    }
    if (map != nullptr) {
        map->clear();
        for (size_t i = 0; i < lines.size(); ++i) {
            const int block = lines[i].block;
            if (block < 0) continue;
            if (size_t(block) >= map->size()) map->resize(size_t(block) + 1, BlockLines{-1, 0});
            BlockLines& at = (*map)[size_t(block)];
            if (at.first < 0) at.first = int(i);
            ++at.count;
        }
        for (BlockLines& b : *map)
            if (b.first < 0) b.first = 0;
    }
    return out;
}

}  // namespace zametti
