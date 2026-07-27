// Виджет редактора на живом окне: что записывается в историю, а что нет.
//
// Единственный тест, которому нужен QtWidgets. Бриф просил обойтись без них, и
// слой модели без них и обходится — за этим следит view-without-widgets. Но обе
// ошибки, ради которых этот тест написан, живут именно в виджете и никаким
// тестом над QTextDocument не ловятся: перекладка полей под ширину окна
// записывалась в историю как правка, а первая правка в только что открытой
// заметке подмешивалась к её исходному состоянию и не отменялась.

#include "doc_model.h"
#include "document_reader.h"
#include "editor_widget.h"
#include "serializer.h"
#include "settings.h"
#include "test_util.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QTest>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextFragment>
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
                expected.toUtf8().constData(), actual.toUtf8().constData());
}

QString readFile(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return QStringLiteral("<нет файла>");
    return QString::fromUtf8(file.readAll());
}

QString writeNote(const char* name, const QString& text) {
    const QString path = g_dir + QLatin1Char('/') + QLatin1String(name);
    QFile file(path);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) file.write(text.toUtf8());
    return path;
}

QString firstLine(const zametti::NoteEditor& editor) {
    return editor.document()->firstBlock().text();
}

void typeAtEnd(zametti::NoteEditor& editor, const QString& text) {
    QTextCursor cursor = editor.textCursor();
    cursor.movePosition(QTextCursor::EndOfBlock);
    editor.setTextCursor(cursor);
    editor.insertPlainText(text);
    QTest::qWait(10);
}

// Просто открыть заметку — не значит её изменить. Ошибка здесь означала бы, что
// чтение чужого файла его переписывает.
void checkOpenDoesNotTouchFile() {
    const QString source = QStringLiteral("#   заголовок с лишними пробелами\n\n*   буллет\n");
    const QString path = writeNote("некано.md", source);

    zametti::NoteEditor editor;
    editor.resize(700, 500);
    editor.show();
    QTest::qWait(20);
    editor.openFile(path);
    QTest::qWait(20);
    editor.save(false);

    checkEqual(source, readFile(path), "открытие заметки не должно её менять");
}

// Правило этапа: документ — содержимое, а не облик. Undo возвращает текст и не
// трогает масштаб.
void checkUndoKeepsAppearance() {
    const QString path = writeNote("правка.md", QStringLiteral("первая строка\n\n- буллет\n"));

    zametti::NoteEditor editor;
    editor.resize(700, 500);
    editor.show();
    QTest::qWait(20);
    editor.openFile(path);

    typeAtEnd(editor, QStringLiteral(" мама мыла раму"));
    checkEqual(QStringLiteral("первая строка мама мыла раму"), firstLine(editor),
               "набранное должно оказаться в документе");

    editor.applyZoom(2.0);
    QTest::qWait(10);
    checkEqual(QStringLiteral("первая строка мама мыла раму"), firstLine(editor),
               "масштаб не должен менять текст");

    editor.undo();
    QTest::qWait(10);
    checkEqual(QStringLiteral("первая строка"), firstLine(editor),
               "undo возвращает текст");
    check(editor.zoom() == qreal(2.0), "undo не должен откатывать масштаб");

    editor.redo();
    QTest::qWait(10);
    checkEqual(QStringLiteral("первая строка мама мыла раму"), firstLine(editor),
               "redo возвращает отменённое");

    editor.save(false);
    checkEqual(QStringLiteral("первая строка мама мыла раму\n\n- буллет\n"), readFile(path),
               "сохранённое содержимое");
}

// Смена облика шага истории не заводит: после зума и перекладки окна одного
// undo обязано хватить, чтобы вернуться к исходному тексту.
void checkAppearanceMakesNoHistoryStep() {
    const QString path = writeNote("облик.md", QStringLiteral("текст\n"));

    zametti::NoteEditor editor;
    editor.resize(700, 500);
    editor.show();
    QTest::qWait(20);
    editor.openFile(path);

    typeAtEnd(editor, QStringLiteral(" правка"));
    // Пауза дольше окна слипания: иначе смена облика подмешалась бы к серии
    // набора и завела бы не новый шаг, а правку текущего — то есть проверка
    // прошла бы и со сломанным кодом.
    QTest::qWait(zametti::appearance().undoCoalesceMs * 2);

    // Всё, что ниже, — облик: масштаб и ширина окна. Ширина особенно коварна:
    // она двигает поля документа, а документ шлёт contentsChanged и на это.
    // Ширины взяты по обе стороны от предела колонки (layout.maxContentWidth),
    // иначе поля не сдвинутся вовсе и проверять будет нечего.
    editor.applyZoom(1.5);
    QTest::qWait(10);
    editor.resize(600, 500);
    QTest::qWait(30);
    editor.resize(1600, 500);
    QTest::qWait(30);
    editor.resize(700, 500);
    QTest::qWait(30);
    editor.applyZoom(1.0);
    QTest::qWait(10);

    editor.undo();
    QTest::qWait(10);
    checkEqual(QStringLiteral("текст"), firstLine(editor),
               "одного undo хватает: облик шагов не заводит");
}

// Первая правка в только что открытой заметке обязана отменяться. Если серия
// набора тянется из прошлой заметки, эта правка сольётся с её исходным
// состоянием, и отменять станет нечего.
void checkFirstEditAfterOpenIsUndoable() {
    const QString first = writeNote("одна.md", QStringLiteral("одна\n"));
    const QString second = writeNote("другая.md", QStringLiteral("другая\n"));

    zametti::NoteEditor editor;
    editor.resize(700, 500);
    editor.show();
    QTest::qWait(20);

    editor.openFile(first);
    typeAtEnd(editor, QStringLiteral(" правка"));

    // Сразу, без паузы: как раз тот случай, когда серия набора могла бы
    // перетечь в новую заметку.
    editor.openFile(second);
    typeAtEnd(editor, QStringLiteral(" ещё"));
    checkEqual(QStringLiteral("другая ещё"), firstLine(editor), "правка во второй заметке");

    editor.undo();
    QTest::qWait(10);
    checkEqual(QStringLiteral("другая"), firstLine(editor),
               "первая правка после открытия обязана отменяться");

    editor.save(false);
    checkEqual(QStringLiteral("одна правка\n"), readFile(first),
               "первая заметка сохранена при переходе ко второй");
}

// Клавиши доходят до операций, и каждая операция — ровно один шаг отмены.
void checkKeysAreOperations() {
    const QString path = writeNote("клавиши.md", QStringLiteral("- пункт\n"));

    zametti::NoteEditor editor;
    editor.resize(700, 500);
    editor.show();
    QTest::qWait(20);
    editor.openFile(path);

    QTextCursor cursor = editor.textCursor();
    cursor.movePosition(QTextCursor::End);
    editor.setTextCursor(cursor);

    QTest::keyClick(&editor, Qt::Key_Return);
    QTest::qWait(10);
    check(editor.document()->blockCount() == 2, "Enter завёл новый пункт");
    check(zametti::isListBlock(editor.document()->findBlockByNumber(1)),
          "новый блок — пункт списка");

    // Набираем в новом пункте и убеждаемся, что это отдельный шаг.
    editor.insertPlainText(QStringLiteral("второй"));
    QTest::qWait(10);

    editor.undo();
    QTest::qWait(10);
    checkEqual(QStringLiteral("- пункт\n-\n"),
               QString::fromStdString(zametti::serialize(
                   zametti::readDocument(*editor.document()))),
               "первый undo снимает набор, но не Enter");

    editor.undo();
    QTest::qWait(10);
    checkEqual(QStringLiteral("- пункт\n"),
               QString::fromStdString(zametti::serialize(
                   zametti::readDocument(*editor.document()))),
               "второй undo снимает Enter");

    // Backspace в начале пункта снимает список — тоже одним шагом.
    editor.redo();
    QTest::qWait(10);
    cursor = editor.textCursor();
    cursor.setPosition(editor.document()->findBlockByNumber(1).position());
    editor.setTextCursor(cursor);
    QTest::keyClick(&editor, Qt::Key_Backspace);
    QTest::qWait(10);
    check(!zametti::isListBlock(editor.document()->findBlockByNumber(1)),
          "Backspace в начале пункта снял список");

    editor.undo();
    QTest::qWait(10);
    check(zametti::isListBlock(editor.document()->findBlockByNumber(1)),
          "undo вернул пункт списком");
}

// Tab, Shift+Tab и переключатель задачи доходят до операций через клавиатуру.
void checkListKeys() {
    const QString path =
        writeNote("списки.md", QStringLiteral("- раз\n- два\n- [ ] дело\n"));

    zametti::NoteEditor editor;
    editor.resize(700, 500);
    editor.show();
    QTest::qWait(20);
    editor.openFile(path);

    auto putCursorIn = [&editor](int block) {
        QTextCursor cursor = editor.textCursor();
        cursor.setPosition(editor.document()->findBlockByNumber(block).position());
        editor.setTextCursor(cursor);
    };
    auto text = [&editor] {
        return QString::fromStdString(
            zametti::serialize(zametti::readDocument(*editor.document())));
    };

    putCursorIn(1);
    QTest::keyClick(&editor, Qt::Key_Tab);
    QTest::qWait(10);
    checkEqual(QStringLiteral("- раз\n  - два\n- [ ] дело\n"), text(),
               "Tab увёл пункт на уровень внутрь");

    QTest::keyClick(&editor, Qt::Key_Backtab);
    QTest::qWait(10);
    checkEqual(QStringLiteral("- раз\n- два\n- [ ] дело\n"), text(),
               "Shift+Tab вернул его обратно");

    // Переключатель задачи — сочетание из конфига.
    const QKeySequence toggle(zametti::appearance().toggleTaskKey,
                              QKeySequence::PortableText);
    check(toggle.count() == 1, "хоткей переключателя разобран");
    putCursorIn(2);
    QTest::keyClick(&editor, Qt::Key(toggle[0].key()), toggle[0].keyboardModifiers());
    QTest::qWait(10);
    checkEqual(QStringLiteral("- раз\n- два\n- [x] дело\n"), text(),
               "задача отмечена");

    QTest::keyClick(&editor, Qt::Key(toggle[0].key()), toggle[0].keyboardModifiers());
    QTest::qWait(10);
    checkEqual(QStringLiteral("- раз\n- два\n- [ ] дело\n"), text(),
               "и снята");

    // Каждое нажатие — свой шаг истории.
    editor.undo();
    QTest::qWait(10);
    checkEqual(QStringLiteral("- раз\n- два\n- [x] дело\n"), text(),
               "undo снимает ровно последнее переключение");
}

// Перемещение пунктов с клавиатуры, вместе с поддеревом и с курсором.
void checkMoveKeys() {
    const QString path = writeNote(
        "перестановка.md", QStringLiteral("- раз\n  - вложенный\n- два\n- три\n"));

    zametti::NoteEditor editor;
    editor.resize(700, 500);
    editor.show();
    QTest::qWait(20);
    editor.openFile(path);

    auto text = [&editor] {
        return QString::fromStdString(
            zametti::serialize(zametti::readDocument(*editor.document())));
    };
    auto press = [&editor](const QString& keys) {
        const QKeySequence sequence(keys, QKeySequence::PortableText);
        QTest::keyClick(&editor, Qt::Key(sequence[0].key()),
                        sequence[0].keyboardModifiers());
        QTest::qWait(10);
    };

    // Курсор в первом пункте, у которого есть вложенный.
    QTextCursor cursor = editor.textCursor();
    cursor.setPosition(editor.document()->findBlockByNumber(0).position() + 2);
    editor.setTextCursor(cursor);

    press(zametti::appearance().moveDownKey);
    checkEqual(QStringLiteral("- два\n- раз\n  - вложенный\n- три\n"), text(),
               "пункт уехал вниз вместе с вложенным");
    checkEqual(QStringLiteral("раз"), editor.textCursor().block().text(),
               "курсор остался в перемещённом пункте");
    check(editor.textCursor().positionInBlock() == 2, "и на том же месте в нём");

    press(zametti::appearance().moveUpKey);
    checkEqual(QStringLiteral("- раз\n  - вложенный\n- два\n- три\n"), text(),
               "и вернулся обратно");

    // Каждое перемещение — свой шаг истории.
    editor.undo();
    QTest::qWait(10);
    checkEqual(QStringLiteral("- два\n- раз\n  - вложенный\n- три\n"), text(),
               "undo отменяет ровно последнее перемещение");
}

// Начертание: на выделение — правка документа, без выделения — формат для
// следующей буквы.
void checkInlineStyle() {
    const QString path = writeNote("начертание.md", QStringLiteral("обычный текст\n"));

    zametti::NoteEditor editor;
    editor.resize(700, 500);
    editor.show();
    QTest::qWait(20);
    editor.openFile(path);

    auto text = [&editor] {
        return QString::fromStdString(
            zametti::serialize(zametti::readDocument(*editor.document())));
    };

    // На выделение.
    QTextCursor cursor = editor.textCursor();
    cursor.setPosition(editor.document()->firstBlock().position());
    cursor.setPosition(editor.document()->firstBlock().position() + 7,
                       QTextCursor::KeepAnchor);
    editor.setTextCursor(cursor);
    QTest::keyClick(&editor, Qt::Key_B, Qt::ControlModifier);
    QTest::qWait(10);
    checkEqual(QStringLiteral("**обычный** текст\n"), text(), "Ctrl+B на выделении");

    editor.undo();
    QTest::qWait(10);
    checkEqual(QStringLiteral("обычный текст\n"), text(), "и отменяется одним шагом");

    // Без выделения: следующая набранная буква идёт жирной.
    cursor = editor.textCursor();
    cursor.movePosition(QTextCursor::End);
    editor.setTextCursor(cursor);
    QTest::keyClick(&editor, Qt::Key_B, Qt::ControlModifier);
    QTest::qWait(10);
    editor.insertPlainText(QStringLiteral(" жирное"));
    QTest::qWait(10);
    checkEqual(QStringLiteral("обычный текст** жирное**\n"), text(),
               "набор после Ctrl+B идёт жирным");
}

// Автозамена при наборе и её отдельный шаг отмены.
void checkInputRules() {
    const QString path = writeNote("автозамена.md", QStringLiteral("текст\n"));

    zametti::NoteEditor editor;
    editor.resize(700, 500);
    editor.show();
    QTest::qWait(20);
    editor.openFile(path);

    auto text = [&editor] {
        return QString::fromStdString(
            zametti::serialize(zametti::readDocument(*editor.document())));
    };

    QTextCursor cursor = editor.textCursor();
    cursor.setPosition(editor.document()->firstBlock().position());
    editor.setTextCursor(cursor);

    // Звёздочка — то, что просили: способ ввода, в файл уходит дефис.
    QTest::keyClick(&editor, Qt::Key_Asterisk);
    QTest::keyClick(&editor, Qt::Key_Space);
    QTest::qWait(10);
    checkEqual(QStringLiteral("- текст\n"), text(), "звёздочка с пробелом дала буллет");

    // Первый Ctrl+Z возвращает набранное, а не отменяет всё сразу.
    editor.undo();
    QTest::qWait(10);
    checkEqual(QStringLiteral("\\* текст\n"), text(),
               "первый undo возвращает набранные знаки");

    editor.undo();
    QTest::qWait(10);
    checkEqual(QStringLiteral("текст\n"), text(), "второй undo снимает набор");
}

// Выделение обязано пережить операцию: она могла тронуть десяток пунктов, и
// терять его после этого — значит заставлять выделять заново.
void checkSelectionSurvivesOperation() {
    const QString path = writeNote(
        "выделение.md", QStringLiteral("- [ ] раз\n- [ ] два\n- [ ] три\n"));

    zametti::NoteEditor editor;
    editor.resize(700, 500);
    editor.show();
    QTest::qWait(20);
    editor.openFile(path);

    auto text = [&editor] {
        return QString::fromStdString(
            zametti::serialize(zametti::readDocument(*editor.document())));
    };

    QTextCursor cursor = editor.textCursor();
    cursor.setPosition(editor.document()->findBlockByNumber(0).position());
    const QTextBlock last = editor.document()->findBlockByNumber(2);
    cursor.setPosition(last.position() + last.length() - 1, QTextCursor::KeepAnchor);
    editor.setTextCursor(cursor);
    const int anchor = editor.textCursor().anchor();
    const int position = editor.textCursor().position();

    const QKeySequence toggle(zametti::appearance().toggleTaskKey,
                              QKeySequence::PortableText);
    QTest::keyClick(&editor, Qt::Key(toggle[0].key()), toggle[0].keyboardModifiers());
    QTest::qWait(10);

    checkEqual(QStringLiteral("- [x] раз\n- [x] два\n- [x] три\n"), text(),
               "переключились все три задачи");
    check(editor.textCursor().hasSelection(), "выделение обязано остаться");
    check(editor.textCursor().anchor() == anchor && editor.textCursor().position() == position,
          "и остаться на прежних границах");

    // Второе нажатие подряд должно снять отметки со всех — то есть выделение
    // действительно живо, а не просто «что-то выделено».
    QTest::keyClick(&editor, Qt::Key(toggle[0].key()), toggle[0].keyboardModifiers());
    QTest::qWait(10);
    checkEqual(QStringLiteral("- [ ] раз\n- [ ] два\n- [ ] три\n"), text(),
               "второе нажатие снимает отметки со всех");
}

// Отмена с клавиатуры, а не вызовом метода. Разница не умозрительная: QTextEdit
// объявляет Ctrl+Z своим и глотает его, так что ярлык окна не срабатывает ни
// разу — а тест, зовущий undo() напрямую, этого не замечает. Именно так ошибка
// и прожила незамеченной.
void checkUndoFromKeyboard() {
    const QString path = writeNote("отмена-клавишей.md", QStringLiteral("текст\n"));

    zametti::NoteEditor editor;
    editor.resize(700, 500);
    editor.show();
    QTest::qWait(20);
    editor.setFocus();
    editor.openFile(path);

    auto text = [&editor] {
        return QString::fromStdString(
            zametti::serialize(zametti::readDocument(*editor.document())));
    };

    QTextCursor cursor = editor.textCursor();
    cursor.movePosition(QTextCursor::End);
    editor.setTextCursor(cursor);
    editor.insertPlainText(QStringLiteral(" правка"));
    QTest::qWait(10);

    QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
    QTest::qWait(10);
    checkEqual(QStringLiteral("текст\n"), text(), "Ctrl+Z с клавиатуры отменяет");

    QTest::keyClick(&editor, Qt::Key_Y, Qt::ControlModifier);
    QTest::qWait(10);
    checkEqual(QStringLiteral("текст правка\n"), text(), "Ctrl+Y возвращает");

    QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
    QTest::qWait(10);
    QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
    QTest::qWait(10);
    // Ctrl+Shift+Z здесь — повтор, а не отмена: так его понимает система.
    checkEqual(QStringLiteral("текст правка\n"), text(), "Ctrl+Shift+Z возвращает");
}

// Текст после переноса строки обязан набираться тем же кеглем. Разделитель
// строк шрифту неизвестен, и без оговорки он попадал под правило увеличения
// эмодзи — а набранное сразу после него наследовало крупный формат.
void checkSizeAfterSoftBreak() {
    const QString path = writeNote("кегль.md", QString());

    zametti::NoteEditor editor;
    editor.resize(700, 500);
    editor.show();
    QTest::qWait(20);
    editor.setFocus();
    editor.openFile(path);

    editor.insertPlainText(QStringLiteral("первая"));
    QTest::qWait(10);
    QTest::keyClick(&editor, Qt::Key_Return);
    QTest::qWait(10);
    editor.insertPlainText(QStringLiteral("вторая"));
    QTest::qWait(10);

    qreal smallest = 0;
    qreal largest = 0;
    for (QTextBlock block = editor.document()->begin(); block.isValid();
         block = block.next()) {
        for (QTextBlock::iterator it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            if (!fragment.isValid() || fragment.text().isEmpty()) continue;
            const qreal size = fragment.charFormat().fontPointSize();
            if (smallest == 0 || size < smallest) smallest = size;
            if (size > largest) largest = size;
        }
    }
    check(smallest > 0 && smallest == largest,
          "кегль после переноса строки не должен меняться");
}

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    if (argc < 2) {
        std::printf("использование: editor_test <каталог для временных файлов>\n");
        return 2;
    }

    // Окно слипания набора укорачиваем: иначе пауза в тесте была бы почти
    // секундой на каждую проверку.
    zametti::appearance().undoCoalesceMs = 40;

    g_dir = QString::fromLocal8Bit(argv[1]) + QStringLiteral("/editor-data");
    QDir(g_dir).removeRecursively();
    if (!QDir().mkpath(g_dir)) {
        std::printf("не создать каталог %s\n", g_dir.toUtf8().constData());
        return 2;
    }

    checkOpenDoesNotTouchFile();
    checkUndoKeepsAppearance();
    checkAppearanceMakesNoHistoryStep();
    checkFirstEditAfterOpenIsUndoable();
    checkKeysAreOperations();
    checkListKeys();
    checkMoveKeys();
    checkInlineStyle();
    checkInputRules();
    checkSelectionSurvivesOperation();
    checkUndoFromKeyboard();
    checkSizeAfterSoftBreak();

    std::printf("проверок %d, провалов %d\n", zt::g_checks, zt::g_failures);
    return zt::g_failures == 0 ? 0 : 1;
}
