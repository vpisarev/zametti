// ZSystem: единственная дверь наружу. См. шапку zsystem.h.

#include "zsystem.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>

namespace zametti {

namespace {

// Путь в его окончательном виде. Симлинки разворачиваются, когда цель есть;
// когда её нет — остаётся вычищенный абсолютный путь. Относительное сюда не
// попадает: его отсеивают выше, ДО этой функции, и в этом весь смысл (именно
// резолвинг относительного пути по рабочему каталогу процесса и стоил владельцу
// каталога).
QString settle(const QString& absolutePath) {
    const QString canonical = QFileInfo(absolutePath).canonicalFilePath();
    if (!canonical.isEmpty()) return canonical;
    return QDir::cleanPath(absolutePath);
}

// dir — это other или предок other? Сравниваются готовые (settle) пути.
bool atOrAbove(const QString& dir, const QString& other) {
    if (dir.isEmpty() || other.isEmpty()) return false;
    if (dir == other) return true;
    return other.startsWith(dir.endsWith(QLatin1Char('/')) ? dir : dir + QLatin1Char('/'));
}

// Каталоги, которые не смеет трогать никто и ни при каких доводах.
QStringList sacredDirs() {
    return {settle(QDir::rootPath()), settle(QDir::homePath()), settle(QDir::currentPath())};
}

}  // namespace

ZSystem::ZSystem(Area area, const QString& root) : area_(area) {
    // ОТНОСИТЕЛЬНЫЙ КОРЕНЬ ОБЛАСТИ НЕ БЫВАЕТ — он и есть инцидент. Объект
    // остаётся бессильным: covers() false, разрушить им нельзя ничего.
    if (root.isEmpty() || !QDir::isAbsolutePath(root)) return;
    const QString settled = settle(root);
    // Областью не объявляют корень файловой системы, домашний каталог или
    // рабочий каталог процесса: «внутри» такой области лежит слишком многое.
    for (const QString& sacred : sacredDirs())
        if (settled == sacred) return;
    root_ = settled;
}

bool ZSystem::covers(const QString& path) const {
    if (root_.isEmpty() || path.isEmpty()) return false;
    if (!QDir::isAbsolutePath(path)) return false;
    const QFileInfo info(path);
    // Канонизуется РОДИТЕЛЬ: удаляемого файла может уже не быть, а симлинк по
    // дороге к нему обязан быть развёрнут.
    const QString parent = settle(info.absolutePath());
    if (parent.isEmpty()) return false;
    const QString target = parent + QLatin1Char('/') + info.fileName();
    // Строго внутри: сам корень области этой дверью не сносится.
    return target.startsWith(root_ + QLatin1Char('/'));
}

// Общая часть обоих удалений: сказать, почему нельзя, — или разрешить.
// Каталог не удаляется НИКОГДА: у каталогов своя дверь, и она с доказательством.
bool ZSystem::guard(const QString& path, const char* what, QString* error) const {
    if (!covers(path)) {
        if (error != nullptr)
            *error = root_.isEmpty()
                         ? QStringLiteral("%1 %2: the area is not named").arg(QLatin1String(what), path)
                         : QStringLiteral("%1 %2: outside %3").arg(QLatin1String(what), path, root_);
        return false;
    }
    const QFileInfo info(path);
    if (info.isDir() && !info.isSymLink()) {
        if (error != nullptr)
            *error = QStringLiteral("%1 %2: it is a directory — directories go through "
                                    "removeStorageTree, which asks for proof")
                         .arg(QLatin1String(what), path);
        return false;
    }
    return true;
}

bool ZSystem::remove(const QString& path, QString* error) const {
    if (!guard(path, "cannot delete", error)) return false;
    const QFileInfo info(path);
    if (!info.exists() && !info.isSymLink()) return true;  // уже нет — и хорошо
    if (QFile::moveToTrash(path)) return true;
    if (QFile::remove(path)) return true;
    if (error != nullptr) *error = QStringLiteral("cannot delete %1").arg(path);
    return false;
}

bool ZSystem::removeForever(const QString& path, QString* error) const {
    if (!guard(path, "cannot delete", error)) return false;
    const QFileInfo info(path);
    if (!info.exists() && !info.isSymLink()) return true;
    if (QFile::remove(path)) return true;
    if (error != nullptr) *error = QStringLiteral("cannot delete %1").arg(path);
    return false;
}

bool ZSystem::rename(const QString& from, const QString& to, QString* error) const {
    if (!guard(from, "cannot rename", error)) return false;
    // Второй конец проверяется тем же мерилом: переименование наружу — это
    // перемещение, и этой дверью его не делают.
    if (!covers(to)) {
        if (error != nullptr)
            *error = QStringLiteral("cannot rename %1 to %2: the target is outside %3")
                         .arg(from, to, root_.isEmpty() ? QStringLiteral("(unnamed area)") : root_);
        return false;
    }
    if (QFile::rename(from, to)) return true;
    if (error != nullptr) *error = QStringLiteral("cannot rename %1 to %2").arg(from, to);
    return false;
}

bool ZSystem::looksLikeStorage(const QString& dir) {
    return QFileInfo(dir + QStringLiteral("/.zametti")).isDir();
}

QStringList ZSystem::homeAreas() {
    QStringList out;
    // Главный ответ — от собственного дома: /Users/вася → /Users,
    // /home/вася → /home, C:/Users/вася → C:/Users. Так работает и там, где мы
    // про имя каталога не догадались бы.
    const QString home = settle(QDir::homePath());
    const QString root = settle(QDir::rootPath());
    if (!home.isEmpty()) {
        const QString parent = settle(QFileInfo(home).absolutePath());
        // Дом прямо в корне (root в контейнере) жилой зоной не делает весь диск.
        if (!parent.isEmpty() && parent != root) out.append(parent);
    }
    // И два общеизвестных — на случай, когда наш дом уехал, а чужие на месте.
    for (const char* known : {"/home", "/Users"}) {
        const QString dir = QString::fromLatin1(known);
        if (QFileInfo(dir).isDir()) out.append(settle(dir));
    }
    out.removeDuplicates();
    return out;
}

QStringList ZSystem::tempAreas() {
    QStringList out;
    const auto take = [&out](const QString& dir) {
        if (dir.isEmpty() || !QDir::isAbsolutePath(dir)) return;
        if (!QFileInfo(dir).isDir()) return;
        const QString settled = settle(dir);
        // Корнем временная зона не бывает: это сделало бы временным всё.
        if (settled.isEmpty() || settled == settle(QDir::rootPath())) return;
        out.append(settled);
    };
    // Qt первым: он и есть реализация стандарта своей системы —
    // GetTempPath() под Windows, NSTemporaryDirectory() на маке, TMPDIR или
    // /tmp на прочих Unix.
    take(QDir::tempPath());
#ifdef Q_OS_WIN
    // Windows: порядок GetTempPath() — %TMP%, потом %TEMP%.
    for (const char* var : {"TMP", "TEMP"}) take(QString::fromLocal8Bit(qgetenv(var)));
#else
    // POSIX: переменная ровно одна.
    take(QString::fromLocal8Bit(qgetenv("TMPDIR")));
    // И места, названные самими системами. /tmp на маке — симлинк на
    // /private/tmp, и settle() разворачивает его сам.
    take(QStringLiteral("/tmp"));
    take(QStringLiteral("/var/tmp"));
#endif
    out.removeDuplicates();
    return out;
}

bool ZSystem::mayRemoveTree(const QString& dir, QString* error) {
    const auto no = [&](const QString& why) {
        if (error != nullptr) *error = why;
        return false;
    };
    const QString settled = settle(dir);

    // 1. Каталог, в котором лежит корень, дом или мы сами, не сносится никогда.
    for (const QString& sacred : sacredDirs())
        if (atOrAbove(settled, sacred))
            return no(QStringLiteral("refusing to delete \"%1\": it holds \"%2\"")
                          .arg(settled, sacred));

    // 2. Временная зона — единственное место, где сносить можно свободно. Она
    //    же лежит в жилой зоне и на маке (tempPath = /Users/…/tmp), и на
    //    Windows (%LOCALAPPDATA%\\Temp внутри C:\\Users\\<имя>), поэтому
    //    проверяется ДО запрета, а не после.
    for (const QString& temp : tempAreas())
        if (settled.startsWith(temp + QLatin1Char('/'))) return true;

    // 3. Жилая зона — нет. Слово владельца: внутри /home и /Users программа
    //    каталогов не удаляет.
    for (const QString& homes : homeAreas())
        if (settled == homes || settled.startsWith(homes + QLatin1Char('/')))
            return no(QStringLiteral("refusing to delete \"%1\": it lives under \"%2\", "
                                     "where people keep their things — zametti removes no "
                                     "directory there, ever")
                          .arg(settled, homes));
    return true;
}

bool ZSystem::removeStorageTree(const QString& dir, QString* error) {
    const auto no = [&](const QString& why) {
        if (error != nullptr) *error = why;
        return false;
    };
    if (dir.isEmpty() || !QDir::isAbsolutePath(dir))
        return no(QStringLiteral("refusing to delete \"%1\": name the directory by an "
                                 "absolute path — a relative one is resolved against the "
                                 "process working directory, which is not the one you mean")
                      .arg(dir));
    const QString settled = settle(dir);
    if (!QFileInfo(settled).isDir()) return true;  // нет каталога — нечего и сносить
    if (!looksLikeStorage(settled))
        return no(QStringLiteral("refusing to delete \"%1\": it is not a zametti storage "
                                 "(no .zametti inside), and zametti deletes no other "
                                 "directory, ever")
                      .arg(settled));
    QString why;
    if (!mayRemoveTree(settled, &why)) return no(why);
    if (QDir(settled).removeRecursively()) return true;
    return no(QStringLiteral("cannot delete the storage %1").arg(settled));
}

bool ZSystem::removeScratchTree(const QString& dir, QString* error) {
    const auto no = [&](const QString& why) {
        if (error != nullptr) *error = why;
        return false;
    };
    if (dir.isEmpty() || !QDir::isAbsolutePath(dir))
        return no(QStringLiteral("refusing to delete \"%1\": name the directory by an "
                                 "absolute path").arg(dir));
    const QString settled = settle(dir);
    if (!QFileInfo(settled).isDir()) return true;  // нет каталога — нечего и сносить
    const QStringList temps = tempAreas();
    bool inside = false;
    for (const QString& temp : temps)
        // СТРОГО внутри: сам временный каталог — не песочница.
        if (settled.startsWith(temp + QLatin1Char('/'))) inside = true;
    if (!inside)
        return no(QStringLiteral("refusing to delete \"%1\": a scratch directory lives "
                                 "inside one of [%2] and nowhere else")
                      .arg(settled, temps.join(QStringLiteral(", "))));
    QString why;
    if (!mayRemoveTree(settled, &why)) return no(why);
    if (QDir(settled).removeRecursively()) return true;
    return no(QStringLiteral("cannot delete the scratch directory %1").arg(settled));
}

bool ZSystem::removeTreeInside(const QString& sandbox, const QString& dir, QString* error) {
    const auto no = [&](const QString& why) {
        if (error != nullptr) *error = why;
        return false;
    };
    if (dir.isEmpty() || !QDir::isAbsolutePath(dir))
        return no(QStringLiteral("refusing to delete \"%1\": name the directory by an "
                                 "absolute path").arg(dir));
    if (sandbox.isEmpty() || !QDir::isAbsolutePath(sandbox))
        return no(QStringLiteral("refusing to delete \"%1\": the sandbox \"%2\" is not "
                                 "an absolute path").arg(dir, sandbox));
    const QString settled = settle(dir);
    if (!QFileInfo(settled).isDir()) return true;  // нет каталога — нечего и сносить
    const QString home = settle(sandbox);
    // СТРОГО внутри: сама песочница — не то, что в ней лежит.
    if (!settled.startsWith(home + QLatin1Char('/')))
        return no(QStringLiteral("refusing to delete \"%1\": it is not inside the sandbox "
                                 "\"%2\"").arg(settled, home));
    QString why;
    if (!mayRemoveTree(settled, &why)) return no(why);
    if (QDir(settled).removeRecursively()) return true;
    return no(QStringLiteral("cannot delete %1").arg(settled));
}

bool ZSystem::runTool(const QString& program, const QStringList& args, int startMs,
                      int finishMs) {
    // Оболочки здесь нет вовсе: аргументы идут списком, а значит нет ни
    // подстановки, ни склейки строк, ни кавычек — и нечего внедрять.
    QProcess process;
    process.start(program, args);
    if (!process.waitForStarted(startMs)) return false;
    if (!process.waitForFinished(finishMs)) {
        process.kill();
        return false;
    }
    return process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
}

bool ZSystem::readTool(const QString& program, const QStringList& args, QString* out,
                       int startMs, int finishMs) {
    if (out != nullptr) out->clear();
    QProcess process;
    process.setProcessChannelMode(QProcess::SeparateChannels);
    process.start(program, args);
    if (!process.waitForStarted(startMs)) return false;
    if (!process.waitForFinished(finishMs)) {
        process.kill();
        return false;
    }
    if (out != nullptr) *out = QString::fromUtf8(process.readAllStandardOutput());
    return process.exitStatus() == QProcess::NormalExit;
}

bool ZSystem::startDetached(const QString& program, const QStringList& args) {
    return QProcess::startDetached(program, args);
}

QStringList ZSystem::splitCommand(const QString& command) {
    return QProcess::splitCommand(command);
}

}  // namespace zametti
