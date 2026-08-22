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


bool ZStorage::isStoreRoot(const QString& dir) {
    return QFileInfo(dir + QStringLiteral("/.zametti")).isDir();
}

ZStorage::ZStorage(const QString& root)
    : root_(QDir::cleanPath(root)), store_(isStoreRoot(QDir::cleanPath(root))) {
    settle_.setSingleShot(true);
    settle_.setInterval(400);
    connect(&settle_, &QTimer::timeout, this, [this] {
        // СВЕРКА СОСТАВА — 1.5 мс на корпусе владельца (276 файлов), а полное
        // перечитывание — 24 мс. Имена те же — молчим.
        const QSet<QString> now = listNames();
        if (now == names_) return;
        names_ = now;
        reload();
    });
}

class ZStorage::Batch {
public:
    explicit Batch(ZStorage& s) : s_(s) { ++s_.quiet_; }
    ~Batch() {
        if (--s_.quiet_ == 0 && s_.pending_) {
            s_.pending_ = false;
            emit s_.catalogChanged();
        }
    }
    Batch(const Batch&) = delete;
    Batch& operator=(const Batch&) = delete;

private:
    ZStorage& s_;
};

void ZStorage::announce(bool structural, const QString& id) {
    if (quiet_ > 0) {
        pending_ = true;
        return;
    }
    if (structural) emit catalogChanged();
    else emit noteChanged(id);
}

QSet<QString> ZStorage::listNames() const {
    QSet<QString> names;
    for (const QString& name : QDir(root_).entryList(QDir::Files | QDir::Hidden)) names.insert(name);
    return names;
}

void ZStorage::setWatching(bool on) {
    if (on == watching()) return;
    if (!on) {
        settle_.stop();
        watcher_.reset();
        return;
    }
    if (!store_) return;
    names_ = listNames();
    watcher_ = std::make_shared<QFileSystemWatcher>();
    watcher_->addPath(root_);
    connect(watcher_.get(), &QFileSystemWatcher::directoryChanged, this,
            [this](const QString&) { settle_.start(); });
}

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
    return journal::History::lockPathFor(store_ ? root_ : QDir::tempPath());
}

bool ZStorage::isLocked() const { return lock_ != nullptr && lock_->isLocked(); }

ZStorage::LockReport ZStorage::forceUnlock() {
    LockReport report;
    QLockFile probe(lockPath());
    if (probe.getLockInfo(&report.holderPid, &report.holderHost, nullptr))
        report.note = QStringLiteral("removing the store lock (held by pid %1 on '%2')")
                          .arg(report.holderPid)
                          .arg(report.holderHost);
    else
        report.note = QStringLiteral("the store had no lock");
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
        report.note = QStringLiteral("removing a stale store lock (pid %1 not alive)").arg(pid);
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

void ZStorage::reload() {
    notes_.clear();
    if (!store_) return;
    // Состав каталога сторож сверяет с тем, что мы читали последними: reload по
    // любой двери — и его точка отсчёта тоже.
    if (watcher_ != nullptr) names_ = listNames();
    // Скан: только "<id>.md".
    for (const QFileInfo& info :
         QDir(root_).entryInfoList({QStringLiteral("*.md")}, QDir::Files)) {
        const QString stem = info.completeBaseName();
        if (!isValidNoteId(stem.toStdString())) continue;
        NoteInfo note = ZNote::Metadata::fromFile(info.absoluteFilePath());
        if (!note.valid()) {
            // Заметка есть на диске, но не читается — молчать нельзя: человек
            // видел бы пустое место в дереве и не узнал бы, что файл на месте.
            std::fprintf(stderr, "cannot read note, it will not be in the catalog: [%s]\n",
                         info.absoluteFilePath().toUtf8().constData());
            continue;
        }
        notes_.insert(stem, std::move(note));
    }
    announce(true, QString());
}

bool ZStorage::readBack(const QString& id, bool* structural) {
    if (structural != nullptr) *structural = false;
    if (!store_ || id.isEmpty()) return false;
    const QString path = pathOf(id);
    const auto old = notes_.constFind(id);
    if (!QFileInfo::exists(path)) {
        if (old != notes_.constEnd()) {
            notes_.erase(old);
            if (structural != nullptr) *structural = true;
        }
        return false;
    }
    NoteInfo fresh = ZNote::Metadata::fromFile(path);
    if (!fresh.valid()) return false;
    // Место заметки в дереве: появилась, сменила родителя, род, архивность,
    // метку порядка — дерево строится заново; иначе меняется одна строка.
    if (structural != nullptr) {
        *structural = old == notes_.constEnd() || old->parent() != fresh.parent() ||
                      old->folder() != fresh.folder() || old->archived() != fresh.archived() ||
                      old->lostFound() != fresh.lostFound() || old->sortMark() != fresh.sortMark();
    }
    notes_.insert(id, std::move(fresh));
    return true;
}

bool ZStorage::refreshNote(const QString& id) {
    bool structural = false;
    const bool ok = readBack(id, &structural);
    if (ok || structural) announce(structural, id);
    return ok;
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
    ZNote note;
    note.load(std::string_view(bytes.constData(), size_t(bytes.size())));
    return note.doc().isEmpty();
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
    if (moved < 0) notes << QStringLiteral("old trash did not migrate to the archive: %1").arg(why);
    else if (moved > 0) {
        notes << QStringLiteral("old trash migrated to the archive: %1 notes").arg(moved);
        changed = true;
    }
    why.clear();
    const int filed = store::fileOrphans(root_, &why);
    if (filed < 0) notes << QStringLiteral("lost & found not set up: %1").arg(why);
    else if (filed > 0) {
        notes << QStringLiteral("notes filed into lost & found: %1").arg(filed);
        changed = true;
    }
    if (changed) reload();
    return notes;
}

QString ZStorage::importNote(const QString& parentId, const QString& sourcePath, QString* error) {
    if (!store_) {
        if (error != nullptr) *error = QStringLiteral("not a store");
        return {};
    }
    const QString made = store::importNote(root_, parentId, sourcePath, error);
    if (made.isEmpty()) return {};
    const QString id = idOfPath(made);
    refreshNote(id);   // новая — структурная новость сама по себе
    return id;
}

QString ZStorage::createNote(const QString& parentId, bool folder, QString* error) {
    if (!store_) {
        if (error != nullptr) *error = QStringLiteral("not a store");
        return {};
    }
    // В архиве ничего не создаётся: Ctrl+N оттуда — на глобальный уровень
    // (правило владельца).
    QString parent = parentId;
    if (!parent.isEmpty() && (!has(parent) || inArchive(parent))) parent.clear();
    const QString made = store::newNote(root_, parent, error);
    if (made.isEmpty()) return {};
    const QString id = idOfPath(made);
    const Batch batch(*this);   // папка — две записи, новость одна
    if (folder) {
        QString why;
        if (!rewriteNote(id, [](ZNote& note) {
                note.setRole(QStringLiteral("folder"));
                note.doc().setTitle(QStringLiteral("New folder"));
            }, history::Rules{}, &why))
            std::fprintf(stderr, "new folder has no role: %s\n", why.toUtf8().constData());
    }
    refreshNote(id);
    return id;
}

bool ZStorage::archive(const QString& id, const history::Rules& rules, QStringList* failed) {
    if (!store_ || !has(id)) return false;
    const Batch batch(*this);
    QStringList doomed{id};
    if (isFolder(id)) doomed += descendantsOf(id);
    bool ok = true;
    for (const QString& victim : doomed) {
        QString why;
        if (isFolder(victim)) {
            // У папки тела нет — только заголовок; журнал ей ни к чему, хватит
            // пометки.
            if (!rewriteNote(victim, [](ZNote& note) { note.setArchived(true); }, rules, &why)) {
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
    const Batch batch(*this);
    QStringList back{id};
    if (isFolder(id)) back += descendantsOf(id);
    bool ok = true;
    for (const QString& one : back) {
        QString why;
        if (isFolder(one)) {
            if (!rewriteNote(one, [](ZNote& note) {
                    note.setArchived(false);
                    if (note.role() == QLatin1String("trash")) note.setRole(QString());
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
        if (error != nullptr) *error = QStringLiteral("no such note");
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
    announce(true, id);
    return true;
}

bool ZStorage::rename(const QString& id, const QString& title, const history::Rules& rules,
                      QString* error) {
    return rewriteNote(id, [&title](ZNote& note) { note.doc().setTitle(title); }, rules, error);
}

bool ZStorage::move(const QString& id, const QString& parentId, const history::Rules& rules,
                    QString* error) {
    return rewriteNote(id, [&parentId](ZNote& note) {
        note.setHasHeader(true);
        note.setParentId(parentId);
    }, rules, error);
}

bool ZStorage::setSortMark(const QString& id, std::optional<SortOrder> order,
                           const history::Rules& rules, QString* error) {
    return rewriteNote(id, [order](ZNote& note) {
        note.setHasHeader(true);
        note.setSortMark(order);
    }, rules, error);
}

bool ZStorage::rewriteNote(const QString& id, const std::function<void(ZNote&)>& change,
                           const history::Rules& rules, QString* error) {
    const QString path = pathOf(id);
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error != nullptr) *error = QStringLiteral("cannot read file: %1").arg(path);
        return false;
    }
    const QByteArray bytes = f.readAll();
    f.close();
    // Заметка поднимается с диска на время операции — та же ZNote, что и у
    // редактора, только без вида: шапка её, тело её, запись её.
    ZNote note(path, bytes, hashOf(std::string_view(bytes.constData(), size_t(bytes.size()))),
               historyOf(id, rules));
    note.load(std::string_view(bytes.constData(), size_t(bytes.size())));
    change(note);
    // ШТАТНЫЙ ПУТЬ ЗАПИСИ: самопроверка разбором обратно, атомарная запись,
    // отпечаток — те же правила, что у открытой заметки. Прежде здесь стоял
    // std::ofstream мимо всего этого (аудит refactor2, §1.4).
    const SaveOutcome outcome = note.save(path, rescueTimestamp(), note.digest());
    if (outcome.result != SaveResult::Written && outcome.result != SaveResult::Unchanged) {
        if (error != nullptr) *error = outcome.message;
        return false;
    }
    if (outcome.result == SaveResult::Written) note.history().record(journal::Kind::Save, outcome.written);
    refreshNote(id);
    return true;
}

}  // namespace zametti
