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

#include "test_util.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <string>

using namespace zametti;
using zametti::journal::Kind;

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

QString append(const QString& path, Kind kind, qint64 time, const QByteArray& body,
               qint64 source = 0) {
    QString error;
    if (!journal::append(path, kind, time, body, source, &error)) return error;
    return {};
}

void checkRoundTrip(const QString& dir) {
    const QString path = journal::journalPath(dir, QStringLiteral("01n6r08s8wy52h"));
    ZT_EQ("пути журнала", str(QDir(dir).filePath("history/01n6r08s8wy52h.log")), str(path));

    journal::Journal empty;
    QString error;
    ZT_TRUE("журнала ещё нет — это не беда", journal::read(path, &empty, &error));
    ZT_EQ("и записей в нём ноль", num(0), num(empty.entries.size()));

    const QByteArray first = noteBody(10, "раз");
    const QByteArray second = noteBody(12, "два");
    ZT_EQ("первая запись", std::string(), str(append(path, Kind::Save, kNow - 3 * kHour, first)));
    const qint64 afterFirst = QFile(path).size();
    ZT_EQ("вторая запись", std::string(),
          str(append(path, Kind::External, kNow - 2 * kHour, second)));
    ZT_TRUE("инвариант B: файл вырос", QFile(path).size() > afterFirst);
    ZT_EQ("восстановление", std::string(),
          str(append(path, Kind::Restore, kNow - kHour, first, kNow - 3 * kHour)));
    ZT_EQ("надгробие", std::string(),
          str(append(path, Kind::Tombstone, kNow - kMinute, QByteArray())));

    journal::Journal read;
    ZT_TRUE("журнал читается", journal::read(path, &read, &error));
    ZT_EQ("ошибки нет", std::string(), str(error));
    ZT_EQ("записей четыре", num(4), num(read.entries.size()));
    ZT_TRUE("хвост цел", !read.tailTrimmed);

    ZT_EQ("вид первой", num(int(Kind::Save)), num(int(read.entries[0].kind)));
    ZT_EQ("вид второй", num(int(Kind::External)), num(int(read.entries[1].kind)));
    ZT_EQ("вид третьей", num(int(Kind::Restore)), num(int(read.entries[2].kind)));
    ZT_EQ("вид четвёртой", num(int(Kind::Tombstone)), num(int(read.entries[3].kind)));
    ZT_EQ("время второй", num(kNow - 2 * kHour), num(read.entries[1].time));
    ZT_EQ("источник восстановления", num(kNow - 3 * kHour), num(read.entries[2].source));
    ZT_EQ("источник обычной записи не пишется", num(0LL), num(read.entries[0].source));
    ZT_EQ("размер до сжатия", num(qint64(second.size())), num(read.entries[1].plainSize));
    ZT_TRUE("слепок в файле меньше исходного", read.entries[1].packedSize < second.size());
    ZT_TRUE("у надгробия слепка нет", !read.entries[3].hasSnapshot());

    QByteArray got;
    ZT_TRUE("слепок первой достаётся", journal::snapshotAt(path, 0, &got, &error));
    ZT_EQ("и он тот самый", str(QString::fromUtf8(first)), str(QString::fromUtf8(got)));
    ZT_TRUE("слепок второй достаётся", journal::snapshotAt(path, 1, &got, &error));
    ZT_EQ("и он тот самый", str(QString::fromUtf8(second)), str(QString::fromUtf8(got)));
    ZT_TRUE("у надгробия слепка не спросить", !journal::snapshotAt(path, 3, &got, &error));
    ZT_TRUE("и сказано почему", error.contains(QStringLiteral("tombstone")));
    ZT_TRUE("за границей записей — отказ", !journal::snapshotAt(path, 99, &got, &error));

    // Отпечаток считается от распакованных байтов и сверяется всегда.
    ZT_EQ("отпечаток первой", hashOf(std::string_view(first.constData(), size_t(first.size()))).hex(),
          read.entries[0].digest.hex());
}

// Порча в середине слепка обязана быть замечена: молча отданные не те байты
// хуже, чем отказ.
void checkCorruption(const QString& dir) {
    const QString path = journal::journalPath(dir, QStringLiteral("порча"));
    const QByteArray body = noteBody(200, "текст");
    append(path, Kind::Save, kNow - kHour, body);

    QFile file(path);
    ZT_TRUE("файл открылся", file.open(QIODevice::ReadWrite));
    QByteArray blob = file.readAll();
    blob[blob.size() / 2] = char(blob[blob.size() / 2] ^ 0x5a);
    file.seek(0);
    file.write(blob);
    file.close();

    QByteArray got;
    QString error;
    ZT_TRUE("испорченный слепок не отдаётся", !journal::snapshotAt(path, 0, &got, &error));
    ZT_TRUE("и сказано, что именно не так",
            error.contains(QStringLiteral("не распаковывается")) ||
                error.contains(QStringLiteral("не сходится")));
}

// Главный случай отказа от fsync: питание пропало посреди записи.
void checkTornTail(const QString& dir) {
    const QString path = journal::journalPath(dir, QStringLiteral("обрыв"));
    const QByteArray a = noteBody(10, "а");
    const QByteArray b = noteBody(20, "б");
    append(path, Kind::Save, kNow - 2 * kHour, a);
    const qint64 whole = QFile(path).size();
    append(path, Kind::Save, kNow - kHour, b);
    const qint64 both = QFile(path).size();

    // Обрываем вторую запись на середине — так и выглядит падение.
    QFile file(path);
    ZT_TRUE("файл открылся", file.open(QIODevice::ReadWrite));
    ZT_TRUE("хвост обрезан руками", file.resize(whole + (both - whole) / 2));
    file.close();

    journal::Journal read;
    QString error;
    ZT_TRUE("журнал с оборванным хвостом открывается", journal::read(path, &read, &error));
    ZT_EQ("целая часть цела", num(1), num(read.entries.size()));
    ZT_TRUE("и про хвост сказано", read.tailTrimmed);
    ZT_EQ("граница целого — конец первой записи", num(whole), num(read.goodBytes));

    QByteArray got;
    ZT_TRUE("слепок целой записи достаётся", journal::snapshotAt(path, 0, &got, &error));
    ZT_EQ("и он тот самый", str(QString::fromUtf8(a)), str(QString::fromUtf8(got)));

    ZT_TRUE("хвост отрезается", journal::trimTail(path, &error));
    ZT_EQ("файл укоротился до целого", num(whole), num(QFile(path).size()));
    ZT_TRUE("после обрезки читается начисто", journal::read(path, &read, &error));
    ZT_TRUE("и хвоста больше нет", !read.tailTrimmed);

    // Дописывать после обрезки можно как ни в чём не бывало.
    ZT_EQ("дозапись после обрыва", std::string(), str(append(path, Kind::Save, kNow, b)));
    journal::read(path, &read, &error);
    ZT_EQ("записей снова две", num(2), num(read.entries.size()));
}

void checkForeignFile(const QString& dir) {
    QString error;
    journal::Journal read;

    const QString junk = QDir(dir).filePath(QStringLiteral("junk.log"));
    QFile file(junk);
    ZT_TRUE("файл создан", file.open(QIODevice::WriteOnly));
    file.write("это вообще не журнал, а просто текст");
    file.close();
    ZT_TRUE("чужой файл отвергается", !journal::read(junk, &read, &error));
    ZT_TRUE("и сказано, что он не наш", error.contains(QStringLiteral("не журнал")));

    // Пустой файл — это пустой журнал: так выглядит только что созданный.
    const QString blank = QDir(dir).filePath(QStringLiteral("blank.log"));
    { QFile f(blank); ZT_TRUE("пустой файл создан", f.open(QIODevice::WriteOnly)); }
    ZT_TRUE("пустой файл — пустой журнал", journal::read(blank, &read, &error));
    ZT_EQ("и записей ноль", num(0), num(read.entries.size()));

    // Журнал из будущего: версия, которой мы не знаем. Молча пропустить чужую
    // историю хуже, чем сказать вслух.
    const QString future = QDir(dir).filePath(QStringLiteral("future.log"));
    append(future, Kind::Save, kNow, noteBody(3, "будущее"));
    QFile ff(future);
    ZT_TRUE("файл открылся", ff.open(QIODevice::ReadWrite));
    QByteArray blob = ff.readAll();
    // Шапка — карта {1: "zametti-journal", 2: 1}; последний байт версии.
    const int versionAt = blob.indexOf("zametti-journal") + 15 + 1;
    blob[versionAt] = char(9);
    ff.seek(0);
    ff.write(blob);
    ff.close();
    ZT_TRUE("журнал незнакомой версии отвергается", !journal::read(future, &read, &error));
    ZT_TRUE("и версия названа", error.contains(QStringLiteral("версии 9")));
}

// Шкала прореживания. Времена задаются напрямую, чтобы проверять правило, а
// не сжатие.
void checkThinningScale() {
    QVector<journal::Entry> entries;
    auto add = [&entries](qint64 time) {
        journal::Entry e;
        e.time = time;
        entries.append(e);
    };

    // Последний час — всё до единой.
    for (int i = 10; i >= 1; --i) add(kNow - i * kMinute);
    ZT_EQ("последний час не прореживается", num(10), num(journal::survivors(entries, kNow).size()));

    // Сутки — не чаще раза в минуту: три записи внутри одной минуты схлопнутся
    // в одну, и это будет последняя из них.
    entries.clear();
    add(kNow - 5 * kHour);
    add(kNow - 5 * kHour + 1000);
    add(kNow - 5 * kHour + 2000);
    add(kNow - 5 * kHour + 3 * kMinute);
    add(kNow - kMinute);
    QVector<int> keep = journal::survivors(entries, kNow);
    ZT_EQ("минута схлопывается в одну запись", num(3), num(keep.size()));
    ZT_EQ("остаётся последняя в минуте", num(2), num(keep[0]));

    // Неделя — раз в час: шесть записей через полчаса покрывают три часа и
    // схлопываются в три, плюс свежая последняя.
    entries.clear();
    for (int i = 0; i < 6; ++i) add(kNow - 3 * kDay + i * 30 * kMinute);
    add(kNow);
    keep = journal::survivors(entries, kNow);
    ZT_EQ("три часа дают три записи плюс последняя", num(4), num(keep.size()));

    // Дальше месяца — раз в месяц; и последняя запись выживает всегда.
    entries.clear();
    for (int i = 0; i < 40; ++i) add(kNow - 400 * kDay + i * kDay);
    keep = journal::survivors(entries, kNow);
    ZT_TRUE("год назад остаётся горстка", keep.size() <= 3);
    ZT_EQ("последняя запись на месте", num(entries.size() - 1), num(keep.last()));

    // Одна-единственная запись не прореживается никогда — для удалённой
    // заметки это её вечный финальный слепок.
    entries.clear();
    add(kNow - 4000 * kDay);
    ZT_EQ("единственная запись вечна", num(1), num(journal::survivors(entries, kNow).size()));
}

void checkThinningFile(const QString& dir) {
    const QString path = journal::journalPath(dir, QStringLiteral("прореживание"));
    // Двести правок за год — случай из брифа.
    for (int i = 0; i < 200; ++i)
        append(path, Kind::Save, kNow - 365 * kDay + i * (365 * kDay / 200), noteBody(i + 1, "п"));

    journal::Journal before;
    QString error;
    journal::read(path, &before, &error);
    ZT_EQ("записей двести", num(200), num(before.entries.size()));
    const qint64 sizeBefore = QFile(path).size();

    ZT_TRUE("прореживание проходит", journal::thin(path, kNow, &error));
    journal::Journal after;
    ZT_TRUE("и журнал остаётся читаемым", journal::read(path, &after, &error));
    ZT_TRUE("записей стало меньше", after.entries.size() < before.entries.size());
    ZT_TRUE("файл ужался", QFile(path).size() < sizeBefore);
    ZT_EQ("последняя запись та же", num(before.entries.last().time), num(after.entries.last().time));
    ZT_EQ("и её отпечаток тот же", before.entries.last().digest.hex(),
          after.entries.last().digest.hex());

    // Слепки переживают переписывание файла.
    QByteArray got;
    ZT_TRUE("слепок после прореживания цел",
            journal::snapshotAt(path, int(after.entries.size()) - 1, &got, &error));
    ZT_EQ("и он тот самый", str(QString::fromUtf8(noteBody(200, "п"))),
          str(QString::fromUtf8(got)));

    // Идемпотентность: второй прогон не меняет ни байта.
    const auto slurp = [](const QString& p) {
        QFile f(p);
        return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
    };
    const QByteArray blob = slurp(path);
    ZT_TRUE("повторное прореживание проходит", journal::thin(path, kNow, &error));
    const QByteArray again = slurp(path);
    ZT_TRUE("и не меняет ни байта", blob == again);
}

// Обещание брифа: первый шаг Ctrl+Z в историю попадает в состояние «до моих
// правок», а не в вычищенное прошлое. Час-шкала обязана это удержать.
void checkFirstStepBack(const QString& dir) {
    const QString path = journal::journalPath(dir, QStringLiteral("шаг-назад"));
    // Старая история, потом сегодняшняя правка, потом моя свежая.
    for (int i = 0; i < 50; ++i) append(path, Kind::Save, kNow - 300 * kDay + i * kDay, noteBody(i + 1, "старое"));
    const QByteArray beforeMyEdits = noteBody(80, "до моих правок");
    append(path, Kind::Save, kNow - 20 * kMinute, beforeMyEdits);
    append(path, Kind::Save, kNow - kMinute, noteBody(90, "мои правки"));

    QString error;
    ZT_TRUE("прореживание проходит", journal::thin(path, kNow, &error));
    journal::Journal read;
    journal::read(path, &read, &error);

    QByteArray got;
    ZT_TRUE("предпоследняя запись достаётся",
            journal::snapshotAt(path, int(read.entries.size()) - 2, &got, &error));
    ZT_EQ("шаг назад ведёт в состояние до моих правок",
          str(QString::fromUtf8(beforeMyEdits)), str(QString::fromUtf8(got)));
}

}  // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir dir;
    QDir().mkpath(QDir(dir.path()).filePath(QStringLiteral("history")));

    checkRoundTrip(dir.path());
    checkCorruption(dir.path());
    checkTornTail(dir.path());
    checkForeignFile(dir.path());
    checkThinningScale();
    checkThinningFile(dir.path());
    checkFirstStepBack(dir.path());

    return zt::report("journal");
}
