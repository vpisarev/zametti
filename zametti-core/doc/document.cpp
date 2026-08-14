#include "document.h"

#include "archive.h"
#include "doc_model.h"
#include "document_builder.h"
#include "document_reader.h"
#include "parser.h"
#include "times.h"
#include "serializer.h"

#include <QRegularExpression>

namespace zametti {

// РЕАЛИЗАЦИЯ ПЕРЕХОДНАЯ, И ЭТО СКАЗАНО ВСЛУХ.
//
// Внутри пока живёт IR: loadMarkdown разбирает в него, toMarkdown из него
// пишет. Так сделано нарочно — сперва интерфейс и наборы, потом нутро. Когда
// md4c начнёт строить QTextDocument напрямую, а писатель пойдёт по документу,
// ЭТОТ ФАЙЛ поменяется целиком, а наборы — ни одной строкой. Если наборы
// придётся править, значит интерфейс был плох, и это повод остановиться.
struct ZDocument::Data {
    Document ir;
    QTextDocument text;
    bool textFresh = false;   // собран ли живой документ из нынешнего IR

    void refreshText() {
        if (textFresh) return;
        buildDocument(ir, text);
        textFresh = true;
    }
};

ZDocument::ZDocument() : d_(std::make_unique<Data>()) {
    // Вёрстка выключена: пока документ не показывают, считать строки и глифы
    // незачем. Вид включит её сам, когда возьмёт документ себе.
    d_->text.setLayoutEnabled(false);
}

ZDocument::~ZDocument() = default;
ZDocument::ZDocument(ZDocument&&) noexcept = default;
ZDocument& ZDocument::operator=(ZDocument&&) noexcept = default;

bool ZDocument::loadMarkdown(std::string_view bytes) {
    // НОРМАЛИЗАЦИЯ ПРОБЕЛОВ — ЧАСТЬ ВВОЗА, а не отдельный шаг.
    //
    // Так делают все нынешние места вызова: и ввоз заметки в хранилище, и
    // причёсывание файла при открытии, и проверка хранилища. Не делать её
    // здесь значило бы, что ZDocument читает не то же, что читает программа, —
    // и первый же набор на заметках владельца поймал бы разницу как «дрейф».
    d_->ir = parse(normaliseSpaces(bytes));
    d_->textFresh = false;
    return true;
}

std::string ZDocument::toMarkdown() const { return serialize(d_->ir); }

Digest ZDocument::digest() const {
    const std::string bytes = toMarkdown();
    return hashOf(std::string_view(bytes));
}

bool ZDocument::isCanonical(std::string_view original) const {
    const std::string canonical = toMarkdown();
    return canonical.size() == original.size() &&
           std::string_view(canonical) == original;
}

const NoteMeta& ZDocument::meta() const { return d_->ir.meta; }
NoteMeta& ZDocument::meta() { return d_->ir.meta; }

// --- шапка -----------------------------------------------------------------
//
// Ключи названы здесь и только здесь. До этого они жили голыми литералами в
// девяти файлах, и опечатка в одном из них никак бы себя не выдала.
namespace {
constexpr char kParent[] = "parent";
constexpr char kRole[] = "role";
constexpr char kCreated[] = "created";
constexpr char kModified[] = "modified";
constexpr char kFolder[] = "folder";
constexpr char kLost[] = "lost";
}  // namespace

QString ZDocument::parentId() const {
    return QString::fromStdString(d_->ir.meta.get(kParent));
}

void ZDocument::setParentId(const QString& id) {
    // Пусто снимает ключ — «в корне». Так это и записано в справочнике.
    d_->ir.meta.set(kParent, id.toStdString());
}

bool ZDocument::isFolder() const { return d_->ir.meta.get(kRole) == kFolder; }
bool ZDocument::isLost() const { return d_->ir.meta.get(kRole) == kLost; }

bool ZDocument::isArchived() const { return store::isArchivedMeta(d_->ir.meta); }

void ZDocument::setArchived(bool archived) {
    store::setArchivedMeta(d_->ir.meta, archived);
}

QString ZDocument::created() const {
    return QString::fromStdString(d_->ir.meta.get(kCreated));
}

QString ZDocument::modified() const {
    return QString::fromStdString(d_->ir.meta.get(kModified));
}

void ZDocument::stampModified() {
    d_->ir.meta.set(kModified, store::isoNow().toStdString());
}

// --- о чём заметка ---------------------------------------------------------

namespace {

// Первый СОДЕРЖАТЕЛЬНЫЙ блок: пустые строки, html-комментарии и шапка не в
// счёт. Правило было написано трижды — в поиске по хранилищу, в дереве и в
// стабе архива; здесь оно одно.
const Block* firstContentBlock(const Document& doc) {
    for (const Block& b : doc.blocks) {
        if (b.kind == Kind::VSpace) continue;
        if (b.kind == Kind::Html) continue;
        if (b.raw && doc.isClosedHtmlComment(b)) continue;
        return &b;
    }
    return nullptr;
}

QString firstLineOf(const Document& doc, const Block& block) {
    const QString whole = blockText(doc, block);
    const qsizetype end = whole.indexOf(QLatin1Char('\n'));
    return (end < 0 ? whole : whole.left(end)).simplified();
}

}  // namespace

QString ZDocument::title() const {
    const Block* first = firstContentBlock(d_->ir);
    return first ? firstLineOf(d_->ir, *first).left(64) : QString();
}

QString ZDocument::snippet(int limit) const {
    const Block* first = firstContentBlock(d_->ir);
    if (!first) return {};
    QString out;
    bool afterTitle = false;
    for (const Block& b : d_->ir.blocks) {
        if (&b == first) { afterTitle = true; continue; }
        if (!afterTitle) continue;
        if (b.kind == Kind::VSpace || b.kind == Kind::Html) continue;
        if (!out.isEmpty()) out += QLatin1Char(' ');
        out += blockText(d_->ir, b).simplified();
        if (out.size() >= limit) break;
    }
    return out.left(limit);
}

std::string ZDocument::archiveStub() const { return store::stubBytes(d_->ir); }

bool ZDocument::isEmpty() const {
    for (const Block& b : d_->ir.blocks)
        if (b.raw || b.kind != Kind::VSpace) return false;
    return true;
}

// --- вложения --------------------------------------------------------------

std::vector<Attachment> ZDocument::attachments() const {
    std::vector<Attachment> out;
    for (const Block& block : d_->ir.blocks) {
        for (const Inline& span : d_->ir.inlines(block)) {
            if (!span.image()) continue;
            const QString href = QString::fromUtf8(d_->ir.href(span).data(),
                                                   qsizetype(d_->ir.href(span).size()));
            if (href.isEmpty()) continue;
            Attachment a;
            // Атрибуты живут во фрагменте адреса: «id.webp#w=600&align=left».
            const qsizetype hash = href.indexOf(QLatin1Char('#'));
            a.id = hash < 0 ? href : href.left(hash);
            if (hash >= 0) {
                const auto parts = href.mid(hash + 1).split(QLatin1Char('&'));
                for (const QString& part : parts) {
                    if (part.startsWith(QLatin1String("w=")))
                        a.width = part.mid(2).toInt();
                    else if (part.startsWith(QLatin1String("align=")))
                        a.align = part.mid(6);
                }
            }
            a.alt = QString::fromUtf8(d_->ir.text(block, span).data(),
                                      qsizetype(d_->ir.text(block, span).size()));
            out.push_back(std::move(a));
        }
    }
    return out;
}

int ZDocument::rewriteAttachments(const std::function<QString(const QString&)>& rename) {
    int changed = 0;
    for (Block& block : d_->ir.blocks) {
        for (Inline& span : d_->ir.inlines(block)) {
            if (!span.image()) continue;
            const std::string_view href = d_->ir.href(span);
            if (href.empty()) continue;
            const QString whole = QString::fromUtf8(href.data(), qsizetype(href.size()));
            const qsizetype hash = whole.indexOf(QLatin1Char('#'));
            const QString name = hash < 0 ? whole : whole.left(hash);
            const QString fresh = rename(name);
            if (fresh.isEmpty() || fresh == name) continue;
            const QString rebuilt = hash < 0 ? fresh : fresh + whole.mid(hash);
            const QByteArray utf8 = rebuilt.toUtf8();
            span.href = d_->ir.append(std::string_view(utf8.constData(), size_t(utf8.size())));
            ++changed;
        }
    }
    if (changed) d_->textFresh = false;
    return changed;
}

// --- поиск -----------------------------------------------------------------

std::vector<Hit> ZDocument::find(const Query& query) const {
    return findInDocument(d_->ir, query);
}

HitLine ZDocument::hitLine(const Hit& hit, int radius) const {
    return zametti::hitLine(d_->ir, hit, radius);
}

// --- живой документ --------------------------------------------------------

QTextDocument& ZDocument::textDocument() {
    d_->refreshText();
    return d_->text;
}

const QTextDocument& ZDocument::textDocument() const {
    const_cast<Data*>(d_.get())->refreshText();
    return d_->text;
}

const Document& ZDocument::ir() const { return d_->ir; }

void ZDocument::setIr(Document doc) {
    d_->ir = std::move(doc);
    d_->textFresh = false;
}

}  // namespace zametti
