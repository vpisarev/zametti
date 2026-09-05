// THE SOFT LOCK (`lock: yes`, brief 18) in the editor.
//
// What the lock is: the editor refuses typed input, paste and drop, while the
// program still writes the note (annotations later; here — the lock itself),
// and the store moves and renames it like any other. What it is not: `access:
// read-only`, which the store refuses to rewrite at all — that one stays as
// it was and is checked here side by side, so the two cannot drift into one.
//
// The temporary unlock is memory, keyed by the note's id: it survives nothing
// — switching to another note and back finds the lock closed again.

#include "editor_widget.h"
#include "test_util.h"
#include "zstorage.h"

#include <QApplication>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>
#include <QTextCursor>

#include <memory>
#include <string>

namespace {

using zametti::NoteEditor;
using zametti::ZStorage;

zametti::ZJournal::Rules rules() { return zametti::ZJournal::Rules{}; }

QString readFile(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QString::fromUtf8(f.readAll());
}

bool writeFile(const QString& path, const QString& text) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    f.write(text.toUtf8());
    return true;
}

}  // namespace

TEST(Lock, Editor) {
    QTemporaryDir home;
    const QString root = home.path() + QStringLiteral("/store");
    QString error;
    ZT_TRUE("хранилище заведено", ZStorage(root).init(&error));
    auto storage = std::make_shared<ZStorage>(root);
    storage->reload();

    const QString bookId = storage->createNote(QString(), false, &error);
    const QString plainId = storage->createNote(QString(), false, &error);
    const QString frozenId = storage->createNote(QString(), false, &error);
    ZT_TRUE("заметки созданы", !bookId.isEmpty() && !plainId.isEmpty() && !frozenId.isEmpty());
    const QString bookPath = storage->pathOf(bookId);
    const QString plainPath = storage->pathOf(plainId);
    const QString frozenPath = storage->pathOf(frozenId);
    ZT_TRUE("книга записана", writeFile(bookPath, QStringLiteral("<!-- zametti\nrole: book\nlock: yes\n"
                                                                    "-->\n\n# Книга\n\nТекст книги.\n")));
    ZT_TRUE("обычная записана", writeFile(plainPath, QStringLiteral("<!-- zametti\n-->\n\n# Обычная\n\nТекст.\n")));
    ZT_TRUE("замороженная записана",
            writeFile(frozenPath, QStringLiteral("<!-- zametti\naccess: read-only\n-->\n\n# Лёд\n\nТекст.\n")));
    storage->reload();

    NoteEditor editor;
    editor.setStorage(storage);
    editor.setAttribute(Qt::WA_DontShowOnScreen);
    editor.show();

    // --- the lock refuses typing, lets the program through -----------------
    editor.openFile(bookPath);
    ZT_TRUE("книга заперта", editor.isLockedNote() && editor.isEffectivelyLocked());
    ZT_TRUE("но не read-only", !editor.isReadOnlyNote());
    ZT_TRUE("и это книга", editor.isBookNote());
    ZT_TRUE("виджет в режиме только-чтение", editor.isReadOnly());
    const QString before = editor.toPlainText();
    QTest::keyClicks(&editor, QStringLiteral("abc"));
    QTest::keyClick(&editor, Qt::Key_Return);
    ZT_EQ("набор не прошёл", before.toStdString(), editor.toPlainText().toStdString());
    {
        // A programmatic edit — the road annotations take — still works.
        QTextCursor cursor = editor.textCursor();
        cursor.movePosition(QTextCursor::End);
        cursor.insertText(QStringLiteral(" (пометка)"));
        ZT_TRUE("правка курсором прошла", editor.toPlainText().contains(QStringLiteral("(пометка)")));
        editor.undo();
    }

    // --- the temporary unlock: until the next note ---------------------------
    int lockSignals = 0;
    QObject::connect(&editor, &NoteEditor::lockChanged, &editor, [&lockSignals] { ++lockSignals; });
    editor.setTemporaryUnlock(true);
    ZT_TRUE("временно открыта", editor.isTemporarilyUnlocked() && !editor.isEffectivelyLocked());
    ZT_TRUE("виджет пускает ввод", !editor.isReadOnly());
    ZT_TRUE("замок в файле цел", readFile(bookPath).contains(QStringLiteral("lock: yes")));
    ZT_EQ("сигнал о замке — один", "1", std::to_string(lockSignals));
    QTest::keyClicks(&editor, QStringLiteral("x"));
    ZT_TRUE("набор прошёл", editor.toPlainText() != before);
    editor.undo();
    editor.save(false, true);

    editor.openFile(plainPath);
    ZT_TRUE("обычная не заперта", !editor.isLockedNote() && !editor.isEffectivelyLocked());
    editor.openFile(bookPath);
    ZT_TRUE("вернулись — заперта снова", editor.isEffectivelyLocked() && !editor.isTemporarilyUnlocked());

    // --- the lock is taken off and put on: a header edit, modified untouched --
    // The `modified` line as it is right now (there may be none): the two
    // toggles below are not content edits and must leave it exactly so.
    const auto modifiedLineOf = [](const QString& text) {
        for (const QString& line : text.split(QLatin1Char('\n')))
            if (line.startsWith(QStringLiteral("modified:"))) return line;
        return QString();
    };
    const QString modifiedBefore = modifiedLineOf(readFile(bookPath));
    editor.setNoteLocked(false);
    ZT_TRUE("замок снят в редакторе", !editor.isLockedNote() && !editor.isEffectivelyLocked());
    ZT_TRUE("и в файле", !readFile(bookPath).contains(QStringLiteral("lock:")));
    ZT_TRUE("роль книги осталась", readFile(bookPath).contains(QStringLiteral("role: book")));
    editor.setNoteLocked(true);
    ZT_TRUE("замок поставлен в файле", readFile(bookPath).contains(QStringLiteral("lock: yes\n")));
    ZT_TRUE("виджет снова только-чтение", editor.isReadOnly());
    ZT_EQ("modified не тронут", modifiedBefore.toStdString(),
          modifiedLineOf(readFile(bookPath)).toStdString());

    // --- the lock does not stop the store: move and rename go through -------
    storage->reload();
    const QString folder = storage->createNote(QString(), true, &error);
    ZT_TRUE("папка создана", !folder.isEmpty());
    ZT_TRUE("запертая книга переносится: " + error.toStdString(),
            storage->move(bookId, folder, rules(), &error));
    ZT_TRUE("а read-only заметка — нет", !storage->move(frozenId, folder, rules(), &error));

    // --- `access: read-only` is the other thing -----------------------------
    editor.openFile(frozenPath);
    ZT_TRUE("read-only заперта по-своему", editor.isReadOnlyNote() && editor.isEffectivelyLocked());
    ZT_TRUE("но не замком", !editor.isLockedNote());
    editor.setTemporaryUnlock(true);
    ZT_TRUE("временный замок read-only не открывает", editor.isEffectivelyLocked());

    EXPECT_EQ(0, zt::report("замок"));
}
