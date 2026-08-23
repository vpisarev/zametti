// МИНИ-ХРАНИЛИЩЕ ДЛЯ НАБОРОВ: маленькое хранилище, собранное КОДОМ.
//
// Почему кодом, а не файлами в репозитории: корпуса у нас не коммитятся, а
// сценарии синхронизации требуют не «какого-нибудь» хранилища, а хранилища в
// точно заданном состоянии — с определёнными временами файлов, с журналами
// нужного вида, со стёртым или подменённым полом времени. Каждый набор берёт
// свежую копию во временном каталоге и приводит её к нужному виду руками.
//
// Прогон повторяется дословно: ни одного обращения к настоящим часам, ни
// одного случайного имени.

#ifndef ZAMETTI_TESTS_MINI_STORE_H
#define ZAMETTI_TESTS_MINI_STORE_H

#include "journal.h"
#include "zstorage.h"

#include <QByteArray>
#include <QCborStreamWriter>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QTemporaryDir>

#include "hash.h"
#include "zstd.h"

namespace zt {

class MiniStore {
public:
    MiniStore() {
        QDir().mkpath(dir_.filePath(QStringLiteral("history")));
        QDir().mkpath(dir_.filePath(QStringLiteral(".zametti")));
    }

    QString root() const { return dir_.path(); }
    QString journalOf(const QString& id) const {
        return dir_.filePath(QStringLiteral("history/%1.log").arg(id));
    }

    // --- пол времени устройства --------------------------------------------

    void setDeviceClock(qint64 utcMs) const {
        write(zametti::ZStorage(root()).deviceClockPath(), QByteArray::number(utcMs) + "\n");
    }
    void dropDeviceClock() const { QFile::remove(zametti::ZStorage(root()).deviceClockPath()); }
    qint64 deviceClock() const { return zametti::ZStorage(root()).deviceClockFloor(); }

    // --- журналы -----------------------------------------------------------

    // ЖУРНАЛ СТАРОГО ВИДА: записи без ревизии, такие, какие писала программа до
    // этапа 17. Другого способа изготовить их нет — нынешний писатель ревизию
    // ставит всегда, — а без них не проверить ни миграцию, ни правило «старые
    // записи сравниваются между собой по времени».
    //
    // Запись собирается здесь руками ровно по формату: карта с ключами
    // 1 род, 2 время, 3 отпечаток, 4 кодек, 5 размер до сжатия, 6 слепок.
    void appendLegacyRecord(const QString& id, zametti::ZJournal::Kind kind, qint64 time,
                            const QByteArray& snapshot) const {
        const QString path = journalOf(id);
        QByteArray blob;
        if (!QFileInfo::exists(path))
            blob = zametti::ZJournal::headerBytes(QStringLiteral("0.1"));

        const zametti::Digest digest =
            zametti::hashOf(std::string_view(snapshot.constData(), size_t(snapshot.size())));
        QByteArray packed(int(ZSTD_compressBound(size_t(snapshot.size()))), Qt::Uninitialized);
        const size_t got = ZSTD_compress(packed.data(), size_t(packed.size()), snapshot.constData(),
                                         size_t(snapshot.size()), 3);
        packed.resize(qsizetype(got));

        QByteArray record;
        QCborStreamWriter writer(&record);
        const bool tombstone = kind == zametti::ZJournal::Kind::Tombstone;
        writer.startMap(quint64(tombstone ? 3 : 6));
        writer.append(1);
        writer.append(int(kind));
        writer.append(2);
        writer.append(time);
        writer.append(3);
        writer.append(QByteArray(reinterpret_cast<const char*>(digest.bytes.data()),
                                 qsizetype(digest.bytes.size())));
        if (!tombstone) {
            writer.append(4);
            writer.append(1);   // Codec::Zstd — полный слепок
            writer.append(5);
            writer.append(qint64(snapshot.size()));
            writer.append(6);
            writer.append(packed);
        }
        writer.endMap();

        QFile file(path);
        (void)file.open(QIODevice::WriteOnly | QIODevice::Append);
        file.write(blob + record);
        file.close();
    }

    // Перевернуть бит в файле — порча, которую обязана заметить целостность.
    void flipByte(const QString& path, qint64 offset, unsigned char mask = 0x01) const {
        QFile file(path);
        (void)file.open(QIODevice::ReadWrite);
        QByteArray all = file.readAll();
        if (offset >= 0 && offset < all.size()) all[offset] = char(all[offset] ^ mask);
        file.seek(0);
        file.write(all);
        file.close();
    }

protected:
    static void write(const QString& path, const QByteArray& bytes) {
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile file(path);
        (void)file.open(QIODevice::WriteOnly | QIODevice::Truncate);
        file.write(bytes);
        file.close();
    }

    QTemporaryDir tmp_;
    QDir dir_{tmp_.path()};
};

}  // namespace zt

#endif  // ZAMETTI_TESTS_MINI_STORE_H
