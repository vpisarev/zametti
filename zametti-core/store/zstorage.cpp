#include "zstorage.h"

#include "note_id.h"
#include "times.h"
#include "journal.h"

#include <cassert>

#include <QDateTime>
#include <QMutex>
#include <QMutexLocker>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
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

ZStorage::DirKind ZStorage::inspect(const QString& dir) {
    const QString clean = QDir::cleanPath(dir);
    if (clean.isEmpty() || !QFileInfo(clean).isDir()) return DirKind::Missing;
    // Хранилище — раньше пустоты: у него внутри есть .zametti, то есть пустым
    // оно не бывает, и порядок проверок тут не вопрос вкуса.
    if (isStoreRoot(clean)) return DirKind::Store;
    // NoDotAndDotDot обязателен: без него «.» и «..» есть в любом каталоге, и
    // пустых каталогов на свете не бывает вовсе.
    const QDir at(clean);
    return at.isEmpty(QDir::AllEntries | QDir::Hidden | QDir::System |
                      QDir::NoDotAndDotDot)
               ? DirKind::Empty
               : DirKind::Foreign;
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

// ПОРЯДОК ОСВОБОЖДЕНИЯ, НАПИСАННЫЙ СЛОВАМИ. Зачем деструктор нужен вообще, при
// том что все члены и так RAII, — сказано в заголовке; здесь сам порядок.
//
// Умолчание разрушило бы члены обратно объявлению, и одна конкретная щель у
// нас там уже есть: объявлены `watcher_`, а следом `settle_`, значит таймер
// умирает ПЕРВЫМ, а сторож каталога, чья лямбда зовёт `settle_.start()`, —
// вторым. Между этими двумя мгновениями пришедшее от файловой системы событие
// дёрнуло бы уже разрушенный таймер.
ZStorage::~ZStorage() {
    // 1. СТОРОЖ И ЕГО ТАЙМЕР — ВМЕСТЕ И ПЕРВЫМИ. После этого в полумёртвый
    //    объект уже ничего не прилетит, и остальное можно разбирать спокойно.
    setWatching(false);
    // 2. ОБЛАКО НЕ ПЕРЕЖИВАЕТ ХРАНИЛИЩЕ: адаптер держит сетевые запросы, шифр
    //    — ключ. Ни тому, ни другому незачем оставаться, когда хранилища уже
    //    нет.
    remote_.reset();
    cipher_.reset();
    // 3. Каталог забыт.
    notes_.clear();
    loaded_ = false;
    // 4. ЗАМОК — ПОСЛЕДНИМ. Пока он наш, в каталог не войдёт вторая копия
    //    программы; отпускаем его, когда всё остальное уже отпущено, а не
    //    посреди разбора.
    lock_.reset();
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
    return QDir(store_ ? root_ : QDir::tempPath()).filePath(QStringLiteral(".zametti/store.lock"));
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

    // РЕЗУЛЬТАТ УДАЛЕНИЯ ЧИТАЕМ, А НЕ ПРЕДПОЛАГАЕМ. Раньше здесь стоял голый
    // QFile::remove(), и отчёт говорил «сняли» независимо от того, сняли ли.
    // Под Windows это неправда регулярно: Qt открывает файл замка БЕЗ
    // FILE_SHARE_DELETE нарочно (qlockfile_win.cpp), поэтому замок, который
    // держит ЖИВОЙ процесс, удалить нельзя вовсе — а именно живой замок и
    // пытаются снять руками. Молчать об этом значит отправить человека искать,
    // почему «снятый» замок на месте.
    //
    // Замок мёртвого процесса снимается везде: его описатель закрыла система.
    if (QFile::exists(lockPath()) && !QFile::remove(lockPath())) {
        report.note = QStringLiteral(
            "the store lock could not be removed (held by a live process pid %1 on '%2'?)")
                          .arg(report.holderPid)
                          .arg(report.holderHost);
        return report;
    }
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
    report.busy = lock_->error() == QLockFile::LockFailedError;
    return report;
}

QString ZStorage::pathOf(const QString& id) const {
    return root_ + QLatin1Char('/') + id + QStringLiteral(".md");
}

QString ZStorage::idOfPath(const QString& path) { return QFileInfo(path).completeBaseName(); }

ZStorage::Target ZStorage::locate(const QString& what, const QString& rootHint) {
    Target target;
    target.root = rootHint;
    target.id = what;
    // Путь узнаём по расширению, а не по существованию файла: заметку могли
    // уже удалить, а журнал её пережил — именно с ним и работают люки.
    if (what.endsWith(QStringLiteral(".md"), Qt::CaseInsensitive)) {
        const QFileInfo info(what);
        target.id = idOfPath(what);
        if (target.root.isEmpty()) target.root = info.absolutePath();
    }
    return target;
}

std::shared_ptr<ZJournal> ZStorage::journalFor(const QString& id,
                                                        const ZJournal::Rules& rules) {
    // Журналы лежат под корнем (history/); каталогу .zametti для этого быть не
    // обязательно — так живут наборы на временном каталоге.
    if (root_.isEmpty() || id.isEmpty()) return std::make_shared<ZJournal>();
    return std::make_shared<ZJournal>(this, id, rules);
}

void ZStorage::reload() {
    notes_.clear();
    loaded_ = true;
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

void ZStorage::NoteTree::insert(const ZStorage::NoteInfo& info) {
    if (!info.valid()) return;
    nodes_.insert(info.id(), info);
    if (info.root()) rootId_ = info.id();
}

QString ZStorage::NoteTree::qualifiedName(const QString& id) const {
    const auto it = nodes_.constFind(id);
    if (it == nodes_.constEnd()) return id;
    QStringList parts{it->title()};
    QSet<QString> seen{id};
    QString up = it->parent();
    bool orphan = false;
    while (!up.isEmpty()) {
        // Цикл родителей — та же беда, что обрыв: честный «?/», не зависание.
        if (seen.contains(up)) {
            orphan = true;
            break;
        }
        seen.insert(up);
        const auto parent = nodes_.constFind(up);
        if (parent == nodes_.constEnd()) {
            orphan = true;
            break;
        }
        parts.prepend(parent->title());
        up = parent->parent();
    }
    if (orphan)
        parts.prepend(QStringLiteral("?"));
    else if (!rootId_.isEmpty() && id != rootId_)
        // Пустой parent — верх дерева; первой компонентой идёт имя
        // хранилища (заголовок корневой заметки), как в дереве окна.
        parts.prepend(nodes_.value(rootId_).title());
    return parts.join(QLatin1Char('/'));
}

ZStorage::NoteTree ZStorage::subtreeFor(const QStringList& ids) const {
    NoteTree out;
    if (!store_) return out;
    if (!loaded_) const_cast<ZStorage*>(this)->reload();
    const QString root = rootId();
    if (const NoteInfo* rootInfo = info(root)) out.insert(*rootInfo);
    for (const QString& id : ids) {
        QString at = id;
        // Вверх до корня; чужое и зацикленное qualifiedName разберёт сам —
        // здесь только сбор карты, и он обязан кончаться.
        for (int depth = 0; !at.isEmpty() && depth < 512; ++depth) {
            if (out.contains(at)) break;
            const NoteInfo* one = info(at);
            if (one == nullptr) break;
            out.insert(*one);
            at = one->parent();
        }
    }
    return out;
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

bool ZStorage::isReadOnly(const QString& id) const {
    // Сама виртуальная папка и её заметки отвечают по её пометке: файла за
    // ними нет вовсе, и писать некуда независимо от ответа.
    if (const VirtualFolder* folder = virtualFolder(id); folder != nullptr)
        return folder->readOnly;
    if (isVirtualId(id)) {
        const VirtualFolder* folder = virtualFolder(id.section(QLatin1Char(':'), 0, 0));
        return folder == nullptr || folder->readOnly;
    }
    // Подъём тот же, что у inArchive, и с той же защитой от цикла: помеченная
    // папка запирает всё, что под ней.
    QSet<QString> seen;
    for (const NoteInfo* meta = info(id); meta != nullptr && !seen.contains(meta->id());
         meta = info(meta->parent())) {
        if (meta->readOnly()) return true;
        seen.insert(meta->id());
    }
    return false;
}

// --- ВИРТУАЛЬНЫЕ ПАПКИ ------------------------------------------------------

void ZStorage::addVirtualFolder(VirtualFolder folder) {
    for (VirtualNote& note : folder.notes) {
        // Признак виртуального — двоеточие в id. Оно не украшение: по нему
        // отличают виртуальную заметку от настоящей ВЕЗДЕ, и id без него молча
        // выглядел бы настоящим.
        Q_ASSERT(isVirtualId(note.id) && "a virtual note id needs a folder prefix");
        if (!isVirtualId(note.id)) {
            std::fprintf(stderr, "virtual note has no folder prefix: %s\n",
                         note.id.toUtf8().constData());
            note.id = folder.id + QLatin1Char(':') + note.id;
        }
        if (!note.title.isEmpty()) continue;
        // Заголовок — первая строка самого документа, как у настоящих заметок.
        // Спрашиваем один раз, на открытии хранилища.
        std::string bytes;
        if (!readFileBytes(note.path, bytes)) {
            std::fprintf(stderr, "virtual note is unreadable: %s\n",
                         note.path.toUtf8().constData());
            note.title = QFileInfo(note.path).completeBaseName();
            continue;
        }
        ZNote doc;
        doc.load(std::string_view(bytes));
        note.title = doc.title();
    }
    virtual_.push_back(std::move(folder));
    emit catalogChanged();
}

const ZStorage::VirtualFolder* ZStorage::virtualFolder(const QString& id) const {
    for (const VirtualFolder& folder : virtual_)
        if (folder.id == id) return &folder;
    return nullptr;
}

const ZStorage::VirtualNote* ZStorage::virtualNote(const QString& id) const {
    for (const VirtualFolder& folder : virtual_)
        for (const VirtualNote& note : folder.notes)
            if (note.id == id) return &note;
    return nullptr;
}

const ZStorage::VirtualNote* ZStorage::virtualNoteAtPath(const QString& path) const {
    if (path.isEmpty()) return nullptr;
    for (const VirtualFolder& folder : virtual_)
        for (const VirtualNote& note : folder.notes)
            if (note.path == path) return &note;
    return nullptr;
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
    const int moved = migrateTrashToArchive(&why);
    if (moved < 0) notes << QStringLiteral("old trash did not migrate to the archive: %1").arg(why);
    else if (moved > 0) {
        notes << QStringLiteral("old trash migrated to the archive: %1 notes").arg(moved);
        changed = true;
    }
    // РАЗВЕРНУТЬ АРХИВНЫЕ СТАБЫ прежних сборок: тело из журнала — обратно в
    // файл. Пока они стабы, у них нет ни ссылок на вложения (по ним считается,
    // чему уходить при удалении насовсем), ни текста для поиска.
    why.clear();
    QStringList leftAlone;
    const int unfolded = unfoldArchivedStubs(&leftAlone, &why);
    if (unfolded < 0) notes << QStringLiteral("archived stubs did not unfold: %1").arg(why);
    else if (unfolded > 0) {
        notes << QStringLiteral("archived stubs unfolded: %1 notes").arg(unfolded);
        changed = true;
    }
    notes << leftAlone;

    why.clear();
    const int filed = fileOrphans(&why);
    if (filed < 0) notes << QStringLiteral("lost & found not set up: %1").arg(why);
    else if (filed > 0) {
        notes << QStringLiteral("notes filed into lost & found: %1").arg(filed);
        changed = true;
    }
    if (changed) reload();
    return notes;
}

QString ZStorage::createNote(const QString& parentId, bool folder, QString* error) {
    if (!store_) {
        if (error != nullptr) *error = QStringLiteral("not a store");
        return {};
    }
    // Родитель проверяется по каталогу — значит, каталог обязан быть прочитан,
    // иначе всякий родитель выглядел бы отсутствующим и заметка молча ложилась
    // бы в корень (см. rootId про ту же ловушку).
    if (!loaded_) reload();
    // В архиве ничего не создаётся: Ctrl+N оттуда — на глобальный уровень
    // (правило владельца).
    QString parent = parentId;
    // Родитель, которого нет, который в архиве ИЛИ ЗАПЕРТ на запись, — не
    // родитель: в архиве ничего не создаётся, в read-only тоже.
    if (!parent.isEmpty() && (!has(parent) || inArchive(parent) || isReadOnly(parent)))
        parent.clear();
    const QString made = newNoteFile(parent, error);
    if (made.isEmpty()) return {};
    const QString id = idOfPath(made);
    const Batch batch(*this);   // папка — две записи, новость одна
    if (folder) {
        QString why;
        if (!rewriteNote(id, [](ZNote& note) {
                note.setRole(QStringLiteral("folder"));
                note.doc().setTitle(QStringLiteral("New folder"));
            }, ZJournal::Rules{}, &why))
            std::fprintf(stderr, "new folder has no role: %s\n", why.toUtf8().constData());
    }
    refreshNote(id);
    return id;
}

bool ZStorage::archive(const QString& id, const ZJournal::Rules& rules, QStringList* failed) {
    if (!store_ || !has(id)) return false;
    // READ-ONLY НЕ УБИРАЕТСЯ В АРХИВ: архивация переписывает файл (тело уходит
    // в журнал, на месте остаётся стаб) — это запись, а запись запрещена.
    if (isReadOnly(id)) {
        std::fprintf(stderr, "read-only note is not archived: %s\n", id.toUtf8().constData());
        if (failed != nullptr)
            failed->append(QStringLiteral("%1: the note is read-only").arg(titleOf(id)));
        return false;
    }
    // КОРЕНЬ НЕ АРХИВИРУЕТСЯ: он папка, а значит descendantsOf унесло бы в
    // архив всё хранилище разом.
    if (isRootNote(id)) {
        if (failed != nullptr)
            failed->append(QStringLiteral("%1: the root note cannot be archived").arg(titleOf(id)));
        return false;
    }
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
        if (!archiveOne(victim, rules, &why)) {
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
                }, ZJournal::Rules{}, &why)) {
                ok = false;
                if (failed != nullptr) *failed << QStringLiteral("%1: %2").arg(titleOf(one), why);
            }
            continue;
        }
        if (!restoreOne(one, &why)) {
            ok = false;
            if (failed != nullptr) *failed << QStringLiteral("%1: %2").arg(titleOf(one), why);
        }
        refreshNote(one);
    }
    return ok;
}

bool ZStorage::remove(const QString& id, const ImportLimits& limits, QString* error) {
    if (!store_ || !has(id)) {
        if (error != nullptr) *error = QStringLiteral("no such note");
        return false;
    }
    // READ-ONLY НЕ УДАЛЯЕТСЯ: замок на записи, который переживает удаление, —
    // это не замок. Снимают пометку, потом удаляют.
    if (isReadOnly(id)) {
        std::fprintf(stderr, "read-only note is not deleted: %s\n", id.toUtf8().constData());
        if (error != nullptr) *error = QStringLiteral("the note is read-only");
        return false;
    }
    // КОРЕНЬ НЕ УДАЛЯЕТСЯ. Без него у хранилища нет ни имени, ни порядка «всех
    // заметок», а поддеревом он унёс бы вообще всё.
    if (isRootNote(id)) {
        if (error != nullptr) *error = QStringLiteral("the root note cannot be deleted");
        return false;
    }
    // ПАПКА УНОСИТ ПОДДЕРЕВО. Прежде уносило только её файл, а дети оставались с
    // оборванным parent — и при следующем открытии уезжали в бюро находок.
    // Дети идут первыми: если что-то не заладится, осиротевших не остаётся.
    QStringList doomed;
    if (isFolder(id)) doomed = descendantsOf(id);
    doomed << id;

    // КАРТИНКИ СЧИТАЕМ ДО УДАЛЕНИЯ и СРАЗУ НА ВЕСЬ ПАКЕТ: чтобы узнать, какие
    // были в заметках, надо прочитать их самих, а через мгновение файлов не
    // будет. Пакетом — потому что картинка, поделённая двумя удаляемыми
    // заметками, при поштучном счёте не ушла бы никогда: каждая «держалась» бы
    // другой.
    const QStringList doomedFiles = attachmentsLeavingWith(doomed);

    // ПУТЬ УДАЛЕНИЯ ОДИН для архивных и живых: надгробие в журнал, файл в
    // мусорку ОС. Прежде у архивной журнал уносился вместе с ней — тело жило
    // только там, и оставить его значило бы спрятать заметку, а не удалить. С
    // архивом-пометкой тело лежит в файле, и журнал обязан пережить удаление:
    // он единственный носитель самого факта, и без него первый же синк привёз
    // бы заметку обратно с другого устройства.
    bool ok = true;
    QString why;
    for (const QString& victim : std::as_const(doomed)) {
        QString one;
        if (!deleteNoteFile(victim, &one)) {
            ok = false;
            if (why.isEmpty()) why = one;
            continue;
        }
        if (!one.isEmpty()) std::fprintf(stderr, "%s\n", one.toUtf8().constData());
    }
    if (!ok) {
        if (error != nullptr) *error = why;
        return false;
    }

    // КАРТИНКИ ХОРОНЯТСЯ, А НЕ СТИРАЮТСЯ: на месте файла остаётся посмертная
    // мини-версия с меткой «удалено». «Файла нет» не доезжает до других
    // устройств — чтобы сказать «его больше нет», нужен файл, который это
    // говорит.
    for (const QString& picture : doomedFiles) {
        QString pictureError;
        if (!retireAttachment(picture, limits, &pictureError) ||
            !pictureError.isEmpty())
            std::fprintf(stderr, "%s\n", pictureError.toUtf8().constData());
    }
    for (const QString& victim : std::as_const(doomed)) notes_.remove(victim);
    announce(true, id);
    return true;
}

bool ZStorage::rename(const QString& id, const QString& title, const ZJournal::Rules& rules,
                      QString* error) {
    return rewriteNote(id, [&title](ZNote& note) { note.doc().setTitle(title); }, rules, error);
}

bool ZStorage::move(const QString& id, const QString& parentId, const ZJournal::Rules& rules,
                    QString* error) {
    // КОРЕНЬ НЕ ПЕРЕНОСИТСЯ: он и есть верх дерева.
    if (isRootNote(id)) {
        if (error != nullptr) *error = QStringLiteral("the root note cannot be moved");
        return false;
    }
    return rewriteNote(id, [&parentId](ZNote& note) {
        note.setHasHeader(true);
        note.setParentId(parentId);
    }, rules, error);
}

bool ZStorage::setReadOnly(const QString& id, bool readOnly, const ZJournal::Rules& rules,
                           QString* error) {
    // ЕДИНСТВЕННАЯ ПРАВКА, КОТОРУЮ ЗАПЕРТАЯ ЗАМЕТКА ПЕРЕЖИВАЕТ, — снятие
    // самого замка. Без неё запереть можно было бы только в одну сторону:
    // отпереть значит записать заметку, которая запрещает себя записывать.
    return rewriteNoteAllowed(id, [readOnly](ZNote& note) {
        note.setHasHeader(true);
        note.setReadOnly(readOnly);
    }, rules, error);
}

bool ZStorage::setSortMark(const QString& id, std::optional<SortOrder> order,
                           const ZJournal::Rules& rules, QString* error) {
    return rewriteNote(id, [order](ZNote& note) {
        note.setHasHeader(true);
        note.setSortMark(order);
    }, rules, error);
}

bool ZStorage::rewriteNote(const QString& id, const std::function<void(ZNote&)>& change,
                           const ZJournal::Rules& rules, QString* error) {
    // READ-ONLY НЕ ПЕРЕЗАПИСЫВАЕТСЯ. Это единственная дверь к шапке и телу
    // закрытой заметки, поэтому заслон стоит здесь, а не в каждом глаголе:
    // rename, move, setSortMark и пометка архива приходят все сюда.
    //
    // Молчать нельзя: в релизной сборке отказ виден только по строке в логе.
    if (isReadOnly(id)) {
        std::fprintf(stderr, "read-only note is not rewritten: %s\n", id.toUtf8().constData());
        if (error != nullptr) *error = QStringLiteral("the note is read-only");
        return false;
    }
    return rewriteNoteAllowed(id, change, rules, error);
}

bool ZStorage::rewriteNoteAllowed(const QString& id, const std::function<void(ZNote&)>& change,
                                  const ZJournal::Rules& rules, QString* error) {
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
               journalFor(id, rules));
    note.load(std::string_view(bytes.constData(), size_t(bytes.size())));
    const bool wasRoot = note.isRoot();
    change(note);
    // КОРЕНЬ НЕ РАЗЖАЛУЕТСЯ. Это единственная дверь к шапке закрытой заметки,
    // и стеречь роль надо здесь, а не в каждом вызывающем: правка, снявшая бы
    // role: root, отвергается целиком, а не «частично применяется».
    if (wasRoot && !note.isRoot()) {
        if (error != nullptr)
            *error = QStringLiteral("the root note cannot lose its role");
        return false;
    }
    // ШТАТНЫЙ ПУТЬ ЗАПИСИ: самопроверка разбором обратно, атомарная запись,
    // отпечаток — те же правила, что у открытой заметки. Прежде здесь стоял
    // std::ofstream мимо всего этого (аудит refactor2, §1.4).
    const SaveOutcome outcome = note.save(path, rescueTimestamp(), note.digest());
    if (outcome.result != SaveResult::Written && outcome.result != SaveResult::Unchanged) {
        if (error != nullptr) *error = outcome.message;
        return false;
    }
    if (outcome.result == SaveResult::Written) note.journal().record(ZJournal::Kind::Save, outcome.written);
    refreshNote(id);
    return true;
}

// --- ЖУРНАЛЫ: файлы, замок, обход history/ ---------------------------------

namespace {

// Подменить журнал целиком и атомарно. Промежуточного состояния у файла не
// существует ни мгновения.
bool replaceFile(const QString& path, const QByteArray& bytes, QString* error) {
    QSaveFile save(path);
    if (!save.open(QIODevice::WriteOnly)) {
        if (error) *error = QStringLiteral("cannot rewrite journal: %1").arg(save.errorString());
        return false;
    }
    save.write(bytes);
    if (!save.commit()) {
        if (error) *error = QStringLiteral("cannot rewrite journal: %1").arg(save.errorString());
        return false;
    }
    return true;
}

QString describeKind(ZJournal::Kind kind) {
    switch (kind) {
        case ZJournal::Kind::Save: return QStringLiteral("save");
        case ZJournal::Kind::External: return QStringLiteral("external");
        case ZJournal::Kind::Restore: return QStringLiteral("restore");
        case ZJournal::Kind::Tombstone: return QStringLiteral("tombstone");
        case ZJournal::Kind::Amendment: return QStringLiteral("amendment");
    }
    return QStringLiteral("?");
}

// Тот самый единый замок (см. zstorage.h, «Единая точка синхронизации»). Один
// на все журналы и на все экземпляры хранилища: история у хранилища одна.
QMutex& gate() {
    static QMutex mutex;
    return mutex;
}

// Обещание «закрытые методы зовутся только из-под замка» — не слова, а
// проверка. tryLock на уже взятом мьютексе не проходит; прошёл — значит замка
// не было, и это ошибка вызова, а не случайность. В релизе не стоит ничего.
void assertLocked() {
#ifndef NDEBUG
    if (gate().tryLock()) {
        gate().unlock();
        assert(false && "history operation called without the lock");
    }
#endif
}

}  // namespace


QString ZStorage::journalPath(const QString& noteId) const {
    return QDir(root_).filePath(QStringLiteral("history/%1.log").arg(noteId));
}


bool ZStorage::dropVoidedLocked(const QString& path, const ZJournal& jrn,
                               const QVector<int>& voided, QString* error) {
    assertLocked();
    if (voided.isEmpty()) return true;

    // ХВОСТОМ — обычный случай: слияние мелкой правки гасит последнюю запись,
    // схлопывание возврата — несколько последних. Тогда файл просто
    // укорачивается по началу первой лишней записи, и всё, что до неё,
    // читатель видит ровно как раньше.
    bool suffix = voided.last() == jrn.size() - 1;
    for (int i = 1; i < voided.size() && suffix; ++i)
        if (voided[i] != voided[i - 1] + 1) suffix = false;
    if (suffix) {
        const qint64 cut = jrn.at(voided.first()).offset();
        if (cut <= 0) {
            if (error) *error = QStringLiteral("cannot tell where the voided tail begins");
            return false;
        }
        QFile file(path);
        if (!file.resize(cut)) {
            if (error) *error = QStringLiteral("cannot cut the journal: %1").arg(file.errorString());
            return false;
        }
        return true;
    }

    // СЕРЕДИНОЙ — редкий случай: гасится запись, приехавшая со стороны.
    // Вырезать её на месте нельзя (звенья поколения считаются от предыдущего
    // слепка), поэтому файл пересобирается целиком и атомарно.
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("cannot read journal: %1").arg(file.errorString());
        return false;
    }
    const QByteArray blob = file.readAll();
    file.close();
    ZJournal full;
    if (!full.parse(blob, ZJournal::Want::All, -1, error)) return false;
    QVector<int> keep;
    for (int i = 0; i < full.size(); ++i)
        if (!voided.contains(i)) keep.append(i);
    QByteArray out;
    if (!full.toBytes(keep, full.cleanVersion(), &out, error)) return false;
    return replaceFile(path, out, error);
}

bool ZStorage::appendJournalLocked(const QString& path, const ZJournal::NewRecord& what, QString* error) {
    assertLocked();

    // ПОМЕТКА В DIRTY-SET — здесь, а не только в writeFileBytes: журналируется
    // каждая запись (редактор, rewriteNote, архив), и это второе горло, через
    // которое проходит всё тронутое. Сама пометка под своей калиткой, не под
    // gate(), — порядок взятия всегда один (gate → dirtyGate), тупика нет.
    markDirty(QFileInfo(path).completeBaseName());

    // Что уже лежит в журнале. Читается только последнее поколение — память и
    // время ограничены им, а не длиной журнала.
    const auto reread = [&](ZJournal* journal, bool* exists) {
        QFile file(path);
        *exists = file.exists() && file.size() > 0;
        if (!*exists) return true;
        if (!file.open(QIODevice::ReadOnly)) {
            if (error) *error = QStringLiteral("cannot read journal: %1").arg(file.errorString());
            return false;
        }
        const QByteArray blob = file.readAll();
        file.close();
        return journal->parse(blob, ZJournal::Want::Chain, std::numeric_limits<int>::max(), error);
    };

    ZJournal jrn;
    bool exists = false;
    if (!reread(&jrn, &exists)) return false;

    // Оборванный хвост отрезаем прежде дозаписи, а не после: иначе мусор
    // остался бы посреди файла и увёл бы за собой всё поколение.
    if (jrn.tailTrimmed()) {
        if (!trimJournalTailLocked(path, error)) return false;
    }

    // ГАШЕНИЕ — прежде рождения новой записи, и порядок здесь существенный:
    // новая запись сжимается относительно предшественника ПО ФАЙЛУ, и если
    // выкинуть погашенные потом, звено цепочки осталось бы без своей базы.
    if (!what.voids().isEmpty() && exists) {
        const QVector<int> voided = jrn.indexesOf(what.voids());
        if (!voided.isEmpty()) {
            if (!dropVoidedLocked(path, jrn, voided, error)) return false;
            jrn = ZJournal{};
            if (!reread(&jrn, &exists)) return false;
        }
    }

    // Рождение записи — дело журнала: ревизию, отпечаток, кодек и сжатие
    // относительно предшественника выбирает он. Здесь только файл.
    QByteArray tail;
    if (!exists) {
        QDir().mkpath(QFileInfo(path).absolutePath());
        // Новый журнал заводится сразу чищеным: он весь написан нынешними
        // правилами, и вычищать в нём нечего по построению. Иначе первая же
        // заметка приезжала бы на чистку зря.
        tail = ZJournal::headerBytes(QString::fromLatin1(ZJournal::kCleanVersion));
    }
    QByteArray record;
    ZJournal::Record made;
    if (!jrn.composeRecord(what, deviceClockFloor(), &made, &record, error)) return false;
    tail += record;

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append)) {
        if (error) *error = QStringLiteral("cannot open journal: %1").arg(file.errorString());
        return false;
    }
    const qint64 wrote = file.write(tail);
    // Закрываем без fsync — см. шапку журнала. Незаписавшийся хвост (диск
    // кончился) читатель отрежет сам, но сказать об этом надо сразу.
    file.close();
    if (wrote != tail.size()) {
        if (error) *error = QStringLiteral("journal written incompletely: %1 of %2 bytes")
                                .arg(wrote)
                                .arg(tail.size());
        return false;
    }
    // Пол устройства поднимаем ПОСЛЕ удачной записи: число обещает «столько уже
    // записано», и обещать это заранее нельзя.
    advanceDeviceClock(made.time());
    return true;
}

bool ZStorage::removeJournalLocked(const QString& path, QString* error) {
    assertLocked();
    if (!QFile::exists(path)) return true;
    if (QFile::moveToTrash(path) || QFile::remove(path)) return true;
    if (error) *error = QStringLiteral("cannot delete journal %1").arg(path);
    return false;
}

bool ZStorage::readJournalLocked(const QString& path, ZJournal* out, QString* error) const {
    assertLocked();
    QFile file(path);
    if (!file.exists()) {
        *out = ZJournal{};
        return true;  // журнала ещё нет — это не беда, а «правок не было»
    }
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("cannot read journal: %1").arg(file.errorString());
        return false;
    }
    const QByteArray blob = file.readAll();
    file.close();
    return out->parse(blob, ZJournal::Want::Frames, -1, error);
}

bool ZStorage::journalSnapshotLocked(const QString& path, int index, QByteArray* out,
                               QString* error) const {
    assertLocked();
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("cannot read journal: %1").arg(file.errorString());
        return false;
    }
    const QByteArray blob = file.readAll();
    file.close();

    ZJournal jrn;
    if (!jrn.parse(blob, ZJournal::Want::Chain, index, error)) return false;
    if (index < 0 || index >= jrn.size()) {
        if (error) *error = QStringLiteral("journal has no record #%1").arg(index);
        return false;
    }
    const ZJournal::Record& entry = jrn.at(index);
    if (!entry.hasSnapshot()) {
        if (error)
            *error = QStringLiteral("record #%1 (%2) has no snapshot")
                         .arg(index)
                         .arg(describeKind(entry.kind()));
        return false;
    }
    return jrn.rebuildAt(index, out, error);
}

bool ZStorage::trimJournalTailLocked(const QString& path, QString* error) {
    assertLocked();
    ZJournal jrn;
    QFile probe(path);
    if (!probe.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("cannot read journal: %1").arg(probe.errorString());
        return false;
    }
    const QByteArray blob = probe.readAll();
    probe.close();
    if (!jrn.parse(blob, ZJournal::Want::Frames, -1, error)) return false;
    if (!jrn.tailTrimmed()) return true;
    QFile file(path);
    if (!file.open(QIODevice::ReadWrite)) {
        if (error) *error = QStringLiteral("cannot open journal: %1").arg(file.errorString());
        return false;
    }
    const bool ok = file.resize(jrn.goodBytes());
    file.close();
    if (!ok && error) *error = QStringLiteral("cannot cut the journal tail");
    return ok;
}

// Шкала прореживания. Ведро — целое число; записи одного ведра схлопываются в
// одну (последнюю, то есть состояние на конец минуты/часа/дня, а не на начало).
// Ветки разнесены слагаемыми, чтобы ведро суток никогда не совпало с ведром
// недели.
namespace {
constexpr qint64 kMinute = 60 * 1000;
constexpr qint64 kHour = 60 * kMinute;
constexpr qint64 kDay = 24 * kHour;
constexpr qint64 kWeek = 7 * kDay;
constexpr qint64 kMonth = 30 * kDay;

qint64 bucketOf(qint64 stamp, qint64 now) {
    const qint64 age = now - stamp;
    if (age < kHour) return stamp;  // последний час — каждая запись сама себе ведро
    if (age < kDay) return -1 - stamp / kMinute;
    if (age < kWeek) return -(1LL << 40) - stamp / kHour;
    if (age < kMonth) return -(2LL << 40) - stamp / kDay;
    return -(3LL << 40) - stamp / kMonth;
}
}  // namespace

QVector<int> ZJournal::survivors(qint64 now) const {
    const QVector<ZJournal::Record>& entries = entries_;
    QVector<int> keep;
    for (int i = 0; i < entries.size(); ++i) {
        // Последняя запись остаётся всегда: для удалённой заметки это её
        // вечный финальный слепок и её tombstone.
        if (i + 1 == entries.size()) {
            keep.append(i);
            continue;
        }
        if (bucketOf(entries[i].time(), now) != bucketOf(entries[i + 1].time(), now)) keep.append(i);
    }
    return keep;
}

bool ZStorage::thinJournalLocked(const QString& path, qint64 now, QString* error) {
    assertLocked();
    QFileInfo info(path);
    if (!info.exists()) return true;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("cannot read journal: %1").arg(file.errorString());
        return false;
    }
    const QByteArray blob = file.readAll();
    file.close();
    // Метка состояния файла на момент чтения — по ней перед подменой видно,
    // не дописал ли кто-то запись, пока мы считали. Так прореживание можно
    // гонять в отдельном потоке, не запирая журнал.
    const qint64 sawSize = info.size();
    const QDateTime sawTime = info.lastModified();

    ZJournal jrn;
    if (!jrn.parse(blob, ZJournal::Want::All, -1, error)) return false;
    const QVector<int> keep = jrn.survivors(now);
    if (keep.size() == jrn.size() && !jrn.tailTrimmed()) return true;  // нечего делать

    // Версию чистки переносим как есть: прореживание — это про время, а не про
    // дубликаты, и объявить журнал чищеным оно права не имеет.
    QByteArray out;
    if (!jrn.toBytes(keep, jrn.cleanVersion(), &out, error)) return false;

    QFileInfo now2(path);
    if (now2.size() != sawSize || now2.lastModified() != sawTime) {
        // Журнал изменился под руками — в него дописали, пока мы считали.
        // Отменяемся молча: прореживание не обязано случиться именно сейчас,
        // а вот потерять свежую запись оно права не имеет.
        return true;
    }

    // Инвариант «журнал только растёт» действует между перезаписями; их всего
    // две — прореживание и чистка.
    return replaceFile(path, out, error);
}

bool ZStorage::rewriteJournalLocked(const QString& path, const ZJournal::Planner& planner, bool force,
                             ZJournal::CompressOutcome* outcome, QString* error) {
    assertLocked();
    ZJournal::CompressOutcome done;
    const auto finish = [&](bool ok) {
        if (outcome != nullptr) *outcome = done;
        return ok;
    };

    QFile file(path);
    if (!file.exists()) return finish(true);   // журнала нет — и чистить нечего
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("cannot read journal: %1").arg(file.errorString());
        return finish(false);
    }
    const QByteArray blob = file.readAll();
    file.close();

    ZJournal jrn;
    if (!jrn.parse(blob, ZJournal::Want::All, -1, error)) return finish(false);

    done.versionBefore = jrn.cleanVersion();
    done.versionAfter = jrn.cleanVersion();
    done.recordsBefore = jrn.size();
    done.recordsAfter = done.recordsBefore;

    // Ленивость: чищеный журнал не трогается вовсе. Люк ходит с force.
    const QString target = QString::fromLatin1(ZJournal::kCleanVersion);
    if (!force && jrn.cleanVersion() == target) return finish(true);

    // Правилу нужны слепки, а не байты: «одинаковы ли две записи» — вопрос про
    // содержимое. Распаковываем разом, потому что дальше правило смотрит на
    // них помногу раз, и распаковывать по требованию значило бы делать это
    // заново на каждом проходе.
    QVector<QByteArray> plain(jrn.size());
    for (int i = 0; i < jrn.size(); ++i) {
        if (!jrn.at(i).hasSnapshot()) continue;
        if (!jrn.rebuildAt(i, &plain[i], error)) return finish(false);
    }

    const QVector<int> keep = planner(jrn, plain);
    // План приходит снаружи, и доверять ему на слово нельзя: перепутанный
    // порядок или номер за границей испортили бы журнал молча.
    for (int i = 0; i < keep.size(); ++i)
        if (keep[i] < 0 || keep[i] >= jrn.size() ||
            (i > 0 && keep[i] <= keep[i - 1])) {
            if (error) *error = QStringLiteral("cleanup rule returned an invalid list");
            return finish(false);
        }

    // НИ БАЙТА, ЕСЛИ НИЧЕГО НЕ ПОМЕНЯЛОСЬ — на этом стоит обещание
    // идемпотентности: повторный форс не переписывает файл вовсе.
    if (keep.size() == jrn.size() && !jrn.tailTrimmed() &&
        jrn.cleanVersion() == target)
        return finish(true);

    // КОГО ВЫБРОСИЛИ — НАЗЫВАЕМ ВСЛУХ (m17, сессия 3). Прежде пересборка
    // стирала записи молча, и это было верно ровно до появления облака:
    // стёртая молча запись возвращается объединением с другого устройства —
    // и возвращается вечно. Теперь их адреса («время + отпечаток») уезжают в
    // запись гашения, которая доедет туда же, куда доехали они.
    //
    // Адрес, а не порог: порог погасил бы конкурентную правку, случайно
    // оказавшуюся ниже него, и катастрофически чувствителен к порче — один
    // бит превращает 16 в 4096.
    QVector<ZJournal::RecordRef> voided;
    {
        int at = 0;
        for (int i = 0; i < jrn.size(); ++i) {
            if (at < keep.size() && keep[at] == i) { ++at; continue; }
            voided.append(ZJournal::RecordRef(jrn.at(i).time(), jrn.at(i).digest()));
        }
    }

    QByteArray out;
    if (!jrn.toBytes(keep, target, &out, error)) return finish(false);
    if (!replaceFile(path, out, error)) return finish(false);

    done.versionAfter = target;
    done.recordsAfter = int(keep.size());
    done.rewritten = true;

    // Запись гашения — ПОСЛЕ пересборки, обычной дозаписью: она такая же
    // запись журнала, как всякая другая, и путь у неё один со всеми.
    // Надгробию гашение не приписываем: у удалённой заметки уже есть своя
    // запись, называющая погашенное.
    if (!voided.isEmpty()) {
        if (!appendJournalLocked(path, ZJournal::NewRecord::amendment().voiding(voided),
                                 error))
            return finish(false);
        ++done.recordsAfter;
    }
    return finish(true);
}

// Общий CAS-страж двух «принятий»: файл журнала обязан быть тем же, каким его
// видел вызывающий, иначе слияние считалось от устаревших байтов.
static bool journalUnchanged(const QString& path, const Digest& expectedBytes, QString* error) {
    QFile f(path);
    Digest now;
    if (f.open(QIODevice::ReadOnly)) {
        const QByteArray bytes = f.readAll();
        now = hashOf(std::string_view(bytes.constData(), size_t(bytes.size())));
    }
    if (now == expectedBytes) return true;
    if (error)
        *error = QStringLiteral("the journal changed while the merge was running — deferred");
    return false;
}

bool ZStorage::adoptMergedJournalLocked(const QString& path, const ZJournal& merged,
                                        const Digest& expectedBytes, QString* error) {
    assertLocked();
    if (!journalUnchanged(path, expectedBytes, error)) return false;
    QVector<int> all;
    all.reserve(merged.size());
    for (int i = 0; i < merged.size(); ++i) all.append(i);
    QByteArray out;
    if (!merged.toBytes(all, merged.cleanVersion(), &out, error)) return false;

    // Самопроверка ПЕРЕД подменой — как у заметки в пути сохранения: слитое
    // обязано доказать, что читается обратно без потерь, прежде чем займёт
    // место журнала. Не доказало — файл не тронут ни байтом.
    ZJournal reread;
    if (!reread.parse(out, ZJournal::Want::All, 0, error)) return false;
    if (reread.size() != merged.size() || reread.damagedCount() != 0 || reread.tailTrimmed()) {
        if (error)
            *error = QStringLiteral(
                "merged journal does not survive its own serialization (%1 -> %2 records)")
                         .arg(merged.size())
                         .arg(reread.size());
        return false;
    }
    for (int i = 0; i < reread.size(); ++i) {
        if (!reread.at(i).hasSnapshot()) continue;
        QByteArray plain;
        // rebuildAt сверяет отпечаток последнего звена сам.
        if (!reread.rebuildAt(i, &plain, error)) return false;
    }
    if (reread.contentDigest() != merged.contentDigest()) {
        if (error)
            *error = QStringLiteral("merged journal changed identity in serialization");
        return false;
    }

    // Журнала могло не быть вовсе (заметка приехала целиком) — history/ тоже.
    QDir().mkpath(QFileInfo(path).absolutePath());
    return replaceFile(path, out, error);
}

bool ZStorage::adoptJournalBytesLocked(const QString& path, const QByteArray& bytes,
                                       const Digest& expectedBytes, QString* error) {
    assertLocked();
    if (!journalUnchanged(path, expectedBytes, error)) return false;
    // Чужие байты приехали из атомарной заливки целого файла: рваному хвосту
    // и испорченной рамке взяться неоткуда, и такое не принимается вовсе —
    // absence-философия оставляет лечение обмену.
    ZJournal reread;
    if (!reread.parse(bytes, ZJournal::Want::All, 0, error)) return false;
    if (reread.damagedCount() != 0 || reread.tailTrimmed()) {
        if (error) *error = QStringLiteral("the incoming journal does not prove itself");
        return false;
    }
    for (int i = 0; i < reread.size(); ++i) {
        if (!reread.at(i).hasSnapshot()) continue;
        QByteArray plain;
        if (!reread.rebuildAt(i, &plain, error)) return false;
    }
    QDir().mkpath(QFileInfo(path).absolutePath());
    return replaceFile(path, bytes, error);
}

// Открытые методы: замок и ничего больше. Ни одной строки работы с файлами
// здесь нет и быть не должно — на этом стоит обещание «всё под замком».
bool ZStorage::appendToJournal(const QString& noteId, const ZJournal::NewRecord& what, QString* error) {
    const QMutexLocker locked(&gate());
    return appendJournalLocked(journalPath(noteId), what, error);
}


bool ZStorage::adoptMergedJournal(const QString& noteId, const ZJournal& merged,
                                  const Digest& expectedBytes, QString* error) {
    const QMutexLocker locked(&gate());
    return adoptMergedJournalLocked(journalPath(noteId), merged, expectedBytes, error);
}

bool ZStorage::adoptJournalBytes(const QString& noteId, const QByteArray& bytes,
                                 const Digest& expectedBytes, QString* error) {
    const QMutexLocker locked(&gate());
    return adoptJournalBytesLocked(journalPath(noteId), bytes, expectedBytes, error);
}


bool ZStorage::readJournalBytes(const QString& noteId, QByteArray* out,
                               QString* error) const {
    Q_ASSERT(out != nullptr);
    const QMutexLocker locked(&gate());
    out->clear();
    QFile file(journalPath(noteId));
    if (!file.exists()) return true;   // журнала нет — заливать нечего
    if (!file.open(QIODevice::ReadOnly)) {
        if (error != nullptr)
            *error = QStringLiteral("cannot read journal of %1: %2")
                         .arg(noteId, file.errorString());
        return false;
    }
    *out = file.readAll();
    return true;
}

bool ZStorage::readJournal(const QString& noteId, ZJournal* out, QString* error) const {
    const QMutexLocker locked(&gate());
    return readJournalLocked(journalPath(noteId), out, error);
}

bool ZStorage::journalSnapshot(const QString& noteId, int index, QByteArray* out,
                         QString* error) const {
    const QMutexLocker locked(&gate());
    return journalSnapshotLocked(journalPath(noteId), index, out, error);
}

bool ZStorage::trimJournalTail(const QString& noteId, QString* error) {
    const QMutexLocker locked(&gate());
    return trimJournalTailLocked(journalPath(noteId), error);
}

bool ZStorage::removeJournal(const QString& noteId, QString* error) {
    const QMutexLocker locked(&gate());
    return removeJournalLocked(journalPath(noteId), error);
}

bool ZStorage::thinJournal(const QString& noteId, qint64 now, QString* error) {
    const QMutexLocker locked(&gate());
    return thinJournalLocked(journalPath(noteId), now, error);
}

bool ZStorage::rewriteJournal(const QString& noteId, const ZJournal::Planner& planner, bool force,
                       ZJournal::CompressOutcome* outcome, QString* error) {
    const QMutexLocker locked(&gate());
    return rewriteJournalLocked(journalPath(noteId), planner, force, outcome, error);
}

ZJournal::ThinReport ZStorage::thinAllJournals(qint64 now, bool dryRun) {
    ZJournal::ThinReport report;
    const QDir history(QDir(root_).filePath(QStringLiteral("history")));
    if (!history.exists()) return report;

    for (const QString& name : history.entryList({QStringLiteral("*.log")}, QDir::Files)) {
        const QString noteId = name.left(name.size() - 4);
        ZJournal before;
        QString error;
        if (!readJournal(noteId, &before, &error)) {
            report.problems.append(QStringLiteral("%1: %2").arg(name, error));
            continue;
        }
        ++report.journals;
        report.recordsBefore += before.size();
        report.bytesBefore += QFileInfo(journalPath(noteId)).size();
        if (before.tailTrimmed()) report.trimmed.append(name);

        if (dryRun) {
            report.recordsAfter += before.survivors(now).size();
            report.bytesAfter += QFileInfo(journalPath(noteId)).size();
            continue;
        }
        if (!thinJournal(noteId, now, &error)) {
            report.problems.append(QStringLiteral("%1: %2").arg(name, error));
            continue;
        }
        ZJournal after;
        readJournal(noteId, &after, &error);
        report.recordsAfter += after.size();
        report.bytesAfter += QFileInfo(journalPath(noteId)).size();
    }
    return report;
}

bool ZStorage::compressJournal(const QString& noteId, const ZJournal::Rules& rules,
                               bool force, ZStorage::CompressReport* report, QString* error) {
    // ВОТ ЗДЕСЬ МИГРАЦИЯ И РАСХОДИТСЯ С ЖИВОЙ ЗАПИСЬЮ, и больше нигде: она
    // чистит ретроактивно. Ставится сторож здесь, а не вызывающим, чтобы
    // «забыть выключить возраст» было негде.
    ZJournal::Rules retro = rules;
    retro.ignoreAge = true;

    ZJournal::Plan plan;
    ZJournal::CompressOutcome outcome;
    const bool ok = rewriteJournal(
        noteId,
        [&](const ZJournal& jrn, const QVector<QByteArray>& snapshots) {
            plan = jrn.planCompress(snapshots, retro);
            return plan.keep;
        },
        force, &outcome, error);

    if (report != nullptr) {
        report->versionBefore = outcome.versionBefore;
        report->versionAfter = outcome.versionAfter;
        report->recordsBefore = outcome.recordsBefore;
        report->recordsAfter = outcome.recordsAfter;
        report->rewritten = outcome.rewritten;
        // Считанное правилом годится, только если правило вообще спрашивали:
        // у чищеного журнала механика до плана не доходит.
        report->duplicates = outcome.rewritten ? plan.duplicates : 0;
        report->merged = outcome.rewritten ? plan.merged : 0;
    }
    return ok;
}

// --- идентичность хранилища -------------------------------------------------

QString ZStorage::identityPath() const {
    return QDir(root_).filePath(QLatin1String(Identity::kFile));
}

ZStorage::Identity ZStorage::identity(QString* error) const {
    Identity out;
    if (!store_) {
        if (error != nullptr) *error = QStringLiteral("not a store: %1").arg(root_);
        return out;
    }
    QFile file(identityPath());
    if (!file.exists()) return out;   // не беда: хранилища старых сборок его не имеют
    if (!file.open(QIODevice::ReadOnly)) {
        if (error != nullptr)
            *error = QStringLiteral("cannot read %1: %2")
                         .arg(QLatin1String(Identity::kFile), file.errorString());
        return out;
    }
    const QByteArray bytes = file.readAll();
    file.close();
    Identity read;
    if (!read.parse(bytes, error)) return out;
    return read;
}

ZStorage::Identity ZStorage::ensureIdentity(QString* error) {
    Identity have = identity(error);
    if (!have.isEmpty()) return have;
    if (!store_) return have;
    // Чеканка. id хранилища — той же чеканки, что у заметок: одна азбука на всё
    // хранилище, и по виду сразу понятно, что это наш идентификатор.
    Identity fresh = Identity::mint(QString::fromStdString(newNoteId()), store::isoNow());
    if (!writeIdentity(fresh, error)) return {};
    return fresh;
}

bool ZStorage::setRootNote(const QString& noteId, QString* error) {
    Identity have = ensureIdentity(error);
    if (have.isEmpty()) return false;
    if (have.rootNote() == noteId) return true;   // писать нечего
    have.setRootNote(noteId);
    return writeIdentity(have, error);
}

bool ZStorage::writeIdentity(const Identity& identity, QString* error) {
    QSaveFile file(identityPath());
    if (!file.open(QIODevice::WriteOnly)) {
        if (error != nullptr)
            *error = QStringLiteral("cannot write %1: %2")
                         .arg(QLatin1String(Identity::kFile), file.errorString());
        return false;
    }
    file.write(identity.toBytes());
    if (!file.commit()) {
        if (error != nullptr)
            *error = QStringLiteral("cannot write %1: %2")
                         .arg(QLatin1String(Identity::kFile), file.errorString());
        return false;
    }
    return true;
}

// --- корневая заметка -------------------------------------------------------

bool ZStorage::isRootNote(const QString& id) const {
    const auto it = notes_.constFind(id);
    return it != notes_.constEnd() && it.value().root();
}

QString ZStorage::rootId() const {
    if (!store_) return {};
    // КАТАЛОГ ОБЯЗАН БЫТЬ ПРОЧИТАН. Иначе «корня нет» ответил бы всякий свежий
    // объект хранилища — и ensureRootNote завёл бы ВТОРОЙ корень. Ошибка
    // дорогая и тихая, поэтому дверь одна и сторож в ней.
    if (!loaded_) const_cast<ZStorage*>(this)->reload();
    // Сперва адрес из zametti.json: это дёшево и это истина, названная явно.
    const QString named = identity().rootNote();
    if (!named.isEmpty() && has(named)) return named;
    // Не назван или назван неверно — ищем по РОЛИ: две записи одного факта, и
    // расхождение лечится (`zametti store root fix`), а не роняет программу.
    for (auto it = notes_.constBegin(); it != notes_.constEnd(); ++it)
        if (it.value().root()) return it.key();
    return {};
}

QString ZStorage::ensureRootNote(QString* error) {
    if (!store_) {
        if (error != nullptr) *error = QStringLiteral("not a store");
        return {};
    }
    const QString have = rootId();
    if (!have.isEmpty()) {
        // Роль есть, а в файле адреса нет (или он устарел) — назовём.
        if (identity().rootNote() != have) setRootNote(have, error);
        return have;
    }
    // РОЖДЕНИЕ КОРНЯ. Имя хранилища по умолчанию — имя каталога: человек
    // узнаёт своё хранилище с первого взгляда, а переименовать сможет по F2.
    const Batch batch(*this);
    const QString id = idOfPath(newNoteFile(QString(), error));
    if (id.isEmpty()) return {};
    const QString name = QDir(root_).dirName();
    QString why;
    if (!rewriteNote(id, [&name](ZNote& note) {
            note.setRole(QStringLiteral("root"));
            note.doc().setTitle(name.isEmpty() ? QStringLiteral("Notes") : name);
        }, ZJournal::Rules{}, &why)) {
        if (error != nullptr) *error = why;
        return {};
    }
    refreshNote(id);
    if (!setRootNote(id, error)) return {};
    return id;
}

}  // namespace zametti
