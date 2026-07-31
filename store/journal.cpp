#include "journal.h"

#include "zstd.h"

#include <QCborStreamReader>
#include <QCborStreamWriter>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

#include <cassert>
#include <cstring>

namespace zametti::journal {
namespace {

// Ключи карт — целые: короче текстовых и не зависят от языка. Номера навсегда;
// новое поле получает новый номер, старые не переиспользуются никогда.
enum Key {
    KeyMagic = 1,
    KeyVersion = 2,

    KeyKind = 1,
    KeyTime = 2,
    KeyDigest = 3,
    KeyCodec = 4,
    KeyPlainSize = 5,
    KeySnapshot = 6,
    KeySource = 7,
};

constexpr int kZstdLevel = 3;

// Шапка — первая запись файла, такая же CBOR-карта, как остальные. Отдельным
// «форматом заголовка» не делаем: один читатель на весь файл проще.
QByteArray headerBytes() {
    QByteArray out;
    QCborStreamWriter writer(&out);
    writer.startMap(2);
    writer.append(KeyMagic);
    writer.append(QLatin1StringView(kMagic));
    writer.append(KeyVersion);
    writer.append(kFormatVersion);
    writer.endMap();
    return out;
}

QByteArray recordBytes(Kind kind, qint64 time, const Digest& digest, const QByteArray& packed,
                       qint64 plainSize, qint64 source) {
    const bool tombstone = kind == Kind::Tombstone;
    const bool restore = kind == Kind::Restore;
    QByteArray out;
    QCborStreamWriter writer(&out);
    writer.startMap(quint64(3 + (tombstone ? 0 : 3) + (restore ? 1 : 0)));
    writer.append(KeyKind);
    writer.append(int(kind));
    writer.append(KeyTime);
    writer.append(time);
    writer.append(KeyDigest);
    writer.append(QByteArray(reinterpret_cast<const char*>(digest.bytes.data()),
                             qsizetype(digest.bytes.size())));
    if (!tombstone) {
        writer.append(KeyCodec);
        writer.append(int(Codec::Zstd));
        writer.append(KeyPlainSize);
        writer.append(plainSize);
        writer.append(KeySnapshot);
        writer.append(packed);
    }
    if (restore) {
        writer.append(KeySource);
        writer.append(source);
    }
    writer.endMap();
    return out;
}

QByteArray compressSnapshot(const QByteArray& plain) {
    QByteArray out(qsizetype(ZSTD_compressBound(size_t(plain.size()))), Qt::Uninitialized);
    const size_t size = ZSTD_compress(out.data(), size_t(out.size()), plain.constData(),
                                      size_t(plain.size()), kZstdLevel);
    if (ZSTD_isError(size)) return {};
    out.resize(qsizetype(size));
    return out;
}

bool decompressSnapshot(const QByteArray& packed, qint64 plainSize, QByteArray* out) {
    out->resize(qsizetype(plainSize));
    if (plainSize == 0) return packed.isEmpty() || ZSTD_getFrameContentSize(
                                                      packed.constData(), size_t(packed.size())) == 0;
    const size_t size = ZSTD_decompress(out->data(), size_t(out->size()), packed.constData(),
                                        size_t(packed.size()));
    return !ZSTD_isError(size) && qint64(size) == plainSize;
}

// Чтение одной карты. Незнакомые ключи пропускаются молча — это и есть
// обещание «неломающие изменения добавляют ключи».
//
// wantSnapshot: собрать байты слепка или пройти мимо, запомнив длину. Мимо —
// случай по умолчанию: таймлайну нужны времена, а не мегабайты, и достать
// один слепок из журнала на 16 МБ стоило 18.8 мс, пока сюда тащились все
// двести (после правки — 2.6 мс).
struct RawRecord {
    bool valid = false;
    Entry entry;
    QByteArray packed;
    QString magic;
    int version = 0;
    bool isHeader = false;
};

bool readValueAsInt(QCborStreamReader& reader, qint64* value) {
    if (!reader.isInteger()) return false;
    *value = reader.toInteger();
    reader.next();
    return true;
}

bool readValueAsBytes(QCborStreamReader& reader, QByteArray* value) {
    if (!reader.isByteArray()) return false;
    auto chunk = reader.readByteArray();
    QByteArray all;
    while (chunk.status == QCborStreamReader::Ok) {
        all += chunk.data;
        chunk = reader.readByteArray();
    }
    if (chunk.status == QCborStreamReader::Error) return false;
    *value = all;
    return true;
}

RawRecord readRecord(QCborStreamReader& reader, bool wantSnapshot) {
    RawRecord out;
    if (!reader.isMap()) return out;
    reader.enterContainer();
    while (reader.lastError() == QCborError::NoError && reader.hasNext()) {
        if (!reader.isInteger()) return out;  // ключ обязан быть целым
        const qint64 key = reader.toInteger();
        reader.next();
        switch (key) {
            case KeyKind:  // он же KeyMagic — различаем по типу значения
                if (reader.isString()) {
                    out.isHeader = true;
                    auto chunk = reader.readString();
                    QString all;
                    while (chunk.status == QCborStreamReader::Ok) {
                        all += chunk.data;
                        chunk = reader.readString();
                    }
                    if (chunk.status == QCborStreamReader::Error) return out;
                    out.magic = all;
                } else {
                    qint64 v = 0;
                    if (!readValueAsInt(reader, &v)) return out;
                    out.entry.kind = Kind(v);
                }
                break;
            case KeyTime: {  // он же KeyVersion
                qint64 v = 0;
                if (!readValueAsInt(reader, &v)) return out;
                out.entry.time = v;
                out.version = int(v);
                break;
            }
            case KeyDigest: {
                QByteArray bytes;
                if (!readValueAsBytes(reader, &bytes)) return out;
                if (bytes.size() != qsizetype(out.entry.digest.bytes.size())) return out;
                std::memcpy(out.entry.digest.bytes.data(), bytes.constData(),
                            size_t(bytes.size()));
                break;
            }
            case KeyCodec: {
                qint64 v = 0;
                if (!readValueAsInt(reader, &v)) return out;
                out.entry.codec = Codec(v);
                break;
            }
            case KeyPlainSize: {
                qint64 v = 0;
                if (!readValueAsInt(reader, &v)) return out;
                out.entry.plainSize = v;
                break;
            }
            case KeySnapshot: {
                if (!reader.isByteArray()) return out;
                if (wantSnapshot) {
                    if (!readValueAsBytes(reader, &out.packed)) return out;
                    out.entry.packedSize = out.packed.size();
                } else {
                    // Слепок пропускаем: у богатой заметки их сотни, и
                    // таймлайну нужны времена, а не мегабайты.
                    out.entry.packedSize = reader.length() > 0 ? qint64(reader.length()) : 0;
                    reader.next();
                }
                break;
            }
            case KeySource: {
                qint64 v = 0;
                if (!readValueAsInt(reader, &v)) return out;
                out.entry.source = v;
                break;
            }
            default:
                reader.next();  // незнакомый ключ — не наше дело
                break;
        }
    }
    if (reader.lastError() != QCborError::NoError) return out;
    if (!reader.leaveContainer()) return out;
    out.valid = true;
    return out;
}

QString describeKind(Kind kind) {
    switch (kind) {
        case Kind::Save: return QStringLiteral("save");
        case Kind::External: return QStringLiteral("external");
        case Kind::Restore: return QStringLiteral("restore");
        case Kind::Tombstone: return QStringLiteral("tombstone");
    }
    return QStringLiteral("?");
}

// Разбор всего файла из памяти. Возвращает число целых записей; хвост, который
// не разобрался, остаётся за границей goodBytes.
// Какие слепки собирать: ничей (таймлайн), один (переход к слепку) или все
// (прореживание переписывает файл целиком).
enum class Want { None, One, All };

bool parseAll(const QByteArray& blob, Journal* out, Want want, int wantIndex,
              QVector<QByteArray>* packed, QString* error) {
    out->entries.clear();
    out->tailTrimmed = false;
    out->goodBytes = 0;
    if (blob.isEmpty()) return true;  // пустой файл — пустой журнал, не беда

    // По читателю на запись. QCborStreamReader сам по последовательности
    // (RFC 8742) не идёт: дочитав первый элемент верхнего уровня, он считает
    // источник кончившимся — проверено пробником. Поэтому смещение ведём мы,
    // а читателя заводим заново от него; заодно это и есть та граница, по
    // которой отрезается оборванный хвост.
    qsizetype offset = 0;
    QCborStreamReader reader(blob.constData(), blob.size());
    const RawRecord header = readRecord(reader, false);
    if (!header.valid || !header.isHeader) {
        if (error) *error = QStringLiteral("не журнал zametti: нет шапки");
        return false;
    }
    if (header.magic != QLatin1StringView(kMagic)) {
        if (error) *error = QStringLiteral("не журнал zametti: чужая магия «%1»").arg(header.magic);
        return false;
    }
    if (header.version != kFormatVersion) {
        if (error)
            *error = QStringLiteral("журнал версии %1, а мы знаем только %2")
                         .arg(header.version)
                         .arg(kFormatVersion);
        return false;
    }
    offset = reader.currentOffset();
    out->goodBytes = qint64(offset);

    while (offset < blob.size()) {
        QCborStreamReader next(blob.constData() + offset, blob.size() - offset);
        const bool takeThis = want == Want::All ||
                              (want == Want::One && out->entries.size() == wantIndex);
        const RawRecord record = readRecord(next, takeThis);
        if (!record.valid || record.isHeader || next.currentOffset() == 0) {
            // Оборванный или испорченный хвост. Всё, что до него, — целое.
            out->tailTrimmed = true;
            break;
        }
        out->entries.append(record.entry);
        if (packed) packed->append(record.packed);
        offset += next.currentOffset();
        out->goodBytes = qint64(offset);
    }
    return true;
}

}  // namespace

QString journalPath(const QString& root, const QString& noteId) {
    return QDir(root).filePath(QStringLiteral("history/%1.log").arg(noteId));
}

bool append(const QString& path, Kind kind, qint64 time, const QByteArray& snapshot,
            qint64 source, QString* error) {
    const bool tombstone = kind == Kind::Tombstone;
    QByteArray packed;
    Digest digest;
    if (!tombstone) {
        digest = hashOf(std::string_view(snapshot.constData(), size_t(snapshot.size())));
        packed = compressSnapshot(snapshot);
        if (packed.isEmpty() && !snapshot.isEmpty()) {
            if (error) *error = QStringLiteral("не удалось сжать слепок");
            return false;
        }
    }

    QByteArray blob;
    QFile file(path);
    const bool fresh = !file.exists() || file.size() == 0;
    if (fresh) {
        QDir().mkpath(QFileInfo(path).absolutePath());
        blob = headerBytes();
    }
    blob += recordBytes(kind, time, digest, packed, snapshot.size(), source);

    if (!file.open(QIODevice::WriteOnly | QIODevice::Append)) {
        if (error) *error = QStringLiteral("журнал не открыть: %1").arg(file.errorString());
        return false;
    }
    const qint64 wrote = file.write(blob);
    // Закрываем без fsync — см. шапку журнала. Незаписавшийся хвост (диск
    // кончился) читатель отрежет сам, но сказать об этом надо сразу.
    file.close();
    if (wrote != blob.size()) {
        if (error) *error = QStringLiteral("журнал записан не целиком: %1 из %2 байт")
                                .arg(wrote)
                                .arg(blob.size());
        return false;
    }
    return true;
}

bool read(const QString& path, Journal* out, QString* error) {
    QFile file(path);
    if (!file.exists()) {
        *out = Journal{};
        return true;  // журнала ещё нет — это не беда, а «правок не было»
    }
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("журнал не прочитать: %1").arg(file.errorString());
        return false;
    }
    const QByteArray blob = file.readAll();
    file.close();
    return parseAll(blob, out, Want::None, -1, nullptr, error);
}

bool snapshotAt(const QString& path, int index, QByteArray* out, QString* error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("журнал не прочитать: %1").arg(file.errorString());
        return false;
    }
    const QByteArray blob = file.readAll();
    file.close();

    Journal journal;
    QVector<QByteArray> packed;
    if (!parseAll(blob, &journal, Want::One, index, &packed, error)) return false;
    if (index < 0 || index >= journal.entries.size()) {
        if (error) *error = QStringLiteral("в журнале нет записи №%1").arg(index);
        return false;
    }
    const Entry& entry = journal.entries[index];
    if (!entry.hasSnapshot()) {
        if (error)
            *error = QStringLiteral("у записи №%1 (%2) слепка нет")
                         .arg(index)
                         .arg(describeKind(entry.kind));
        return false;
    }
    if (entry.codec != Codec::Zstd && entry.codec != Codec::None) {
        if (error)
            *error = QStringLiteral("слепок записи №%1 сжат неизвестным кодеком %2")
                         .arg(index)
                         .arg(int(entry.codec));
        return false;
    }
    if (entry.codec == Codec::None) {
        *out = packed[index];
    } else if (!decompressSnapshot(packed[index], entry.plainSize, out)) {
        if (error) *error = QStringLiteral("слепок записи №%1 не распаковывается").arg(index);
        return false;
    }
    // Сверка всегда: история, молча отдающая не те байты, хуже отсутствующей.
    const Digest actual = hashOf(std::string_view(out->constData(), size_t(out->size())));
    if (actual != entry.digest) {
        if (error)
            *error = QStringLiteral("слепок записи №%1 не сходится с отпечатком").arg(index);
        return false;
    }
    return true;
}

bool trimTail(const QString& path, QString* error) {
    Journal journal;
    if (!read(path, &journal, error)) return false;
    if (!journal.tailTrimmed) return true;
    QFile file(path);
    if (!file.open(QIODevice::ReadWrite)) {
        if (error) *error = QStringLiteral("журнал не открыть: %1").arg(file.errorString());
        return false;
    }
    const bool ok = file.resize(journal.goodBytes);
    file.close();
    if (!ok && error) *error = QStringLiteral("хвост журнала не отрезать");
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

QVector<int> survivors(const QVector<Entry>& entries, qint64 now) {
    QVector<int> keep;
    for (int i = 0; i < entries.size(); ++i) {
        // Последняя запись остаётся всегда: для удалённой заметки это её
        // вечный финальный слепок и её tombstone.
        if (i + 1 == entries.size()) {
            keep.append(i);
            continue;
        }
        if (bucketOf(entries[i].time, now) != bucketOf(entries[i + 1].time, now)) keep.append(i);
    }
    return keep;
}

bool thin(const QString& path, qint64 now, QString* error) {
    QFile file(path);
    if (!file.exists()) return true;
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("журнал не прочитать: %1").arg(file.errorString());
        return false;
    }
    const QByteArray blob = file.readAll();
    file.close();

    Journal journal;
    QVector<QByteArray> packed;
    if (!parseAll(blob, &journal, Want::All, -1, &packed, error)) return false;
    const QVector<int> keep = survivors(journal.entries, now);
    if (keep.size() == journal.entries.size() && !journal.tailTrimmed) return true;  // нечего делать

    QByteArray out = headerBytes();
    for (int i : keep) {
        const Entry& e = journal.entries[i];
        out += recordBytes(e.kind, e.time, e.digest, packed[i], e.plainSize, e.source);
    }

    // Переписывание целиком и атомарно: журнал в промежуточном состоянии не
    // существует ни мгновения. Инвариант «только растёт» действует между
    // прореживаниями, и это единственное место, которое его нарушает.
    QSaveFile save(path);
    if (!save.open(QIODevice::WriteOnly)) {
        if (error) *error = QStringLiteral("журнал не переписать: %1").arg(save.errorString());
        return false;
    }
    save.write(out);
    if (!save.commit()) {
        if (error) *error = QStringLiteral("журнал не переписать: %1").arg(save.errorString());
        return false;
    }
    return true;
}

}  // namespace zametti::journal
