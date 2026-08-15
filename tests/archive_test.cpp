// Архив: тело в журнале, файл — стаб.
//
// Главный инвариант этапа (A из брифа): архивация НЕ ТЕРЯЕТ НИ БАЙТА тела.
// Держится он порядком шагов — сперва журнал, потом срез, — и проверяется тремя
// способами: тело есть в журнале, повтор ничего не портит, а обрыв между
// шагами оставляет полную заметку.
//
// Второе (B): возврат отдаёт ровно то, что было, и туда же, откуда убрали, —
// `parent` архив не трогает вовсе, поэтому и помнить ему нечего.

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
using zametti::store::stubBytes;

namespace {

QString g_root;

std::string s(const QString& q) { return q.toStdString(); }

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
    zametti::journal::Journal read;
    QString error;
    if (!zametti::journal::History(g_root).read(id, &read, &error)) return -1;
    return int(read.entries.size());
}

QByteArray head(const QString& id) {
    zametti::journal::History history(g_root);
    zametti::journal::Journal read;
    QString error;
    if (!history.read(id, &read, &error) || read.entries.isEmpty()) return {};
    QByteArray body;
    if (!history.snapshotAt(id, int(read.entries.size()) - 1, &body, &error)) return {};
    return body;
}

const char* kBody =
    "<!-- zametti\n"
    "parent: 0000000000000p\n"
    "created: 2020-01-01T00:00:00+03:00\n"
    "modified: 2020-05-05T12:00:00+03:00\n"
    "-->\n"
    "\n# Фототехника\n\nДлинный текст заметки, который и составляет всё её тело.\n"
    "\n![снимок](01jd7f0kq2m8xab7.webp#w=600)\n";

// --- стаб ------------------------------------------------------------------

void checkStub() {
    const std::string stub = noteOf(kBody).archiveStub();

    ZT_TRUE("в стабе есть заголовок", stub.find("# Фототехника") != std::string::npos);
    ZT_TRUE("тела в стабе нет", stub.find("Длинный текст") == std::string::npos);
    ZT_TRUE("ссылки на картинку в стабе нет", stub.find("01jd7f0kq2m8xab7") == std::string::npos);
    ZT_TRUE("стаб помечен архивным", stub.find("archived: yes") != std::string::npos);
    ZT_TRUE("parent НЕ тронут: архив не переносит",
            stub.find("parent: 0000000000000p") != std::string::npos);
    ZT_TRUE("created на месте", stub.find("created: 2020-01-01T00:00:00+03:00") != std::string::npos);
    ZT_TRUE("modified на месте",
            stub.find("modified: 2020-05-05T12:00:00+03:00") != std::string::npos);

    // СТАБ — ЗАКОННЫЙ MARKDOWN, и это не формальность: его разбирают тем же
    // ядром, показывают в списке и ищут по заголовку.
    const zametti::ZDocument back = noteOf(stub);
    ZT_EQ("круг разбор→запись у стаба побайтовый", stub, back.toMarkdown());
    ZT_TRUE("стаб читается как архивный", back.isArchived());
    // РАЗМЕР СТАБА НЕ ЗАВИСИТ ОТ ТЕЛА — в этом и смысл. На фикстуре в три
    // строки выигрыш почти не виден (151 байт против 287: шапка и есть почти
    // весь файл), поэтому спрашиваем на большой заметке.
    std::string big = kBody;
    big += std::string(50000, 'x');
    big += "\n";
    const std::string bigStub = noteOf(big).archiveStub();
    ZT_TRUE("стаб большой заметки того же размера, что и маленькой: " +
                std::to_string(bigStub.size()) + " байт против тела в " +
                std::to_string(big.size()),
            bigStub.size() < 400 && big.size() > 50000);
}

// --- архивация -------------------------------------------------------------

void checkArchiveKeepsBody() {
    freshStore();
    const QString id = QStringLiteral("01aa00000000aa");
    write(id, kBody);
    const std::string was = read(id);

    QString error;
    ZT_TRUE("архивация прошла: " + s(error), archiveNote(g_root, id, rules(), &error));

    // ИНВАРИАНТ A: тело целиком лежит в журнале, и это те самые байты.
    ZT_EQ("в голове журнала — тело ДО среза", was,
          std::string(head(id).constData(), size_t(head(id).size())));
    const std::string now = read(id);
    ZT_TRUE("файл стал стабом", now.find("Длинный текст") == std::string::npos);
    ZT_TRUE("и помечен архивным", now.find("archived: yes") != std::string::npos);

    // ИДЕМПОТЕНТНОСТЬ: повтор ничего не портит — ни файла, ни журнала. Так
    // выглядит второй заход после падения между шагами.
    const int before = records(id);
    ZT_TRUE("повторная архивация не жалуется", archiveNote(g_root, id, rules(), &error));
    ZT_EQ("файл не изменился", now, read(id));
    ZT_TRUE("и записей не прибавилось", records(id) == before);
}

// Тело в журнал ложится ПО ОБЩИМ ПРАВИЛАМ ОТБОРА: только что сохранённая
// заметка (голова журнала уже равна файлу) новой записи не заводит.
void checkArchiveObeysHistoryRules() {
    freshStore();
    const QString id = QStringLiteral("01bb00000000bb");
    write(id, kBody);

    QString error;
    const QByteArray body(kBody);
    ZT_TRUE("голова записана руками",
            zametti::journal::History(g_root).append(id, zametti::journal::Kind::Save,
                                                     QDateTime::currentMSecsSinceEpoch(), body,
                                                     0, &error));
    const int before = records(id);
    ZT_TRUE("архивация прошла", archiveNote(g_root, id, rules(), &error));
    ZT_TRUE("записи не прибавилось: голова и так равна телу (" +
                std::to_string(records(id)) + " против " + std::to_string(before) + ")",
            records(id) == before);
    ZT_TRUE("а файл всё равно стал стабом",
            read(id).find("Длинный текст") == std::string::npos);
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
    ZT_TRUE("тело вернулось", now.find("Длинный текст") != std::string::npos);
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

// Стаб без журнала — единственный случай, когда возврат отказывает. Молча
// оставить человека с одной строкой вместо заметки нельзя.
void checkRestoreWithoutJournal() {
    freshStore();
    const QString id = QStringLiteral("01dd00000000dd");
    write(id, "<!-- zametti\narchived: yes\n-->\n\n# Стаб без истории\n");
    QString error;
    ZT_TRUE("возврат отказал", !restoreNote(g_root, id, &error));
    ZT_TRUE("и объяснил почему: " + s(error), !error.isEmpty());
    ZT_TRUE("стаб на месте", read(id).find("# Стаб без истории") != std::string::npos);
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
    const zametti::ZDocument old = noteOf("<!-- zametti\nrole: trash\n-->\n\n# Корзина\n");
    ZT_TRUE("role: trash читается как архивность", old.isArchived());
    const zametti::ZDocument plain = noteOf("<!-- zametti\nparent: x\n-->\n\n# Живая\n");
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
        const zametti::ZDocument doc = noteOf(read(info.completeBaseName()));
        if (doc.headerValue(QStringLiteral("role")).toStdString() == zametti::store::kLostRole) bureau = info.completeBaseName();
    }
    ZT_TRUE("бюро заведено", !bureau.isEmpty());
    ZT_TRUE("и это папка с заголовком",
            read(bureau).find("# Бюро находок") != std::string::npos);

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
    zametti::ZDocument doc = noteOf(moved);
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

    checkStub();
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
