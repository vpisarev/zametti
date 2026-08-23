#include "journal.h"
#include <cstdio>
#include "zstorage.h"

#include "history_rules.h"   // sameApartFromModified, changedChars — про формат заметки

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

namespace zametti {
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

    // Версия содержимого (шапка). Номер взят СВОБОДНЫЙ, а не 3: читатель у
    // шапки и у записи один, и ключи их живут в общем пространстве. Совпасть с
    // KeyDigest значило бы различать их по типу значения — так уже сделано у
    // пары Magic/ZJournal::Kind, и хватит: каждая такая пара это ловушка для того, кто
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
    ZJournal::Entry entry;
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
    ZJournal::Kind kind = ZJournal::Kind::Save;
    qint64 time = 0;
    qint64 seq = 0;
    Digest digest;
    ZJournal::Codec codec = ZJournal::Codec::Zstd;
    qint64 plainSize = 0;
    qint64 packedSize = 0;
    qint64 source = 0;
    QByteArray frameHash;
    QVector<ZJournal::EntryRef> voids;
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
                    kind = ZJournal::Kind(v);
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
                codec = ZJournal::Codec(v);
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
                    voids.append(ZJournal::EntryRef(when, ref));
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
    out.entry = ZJournal::Entry(kind, time, seq, digest, source, std::move(voids));
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
    writer.append(QLatin1StringView(ZJournal::kMagic));
    writer.append(KeyVersion);
    writer.append(ZJournal::kFormatVersion);
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
// обнуляет поле во всём журнале); с ZJournal::Entry оно попадает туда по построению.
//
// packed — сжатый слепок, отдельно: он не свойство рамки, а её содержимое, и
// при пересборке он другой, чем был в файле, хотя рамка та же.
QByteArray ZJournal::frameBytes(const ZJournal::Entry& e) {
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
    for (const ZJournal::EntryRef& ref : e.voids()) {
        put64(ref.time());
        putDigest(ref.digest());
    }
    return out;
}

QByteArray ZJournal::recordBytes(const ZJournal::Entry& e, const QByteArray& packed) {
    const bool bodyless = !e.hasSnapshot();   // надгробие и гашение
    const bool restore = e.kind() == ZJournal::Kind::Restore;
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
        for (const ZJournal::EntryRef& ref : e.voids()) {
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
    if (header.magic != QLatin1StringView(ZJournal::kMagic)) {
        if (error) *error = QStringLiteral("not a zametti journal: foreign magic '%1'").arg(header.magic);
        return false;
    }
    if (header.version != ZJournal::kFormatVersion) {
        if (error)
            *error = QStringLiteral("journal version %1, but we only know %2")
                         .arg(header.version)
                         .arg(ZJournal::kFormatVersion);
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
        ZJournal::Entry& placed = out->entries_.last();
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
    const QVector<ZJournal::Entry>& entries = entries_;
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
        const ZJournal::Entry& e = entries[i];
        if (isDamaged(i)) {
            if (error)
                *error = QStringLiteral("record #%1: the frame checksum does not match").arg(i);
            return false;
        }
        if (!e.hasSnapshot()) continue;  // надгробие цепочку не рвёт: у него слепка нет
        if (e.codec() != ZJournal::Codec::Zstd && e.codec() != ZJournal::Codec::None && e.codec() != ZJournal::Codec::ZstdDelta) {
            if (error)
                *error = QStringLiteral("snapshot of record #%1 compressed with unknown codec %2")
                             .arg(i)
                             .arg(int(e.codec()));
            return false;
        }
        QByteArray plain;
        if (e.codec() == ZJournal::Codec::None) {
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
    const QVector<ZJournal::Entry>& entries = entries_;
    *out = headerBytes(clean);
    QByteArray previous;
    int written = 0;
    for (int i : keep) {
        const ZJournal::Entry& e = entries[i];
        if (!e.hasSnapshot()) {
            // Надгробие поколения не начинает и не рвёт: слепка у него нет.
            ZJournal::Entry frame = e;
            frame.layAs(ZJournal::Codec::Zstd, 0);
            *out += recordBytes(frame, QByteArray());
            continue;
        }
        QByteArray plain;
        if (!rebuildAt(i, &plain, error)) return false;
        const bool full = written % ZJournal::kGeneration == 0;
        const ZJournal::Codec codec = codecFor(full);
        const QByteArray body = compressSnapshot(plain, full ? QByteArray() : previous);
        if (body.isEmpty() && !plain.isEmpty()) {
            if (error) *error = QStringLiteral("cannot compress snapshot of record #%1").arg(i);
            return false;
        }
        // Меняется только укладка: кодек и размер до сжатия. Всё остальное —
        // род, время, отпечаток, source и любое будущее поле рамки — едет из
        // прежней записи неприкосновенным.
        ZJournal::Entry frame = e;
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
bool ZJournal::Entry::isBefore(const ZJournal::Entry& other) const {
    if (seq_ != other.seq_) return seq_ < other.seq_;
    if (time_ != other.time_) return time_ < other.time_;
    // Контент старше надгробия: при равном ключе правка побеждает удаление.
    if (hasSnapshot() != other.hasSnapshot()) return hasSnapshot();
    return std::lexicographical_compare(digest_.bytes.begin(), digest_.bytes.end(),
                                        other.digest_.bytes.begin(), other.digest_.bytes.end());
}

qint64 ZJournal::latestTime() const {
    qint64 latest = 0;
    for (const ZJournal::Entry& e : entries_) latest = qMax(latest, e.time());
    return latest;
}

ZJournal::Stamp ZJournal::Stamp::now() { return ZJournal::Stamp(QDateTime::currentMSecsSinceEpoch(), true); }

qint64 ZJournal::stampFor(ZJournal::Stamp when, qint64 deviceFloor) const {
    qint64 stamp = when.requested();
    if (when.guarded() && deviceFloor > 0) stamp = qMax(stamp, deviceFloor + 1);
    const qint64 own = latestTime();
    if (own > 0) stamp = qMax(stamp, own + 1);
    return stamp;
}

bool ZJournal::composeRecord(const ZJournal::NewRecord& what, qint64 deviceFloor, ZJournal::Entry* frame,
                             QByteArray* bytes, QString* error) const {
    const ZJournal::Kind kind = what.kind();
    const QByteArray& snapshot = what.snapshot();
    // Слепка нет у надгробия и у гашения; всё, что ниже, спрашивает про это
    // одинаково.
    const bool bodyless = kind == ZJournal::Kind::Tombstone || kind == ZJournal::Kind::Amendment;
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
            if (inGeneration < ZJournal::kGeneration) {
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
    ZJournal::Entry made(kind, time, nextSeq(), digest, what.source(), what.voids());
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
    const ZJournal::Entry& target = entries_[index];
    for (const ZJournal::Entry& e : entries_)
        if (e.voidsEntry(target)) return true;
    return false;
}

QVector<int> ZJournal::indexesOf(const QVector<ZJournal::EntryRef>& refs) const {
    QVector<int> out;
    for (int i = 0; i < entries_.size(); ++i)
        for (const ZJournal::EntryRef& ref : refs)
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
    for (const ZJournal::Entry& e : entries_) seq = qMax(seq, e.seq());
    return seq + 1;
}

int ZJournal::indexOf(qint64 time, const Digest& digest) const {
    int nearest = -1;
    for (int i = 0; i < entries_.size(); ++i) {
        const ZJournal::Entry& e = entries_[i];
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

// --- отбор: что писать, кого гасить ----------------------------------------

ZJournal::Step ZJournal::planStep(const SnapshotOf& snapshotOf, const QByteArray& fresh, ZJournal::Kind kind,
                                  qint64 now, const Rules& rules) const {
    const ZJournal& journal = *this;
    Step step;

    // Вот он, сторож свежести, и он тут ровно один — на оба правила сразу.
    const qint64 window = qint64(qMax(1, rules.mergeHours)) * 3600 * 1000;
    const auto stale = [&](qint64 time) { return !rules.ignoreAge && now - time > window; };

    // ВОЗВРАТ К УЖЕ ЗАПИСАННОМУ СОСТОЯНИЮ. Ищем в хвосте самую старую запись,
    // равную новому слепку. Нашли — всё, что после неё, было работой, которую
    // человек сам же и отменил: она уходит, а новая запись не пишется вовсе.
    //
    //   …, M0   →   …, M0, M0'   →   …, M0
    //
    // Одного сравнения с последней записью мало: набрал человек текст (M0'),
    // отменил его (M0" = M0) — и одинаковые записи оказывались ЧЕРЕЗ ОДНУ. Так
    // и вышло у владельца в «Пробуем Obsidian».
    //
    // Схлопывание — ТОЛЬКО для обычного сохранения, и чужую вешку оно не
    // перепрыгивает: восстановление из истории и приход правки снаружи —
    // вешки, поставленные не набором, и стирать их нельзя ничем.
    int sameAs = -1;
    for (int i = kind == ZJournal::Kind::Save ? journal.size() - 1 : -1; i >= 0; --i) {
        const ZJournal::Entry& entry = journal.at(i);
        if (stale(entry.time())) break;      // дальше история старая, её не трогаем
        if (!entry.hasSnapshot()) break;   // надгробие: за него не заглядываем
        const QByteArray older = snapshotOf(i);
        if (older.isNull()) break;         // слепок не собрался — дальше не идём
        if (sameApartFromModified(older, fresh)) {
            sameAs = i;   // нашли; но, может, ещё старее лежит такая же
            continue;
        }
        if (sameAs >= 0) break;                        // старее — уже другое состояние
        if (entry.kind() != ZJournal::Kind::Save) break;           // чужую вешку не перепрыгиваем
    }
    if (sameAs >= 0) {
        for (int i = sameAs + 1; i < journal.size(); ++i) step.voided.append(i);
        step.writeNew = false;
        step.dropped = step.voided.size() + 1;   // хвост и сама новая
        return step;
    }

    // ЗАМЕНА ВМЕСТО ДОБАВЛЕНИЯ: мелкая правка встаёт на место прошлой записи.
    // Условий три, и все обязаны сойтись — прошлая запись свежая (её ещё не
    // поздно переписать), она тоже обычное сохранение, и версии разошлись на
    // мелочь.
    //
    // Меньше двух записей — это защита опорной: после замены в журнале
    // обязана остаться хотя бы одна, а первая — то, с чего заметка начиналась,
    // и стереть её нельзя ничем.
    if (kind != ZJournal::Kind::Save || journal.size() < 2) return step;
    const ZJournal::Entry& back = journal.at(journal.size() - 1);
    if (back.kind() != ZJournal::Kind::Save || !back.hasSnapshot() || stale(back.time())) return step;
    const QByteArray tail = snapshotOf(journal.size() - 1);
    if (tail.isNull() || tail.isEmpty()) return step;
    if (changedChars(tail, fresh) > qMax(0, rules.mergeChars)) return step;
    step.voided.append(journal.size() - 1);
    step.merged = 1;
    return step;
}

ZJournal::Plan ZJournal::planCompress(const QVector<QByteArray>& snapshots,
                                      const Rules& rules) const {
    const ZJournal& journal = *this;
    Plan plan;
    for (int i = 0; i < journal.size(); ++i) plan.keep.append(i);
    if (journal.isEmpty()) return plan;

    // ПОЧЕМУ ПРОХОДОВ МОЖЕТ БЫТЬ НЕСКОЛЬКО. Один проход — это «как если бы
    // записи дописывали по одной», и неподвижной точкой он сам по себе не
    // является. Пример: A, B, C, где A и B разошлись на 150 знаков (не
    // сливаются), B и C — на 80 (сливаются). Проход даёт A, C — а между ними
    // может оказаться и 90 знаков, то есть следующий проход слил бы и их.
    // Гоняем до неподвижности; на этом стоит обещание идемпотентности —
    // повторная миграция форсом не находит уже ничего.
    //
    // Каждый проход, который что-то меняет, укорачивает список хотя бы на
    // одну запись, так что проходов не больше, чем записей.
    for (int pass = 0; pass <= journal.size(); ++pass) {
        QVector<int> accepted;      // номера принятых записей
        QVector<ZJournal::Entry> acc;  // их рамки — их и видит правило
        const SnapshotOf snapshotOf = [&](int i) { return snapshots[accepted[i]]; };
        int duplicates = 0;
        int merged = 0;

        for (int idx : std::as_const(plan.keep)) {
            const ZJournal::Entry& entry = journal.at(idx);
            // «Сейчас» для записи — время её самой: пересборка проигрывает
            // историю заново, и свежесть в ней меряется от момента записи, а не
            // от сегодняшнего дня. Миграции это безразлично (она на возраст не
            // глядит), а вот показу чистой истории корпусным читателем — нет.
            // Правило видит НАКОПЛЕННЫЙ журнал: пересборка проигрывает
            // историю заново, запись за записью.
            const Step step =
                ZJournal(acc).planStep(snapshotOf, snapshots[idx], entry.kind(), entry.time(), rules);
            duplicates += step.dropped;
            merged += step.merged;
            // Пересборка чистит журнал НАСОВСЕМ: это разовая миграция старых
            // журналов, которых ещё не было в облаке, и там гасить нечего —
            // погашенное просто не попадает в новый файл.
            for (int i = step.voided.size() - 1; i >= 0; --i) {
                acc.removeAt(step.voided[i]);
                accepted.removeAt(step.voided[i]);
            }
            if (!step.writeNew) continue;
            acc.append(entry);
            accepted.append(idx);
        }
        ++plan.passes;
        const bool settled = accepted.size() == plan.keep.size();
        plan.keep = accepted;
        plan.duplicates += duplicates;
        plan.merged += merged;
        if (settled) break;
    }
    return plan;
}

// --- журнал заметки: то, что делается через хранилище ------------------------

ZJournal::ZJournal(ZStorage* store, QString noteId, Rules rules)
    : store_(store), id_(std::move(noteId)), rules_(rules) {}

void ZJournal::ensureBaseline(const QByteArray& contents, qint64 fileTimeMs) {
    if (!available()) return;
    ZJournal have;
    QString error;
    if (!store_->readJournal(id_, &have, &error)) {
        std::fprintf(stderr, "cannot read history: %s\n", error.toUtf8().constData());
        return;
    }
    if (!have.isEmpty()) return;   // история уже начата
    // Опорной записи отдают время ФАЙЛА: заметка, лежавшая с 2017 года, обязана
    // и в истории начинаться 2017 годом. Поэтому момент назван, а не «сейчас»,
    // и страж монотонности его не поднимает.
    const ZJournal::Stamp when = fileTimeMs > 0 ? ZJournal::Stamp::at(fileTimeMs) : ZJournal::Stamp::now();
    if (!store_->appendToJournal(id_, ZJournal::NewRecord::save(contents, when), &error))
        std::fprintf(stderr, "baseline record not written: %s\n", error.toUtf8().constData());
}

void ZJournal::compressOnce() {
    if (!available() || compressed_) return;   // за один заход в заметку — один раз
    compressed_ = true;
    QString error;
    if (!store_->compressJournal(id_, rules_, false, nullptr, &error))
        std::fprintf(stderr, "history not cleaned: %s\n", error.toUtf8().constData());
    tailKnown_ = false;   // хвост мог переехать
}

void ZJournal::loadTail() {
    if (tailKnown_) return;
    tailKnown_ = true;
    tail_.clear();
    tailTime_ = 0;
    ZJournal read;
    QString error;
    if (store_->readJournal(id_, &read, &error) && !read.isEmpty()) {
        // Голова, а не последняя по файлу: с чем сравнивать свежий слепок,
        // решает ПОРЯДОК записей, а не их укладка. Разойтись эти две вещи могут
        // только у журнала, побывавшего в синхронизации, — и тогда мелкая
        // правка слилась бы не с той записью.
        const int last = read.lastSnapshotIndex();
        if (last >= 0 && store_->journalSnapshot(id_, last, &tail_, &error))
            tailTime_ = read.at(last).time();
    }
}

bool ZJournal::record(ZJournal::Kind kind, const QByteArray& snapshot, QString* error) {
    qint64 source = 0;
    if (kind == ZJournal::Kind::Save && restoreSource_ != 0) {
        kind = ZJournal::Kind::Restore;
        source = restoreSource_;
    }
    if (kind == ZJournal::Kind::Save || kind == ZJournal::Kind::Restore) restoreSource_ = 0;
    if (!available()) return false;

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QString why;
    QString* err = error != nullptr ? error : &why;

    // ПЕРВАЯ ЗАПИСЬ В ЖУРНАЛ — первый из двух триггеров ленивой миграции.
    // Старый журнал чистится ДО того, как правило отбора начнёт сравнивать
    // новый слепок с хвостом: иначе оно работало бы поверх дубликатов, которых
    // призвано не допускать. Хвост после чистки разжимается заново.
    compressOnce();
    loadTail();

    ZJournal read;
    if (!store_->readJournal(id_, &read, err)) {
        std::fprintf(stderr, "cannot read history: %s\n", err->toUtf8().constData());
        tailKnown_ = false;
        return false;
    }
    // Слепки правило спрашивает по одному и только те, до которых дошло: у
    // хвоста они уже в памяти (ради этого журнал не разжимается), за
    // остальными идём в журнал.
    const int lastIndex = read.size() - 1;
    const auto snapshotOf = [&](int i) -> QByteArray {
        if (i == lastIndex && tailTime_ > 0) return tail_;
        QByteArray older;
        QString ignored;
        if (!store_->journalSnapshot(id_, i, &older, &ignored)) return {};
        return older;
    };
    const Step step = read.planStep(snapshotOf, snapshot, kind, now, rules_);

    // ГАШЕНИЕ ВМЕСТО СТИРАНИЯ. Записи, которые правило объявило лишними,
    // адресуются парой (время, отпечаток) и едут этим адресом в новой записи:
    // их байты выкидываются здесь же, а другое устройство, увидев новую запись,
    // погасит те же у себя. Стирание без адреса не доезжало никуда — уехавшая
    // запись возвращалась объединением и возвращалась бы вечно.
    QVector<ZJournal::EntryRef> voids;
    voids.reserve(step.voided.size());
    for (int i : step.voided) voids.append(ZJournal::EntryRef(read.at(i).time(), read.at(i).digest()));

    bool ok = true;
    if (step.writeNew) {
        ZJournal::NewRecord what = kind == ZJournal::Kind::Restore
                             ? ZJournal::NewRecord::restore(snapshot, source)
                             : (kind == ZJournal::Kind::External ? ZJournal::NewRecord::external(snapshot)
                                                       : ZJournal::NewRecord::save(snapshot));
        ok = store_->appendToJournal(id_, what.voiding(voids), err);
    } else if (!voids.isEmpty()) {
        // Человек вернулся к уже записанному состоянию: нового слепка нет, а
        // сказать «того, что между, больше нет» надо.
        ok = store_->appendToJournal(id_, ZJournal::NewRecord::amendment().voiding(voids), err);
    }
    if (!ok) {
        std::fprintf(stderr, "history not written: %s\n", err->toUtf8().constData());
        tailKnown_ = false;   // что там теперь — неизвестно
        return false;
    }
    tail_ = snapshot;
    tailTime_ = now;
    return true;
}

bool ZJournal::refresh(QString* error) {
    if (!available()) {
        if (error != nullptr) *error = QStringLiteral("note has no journal");
        return false;
    }
    compressOnce();
    // Хранилище наполняет НАС ЖЕ: рамки читаются в собственные записи, а кэш
    // захода (хвост, признаки) переживает чтение — он про заметку, а не про
    // конкретный разбор.
    ZJournal read;
    if (!store_->readJournal(id_, &read, error)) return false;
    entries_ = std::move(read.entries_);
    packed_ = std::move(read.packed_);
    damaged_ = std::move(read.damaged_);
    tailTrimmed_ = read.tailTrimmed_;
    goodBytes_ = read.goodBytes_;
    cleanVersion_ = read.cleanVersion_;
    return true;
}

bool ZJournal::snapshotAt(int index, QByteArray* out, QString* error) const {
    if (!available()) {
        if (error != nullptr) *error = QStringLiteral("note has no journal");
        return false;
    }
    return store_->journalSnapshot(id_, index, out, error);
}

}  // namespace zametti
