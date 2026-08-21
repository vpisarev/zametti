// ВЕРСИЯ ФОРМАТА ЗАМЕТКИ — ЛЕНИВО (решение владельца, refactor3).
//
// `version: 1` получает только заметка, которую программа ПИШЕТ. Открыть и
// посмотреть — файл не трогает: старая заметка без версии после принудительной
// записи без правок остаётся байт в байт. Правка — и версия встаёт первой
// строкой шапки. Более новую версию программа не понижает. Судья — файл.

#include "editor_widget.h"
#include "test_util.h"
#include "testdata.h"

#include <QDir>
#include <QFile>
#include <QTest>
#include <QTextCursor>

#include <string>

namespace {

QString g_dir;

QString writeFile(const QString& name, const QString& text) {
    const QString path = QDir(g_dir).filePath(name);
    QFile file(path);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) file.write(text.toUtf8());
    return path;
}

std::string readFile(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return file.readAll().toStdString();
}

void typeAtEnd(zametti::NoteEditor& editor, const QString& text) {
    QTextCursor cursor = editor.textCursor();
    cursor.movePosition(QTextCursor::End);
    editor.setTextCursor(cursor);
    editor.insertPlainText(text);
    QTest::qWait(20);
}

void checkLazyVersion() {
    const std::string old =
        "<!-- zametti\ncreated: 2024-05-01T10:00:00+02:00\nmodified: 2024-06-01T12:00:00+02:00\n"
        "-->\n\nстарая заметка\n";
    const QString path = writeFile(QStringLiteral("старая.md"), QString::fromStdString(old));

    zametti::NoteEditor editor;
    editor.resize(700, 500);
    editor.show();
    QTest::qWait(20);
    editor.openFile(path);
    QTest::qWait(20);

    // Открыли, записали принудительно (так делает уход из заметки и выход из
    // программы) — ничего не правили, файл нетронут, версии нет.
    editor.save(false, /*force=*/true);
    QTest::qWait(20);
    ZT_EQ("без правок файл байт в байт", old, readFile(path));

    // Правка — и версия первой строкой, modified поднят.
    typeAtEnd(editor, QStringLiteral(" дописано"));
    editor.save(false);
    QTest::qWait(20);
    const std::string written = readFile(path);
    ZT_TRUE("правленая заметка получила version: 1 первой строкой",
            written.rfind("<!-- zametti\nversion: 1\ncreated: ", 0) == 0);
    ZT_TRUE("правка в файле", written.find("дописано") != std::string::npos);
    ZT_TRUE("modified переписан", written.find("modified: 2024-06-01T12:00:00+02:00") == std::string::npos);

    // Повторная запись без правок версию не двоит и файл не трогает.
    editor.save(false, /*force=*/true);
    QTest::qWait(20);
    ZT_EQ("второй записи без правок нет", written, readFile(path));
}

void checkNewerVersionKept() {
    const std::string newer = "<!-- zametti\nversion: 2\ncreated: 2024-05-01T10:00:00+02:00\n-->\n\nновее\n";
    const QString path = writeFile(QStringLiteral("новее.md"), QString::fromStdString(newer));

    zametti::NoteEditor editor;
    editor.resize(700, 500);
    editor.show();
    QTest::qWait(20);
    editor.openFile(path);
    QTest::qWait(20);
    typeAtEnd(editor, QStringLiteral(" правка"));
    editor.save(false);
    QTest::qWait(20);
    const std::string written = readFile(path);
    ZT_TRUE("версия 2 не понижена", written.rfind("<!-- zametti\nversion: 2\n", 0) == 0);
    ZT_TRUE("и не удвоена", written.find("version: 1") == std::string::npos);
}

}  // namespace

TEST(VersionStamp, All) {
    g_dir = zt::TestData::outDir(QStringLiteral("version-stamp"));
    checkLazyVersion();
    checkNewerVersionKept();
}
