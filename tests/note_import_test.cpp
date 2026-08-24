// Импорт .md в хранилище: куда попадает заметка и видно ли её.
//
// Владелец: «непонятно куда она импортируется, я не нашёл в какую папку она
// попадает, и в средней колонке она не показывается, а должна показываться
// наверху». Набор разбирает жалобу на три отдельных вопроса, потому что ответы
// у них разные: попала ли она в выбранную папку, есть ли она в списке колонки,
// и на каком месте списка стоит.

#include "note_list.h"
#include "pieces.h"
#include "note_tree.h"
#include "zstorage.h"

#include "test_util.h"

#include <vector>
#include "testdata.h"

#include <QApplication>
#include <QDir>
#include <QFile>

#include <fstream>
#include <sstream>
#include <string>

using zametti::NoteListModel;
using zametti::NoteTreeModel;

namespace {

std::string s(const QString& q) { return q.toStdString(); }

QString g_root;
QString g_outside;

void writeNote(const QString& id, const QString& body) {
    QFile f(g_root + QLatin1Char('/') + id + QStringLiteral(".md"));
    if (!f.open(QIODevice::WriteOnly)) return;
    f.write(body.toUtf8());
}

// Чужой файл: без шапки zametti, как и любой markdown снаружи. Дата правки
// задаётся при ОТКРЫТОМ файле: на закрытом QFile::setFileTime молча не
// срабатывает, и первая редакция этой проверки из-за этого мерила не то.
QString writeOutside(const QString& name, const QString& text, const QDate& modified = {}) {
    const QString path = g_outside + QLatin1Char('/') + name;
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return path;
    f.write(text.toUtf8());
    f.flush();
    if (modified.isValid())
        f.setFileTime(QDateTime(modified, QTime(0, 0), QTimeZone::UTC),
                      QFileDevice::FileModificationTime);
    return path;
}

std::string metaOf(const QString& path, const char* key) {
    std::ifstream in(path.toStdString(), std::ios::binary);
    std::ostringstream all;
    all << in.rdbuf();
    const zametti::ZNote doc = noteOf(all.str());
    return std::string(head(doc, key));
}

// Место заметки в средней колонке; -1 — её там нет вовсе.
int rowInList(const NoteListModel& list, const QString& id) {
    for (int row = 0; row < list.rowCount(); ++row)
        if (list.data(list.index(row, 0), NoteListModel::IdRole).toString() == id) return row;
    return -1;
}

void checkImportGoesToChosenFolder() {
    const QString source = writeOutside(QStringLiteral("привезённая.md"),
                                        QStringLiteral("# Привезённая\n\nТекст.\n"),
                                        QDate(2024, 5, 5));
    QString error;
    zametti::ZStorage storage(g_root);
    const QString made =
        storage.pathOf(storage.importNote(QStringLiteral("00000000000001"), source, &error));
    ZT_TRUE("импорт прошёл: " + s(error), !made.isEmpty());
    if (made.isEmpty()) return;

    ZT_EQ("заметка легла в выбранную папку", std::string("00000000000001"),
          metaOf(made, "parent"));
    ZT_EQ("исходник не тронут", std::string("# Привезённая\n\nТекст.\n"), [&] {
        std::ifstream in(source.toStdString(), std::ios::binary);
        std::ostringstream all;
        all << in.rdbuf();
        return all.str();
    }());
}

// Главное: заметка обязана быть видна в средней колонке, и наверху. Импорт —
// это событие «сейчас», и человек ищет привезённое там, где лежит свежее.
// Своё хранилище: обе проверки импортируют, и заметка, привезённая в первой,
// стояла бы в списке второй — тогда «наверху» означало бы не то, что проверяем.
void checkImportIsVisibleOnTop() {
    const QString root = g_root + QStringLiteral("-top");
    QDir(root).removeRecursively();
    QDir().mkpath(root + QStringLiteral("/.zametti"));
    {
        QFile f(root + QStringLiteral("/00000000000002.md"));
        if (f.open(QIODevice::WriteOnly))
            f.write(QStringLiteral("<!-- zametti\ncreated: 2026-01-01T00:00:00Z\n"
                                   "modified: 2026-08-08T00:00:00Z\n-->\n\n# Свежая заметка\n")
                        .toUtf8());
    }

    NoteTreeModel model(root);
    model.setFoldersOnly(true);
    NoteListModel list;

    // Старый файл: правлен три года назад. Ровно тот случай, на котором ловится
    // «привезли и потеряли» — по дате правки исходника он ушёл бы в самый низ.
    const QString source = writeOutside(QStringLiteral("старая.md"),
                                        QStringLiteral("# Старая привезённая\n"),
                                        QDate(2023, 1, 1));
    QString error;
    zametti::ZStorage storage(root);
    const QString madeId = storage.importNote(QString(), source, &error);
    const QString made = madeId.isEmpty() ? QString() : storage.pathOf(madeId);
    ZT_TRUE("импорт в корень прошёл: " + s(error), !made.isEmpty());
    if (made.isEmpty()) return;

    model.refresh();
    list.setRows(model.notesInSubtree(QModelIndex()));

    const QString id = QFileInfo(made).completeBaseName();
    const int row = rowInList(list, id);
    ZT_TRUE("привезённая заметка есть в средней колонке", row >= 0);
    ZT_EQ("и стоит наверху", std::string("0"), std::to_string(row));

    // Хронология источника не выбрасывается: она уходит в created, иначе
    // привезённый архив терял бы свой порядок навсегда.
    ZT_EQ("дата создания взята у исходника", std::string("2023-01-01"),
          metaOf(made, "created").substr(0, 10));
    // А дата правки — сегодняшняя: привоз и есть правка этого хранилища.
    // МЕСТНАЯ дата, а не UTC: времена пишутся с офсетом (этап 15), и сразу
    // после полуночи по местному времени UTC-дата ещё вчерашняя — проверка
    // краснела ровно в этот час по своей же ошибке.
    ZT_EQ("дата правки — сегодняшняя",
          s(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd"))),
          metaOf(made, "modified").substr(0, 10));
}

}  // namespace

static int ztRunSuite(int argc, char** argv) {
    (void)argc;
    (void)argv;

    g_root = QDir::tempPath() + QStringLiteral("/zametti-note-import-test");
    g_outside = QDir::tempPath() + QStringLiteral("/zametti-note-import-src");
    QDir(g_root).removeRecursively();
    QDir(g_outside).removeRecursively();
    QDir().mkpath(g_root + QStringLiteral("/.zametti"));
    QDir().mkpath(g_outside);

    writeNote(QStringLiteral("00000000000001"),
              QStringLiteral("<!-- zametti\nrole: folder\ncreated: 2026-01-01T00:00:00Z\n"
                             "modified: 2026-01-01T00:00:00Z\n-->\n\n# Поездки\n"));
    writeNote(QStringLiteral("00000000000002"),
              QStringLiteral("<!-- zametti\ncreated: 2026-01-01T00:00:00Z\n"
                             "modified: 2026-08-08T00:00:00Z\n-->\n\n# Свежая заметка\n"));

    checkImportGoesToChosenFolder();
    checkImportIsVisibleOnTop();

    return zt::report("note-import");
}

// Набор целиком одним TEST: тело не тронуто, argv ему собран здесь.
// Дробить на отдельные проверки — отдельная работа, по одному набору.
TEST(NoteImport, All) {
    std::vector<QByteArray> ztArgs{QByteArrayLiteral("note_import_test")};
    std::vector<char*> ztArgv;
    for (QByteArray& a : ztArgs) ztArgv.push_back(a.data());
    EXPECT_EQ(0, ztRunSuite(int(ztArgv.size()), ztArgv.data()));
}
