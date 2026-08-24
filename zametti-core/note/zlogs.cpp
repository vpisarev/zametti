#include "zlogs.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutexLocker>
#include <QSaveFile>
#include <QStandardPaths>

#include <cstdio>

namespace zametti {

ZLogs& ZLogs::instance() {
    static ZLogs logs;
    return logs;
}

ZLogs::ZLogs(const QString& dir)
    : dir_(dir.isEmpty()
               ? QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)
               : dir) {}

void ZLogs::configure(const Limits& limits) {
    const QMutexLocker locked(&gate_);
    limits_ = limits;
    errChecked_ = false;
    syncChecked_ = false;
}

QString ZLogs::errPath() const { return dir_ + QStringLiteral("/err.log"); }
QString ZLogs::syncPath() const { return dir_ + QStringLiteral("/sync.log"); }

void ZLogs::write(LogKind kind, const QString& line) {
    const QMutexLocker locked(&gate_);
    if (kind == LogKind::Err)
        writeFile(errPath(), limits_.errEnabled, &errChecked_, line);
    else
        writeFile(syncPath(), limits_.syncEnabled, &syncChecked_, line);
}

void ZLogs::writeFile(const QString& path, bool enabled, bool* checked, const QString& line) {
    if (!enabled) return;
    if (!*checked) {
        *checked = true;
        trimIfOversized(path);
    }
    QDir().mkpath(dir_);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Append)) {
        // Лог — удобство, не условие работы: жалуемся в stderr и живём.
        fprintf(stderr, "zametti: cannot write %s: %s\n", qPrintable(path),
                qPrintable(f.errorString()));
        return;
    }
    const QString stamp =
        QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    f.write(stamp.toUtf8() + ' ' + line.toUtf8() + '\n');
}

void ZLogs::trimIfOversized(const QString& path) {
    QFile f(path);
    const qint64 size = QFileInfo(path).size();
    if (size <= limits_.maxBytes) return;
    // Свежие 80% предела, хвостом: файл целиком не читается никогда.
    const qint64 keep = limits_.maxBytes * 8 / 10;
    if (!f.open(QIODevice::ReadOnly)) return;
    if (!f.seek(size - keep)) return;
    QByteArray tail = f.readAll();
    f.close();
    // Рез по границе строки: первая (вероятно рваная) строка выбрасывается.
    const int firstBreak = tail.indexOf('\n');
    if (firstBreak >= 0) tail.remove(0, firstBreak + 1);
    QSaveFile out(path);
    if (!out.open(QIODevice::WriteOnly)) return;
    out.write(tail);
    out.commit();
}

}  // namespace zametti
