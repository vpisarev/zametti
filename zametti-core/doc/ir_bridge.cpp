// ВРЕМЕННЫЙ МОСТИК К УМИРАЮЩЕМУ ПРЕДСТАВЛЕНИЮ.
//
// Промежуточное представление (zametti::Document — арена блоков и спанов)
// сносится. Пока его последние потребители не переведены на ZDocument, старая
// подпись serialize(Document) остаётся — но ведёт она уже В НОВЫЙ ПИСАТЕЛЬ:
// строит живой документ и просит заметку записать себя.
//
// Смысл мостика не в удобстве, а в СЕТИ. Через serialize(Document) сегодня
// проходят все корпусные проверки — круг на заметках владельца, идемпотентность
// на спецификациях CommonMark и GFM, фаззинг операций. Направив их в новый
// писатель, я получаю на нём всё прежнее покрытие сразу, а не после того, как
// перепишу каждого потребителя.
//
// Файл умрёт вместе с ir.h, и ничего, кроме этой подписи, в нём нет.

#include "document_impl.h"

#include "document_builder.h"
#include "ir.h"
#include "serializer.h"

namespace zametti {

namespace {

// ZDocument, наполненный из представления. Ходить внутрь класса можно только
// его же частям — этот файл ею и является.
class IrBridge : public ZDocument {
public:
    explicit IrBridge(const Document& ir) {
        d_->header.setLines(ir.meta.lines);
        d_->header.setPresent(ir.meta.present);
        d_->header.setBlankAfter(ir.meta.blankAfter);
        buildDocument(ir, d_->text);
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
