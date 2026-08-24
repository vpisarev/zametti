// БУХГАЛТЕРИЯ СИНХРОНИЗАЦИИ — КЭШ, НЕ ИСТИНА.
//
// Словарь «что я знал о блобе на момент последнего обмена»: etag последней
// операции, BLAKE3 залитого шифротекста, BLAKE3 локального файла журнала на
// тот момент — плюс (mtime, size) файлов заметок для stat-скана. По нему
// устойчивый синк стоит один листинг и ноль чтений содержимого.
//
// Файлов хранилища этот класс НЕ ЧИТАЕТ и не знает ни ZStorage, ни ZJournal,
// ни RemoteStore: все хеши считает движок, ledger лишь отвечает «что было
// записано» и запоминает «что стало». Ключи — имена блобов ("<id>.log"),
// не пути.
//
// Инвариант D (бриф m17): удаление этого файла не меняет результата синка —
// всё честно деградирует в «скачать и слить», идемпотентно. Поэтому битый или
// отсутствующий файл — не ошибка, а пустая бухгалтерия; поэтому же незнакомая
// версия формата не «отказ работать», а тот же пустой старт.
//
// Живёт в каталоге приложения (не в хранилище): бухгалтерия пер-девайсная и с
// копией каталога не путешествует. Имя файла — по (storeId + хеш локального
// пути), чтобы две копии одного хранилища на машине её не делили.

#ifndef ZAMETTI_SYNC_LEDGER_H
#define ZAMETTI_SYNC_LEDGER_H

#include "hash.h"

#include <QHash>
#include <QString>
#include <QStringList>

namespace zametti {

class SyncLedger {
public:
    static constexpr int kFormatVersion = 1;

    // Что помним о блобе. Пустой etag = «не знаем ничего».
    struct Blob {
        QString etag;       // метка сервера после последней операции
        Digest sealedHash;  // BLAKE3 шифротекста, который уехал или приехал
        Digest plainHash;   // BLAKE3 БАЙТОВ локального файла на тот момент
        bool isEmpty() const { return etag.isEmpty() && sealedHash.empty() && plainHash.empty(); }
    };
    // Подсказка stat-скана: какими файл заметки видели в последний раз.
    struct Stat {
        qint64 mtimeMs = 0;
        qint64 size = -1;
        bool isEmpty() const { return mtimeMs == 0 && size < 0; }
    };

    SyncLedger() = default;

    // Куда класть бухгалтерию этого хранилища на этой машине.
    static QString pathFor(const QString& storeId, const QString& storeRoot);

    // Прочитать. Отсутствие, порча и незнакомая версия — МОЛЧА пустая
    // бухгалтерия с тем же путём: кэш, не истина.
    static SyncLedger load(const QString& path);

    // Записать атомарно. false — и объяснение; для вызывающего это повод
    // сказать в лог, но не остановить синк: без бухгалтерии он лишь дороже.
    bool save(QString* error = nullptr) const;

    const QString& path() const { return path_; }

    Blob blob(const QString& name) const { return blobs_.value(name); }
    void setBlob(const QString& name, const Blob& state) { blobs_.insert(name, state); }
    void dropBlob(const QString& name) { blobs_.remove(name); }
    QStringList blobNames() const;

    Stat fileStat(const QString& noteId) const { return files_.value(noteId); }
    void setFileStat(const QString& noteId, const Stat& state) { files_.insert(noteId, state); }
    void dropFile(const QString& noteId) { files_.remove(noteId); }
    // Кого видели в прошлый раз — diff против readdir ловит удаления.
    QStringList knownFiles() const;

    // Чисто ли завершился прошлый заход. false при старте значит «после
    // нештатного завершения» — разовый stat-скан на любой платформе.
    bool cleanShutdown() const { return cleanShutdown_; }
    void setCleanShutdown(bool clean) { cleanShutdown_ = clean; }

    bool isEmpty() const { return blobs_.isEmpty() && files_.isEmpty(); }
    void clear() { blobs_.clear(); files_.clear(); }

protected:
    QString path_;
    QHash<QString, Blob> blobs_;
    QHash<QString, Stat> files_;
    bool cleanShutdown_ = true;
};

}  // namespace zametti

#endif  // ZAMETTI_SYNC_LEDGER_H
