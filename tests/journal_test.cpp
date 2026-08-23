// Журнал правок: формат, оборванный хвост, прореживание.
//
// Что здесь проверяется и почему именно это:
//   - круг «записал — прочитал» на всех четырёх видах записей;
//   - слепок сходится с отпечатком, а подмена байтов замечается;
//   - оборванный хвост (падение во время записи) НЕ делает журнал битым —
//     это главный случай, ради которого мы не зовём fsync;
//   - чужой файл и незнакомая версия формата отвергаются вслух;
//   - инвариант B: между прореживаниями файл строго растёт;
//   - инвариант C: прореживание не трогает последнюю запись, идемпотентно,
//     и первый шаг назад попадает в состояние «до моих правок».

#include "journal.h"
#include "zstorage.h"

#include "test_util.h"

#include <vector>
#include "testdata.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <atomic>
#include <string>
#include <thread>

using namespace zametti;


namespace {

std::string str(const QString& s) { return s.toStdString(); }

// Каркас сравнивает строки, поэтому числа приводим сами — заодно в отчёте о
// провале видно значение, а не «не сошлось».
template <typename T>
std::string num(T value) { return std::to_string(value); }

QByteArray noteBody(int lines, const char* mark) {
    QByteArray out = "<!-- zametti\ncreated: 2026-07-31T00:00:00Z\n-->\n\n# Заметка\n\n";
    for (int i = 0; i < lines; ++i)
        out += QByteArray("- пункт ") + QByteArray::number(i) + " " + mark + "\n";
    return out;
}

// Время в мс; «сейчас» у тестов своё, чтобы шкала не зависела от календаря.
constexpr qint64 kNow = 1'700'000'000'000LL;
constexpr qint64 kMinute = 60 * 1000;
constexpr qint64 kHour = 60 * kMinute;
constexpr qint64 kDay = 24 * kHour;

// Что дописать — по роду записи. Наборам удобно перечислять роды, а
// именованные создатели не дают собрать неверное сочетание.
ZJournal::NewRecord recordFor(ZJournal::Kind kind, qint64 time, const QByteArray& body, qint64 source) {
    const ZJournal::Stamp when = ZJournal::Stamp::at(time);
    switch (kind) {
        case ZJournal::Kind::External: return ZJournal::NewRecord::external(body, when);
        case ZJournal::Kind::Restore: return ZJournal::NewRecord::restore(body, source, when);
        case ZJournal::Kind::Tombstone: return ZJournal::NewRecord::tombstone(when);
        case ZJournal::Kind::Amendment: return ZJournal::NewRecord::amendment(when);
        case ZJournal::Kind::Save: break;
    }
    return ZJournal::NewRecord::save(body, when);
}

QString append(ZStorage& h, const QString& id, ZJournal::Kind kind, qint64 time,
               const QByteArray& body, qint64 source = 0) {
    QString error;
    if (!h.appendToJournal(id, recordFor(kind, time, body, source), &error)) return error;
    return {};
}

void checkRoundTrip(const QString& dir) {
    ZStorage h(dir);
    const QString id = QStringLiteral("01n6r08s8wy52h");
    const QString path = h.journalPath(id);
    ZT_EQ("пути журнала", str(QDir(dir).filePath("history/01n6r08s8wy52h.log")), str(path));

    ZJournal empty;
    QString error;
    ZT_TRUE("журнала ещё нет — это не беда", h.readJournal(id, &empty, &error));
    ZT_EQ("и записей в нём ноль", num(0), num(empty.size()));

    const QByteArray first = noteBody(10, "раз");
    const QByteArray second = noteBody(12, "два");
    ZT_EQ("первая запись", std::string(), str(append(h, id, ZJournal::Kind::Save, kNow - 3 * kHour, first)));
    const qint64 afterFirst = QFile(path).size();
    ZT_EQ("вторая запись", std::string(),
          str(append(h, id, ZJournal::Kind::External, kNow - 2 * kHour, second)));
    ZT_TRUE("инвариант B: файл вырос", QFile(path).size() > afterFirst);
    ZT_EQ("восстановление", std::string(),
          str(append(h, id, ZJournal::Kind::Restore, kNow - kHour, first, kNow - 3 * kHour)));
    ZT_EQ("надгробие", std::string(),
          str(append(h, id, ZJournal::Kind::Tombstone, kNow - kMinute, QByteArray())));

    ZJournal read;
    ZT_TRUE("журнал читается", h.readJournal(id, &read, &error));
    ZT_EQ("ошибки нет", std::string(), str(error));
    ZT_EQ("записей четыре", num(4), num(read.size()));
    ZT_TRUE("хвост цел", !read.tailTrimmed());

    ZT_EQ("вид первой", num(int(ZJournal::Kind::Save)), num(int(read.at(0).kind())));
    ZT_EQ("вид второй", num(int(ZJournal::Kind::External)), num(int(read.at(1).kind())));
    ZT_EQ("вид третьей", num(int(ZJournal::Kind::Restore)), num(int(read.at(2).kind())));
    ZT_EQ("вид четвёртой", num(int(ZJournal::Kind::Tombstone)), num(int(read.at(3).kind())));
    ZT_EQ("время второй", num(kNow - 2 * kHour), num(read.at(1).time()));
    ZT_EQ("ревизия первой", num(1LL), num(read.at(0).seq()));
    ZT_EQ("ревизия второй", num(2LL), num(read.at(1).seq()));
    ZT_EQ("ревизия третьей", num(3LL), num(read.at(2).seq()));
    ZT_EQ("ревизия есть и у надгробия", num(4LL), num(read.at(3).seq()));
    ZT_EQ("источник восстановления", num(kNow - 3 * kHour), num(read.at(2).source()));
    ZT_EQ("источник обычной записи не пишется", num(0LL), num(read.at(0).source()));
    ZT_EQ("размер до сжатия", num(qint64(second.size())), num(read.at(1).plainSize()));
    ZT_TRUE("слепок в файле меньше исходного", read.at(1).packedSize() < second.size());
    ZT_TRUE("у надгробия слепка нет", !read.at(3).hasSnapshot());

    QByteArray got;
    ZT_TRUE("слепок первой достаётся", h.journalSnapshot(id, 0, &got, &error));
    ZT_EQ("и он тот самый", str(QString::fromUtf8(first)), str(QString::fromUtf8(got)));
    ZT_TRUE("слепок второй достаётся", h.journalSnapshot(id, 1, &got, &error));
    ZT_EQ("и он тот самый", str(QString::fromUtf8(second)), str(QString::fromUtf8(got)));
    ZT_TRUE("у надгробия слепка не спросить", !h.journalSnapshot(id, 3, &got, &error));
    ZT_TRUE("и сказано почему", error.contains(QStringLiteral("tombstone")));
    ZT_TRUE("за границей записей — отказ", !h.journalSnapshot(id, 99, &got, &error));

    // Отпечаток считается от распакованных байтов и сверяется всегда.
    ZT_EQ("отпечаток первой", hashOf(std::string_view(first.constData(), size_t(first.size()))).hex(),
          read.at(0).digest().hex());
}

// Ревизия переживает переписывание файла целиком. Прореживание и чистка — это
// пересборка всех выживших записей с нуля, и поле рамки, которое там потеряется,
// потеряется молча: круг «записал — прочитал» идёт через дозапись и такой потери
// не увидит. Поэтому спрашиваем отдельно.
void checkRevisionsSurviveThinning(const QString& dir) {
    ZStorage h(dir);
    const QString id = QStringLiteral("ревизии");
    // Правки редкие и старые — прореживание обязано что-то выбросить.
    for (int i = 0; i < 12; ++i)
        append(h, id, ZJournal::Kind::Save, kNow - 300 * kDay + i * kMinute, noteBody(i + 1, "р"));

    ZJournal before;
    QString error;
    ZT_TRUE("журнал читается", h.readJournal(id, &before, &error));
    ZT_EQ("ревизии подряд", num(12LL), num(before.at(before.size() - 1).seq()));

    // Кто выживет — знаем заранее: та же чистая функция, что и у прореживания.
    QVector<int> keep = before.survivors(kNow);
    ZT_TRUE("прореживанию есть что выбросить", keep.size() < before.size());
    ZT_TRUE("прореживание проходит", h.thinJournal(id, kNow, &error));

    ZJournal after;
    ZT_TRUE("и журнал читается", h.readJournal(id, &after, &error));
    ZT_EQ("выживших столько, сколько обещано", num(keep.size()), num(after.size()));
    bool same = after.size() == keep.size();
    for (int i = 0; i < after.size() && same; ++i)
        same = after.at(i).seq() == before.at(keep[i]).seq();
    ZT_TRUE("и ревизия каждого — прежняя, а не пересчитанная", same);

    // Номера не переиспользуются: следующая запись продолжает максимум.
    ZT_EQ("дозапись после прореживания", std::string(),
          str(append(h, id, ZJournal::Kind::Save, kNow, noteBody(20, "р"))));
    ZJournal grown;
    h.readJournal(id, &grown, &error);
    ZT_EQ("новая ревизия — на единицу больше максимума", num(13LL), num(grown.at(grown.size() - 1).seq()));
}

// Порча в середине слепка обязана быть замечена: молча отданные не те байты
// хуже, чем отказ.
void checkCorruption(const QString& dir) {
    ZStorage h(dir);
    const QString id = QStringLiteral("порча");
    const QString path = h.journalPath(id);
    const QByteArray body = noteBody(200, "текст");
    append(h, id, ZJournal::Kind::Save, kNow - kHour, body);

    QFile file(path);
    ZT_TRUE("файл открылся", file.open(QIODevice::ReadWrite));
    QByteArray blob = file.readAll();
    blob[blob.size() / 2] = char(blob[blob.size() / 2] ^ 0x5a);
    file.seek(0);
    file.write(blob);
    file.close();

    QByteArray got;
    QString error;
    ZT_TRUE("испорченный слепок не отдаётся", !h.journalSnapshot(id, 0, &got, &error));
    ZT_TRUE("и сказано, что именно не так",
            error.contains(QStringLiteral("does not decompress")) ||
                error.contains(QStringLiteral("mismatch")));
}

// Главный случай отказа от fsync: питание пропало посреди записи.
void checkTornTail(const QString& dir) {
    ZStorage h(dir);
    const QString id = QStringLiteral("обрыв");
    const QString path = h.journalPath(id);
    const QByteArray a = noteBody(10, "а");
    const QByteArray b = noteBody(20, "б");
    append(h, id, ZJournal::Kind::Save, kNow - 2 * kHour, a);
    const qint64 whole = QFile(path).size();
    append(h, id, ZJournal::Kind::Save, kNow - kHour, b);
    const qint64 both = QFile(path).size();

    // Обрываем вторую запись на середине — так и выглядит падение.
    QFile file(path);
    ZT_TRUE("файл открылся", file.open(QIODevice::ReadWrite));
    ZT_TRUE("хвост обрезан руками", file.resize(whole + (both - whole) / 2));
    file.close();

    ZJournal read;
    QString error;
    ZT_TRUE("журнал с оборванным хвостом открывается", h.readJournal(id, &read, &error));
    ZT_EQ("целая часть цела", num(1), num(read.size()));
    ZT_TRUE("и про хвост сказано", read.tailTrimmed());
    ZT_EQ("граница целого — конец первой записи", num(whole), num(read.goodBytes()));

    QByteArray got;
    ZT_TRUE("слепок целой записи достаётся", h.journalSnapshot(id, 0, &got, &error));
    ZT_EQ("и он тот самый", str(QString::fromUtf8(a)), str(QString::fromUtf8(got)));

    ZT_TRUE("хвост отрезается", h.trimJournalTail(id, &error));
    ZT_EQ("файл укоротился до целого", num(whole), num(QFile(path).size()));
    ZT_TRUE("после обрезки читается начисто", h.readJournal(id, &read, &error));
    ZT_TRUE("и хвоста больше нет", !read.tailTrimmed());

    // Дописывать после обрезки можно как ни в чём не бывало.
    ZT_EQ("дозапись после обрыва", std::string(), str(append(h, id, ZJournal::Kind::Save, kNow, b)));
    h.readJournal(id, &read, &error);
    ZT_EQ("записей снова две", num(2), num(read.size()));
}

void checkForeignFile(const QString& dir) {
    ZStorage h(dir);
    QString error;
    ZJournal read;

    const QString junkId = QStringLiteral("мусор");
    const QString junk = h.journalPath(junkId);
    QFile file(junk);
    ZT_TRUE("файл создан", file.open(QIODevice::WriteOnly));
    file.write("это вообще не журнал, а просто текст");
    file.close();
    ZT_TRUE("чужой файл отвергается", !h.readJournal(junkId, &read, &error));
    ZT_TRUE("и сказано, что он не наш", error.contains(QStringLiteral("not a zametti journal")));

    // Пустой файл — это пустой журнал: так выглядит только что созданный.
    const QString blankId = QStringLiteral("пустой");
    const QString blank = h.journalPath(blankId);
    { QFile f(blank); ZT_TRUE("пустой файл создан", f.open(QIODevice::WriteOnly)); }
    ZT_TRUE("пустой файл — пустой журнал", h.readJournal(blankId, &read, &error));
    ZT_EQ("и записей ноль", num(0), num(read.size()));

    // Журнал из будущего: версия, которой мы не знаем. Молча пропустить чужую
    // историю хуже, чем сказать вслух.
    const QString futureId = QStringLiteral("будущее");
    const QString future = h.journalPath(futureId);
    append(h, futureId, ZJournal::Kind::Save, kNow, noteBody(3, "будущее"));
    QFile ff(future);
    ZT_TRUE("файл открылся", ff.open(QIODevice::ReadWrite));
    QByteArray blob = ff.readAll();
    // Шапка — карта {1: "zametti-journal", 2: 1}; последний байт версии.
    const int versionAt = blob.indexOf("zametti-journal") + 15 + 1;
    blob[versionAt] = char(9);
    ff.seek(0);
    ff.write(blob);
    ff.close();
    ZT_TRUE("журнал незнакомой версии отвергается", !h.readJournal(futureId, &read, &error));
    ZT_TRUE("и версия названа", error.contains(QStringLiteral("version 9")));
}

// Шкала прореживания. Времена задаются напрямую, чтобы проверять правило, а
// не сжатие.
void checkThinningScale() {
    QVector<ZJournal::Record> entries;
    auto add = [&entries](qint64 time) {
        entries.append(ZJournal::Record(ZJournal::Kind::Save, time, 0, Digest{}));
    };

    // Последний час — всё до единой.
    for (int i = 10; i >= 1; --i) add(kNow - i * kMinute);
    ZT_EQ("последний час не прореживается", num(10), num(ZJournal(entries).survivors(kNow).size()));

    // Сутки — не чаще раза в минуту: три записи внутри одной минуты схлопнутся
    // в одну, и это будет последняя из них.
    entries.clear();
    add(kNow - 5 * kHour);
    add(kNow - 5 * kHour + 1000);
    add(kNow - 5 * kHour + 2000);
    add(kNow - 5 * kHour + 3 * kMinute);
    add(kNow - kMinute);
    QVector<int> keep = ZJournal(entries).survivors(kNow);
    ZT_EQ("минута схлопывается в одну запись", num(3), num(keep.size()));
    ZT_EQ("остаётся последняя в минуте", num(2), num(keep[0]));

    // Неделя — раз в час: шесть записей через полчаса покрывают три часа и
    // схлопываются в три, плюс свежая последняя.
    entries.clear();
    for (int i = 0; i < 6; ++i) add(kNow - 3 * kDay + i * 30 * kMinute);
    add(kNow);
    keep = ZJournal(entries).survivors(kNow);
    ZT_EQ("три часа дают три записи плюс последняя", num(4), num(keep.size()));

    // Дальше месяца — раз в месяц; и последняя запись выживает всегда.
    entries.clear();
    for (int i = 0; i < 40; ++i) add(kNow - 400 * kDay + i * kDay);
    keep = ZJournal(entries).survivors(kNow);
    ZT_TRUE("год назад остаётся горстка", keep.size() <= 3);
    ZT_EQ("последняя запись на месте", num(entries.size() - 1), num(keep.last()));

    // Одна-единственная запись не прореживается никогда — для удалённой
    // заметки это её вечный финальный слепок.
    entries.clear();
    add(kNow - 4000 * kDay);
    ZT_EQ("единственная запись вечна", num(1), num(ZJournal(entries).survivors(kNow).size()));
}

void checkThinningFile(const QString& dir) {
    ZStorage h(dir);
    const QString id = QStringLiteral("прореживание");
    const QString path = h.journalPath(id);
    // Двести правок за год — случай из брифа.
    for (int i = 0; i < 200; ++i)
        append(h, id, ZJournal::Kind::Save, kNow - 365 * kDay + i * (365 * kDay / 200), noteBody(i + 1, "п"));

    ZJournal before;
    QString error;
    h.readJournal(id, &before, &error);
    ZT_EQ("записей двести", num(200), num(before.size()));
    const qint64 sizeBefore = QFile(path).size();

    ZT_TRUE("прореживание проходит", h.thinJournal(id, kNow, &error));
    ZJournal after;
    ZT_TRUE("и журнал остаётся читаемым", h.readJournal(id, &after, &error));
    ZT_TRUE("записей стало меньше", after.size() < before.size());
    ZT_TRUE("файл ужался", QFile(path).size() < sizeBefore);
    ZT_EQ("последняя запись та же", num(before.at(before.size() - 1).time()), num(after.at(after.size() - 1).time()));
    ZT_EQ("и её отпечаток тот же", before.at(before.size() - 1).digest().hex(),
          after.at(after.size() - 1).digest().hex());

    // Слепки переживают переписывание файла.
    QByteArray got;
    ZT_TRUE("слепок после прореживания цел",
            h.journalSnapshot(id, int(after.size()) - 1, &got, &error));
    ZT_EQ("и он тот самый", str(QString::fromUtf8(noteBody(200, "п"))),
          str(QString::fromUtf8(got)));

    // Идемпотентность: второй прогон не меняет ни байта.
    const auto slurp = [](const QString& p) {
        QFile f(p);
        return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
    };
    const QByteArray blob = slurp(path);
    ZT_TRUE("повторное прореживание проходит", h.thinJournal(id, kNow, &error));
    const QByteArray again = slurp(path);
    ZT_TRUE("и не меняет ни байта", blob == again);
}

// Обещание брифа: первый шаг Ctrl+Z в историю попадает в состояние «до моих
// правок», а не в вычищенное прошлое. Час-шкала обязана это удержать.
void checkFirstStepBack(const QString& dir) {
    ZStorage h(dir);
    const QString id = QStringLiteral("шаг-назад");
    const QString path = h.journalPath(id);
    // Старая история, потом сегодняшняя правка, потом моя свежая.
    for (int i = 0; i < 50; ++i) append(h, id, ZJournal::Kind::Save, kNow - 300 * kDay + i * kDay, noteBody(i + 1, "старое"));
    const QByteArray beforeMyEdits = noteBody(80, "до моих правок");
    append(h, id, ZJournal::Kind::Save, kNow - 20 * kMinute, beforeMyEdits);
    append(h, id, ZJournal::Kind::Save, kNow - kMinute, noteBody(90, "мои правки"));

    QString error;
    ZT_TRUE("прореживание проходит", h.thinJournal(id, kNow, &error));
    ZJournal read;
    h.readJournal(id, &read, &error);

    QByteArray got;
    ZT_TRUE("предпоследняя запись достаётся",
            h.journalSnapshot(id, int(read.size()) - 2, &got, &error));
    ZT_EQ("шаг назад ведёт в состояние до моих правок",
          str(QString::fromUtf8(beforeMyEdits)), str(QString::fromUtf8(got)));
}

// Поколения: полный слепок раз в kGeneration записей, между ними — звенья
// цепочки. Ради этого уклада журнал и переписан, поэтому проверяется и
// расклад кодеков, и то, что КАЖДЫЙ слепок собирается обратно.
void checkGenerations(const QString& dir) {
    ZStorage h(dir);
    const QString id = QStringLiteral("поколения");
    const QString path = h.journalPath(id);
    const int count = ZJournal::kGeneration * 3 + 5;
    QVector<QByteArray> bodies;
    QByteArray body = noteBody(300, "начало");
    for (int i = 0; i < count; ++i) {
        // Правка в середине, а не дозапись в конец: так правит человек.
        body.replace(QByteArray("- пункт 100 "), QByteArray("- ПУНКТ " + QByteArray::number(i)));
        bodies.append(body);
        ZT_EQ("запись прошла", std::string(),
              str(append(h, id, ZJournal::Kind::Save, kNow - (count - i) * kMinute, body)));
    }

    ZJournal read;
    QString error;
    ZT_TRUE("журнал читается", h.readJournal(id, &read, &error));
    ZT_EQ("записей столько, сколько писали", num(count), num(read.size()));

    int fullCount = 0;
    for (int i = 0; i < read.size(); ++i) {
        const bool shouldBeFull = i % ZJournal::kGeneration == 0;
        ZT_EQ(shouldBeFull ? "начало поколения — полный слепок"
                           : "внутри поколения — звено цепочки",
              num(int(shouldBeFull ? ZJournal::Codec::Zstd : ZJournal::Codec::ZstdDelta)),
              num(int(read.at(i).codec())));
        if (read.at(i).full()) ++fullCount;
    }
    ZT_EQ("полных слепков ровно по числу поколений", num(4), num(fullCount));

    // Каждый слепок обязан собираться, а не только последний.
    for (int i = 0; i < count; ++i) {
        QByteArray got;
        if (!h.journalSnapshot(id, i, &got, &error)) {
            ZT_TRUE(("слепок №" + std::to_string(i) + " собирается").c_str(), false);
            break;
        }
        if (got != bodies[i]) {
            ZT_EQ(("слепок №" + std::to_string(i) + " тот самый").c_str(),
                  str(QString::fromUtf8(bodies[i])), str(QString::fromUtf8(got)));
            break;
        }
    }
    ZT_TRUE("все слепки собрались", true);

    // Ради чего всё затевалось: журнал много меньше суммы отдельных слепков.
    qint64 plainTotal = 0;
    for (const QByteArray& b : bodies) plainTotal += b.size();
    ZT_TRUE("журнал вдесятеро меньше суммы слепков", QFile(path).size() * 10 < plainTotal);
}

// Порча одного звена не должна уносить журнал целиком: соседние поколения
// читаются, а испорченное — отказывается вслух.
void checkDamageContained(const QString& dir) {
    ZStorage h(dir);
    const QString id = QStringLiteral("урон");
    const QString path = h.journalPath(id);
    const int count = ZJournal::kGeneration * 2 + 4;
    QByteArray body = noteBody(200, "цел");
    for (int i = 0; i < count; ++i) {
        body.replace(QByteArray("- пункт 50 "), QByteArray("- ПУНКТ " + QByteArray::number(i)));
        append(h, id, ZJournal::Kind::Save, kNow - (count - i) * kMinute, body);
    }
    ZJournal read;
    QString error;
    h.readJournal(id, &read, &error);

    // Портим последние байты слепка записи №3 — это середина первого
    // поколения. Рамку не трогаем: если испортить длину, CBOR потеряет всё,
    // что дальше, и это уже не про цепочку.
    const int victim = 3;
    const qint64 at = read.at(victim + 1).offset() - 3;
    QFile file(path);
    ZT_TRUE("файл открылся", file.open(QIODevice::ReadWrite));
    QByteArray blob = file.readAll();
    blob[qsizetype(at)] = char(blob[qsizetype(at)] ^ 0x5a);
    file.seek(0);
    file.write(blob);
    file.close();

    QByteArray got;
    ZT_TRUE("испорченное звено отказывается", !h.journalSnapshot(id, victim, &got, &error));
    ZT_TRUE("следующее поколение читается",
            h.journalSnapshot(id, ZJournal::kGeneration, &got, &error));
    ZT_TRUE("и последняя запись читается", h.journalSnapshot(id, count - 1, &got, &error));
    ZT_TRUE("до испорченного звена тоже читается",
            h.journalSnapshot(id, victim - 1, &got, &error));
}

// Прореживание перекладывает выживших в ровные поколения заново.
void checkThinRebuildsGenerations(const QString& dir) {
    ZStorage h(dir);
    const QString id = QStringLiteral("поколения-после");
    const QString path = h.journalPath(id);
    const int count = 200;
    QVector<QByteArray> bodies;
    QByteArray body = noteBody(100, "год");
    for (int i = 0; i < count; ++i) {
        body.replace(QByteArray("- пункт 40 "), QByteArray("- ПУНКТ " + QByteArray::number(i)));
        bodies.append(body);
        append(h, id, ZJournal::Kind::Save, kNow - 365 * kDay + i * (365 * kDay / count), body);
    }
    QString error;
    ZT_TRUE("прореживание проходит", h.thinJournal(id, kNow, &error));

    ZJournal after;
    ZT_TRUE("журнал читается", h.readJournal(id, &after, &error));
    ZT_TRUE("записей стало меньше", after.size() < count);
    for (int i = 0; i < after.size(); ++i)
        ZT_EQ("поколения после прореживания ровные",
              num(int(i % ZJournal::kGeneration == 0 ? ZJournal::Codec::Zstd
                                                    : ZJournal::Codec::ZstdDelta)),
              num(int(after.at(i).codec())));

    // И слепки по-прежнему те же самые байты, что писались.
    bool allGood = true;
    for (int i = 0; i < after.size() && allGood; ++i) {
        QByteArray got;
        allGood = h.journalSnapshot(id, i, &got, &error) && bodies.contains(got);
    }
    ZT_TRUE("все выжившие слепки целы и совпадают с исходными", allGood);
}

// Единая точка синхронизации на деле: пока один поток прореживает, другой
// дописывает. Журнал обязан остаться целым, а последняя запись — той, что
// дописали последней. Какие записи выкинет прореживание — его дело, а вот
// испортить или потерять свежую он права не имеет.
void checkConcurrency(const QString& dir) {
    ZStorage h(dir);
    const QString id = QStringLiteral("вдвоём");
    const QString path = h.journalPath(id);
    QByteArray body = noteBody(150, "начало");
    for (int i = 0; i < 120; ++i) {
        body.replace(QByteArray("- пункт 70 "), QByteArray("- ПУНКТ " + QByteArray::number(i)));
        append(h, id, ZJournal::Kind::Save, kNow - 400 * kDay + i * kDay, body);
    }

    std::atomic<int> thinRuns{0};
    std::thread thinner([&h, &id, &thinRuns] {
        for (int i = 0; i < 30; ++i) {
            QString error;
            h.thinJournal(id, kNow + i * kMinute, &error);
            ++thinRuns;
        }
    });

    QByteArray last;
    for (int i = 0; i < 60; ++i) {
        body.replace(QByteArray("- пункт 20 "), QByteArray("- СВЕЖЕЕ " + QByteArray::number(i)));
        last = body;
        const QString error = append(h, id, ZJournal::Kind::Save, kNow + kHour + i * 1000, body);
        if (!error.isEmpty()) {
            ZT_EQ("дозапись рядом с прореживанием", std::string(), str(error));
            break;
        }
    }
    thinner.join();
    ZT_TRUE("прореживание отработало", thinRuns.load() == 30);

    ZJournal read;
    QString error;
    ZT_TRUE("журнал цел после совместной работы", h.readJournal(id, &read, &error));
    ZT_TRUE("хвост не оборван", !read.tailTrimmed());
    ZT_TRUE("записи на месте", read.size() > 0);

    QByteArray got;
    ZT_TRUE("последний слепок достаётся",
            h.journalSnapshot(id, int(read.size()) - 1, &got, &error));
    ZT_EQ("и это то, что дописали последним", str(QString::fromUtf8(last)),
          str(QString::fromUtf8(got)));

    // И ни одна уцелевшая запись не должна разъехаться со своим отпечатком.
    bool allGood = true;
    for (int i = 0; i < read.size() && allGood; ++i)
        allGood = h.journalSnapshot(id, i, &got, &error);
    ZT_TRUE("все уцелевшие слепки собираются", allGood);
}

}  // namespace

static int ztRunSuite(int argc, char** argv) {
    (void)argc;
    (void)argv;
    QTemporaryDir dir;
    QDir().mkpath(QDir(dir.path()).filePath(QStringLiteral("history")));

    checkRoundTrip(dir.path());
    checkRevisionsSurviveThinning(dir.path());
    checkCorruption(dir.path());
    checkTornTail(dir.path());
    checkForeignFile(dir.path());
    checkThinningScale();
    checkThinningFile(dir.path());
    checkGenerations(dir.path());
    checkDamageContained(dir.path());
    checkThinRebuildsGenerations(dir.path());
    checkConcurrency(dir.path());
    checkFirstStepBack(dir.path());

    return zt::report("journal");
}

// Набор целиком одним TEST: тело не тронуто, argv ему собран здесь.
// Дробить на отдельные проверки — отдельная работа, по одному набору.
TEST(Journal, All) {
    std::vector<QByteArray> ztArgs{QByteArrayLiteral("journal_test")};
    std::vector<char*> ztArgv;
    for (QByteArray& a : ztArgs) ztArgv.push_back(a.data());
    EXPECT_EQ(0, ztRunSuite(int(ztArgv.size()), ztArgv.data()));
}
