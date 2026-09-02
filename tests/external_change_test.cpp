// Внешние изменения файла.
//
// Претензия, ради которой всё затевалось: редактор, молча перечитавший файл и
// потерявший undo, — это провал. Проверяются оба сценария.

#include "pieces.h"
#include "editor_widget.h"
#include "times.h"

#include "test_util.h"

#include <vector>
#include "testdata.h"

#include "scratch_files.h"
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QScrollBar>
#include <QSignalSpy>
#include <QRegularExpression>
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

QString readFile(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return QStringLiteral("<нет файла>");
    return QString::fromUtf8(file.readAll());
}

void writeFile(const QString& path, const QString& text) {
    QFile file(path);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) file.write(text.toUtf8());
}

QString textOf(const zametti::NoteEditor& editor) {
    return QString::fromStdString(
        markdownOf(blocksOf(*editor.document())));
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

// Внешние редакторы пишут «обрезать до нуля → записать». Сторож стреляет и на
// пустом файле посреди записи; без отстойника пустой документ попадал в
// историю, и первый Ctrl+Z после внешней правки давал пустую заметку.
void checkTruncateWriteRace() {
    const QString path = g_dir + QStringLiteral("/гонка.md");
    writeFile(path, QStringLiteral("раз\n\nдва\n"));

    zametti::NoteEditor editor;
    editor.resize(700, 500);
    editor.show();
    QTest::qWait(20);
    editor.openFile(path);
    QTest::qWait(20);

    // Чужая запись в два приёма. Пауза больше отстойника (150 мс), но меньше
    // повторной попытки для опустевшего файла (ещё 300 мс): без отстойника
    // пустота успевает усвоиться, с ним — перечитывается уже полный файл.
    writeFile(path, QString());
    QTest::qWait(220);
    writeFile(path, QStringLiteral("раз\n\nдва изменено\n"));
    const QString expected = QStringLiteral("раз\n\nдва изменено\n");
    for (int i = 0; i < 150 && textOf(editor) != expected; ++i) QTest::qWait(20);
    checkEqual(expected, textOf(editor), "внешняя правка подтянулась");

    QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
    QTest::qWait(20);
    checkEqual(QStringLiteral("раз\n\nдва\n"), textOf(editor),
               "первый Ctrl+Z возвращает состояние до внешней правки, а не пустоту");
}

// --- внешний редактор и метаданные (этап 4) ---------------------------------
//
// Инвариант: тихой потери метаданных не бывает. Либо они валидны, либо
// предложено восстановление. Правку parent руками мы считаем законным
// переносом, а не потерей, — это решение брифа.

// Тело правится, шапка цела: обычное перечитывание, никаких вопросов.
void checkExternalBodyEdit() {
    const QString path = g_dir + QStringLiteral("/00000000000001.md");
    writeFile(path, QStringLiteral("<!-- zametti\nid: 00000000000001\n"
                                   "created: 2020-01-01T00:00:00Z\nparent: 0000000000000p\n"
                                   "-->\n\n# Заметка\n\nстарое тело\n"));
    zametti::NoteEditor editor;
    editor.resize(700, 500);
    editor.show();
    QTest::qWait(20);
    editor.openFile(path);
    QTest::qWait(20);

    QSignalSpy damaged(&editor, &zametti::NoteEditor::metaDamaged);
    writeFile(path, QStringLiteral("<!-- zametti\nid: 00000000000001\n"
                                   "created: 2020-01-01T00:00:00Z\nparent: 0000000000000p\n"
                                   "-->\n\n# Заметка\n\nновое тело\n"));
    waitForWatcher(editor, QStringLiteral("# Заметка\n\nновое тело\n"));
    checkEqual(QStringLiteral("# Заметка\n\nновое тело\n"), textOf(editor),
               "правка тела снаружи подтягивается");
    check(damaged.isEmpty(), "целая шапка вопросов не вызывает");
    check(!editor.hasDamagedMeta(), "чинить нечего");
}

// ВНЕШНЯЯ ПРАВКА И ВОЗВРАТ ИЗ РЕЖИМА ИСХОДНИКА — ОДИН ПУТЬ (applySourceText):
// отступы живут ОБЫЧНЫМИ пробелами (канон 03.09.2026): старые неразрывные из
// файла мигрируют в обычные при чтении, набранные снаружи обычные остаются
// собой, а undo возвращает всё одним шагом.
void checkExternalKeepsIndent() {
    const QString nbsp(QChar(0xa0));
    const QString path = g_dir + QStringLiteral("/отступы.md");
    writeFile(path, nbsp + nbsp + QStringLiteral("стих\n\nобычный\n"));
    zametti::NoteEditor editor;
    editor.resize(700, 500);
    editor.show();
    QTest::qWait(20);
    editor.openFile(path);
    QTest::qWait(20);

    writeFile(path, nbsp + nbsp + QStringLiteral("стих\n   второй\n\nобычный\n"));
    const QString expected = QStringLiteral("  стих\n   второй\n\nобычный\n");
    waitForWatcher(editor, expected);
    checkEqual(expected, textOf(editor),
               "старые неразрывные мигрировали, обычные ведущие целы");
    editor.undo();
    QTest::qWait(20);
    checkEqual(QStringLiteral("  стих\n\nобычный\n"), textOf(editor),
               "undo возвращает состояние до внешнего изменения одним шагом");
}

// Шапку снесли целиком: предложено восстановление, тело при этом сохраняется.
void checkExternalMetaLost() {
    const QString path = g_dir + QStringLiteral("/00000000000002.md");
    writeFile(path, QStringLiteral("<!-- zametti\nid: 00000000000002\n"
                                   "created: 2019-03-03T00:00:00Z\nparent: 0000000000000p\n"
                                   "-->\n\n# Важная\n\nтело на месте\n"));
    zametti::NoteEditor editor;
    editor.resize(700, 500);
    editor.show();
    QTest::qWait(20);
    editor.openFile(path);
    QTest::qWait(20);

    QSignalSpy damaged(&editor, &zametti::NoteEditor::metaDamaged);
    // Внешний редактор снёс шапку и заодно правил текст.
    writeFile(path, QStringLiteral("# Важная\n\nтело правлено снаружи\n"));
    for (int i = 0; i < 150 && damaged.isEmpty(); ++i) QTest::qWait(20);
    check(!damaged.isEmpty(), "о пропаже метаданных сказано вслух");
    check(editor.hasDamagedMeta(), "прежние значения не потеряны");
    checkEqual(QStringLiteral("# Важная\n\nтело правлено снаружи\n"), textOf(editor),
               "тело осталось внешним");

    editor.restoreDamagedMeta();
    QTest::qWait(50);
    const QString written = readFile(path);
    // created вернулся ТЕМ ЖЕ МОМЕНТОМ, а не той же строкой: сохранение
    // переписывает времена в новом виде (ISO-8601 с офсетом, этап 15), и
    // сравнивать здесь надо моменты — иначе проверка держалась бы за
    // представление, которое мы же и меняем.
    const QRegularExpression createdLine(QStringLiteral("created: ([^\n]+)"));
    const QRegularExpressionMatch got = createdLine.match(written);
    check(got.hasMatch(), "created в шапке есть");
    check(got.hasMatch() &&
              zametti::store::parseNoteTime(got.captured(1)) ==
                  QDateTime::fromString(QStringLiteral("2019-03-03T00:00:00Z"), Qt::ISODate),
          "created вернулся тем же моментом");
    check(written.contains(QStringLiteral("parent: 0000000000000p")), "parent вернулся");
    check(written.contains(QStringLiteral("тело правлено снаружи")),
          "правки тела при починке сохранились");
    check(!editor.hasDamagedMeta(), "чинить больше нечего");
}

// Отказ от починки: заметка живёт без шапки, повторных вопросов нет.
void checkExternalMetaRefused() {
    const QString path = g_dir + QStringLiteral("/00000000000003.md");
    writeFile(path, QStringLiteral("<!-- zametti\nid: 00000000000003\n"
                                   "created: 2019-04-04T00:00:00Z\n-->\n\n# Отказ\n\nтело\n"));
    zametti::NoteEditor editor;
    editor.resize(700, 500);
    editor.show();
    QTest::qWait(20);
    editor.openFile(path);
    QTest::qWait(20);

    QSignalSpy damaged(&editor, &zametti::NoteEditor::metaDamaged);
    writeFile(path, QStringLiteral("# Отказ\n\nтело снаружи\n"));
    for (int i = 0; i < 150 && damaged.isEmpty(); ++i) QTest::qWait(20);
    check(!damaged.isEmpty(), "спросили");
    editor.forgetDamagedMeta();
    check(!editor.hasDamagedMeta(), "отказ запомнен");

    // Правка после отказа не воскрешает шапку самовольно.
    QTest::keyClick(&editor, Qt::Key_A);
    editor.save(false);
    QTest::qWait(50);
    check(!readFile(path).contains(QStringLiteral("<!-- zametti")),
          "после отказа заметка живёт без метаданных");
}

// parent, заменённый на другой валидный id, — законный перенос: это не
// потеря, и вопросов быть не должно.
void checkExternalParentChange() {
    const QString path = g_dir + QStringLiteral("/00000000000004.md");
    writeFile(path, QStringLiteral("<!-- zametti\nid: 00000000000004\n"
                                   "created: 2021-01-01T00:00:00Z\nparent: 0000000000000a\n"
                                   "-->\n\n# Переезд\n\nтело\n"));
    zametti::NoteEditor editor;
    editor.resize(700, 500);
    editor.show();
    QTest::qWait(20);
    editor.openFile(path);
    QTest::qWait(20);

    QSignalSpy damaged(&editor, &zametti::NoteEditor::metaDamaged);
    QSignalSpy adopted(&editor, &zametti::NoteEditor::externalAdopted);
    writeFile(path, QStringLiteral("<!-- zametti\nid: 00000000000004\n"
                                   "created: 2021-01-01T00:00:00Z\nparent: 0000000000000b\n"
                                   "-->\n\n# Переезд\n\nтело\n"));
    for (int i = 0; i < 150 && adopted.isEmpty(); ++i) QTest::qWait(20);
    check(!adopted.isEmpty(), "о принятой внешней правке сказано — дереву пора обновиться");
    check(damaged.isEmpty(), "смена parent потерей не считается");

    // И сохранение поверх не возвращает прежнего родителя.
    QTest::keyClick(&editor, Qt::Key_B);
    editor.save(false);
    QTest::qWait(50);
    check(readFile(path).contains(QStringLiteral("parent: 0000000000000b")),
          "новый родитель пережил сохранение из приложения");
}

// Род заметки — дело хранилища, а не чужого редактора. Вписанный снаружи
// role снимается молча: заметка папкой не становится никогда, и спрашивать
// об этом человека не о чем.
void checkExternalRoleRefused() {
    const QString path = g_dir + QStringLiteral("/00000000000005.md");
    writeFile(path, QStringLiteral("<!-- zametti\nid: 00000000000005\n"
                                   "created: 2021-01-01T00:00:00Z\n-->\n\n"
                                   "# Обычная\n\nтело\n"));
    zametti::NoteEditor editor;
    editor.resize(700, 500);
    editor.show();
    QTest::qWait(20);
    editor.openFile(path);
    QTest::qWait(20);

    QSignalSpy adopted(&editor, &zametti::NoteEditor::externalAdopted);
    writeFile(path, QStringLiteral("<!-- zametti\nid: 00000000000005\n"
                                   "created: 2021-01-01T00:00:00Z\nrole: folder\n-->\n\n"
                                   "# Обычная\n\nтело\n"));
    for (int i = 0; i < 150 && adopted.isEmpty(); ++i) QTest::qWait(20);
    check(!adopted.isEmpty(), "внешняя правка принята");

    // Сохранение из приложения возвращает файл к правде: role там взяться
    // неоткуда.
    QTest::keyClick(&editor, Qt::Key_B);
    editor.save(false);
    QTest::qWait(50);
    const QString written = readFile(path);
    check(!written.contains(QStringLiteral("role:")),
          "вписанный снаружи role снят при первом же сохранении");
    // Момент тот же; вид метки после сохранения новый (ISO-8601 с офсетом) —
    // это ленивая миграция времён, а не пострадавшая шапка.
    const QRegularExpressionMatch kept =
        QRegularExpression(QStringLiteral("created: ([^\n]+)")).match(written);
    check(kept.hasMatch() &&
              zametti::store::parseNoteTime(kept.captured(1)) ==
                  QDateTime::fromString(QStringLiteral("2021-01-01T00:00:00Z"), Qt::ISODate),
          "остальная шапка не пострадала: created тот же момент");
    check(written.contains(QStringLiteral("тело")), "текст заметки на месте");
}

static int ztRunSuite(int argc, char** argv) {
    if (argc < 2) {
        std::printf("использование: external_change_test <каталог для временных файлов>\n");
        return 2;
    }

    g_dir = QString::fromLocal8Bit(argv[1]) + QStringLiteral("/external-data");
    zt::dropTree(g_dir);
    if (!QDir().mkpath(g_dir)) {
        std::printf("не создать каталог %s\n", g_dir.toUtf8().constData());
        return 2;
    }

    checkAdoptsWhenClean();
    checkAsksWhenDirty();
    checkTakesExternal();
    checkOwnSaveIsNotExternal();
    checkShrunkFromOutside();
    checkTruncateWriteRace();
    checkExternalBodyEdit();
    checkExternalKeepsIndent();
    checkExternalMetaLost();
    checkExternalMetaRefused();
    checkExternalParentChange();
    checkExternalRoleRefused();

    std::printf("проверок %d, провалов %d\n", zt::g_checks, zt::g_failures);
    return zt::freshFailures();
}

// Набор целиком одним TEST: тело не тронуто, argv ему собран здесь.
// Дробить на отдельные проверки — отдельная работа, по одному набору.
TEST(ExternalChange, All) {
    std::vector<QByteArray> ztArgs{QByteArrayLiteral("external_change_test")};
    ztArgs.push_back((zt::TestData::outDir(QStringLiteral("external-change"))).toLocal8Bit());
    std::vector<char*> ztArgv;
    for (QByteArray& a : ztArgs) ztArgv.push_back(a.data());
    EXPECT_EQ(0, ztRunSuite(int(ztArgv.size()), ztArgv.data()));
}
