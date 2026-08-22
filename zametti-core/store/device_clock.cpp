#include "device_clock.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

namespace zametti::store {

QString DeviceClock::pathFor(const QString& root) {
    return QDir(root).filePath(QStringLiteral(".zametti/last-written"));
}

qint64 DeviceClock::floor() const {
    QFile file(pathFor(root_));
    if (!file.open(QIODevice::ReadOnly)) return 0;
    // Число десятичным текстом, а не восемью байтами: его видно глазом в
    // отладке, и оно не зависит от порядка байтов машины. Читаем с потолком,
    // чтобы мусор вместо числа не превратился в пол длиной в тысячелетия.
    const QByteArray raw = file.read(32).trimmed();
    file.close();
    bool ok = false;
    const qint64 value = raw.toLongLong(&ok);
    return ok && value > 0 ? value : 0;
}

void DeviceClock::advanceTo(qint64 time) const {
    if (time <= floor()) return;
    const QString path = pathFor(root_);
    QDir().mkpath(QFileInfo(path).absolutePath());
    // Атомарно: порванного числа не бывает по построению, а порванное число —
    // это пол в далёком будущем, от которого потом не избавиться.
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return;
    file.write(QByteArray::number(time));
    file.write("\n");
    file.commit();
}

}  // namespace zametti::store
