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
        assert(false && "операция истории вызвана без замка");
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
                       qint64 plainSize, qint64 source, Codec codec) {
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
        writer.append(int(codec));
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

// Собрать распакованный слепок записи index, идя от начала её поколения.
// packed выровнен по entries; звенья своего поколения в нём есть.
// eachLink — сверять отпечаток на КАЖДОМ звене. По умолчанию нет: сверка стоит
// 210 мкс на звено против 4.7 мкс на саму распаковку (замер на заметке в
// 239 КБ), то есть на цепочке в 31 звено это 98% всей цены. И она избыточна:
// порча любого звена меняет итоговые байты, а отпечаток последнего мы сверяем
// всегда. Посленинная сверка нужна ровно для одного — назвать, какое именно
// звено испорчено, — и включается она только тогда, когда итог уже не сошёлся.
bool rebuildAt(const QVector<Entry>& entries, const QVector<QByteArray>& packed, int index,
               QByteArray* out, QString* error, bool eachLink = false) {
    int base = index;
    while (base > 0 && !entries[base].full()) --base;
    if (!entries[base].full()) {
        if (error)
            *error = QStringLiteral("поколение записи №%1 начинается не с полного слепка")
                         .arg(index);
        return false;
    }
    QByteArray current;
    for (int i = base; i <= index; ++i) {
        const Entry& e = entries[i];
        if (!e.hasSnapshot()) continue;  // надгробие цепочку не рвёт: у него слепка нет
        if (e.codec != Codec::Zstd && e.codec != Codec::None && e.codec != Codec::ZstdDelta) {
            if (error)
                *error = QStringLiteral("слепок записи №%1 сжат неизвестным кодеком %2")
                             .arg(i)
                             .arg(int(e.codec));
            return false;
        }
        QByteArray plain;
        if (e.codec == Codec::None) {
            plain = packed[i];
        } else if (!decompressSnapshot(packed[i], e.full() ? QByteArray() : current, e.plainSize,
                                       &plain)) {
            if (error) *error = QStringLiteral("слепок записи №%1 не распаковывается").arg(i);
            return false;
        }
        if (eachLink || i == index) {
            const Digest actual =
                hashOf(std::string_view(plain.constData(), size_t(plain.size())));
            if (actual != e.digest) {
                if (error)
                    *error = QStringLiteral("слепок записи №%1 не сходится с отпечатком").arg(i);
                // Итог не сошёлся — теперь стоит пройти цепочку с проверкой
                // каждого звена и назвать то, с которого всё пошло не так.
                if (!eachLink) {
                    QByteArray ignored;
                    QString which;
                    if (!rebuildAt(entries, packed, index, &ignored, &which, true) &&
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
// Какие слепки собирать: ничьих (таймлайн), поколение до нужной записи
// (переход к слепку, дозапись) или все (прореживание переписывает файл
// целиком). Поколение держится скользящим окном: встретив полный слепок,
// прежде собранное выбрасываем — память ограничена одним поколением, а не
// длиной журнала.
enum class Want { None, Chain, All };

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
        const bool takeThis =
            want == Want::All || (want == Want::Chain && out->entries.size() <= wantIndex);
        const RawRecord record = readRecord(next, takeThis);
        if (!record.valid || record.isHeader || next.currentOffset() == 0) {
            // Оборванный или испорченный хвост. Всё, что до него, — целое.
            out->tailTrimmed = true;
            break;
        }
        // Скользящее окно чистит собранное, только пока нужное поколение ещё
        // не набрано: иначе полный слепок, встреченный ПОСЛЕ него, выбросил бы
        // как раз то, ради чего читали.
        if (want == Want::Chain && packed && record.entry.full() &&
            out->entries.size() <= wantIndex)
            for (QByteArray& old : *packed) old.clear();
        out->entries.append(record.entry);
        out->entries.last().offset = qint64(offset);
        if (packed) packed->append(record.packed);
        offset += next.currentOffset();
        out->goodBytes = qint64(offset);
    }
    return true;
}

}  // namespace

History::History(QString root) : root_(std::move(root)) {}

QString History::pathFor(const QString& noteId) const {
    return QDir(root_).filePath(QStringLiteral("history/%1.log").arg(noteId));
}

QString storeLockPath(const QString& root) {
    return QDir(root).filePath(QStringLiteral(".zametti/store.lock"));
}

bool History::appendLocked(const QString& path, Kind kind, qint64 time,
                           const QByteArray& snapshot, qint64 source, QString* error) {
    assertLocked();
    const bool tombstone = kind == Kind::Tombstone;

    // Что уже лежит в журнале: сколько записей прошло от начала поколения и
    // чем был предшественник. Читается только последнее поколение — память и
    // время ограничены им, а не длиной журнала.
    Journal journal;
    QVector<QByteArray> packed;
    QByteArray blob;
    QFile file(path);
    const bool exists = file.exists() && file.size() > 0;
    if (exists) {
        if (!file.open(QIODevice::ReadOnly)) {
            if (error) *error = QStringLiteral("журнал не прочитать: %1").arg(file.errorString());
            return false;
        }
        blob = file.readAll();
        file.close();
        if (!parseAll(blob, &journal, Want::Chain, std::numeric_limits<int>::max(), &packed, error))
            return false;
    }

    // Оборванный хвост отрезаем прежде дозаписи, а не после: иначе мусор
    // остался бы посреди файла и увёл бы за собой всё поколение.
    if (journal.tailTrimmed) {
        if (!trimTailLocked(path, error)) return false;
    }

    QByteArray base;   // пусто — запись станет полным слепком
    Digest digest;
    if (!tombstone) {
        digest = hashOf(std::string_view(snapshot.constData(), size_t(snapshot.size())));

        int last = journal.entries.size() - 1;
        while (last >= 0 && !journal.entries[last].hasSnapshot()) --last;
        if (last >= 0) {
            int from = last;
            while (from > 0 && !journal.entries[from].full()) --from;
            const int inGeneration = last - from + 1;
            if (inGeneration < kGeneration) {
                QString why;
                // Не собрался предшественник — начинаем новое поколение. Это
                // строго лучше отказа: беда в старом поколении не мешает
                // писать новую историю.
                if (!rebuildAt(journal.entries, packed, last, &base, &why)) base.clear();
            }
        }
    }

    const Codec codec = base.isEmpty() ? Codec::Zstd : Codec::ZstdDelta;
    QByteArray body;
    if (!tombstone) {
        body = compressSnapshot(snapshot, base);
        if (body.isEmpty() && !snapshot.isEmpty()) {
            if (error) *error = QStringLiteral("не удалось сжать слепок");
            return false;
        }
    }

    QByteArray tail;
    if (!exists) {
        QDir().mkpath(QFileInfo(path).absolutePath());
        tail = headerBytes();
    }
    tail += recordBytes(kind, time, digest, body, snapshot.size(), source, codec);

    if (!file.open(QIODevice::WriteOnly | QIODevice::Append)) {
        if (error) *error = QStringLiteral("журнал не открыть: %1").arg(file.errorString());
        return false;
    }
    const qint64 wrote = file.write(tail);
    // Закрываем без fsync — см. шапку журнала. Незаписавшийся хвост (диск
    // кончился) читатель отрежет сам, но сказать об этом надо сразу.
    file.close();
    if (wrote != tail.size()) {
        if (error) *error = QStringLiteral("журнал записан не целиком: %1 из %2 байт")
                                .arg(wrote)
                                .arg(tail.size());
        return false;
    }
    return true;
}

bool History::readLocked(const QString& path, Journal* out, QString* error) const {
    assertLocked();
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

bool History::snapshotAtLocked(const QString& path, int index, QByteArray* out,
                               QString* error) const {
    assertLocked();
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("журнал не прочитать: %1").arg(file.errorString());
        return false;
    }
    const QByteArray blob = file.readAll();
    file.close();

    Journal journal;
    QVector<QByteArray> packed;
    if (!parseAll(blob, &journal, Want::Chain, index, &packed, error)) return false;
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
    return rebuildAt(journal.entries, packed, index, out, error);
}

bool History::trimTailLocked(const QString& path, QString* error) {
    assertLocked();
    Journal journal;
    QFile probe(path);
    if (!probe.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("журнал не прочитать: %1").arg(probe.errorString());
        return false;
    }
    const QByteArray blob = probe.readAll();
    probe.close();
    if (!parseAll(blob, &journal, Want::None, -1, nullptr, error)) return false;
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

bool History::thinLocked(const QString& path, qint64 now, QString* error) {
    assertLocked();
    QFileInfo info(path);
    if (!info.exists()) return true;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("журнал не прочитать: %1").arg(file.errorString());
        return false;
    }
    const QByteArray blob = file.readAll();
    file.close();
    // Метка состояния файла на момент чтения — по ней перед подменой видно,
    // не дописал ли кто-то запись, пока мы считали. Так прореживание можно
    // гонять в отдельном потоке, не запирая журнал.
    const qint64 sawSize = info.size();
    const QDateTime sawTime = info.lastModified();

    Journal journal;
    QVector<QByteArray> packed;
    if (!parseAll(blob, &journal, Want::All, -1, &packed, error)) return false;
    const QVector<int> keep = survivors(journal.entries, now);
    if (keep.size() == journal.entries.size() && !journal.tailTrimmed) return true;  // нечего делать

    // Выжившие перекодируются заново: звено цепочки осмысленно только рядом со
    // своим предшественником, а он мог не выжить. Заодно поколения выходят
    // ровными, а не рваными.
    QByteArray out = headerBytes();
    QByteArray previous;
    int written = 0;
    for (int i : keep) {
        const Entry& e = journal.entries[i];
        if (!e.hasSnapshot()) {
            out += recordBytes(e.kind, e.time, e.digest, QByteArray(), 0, e.source, Codec::Zstd);
            continue;
        }
        QByteArray plain;
        if (!rebuildAt(journal.entries, packed, i, &plain, error)) return false;
        const bool full = written % kGeneration == 0;
        const Codec codec = full ? Codec::Zstd : Codec::ZstdDelta;
        const QByteArray body = compressSnapshot(plain, full ? QByteArray() : previous);
        if (body.isEmpty() && !plain.isEmpty()) {
            if (error) *error = QStringLiteral("не удалось сжать слепок записи №%1").arg(i);
            return false;
        }
        out += recordBytes(e.kind, e.time, e.digest, body, plain.size(), e.source, codec);
        previous = plain;
        ++written;
    }

    QFileInfo now2(path);
    if (now2.size() != sawSize || now2.lastModified() != sawTime) {
        // Журнал изменился под руками — в него дописали, пока мы считали.
        // Отменяемся молча: прореживание не обязано случиться именно сейчас,
        // а вот потерять свежую запись оно права не имеет.
        return true;
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

// Открытые методы: замок и ничего больше. Ни одной строки работы с файлами
// здесь нет и быть не должно — на этом стоит обещание «всё под замком».
bool History::append(const QString& noteId, Kind kind, qint64 time, const QByteArray& snapshot,
                     qint64 source, QString* error) {
    const QMutexLocker locked(&gate());
    return appendLocked(pathFor(noteId), kind, time, snapshot, source, error);
}

bool History::truncate(const QString& noteId, int keepCount, QString* error) {
    const QMutexLocker locked(&gate());
    const QString path = pathFor(noteId);

    if (keepCount < 1) {
        if (error) *error = QStringLiteral("первую запись журнала стирать нельзя");
        return false;
    }

    Journal journal;
    if (!readLocked(path, &journal, error)) return false;
    if (keepCount >= journal.entries.size()) return true;   // отбрасывать нечего

    // Режем по НАЧАЛУ первой лишней записи: всё, что до него, — целые записи,
    // и читатель их видит ровно как раньше.
    const qint64 cut = journal.entries[keepCount].offset;
    if (cut <= 0) {
        if (error) *error = QStringLiteral("непонятно, где кончается запись %1").arg(keepCount);
        return false;
    }
    QFile file(path);
    if (!file.resize(cut)) {
        if (error) *error = QStringLiteral("журнал не укоротить: %1").arg(file.errorString());
        return false;
    }
    return true;
}

bool History::read(const QString& noteId, Journal* out, QString* error) const {
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

ThinReport History::thinAll(qint64 now, bool dryRun) {
    ThinReport report;
    const QDir history(QDir(root_).filePath(QStringLiteral("history")));
    if (!history.exists()) return report;

    for (const QString& name : history.entryList({QStringLiteral("*.log")}, QDir::Files)) {
        const QString noteId = name.left(name.size() - 4);
        Journal before;
        QString error;
        if (!read(noteId, &before, &error)) {
            report.problems.append(QStringLiteral("%1: %2").arg(name, error));
            continue;
        }
        ++report.journals;
        report.recordsBefore += before.entries.size();
        report.bytesBefore += QFileInfo(pathFor(noteId)).size();
        if (before.tailTrimmed) report.trimmed.append(name);

        if (dryRun) {
            report.recordsAfter += survivors(before.entries, now).size();
            report.bytesAfter += QFileInfo(pathFor(noteId)).size();
            continue;
        }
        if (!thin(noteId, now, &error)) {
            report.problems.append(QStringLiteral("%1: %2").arg(name, error));
            continue;
        }
        Journal after;
        read(noteId, &after, &error);
        report.recordsAfter += after.entries.size();
        report.bytesAfter += QFileInfo(pathFor(noteId)).size();
    }
    return report;
}

}  // namespace zametti::journal
