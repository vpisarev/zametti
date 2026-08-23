// ПОРЯДОК ЗАПИСЕЙ И ЧАСЫ.
//
// Что здесь проверяется и почему именно это:
//   - свойства компаратора: антисимметрия, транзитивность, тотальность. Без
//     них порядок не тотален, сортировка недетерминирована, и два устройства
//     расходятся молча;
//   - миграция: у старых записей ревизии нет, между собой они сравниваются по
//     времени — ровно как раньше, — а первая новая правка законно становится
//     головой;
//   - отравленная голова: запись «из будущего» бьётся первой же каузально
//     поздней правкой, а не ждёт наступления своей даты;
//   - тайбрейк «контент старше надгробия»: правка побеждает удаление;
//   - страж времени: часы, переведённые назад, не инвертируют свои же записи;
//     потеря пола устройства не ломает ничего; названный момент (время файла у
//     опорной записи) страж НЕ поднимает.

#include "journal.h"

#include "mini_store.h"
#include "test_util.h"

#include <QByteArray>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <string>
#include <vector>

using namespace zametti;
using Entry = zametti::journal::ZJournal::Entry;
using zametti::journal::Kind;
using zametti::journal::NewRecord;
using zametti::journal::Stamp;
using zametti::journal::ZJournal;

namespace {

std::string str(const QString& s) { return s.toStdString(); }
template <typename T>
std::string num(T value) { return std::to_string(value); }

constexpr qint64 kNow = 1'700'000'000'000LL;
constexpr qint64 kHour = 60 * 60 * 1000LL;
constexpr qint64 kDay = 24 * kHour;

QByteArray note(const char* mark) {
    return QByteArray("<!-- zametti\nversion: 1\n-->\n\n# Заметка\n\n") + mark + "\n";
}

Digest digestOf(const char* mark) {
    const QByteArray body = note(mark);
    return hashOf(std::string_view(body.constData(), size_t(body.size())));
}

Entry entry(Kind kind, qint64 time, qint64 seq, const char* mark) {
    return Entry(kind, time, seq, digestOf(mark));
}

// --- 1. Компаратор: тотальный порядок, а не «как получится» ----------------

void checkComparatorIsTotalOrder() {
    // Набор нарочно вырожденный: одинаковые времена, одинаковые ревизии,
    // надгробия вперемешку с содержательными записями.
    std::vector<Entry> all;
    for (qint64 seq : {0LL, 1LL, 2LL})
        for (qint64 t : {kNow, kNow + 1})
            for (const char* mark : {"раз", "два"}) {
                all.push_back(entry(Kind::Save, t, seq, mark));
                all.push_back(entry(Kind::Tombstone, t, seq, mark));
            }

    bool antisymmetric = true;
    bool total = true;
    for (const Entry& a : all)
        for (const Entry& b : all) {
            const bool ab = a.isBefore(b);
            const bool ba = b.isBefore(a);
            if (ab && ba) antisymmetric = false;
            // Тотальность: неразличимыми могут быть только записи с полностью
            // равным ключом — то есть равные во всём, что мы сравниваем.
            if (!ab && !ba) {
                const bool sameKey = a.seq() == b.seq() && a.time() == b.time() &&
                                     a.hasSnapshot() == b.hasSnapshot() &&
                                     a.digest() == b.digest();
                if (!sameKey) total = false;
            }
        }
    ZT_TRUE("порядок антисимметричен", antisymmetric);
    ZT_TRUE("порядок тотален: несравнимых пар нет", total);

    bool transitive = true;
    for (const Entry& a : all)
        for (const Entry& b : all)
            for (const Entry& c : all)
                if (a.isBefore(b) && b.isBefore(c) && !a.isBefore(c)) transitive = false;
    ZT_TRUE("порядок транзитивен", transitive);
}

// --- 2. Старые записи: между собой — по времени ----------------------------

void checkLegacyOrderIsByTime() {
    // Ревизии нет ни у одной (журнал до этапа 17), времена вразнобой.
    ZJournal legacy(QVector<Entry>{entry(Kind::Save, kNow + 10, 0, "раз"),
                                   entry(Kind::Save, kNow + 30, 0, "два"),
                                   entry(Kind::Save, kNow + 20, 0, "три")});
    ZT_EQ("голова старого журнала — самая поздняя по времени", num(1),
          num(legacy.headIndex()));
    ZT_EQ("следующая ревизия у него первая", num(1LL), num(legacy.nextSeq()));
}

// --- 3. Отравленная голова: ревизия бьёт время -----------------------------

void checkPoisonedHeadLosesToFreshEdit() {
    // Часы ушли на сутки вперёд, запись «из будущего» осела в журнале.
    ZJournal poisoned(QVector<Entry>{entry(Kind::Save, kNow, 1, "раз"),
                                     entry(Kind::Save, kNow + kDay, 2, "будущее")});
    ZT_EQ("пока голова — та, что из будущего", num(1), num(poisoned.headIndex()));

    // Приехала каузально более поздняя правка. Её время — честное «сейчас»,
    // то есть МЕНЬШЕ отравленного, а ревизия больше.
    ZJournal fixed(QVector<Entry>{entry(Kind::Save, kNow, 1, "раз"),
                                  entry(Kind::Save, kNow + kDay, 2, "будущее"),
                                  entry(Kind::Save, kNow + 1000, 3, "правка")});
    ZT_EQ("голова сменилась сразу, а не через сутки", num(2), num(fixed.headIndex()));
}

// --- 4. Тайбрейк: контент старше надгробия ---------------------------------

void checkEditBeatsDeleteOnEqualKey() {
    const Entry content = entry(Kind::Save, kNow, 7, "раз");
    const Entry grave = entry(Kind::Tombstone, kNow, 7, "раз");
    ZT_TRUE("при полностью равном ключе контент считается более ранним",
            content.isBefore(grave));
    ZT_TRUE("и обратное неверно", !grave.isBefore(content));

    ZJournal both(QVector<Entry>{content, grave});
    ZT_EQ("значит голова — надгробие, пока правки поверх него нет", num(1),
          num(both.headIndex()));

    ZJournal edited(QVector<Entry>{content, grave, entry(Kind::Save, kNow, 8, "после")});
    ZT_EQ("а правка поверх удаления побеждает", num(2), num(edited.headIndex()));
}

// --- 5. Часы переведены назад: свои записи не инвертируются ----------------

void checkClockBackDoesNotInvert() {
    zt::MiniStore store;
    journal::History history(store.root());
    const QString id = QStringLiteral("01n7clockback0");
    QString error;

    // Так выглядит машина, на которой часы ушли на три часа НАЗАД: предыдущие
    // записи этого устройства сделаны «в будущем» относительно нынешних часов.
    const qint64 nowReal = QDateTime::currentMSecsSinceEpoch();
    store.setDeviceClock(nowReal + 3 * kHour);

    ZT_TRUE("запись проходит",
            history.append(id, NewRecord::save(note("после перевода"), Stamp::now()), &error));

    ZJournal read;
    ZT_TRUE("журнал читается", history.read(id, &read, &error));
    ZT_EQ("записана одна", num(1), num(read.size()));
    ZT_TRUE("и её время НЕ раньше прежнего пола устройства",
            read.at(0).time() > nowReal + 3 * kHour);
    ZT_EQ("пол устройства поднялся до неё", num(read.at(0).time()), num(store.deviceClock()));

    // Вторая запись — тем же перевёрнутым часам вопреки — обязана лечь позже.
    ZT_TRUE("вторая запись проходит",
            history.append(id, NewRecord::save(note("ещё правка"), Stamp::now()), &error));
    ZT_TRUE("журнал читается", history.read(id, &read, &error));
    ZT_TRUE("порядок не инвертирован", read.at(0).isBefore(read.at(1)));
    ZT_EQ("и голова — свежая правка", num(1), num(read.headIndex()));
}

// --- 6. Потеря пола устройства ---------------------------------------------

void checkLostDeviceClockStillWorks() {
    zt::MiniStore store;
    journal::History history(store.root());
    const QString id = QStringLiteral("01n7clocklost0");
    QString error;

    ZT_TRUE("первая запись", history.append(id, NewRecord::save(note("раз"), Stamp::now()), &error));
    store.dropDeviceClock();   // число потеряли: чужая копия каталога, чистка, что угодно
    ZT_EQ("пола нет", num(0LL), num(store.deviceClock()));

    ZT_TRUE("вторая запись проходит и без пола",
            history.append(id, NewRecord::save(note("два"), Stamp::now()), &error));

    ZJournal read;
    ZT_TRUE("журнал читается", history.read(id, &read, &error));
    ZT_EQ("записей две", num(2), num(read.size()));
    // Пол журнала выводится из самих записей и потеряться не может — поэтому
    // порядок внутри заметки цел даже при стёртом last-written.
    ZT_TRUE("порядок внутри заметки цел", read.at(0).isBefore(read.at(1)));
}

// --- 6б. Пол журнала держит порядок и у названных моментов ------------------

void checkJournalFloorHoldsNamedMoments() {
    zt::MiniStore store;
    journal::History history(store.root());
    const QString id = QStringLiteral("01n7floorown00");
    QString error;

    ZT_TRUE("первая запись",
            history.append(id, NewRecord::save(note("раз"), Stamp::at(kNow)), &error));
    // Названный момент ИЗ ПРОШЛОГО в непустой журнал: пол устройства к нему не
    // применяется, а пол самого журнала — применяется, иначе запись легла бы в
    // файл раньше своей предшественницы и порядок разошёлся бы с укладкой.
    ZT_TRUE("вторая запись с временем из прошлого",
            history.append(id, NewRecord::save(note("два"), Stamp::at(kNow - kHour)), &error));

    ZJournal read;
    ZT_TRUE("журнал читается", history.read(id, &read, &error));
    ZT_EQ("время второй поднято до «сразу после первой»", num(kNow + 1), num(read.at(1).time()));
    ZT_TRUE("порядок совпадает с укладкой", read.at(0).isBefore(read.at(1)));
}

// --- 7. Названный момент страж не поднимает --------------------------------

void checkNamedMomentSurvivesGuard() {
    zt::MiniStore store;
    journal::History history(store.root());
    const QString id = QStringLiteral("01n7oldnote000");
    QString error;

    // Пол устройства высокий: сегодня на этой машине уже писали.
    store.setDeviceClock(QDateTime::currentMSecsSinceEpoch());

    // А опорной записи отдают время ФАЙЛА — заметка лежит с 2017 года.
    const qint64 y2017 = 1'497'859'669'000LL;
    ZT_TRUE("опорная запись проходит",
            history.append(id, NewRecord::save(note("старая"), Stamp::at(y2017)), &error));

    ZJournal read;
    ZT_TRUE("журнал читается", history.read(id, &read, &error));
    ZT_EQ("время опорной записи — то самое, названное", num(y2017), num(read.at(0).time()));
}

// --- 8. Старый журнал на диске + новая правка ------------------------------

void checkLegacyJournalOnDisk() {
    zt::MiniStore store;
    journal::History history(store.root());
    const QString id = QStringLiteral("01n7legacylog0");
    QString error;

    // Журнал, написанный прежней сборкой: ревизий в записях нет вовсе.
    store.appendLegacyRecord(id, Kind::Save, kNow, note("раз"));
    store.appendLegacyRecord(id, Kind::Save, kNow + 60'000, note("два"));

    ZJournal read;
    ZT_TRUE("старый журнал читается", history.read(id, &read, &error));
    ZT_EQ("записей две", num(2), num(read.size()));
    ZT_EQ("ревизия первой — ноль", num(0LL), num(read.at(0).seq()));
    ZT_EQ("ревизия второй — тоже", num(0LL), num(read.at(1).seq()));
    ZT_EQ("голова — поздняя по времени", num(1), num(read.headIndex()));

    // Первая новая правка законно становится головой.
    ZT_TRUE("новая запись проходит",
            history.append(id, NewRecord::save(note("три"), Stamp::at(kNow + 120'000)), &error));
    ZT_TRUE("журнал читается", history.read(id, &read, &error));
    ZT_EQ("у новой записи ревизия первая", num(1LL), num(read.at(2).seq()));
    ZT_EQ("и голова теперь она", num(2), num(read.headIndex()));
    ZT_TRUE("порядок старых записей не тронут", read.at(0).isBefore(read.at(1)));

    // Слепки старых записей собираются: формат тот же, ключ добавочный.
    QByteArray got;
    ZT_TRUE("слепок старой записи собирается", history.snapshotAt(id, 0, &got, &error));
    ZT_EQ("и он тот самый", str(QString::fromUtf8(note("раз"))), str(QString::fromUtf8(got)));
}

// --- 9. Гашение: хвостом, серединой, повторно -------------------------------

// Кого видно человеку: записи, которые говорят о содержимом и не погашены.
std::string waypoints(const ZJournal& j) {
    std::string out;
    for (int i = 0; i < j.size(); ++i) {
        if (!j.at(i).statesContent() || j.isVoided(i)) continue;
        if (!out.empty()) out += " ";
        out += std::to_string(i);
    }
    return out;
}

void checkVoidingTail() {
    zt::MiniStore store;
    journal::History history(store.root());
    const QString id = QStringLiteral("01n7voidtail00");
    QString error;

    ZT_TRUE("первая", history.append(id, NewRecord::save(note("раз"), Stamp::at(kNow)), &error));
    ZT_TRUE("вторая",
            history.append(id, NewRecord::save(note("два"), Stamp::at(kNow + 1000)), &error));

    ZJournal read;
    history.read(id, &read, &error);
    const journal::EntryRef doomed(read.at(1).time(), read.at(1).digest());
    const qint64 sizeBefore = QFileInfo(store.journalOf(id)).size();

    // Третья запись гасит вторую — та лежит хвостом, значит её байты уходят.
    ZT_TRUE("третья гасит вторую",
            history.append(id,
                           NewRecord::save(note("три"), Stamp::at(kNow + 2000))
                               .voiding({doomed}),
                           &error));

    ZT_TRUE("журнал читается", history.read(id, &read, &error));
    ZT_EQ("записей две: погашенная выкинута из файла", num(2), num(read.size()));
    ZT_TRUE("файл не вырос", QFileInfo(store.journalOf(id)).size() <= sizeBefore * 2);
    ZT_EQ("вешки — обе оставшиеся", std::string("0 1"), waypoints(read));
    ZT_EQ("голова — третья", num(1), num(read.headIndex()));
    ZT_EQ("и адрес погашенной она несёт", num(1), num(read.at(1).voids().size()));

    // Слепок оставшейся собирается: цепочка поколений не порвана.
    QByteArray got;
    ZT_TRUE("слепок головы собирается", history.snapshotAt(id, 1, &got, &error));
    ZT_EQ("и он тот самый", str(QString::fromUtf8(note("три"))), str(QString::fromUtf8(got)));
}

void checkVoidingMiddle() {
    zt::MiniStore store;
    journal::History history(store.root());
    const QString id = QStringLiteral("01n7voidmid000");
    QString error;

    for (const char* mark : {"раз", "два", "три"})
        history.append(id, NewRecord::save(note(mark), Stamp::at(kNow + 1000 * (*mark))), &error);

    ZJournal read;
    history.read(id, &read, &error);
    ZT_EQ("записей три", num(3), num(read.size()));
    const journal::EntryRef middle(read.at(1).time(), read.at(1).digest());

    // Гасим СЕРЕДИНУ — так будет выглядеть гашение записи, приехавшей со
    // стороны. Вырезать её на месте нельзя: звенья поколения считаются от
    // предыдущего слепка, поэтому файл пересобирается целиком.
    ZT_TRUE("четвёртая гасит середину",
            history.append(id, NewRecord::save(note("четыре"), Stamp::at(kNow + 9000))
                                   .voiding({middle}),
                           &error));

    ZT_TRUE("журнал читается", history.read(id, &read, &error));
    ZT_EQ("записей три: середина ушла, новая пришла", num(3), num(read.size()));
    QByteArray got;
    ZT_TRUE("слепок первой цел", history.snapshotAt(id, 0, &got, &error));
    ZT_EQ("и он тот самый", str(QString::fromUtf8(note("раз"))), str(QString::fromUtf8(got)));
    ZT_TRUE("слепок бывшей третьей цел", history.snapshotAt(id, 1, &got, &error));
    ZT_EQ("и он тот самый", str(QString::fromUtf8(note("три"))), str(QString::fromUtf8(got)));
}

void checkVoidedRecordStaysVoidedWhenItComesBack() {
    zt::MiniStore store;
    journal::History history(store.root());
    const QString id = QStringLiteral("01n7voidback00");
    QString error;

    history.append(id, NewRecord::save(note("раз"), Stamp::at(kNow)), &error);
    history.append(id, NewRecord::save(note("два"), Stamp::at(kNow + 1000)), &error);
    ZJournal read;
    history.read(id, &read, &error);
    const journal::EntryRef doomed(read.at(1).time(), read.at(1).digest());
    history.append(id, NewRecord::save(note("три"), Stamp::at(kNow + 2000)).voiding({doomed}),
                   &error);

    // ТАК ВЫГЛЯДИТ ОБЪЕДИНЕНИЕ: погашенная запись приезжает обратно с копии,
    // которая про гашение ещё не знала. Адрес у неё тот же — значит она
    // гасится повторно и человеку не показывается.
    store.appendLegacyRecord(id, Kind::Save, doomed.time(), note("два"));

    ZT_TRUE("журнал читается", history.read(id, &read, &error));
    ZT_EQ("записей стало три", num(3), num(read.size()));
    ZT_TRUE("вернувшаяся погашена", read.isVoided(2));
    ZT_EQ("вешки прежние", std::string("0 1"), waypoints(read));
    ZT_EQ("и голова прежняя", num(1), num(read.headIndex()));
}

void checkAmendmentHidesWithoutStatingContent() {
    zt::MiniStore store;
    journal::History history(store.root());
    const QString id = QStringLiteral("01n7amend00000");
    QString error;

    history.append(id, NewRecord::save(note("раз"), Stamp::at(kNow)), &error);
    history.append(id, NewRecord::save(note("два"), Stamp::at(kNow + 1000)), &error);
    ZJournal read;
    history.read(id, &read, &error);
    const journal::EntryRef doomed(read.at(1).time(), read.at(1).digest());

    // «Набрал и отменил»: нового слепка нет, а сказать «того, что между, больше
    // нет» надо.
    ZT_TRUE("гашение пишется",
            history.append(id, NewRecord::amendment(Stamp::at(kNow + 2000)).voiding({doomed}),
                           &error));

    ZT_TRUE("журнал читается", history.read(id, &read, &error));
    ZT_EQ("в файле две записи: первая и гашение", num(2), num(read.size()));
    ZT_EQ("вешка одна — первая", std::string("0"), waypoints(read));
    ZT_EQ("голова — первая, гашение головой быть не может", num(0), num(read.headIndex()));
    ZT_EQ("и последний слепок — её же", num(0), num(read.lastSnapshotIndex()));
    ZT_TRUE("у гашения слепка нет", !read.at(1).hasSnapshot());
}

void checkUnnamedConcurrentEditSurvives() {
    // Конкурентная правка с другого устройства НЕ названа в списке гашения —
    // значит она жива. Это то, что порог по ревизии убил бы молча.
    ZJournal j(QVector<Entry>{entry(Kind::Save, kNow, 1, "раз"),
                              entry(Kind::Save, kNow + 1000, 2, "два"),
                              Entry(Kind::Save, kNow + 1500, 2, digestOf("чужая")),
                              Entry(Kind::Save, kNow + 2000, 3, digestOf("три"), 0,
                                    QVector<journal::EntryRef>{
                                        journal::EntryRef(kNow + 1000, digestOf("два"))})});
    ZT_TRUE("названная погашена", j.isVoided(1));
    ZT_TRUE("чужая, не названная, жива", !j.isVoided(2));
    ZT_EQ("вешки — все, кроме погашенной", std::string("0 2 3"), waypoints(j));
}

// --- 10. Целостность рамки --------------------------------------------------

void checkBrokenFrameIsNotADeletion() {
    zt::MiniStore store;
    journal::History history(store.root());
    const QString id = QStringLiteral("01n7frame00000");
    QString error;

    history.append(id, NewRecord::save(note("раз"), Stamp::at(kNow)), &error);
    history.append(id, NewRecord::save(note("два"), Stamp::at(kNow + 1000)), &error);

    ZJournal read;
    ZT_TRUE("журнал читается", history.read(id, &read, &error));
    ZT_EQ("записей две", num(2), num(read.size()));
    ZT_EQ("голова — вторая", num(1), num(read.headIndex()));

    // РОД ЗАПИСИ ЛЕЖИТ ОДНИМ БАЙТОМ. В карте он первый: заголовок карты, ключ
    // 1, значение. Перевернуть в нём два бита — и правка (1) становится
    // надгробием (4). Без контрольной суммы рамки это уехало бы в облако как
    // законное удаление, а предохранитель массового удаления не сработал бы:
    // заметка одна.
    const qint64 kindAt = read.at(1).offset() + 2;
    QFile file(store.journalOf(id));
    (void)file.open(QIODevice::ReadOnly);
    const QByteArray blob = file.readAll();
    file.close();
    ZT_EQ("на этом месте действительно род «сохранение»", num(1),
          num(int(blob[qsizetype(kindAt)])));

    store.flipByte(store.journalOf(id), kindAt, 0x05);   // 1 → 4, надгробие

    ZT_TRUE("журнал по-прежнему читается", history.read(id, &read, &error));
    ZT_EQ("записей всё ещё две: порча не отрезала остаток", num(2), num(read.size()));
    ZT_EQ("испорченная названа", num(1), num(read.damagedCount()));
    ZT_TRUE("и это она", read.isDamaged(1));
    ZT_EQ("голова — уцелевшая первая", num(0), num(read.headIndex()));
    ZT_TRUE("а не надгробие", read.at(read.headIndex()).kind() == Kind::Save);

    QByteArray got;
    ZT_TRUE("испорченная запись слепка не отдаёт", !history.snapshotAt(id, 1, &got, &error));
    ZT_TRUE("а целая отдаёт", history.snapshotAt(id, 0, &got, &error));
    ZT_EQ("и он тот самый", str(QString::fromUtf8(note("раз"))), str(QString::fromUtf8(got)));
}

void checkUnknownVoidAddressDoesNothing() {
    // Испорченный адрес гашения не совпадает ни с чем — и просто не
    // применяется. Порог по ревизии на его месте выкосил бы диапазон.
    ZJournal j(QVector<Entry>{
        entry(Kind::Save, kNow, 1, "раз"),
        entry(Kind::Save, kNow + 1000, 2, "два"),
        Entry(Kind::Save, kNow + 2000, 3, digestOf("три"), 0,
              QVector<journal::EntryRef>{journal::EntryRef(kNow + 1000, digestOf("чужой"))})});
    ZT_TRUE("первая жива", !j.isVoided(0));
    ZT_TRUE("вторая жива: адрес не совпал", !j.isVoided(1));
    ZT_EQ("вешки все", std::string("0 1 2"), waypoints(j));
}

}  // namespace

static int ztRunSuite(int argc, char** argv) {
    (void)argc;
    (void)argv;
    checkComparatorIsTotalOrder();
    checkLegacyOrderIsByTime();
    checkPoisonedHeadLosesToFreshEdit();
    checkEditBeatsDeleteOnEqualKey();
    checkClockBackDoesNotInvert();
    checkLostDeviceClockStillWorks();
    checkJournalFloorHoldsNamedMoments();
    checkNamedMomentSurvivesGuard();
    checkLegacyJournalOnDisk();
    checkVoidingTail();
    checkVoidingMiddle();
    checkVoidedRecordStaysVoidedWhenItComesBack();
    checkAmendmentHidesWithoutStatingContent();
    checkUnnamedConcurrentEditSurvives();
    checkBrokenFrameIsNotADeletion();
    checkUnknownVoidAddressDoesNothing();
    return zt::report("journal_order");
}

TEST(JournalOrder, All) {
    std::vector<QByteArray> ztArgs{QByteArrayLiteral("journal_order_test")};
    std::vector<char*> ztArgv;
    for (QByteArray& a : ztArgs) ztArgv.push_back(a.data());
    EXPECT_EQ(0, ztRunSuite(int(ztArgv.size()), ztArgv.data()));
}
