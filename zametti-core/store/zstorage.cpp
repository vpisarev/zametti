#include "zstorage.h"

#include "note_id.h"
#include "times.h"

#include "journal.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
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
    if (!store_ || id.isEmpty()) return ZNoteHistory();
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
