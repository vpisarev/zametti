// FolderCloud: «облако» в каталоге. См. шапку folder_remote.h.

#include "folder_remote.h"

#include "hash.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

namespace zametti {

FolderCloud::FolderCloud(const QString& dir) : dir_(dir) {}

QString FolderCloud::pathOf(const QString& name) const {
    // Имена блобов плоские. Всё, что похоже на путь, отвергается здесь, а не
    // где-то в глубине: «..» в имени с сервера — это чужая попытка написать
    // мимо каталога.
    if (name.isEmpty() || name.contains(QLatin1Char('/')) ||
        name.contains(QLatin1Char('\\')) || name == QLatin1String("..") ||
        name.startsWith(QLatin1Char('.')))
        return QString();
    return dir_ + QLatin1Char('/') + name;
}

bool FolderCloud::tripped(const QString& op, QString* error) {
    const int left = failures_.value(op, 0);
    if (left <= 0) return false;
    failures_.insert(op, left - 1);
    if (error != nullptr)
        *error = QStringLiteral("folder remote: %1 failed on purpose").arg(op);
    return true;
}

void FolderCloud::failNext(const QString& op, int times) {
    failures_.insert(op, failures_.value(op, 0) + times);
}

QString FolderCloud::etagOf(const QByteArray& bytes) const {
    const std::string material =
        std::to_string(etagSalt_) + ":" +
        std::string(bytes.constData(), size_t(bytes.size()));
    return QString::fromStdString(hashOf(material).hex()).left(32);
}

bool FolderCloud::list(QVector<Entry>* out, QString* error) {
    Q_ASSERT(out != nullptr);
    ++counters_.lists;
    ++traffic_.requests;
    if (tripped(QStringLiteral("list"), error)) return false;

    QDir dir(dir_);
    if (!dir.exists()) {
        if (error != nullptr)
            *error = QStringLiteral("folder remote: %1 does not exist").arg(dir_);
        return false;
    }
    out->clear();
    const QFileInfoList files = dir.entryInfoList(QDir::Files, QDir::Name);
    for (const QFileInfo& info : files) {
        // etag считаем по содержимому: иначе «метка версии» врала бы при
        // копировании каталога. Листинг от этого дороже, но это стенд.
        QFile file(info.absoluteFilePath());
        if (!file.open(QIODevice::ReadOnly)) continue;
        const QByteArray bytes = file.readAll();
        Entry entry;
        entry.name = info.fileName();
        entry.size = bytes.size();
        entry.etag = etagOf(bytes);
        out->append(entry);
    }
    return true;
}

bool FolderCloud::get(const QString& name, QByteArray* bytes, QString* etag,
                       QString* error) {
    Q_ASSERT(bytes != nullptr);
    ++counters_.gets;
    ++traffic_.requests;
    if (tripped(QStringLiteral("get"), error)) return false;

    const QString path = pathOf(name);
    QFile file(path);
    if (path.isEmpty() || !file.open(QIODevice::ReadOnly)) {
        if (error != nullptr)
            *error = QStringLiteral("folder remote: cannot read %1").arg(name);
        return false;
    }
    *bytes = file.readAll();
    traffic_.bytesDown += bytes->size();
    if (etag != nullptr) *etag = etagOf(*bytes);
    return true;
}

bool FolderCloud::put(const QString& name, const QByteArray& bytes, QString* etag,
                       QString* error) {
    ++counters_.puts;
    ++traffic_.requests;
    if (tripped(QStringLiteral("put"), error)) return false;

    const QString path = pathOf(name);
    if (path.isEmpty()) {
        if (error != nullptr)
            *error = QStringLiteral("folder remote: bad blob name %1").arg(name);
        return false;
    }
    // Через временный файл с переименованием: оборванная заливка не должна
    // оставлять на «сервере» полблоба (настоящий сервер держит PUT
    // атомарным сам).
    const QString temp = path + QLatin1String(".part");
    QFile file(temp);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error != nullptr)
            *error = QStringLiteral("folder remote: cannot write %1").arg(name);
        return false;
    }
    const bool written = file.write(bytes) == bytes.size();
    file.close();
    const ZSystem area = files();
    if (!written) {
        area.removeForever(temp);
        if (error != nullptr)
            *error = QStringLiteral("folder remote: short write on %1").arg(name);
        return false;
    }
    QString why;
    if (!area.removeForever(path, &why)) {
        if (error != nullptr)
            *error = QStringLiteral("folder remote: cannot replace %1: %2").arg(name, why);
        return false;
    }
    if (!area.rename(temp, path, &why)) {
        area.removeForever(temp);
        if (error != nullptr)
            *error = QStringLiteral("folder remote: cannot replace %1: %2").arg(name, why);
        return false;
    }
    traffic_.bytesUp += bytes.size();
    if (etag != nullptr) *etag = etagOf(bytes);
    return true;
}

bool FolderCloud::putIfMatch(const QString& name, const QByteArray& bytes,
                              const QString& expectedEtag, QString* etag,
                              bool* preconditionFailed, QString* error) {
    if (preconditionFailed != nullptr) *preconditionFailed = false;
    // Условие проверяем ЧЕСТНО — стенд обязан уметь то, что умеет хороший
    // сервер, иначе движок никогда не встретит отказ по условию в наборах.
    QByteArray current;
    QString currentEtag;
    const QString path = pathOf(name);
    if (!path.isEmpty() && QFile::exists(path)) {
        QFile file(path);
        if (file.open(QIODevice::ReadOnly)) {
            current = file.readAll();
            currentEtag = etagOf(current);
        }
    }
    if (currentEtag != expectedEtag) {
        ++counters_.puts;
        ++traffic_.requests;
        if (preconditionFailed != nullptr) *preconditionFailed = true;
        if (error != nullptr)
            *error = QStringLiteral("folder remote: %1 changed under us").arg(name);
        return false;
    }
    return put(name, bytes, etag, error);
}

bool FolderCloud::del(const QString& name, QString* error) {
    ++counters_.dels;
    ++traffic_.requests;
    if (tripped(QStringLiteral("del"), error)) return false;

    const QString path = pathOf(name);
    if (path.isEmpty()) {
        if (error != nullptr)
            *error = QStringLiteral("folder remote: bad blob name %1").arg(name);
        return false;
    }
    // Нет файла — не беда: удаление идемпотентно, как и на сервере. Мимо
    // корзины нарочно: облачными блобами её засорять незачем.
    QString why;
    if (files().removeForever(path, &why)) return true;
    if (error != nullptr)
        *error = QStringLiteral("folder remote: cannot remove %1: %2").arg(name, why);
    return false;
}

bool FolderCloud::mkdirOnce(QString* error) {
    ++counters_.mkdirs;
    ++traffic_.requests;
    if (tripped(QStringLiteral("mkdir"), error)) return false;

    QDir dir(dir_);
    if (dir.exists()) return true;
    if (!QDir().mkpath(dir_)) {
        if (error != nullptr)
            *error = QStringLiteral("folder remote: cannot create %1").arg(dir_);
        return false;
    }
    return true;
}

}  // namespace zametti
