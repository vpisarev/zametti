// Внешние изменения файла.
//
// Претензия, ради которой всё затевалось: редактор, молча перечитавший файл и
// потерявший undo, — это провал. Проверяются оба сценария.

#include "document_reader.h"
#include "editor_widget.h"
#include "serializer.h"
#include "test_util.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QScrollBar>
#include <QTest>
#include <QTextCursor>
#include <QTextDocument>

#include <string>

namespace {

QString g_dir;

void check(bool ok, const std::string& what) {
    ++zt::g_checks;
    if (ok) return;
    ++zt::g_failures;
    std::printf("провал: %s\n", what.c_str());
}

void checkEqual(const QString& expected, const QString& actual, const std::string& what) {
    ++zt::g_checks;
    if (expected == actual) return;
    ++zt::g_failures;
    std::printf("провал: %s\n  ждали:  %s\n  вышло:  %s\n", what.c_str(),
                expected.toUtf8().replace("\n", "\\n").constData(),
                actual.toUtf8().replace("\n", "\\n").constData());
}

void writeFile(const QString& path, const QString& text) {
    QFile file(path);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) file.write(text.toUtf8());
}

QString textOf(const zametti::NoteEditor& editor) {
    return QString::fromStdString(
        zametti::serialize(zametti::readDocument(*editor.document())));
}

// Слежение за файлом идёт через операционную систему, и мгновенным оно не
// бывает. Ждём события, а не гадаем о задержке.
void waitForWatcher(const zametti::NoteEditor& editor, const QString& expected) {
    for (int i = 0; i < 100 && textOf(editor) != expected; ++i) QTest::qWait(20);
}

void waitForConflict(const zametti::NoteEditor& editor) {
    for (int i = 0; i < 100 && !editor.hasExternalConflict(); ++i) QTest::qWait(20);
}

// Без несохранённых правок внешнее содержимое применяется само — и отменяется,
// как обычная правка.
void checkAdoptsWhenClean() {
    const QString path = g_dir + QStringLiteral("/чистая.md");
    writeFile(path, QStringLiteral("исходный текст\n"));

    zametti::NoteEditor editor;
    editor.resize(700, 500);
    editor.show();
    QTest::qWait(20);
    editor.openFile(path);
    QTest::qWait(20);

    writeFile(path, QStringLiteral("правка снаружи\n"));
    waitForWatcher(editor, QStringLiteral("правка снаружи\n"));

    checkEqual(QStringLiteral("правка снаружи\n"), textOf(editor),
               "внешнее содержимое применилось само");
    check(!editor.hasExternalConflict(), "спрашивать было не о чем");

    editor.undo();
    QTest::qWait(20);
    checkEqual(QStringLiteral("исходный текст\n"), textOf(editor),
               "undo возвращает состояние до внешнего изменения");
}

// С несохранёнными правками ничего не затирается молча: ждём ответа.
void checkAsksWhenDirty() {
    const QString path = g_dir + QStringLiteral("/правленая.md");
    writeFile(path, QStringLiteral("исходный текст\n"));

    zametti::NoteEditor editor;
    editor.resize(700, 500);
    editor.show();
    QTest::qWait(20);
    editor.openFile(path);
    QTest::qWait(20);

    QTextCursor cursor = editor.textCursor();
    cursor.movePosition(QTextCursor::End);
    editor.setTextCursor(cursor);
    editor.insertPlainText(QStringLiteral(" и моя правка"));
    QTest::qWait(20);

    writeFile(path, QStringLiteral("правка снаружи\n"));
    waitForConflict(editor);

    check(editor.hasExternalConflict(), "должен был спросить");
    checkEqual(QStringLiteral("исходный текст и моя правка\n"), textOf(editor),
               "до ответа документ не меняется");

    // «Оставить мои»: наша версия остаётся и уходит в файл при сохранении.
    editor.resolveExternalConflict(false);
    QTest::qWait(20);
    checkEqual(QStringLiteral("исходный текст и моя правка\n"), textOf(editor),
               "выбор «оставить мои» документ не трогает");
    editor.save(false);
    QTest::qWait(20);

    QFile written(path);
    check(written.open(QIODevice::ReadOnly), "файл не читается");
    checkEqual(QStringLiteral("исходный текст и моя правка\n"),
               QString::fromUtf8(written.readAll()),
               "наша версия перезаписала внешнюю");
}

// «Взять внешние» — тоже обычный шаг истории, свои правки возвращаются отменой.
void checkTakesExternal() {
    const QString path = g_dir + QStringLiteral("/взять-внешние.md");
    writeFile(path, QStringLiteral("исходный текст\n"));

    zametti::NoteEditor editor;
    editor.resize(700, 500);
    editor.show();
    QTest::qWait(20);
    editor.openFile(path);
    QTest::qWait(20);

    QTextCursor cursor = editor.textCursor();
    cursor.movePosition(QTextCursor::End);
    editor.setTextCursor(cursor);
    editor.insertPlainText(QStringLiteral(" и моя правка"));
    QTest::qWait(20);

    writeFile(path, QStringLiteral("правка снаружи\n"));
    waitForConflict(editor);
    check(editor.hasExternalConflict(), "должен был спросить");

    editor.resolveExternalConflict(true);
    QTest::qWait(20);
    checkEqual(QStringLiteral("правка снаружи\n"), textOf(editor), "взяли внешнее");

    editor.undo();
    QTest::qWait(20);
    checkEqual(QStringLiteral("исходный текст и моя правка\n"), textOf(editor),
               "свои правки возвращаются отменой");
}

// Собственная запись не должна выглядеть как чужая правка.
void checkOwnSaveIsNotExternal() {
    const QString path = g_dir + QStringLiteral("/своя-запись.md");
    writeFile(path, QStringLiteral("текст\n"));

    zametti::NoteEditor editor;
    editor.resize(700, 500);
    editor.show();
    QTest::qWait(20);
    editor.openFile(path);
    QTest::qWait(20);

    QTextCursor cursor = editor.textCursor();
    cursor.movePosition(QTextCursor::End);
    editor.setTextCursor(cursor);
    editor.insertPlainText(QStringLiteral(" дописали"));
    QTest::qWait(20);
    editor.save(false);

    // Даём слежению время сработать: если оно примет нашу запись за чужую,
    // появится вопрос, которого быть не должно.
    QTest::qWait(400);
    check(!editor.hasExternalConflict(), "собственная запись не должна вызывать вопрос");
    checkEqual(QStringLiteral("текст дописали\n"), textOf(editor), "документ цел");
}

// Заметку снаружи урезали до пары строк, а курсор стоял далеко внизу. Позиция
// обязана поджаться в границы нового документа — иначе следующая же правка
// пришлась бы мимо, а прокрутка осталась бы за пределами.
void checkShrunkFromOutside() {
    const QString path = g_dir + QStringLiteral("/урезали.md");
    QString big;
    for (int i = 0; i < 200; ++i)
        big += QStringLiteral("Строка номер %1 длинной заметки.\n\n").arg(i);
    writeFile(path, big);

    zametti::NoteEditor editor;
    editor.resize(700, 500);
    editor.show();
    QTest::qWait(30);
    editor.setFocus();
    editor.openFile(path);
    QTest::qWait(20);

    QTextCursor cursor = editor.textCursor();
    cursor.movePosition(QTextCursor::End);
    editor.setTextCursor(cursor);
    editor.ensureCursorVisible();
    QTest::qWait(20);
    check(editor.textCursor().position() > 1000, "курсор должен стоять далеко внизу");

    writeFile(path, QStringLiteral("коротко\n\nи всё\n"));
    waitForWatcher(editor, QStringLiteral("коротко\n\nи всё\n"));

    check(editor.textCursor().position() < editor.document()->characterCount(),
          "курсор поджался в границы урезанной заметки");
    check(editor.verticalScrollBar()->value() <= editor.verticalScrollBar()->maximum(),
          "прокрутка не осталась за пределами");

    // И правка после этого должна лечь туда, где стоит курсор.
    editor.insertPlainText(QStringLiteral(" дописано"));
    QTest::qWait(20);
    checkEqual(QStringLiteral("коротко\n\nи всё дописано\n"), textOf(editor),
               "набор после урезания ложится по месту");
}

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    if (argc < 2) {
        std::printf("использование: external_change_test <каталог для временных файлов>\n");
        return 2;
    }

    g_dir = QString::fromLocal8Bit(argv[1]) + QStringLiteral("/external-data");
    QDir(g_dir).removeRecursively();
    if (!QDir().mkpath(g_dir)) {
        std::printf("не создать каталог %s\n", g_dir.toUtf8().constData());
        return 2;
    }

    checkAdoptsWhenClean();
    checkAsksWhenDirty();
    checkTakesExternal();
    checkOwnSaveIsNotExternal();
    checkShrunkFromOutside();

    std::printf("проверок %d, провалов %d\n", zt::g_checks, zt::g_failures);
    return zt::g_failures == 0 ? 0 : 1;
}
