// Пол времени записей устройства — `.zametti/last-written`. Что это и зачем —
// в zstorage.h, у объявления deviceClockFloor.

#include "zstorage.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

namespace zametti {

QString ZStorage::deviceClockPath() const {
    return QDir(root_).filePath(QStringLiteral(".zametti/last-written"));
}

qint64 ZStorage::deviceClockFloor() const {
    QFile file(deviceClockPath());
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

void ZStorage::advanceDeviceClock(qint64 time) const {
    if (time <= deviceClockFloor()) return;
    const QString path = deviceClockPath();
    QDir().mkpath(QFileInfo(path).absolutePath());
    // ОБЫЧНАЯ ЗАПИСЬ, БЕЗ FSYNC — и это замер, а не вкус. QSaveFile внутри
    // себя зовёт fdatasync, и на каждом автосохранении он стоил 5 мс из 13
    // (замер на Release, ext4): столько же, сколько вся запись самой заметки.
    // Журнал fsync не зовёт по той же причине и с тем же обоснованием.
    //
    // Порванного числа при этом не бывает: четырнадцать байт ложатся одной
    // записью в один сектор, а сектор попадает на диск целиком — либо старый,
    // либо новый. Потерять обновление можно (питание выдернули до сброса
    // страниц), и это штатная деградация: пол опустится до пола журнала, а тот
    // выводится из записей и потеряться не может.
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return;
    file.write(QByteArray::number(time));
    file.write("\n");
    file.close();
}

}  // namespace zametti
