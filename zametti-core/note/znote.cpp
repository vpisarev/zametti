#include "znote.h"
#include "settings.h"

#include "times.h"

#include <QFile>
#include <QSaveFile>
#include <QFileInfo>

namespace zametti {
namespace {
// Ключи шапки названы ОДИН раз (прежде — здесь и в document.cpp).
constexpr char kParent[] = "parent";
constexpr char kRole[] = "role";
constexpr char kCreated[] = "created";
constexpr char kModified[] = "modified";
constexpr char kFolder[] = "folder";
constexpr char kLost[] = "lost";
constexpr char kRoot[] = "root";
constexpr char kSort[] = "sort";
// Books (brief 18): the role, and the two header keys the list shows.
constexpr char kBook[] = "book";
constexpr char kAuthor[] = "author";
constexpr char kYear[] = "year";
// The cover of a book: the attachment's file name next to the note (the fb2
// import writes it; the body does not show it — the list and the reading
// mode do, books2).
constexpr char kCover[] = "cover";
// Сколько знаков сниппета держим в метаданных: две-три строки списка при
// любой разумной ширине панели.
constexpr int kSnippetChars = 200;
}  // namespace
}  // namespace zametti

namespace zametti {

ZNote::ZNote(QString path, QByteArray fileBytes, Digest digest,
             std::shared_ptr<ZJournal> journal)
    : path_(std::move(path)),
      digest_(digest),
      lastSaved_(std::move(fileBytes)),
      journal_(journal != nullptr ? std::move(journal) : std::make_shared<ZJournal>()) {}

QString ZNote::id() const { return QFileInfo(path_).completeBaseName(); }

// --- круг файла ---------------------------------------------------------------

std::shared_ptr<const ZDocStyle> ZNote::styleFor(const NoteHeader& header) const {
    // THE LOOK IS CHOSEN BY THE ROLE, HERE AND ONLY HERE (brief 18): a book is
    // built with the reading style — serif, the red line, justified — and
    // every road to a note's document (editor, reader, history, import) goes
    // through load(). Switching the window between reading and editing
    // never rebuilds: the same document, the same undo stack.
    if (header.get(kRole) == kBook) return settings().readingStyle();
    return nullptr;
}

void ZNote::refreshStyle() {
    // A changed config (books2, owner's report: reading.codeStep "does
    // nothing"): the document carried the reading style copied at load, and a
    // rebuild for the new look reused that copy. The role chooses again.
    doc_.setStyle(styleFor(header_));
}

bool ZNote::load(std::string_view bytes) {
    NoteHeader lifted;
    std::vector<Piece> built;
    if (!doc_.loadMarkdown(bytes, &lifted, &built,
                           [this](const NoteHeader& header) { return styleFor(header); }))
        return false;
    header_ = lifted;
    // Блоки сборки — заплатке редактора; она сравнивает их с правленым.
    built_.set(std::move(built), doc_.revision());
    stats_.invalidate();
    search_.clear();
    return true;
}

const std::vector<ZDocument::OutlineEntry>& ZNote::outline() {
    if (!outline_.freshFor(doc_.revision())) outline_.set(doc_.outline(), doc_.revision());
    return outline_.value();
}

std::string ZNote::toMarkdown() const { return doc_.toMarkdown(header_); }

QByteArray ZNote::fileBytes(std::vector<Piece>* fileBlocks, bool* enriched, bool* reshaped) const {
    return doc_.fileBytes(header_, fileBlocks, enriched, reshaped);
}


bool ZNote::canonicaliseFile(const QString& path, std::string& text, Digest& digest) {
    ZNote note;
    note.load(text);
    if (!note.hasHeader()) return false;   // не наша заметка

    const std::string canonical = note.toMarkdown();
    if (canonical == text) return false;   // и так канон

    // Последний рубеж, тот же, что и при записи: причёсанное обязано читаться
    // обратно той же заметкой — шапка строка в строку, тело — строением.
    ZNote back;
    back.load(canonical);
    if (back.header_.present() != note.header_.present() ||
        back.header_.lines() != note.header_.lines() || !note.doc_.sameSkeleton(back.doc_))
        return false;

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return false;
    file.write(canonical.data(), qint64(canonical.size()));
    if (!file.commit()) return false;

    text = canonical;
    digest = hashOf(std::string_view(text));
    return true;
}

bool ZNote::isCanonical(std::string_view original) const {
    const std::string canonical = toMarkdown();
    return std::string_view(canonical) == original;
}

SaveOutcome ZNote::save(const QString& path, const QString& timestamp, const Digest& known,
                        const std::vector<Piece>* prebuiltBlocks,
                        const QByteArray* prebuiltText) {
    return doc_.saveTo(path, timestamp, nullptr, header_, known, prebuiltBlocks, prebuiltText);
}

// --- метаданные ---------------------------------------------------------------

ZNote::Metadata ZNote::metadata() const {
    Metadata m;
    m.id_ = id();
    m.path_ = path_.isEmpty() ? QString() : QFileInfo(path_).absoluteFilePath();
    m.parent_ = parentId();
    // ВРЕМЕНА ПРИВОДЯТСЯ К UTC ПРЯМО ЗДЕСЬ. В шапке они с офсетом («…+02:00»),
    // а сравниваются и сортируются строками — лексикографически «21:40+02:00»
    // больше «19:40Z», хотя это один момент. Показывает даты список, и он
    // переводит в местную зону сам.
    m.modified_ = store::comparableTime(modified().toStdString());
    m.created_ = store::comparableTime(created().toStdString());
    m.sortMark_ = sortMark();
    m.archived_ = isArchived();
    m.readOnly_ = isReadOnly();
    m.locked_ = isLocked();
    m.book_ = isBook();
    if (m.book_) {
        m.author_ = bookAuthor();
        m.year_ = bookYear();
        m.cover_ = bookCover();
    }
    m.folder_ = isFolder() || isLost();
    m.root_ = isRoot();
    m.lostFound_ = isLost();
    m.title_ = title();
    m.snippet_ = doc_.snippet(kSnippetChars);
    return m;
}

ZNote::Metadata ZNote::Metadata::fromFile(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    const QByteArray bytes = f.readAll();
    ZNote note(path, bytes, Digest{}, nullptr);
    if (!note.load(std::string_view(bytes.constData(), size_t(bytes.size())))) return {};
    Metadata m = note.metadata();
    // Времени в шапке нет — берём у файла (а созданию — время правки: лучше,
    // чем «в начале времён»).
    if (m.modified_.isEmpty())
        m.modified_ = QFileInfo(path).lastModified().toUTC().toString(Qt::ISODate);
    if (m.created_.isEmpty()) m.created_ = m.modified_;
    return m;
}

QString ZNote::parentId() const { return QString::fromStdString(header_.get(kParent)); }
void ZNote::setParentId(const QString& id) { header_.set(kParent, id.toStdString()); }
// КОРЕНЬ — ПАПКА. Тогда правило «тело папки — ровно один заголовок» и все
// правила дерева применяются к нему без единой оговорки: имя хранилища это и
// есть заголовок корневой заметки, а переименование по F2 — то, как оно
// меняется.
bool ZNote::isFolder() const {
    const std::string role = header_.get(kRole);
    return role == kFolder || role == kRoot;
}
bool ZNote::isRoot() const { return header_.get(kRole) == kRoot; }
bool ZNote::isLost() const { return header_.get(kRole) == kLost; }
QString ZNote::role() const { return QString::fromStdString(header_.get(kRole)); }
void ZNote::setRole(const QString& role) {
    if (role.isEmpty()) header_.unset(kRole);
    else header_.set(kRole, role.toStdString());
}
bool ZNote::isArchived() const { return header_.archived(); }
void ZNote::setArchived(bool archived) { header_.setArchived(archived); }
bool ZNote::isReadOnly() const { return header_.readOnly(); }
void ZNote::setReadOnly(bool readOnly) { header_.setReadOnly(readOnly); }
bool ZNote::isLocked() const { return header_.locked(); }
void ZNote::setLocked(bool locked) { header_.setLocked(locked); }
bool ZNote::isBook() const { return header_.get(kRole) == kBook; }
QString ZNote::bookAuthor() const { return QString::fromStdString(header_.get(kAuthor)); }
QString ZNote::bookYear() const { return QString::fromStdString(header_.get(kYear)); }
QString ZNote::bookCover() const { return QString::fromStdString(header_.get(kCover)); }
QString ZNote::created() const { return QString::fromStdString(header_.get(kCreated)); }
QString ZNote::modified() const { return QString::fromStdString(header_.get(kModified)); }
void ZNote::stampModified() { header_.set(kModified, store::isoNow().toStdString()); }
std::optional<SortOrder> ZNote::sortMark() const {
    return parseSortOrder(QString::fromStdString(header_.get(kSort)));
}
void ZNote::setSortMark(std::optional<SortOrder> order) { applySortMark(header_, order); }
QString ZNote::headerValue(const QString& key) const {
    return QString::fromStdString(header_.get(key.toStdString()));
}
void ZNote::setHeaderValue(const QString& key, const QString& value) {
    header_.set(key.toStdString(), value.toStdString());
}
QString ZNote::title() const {
    const QString own = doc_.title();
    return own.isEmpty() ? QStringLiteral("Untitled") : own;
}

ZDocument ZNote::replaceDoc(ZDocument fresh) {
    ZDocument previous = doc_;
    doc_ = std::move(fresh);
    // Другой документ — всё производное от прежнего вслух устарело: ревизия
    // у нового может совпасть случайно.
    built_.invalidate();
    stats_.invalidate();
    search_.clear();
    return previous;
}

NoteHeader ZNote::takeLostMeta() {
    NoteHeader lost = lostMeta_;
    lostMeta_ = NoteHeader();
    return lost;
}

void ZNote::markWritten(const Digest& digest, QByteArray written) {
    digest_ = digest;
    lastSaved_ = std::move(written);
}

bool ZNote::setSelfCheckFailed(bool failed) {
    if (failed == selfCheckFailed_) return false;
    selfCheckFailed_ = failed;
    return true;
}

}  // namespace zametti
