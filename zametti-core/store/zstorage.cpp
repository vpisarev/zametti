#include "zstorage.h"

#include "note_id.h"
#include "times.h"

#include "archive.h"
#include "journal.h"
#include "lost_found.h"
#include "store.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QSysInfo>

#ifdef Q_OS_UNIX
#include <cerrno>
#include <csignal>
#include <sys/types.h>
#endif

#include <cstdio>
#include <string>

namespace zametti {

namespace {
// Сколько знаков сниппета держим. Две-три строки списка при любой разумной
// ширине панели; резать точно по строкам нельзя — ширина известна только
// делегату, и она меняется вместе с разделителем.
constexpr int kSnippetChars = 200;
}  // namespace

bool ZStorage::isStoreRoot(const QString& dir) {
    return QFileInfo(dir + QStringLiteral("/.zametti")).isDir();
}

ZStorage::ZStorage(const QString& root)
    : root_(QDir::cleanPath(root)), store_(isStoreRoot(QDir::cleanPath(root))) {}

namespace {
// Жив ли процесс с таким pid. kill с нулевым сигналом ничего не шлёт, только
// проверяет право послать: единственный переносимый по UNIX способ спросить.
bool processAlive(qint64 pid) {
#ifdef Q_OS_UNIX
    return ::kill(pid_t(pid), 0) == 0 || errno == EPERM;
#else
    Q_UNUSED(pid);
    return true;   // на прочих системах не гадаем: пусть решает --unlock
#endif
}
}  // namespace

QString ZStorage::lockPath() const {
    return journal::storeLockPath(store_ ? root_ : QDir::tempPath());
}

bool ZStorage::isLocked() const { return lock_ != nullptr && lock_->isLocked(); }

ZStorage::LockReport ZStorage::forceUnlock() {
    LockReport report;
    QLockFile probe(lockPath());
    if (probe.getLockInfo(&report.holderPid, &report.holderHost, nullptr))
        report.note = QStringLiteral("снимаю замок хранилища (был за pid %1 на «%2»)")
                          .arg(report.holderPid)
                          .arg(report.holderHost);
    else
        report.note = QStringLiteral("замка на хранилище и не было");
    QFile::remove(lockPath());
    return report;
}

ZStorage::LockReport ZStorage::lock() {
    LockReport report;
    if (lock_ == nullptr) lock_ = std::make_shared<QLockFile>(lockPath());
    if (lock_->isLocked() || lock_->tryLock(0)) {
        report.locked = true;
        return report;
    }
    // Труп нашей машины — снимаем и пробуем снова; живой pid не трогаем.
    qint64 pid = 0;
    QString host;
    QString app;
    if (lock_->getLockInfo(&pid, &host, &app) && host == QSysInfo::machineHostName() &&
        pid > 0 && !processAlive(pid)) {
        report.note = QStringLiteral("снимаю забытый замок хранилища (pid %1 не жив)").arg(pid);
        QFile::remove(lockPath());
        if (lock_->tryLock(0)) {
            report.locked = true;
            return report;
        }
    }
    lock_->getLockInfo(&report.holderPid, &report.holderHost, &app);
    return report;
}

QString ZStorage::pathOf(const QString& id) const {
    return root_ + QLatin1Char('/') + id + QStringLiteral(".md");
}

QString ZStorage::idOfPath(const QString& path) { return QFileInfo(path).completeBaseName(); }

ZNoteHistory ZStorage::historyOf(const QString& id, const history::Rules& rules) const {
    // Журналы лежат под корнем (history/); каталогу .zametti для этого быть не
    // обязательно — так живут наборы на временном каталоге.
    if (root_.isEmpty() || id.isEmpty()) return ZNoteHistory();
    return ZNoteHistory(root_, id, rules);
}

bool ZStorage::readInfo(const QString& path, NoteInfo& out) const {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    const QByteArray bytes = f.readAll();
    ZDocument doc;
    doc.loadMarkdown(std::string_view(bytes.constData(), size_t(bytes.size())));

    out.id_ = idOfPath(path);
    out.path_ = QFileInfo(path).absoluteFilePath();
    out.parent_ = doc.parentId();
    // ВРЕМЕНА ПРИВОДЯТСЯ К UTC ПРЯМО ЗДЕСЬ. В шапке они с офсетом («…+02:00»),
    // а сравниваются и сортируются строками — лексикографически «21:40+02:00»
    // больше «19:40Z», хотя это один момент. Дальше ходит только сравнимая
    // форма; показывает даты список, и он переводит в местную зону сам.
    out.modified_ = store::comparableTime(doc.modified().toStdString());
    out.created_ = store::comparableTime(doc.created().toStdString());
    if (out.modified_.isEmpty())
        out.modified_ = QFileInfo(path).lastModified().toUTC().toString(Qt::ISODate);
    if (out.created_.isEmpty()) out.created_ = out.modified_;
    // Метка сортировки. Чужое значение не должно ни ронять программу, ни молча
    // подменяться на своё: жалуемся и показываем папку по наследству.
    const QString sort = doc.headerValue(QStringLiteral("sort"));
    out.sortMark_.reset();
    if (!sort.isEmpty()) {
        out.sortMark_ = doc.sortOrder();
        if (!out.sortMark_.has_value())
            std::fprintf(stderr, "непонятная метка сортировки [%s] в [%s] — папка наследует\n",
                         sort.toUtf8().constData(), path.toUtf8().constData());
    }
    out.archived_ = doc.isArchived();
    out.folder_ = doc.isFolder() || doc.isLost();
    out.lostFound_ = doc.isLost();
    // Заголовок и сниппет — глаголы заметки: то же правило «первый
    // содержательный блок» стоит в поиске по хранилищу и в стабе архива.
    out.title_ = doc.title();
    if (out.title_.isEmpty()) out.title_ = QStringLiteral("Без названия");
    out.snippet_ = doc.snippet(kSnippetChars);
    return true;
}

void ZStorage::reload() {
    notes_.clear();
    if (!store_) return;
    // Скан: только "<id>.md".
    for (const QFileInfo& info :
         QDir(root_).entryInfoList({QStringLiteral("*.md")}, QDir::Files)) {
        const QString stem = info.completeBaseName();
        if (!isValidNoteId(stem.toStdString())) continue;
        NoteInfo note;
        if (!readInfo(info.absoluteFilePath(), note)) {
            // Заметка есть на диске, но не читается — молчать нельзя: человек
            // видел бы пустое место в дереве и не узнал бы, что файл на месте.
            std::fprintf(stderr, "заметка не читается, в каталоге её не будет: [%s]\n",
                         info.absoluteFilePath().toUtf8().constData());
            continue;
        }
        notes_.insert(stem, std::move(note));
    }
}

bool ZStorage::refreshNote(const QString& id) {
    if (!store_ || id.isEmpty()) return false;
    const QString path = pathOf(id);
    if (!QFileInfo::exists(path)) {
        notes_.remove(id);
        return false;
    }
    NoteInfo fresh;
    if (!readInfo(path, fresh)) return false;
    notes_.insert(id, std::move(fresh));
    return true;
}

const ZStorage::NoteInfo* ZStorage::info(const QString& id) const {
    const auto it = notes_.constFind(id);
    return it == notes_.constEnd() ? nullptr : &it.value();
}

bool ZStorage::isFolder(const QString& id) const {
    const NoteInfo* meta = info(id);
    return meta != nullptr && meta->folder();
}

bool ZStorage::inArchive(const QString& id) const {
    QSet<QString> seen;
    for (const NoteInfo* meta = info(id); meta != nullptr && !seen.contains(meta->id());
         meta = info(meta->parent())) {
        if (meta->archived()) return true;
        seen.insert(meta->id());
    }
    return false;
}

QStringList ZStorage::childrenOf(const QString& id) const {
    QStringList out;
    for (auto it = notes_.constBegin(); it != notes_.constEnd(); ++it)
        if (it->parent() == id) out << it.key();
    return out;
}

QStringList ZStorage::descendantsOf(const QString& id) const {
    QStringList out;
    QSet<QString> seen{id};
    const std::function<void(const QString&)> walk = [&](const QString& at) {
        for (const QString& child : childrenOf(at)) {
            if (seen.contains(child)) continue;   // цикл в родителях — не зациклиться
            seen.insert(child);
            walk(child);
            out << child;
        }
    };
    walk(id);
    return out;
}

bool ZStorage::isEmptyNote(const QString& id) const {
    if (isFolder(id)) return childrenOf(id).isEmpty();
    QFile f(pathOf(id));
    if (!f.open(QIODevice::ReadOnly)) return false;
    const QByteArray bytes = f.readAll();
    ZDocument doc;
    doc.loadMarkdown(std::string_view(bytes.constData(), size_t(bytes.size())));
    return doc.isEmpty();
}

QString ZStorage::titleOf(const QString& id) const {
    const NoteInfo* meta = info(id);
    return meta == nullptr ? QString() : meta->title();
}

QStringList ZStorage::migrate() {
    QStringList notes;
    if (!store_) return notes;
    bool changed = false;
    QString why;
    const int moved = store::migrateTrashToArchive(root_, &why);
    if (moved < 0) notes << QStringLiteral("корзина не переехала в архив: %1").arg(why);
    else if (moved > 0) {
        notes << QStringLiteral("корзина переехала в архив: заметок %1").arg(moved);
        changed = true;
    }
    why.clear();
    const int filed = store::fileOrphans(root_, &why);
    if (filed < 0) notes << QStringLiteral("бюро находок не завелось: %1").arg(why);
    else if (filed > 0) {
        notes << QStringLiteral("в бюро находок прописано заметок: %1").arg(filed);
        changed = true;
    }
    if (changed) reload();
    return notes;
}

QString ZStorage::importNote(const QString& parentId, const QString& sourcePath, QString* error) {
    if (!store_) {
        if (error != nullptr) *error = QStringLiteral("это не хранилище");
        return {};
    }
    const QString made = store::importNote(root_, parentId, sourcePath, error);
    if (made.isEmpty()) return {};
    const QString id = idOfPath(made);
    refreshNote(id);
    return id;
}

QString ZStorage::createNote(const QString& parentId, bool folder, QString* error) {
    if (!store_) {
        if (error != nullptr) *error = QStringLiteral("это не хранилище");
        return {};
    }
    // В архиве ничего не создаётся: Ctrl+N оттуда — на глобальный уровень
    // (правило владельца).
    QString parent = parentId;
    if (!parent.isEmpty() && (!has(parent) || inArchive(parent))) parent.clear();
    const QString made = store::newNote(root_, parent, error);
    if (made.isEmpty()) return {};
    const QString id = idOfPath(made);
    if (folder) {
        QString why;
        if (!rewriteNote(id, [](ZDocument& doc) {
                doc.setHeaderValue(QStringLiteral("role"), QStringLiteral("folder"));
                doc.setTitle(QStringLiteral("Новая папка"));
            }, history::Rules{}, &why))
            std::fprintf(stderr, "новая папка без роли: %s\n", why.toUtf8().constData());
    }
    refreshNote(id);
    return id;
}

bool ZStorage::archive(const QString& id, const history::Rules& rules, QStringList* failed) {
    if (!store_ || !has(id)) return false;
    QStringList doomed{id};
    if (isFolder(id)) doomed += descendantsOf(id);
    bool ok = true;
    for (const QString& victim : doomed) {
        QString why;
        if (isFolder(victim)) {
            // У папки тела нет — только заголовок; журнал ей ни к чему, хватит
            // пометки.
            if (!rewriteNote(victim, [](ZDocument& doc) { doc.setArchived(true); }, rules, &why)) {
                ok = false;
                if (failed != nullptr) *failed << QStringLiteral("%1: %2").arg(titleOf(victim), why);
            }
            continue;
        }
        if (!store::archiveNote(root_, victim, rules, &why)) {
            ok = false;
            if (failed != nullptr) *failed << QStringLiteral("%1: %2").arg(titleOf(victim), why);
        }
        refreshNote(victim);
    }
    return ok;
}

bool ZStorage::restore(const QString& id, QStringList* failed) {
    if (!store_ || !has(id) || !inArchive(id)) return false;
    QStringList back{id};
    if (isFolder(id)) back += descendantsOf(id);
    bool ok = true;
    for (const QString& one : back) {
        QString why;
        if (isFolder(one)) {
            if (!rewriteNote(one, [](ZDocument& doc) {
                    doc.setArchived(false);
                    if (doc.headerValue(QStringLiteral("role")) == QLatin1String("trash"))
                        doc.setHeaderValue(QStringLiteral("role"), QString());
                }, history::Rules{}, &why)) {
                ok = false;
                if (failed != nullptr) *failed << QStringLiteral("%1: %2").arg(titleOf(one), why);
            }
            continue;
        }
        if (!store::restoreNote(root_, one, &why)) {
            ok = false;
            if (failed != nullptr) *failed << QStringLiteral("%1: %2").arg(titleOf(one), why);
        }
        refreshNote(one);
    }
    return ok;
}

bool ZStorage::remove(const QString& id, QString* error) {
    if (!store_ || !has(id)) {
        if (error != nullptr) *error = QStringLiteral("такой заметки нет");
        return false;
    }
    // КАРТИНКИ СЧИТАЕМ ДО УДАЛЕНИЯ: чтобы узнать, какие были в заметке, надо
    // прочитать её саму, а через мгновение файла не будет.
    const QStringList doomedFiles = store::attachmentsLeavingWith(root_, {id});
    // У АРХИВНОЙ ЗАМЕТКИ ЖУРНАЛ УХОДИТ ВМЕСТЕ С НЕЙ: тело архивной живёт в
    // журнале и больше нигде, файл — стаб в одну строку. Оставить журнал
    // значило бы не удалить заметку, а спрятать её. У прочих остаётся
    // надгробие («сначала надгробие, потом файл» — правило хранилища).
    QString why;
    const bool gone = inArchive(id) ? store::forgetNote(root_, id, &why)
                                    : store::deleteNoteFile(root_, id, &why);
    if (!gone) {
        if (error != nullptr) *error = why;
        return false;
    }
    if (!why.isEmpty()) std::fprintf(stderr, "%s\n", why.toUtf8().constData());
    // Картинки — следом, в ту же мусорку ОС.
    for (const QString& picture : doomedFiles) {
        QString pictureError;
        if (!store::deleteAttachmentFile(root_, picture, &pictureError))
            std::fprintf(stderr, "%s\n", pictureError.toUtf8().constData());
    }
    notes_.remove(id);
    return true;
}

bool ZStorage::rewriteNote(const QString& id, const std::function<void(ZDocument&)>& change,
                           const history::Rules& rules, QString* error) {
    const QString path = pathOf(id);
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error != nullptr) *error = QStringLiteral("файл не читается: %1").arg(path);
        return false;
    }
    const QByteArray bytes = f.readAll();
    f.close();
    ZDocument doc;
    doc.loadMarkdown(std::string_view(bytes.constData(), size_t(bytes.size())));
    change(doc);
    // ШТАТНЫЙ ПУТЬ ЗАПИСИ: самопроверка разбором обратно, атомарная запись,
    // отпечаток — те же правила, что у открытой заметки. Прежде здесь стоял
    // std::ofstream мимо всего этого (аудит refactor2, §1.4).
    const SaveOutcome outcome =
        doc.saveTo(path, rescueTimestamp(),
                   nullptr, doc.header(), hashOf(std::string_view(bytes.constData(), size_t(bytes.size()))));
    if (outcome.result != SaveResult::Written && outcome.result != SaveResult::Unchanged) {
        if (error != nullptr) *error = outcome.message;
        return false;
    }
    if (outcome.result == SaveResult::Written) {
        ZNoteHistory history = historyOf(id, rules);
        history.record(journal::Kind::Save, outcome.written);
    }
    refreshNote(id);
    return true;
}

}  // namespace zametti
