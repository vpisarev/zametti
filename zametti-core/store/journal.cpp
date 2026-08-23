#include "journal.h"

#include "zstd.h"

#include <QCborStreamReader>
#include <QCborStreamWriter>
#include <QDateTime>
#include <QDir>
#include <QMutex>
#include <QMutexLocker>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

#include <algorithm>
#include <cassert>
#include <cstring>
#include <limits>
#include <utility>

namespace zametti::journal {
namespace {

// Тот самый единый замок (см. journal.h). Один на все журналы и на все
// экземпляры History: история у хранилища одна.
QMutex& gate() {
    static QMutex mutex;
    return mutex;
}

// Обещание «закрытые методы зовутся только из-под замка» — не слова, а
// проверка. tryLock на уже взятом мьютексе не проходит; прошёл — значит замка
// не было, и это ошибка вызова, а не случайность. В релизе не стоит ничего.
void assertLocked() {
#ifndef NDEBUG
    if (gate().tryLock()) {
        gate().unlock();
        assert(false && "history operation called without the lock");
    }
#endif
}

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

    // Версия содержимого (шапка). Номер взят СВОБОДНЫЙ, а не 3: читатель у
    // шапки и у записи один, и ключи их живут в общем пространстве. Совпасть с
    // KeyDigest значило бы различать их по типу значения — так уже сделано у
    // пары Magic/Kind, и хватит: каждая такая пара это ловушка для того, кто
    // добавит поле следующим.
    KeyClean = 8,

    // Ревизия записи (счётчик Лампорта). Нового номера формата не требует:
    // читатель незнакомые ключи пропускает молча — обещание с этапа 7, — и
    // старая сборка такой журнал по-прежнему прочтёт, просто не увидит
    // ревизий.
    KeySeq = 9,

    // Список погашенных записей: массив пар [время, отпечаток]. Пусто — ключа
    // нет вовсе, и это подавляющее большинство записей.
    KeyVoids = 10,

    // Контрольная сумма рамки: BLAKE3, первые 16 байт. Нет ключа — запись
    // написана до этапа 17, и проверять нечего.
    KeyFrameHash = 11,
};

// Сколько байт отпечатка кладём в сумму рамки. Шестнадцать: рамка живёт рядом
// со своим слепком, у которого полные 32, и удваивать эту цену незачем — 128
// бит хватит, чтобы случайная порча не совпала никогда.
constexpr int kFrameHashBytes = 16;

constexpr int kZstdLevel = 3;

// Окно должно накрывать предшественника вместе с новым слепком, иначе
// совпадений в нём не найти. Считаем по месту, а не берём с запасом: окно —
// это ещё и память, которую займёт сжатие.
int windowLogFor(qint64 bytes) {
    int log = 10;
    while ((qint64(1) << log) < bytes && log < 27) ++log;
    return log;
}

// base пуст — полный слепок; иначе звено цепочки. Механизм тот же, что у
// `zstd --patch-from`: предшественник объявляется префиксом, и всё, что в нём
// повторяется, уходит в ссылки.
QByteArray compressSnapshot(const QByteArray& plain, const QByteArray& base) {
    QByteArray out(qsizetype(ZSTD_compressBound(size_t(plain.size()))), Qt::Uninitialized);
    size_t size = 0;
    if (base.isEmpty()) {
        size = ZSTD_compress(out.data(), size_t(out.size()), plain.constData(),
                             size_t(plain.size()), kZstdLevel);
    } else {
        ZSTD_CCtx* ctx = ZSTD_createCCtx();
        ZSTD_CCtx_setParameter(ctx, ZSTD_c_compressionLevel, kZstdLevel);
        ZSTD_CCtx_setParameter(ctx, ZSTD_c_windowLog,
                               windowLogFor(base.size() + plain.size()));
        ZSTD_CCtx_refPrefix(ctx, base.constData(), size_t(base.size()));
        size = ZSTD_compress2(ctx, out.data(), size_t(out.size()), plain.constData(),
                              size_t(plain.size()));
        ZSTD_freeCCtx(ctx);
    }
    if (ZSTD_isError(size)) return {};
    out.resize(qsizetype(size));
    return out;
}

bool decompressSnapshot(const QByteArray& packed, const QByteArray& base, qint64 plainSize,
                        QByteArray* out) {
    out->resize(qsizetype(plainSize));
    if (plainSize == 0)
        return packed.isEmpty() ||
               ZSTD_getFrameContentSize(packed.constData(), size_t(packed.size())) == 0;
    // Предел окна на распаковке не трогаем: по умолчанию он 27, то есть не
    // меньше любого, каким мы сжимаем. Однажды я поставил его «по месту», и
    // полные слепки перестали распаковываться — окно вышло уже, чем при сжатии.
    ZSTD_DCtx* ctx = ZSTD_createDCtx();
    if (!base.isEmpty()) ZSTD_DCtx_refPrefix(ctx, base.constData(), size_t(base.size()));
    const size_t size = ZSTD_decompressDCtx(ctx, out->data(), size_t(out->size()),
                                            packed.constData(), size_t(packed.size()));
    ZSTD_freeDCtx(ctx);
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
    // Разобралась, но сумма рамки не сошлась: запись есть, верить ей нельзя.
    bool damaged = false;
    bool valid = false;
    Entry entry;
    QByteArray packed;
    QString magic;
    int version = 0;
    QString clean;
    bool isHeader = false;
};

// Строка CBOR читается кусками — она может быть разбита на части.
bool readValueAsString(QCborStreamReader& reader, QString* value) {
    if (!reader.isString()) return false;
    auto chunk = reader.readString();
    QString all;
    while (chunk.status == QCborStreamReader::Ok) {
        all += chunk.data;
        chunk = reader.readString();
    }
    if (chunk.status == QCborStreamReader::Error) return false;
    *value = all;
    return true;
}

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
    // Значения копятся в локальных: ключи в карте идут в любом порядке, а
    // рамка записи неизменяема — она рождается один раз, когда прочитано всё.
    Kind kind = Kind::Save;
    qint64 time = 0;
    qint64 seq = 0;
    Digest digest;
    Codec codec = Codec::Zstd;
    qint64 plainSize = 0;
    qint64 packedSize = 0;
    qint64 source = 0;
    QByteArray frameHash;
    QVector<EntryRef> voids;
    reader.enterContainer();
    while (reader.lastError() == QCborError::NoError && reader.hasNext()) {
        if (!reader.isInteger()) return out;  // ключ обязан быть целым
        const qint64 key = reader.toInteger();
        reader.next();
        switch (key) {
            case KeyKind:  // он же KeyMagic — различаем по типу значения
                if (reader.isString()) {
                    out.isHeader = true;
                    if (!readValueAsString(reader, &out.magic)) return out;
                } else {
                    qint64 v = 0;
                    if (!readValueAsInt(reader, &v)) return out;
                    kind = Kind(v);
                }
                break;
            case KeyTime: {  // он же KeyVersion
                qint64 v = 0;
                if (!readValueAsInt(reader, &v)) return out;
                time = v;
                out.version = int(v);
                break;
            }
            case KeyDigest: {
                QByteArray bytes;
                if (!readValueAsBytes(reader, &bytes)) return out;
                if (bytes.size() != qsizetype(digest.bytes.size())) return out;
                std::memcpy(digest.bytes.data(), bytes.constData(), size_t(bytes.size()));
                break;
            }
            case KeyCodec: {
                qint64 v = 0;
                if (!readValueAsInt(reader, &v)) return out;
                codec = Codec(v);
                break;
            }
            case KeyPlainSize: {
                qint64 v = 0;
                if (!readValueAsInt(reader, &v)) return out;
                plainSize = v;
                break;
            }
            case KeySnapshot: {
                if (!reader.isByteArray()) return out;
                if (wantSnapshot) {
                    if (!readValueAsBytes(reader, &out.packed)) return out;
                    packedSize = out.packed.size();
                } else {
                    // Слепок пропускаем: у богатой заметки их сотни, и
                    // таймлайну нужны времена, а не мегабайты.
                    packedSize = reader.length() > 0 ? qint64(reader.length()) : 0;
                    reader.next();
                }
                break;
            }
            case KeySource: {
                qint64 v = 0;
                if (!readValueAsInt(reader, &v)) return out;
                source = v;
                break;
            }
            case KeySeq: {
                qint64 v = 0;
                if (!readValueAsInt(reader, &v)) return out;
                seq = v;
                break;
            }
            case KeyFrameHash: {
                if (!readValueAsBytes(reader, &frameHash)) return out;
                break;
            }
            case KeyVoids: {
                if (!reader.isArray()) return out;
                reader.enterContainer();
                while (reader.lastError() == QCborError::NoError && reader.hasNext()) {
                    if (!reader.isArray()) return out;
                    reader.enterContainer();
                    qint64 when = 0;
                    QByteArray bytes;
                    if (!readValueAsInt(reader, &when)) return out;
                    if (!readValueAsBytes(reader, &bytes)) return out;
                    Digest ref;
                    if (bytes.size() != qsizetype(ref.bytes.size())) return out;
                    std::memcpy(ref.bytes.data(), bytes.constData(), size_t(bytes.size()));
                    if (!reader.leaveContainer()) return out;
                    voids.append(EntryRef(when, ref));
                }
                if (reader.lastError() != QCborError::NoError) return out;
                if (!reader.leaveContainer()) return out;
                break;
            }
            case KeyClean:
                if (!readValueAsString(reader, &out.clean)) return out;
                break;
            default:
                reader.next();  // незнакомый ключ — не наше дело
                break;
        }
    }
    if (reader.lastError() != QCborError::NoError) return out;
    if (!reader.leaveContainer()) return out;
    out.entry = Entry(kind, time, seq, digest, source, std::move(voids));
    out.entry.layAs(codec, plainSize);
    out.entry.placeAt(0, packedSize);   // смещение проставит разбор всей ленты

    // ЦЕЛОСТНОСТЬ РАМКИ. Нет ключа — запись написана до этапа 17, и проверять
    // нечего; это единственная законная ветка «без суммы». Не сошлась — запись
    // невалидна, и разбор обходится с ней как с оборванным хвостом: молча
    // отдавать не тот род или не то время хуже, чем не отдать ничего.
    if (!frameHash.isEmpty()) {
        const QByteArray frame = ZJournal::frameBytes(out.entry);
        const Digest sum = hashOf(std::string_view(frame.constData(), size_t(frame.size())));
        if (frameHash != QByteArray(reinterpret_cast<const char*>(sum.bytes.data()),
                                    kFrameHashBytes))
            out.damaged = true;
    }
    out.valid = true;
    return out;
}

// Подменить журнал целиком и атомарно. Промежуточного состояния у файла не
// существует ни мгновения.
bool replaceFile(const QString& path, const QByteArray& bytes, QString* error) {
    QSaveFile save(path);
    if (!save.open(QIODevice::WriteOnly)) {
        if (error) *error = QStringLiteral("cannot rewrite journal: %1").arg(save.errorString());
        return false;
    }
    save.write(bytes);
    if (!save.commit()) {
        if (error) *error = QStringLiteral("cannot rewrite journal: %1").arg(save.errorString());
        return false;
    }
    return true;
}

QString describeKind(Kind kind) {
    switch (kind) {
        case Kind::Save: return QStringLiteral("save");
        case Kind::External: return QStringLiteral("external");
        case Kind::Restore: return QStringLiteral("restore");
        case Kind::Tombstone: return QStringLiteral("tombstone");
        case Kind::Amendment: return QStringLiteral("amendment");
    }
    return QStringLiteral("?");
}

// Разбор всего файла из памяти. Возвращает число целых записей; хвост, который
// не разобрался, остаётся за границей goodBytes.
// Какие слепки собирать: ничьих (таймлайн), поколение до нужной записи
// (переход к слепку, дозапись) или все (прореживание переписывает файл
// целиком). Поколение держится скользящим окном: встретив полный слепок,
// прежде собранное выбрасываем — память ограничена одним поколением, а не
// длиной журнала.
}  // namespace

QByteArray ZJournal::headerBytes(const QString& clean) {
    QByteArray out;
    QCborStreamWriter writer(&out);
    writer.startMap(clean.isEmpty() ? 2 : 3);
    writer.append(KeyMagic);
    writer.append(QLatin1StringView(kMagic));
    writer.append(KeyVersion);
    writer.append(kFormatVersion);
    if (!clean.isEmpty()) {
        writer.append(KeyClean);
        writer.append(clean);
    }
    writer.endMap();
    return out;
}


// Байты одной записи. Рамка приходит ЦЕЛИКОМ, отдельными параметрами не
// разбирается: у записи два писателя — дозапись и пересборка файла, — и
// поле, добавленное в рамку, обязано попасть в оба. Со списком параметров
// это держалось на внимательности (забыл в пересборке — прореживание молча
// обнуляет поле во всём журнале); с Entry оно попадает туда по построению.
//
// packed — сжатый слепок, отдельно: он не свойство рамки, а её содержимое, и
// при пересборке он другой, чем был в файле, хотя рамка та же.
QByteArray ZJournal::frameBytes(const Entry& e) {
    // Ширины фиксированные и порядок жёсткий: сумма обязана считаться
    // одинаково на любой машине, поэтому ни QDataStream с его версиями, ни
    // порядок байтов машины здесь не участвуют.
    QByteArray out("zframe1");
    const auto put64 = [&out](qint64 v) {
        for (int i = 0; i < 8; ++i) out.append(char((quint64(v) >> (8 * i)) & 0xff));
    };
    const auto putDigest = [&out](const Digest& d) {
        out.append(reinterpret_cast<const char*>(d.bytes.data()), qsizetype(d.bytes.size()));
    };
    out.append(char(int(e.kind())));
    put64(e.time());
    put64(e.seq());
    put64(e.source());
    out.append(char(int(e.codec())));
    put64(e.plainSize());
    putDigest(e.digest());
    put64(e.voids().size());
    for (const EntryRef& ref : e.voids()) {
        put64(ref.time());
        putDigest(ref.digest());
    }
    return out;
}

QByteArray ZJournal::recordBytes(const Entry& e, const QByteArray& packed) {
    const bool bodyless = !e.hasSnapshot();   // надгробие и гашение
    const bool restore = e.kind() == Kind::Restore;
    // Нулевую ревизию не пишем вовсе: «нет ключа = 0» — уже принятый в этом
    // файле уговор (так же читается отсутствующий KeyClean). Плата за это не
    // два байта, а обещание идемпотентности: журнал, написанный до этапа 17,
    // после перекодировки прореживанием остаётся байт в байт прежним.
    const bool hasSeq = e.seq() != 0;
    const bool hasVoids = !e.voids().isEmpty();
    QByteArray out;
    QCborStreamWriter writer(&out);
    writer.startMap(quint64(4 + (bodyless ? 0 : 3) + (restore ? 1 : 0) + (hasSeq ? 1 : 0) +
                            (hasVoids ? 1 : 0)));
    writer.append(KeyKind);
    writer.append(int(e.kind()));
    writer.append(KeyTime);
    writer.append(e.time());
    if (hasSeq) {
        writer.append(KeySeq);
        writer.append(e.seq());
    }
    writer.append(KeyDigest);
    writer.append(QByteArray(reinterpret_cast<const char*>(e.digest().bytes.data()),
                             qsizetype(e.digest().bytes.size())));
    if (!bodyless) {
        writer.append(KeyCodec);
        writer.append(int(e.codec()));
        writer.append(KeyPlainSize);
        writer.append(e.plainSize());
        writer.append(KeySnapshot);
        writer.append(packed);
    }
    if (restore) {
        writer.append(KeySource);
        writer.append(e.source());
    }
    if (hasVoids) {
        writer.append(KeyVoids);
        writer.startArray(quint64(e.voids().size()));
        for (const EntryRef& ref : e.voids()) {
            writer.startArray(2);
            writer.append(ref.time());
            writer.append(QByteArray(reinterpret_cast<const char*>(ref.digest().bytes.data()),
                                     qsizetype(ref.digest().bytes.size())));
            writer.endArray();
        }
        writer.endArray();
    }
    {
        const QByteArray frame = frameBytes(e);
        const Digest sum = hashOf(std::string_view(frame.constData(), size_t(frame.size())));
        writer.append(KeyFrameHash);
        writer.append(QByteArray(reinterpret_cast<const char*>(sum.bytes.data()),
                                 kFrameHashBytes));
    }
    writer.endMap();
    return out;
}


bool ZJournal::parse(const QByteArray& blob, Want want, int wantIndex, QString* error) {
    ZJournal* const out = this;
    QVector<QByteArray>* const packed = &packed_;
    entries_.clear();
    packed_.clear();
    damaged_.clear();
    tailTrimmed_ = false;
    goodBytes_ = 0;
    cleanVersion_.clear();
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
        if (error) *error = QStringLiteral("not a zametti journal: no header");
        return false;
    }
    if (header.magic != QLatin1StringView(kMagic)) {
        if (error) *error = QStringLiteral("not a zametti journal: foreign magic '%1'").arg(header.magic);
        return false;
    }
    if (header.version != kFormatVersion) {
        if (error)
            *error = QStringLiteral("journal version %1, but we only know %2")
                         .arg(header.version)
                         .arg(kFormatVersion);
        return false;
    }
    out->cleanVersion_ = header.clean;
    offset = reader.currentOffset();
    out->goodBytes_ = qint64(offset);

    while (offset < blob.size()) {
        QCborStreamReader next(blob.constData() + offset, blob.size() - offset);
        const bool takeThis =
            want == Want::All || (want == Want::Chain && out->entries_.size() <= wantIndex);
        const RawRecord record = readRecord(next, takeThis);
        if (!record.valid || record.isHeader || next.currentOffset() == 0) {
            // Оборванный или испорченный хвост. Всё, что до него, — целое.
            out->tailTrimmed_ = true;
            break;
        }
        // Скользящее окно чистит собранное, только пока нужное поколение ещё
        // не набрано: иначе полный слепок, встреченный ПОСЛЕ него, выбросил бы
        // как раз то, ради чего читали.
        if (want == Want::Chain && packed && record.entry.full() &&
            out->entries_.size() <= wantIndex)
            for (QByteArray& old : *packed) old.clear();
        if (record.damaged) out->damaged_.append(int(out->entries_.size()));
        out->entries_.append(record.entry);
        Entry& placed = out->entries_.last();
        placed.placeAt(qint64(offset), placed.packedSize());
        if (packed) packed->append(record.packed);
        offset += next.currentOffset();
        out->goodBytes_ = qint64(offset);
    }
    return true;
}


// Собрать распакованный слепок записи index, идя от начала её поколения.
// packed выровнен по entries; звенья своего поколения в нём есть.
// eachLink — сверять отпечаток на КАЖДОМ звене. По умолчанию нет: сверка стоит
// 210 мкс на звено против 4.7 мкс на саму распаковку (замер на заметке в
// 239 КБ), то есть на цепочке в 31 звено это 98% всей цены. И она избыточна:
// порча любого звена меняет итоговые байты, а отпечаток последнего мы сверяем
// всегда. Посленинная сверка нужна ровно для одного — назвать, какое именно
// звено испорчено, — и включается она только тогда, когда итог уже не сошёлся.
bool ZJournal::rebuildAt(int index, QByteArray* out, QString* error, bool eachLink) const {
    const QVector<Entry>& entries = entries_;
    const QVector<QByteArray>& packed = packed_;
    // Испорченной рамке верить нельзя ни в чём — ни ей самой, ни звеньям,
    // которые на неё опираются.
    if (isDamaged(index)) {
        if (error)
            *error = QStringLiteral("record #%1: the frame checksum does not match").arg(index);
        return false;
    }
    int base = index;
    while (base > 0 && !entries[base].full()) --base;
    if (!entries[base].full()) {
        if (error)
            *error = QStringLiteral("generation of record #%1 does not start with a full snapshot")
                         .arg(index);
        return false;
    }
    QByteArray current;
    for (int i = base; i <= index; ++i) {
        const Entry& e = entries[i];
        if (isDamaged(i)) {
            if (error)
                *error = QStringLiteral("record #%1: the frame checksum does not match").arg(i);
            return false;
        }
        if (!e.hasSnapshot()) continue;  // надгробие цепочку не рвёт: у него слепка нет
        if (e.codec() != Codec::Zstd && e.codec() != Codec::None && e.codec() != Codec::ZstdDelta) {
            if (error)
                *error = QStringLiteral("snapshot of record #%1 compressed with unknown codec %2")
                             .arg(i)
                             .arg(int(e.codec()));
            return false;
        }
        QByteArray plain;
        if (e.codec() == Codec::None) {
            plain = packed[i];
        } else if (!decompressSnapshot(packed[i], e.full() ? QByteArray() : current, e.plainSize(),
                                       &plain)) {
            if (error) *error = QStringLiteral("snapshot of record #%1 does not decompress").arg(i);
            return false;
        }
        if (eachLink || i == index) {
            const Digest actual =
                hashOf(std::string_view(plain.constData(), size_t(plain.size())));
            if (actual != e.digest()) {
                if (error)
                    *error = QStringLiteral("snapshot of record #%1: hash mismatch").arg(i);
                // Итог не сошёлся — теперь стоит пройти цепочку с проверкой
                // каждого звена и назвать то, с которого всё пошло не так.
                if (!eachLink) {
                    QByteArray ignored;
                    QString which;
                    if (!rebuildAt(index, &ignored, &which, true) &&
                        error != nullptr)
                        *error = which;
                }
                return false;
            }
        }
        // Перекладываем, а не копируем: на цепочке в 31 звено копия слепка в
        // четверть мегабайта каждый раз — половина оставшейся цены.
        current = std::move(plain);
    }
    *out = current;
    return true;
}


// Собрать журнал заново из выживших записей. Общая часть прореживания и
// чистки: обе переписывают файл целиком, и держать это двумя кусками кода —
// прямой путь к тому, что поколения у них однажды разъедутся.
//
// Выжившие перекодируются с нуля: звено цепочки осмысленно только рядом со
// своим предшественником, а он мог не выжить.
bool ZJournal::toBytes(const QVector<int>& keep, const QString& clean, QByteArray* out,
                       QString* error) const {
    const QVector<Entry>& entries = entries_;
    *out = headerBytes(clean);
    QByteArray previous;
    int written = 0;
    for (int i : keep) {
        const Entry& e = entries[i];
        if (!e.hasSnapshot()) {
            // Надгробие поколения не начинает и не рвёт: слепка у него нет.
            Entry frame = e;
            frame.layAs(Codec::Zstd, 0);
            *out += recordBytes(frame, QByteArray());
            continue;
        }
        QByteArray plain;
        if (!rebuildAt(i, &plain, error)) return false;
        const bool full = written % kGeneration == 0;
        const Codec codec = codecFor(full);
        const QByteArray body = compressSnapshot(plain, full ? QByteArray() : previous);
        if (body.isEmpty() && !plain.isEmpty()) {
            if (error) *error = QStringLiteral("cannot compress snapshot of record #%1").arg(i);
            return false;
        }
        // Меняется только укладка: кодек и размер до сжатия. Всё остальное —
        // род, время, отпечаток, source и любое будущее поле рамки — едет из
        // прежней записи неприкосновенным.
        Entry frame = e;
        frame.layAs(codec, plain.size());
        *out += recordBytes(frame, body);
        previous = plain;
        ++written;
    }
    return true;
}





// Шапка — первая запись файла, такая же CBOR-карта, как остальные. Отдельным
// «форматом заголовка» не делаем: один читатель на весь файл проще.
//
// clean — по какому своду правил журнал вычищен; пусто — ключа в шапке нет
// вовсе, и это v0. Новый журнал заводится сразу чистым: он пишется нынешними
// правилами, чистить в нём нечего по построению.
History::History(QString root) : root_(std::move(root)), clock_(root_) {}

bool Entry::isBefore(const Entry& other) const {
    if (seq_ != other.seq_) return seq_ < other.seq_;
    if (time_ != other.time_) return time_ < other.time_;
    // Контент старше надгробия: при равном ключе правка побеждает удаление.
    if (hasSnapshot() != other.hasSnapshot()) return hasSnapshot();
    return std::lexicographical_compare(digest_.bytes.begin(), digest_.bytes.end(),
                                        other.digest_.bytes.begin(), other.digest_.bytes.end());
}

qint64 ZJournal::latestTime() const {
    qint64 latest = 0;
    for (const Entry& e : entries_) latest = qMax(latest, e.time());
    return latest;
}

Stamp Stamp::now() { return Stamp(QDateTime::currentMSecsSinceEpoch(), true); }

qint64 ZJournal::stampFor(Stamp when, qint64 deviceFloor) const {
    qint64 stamp = when.requested();
    if (when.guarded() && deviceFloor > 0) stamp = qMax(stamp, deviceFloor + 1);
    const qint64 own = latestTime();
    if (own > 0) stamp = qMax(stamp, own + 1);
    return stamp;
}

bool ZJournal::composeRecord(const NewRecord& what, qint64 deviceFloor, Entry* frame,
                             QByteArray* bytes, QString* error) const {
    const Kind kind = what.kind();
    const QByteArray& snapshot = what.snapshot();
    // Слепка нет у надгробия и у гашения; всё, что ниже, спрашивает про это
    // одинаково.
    const bool bodyless = kind == Kind::Tombstone || kind == Kind::Amendment;
    const qint64 time = stampFor(what.when(), deviceFloor);

    // Предшественник, относительно которого сожмётся слепок. Пусто — запись
    // начинает новое поколение и ложится полным слепком.
    QByteArray base;
    Digest digest;
    if (!bodyless) {
        digest = hashOf(std::string_view(snapshot.constData(), size_t(snapshot.size())));

        const int last = lastInFileWithSnapshot();
        if (last >= 0) {
            int from = last;
            while (from > 0 && !entries_[from].full()) --from;
            const int inGeneration = last - from + 1;
            if (inGeneration < kGeneration) {
                QString why;
                // Не собрался предшественник — начинаем новое поколение. Это
                // строго лучше отказа: беда в старом поколении не мешает
                // писать новую историю.
                if (!rebuildAt(last, &base, &why)) base.clear();
            }
        }
    }

    QByteArray body;
    if (!bodyless) {
        body = compressSnapshot(snapshot, base);
        if (body.isEmpty() && !snapshot.isEmpty()) {
            if (error) *error = QStringLiteral("cannot compress snapshot");
            return false;
        }
    }

    // Ревизия — свойство журнала: ни архив, ни удаление, ни редактор про неё
    // знать не должны, и выбирать её обязано одно место.
    Entry made(kind, time, nextSeq(), digest, what.source(), what.voids());
    made.layAs(codecFor(base.isEmpty()), snapshot.size());
    *bytes = recordBytes(made, body);
    if (frame != nullptr) *frame = made;
    return true;
}

// Последняя ПО ФАЙЛУ запись со слепком. Это не голова: дельта осмысленна
// только относительно того, что физически лежит рядом, поэтому здесь обход
// позиционный и другим быть не может.
int ZJournal::lastInFileWithSnapshot() const {
    int last = int(entries_.size()) - 1;
    while (last >= 0 && !entries_[last].hasSnapshot()) --last;
    return last;
}

bool ZJournal::isVoided(int index) const {
    const Entry& target = entries_[index];
    for (const Entry& e : entries_)
        if (e.voidsEntry(target)) return true;
    return false;
}

QVector<int> ZJournal::indexesOf(const QVector<EntryRef>& refs) const {
    QVector<int> out;
    for (int i = 0; i < entries_.size(); ++i)
        for (const EntryRef& ref : refs)
            if (entries_[i].isAddressedBy(ref.time(), ref.digest())) {
                out.append(i);
                break;
            }
    return out;
}

int ZJournal::headIndex() const {
    int best = -1;
    for (int i = 0; i < entries_.size(); ++i) {
        // Гашение головой быть не может: оно ничего не говорит о содержимом, а
        // погашенная запись не в счёт вовсе.
        if (!entries_[i].statesContent() || isVoided(i) || isDamaged(i)) continue;
        if (best < 0 || entries_[best].isBefore(entries_[i])) best = i;
    }
    return best;
}

int ZJournal::lastSnapshotIndex() const {
    int best = -1;
    for (int i = 0; i < entries_.size(); ++i) {
        if (!entries_[i].hasSnapshot() || isVoided(i) || isDamaged(i)) continue;
        if (best < 0 || entries_[best].isBefore(entries_[i])) best = i;
    }
    return best;
}

int ZJournal::previousSnapshotIndex(int from) const {
    int at = from - 1;
    while (at >= 0 && (!entries_[at].hasSnapshot() || isVoided(at) || isDamaged(at))) --at;
    return at;
}

qint64 ZJournal::nextSeq() const {
    // Края закрыты сами собой: пустой журнал даёт 1; рваный хвост к этому
    // моменту уже отрезан, и запись, которой не существовало, свой номер не
    // резервирует; приехавший с чужого устройства журнал с большими номерами
    // просто продолжается с них; дыры в нумерации безразличны. Надгробие
    // участвует в максимуме наравне с прочими — иначе правка поверх удаления
    // получила бы номер МЕНЬШЕ надгробия и проиграла бы ему, а правка обязана
    // побеждать.
    qint64 seq = 0;
    for (const Entry& e : entries_) seq = qMax(seq, e.seq());
    return seq + 1;
}

int ZJournal::indexOf(qint64 time, const Digest& digest) const {
    int nearest = -1;
    for (int i = 0; i < entries_.size(); ++i) {
        const Entry& e = entries_[i];
        if (e.isAddressedBy(time, digest)) return i;
        if (e.hasSnapshot() && e.time() <= time) nearest = i;
    }
    // Точной нет. Может, она просто переехала во времени — ищем по отпечатку.
    for (int i = 0; i < entries_.size(); ++i)
        if (entries_[i].digest() == digest && entries_[i].hasSnapshot()) return i;
    // И этого нет: запись вычистили. Ближайшая не позже искомой — то самое
    // состояние, которое к тому моменту в журнале и осталось.
    return nearest;
}

QString History::pathFor(const QString& noteId) const {
    return QDir(root_).filePath(QStringLiteral("history/%1.log").arg(noteId));
}

QString History::lockPathFor(const QString& root) {
    return QDir(root).filePath(QStringLiteral(".zametti/store.lock"));
}

bool History::dropVoidedLocked(const QString& path, const ZJournal& journal,
                               const QVector<int>& voided, QString* error) {
    assertLocked();
    if (voided.isEmpty()) return true;

    // ХВОСТОМ — обычный случай: слияние мелкой правки гасит последнюю запись,
    // схлопывание возврата — несколько последних. Тогда файл просто
    // укорачивается по началу первой лишней записи, и всё, что до неё,
    // читатель видит ровно как раньше.
    bool suffix = voided.last() == journal.size() - 1;
    for (int i = 1; i < voided.size() && suffix; ++i)
        if (voided[i] != voided[i - 1] + 1) suffix = false;
    if (suffix) {
        const qint64 cut = journal.at(voided.first()).offset();
        if (cut <= 0) {
            if (error) *error = QStringLiteral("cannot tell where the voided tail begins");
            return false;
        }
        QFile file(path);
        if (!file.resize(cut)) {
            if (error) *error = QStringLiteral("cannot cut the journal: %1").arg(file.errorString());
            return false;
        }
        return true;
    }

    // СЕРЕДИНОЙ — редкий случай: гасится запись, приехавшая со стороны.
    // Вырезать её на месте нельзя (звенья поколения считаются от предыдущего
    // слепка), поэтому файл пересобирается целиком и атомарно.
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("cannot read journal: %1").arg(file.errorString());
        return false;
    }
    const QByteArray blob = file.readAll();
    file.close();
    ZJournal full;
    if (!full.parse(blob, ZJournal::Want::All, -1, error)) return false;
    QVector<int> keep;
    for (int i = 0; i < full.size(); ++i)
        if (!voided.contains(i)) keep.append(i);
    QByteArray out;
    if (!full.toBytes(keep, full.cleanVersion(), &out, error)) return false;
    return replaceFile(path, out, error);
}

bool History::appendLocked(const QString& path, const NewRecord& what, QString* error) {
    assertLocked();

    // Что уже лежит в журнале. Читается только последнее поколение — память и
    // время ограничены им, а не длиной журнала.
    const auto reread = [&](ZJournal* journal, bool* exists) {
        QFile file(path);
        *exists = file.exists() && file.size() > 0;
        if (!*exists) return true;
        if (!file.open(QIODevice::ReadOnly)) {
            if (error) *error = QStringLiteral("cannot read journal: %1").arg(file.errorString());
            return false;
        }
        const QByteArray blob = file.readAll();
        file.close();
        return journal->parse(blob, ZJournal::Want::Chain, std::numeric_limits<int>::max(), error);
    };

    ZJournal journal;
    bool exists = false;
    if (!reread(&journal, &exists)) return false;

    // Оборванный хвост отрезаем прежде дозаписи, а не после: иначе мусор
    // остался бы посреди файла и увёл бы за собой всё поколение.
    if (journal.tailTrimmed()) {
        if (!trimTailLocked(path, error)) return false;
    }

    // ГАШЕНИЕ — прежде рождения новой записи, и порядок здесь существенный:
    // новая запись сжимается относительно предшественника ПО ФАЙЛУ, и если
    // выкинуть погашенные потом, звено цепочки осталось бы без своей базы.
    if (!what.voids().isEmpty() && exists) {
        const QVector<int> voided = journal.indexesOf(what.voids());
        if (!voided.isEmpty()) {
            if (!dropVoidedLocked(path, journal, voided, error)) return false;
            journal = ZJournal{};
            if (!reread(&journal, &exists)) return false;
        }
    }

    // Рождение записи — дело журнала: ревизию, отпечаток, кодек и сжатие
    // относительно предшественника выбирает он. Здесь только файл.
    QByteArray tail;
    if (!exists) {
        QDir().mkpath(QFileInfo(path).absolutePath());
        // Новый журнал заводится сразу чищеным: он весь написан нынешними
        // правилами, и вычищать в нём нечего по построению. Иначе первая же
        // заметка приезжала бы на чистку зря.
        tail = ZJournal::headerBytes(QString::fromLatin1(kCleanVersion));
    }
    QByteArray record;
    Entry made;
    if (!journal.composeRecord(what, clock_.floor(), &made, &record, error)) return false;
    tail += record;

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append)) {
        if (error) *error = QStringLiteral("cannot open journal: %1").arg(file.errorString());
        return false;
    }
    const qint64 wrote = file.write(tail);
    // Закрываем без fsync — см. шапку журнала. Незаписавшийся хвост (диск
    // кончился) читатель отрежет сам, но сказать об этом надо сразу.
    file.close();
    if (wrote != tail.size()) {
        if (error) *error = QStringLiteral("journal written incompletely: %1 of %2 bytes")
                                .arg(wrote)
                                .arg(tail.size());
        return false;
    }
    // Пол устройства поднимаем ПОСЛЕ удачной записи: число обещает «столько уже
    // записано», и обещать это заранее нельзя.
    clock_.advanceTo(made.time());
    return true;
}

bool History::readLocked(const QString& path, ZJournal* out, QString* error) const {
    assertLocked();
    QFile file(path);
    if (!file.exists()) {
        *out = ZJournal{};
        return true;  // журнала ещё нет — это не беда, а «правок не было»
    }
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("cannot read journal: %1").arg(file.errorString());
        return false;
    }
    const QByteArray blob = file.readAll();
    file.close();
    return out->parse(blob, ZJournal::Want::Frames, -1, error);
}

bool History::snapshotAtLocked(const QString& path, int index, QByteArray* out,
                               QString* error) const {
    assertLocked();
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("cannot read journal: %1").arg(file.errorString());
        return false;
    }
    const QByteArray blob = file.readAll();
    file.close();

    ZJournal journal;
    if (!journal.parse(blob, ZJournal::Want::Chain, index, error)) return false;
    if (index < 0 || index >= journal.size()) {
        if (error) *error = QStringLiteral("journal has no record #%1").arg(index);
        return false;
    }
    const Entry& entry = journal.at(index);
    if (!entry.hasSnapshot()) {
        if (error)
            *error = QStringLiteral("record #%1 (%2) has no snapshot")
                         .arg(index)
                         .arg(describeKind(entry.kind()));
        return false;
    }
    return journal.rebuildAt(index, out, error);
}

bool History::trimTailLocked(const QString& path, QString* error) {
    assertLocked();
    ZJournal journal;
    QFile probe(path);
    if (!probe.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("cannot read journal: %1").arg(probe.errorString());
        return false;
    }
    const QByteArray blob = probe.readAll();
    probe.close();
    if (!journal.parse(blob, ZJournal::Want::Frames, -1, error)) return false;
    if (!journal.tailTrimmed()) return true;
    QFile file(path);
    if (!file.open(QIODevice::ReadWrite)) {
        if (error) *error = QStringLiteral("cannot open journal: %1").arg(file.errorString());
        return false;
    }
    const bool ok = file.resize(journal.goodBytes());
    file.close();
    if (!ok && error) *error = QStringLiteral("cannot cut the journal tail");
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

QVector<int> ZJournal::survivors(qint64 now) const {
    const QVector<Entry>& entries = entries_;
    QVector<int> keep;
    for (int i = 0; i < entries.size(); ++i) {
        // Последняя запись остаётся всегда: для удалённой заметки это её
        // вечный финальный слепок и её tombstone.
        if (i + 1 == entries.size()) {
            keep.append(i);
            continue;
        }
        if (bucketOf(entries[i].time(), now) != bucketOf(entries[i + 1].time(), now)) keep.append(i);
    }
    return keep;
}

bool History::thinLocked(const QString& path, qint64 now, QString* error) {
    assertLocked();
    QFileInfo info(path);
    if (!info.exists()) return true;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("cannot read journal: %1").arg(file.errorString());
        return false;
    }
    const QByteArray blob = file.readAll();
    file.close();
    // Метка состояния файла на момент чтения — по ней перед подменой видно,
    // не дописал ли кто-то запись, пока мы считали. Так прореживание можно
    // гонять в отдельном потоке, не запирая журнал.
    const qint64 sawSize = info.size();
    const QDateTime sawTime = info.lastModified();

    ZJournal journal;
    if (!journal.parse(blob, ZJournal::Want::All, -1, error)) return false;
    const QVector<int> keep = journal.survivors(now);
    if (keep.size() == journal.size() && !journal.tailTrimmed()) return true;  // нечего делать

    // Версию чистки переносим как есть: прореживание — это про время, а не про
    // дубликаты, и объявить журнал чищеным оно права не имеет.
    QByteArray out;
    if (!journal.toBytes(keep, journal.cleanVersion(), &out, error)) return false;

    QFileInfo now2(path);
    if (now2.size() != sawSize || now2.lastModified() != sawTime) {
        // Журнал изменился под руками — в него дописали, пока мы считали.
        // Отменяемся молча: прореживание не обязано случиться именно сейчас,
        // а вот потерять свежую запись оно права не имеет.
        return true;
    }

    // Инвариант «журнал только растёт» действует между перезаписями; их всего
    // две — прореживание и чистка.
    return replaceFile(path, out, error);
}

bool History::compressLocked(const QString& path, const Planner& planner, bool force,
                             CompressOutcome* outcome, QString* error) {
    assertLocked();
    CompressOutcome done;
    const auto finish = [&](bool ok) {
        if (outcome != nullptr) *outcome = done;
        return ok;
    };

    QFile file(path);
    if (!file.exists()) return finish(true);   // журнала нет — и чистить нечего
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("cannot read journal: %1").arg(file.errorString());
        return finish(false);
    }
    const QByteArray blob = file.readAll();
    file.close();

    ZJournal journal;
    if (!journal.parse(blob, ZJournal::Want::All, -1, error)) return finish(false);

    done.versionBefore = journal.cleanVersion();
    done.versionAfter = journal.cleanVersion();
    done.recordsBefore = journal.size();
    done.recordsAfter = done.recordsBefore;

    // Ленивость: чищеный журнал не трогается вовсе. Люк ходит с force.
    const QString target = QString::fromLatin1(kCleanVersion);
    if (!force && journal.cleanVersion() == target) return finish(true);

    // Правилу нужны слепки, а не байты: «одинаковы ли две записи» — вопрос про
    // содержимое. Распаковываем разом, потому что дальше правило смотрит на
    // них помногу раз, и распаковывать по требованию значило бы делать это
    // заново на каждом проходе.
    QVector<QByteArray> plain(journal.size());
    for (int i = 0; i < journal.size(); ++i) {
        if (!journal.at(i).hasSnapshot()) continue;
        if (!journal.rebuildAt(i, &plain[i], error)) return finish(false);
    }

    const QVector<int> keep = planner(journal, plain);
    // План приходит снаружи, и доверять ему на слово нельзя: перепутанный
    // порядок или номер за границей испортили бы журнал молча.
    for (int i = 0; i < keep.size(); ++i)
        if (keep[i] < 0 || keep[i] >= journal.size() ||
            (i > 0 && keep[i] <= keep[i - 1])) {
            if (error) *error = QStringLiteral("cleanup rule returned an invalid list");
            return finish(false);
        }

    // НИ БАЙТА, ЕСЛИ НИЧЕГО НЕ ПОМЕНЯЛОСЬ — на этом стоит обещание
    // идемпотентности: повторный форс не переписывает файл вовсе.
    if (keep.size() == journal.size() && !journal.tailTrimmed() &&
        journal.cleanVersion() == target)
        return finish(true);

    QByteArray out;
    if (!journal.toBytes(keep, target, &out, error)) return finish(false);
    if (!replaceFile(path, out, error)) return finish(false);

    done.versionAfter = target;
    done.recordsAfter = int(keep.size());
    done.rewritten = true;
    return finish(true);
}

// Открытые методы: замок и ничего больше. Ни одной строки работы с файлами
// здесь нет и быть не должно — на этом стоит обещание «всё под замком».
bool History::append(const QString& noteId, const NewRecord& what, QString* error) {
    const QMutexLocker locked(&gate());
    return appendLocked(pathFor(noteId), what, error);
}


bool History::read(const QString& noteId, ZJournal* out, QString* error) const {
    const QMutexLocker locked(&gate());
    return readLocked(pathFor(noteId), out, error);
}

bool History::snapshotAt(const QString& noteId, int index, QByteArray* out,
                         QString* error) const {
    const QMutexLocker locked(&gate());
    return snapshotAtLocked(pathFor(noteId), index, out, error);
}

bool History::trimTail(const QString& noteId, QString* error) {
    const QMutexLocker locked(&gate());
    return trimTailLocked(pathFor(noteId), error);
}

bool History::thin(const QString& noteId, qint64 now, QString* error) {
    const QMutexLocker locked(&gate());
    return thinLocked(pathFor(noteId), now, error);
}

bool History::compress(const QString& noteId, const Planner& planner, bool force,
                       CompressOutcome* outcome, QString* error) {
    const QMutexLocker locked(&gate());
    return compressLocked(pathFor(noteId), planner, force, outcome, error);
}

ThinReport History::thinAll(qint64 now, bool dryRun) {
    ThinReport report;
    const QDir history(QDir(root_).filePath(QStringLiteral("history")));
    if (!history.exists()) return report;

    for (const QString& name : history.entryList({QStringLiteral("*.log")}, QDir::Files)) {
        const QString noteId = name.left(name.size() - 4);
        ZJournal before;
        QString error;
        if (!read(noteId, &before, &error)) {
            report.problems.append(QStringLiteral("%1: %2").arg(name, error));
            continue;
        }
        ++report.journals;
        report.recordsBefore += before.size();
        report.bytesBefore += QFileInfo(pathFor(noteId)).size();
        if (before.tailTrimmed()) report.trimmed.append(name);

        if (dryRun) {
            report.recordsAfter += before.survivors(now).size();
            report.bytesAfter += QFileInfo(pathFor(noteId)).size();
            continue;
        }
        if (!thin(noteId, now, &error)) {
            report.problems.append(QStringLiteral("%1: %2").arg(name, error));
            continue;
        }
        ZJournal after;
        read(noteId, &after, &error);
        report.recordsAfter += after.size();
        report.bytesAfter += QFileInfo(pathFor(noteId)).size();
    }
    return report;
}

}  // namespace zametti::journal
