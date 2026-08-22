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

#include "device_clock.h"
#include "journal.h"

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
    QString pathOf(const QString& id) const {
        return dir_.filePath(id + QStringLiteral(".md"));
    }
    QString journalOf(const QString& id) const {
        return dir_.filePath(QStringLiteral("history/%1.log").arg(id));
    }

    // --- заметки и вложения ------------------------------------------------

    // Заметка с шапкой и телом. Возвращает те же байты, что легли в файл.
    QByteArray addNote(const QString& id, const QString& title, const QString& body,
                       const QString& parent = {}, bool archived = false) const {
        QByteArray out = "<!-- zametti\nversion: 1\n";
        if (!parent.isEmpty()) out += "parent: " + parent.toUtf8() + "\n";
        out += "created: 2026-01-01T00:00:00+03:00\n";
        out += "modified: 2026-01-02T00:00:00+03:00\n";
        if (archived) out += "archived: yes\n";
        out += "-->\n\n# " + title.toUtf8() + "\n";
        if (!body.isEmpty()) out += "\n" + body.toUtf8();
        write(pathOf(id), out);
        return out;
    }

    void addAttachment(const QString& name, const QByteArray& bytes) const {
        write(dir_.filePath(name), bytes);
    }

    // Время файла — им, среди прочего, датируется опорная запись журнала.
    void setFileTime(const QString& id, qint64 utcMs) const {
        QFile file(pathOf(id));
        file.open(QIODevice::ReadWrite);
        file.setFileTime(QDateTime::fromMSecsSinceEpoch(utcMs), QFileDevice::FileModificationTime);
        file.close();
    }

    // --- пол времени устройства --------------------------------------------

    void setDeviceClock(qint64 utcMs) const {
        write(zametti::store::DeviceClock::pathFor(root()), QByteArray::number(utcMs) + "\n");
    }
    void dropDeviceClock() const { QFile::remove(zametti::store::DeviceClock::pathFor(root())); }
    qint64 deviceClock() const { return zametti::store::DeviceClock(root()).floor(); }

    // --- журналы -----------------------------------------------------------

    // ЖУРНАЛ СТАРОГО ВИДА: записи без ревизии, такие, какие писала программа до
    // этапа 17. Другого способа изготовить их нет — нынешний писатель ревизию
    // ставит всегда, — а без них не проверить ни миграцию, ни правило «старые
    // записи сравниваются между собой по времени».
    //
    // Запись собирается здесь руками ровно по формату: карта с ключами
    // 1 род, 2 время, 3 отпечаток, 4 кодек, 5 размер до сжатия, 6 слепок.
    void appendLegacyRecord(const QString& id, zametti::journal::Kind kind, qint64 time,
                            const QByteArray& snapshot) const {
        const QString path = journalOf(id);
        QByteArray blob;
        if (!QFileInfo::exists(path))
            blob = zametti::journal::ZJournal::headerBytes(QStringLiteral("0.1"));

        const zametti::Digest digest =
            zametti::hashOf(std::string_view(snapshot.constData(), size_t(snapshot.size())));
        QByteArray packed(int(ZSTD_compressBound(size_t(snapshot.size()))), Qt::Uninitialized);
        const size_t got = ZSTD_compress(packed.data(), size_t(packed.size()), snapshot.constData(),
                                         size_t(snapshot.size()), 3);
        packed.resize(qsizetype(got));

        QByteArray record;
        QCborStreamWriter writer(&record);
        const bool tombstone = kind == zametti::journal::Kind::Tombstone;
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
        file.open(QIODevice::WriteOnly | QIODevice::Append);
        file.write(blob + record);
        file.close();
    }

    // Перевернуть бит в файле — порча, которую обязана заметить целостность.
    void flipByte(const QString& path, qint64 offset, unsigned char mask = 0x01) const {
        QFile file(path);
        file.open(QIODevice::ReadWrite);
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
        file.open(QIODevice::WriteOnly | QIODevice::Truncate);
        file.write(bytes);
        file.close();
    }

    QTemporaryDir tmp_;
    QDir dir_{tmp_.path()};
};

}  // namespace zt

#endif  // ZAMETTI_TESTS_MINI_STORE_H
