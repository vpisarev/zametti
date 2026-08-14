#include "document_impl.h"

#include "archive.h"
#include "doc_model.h"
#include "document_builder.h"
#include "document_pieces.h"
#include "document_reader.h"
#include "document_saver.h"
#include "note_header.h"
#include "parser.h"
#include "serializer.h"
#include "text_stats.h"
#include "times.h"

#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>

namespace zametti {

// ЖИВАЯ МОДЕЛЬ — QTextDocument, и только он. Шапка живёт рядом: в документе её
// нет и быть не должно, редактор её не видит.
//
// ПЕРЕХОДНОЕ, И ЭТО СКАЗАНО ВСЛУХ: разбор и запись пока ходят через
// промежуточное представление — parse() строит его, buildDocument переносит в
// документ, и обратно тем же путём. Представление при этом ВРЕМЕННОЕ: оно
// живёт внутри двух функций и наружу не выходит. Заменить эти два мостика на
// прямой проход md4c → QTextDocument и обратно — следующий шаг, и наборы его
// сторожат: круг обязан остаться неподвижной точкой.

namespace {

// Мостик наружу: документ + шапка как то представление, которое пока ещё
// понимают сериализатор и хранилище.
Document irOf(const QTextDocument& text, const NoteHeader& header) {
    Document ir = readDocument(text);
    ir.meta.lines = header.lines();
    ir.meta.present = header.present();
    ir.meta.blankAfter = header.blankAfter();
    return ir;
}

void headerFrom(const NoteMeta& meta, NoteHeader& out) {
    out.setLines(meta.lines);
    out.setPresent(meta.present);
    out.setBlankAfter(meta.blankAfter);
}

// Ключи шапки названы ОДИН раз. До этого «parent», «role», «archived» и прочие
// жили голыми литералами в девяти файлах, и опечатка в одном никак бы себя не
// выдала.
constexpr char kParent[] = "parent";
constexpr char kRole[] = "role";
constexpr char kCreated[] = "created";
constexpr char kModified[] = "modified";
constexpr char kFolder[] = "folder";
constexpr char kLost[] = "lost";

}  // namespace

ZDocument::ZDocument() : d_(std::make_shared<Data>()) {}

ZDocument ZDocument::clone() const {
    ZDocument out;
    out.d_->header = d_->header;
    // Содержимое переносим байтами: копировать QTextDocument иначе значило бы
    // тащить с собой стек отмены и состояние вёрстки, а слепку они не нужны.
    out.loadMarkdown(toMarkdown());
    return out;
}

// --- круг с диском ---------------------------------------------------------

bool ZDocument::loadMarkdown(std::string_view bytes) {
    // НОРМАЛИЗАЦИЯ ПРОБЕЛОВ — ЧАСТЬ ВВОЗА, а не отдельный шаг: так делают все
    // нынешние места вызова, и без неё ZDocument читал бы не то же, что читает
    // программа.
    //
    // Промежуточного представления на этом пути больше нет: md4c собирает
    // логические блоки, сборщик кладёт их в живой документ. Черновик разбора
    // умирает вместе с вызовом.
    std::vector<Piece> blocks;
    parsePieces(normaliseSpaces(bytes), blocks, d_->header);
    buildDocument(blocks, d_->text);
    return true;
}


Digest ZDocument::digest() const {
    const std::string bytes = toMarkdown();
    return hashOf(std::string_view(bytes));
}

bool ZDocument::isCanonical(std::string_view original) const {
    const std::string canonical = toMarkdown();
    return std::string_view(canonical) == original;
}

// --- шапка -----------------------------------------------------------------

QString ZDocument::parentId() const {
    return QString::fromStdString(d_->header.get(kParent));
}

void ZDocument::setParentId(const QString& id) {
    d_->header.set(kParent, id.toStdString());
}

bool ZDocument::isFolder() const { return d_->header.get(kRole) == kFolder; }
bool ZDocument::isLost() const { return d_->header.get(kRole) == kLost; }

bool ZDocument::isArchived() const {
    NoteMeta meta;
    meta.lines = d_->header.lines();
    meta.present = d_->header.present();
    return store::isArchivedMeta(meta);
}

void ZDocument::setArchived(bool archived) {
    NoteMeta meta;
    meta.lines = d_->header.lines();
    meta.present = d_->header.present();
    meta.blankAfter = d_->header.blankAfter();
    store::setArchivedMeta(meta, archived);
    headerFrom(meta, d_->header);
}

QString ZDocument::created() const {
    return QString::fromStdString(d_->header.get(kCreated));
}

QString ZDocument::modified() const {
    return QString::fromStdString(d_->header.get(kModified));
}

void ZDocument::stampModified() {
    d_->header.set(kModified, store::isoNow().toStdString());
}

QString ZDocument::headerValue(const QString& key) const {
    return QString::fromStdString(d_->header.get(key.toStdString()));
}

void ZDocument::setHeaderValue(const QString& key, const QString& value) {
    d_->header.set(key.toStdString(), value.toStdString());
}

bool ZDocument::hasHeader() const { return d_->header.present(); }

// --- о чём заметка ---------------------------------------------------------

namespace {

// Первый СОДЕРЖАТЕЛЬНЫЙ блок: пустые строки и html-комментарии не в счёт.
// Правило было написано ТРИЖДЫ — в поиске по хранилищу, в дереве и в стабе
// архива; здесь оно одно.
QTextBlock firstContentBlock(const QTextDocument& doc) {
    for (QTextBlock b = doc.begin(); b.isValid(); b = b.next()) {
        const Kind kind = kindOf(b);
        if (kind == Kind::VSpace || kind == Kind::Html) continue;
        return b;
    }
    return {};
}

QString firstLineOf(const QTextBlock& block) {
    const QString whole = block.text();
    const qsizetype end = whole.indexOf(QChar::LineSeparator);
    return (end < 0 ? whole : whole.left(end)).simplified();
}

}  // namespace

QString ZDocument::title() const {
    const QTextBlock first = firstContentBlock(d_->text);
    return first.isValid() ? firstLineOf(first).left(64) : QString();
}

QString ZDocument::snippet(int limit) const {
    const QTextBlock first = firstContentBlock(d_->text);
    if (!first.isValid()) return {};
    QString out;
    for (QTextBlock b = first.next(); b.isValid(); b = b.next()) {
        const Kind kind = kindOf(b);
        if (kind == Kind::VSpace || kind == Kind::Html) continue;
        if (!out.isEmpty()) out += QLatin1Char(' ');
        out += b.text().simplified();
        if (out.size() >= limit) break;
    }
    return out.left(limit);
}

std::string ZDocument::archiveStub() const { return store::stubBytes(irOf(d_->text, d_->header)); }

bool ZDocument::isEmpty() const {
    for (QTextBlock b = d_->text.begin(); b.isValid(); b = b.next()) {
        if (isRawBlock(b) || kindOf(b) != Kind::VSpace) return false;
    }
    return true;
}

Stats ZDocument::stats() const {
    const NoteStats counted = documentStats(d_->text);
    Stats out;
    out.words = counted.words;
    out.lines = counted.lines;
    out.blocks = counted.blocks;
    out.images = counted.images;
    out.valid = counted.valid;
    return out;
}

// --- блоки -----------------------------------------------------------------

int ZDocument::blockCount() const { return d_->text.blockCount(); }

BlockInfo ZDocument::blockAt(int index) const {
    BlockInfo out;
    const QTextBlock block = d_->text.findBlockByNumber(index);
    if (!block.isValid()) return out;
    out.index = index;
    out.kind = kindOf(block);
    const MarkerStyle style = markerOf(block);
    out.marker = style.marker;
    out.level = levelOf(block);
    out.headingLevel = block.blockFormat().headingLevel();
    out.checked = style.checked;
    out.raw = isRawBlock(block);
    out.continuation = isContinuationBlock(block);
    out.info = block.blockFormat().stringProperty(InfoProperty);
    out.text = block.text();
    return out;
}

std::vector<BlockInfo> ZDocument::blocks() const {
    std::vector<BlockInfo> out;
    out.reserve(size_t(d_->text.blockCount()));
    int index = 0;
    for (QTextBlock b = d_->text.begin(); b.isValid(); b = b.next(), ++index)
        out.push_back(blockAt(index));
    return out;
}


// --- вложения --------------------------------------------------------------

std::vector<Attachment> ZDocument::attachments() const {
    std::vector<Attachment> out;
    for (QTextBlock b = d_->text.begin(); b.isValid(); b = b.next()) {
        for (auto it = b.begin(); it != b.end(); ++it) {
            const QTextFragment fragment = it.fragment();
            if (!fragment.isValid()) continue;
            const QTextCharFormat format = fragment.charFormat();
            if ((format.intProperty(SpanStyleProperty) & SpanImage) == 0) continue;
            const QString href = format.anchorHref();
            if (href.isEmpty()) continue;

            Attachment a;
            // Атрибуты живут во фрагменте адреса: «id.webp#w=600&align=left».
            const qsizetype hash = href.indexOf(QLatin1Char('#'));
            a.id = hash < 0 ? href : href.left(hash);
            if (hash >= 0) {
                for (const QString& part : href.mid(hash + 1).split(QLatin1Char('&'))) {
                    if (part.startsWith(QLatin1String("w=")))
                        a.width = part.mid(2).toInt();
                    else if (part.startsWith(QLatin1String("align=")))
                        a.align = part.mid(6);
                }
            }
            a.alt = fragment.text();
            out.push_back(std::move(a));
        }
    }
    return out;
}

int ZDocument::rewriteAttachments(const std::function<QString(const QString&)>& rename) {
    int changed = 0;
    QTextCursor cursor(&d_->text);
    cursor.beginEditBlock();
    for (QTextBlock b = d_->text.begin(); b.isValid(); b = b.next()) {
        for (auto it = b.begin(); it != b.end(); ++it) {
            const QTextFragment fragment = it.fragment();
            if (!fragment.isValid()) continue;
            QTextCharFormat format = fragment.charFormat();
            if ((format.intProperty(SpanStyleProperty) & SpanImage) == 0) continue;
            const QString href = format.anchorHref();
            if (href.isEmpty()) continue;

            const qsizetype hash = href.indexOf(QLatin1Char('#'));
            const QString name = hash < 0 ? href : href.left(hash);
            const QString fresh = rename(name);
            if (fresh.isEmpty() || fresh == name) continue;

            format.setAnchorHref(hash < 0 ? fresh : fresh + href.mid(hash));
            cursor.setPosition(fragment.position());
            cursor.setPosition(fragment.position() + fragment.length(),
                               QTextCursor::KeepAnchor);
            cursor.setCharFormat(format);
            ++changed;
        }
    }
    cursor.endEditBlock();
    return changed;
}

// --- поиск -----------------------------------------------------------------

std::vector<Hit> ZDocument::find(const Query& query) const {
    return findInDocument(readDocument(d_->text), query);
}

HitLine ZDocument::hitLine(const Hit& hit, int radius) const {
    return zametti::hitLine(readDocument(d_->text), hit, radius);
}

// --- сравнение -------------------------------------------------------------

bool ZDocument::sameSkeleton(const ZDocument& other) const {
    return zametti::sameSkeleton(irOf(d_->text, d_->header), irOf(other.d_->text, other.d_->header));
}

bool ZDocument::sameBody(const ZDocument& other) const {
    // Шапку выбрасываем целиком: в ней `modified`, и без этого всякое
    // сравнение начиналось бы с неё.
    return serialize(readDocument(d_->text)) == serialize(readDocument(other.d_->text));
}

}  // namespace zametti
