// Корневая заметка хранилища: role: root, её стражи и рождение.
//
// Корень — НАСТОЯЩАЯ ЗАМЕТКА: её заголовок это имя хранилища, она папка (чтобы
// правила дерева применялись без оговорок), и снести её нельзя ничем.

#include "store.h"
#include "store_identity.h"
#include "znote.h"
#include "zstorage.h"

#include "test_util.h"
#include "testdata.h"

#include <QDir>
#include <QTemporaryDir>

#include <string>

namespace {

using zametti::ZNote;
using zametti::ZStorage;

std::string s(const QString& v) { return v.toStdString(); }

// Хранилище во временном каталоге; имя каталога — будущее имя хранилища.
struct Fixture {
    QTemporaryDir dir;
    QString root = dir.filePath(QStringLiteral("Мои заметки"));
    ZStorage storage{(zametti::store::initStore(root, nullptr), root)};
};

// --- шапка: role: root ------------------------------------------------------

void checkRoleInHeader() {
    ZNote note;
    ZT_TRUE("заметка с ролью root читается",
            note.load("<!-- zametti\nversion: 1\nrole: root\n-->\n\n# Хранилище\n"));
    ZT_TRUE("это корень", note.isRoot());
    // ПАПКА: тогда правило «тело папки — ровно один заголовок» и правила дерева
    // применяются к корню без единой оговорки.
    ZT_TRUE("и он папка", note.isFolder());
    ZT_TRUE("но не бюро находок", !note.isLost());
    ZT_EQ("роль называется словом", std::string("root"), s(note.role()));
    ZT_TRUE("метаданные тоже знают", note.metadata().root() && note.metadata().folder());

    ZNote plain;
    ZT_TRUE("обычная заметка читается", plain.load("# Просто\n"));
    ZT_TRUE("и корнем не считается", !plain.isRoot());
    ZT_TRUE("и папкой тоже", !plain.isFolder());

    ZNote folder;
    ZT_TRUE("папка читается",
            folder.load("<!-- zametti\nversion: 1\nrole: folder\n-->\n\n# Дом\n"));
    ZT_TRUE("папка не корень", !folder.isRoot());
    ZT_TRUE("но папка", folder.isFolder());
}

// --- рождение ---------------------------------------------------------------

void checkBirth() {
    Fixture f;
    ZT_TRUE("корня сперва нет", f.storage.rootId().isEmpty());

    QString error;
    const QString id = f.storage.ensureRootNote(&error);
    ZT_TRUE("корень заведён: " + s(error), !id.isEmpty());
    ZT_TRUE("он есть в каталоге", f.storage.has(id));
    ZT_TRUE("он корень", f.storage.isRootNote(id));
    ZT_TRUE("он папка", f.storage.isFolder(id));
    ZT_EQ("имя хранилища — имя каталога", std::string("Мои заметки"), s(f.storage.titleOf(id)));
    ZT_EQ("и адрес назван в zametti.json", s(id), s(f.storage.identity().rootNote()));

    // ВТОРОЙ РАЗ НИЧЕГО НЕ РОЖДАЕТ.
    ZT_EQ("тот же корень", s(id), s(f.storage.ensureRootNote(&error)));
    int roots = 0;
    for (const QString& note : f.storage.ids())
        if (f.storage.isRootNote(note)) ++roots;
    ZT_EQ("корень ровно один", std::string("1"), std::to_string(roots));
}

// Роль есть, а адреса в zametti.json нет: находим по роли и называем.
void checkFoundByRole() {
    Fixture f;
    QString error;
    const QString id = f.storage.ensureRootNote(&error);
    ZT_TRUE("корень заведён", !id.isEmpty());
    // Так выглядит хранилище, приехавшее без zametti.json или с чужим адресом.
    ZT_TRUE("адрес сбит", f.storage.setRootNote(QString(), &error));

    ZStorage again(f.root);
    ZT_EQ("корень найден по роли", s(id), s(again.rootId()));
    ZT_EQ("и адрес починен", s(id), s(again.ensureRootNote(&error)));
    ZT_EQ("в файле тоже", s(id), s(again.identity().rootNote()));
}

// --- стражи -----------------------------------------------------------------

void checkGuards() {
    Fixture f;
    QString error;
    const QString id = f.storage.ensureRootNote(&error);
    ZT_TRUE("корень заведён", !id.isEmpty());
    const QString other = f.storage.createNote(QString(), true, &error);
    ZT_TRUE("папка рядом заведена", !other.isEmpty());

    QStringList failed;
    ZT_TRUE("архивировать корень нельзя", !f.storage.archive(id, {}, &failed));
    ZT_TRUE("и сказано почему", !failed.isEmpty());
    ZT_TRUE("корень цел", f.storage.has(id) && !f.storage.inArchive(id));

    error.clear();
    ZT_TRUE("удалить корень нельзя", !f.storage.remove(id, zametti::ImportLimits{}, &error));
    ZT_TRUE("и сказано почему", !error.isEmpty());
    ZT_TRUE("корень цел", f.storage.has(id));

    error.clear();
    ZT_TRUE("перенести корень нельзя", !f.storage.move(id, other, {}, &error));
    ZT_TRUE("и сказано почему", !error.isEmpty());

    error.clear();
    ZT_TRUE("разжаловать корень нельзя",
            !f.storage.rewriteNote(id, [](ZNote& note) { note.setRole(QString()); }, {}, &error));
    ZT_TRUE("и сказано почему", !error.isEmpty());
    ZT_TRUE("роль на месте", f.storage.isRootNote(id));

    // ПЕРЕИМЕНОВАТЬ — МОЖНО: это и есть смена имени хранилища.
    error.clear();
    ZT_TRUE("переименование проходит",
            f.storage.rename(id, QStringLiteral("Архив мыслей"), {}, &error));
    ZT_EQ("имя сменилось", std::string("Архив мыслей"), s(f.storage.titleOf(id)));
    ZT_TRUE("а роль осталась", f.storage.isRootNote(id));

    // Обычную заметку всё это по-прежнему берёт.
    error.clear();
    ZT_TRUE("папку рядом архивировать можно", f.storage.archive(other, {}, &failed));
}

}  // namespace

TEST(RootNote, All) {
    checkRoleInHeader();
    checkBirth();
    checkFoundByRole();
    checkGuards();
    EXPECT_EQ(0, zt::freshFailures());
}
