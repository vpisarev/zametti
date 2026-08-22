// Архив — ПОМЕТКА, и ничего кроме.
//
// Так было не всегда: до этапа 17 архивация срезала тело в журнал, оставляя от
// заметки стаб из шапки и заголовка. Стоило это дорого — вместе с телом из
// файла исчезали ссылки на картинки, и «удалить насовсем» не уносило ни одной
// (искать их было негде), а картинку, которую держала только архивная заметка,
// уносило удаление СОСЕДНЕЙ. Плюс поиск по хранилищу переставал видеть
// архивные тела, а вышедший из режима истории человек получал стаб, полностью
// доступный для правки.
//
// Теперь тело остаётся в файле, а в шапке появляется одна строка. Отсюда и
// проверки: файл после архивации отличается от исходного ровно этой строкой,
// голова журнала равна файлу, а возврат не зависит от журнала вовсе.

#include "archive.h"
#include "pieces.h"
#include "lost_found.h"
#include "journal.h"
#include "times.h"

#include "test_util.h"

#include <vector>
#include "testdata.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QFile>

#include <string>

using zametti::store::archiveNote;
using zametti::store::forgetNote;
using zametti::store::isArchivedMeta;
using zametti::store::migrateTrashToArchive;
using zametti::store::restoreNote;

namespace {

QString g_root;

std::string s(const QString& q) { return q.toStdString(); }

template <typename T>
std::string num(T value) { return std::to_string(value); }

zametti::history::Rules rules() {
    zametti::history::Rules r;
    r.mergeChars = 100;
    r.mergeHours = 24;
    return r;
}

void freshStore() {
    QDir(g_root).removeRecursively();
    QDir().mkpath(g_root + QStringLiteral("/.zametti"));
    QDir().mkpath(g_root + QStringLiteral("/history"));
}

QString notePath(const QString& id) {
    return g_root + QLatin1Char('/') + id + QStringLiteral(".md");
}

void write(const QString& id, const std::string& text) {
    QFile f(notePath(id));
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        f.write(text.data(), qint64(text.size()));
}

std::string read(const QString& id) {
    QFile f(notePath(id));
    if (!f.open(QIODevice::ReadOnly)) return {};
    const QByteArray bytes = f.readAll();
    return std::string(bytes.constData(), size_t(bytes.size()));
}

int records(const QString& id) {
    zametti::journal::ZJournal read;
    QString error;
    if (!zametti::journal::History(g_root).read(id, &read, &error)) return -1;
    return int(read.size());
}

// Голова журнала — по общей формуле порядка, а не по месту в файле.
QByteArray head(const QString& id) {
    zametti::journal::History history(g_root);
    zametti::journal::ZJournal read;
    QString error;
    if (!history.read(id, &read, &error) || read.isEmpty()) return {};
    const int at = read.lastSnapshotIndex();
    QByteArray body;
    if (at < 0 || !history.snapshotAt(id, at, &body, &error)) return {};
    return body;
}

// Сколько вешек видит человек: записи о содержимом, не погашенные.
int waypoints(const QString& id) {
    zametti::journal::ZJournal read;
    QString error;
    if (!zametti::journal::History(g_root).read(id, &read, &error)) return -1;
    int count = 0;
    for (int i = 0; i < read.size(); ++i)
        if (read.at(i).statesContent() && !read.isVoided(i)) ++count;
    return count;
}

const char* kBody =
    "<!-- zametti\n"
    "parent: 0000000000000p\n"
    "created: 2020-01-01T00:00:00+03:00\n"
    "modified: 2020-05-05T12:00:00+03:00\n"
    "-->\n"
    "\n# Фототехника\n\nДлинный текст заметки, который и составляет всё её тело.\n"
    "\n![снимок](01jd7f0kq2m8xab7.webp#w=600)\n";

// --- архивация -------------------------------------------------------------

void checkArchiveKeepsBody() {
    freshStore();
    const QString id = QStringLiteral("01aa00000000aa");
    write(id, kBody);
    const std::string was = read(id);

    QString error;
    ZT_TRUE("архивация прошла: " + s(error), archiveNote(g_root, id, rules(), &error));

    const std::string now = read(id);
    // ТЕЛО ОСТАЛОСЬ В ФАЙЛЕ — вот главное, ради чего всё и переделано.
    ZT_TRUE("тело на месте", now.find("Длинный текст") != std::string::npos);
    ZT_TRUE("ссылка на картинку на месте: по ней и считаются вложения при "
            "удалении насовсем",
            now.find("01jd7f0kq2m8xab7") != std::string::npos);
    ZT_TRUE("и файл помечен архивным", now.find("archived: yes") != std::string::npos);
    ZT_EQ("а отличие от исходного — ровно одна строка шапки",
          was.substr(0, was.find("-->")) + "archived: yes\n" + was.substr(was.find("-->")), now);

    // ГОЛОВА ЖУРНАЛА РАВНА ФАЙЛУ: файл изменился на строку, значит запись
    // положена. На этом инварианте стоит вся синхронизация.
    ZT_EQ("голова журнала равна файлу", now,
          std::string(head(id).constData(), size_t(head(id).size())));

    // ИДЕМПОТЕНТНОСТЬ: повтор ничего не портит — ни файла, ни журнала.
    const int before = records(id);
    ZT_TRUE("повторная архивация не жалуется", archiveNote(g_root, id, rules(), &error));
    ZT_EQ("файл не изменился", now, read(id));
    ZT_TRUE("и записей не прибавилось", records(id) == before);
}

// Пометка ложится в журнал ПО ОБЩИМ ПРАВИЛАМ ОТБОРА: она мелкая (одна строка),
// значит гасит прошлую запись, а не встаёт рядом с ней. Вешек не прибавляется,
// но голова обязана сойтись с файлом.
void checkArchiveObeysHistoryRules() {
    freshStore();
    const QString id = QStringLiteral("01bb00000000bb");
    write(id, kBody);

    QString error;
    const QByteArray body(kBody);
    zametti::journal::History history(g_root);
    // ДВЕ записи, а не одна: опорную правило не гасит никогда — после замены в
    // журнале обязана остаться хотя бы одна запись, а первая это то, с чего
    // заметка начиналась.
    ZT_TRUE("опорная записана руками",
            history.append(id, zametti::journal::NewRecord::save(body, zametti::journal::Stamp::now()),
                           &error));
    QByteArray grown(kBody);
    grown += "\nещё абзац, чтобы вторая запись не слилась с опорной: " +
             QByteArray(200, 'y') + "\n";
    write(id, std::string(grown.constData(), size_t(grown.size())));
    ZT_TRUE("и вторая",
            history.append(id, zametti::journal::NewRecord::save(grown, zametti::journal::Stamp::now()),
                           &error));
    const int before = waypoints(id);
    ZT_TRUE("архивация прошла", archiveNote(g_root, id, rules(), &error));
    ZT_TRUE("вешек не прибавилось: правка мелкая (" + std::to_string(waypoints(id)) +
                " против " + std::to_string(before) + ")",
            waypoints(id) == before);
    ZT_EQ("а голова журнала — уже помеченный файл", read(id),
          std::string(head(id).constData(), size_t(head(id).size())));
}

// --- возврат ---------------------------------------------------------------

void checkRestore() {
    freshStore();
    const QString id = QStringLiteral("01cc00000000cc");
    write(id, kBody);
    const std::string was = read(id);

    QString error;
    ZT_TRUE("архивация прошла", archiveNote(g_root, id, rules(), &error));
    const int afterArchive = records(id);
    ZT_TRUE("возврат прошёл: " + s(error), restoreNote(g_root, id, &error));

    const std::string now = read(id);
    ZT_TRUE("тело на месте: оно никуда и не уходило",
            now.find("Длинный текст") != std::string::npos);
    ZT_TRUE("пометки архивности нет", now.find("archived") == std::string::npos);
    ZT_TRUE("parent тот же — заметка дома",
            now.find("parent: 0000000000000p") != std::string::npos);
    ZT_TRUE("картинка на месте", now.find("01jd7f0kq2m8xab7") != std::string::npos);
    ZT_EQ("файл вернулся байт в байт", was, now);
    ZT_TRUE("возврат оставил вешку в истории (" + std::to_string(records(id)) + ")",
            records(id) == afterArchive + 1);

    // Повторный возврат — не ошибка: заметка уже дома.
    ZT_TRUE("повтор возврата молчит", restoreNote(g_root, id, &error));
    ZT_EQ("и файл не изменился", now, read(id));
}

// ВОЗВРАТ БОЛЬШЕ НЕ ЗАВИСИТ ОТ ЖУРНАЛА. Раньше тело брали из головы, и
// архивная заметка без истории возвращалась стабом — то есть не возвращалась
// вовсе. Теперь тело всё это время лежало в файле, и снять пометку можно даже
// у заметки, приехавшей с чужой машины без журнала.
void checkRestoreWithoutJournal() {
    freshStore();
    const QString id = QStringLiteral("01dd00000000dd");
    write(id,
          "<!-- zametti\narchived: yes\n-->\n\n# Без истории\n\nА тело у неё есть.\n");
    QString error;
    ZT_TRUE("возврат прошёл: " + s(error), restoreNote(g_root, id, &error));
    const std::string now = read(id);
    ZT_TRUE("пометки нет", now.find("archived") == std::string::npos);
    ZT_TRUE("тело на месте", now.find("А тело у неё есть") != std::string::npos);
}

// --- удалить насовсем ------------------------------------------------------

void checkForget() {
    freshStore();
    const QString id = QStringLiteral("01ee00000000ee");
    write(id, kBody);
    QString error;
    ZT_TRUE("архивация прошла", archiveNote(g_root, id, rules(), &error));
    const QString log = zametti::journal::History(g_root).pathFor(id);
    ZT_TRUE("журнал есть", QFile::exists(log));

    ZT_TRUE("забыли насовсем", forgetNote(g_root, id, &error));
    ZT_TRUE("файла нет", !QFile::exists(notePath(id)));
    // ЖУРНАЛ УЕХАЛ ВМЕСТЕ С ЗАМЕТКОЙ — и только здесь. У архивной тело живёт в
    // журнале и больше нигде: оставить его значило бы не удалить заметку, а
    // спрятать.
    ZT_TRUE("и журнала тоже нет", !QFile::exists(log));
}

// --- разворачивание стабов прошлых сборок -----------------------------------

// В хранилище, которое пожило на прежней сборке, архивные заметки лежат
// стабами: шапка плюс строка заголовка, а тело — в журнале. Разворачивание
// возвращает тело в файл, оставляя пометку. Без него у таких заметок так и не
// будет ни ссылок на вложения, ни текста для поиска.
void checkUnfoldStubs() {
    freshStore();
    const QString id = QStringLiteral("01ff00000000ff");
    const QString noHistory = QStringLiteral("01ff00000000fe");
    const QString alive = QStringLiteral("01ff00000000fd");

    // Заметка, убранная в архив ПРЕЖНЕЙ сборкой: в журнале тело, в файле стаб.
    QString error;
    const QByteArray body(kBody);
    ZT_TRUE("тело записано в журнал",
            zametti::journal::History(g_root).append(
                id, zametti::journal::NewRecord::save(body, zametti::journal::Stamp::now()),
                &error));
    write(id,
          "<!-- zametti\nparent: 0000000000000p\ncreated: 2020-01-01T00:00:00+03:00\n"
          "modified: 2020-05-05T12:00:00+03:00\narchived: yes\nsort: created\n-->\n"
          "\n# Фототехника\n");
    // Стаб без журнала: приехал с чужой машины, разворачивать не из чего.
    write(noHistory, "<!-- zametti\narchived: yes\n-->\n\n# Без истории\n");
    // Живая заметка — её трогать нельзя ничем.
    write(alive, kBody);
    const std::string aliveWas = read(alive);

    // АРХИВНАЯ С ТЕЛОМ — та, что убрана уже новым путём. В журнале у неё лежит
    // СТАРЫЙ текст, и разворачивание обязано пройти мимо: иначе оно откатило бы
    // заметку к прошлой версии. Ею и приколот детектор стаба.
    const QString full = QStringLiteral("01ff00000000fc");
    QByteArray older(kBody);
    older.replace("Длинный текст", "Старый текст");
    ZT_TRUE("старая версия записана в журнал",
            zametti::journal::History(g_root).append(
                full, zametti::journal::NewRecord::save(older, zametti::journal::Stamp::now()),
                &error));
    std::string fullBytes = kBody;
    fullBytes.insert(fullBytes.find("-->"), "archived: yes\n");
    write(full, fullBytes);

    QStringList leftAlone;
    const int done = zametti::store::unfoldArchivedStubs(g_root, &leftAlone, &error);
    ZT_EQ("развёрнута одна", num(1), num(done));

    const std::string now = read(id);
    ZT_TRUE("тело вернулось в файл", now.find("Длинный текст") != std::string::npos);
    ZT_TRUE("и ссылка на картинку тоже", now.find("01jd7f0kq2m8xab7") != std::string::npos);
    ZT_TRUE("пометка архива на месте", now.find("archived: yes") != std::string::npos);
    ZT_TRUE("ключи, добавленные ПОСЛЕ архивации, целы: шапка берётся из файла",
            now.find("sort: created") != std::string::npos);
    ZT_TRUE("modified не тронут",
            now.find("modified: 2020-05-05T12:00:00+03:00") != std::string::npos);

    ZT_EQ("стаб без журнала не тронут",
          std::string("<!-- zametti\narchived: yes\n-->\n\n# Без истории\n"), read(noHistory));
    ZT_TRUE("и назван в отчёте", leftAlone.size() == 1 && leftAlone.first().contains(noHistory));
    ZT_EQ("живая заметка не тронута", aliveWas, read(alive));
    ZT_EQ("архивная С ТЕЛОМ не тронута: она не стаб, и откатывать её к журналу нельзя",
          fullBytes, read(full));

    // ИДЕМПОТЕНТНОСТЬ: второй заход не находит работы и не пишет ни байта.
    const std::string after = read(id);
    QStringList again;
    ZT_EQ("второй заход разворачивает ноль", num(0),
          num(zametti::store::unfoldArchivedStubs(g_root, &again, &error)));
    ZT_EQ("и файл не изменился", after, read(id));
}

// --- старая корзина --------------------------------------------------------

void checkTrashMigration() {
    freshStore();
    // Корзина старого вида: заметка role: trash, внутри — выброшенная, у
    // которой прежний родитель спрятан в trash-parent.
    write(QStringLiteral("0000000000000t"),
          "<!-- zametti\nrole: trash\ncreated: 2020-01-01T00:00:00+03:00\n-->\n\n# Корзина\n");
    write(QStringLiteral("0000000000000p"),
          "<!-- zametti\nrole: folder\ncreated: 2020-01-01T00:00:00+03:00\n-->\n\n# Папка\n");
    write(QStringLiteral("0000000000000v"),
          "<!-- zametti\nparent: 0000000000000t\ntrash-parent: 0000000000000p\n"
          "trash-path: Папка\ncreated: 2020-01-01T00:00:00+03:00\n"
          "modified: 2020-02-02T00:00:00+03:00\n-->\n\n# Выброшенная\n\nТело цело.\n");

    QString error;
    ZT_TRUE("переехала одна заметка", migrateTrashToArchive(g_root, &error) == 1);
    const std::string moved = read(QStringLiteral("0000000000000v"));
    ZT_TRUE("родитель вернулся прежний",
            moved.find("parent: 0000000000000p") != std::string::npos);
    ZT_TRUE("пометка архивности стоит", moved.find("archived: yes") != std::string::npos);
    ZT_TRUE("ключи корзины ушли", moved.find("trash-parent") == std::string::npos &&
                                      moved.find("trash-path") == std::string::npos);
    // ТЕЛО НЕ ТРОГАЕМ: миграция про структуру, а не про архив. Стабом заметку
    // сделает только новая архивация.
    ZT_TRUE("тело осталось на месте", moved.find("Тело цело.") != std::string::npos);
    // modified не поднимается: правка организационная.
    ZT_TRUE("modified не тронут",
            moved.find("modified: 2020-02-02T00:00:00+03:00") != std::string::npos);
    ZT_TRUE("заметка-корзина удалена", !QFile::exists(notePath(QStringLiteral("0000000000000t"))));

    // Идемпотентность: второй проход не делает ничего.
    const std::string after = read(QStringLiteral("0000000000000v"));
    ZT_TRUE("второй проход не находит корзины", migrateTrashToArchive(g_root, &error) == 0);
    ZT_EQ("и ничего не переписывает", after, read(QStringLiteral("0000000000000v")));
}

// Старый вид пометки читается как архивный и без миграции: хранилище могло
// приехать с чужой машины или от прежней сборки.
void checkOldRoleIsRead() {
    const zametti::ZNote old = noteOf("<!-- zametti\nrole: trash\n-->\n\n# Корзина\n");
    ZT_TRUE("role: trash читается как архивность", old.isArchived());
    const zametti::ZNote plain = noteOf("<!-- zametti\nparent: x\n-->\n\n# Живая\n");
    ZT_TRUE("обычная заметка архивной не считается", !plain.isArchived());
}

// --- бюро находок ----------------------------------------------------------
//
// ПЕРВОЕ САНКЦИОНИРОВАННОЕ ИСКЛЮЧЕНИЕ из правила «загрузка ничего не пишет», и
// проверяется здесь именно рамка исключения: пишем только сиротам, пишем два
// порта, `modified` не трогаем, повтор не пишет ничего.
void checkLostFound() {
    freshStore();
    // Живая пара «папка → заметка» и сирота: parent есть, а такой заметки нет.
    // Ровно так выглядит файл, вытащенный из системной корзины мимо программы,
    // и копия из чужого хранилища.
    write(QStringLiteral("0000000000000p"),
          "<!-- zametti\nrole: folder\ncreated: 2020-01-01T00:00:00+03:00\n-->\n\n# Папка\n");
    write(QStringLiteral("01aa11111111aa"),
          "<!-- zametti\nparent: 0000000000000p\ncreated: 2020-01-01T00:00:00+03:00\n"
          "modified: 2020-02-02T00:00:00+03:00\n-->\n\n# Своя\n");
    write(QStringLiteral("01bb22222222bb"),
          "<!-- zametti\nparent: 00000000000zzz\ncreated: 2021-01-01T00:00:00+03:00\n"
          "modified: 2021-02-02T00:00:00+03:00\n-->\n\n# Найдёныш\n\nТело цело.\n");
    const std::string homeWas = read(QStringLiteral("01aa11111111aa"));

    QString error;
    ZT_TRUE("прописан ровно один найдёныш: " + s(error),
            zametti::store::fileOrphans(g_root, &error) == 1);

    // Само бюро: заводится только под первую находку, и это папка.
    QString bureau;
    for (const QFileInfo& info : QDir(g_root).entryInfoList({QStringLiteral("*.md")}, QDir::Files)) {
        const zametti::ZNote doc = noteOf(read(info.completeBaseName()));
        if (doc.role().toStdString() == zametti::store::kLostRole) bureau = info.completeBaseName();
    }
    ZT_TRUE("бюро заведено", !bureau.isEmpty());
    ZT_TRUE("и это папка с заголовком",
            read(bureau).find("# Lost & found") != std::string::npos);

    const std::string found = read(QStringLiteral("01bb22222222bb"));
    ZT_TRUE("текущий порт — бюро",
            found.find("parent: " + bureau.toStdString()) != std::string::npos);
    ZT_TRUE("оригинал сохранён", found.find("lost-parent: 00000000000zzz") != std::string::npos);
    ZT_TRUE("тело не тронуто", found.find("Тело цело.") != std::string::npos);
    ZT_TRUE("modified не поднят",
            found.find("modified: 2021-02-02T00:00:00+03:00") != std::string::npos);
    ZT_EQ("живую заметку не тронули вовсе", homeWas, read(QStringLiteral("01aa11111111aa")));

    // ИДЕМПОТЕНТНОСТЬ: второй проход не пишет ничего — ни файлов, ни новой
    // папки. Это и есть инвариант C брифа.
    const std::string afterFirst = read(QStringLiteral("01bb22222222bb"));
    const int filesBefore =
        int(QDir(g_root).entryInfoList({QStringLiteral("*.md")}, QDir::Files).size());
    ZT_TRUE("второй проход не находит сирот", zametti::store::fileOrphans(g_root, &error) == 0);
    ZT_EQ("и ничего не переписывает", afterFirst, read(QStringLiteral("01bb22222222bb")));
    ZT_TRUE("и второго бюро не заводит",
            int(QDir(g_root).entryInfoList({QStringLiteral("*.md")}, QDir::Files).size()) ==
                filesBefore);

    // Вытащили обычным переносом — бюро больше её не трогает.
    std::string moved = read(QStringLiteral("01bb22222222bb"));
    zametti::ZNote doc = noteOf(moved);
    setHead(doc, "parent", "0000000000000p");
    write(QStringLiteral("01bb22222222bb"), doc.toMarkdown());
    ZT_TRUE("после переноса сирот снова нет", zametti::store::fileOrphans(g_root, &error) == 0);
    ZT_TRUE("и заметка осталась там, куда её перенесли",
            read(QStringLiteral("01bb22222222bb")).find("parent: 0000000000000p") !=
                std::string::npos);
}

// Хранилище без сирот не получает ни бюро, ни единой записи: правило
// «загрузка не пишет» отменяется ровно для находок и ни для чего больше.
void checkCleanStoreIsNotTouched() {
    freshStore();
    write(QStringLiteral("0000000000000p"),
          "<!-- zametti\nrole: folder\ncreated: 2020-01-01T00:00:00+03:00\n-->\n\n# Папка\n");
    write(QStringLiteral("01cc33333333cc"),
          "<!-- zametti\nparent: 0000000000000p\ncreated: 2020-01-01T00:00:00+03:00\n-->\n\n# Живая\n");
    const std::string was = read(QStringLiteral("01cc33333333cc"));
    const int files = int(QDir(g_root).entryInfoList({QStringLiteral("*.md")}, QDir::Files).size());

    QString error;
    ZT_TRUE("сирот нет", zametti::store::fileOrphans(g_root, &error) == 0);
    ZT_EQ("файл не тронут", was, read(QStringLiteral("01cc33333333cc")));
    ZT_TRUE("и бюро не заведено",
            int(QDir(g_root).entryInfoList({QStringLiteral("*.md")}, QDir::Files).size()) == files);
}

}  // namespace

static int ztRunSuite(int argc, char** argv) {
    (void)argc;
    (void)argv;
    g_root = QDir::tempPath() + QStringLiteral("/zametti-archive-test");

    checkUnfoldStubs();
    checkArchiveKeepsBody();
    checkArchiveObeysHistoryRules();
    checkRestore();
    checkRestoreWithoutJournal();
    checkForget();
    checkTrashMigration();
    checkOldRoleIsRead();
    checkLostFound();
    checkCleanStoreIsNotTouched();

    QDir(g_root).removeRecursively();
    return zt::report("архив и бюро находок");
}

// Набор целиком одним TEST: тело не тронуто, argv ему собран здесь.
// Дробить на отдельные проверки — отдельная работа, по одному набору.
TEST(Archive, All) {
    std::vector<QByteArray> ztArgs{QByteArrayLiteral("archive_test")};
    std::vector<char*> ztArgv;
    for (QByteArray& a : ztArgs) ztArgv.push_back(a.data());
    EXPECT_EQ(0, ztRunSuite(int(ztArgv.size()), ztArgv.data()));
}
