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
#include "marker.h"
#include "serializer.h"
#include "settings.h"
#include "test_util.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QTest>
#include <QTextBlock>
#include <QAbstractTextDocumentLayout>
#include <QFontMetricsF>
#include <QImage>
#include <QLineEdit>
#include <QScrollBar>
#include <QTextCursor>
#include <QTextFragment>
#include <QTextLayout>
#include <QTextLine>
#include <QTextDocument>

#include <string>
#include <tuple>

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

// Встроенный код с клавиатуры и авто-отступ в блоке кода.
void checkCodeTyping() {
    const QString path = writeNote("код-набором.md", QString());

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

    // Кавычки делают код, и набор после них идёт обычным текстом.
    editor.insertPlainText(QStringLiteral("вот "));
    QTest::keyClick(&editor, Qt::Key_QuoteLeft);
    editor.insertPlainText(QStringLiteral("код"));
    QTest::keyClick(&editor, Qt::Key_QuoteLeft);
    editor.insertPlainText(QStringLiteral(" конец"));
    QTest::qWait(10);
    checkEqual(QStringLiteral("вот `код` конец\n"), text(),
               "кавычки делают код, а дальше идёт обычный текст");

    // Авто-отступ: новая строка блока кода наследует отступ предыдущей.
    const QString code = writeNote("отступ-кода.md", QString());
    editor.openFile(code);
    QTest::qWait(20);
    for (int i = 0; i < 3; ++i) QTest::keyClick(&editor, Qt::Key_QuoteLeft);
    QTest::keyClick(&editor, Qt::Key_Return);
    editor.insertPlainText(QStringLiteral("    if x:"));
    QTest::keyClick(&editor, Qt::Key_Return);
    editor.insertPlainText(QStringLiteral("return 1"));
    QTest::qWait(10);
    checkEqual(QStringLiteral("```\n    if x:\n    return 1\n```\n"), text(),
               "новая строка кода наследует отступ предыдущей");

    // Enter вместе с отступом — один шаг отмены.
    QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
    QTest::qWait(10);
    QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
    QTest::qWait(10);
    checkEqual(QStringLiteral("```\n    if x:\n```\n"), text(),
               "Enter с отступом отменяется одним шагом");
}

// Щелчок по чекбоксу — самый ходовой способ отметить задачу. Проверяется
// настоящим щелчком по вьюпорту, а не вызовом операции: попадание считается по
// геометрии рамки, и ошибиться в ней проще всего именно там.
void checkCheckboxClick() {
    const QString path = writeNote("щелчок.md",
                                   QStringLiteral("- [ ] первая\n- [ ] вторая\n"));

    zametti::NoteEditor editor;
    editor.resize(700, 500);
    editor.show();
    QTest::qWait(30);
    editor.setFocus();
    editor.openFile(path);
    QTest::qWait(20);

    auto text = [&editor] {
        return QString::fromStdString(
            zametti::serialize(zametti::readDocument(*editor.document())));
    };

    auto clickAt = [&editor](const QPointF& point) {
        QTest::mouseClick(
            editor.viewport(), Qt::LeftButton, Qt::NoModifier,
            QPoint(int(point.x()) - editor.horizontalScrollBar()->value(),
                   int(point.y()) - editor.verticalScrollBar()->value()));
        QTest::qWait(10);
    };

    const QRectF box =
        zametti::checkboxRect(editor.document()->firstBlock(), editor.baseFont());
    check(!box.isNull(), "у задачи должна быть рамка чекбокса");

    clickAt(box.center());
    checkEqual(QStringLiteral("- [x] первая\n- [ ] вторая\n"), text(),
               "щелчок по рамке отмечает задачу");

    clickAt(box.center());
    checkEqual(QStringLiteral("- [ ] первая\n- [ ] вторая\n"), text(),
               "второй щелчок снимает отметку");

    // Мимо рамки — обычный щелчок по тексту, отметка не меняется.
    clickAt(box.center() + QPointF(200, 0));
    checkEqual(QStringLiteral("- [ ] первая\n- [ ] вторая\n"), text(),
               "щелчок по тексту отметку не трогает");

    // И щелчок — обычная правка: отменяется.
    clickAt(box.center());
    QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
    QTest::qWait(10);
    checkEqual(QStringLiteral("- [ ] первая\n- [ ] вторая\n"), text(),
               "щелчок отменяется как обычная правка");
}

// Прокрутка при правке. Документ пересобирается целиком, и место в нём надо
// возвращать — но по экранной высоте курсора, а не по доле от высоты заметки:
// высота меняется от правки к правке, и доля каждый раз попадает не туда.
// Замер до починки: заметка уползала вверх на ~25 px за каждый добавленный пункт.
void checkScrollHolds() {
    QString source;
    for (int i = 0; i < 40; ++i)
        source += QStringLiteral("Абзац номер %1, чтобы заметка была длинной.\n\n").arg(i);
    for (int i = 0; i < 20; ++i) source += QStringLiteral("- пункт %1\n").arg(i);
    const QString path = writeNote("прокрутка.md", source);

    zametti::NoteEditor editor;
    editor.resize(700, 500);
    editor.show();
    QTest::qWait(30);
    editor.setFocus();
    editor.openFile(path);
    QTest::qWait(20);

    // Ищем пункт по тексту, а не по номеру: пустые строки между абзацами — тоже
    // блоки, и номер зависел бы от того, сколько их в заметке.
    int target = -1;
    for (QTextBlock block = editor.document()->begin(); block.isValid();
         block = block.next())
        if (block.text() == QStringLiteral("пункт 5")) {
            target = block.blockNumber();
            break;
        }
    check(target > 0, "пункт 5 должен найтись");

    QTextCursor cursor = editor.textCursor();
    cursor.setPosition(editor.document()->findBlockByNumber(target).position());
    cursor.movePosition(QTextCursor::EndOfBlock);
    editor.setTextCursor(cursor);
    editor.ensureCursorVisible();
    QTest::qWait(20);
    // Отводим курсор от нижнего края: у края прокрутка растёт законно.
    editor.verticalScrollBar()->setValue(editor.verticalScrollBar()->value() + 150);
    QTest::qWait(20);

    const int before = editor.verticalScrollBar()->value();
    check(before > 0, "заметка должна быть прокручена");

    for (int i = 0; i < 5; ++i) {
        QTest::keyClick(&editor, Qt::Key_Return);
        editor.insertPlainText(QStringLiteral("новый"));
        QTest::qWait(10);
    }
    checkEqual(QString::number(before),
               QString::number(editor.verticalScrollBar()->value()),
               "прокрутка держится, пока курсор виден");
}

// Правка, меняющая высоту собственного блока: выход из списка. Пункт становится
// абзацем и получает другие отступы. Держаться при этом за курсор нельзя — весь
// текст выше уехал бы (замер до починки: 23 px), поэтому вид держится за блок
// НАД правкой.
void checkScrollHoldsWhenBlockChangesHeight() {
    QString source;
    for (int i = 0; i < 30; ++i)
        source += QStringLiteral("Абзац номер %1 для высоты.\n\n").arg(i);
    source += QStringLiteral("Ну и номера:\n\n1. раз\n2. два\n3. три\n\nхвост\n");
    const QString path = writeNote("выход-из-списка.md", source);

    zametti::NoteEditor editor;
    editor.resize(700, 500);
    editor.show();
    QTest::qWait(30);
    editor.setFocus();
    editor.openFile(path);
    QTest::qWait(20);

    int last = -1;
    for (QTextBlock block = editor.document()->begin(); block.isValid();
         block = block.next())
        if (zametti::isListBlock(block)) last = block.blockNumber();
    check(last > 0, "список должен найтись");

    QTextCursor cursor = editor.textCursor();
    cursor.setPosition(editor.document()->findBlockByNumber(last).position());
    cursor.movePosition(QTextCursor::EndOfBlock);
    editor.setTextCursor(cursor);
    editor.ensureCursorVisible();
    editor.verticalScrollBar()->setValue(editor.verticalScrollBar()->value() + 120);
    QTest::qWait(20);

    // Следим за абзацем выше списка: он меняться не должен вовсе.
    const int watched = last - 4;
    auto watchedY = [&editor, watched] {
        const QTextBlock block = editor.document()->findBlockByNumber(watched);
        return int(editor.document()->documentLayout()->blockBoundingRect(block).top()) -
               editor.verticalScrollBar()->value();
    };
    const int before = watchedY();

    // Новый пункт, пустой пункт, выход из списка.
    QTest::keyClick(&editor, Qt::Key_Return);
    editor.insertPlainText(QStringLiteral("четыре"));
    QTest::qWait(10);
    checkEqual(QString::number(before), QString::number(watchedY()),
               "текст выше стоит при добавлении пункта");

    QTest::keyClick(&editor, Qt::Key_Return);
    QTest::qWait(10);
    QTest::keyClick(&editor, Qt::Key_Return);
    QTest::qWait(10);
    checkEqual(QString::number(before), QString::number(watchedY()),
               "и при выходе из списка тоже");
}

// Ссылка не должна расти от набора за её правым краем. Qt берёт оформление знака
// перед курсором, а у ссылки оно с адресом — пробел и запятая после ссылки
// уезжали внутрь неё, и в файл шло "[текст ,](адрес)".
void checkLinkDoesNotGrow() {
    const QString path =
        writeNote("ссылка.md", QStringLiteral("вот [ссылка](https://example.com)\n"));

    zametti::NoteEditor editor;
    editor.resize(700, 500);
    editor.show();
    QTest::qWait(20);
    editor.setFocus();
    editor.openFile(path);
    QTest::qWait(20);

    auto text = [&editor] {
        return QString::fromStdString(
            zametti::serialize(zametti::readDocument(*editor.document())));
    };

    QTextCursor cursor = editor.textCursor();
    cursor.movePosition(QTextCursor::End);
    editor.setTextCursor(cursor);
    QTest::keyClick(&editor, Qt::Key_Space);
    editor.insertPlainText(QStringLiteral("хвост"));
    QTest::qWait(10);
    checkEqual(QStringLiteral("вот [ссылка](https://example.com) хвост\n"), text(),
               "набранное за ссылкой в неё не уезжает");

    // Запятая — тот же случай, и на ней это заметили.
    QTest::keyClick(&editor, Qt::Key_Comma);
    QTest::qWait(10);
    check(!text().contains(QStringLiteral(",](")), "запятая тоже остаётся снаружи");

    // А набор ВНУТРИ ссылки её по-прежнему продолжает: там это и нужно.
    const QString inside =
        writeNote("внутри-ссылки.md", QStringLiteral("[ссылка](https://example.com)\n"));
    editor.openFile(inside);
    QTest::qWait(20);
    QTextCursor middle = editor.textCursor();
    middle.setPosition(editor.document()->firstBlock().position() + 3);
    editor.setTextCursor(middle);
    editor.insertPlainText(QStringLiteral("XX"));
    QTest::qWait(10);
    checkEqual(QStringLiteral("[ссыXXлка](https://example.com)\n"), text(),
               "внутри ссылки набор её продолжает");
}

// Щелчок по рамке при выделении: переключает всё выделенное разом и выделение
// сохраняет — ровно как Ctrl+Space. А двойной щелчок не должен выделять строку:
// человек метил в чекбокс, а не в слово под ним.
void checkCheckboxClickWithSelection() {
    const QString path = writeNote(
        "щелчок-выделение.md", QStringLiteral("- [ ] раз\n- [ ] два\n- [ ] три\n"));

    zametti::NoteEditor editor;
    editor.resize(700, 500);
    editor.show();
    QTest::qWait(30);
    editor.setFocus();
    editor.openFile(path);
    QTest::qWait(20);

    auto text = [&editor] {
        return QString::fromStdString(
            zametti::serialize(zametti::readDocument(*editor.document())));
    };
    auto clickBox = [&editor](int number, bool twice) {
        const QRectF box = zametti::checkboxRect(
            editor.document()->findBlockByNumber(number), editor.baseFont());
        const QPoint at(int(box.center().x()) - editor.horizontalScrollBar()->value(),
                        int(box.center().y()) - editor.verticalScrollBar()->value());
        QTest::mouseClick(editor.viewport(), Qt::LeftButton, Qt::NoModifier, at);
        if (twice)
            QTest::mouseDClick(editor.viewport(), Qt::LeftButton, Qt::NoModifier, at);
        QTest::qWait(10);
    };

    QTextCursor all = editor.textCursor();
    all.movePosition(QTextCursor::Start);
    all.movePosition(QTextCursor::End, QTextCursor::KeepAnchor);
    editor.setTextCursor(all);
    const int selected = all.selectionEnd() - all.selectionStart();

    clickBox(1, false);
    checkEqual(QStringLiteral("- [x] раз\n- [x] два\n- [x] три\n"), text(),
               "щелчок при выделении отмечает все задачи разом");
    checkEqual(QString::number(selected),
               QString::number(editor.textCursor().selectionEnd() -
                               editor.textCursor().selectionStart()),
               "выделение при этом остаётся");

    clickBox(1, false);
    checkEqual(QStringLiteral("- [ ] раз\n- [ ] два\n- [ ] три\n"), text(),
               "второй щелчок снимает отметки со всех");

    // Двойной щелчок по рамке: одно переключение и никакого выделения.
    const QString single = writeNote("двойной-щелчок.md", QStringLiteral("- [ ] дело\n"));
    editor.openFile(single);
    QTest::qWait(20);
    clickBox(0, true);
    checkEqual(QStringLiteral("- [x] дело\n"), text(),
               "двойной щелчок по рамке переключает задачу один раз");
    check(!editor.textCursor().hasSelection(),
          "двойной щелчок по рамке строку не выделяет");
}

// Отмена ставит курсор туда, где была отменяемая правка. Раньше он вставал
// туда, где стоял в возвращаемом состоянии, — а у только что открытого файла в
// первом шаге записан ноль, и первая же отмена швыряла курсор в начало заметки.
void checkUndoKeepsCursor() {
    const QString path = writeNote(
        "курсор-отмены.md",
        QStringLiteral("вступление\n\n- раз\n  - вложенный\n  - ещё вложенный\n- два\n"));

    zametti::NoteEditor editor;
    editor.resize(700, 500);
    editor.show();
    QTest::qWait(30);
    editor.setFocus();
    editor.openFile(path);
    QTest::qWait(20);

    QTextCursor cursor = editor.textCursor();
    cursor.setPosition(editor.document()->findBlockByNumber(2).position());
    const QTextBlock last = editor.document()->findBlockByNumber(3);
    cursor.setPosition(last.position() + last.length() - 1, QTextCursor::KeepAnchor);
    editor.setTextCursor(cursor);
    const int before = editor.textCursor().position();
    check(before > 0, "курсор должен стоять не в начале");

    QTest::keyClick(&editor, Qt::Key_3, Qt::ControlModifier);
    QTest::qWait(10);
    QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
    QTest::qWait(10);

    checkEqual(QStringLiteral("вступление\n\n- раз\n  - вложенный\n  - ещё вложенный\n- два\n"),
               QString::fromStdString(
                   zametti::serialize(zametti::readDocument(*editor.document()))),
               "отмена вернула прежний вид списка");
    checkEqual(QString::number(before), QString::number(editor.textCursor().position()),
               "курсор после отмены остался у правки");
}

// Вид не должен уезжать ни от одной операции. Правка меняет строку, а не окно:
// прокрутка обязана остаться на месте, а курсор — сдвинуться не больше чем на
// строку.
//
// Обмен пунктов местами этого не соблюдал: правки над IR ставят курсор уже
// после пересборки, и показ курсора внутри пересборки уводил вид к началу
// документа, а потом обратно вниз — переставленный пункт оказывался у самой
// нижней кромки окна.
void checkViewHoldsForEveryOperation() {
    struct Probe {
        const char* name;
        Qt::Key key;
        Qt::KeyboardModifiers mods;
    };
    const Probe probes[] = {
        {"Enter", Qt::Key_Return, Qt::NoModifier},
        {"Tab", Qt::Key_Tab, Qt::NoModifier},
        {"Ctrl+Space", Qt::Key_Space, Qt::ControlModifier},
        {"в нумерованный", Qt::Key_3, Qt::ControlModifier},
        {"в абзац", Qt::Key_0, Qt::ControlModifier | Qt::ShiftModifier},
        {"жирный", Qt::Key_B, Qt::ControlModifier},
        {"блок кода", Qt::Key_E, Qt::ControlModifier | Qt::ShiftModifier},
        {"Ctrl+Down", Qt::Key_Down, Qt::ControlModifier},
        {"Ctrl+Up", Qt::Key_Up, Qt::ControlModifier},
    };

    QString source;
    for (int i = 0; i < 30; ++i)
        source += QStringLiteral("Абзац %1 для высоты окна.\n\n").arg(i);
    for (int i = 0; i < 12; ++i) source += QStringLiteral("- [ ] пункт %1\n").arg(i);

    for (const Probe& probe : probes) {
        const QString path = writeNote(
            (std::string("вид-") + probe.name + ".md").c_str(), source);

        zametti::NoteEditor editor;
        editor.resize(700, 500);
        editor.show();
        QTest::qWait(20);
        editor.setFocus();
        editor.openFile(path);
        QTest::qWait(20);

        int first = -1;
        for (QTextBlock block = editor.document()->begin(); block.isValid();
             block = block.next())
            if (zametti::isListBlock(block)) {
                first = block.blockNumber();
                break;
            }
        check(first > 0, "список должен найтись");

        QTextCursor cursor = editor.textCursor();
        cursor.setPosition(
            editor.document()->findBlockByNumber(first + 4).position() + 3);
        editor.setTextCursor(cursor);
        editor.ensureCursorVisible();
        editor.verticalScrollBar()->setValue(editor.verticalScrollBar()->value() + 100);
        QTest::qWait(10);

        const int scroll = editor.verticalScrollBar()->value();
        const int y = editor.cursorRect().top();
        QTest::keyClick(&editor, probe.key, probe.mods);
        QTest::qWait(10);

        checkEqual(QString::number(scroll),
                   QString::number(editor.verticalScrollBar()->value()),
                   std::string("вид не уехал: ") + probe.name);
        // Курсор вправе сдвинуться на свою строку — но не на пол-окна.
        check(qAbs(editor.cursorRect().top() - y) <= 40,
              std::string("курсор сдвинулся не больше чем на строку: ") + probe.name);
    }
}

// Шаг вниз между блоками с разными полями. Маркер списка отодвигает текст
// пункта вправо, а движение по вертикали держит экранный X — из начала пункта
// курсор попадал на пару знаков внутрь следующего абзаца, и выделение
// прихватывало лишнее.
void checkColumnAcrossMargins() {
    const QString path = writeNote("колонка.md",
                                   QStringLiteral("- пункт списка\n\nОбычный абзац.\n"));

    zametti::NoteEditor editor;
    editor.resize(700, 400);
    editor.show();
    QTest::qWait(20);
    editor.setFocus();
    editor.openFile(path);
    QTest::qWait(20);

    check(editor.document()->firstBlock().blockFormat().leftMargin() >
              editor.document()->findBlockByNumber(1).blockFormat().leftMargin(),
          "у пункта поле шире, чем у абзаца");

    QTextCursor cursor = editor.textCursor();
    cursor.setPosition(editor.document()->firstBlock().position());
    editor.setTextCursor(cursor);
    QTest::keyClick(&editor, Qt::Key_Down);
    QTest::qWait(10);

    checkEqual(QStringLiteral("1"), QString::number(editor.textCursor().blockNumber()),
               "шаг вниз привёл в следующий блок");
    checkEqual(QStringLiteral("0"),
               QString::number(editor.textCursor().positionInBlock()),
               "и в ту же колонку, а не внутрь текста");

    // То же с выделением: лишних знаков захватываться не должно.
    cursor.setPosition(editor.document()->firstBlock().position());
    editor.setTextCursor(cursor);
    QTest::keyClick(&editor, Qt::Key_Down, Qt::ShiftModifier);
    QTest::qWait(10);
    checkEqual(QStringLiteral("пункт списка"),
               editor.textCursor().selectedText().replace(QChar::ParagraphSeparator,
                                                          QString()),
               "выделено ровно содержимое пункта");
}

// Выделение нескольких строк рисуется сплошным блоком. Высота строки назначена,
// а Qt красит выделение по естественной — между полосами оставался
// незакрашенный ряд, и на укороченной строке он читался сколом на углу.
void checkSelectionHasNoGaps() {
    QString source;
    for (int i = 0; i < 4; ++i)
        source += QStringLiteral("- пункт %1, достаточно длинный, чтобы занять ширину\n")
                      .arg(i);
    const QString path = writeNote("сплошное-выделение.md", source);

    zametti::NoteEditor editor;
    editor.resize(760, 260);
    editor.show();
    QTest::qWait(20);
    editor.setFocus();
    editor.openFile(path);
    QTest::qWait(20);

    QTextCursor cursor = editor.textCursor();
    cursor.setPosition(editor.document()->firstBlock().position());
    const QTextBlock last = editor.document()->findBlockByNumber(3);
    cursor.setPosition(last.position() + last.length() - 1, QTextCursor::KeepAnchor);
    editor.setTextCursor(cursor);
    QTest::qWait(20);

    QImage shot(editor.size(), QImage::Format_ARGB32);
    editor.render(&shot);

    const QColor highlight = editor.palette().color(QPalette::Highlight);
    auto isHighlight = [&highlight](QRgb pixel) {
        return qAbs(qRed(pixel) - highlight.red()) < 40 &&
               qAbs(qGreen(pixel) - highlight.green()) < 40 &&
               qAbs(qBlue(pixel) - highlight.blue()) < 40;
    };

    int firstRow = -1;
    int lastRow = -1;
    for (int y = 0; y < shot.height(); ++y) {
        bool any = false;
        for (int x = 0; x < shot.width() && !any; ++x) any = isHighlight(shot.pixel(x, y));
        if (!any) continue;
        if (firstRow < 0) firstRow = y;
        lastRow = y;
    }
    check(firstRow >= 0 && lastRow - firstRow > 40,
          "выделение должно занимать несколько строк");

    int empty = 0;
    for (int y = firstRow; y <= lastRow; ++y) {
        bool any = false;
        for (int x = 0; x < shot.width() && !any; ++x) any = isHighlight(shot.pixel(x, y));
        if (!any) ++empty;
    }
    checkEqual(QStringLiteral("0"), QString::number(empty),
               "внутри выделения не должно быть незакрашенных рядов");
}

// Пустая заметка. Каретка в ней должна быть видна и стоять там же, где встал бы
// текст: у левого поля и в высоту строки. До правки блок оставался вовсе без
// формата, каретка выходила кеглем по умолчанию в самом углу окна, и человек её
// попросту не находил.
void checkEmptyNoteCaret() {
    const QString path = writeNote("пустая.md", QString());

    // Окно нарочно широкое: при такой ширине колонка центрируется, и поля
    // документа меняются уже после сборки. Пустой документ от смены полей не
    // переразмечался — каретка оставалась у прежнего поля и кеглем по
    // умолчанию, то есть далеко от текста и вдвое ниже. На узком окне ошибки не
    // видно вовсе: там колонку не двигают.
    zametti::NoteEditor editor;
    editor.resize(1600, 400);
    editor.show();
    QTest::qWait(20);
    editor.setFocus();
    editor.openFile(path);
    QTest::qWait(20);

    const QRect caret = editor.cursorRect();

    // И тот же случай на пути запуска: заметку открывают ещё до show(), в узком
    // окне, а колонку двигает уже первое изменение размера — пересборки при
    // этом нет вовсе, и подсказка о разметке нужна именно там, где меняются
    // поля.
    {
        zametti::NoteEditor started;
        started.openFile(path);
        started.resize(1600, 400);
        started.show();
        QTest::qWait(20);
        started.setFocus();
        QTest::qWait(20);
        check(started.cursorRect().x() > 100,
              "при запуске каретка пустой заметки стоит в колонке текста");
        check(started.cursorRect().height() > 15,
              "и высотой в строку, а не кеглем по умолчанию");
    }
    // Левое поле страницы — там же, где начинается текст обычного абзаца.
    const QString filled = writeNote("не-пустая.md", QStringLiteral("текст\n"));
    editor.openFile(filled);
    QTest::qWait(20);
    const QRect withText = editor.cursorRect();
    editor.openFile(path);
    QTest::qWait(20);

    checkEqual(QString::number(withText.x()), QString::number(editor.cursorRect().x()),
               "каретка пустой заметки стоит у того же поля, что и текст");
    check(caret.height() > 1 && qAbs(caret.height() - withText.height()) <= 1,
          "и той же высоты, что строка текста");
}

// Каретка нарисована нами, а не Qt: цвет своей Qt не отдаёт ни одним способом.
// Раз рисуем сами — проверяем и то, что вокруг: след на прежнем месте, выделение
// и потерю фокуса. Ровно этого от кастомной каретки и опасаются.
void checkCaretPainting() {
    const zametti::Appearance saved = zametti::appearance();
    struct Restore {
        const zametti::Appearance& from;
        ~Restore() { zametti::appearance() = from; }
    } restore{saved};
    zametti::appearance().caretWidth = 4.0;
    zametti::appearance().caretColor = QColor(220, 30, 30);

    const QString path = writeNote("каретка-цвет.md", QStringLiteral("первая строка\n"));
    QWidget host;
    zametti::NoteEditor editor(&host);
    QLineEdit other(&host);
    editor.setGeometry(0, 0, 600, 200);
    other.setGeometry(0, 210, 600, 30);
    host.resize(600, 260);
    host.show();
    QTest::qWait(20);
    editor.setFocus();
    editor.openFile(path);
    QTest::qWait(20);

    // Каретка горит сразу после движения курсора, поэтому снимок берём тут же:
    // иначе кадр попал бы в погасшую фазу.
    // Ищем ровно цвет каретки, а не «что-нибудь красноватое»: подпалённые края
    // букв на выделении дают оранжевый, и на нём проверка ложно срабатывала.
    auto isCaret = [](QColor c) {
        return qAbs(c.red() - 220) < 12 && qAbs(c.green() - 30) < 12 && qAbs(c.blue() - 30) < 12;
    };
    auto ink = [&editor, isCaret](QPoint at) {
        const QImage shot = editor.viewport()->grab().toImage();
        int found = 0;
        for (int y = at.y() + 2; y <= at.y() + 10; ++y)
            for (int x = at.x(); x < at.x() + 6; ++x)
                if (shot.rect().contains(x, y) && isCaret(shot.pixelColor(x, y))) ++found;
        return found;
    };

    QTextCursor cursor = editor.textCursor();
    cursor.movePosition(QTextCursor::EndOfBlock);
    editor.setTextCursor(cursor);
    QTest::qWait(10);
    const QPoint away = editor.cursorRect().topLeft();
    check(ink(away) > 0, "каретка нарисована заданным цветом");

    cursor.setPosition(0);
    editor.setTextCursor(cursor);
    QTest::qWait(10);
    check(ink(away) == 0, "на прежнем месте следа не остаётся");
    check(ink(editor.cursorRect().topLeft()) > 0, "и она на новом месте");

    cursor.setPosition(5, QTextCursor::KeepAnchor);
    editor.setTextCursor(cursor);
    QTest::qWait(10);
    check(ink(editor.cursorRect().topLeft()) == 0, "при выделении каретка не рисуется");

    cursor.clearSelection();
    editor.setTextCursor(cursor);
    QTest::qWait(10);
    other.setFocus();
    QTest::qWait(30);
    check(ink(editor.cursorRect().topLeft()) == 0, "без фокуса каретки нет");
    editor.setFocus();
    QTest::qWait(20);
    check(ink(editor.cursorRect().topLeft()) > 0, "с возвратом фокуса — снова есть");
}

// Ширина каретки — настройка, и меряется она по нарисованному: своей каретки у
// Qt больше нет, её ширина всегда ноль.
void checkCaretWidth() {
    const zametti::Appearance saved = zametti::appearance();
    struct Restore {
        const zametti::Appearance& from;
        ~Restore() { zametti::appearance() = from; }
    } restore{saved};
    zametti::appearance().caretColor = QColor(220, 30, 30);

    const QString path = writeNote("каретка.md", QStringLiteral("текст\n"));

    // Сколько подряд закрашенных пикселей от левого края каретки.
    // Снимок берём у вьюпорта: cursorRect отдаёт именно его координаты, а у
    // виджета есть рамка, и по ней всё съезжает на пиксель.
    auto painted = [](zametti::NoteEditor& editor) {
        const QImage shot = editor.viewport()->grab().toImage();
        const QRect at = editor.cursorRect();
        int run = 0;
        for (int x = at.left(); x < at.left() + 20; ++x) {
            const QPoint p(x, at.top() + 5);
            if (!shot.rect().contains(p) || qAbs(shot.pixelColor(p).red() - 220) >= 12 ||
                qAbs(shot.pixelColor(p).green() - 30) >= 12)
                break;
            ++run;
        }
        return run;
    };

    for (const auto& [width, zoom, expected] :
         {std::tuple<qreal, qreal, int>{2.0, 1.0, 2}, {5.0, 1.0, 5}, {2.0, 2.0, 4}}) {
        zametti::appearance().caretWidth = width;
        zametti::NoteEditor editor;
        editor.resize(700, 300);
        editor.show();
        QTest::qWait(20);
        editor.setFocus();
        editor.setZoom(zoom);
        editor.openFile(path);
        QTest::qWait(20);
        QTextCursor cursor = editor.textCursor();
        cursor.movePosition(QTextCursor::EndOfBlock);
        editor.setTextCursor(cursor);
        QTest::qWait(10);
        checkEqual(QString::number(expected), QString::number(painted(editor)),
                   "ширина каретки: настройка " + std::to_string(int(width)) + ", масштаб " +
                       std::to_string(int(zoom)));
    }

    // Ноль в настройке каретку не прячет: меньше пикселя не бывает.
    zametti::appearance().caretWidth = 0.0;
    zametti::NoteEditor thin;
    thin.resize(700, 300);
    thin.show();
    QTest::qWait(20);
    thin.setFocus();
    thin.openFile(path);
    QTest::qWait(20);
    QTextCursor cursor = thin.textCursor();
    cursor.movePosition(QTextCursor::EndOfBlock);
    thin.setTextCursor(cursor);
    QTest::qWait(10);
    check(painted(thin) >= 1, "нулевая настройка не прячет каретку");
}

// Отмена правки, сделанной далеко за краем окна. Как футбольный арбитр: пока
// действие в пределах видимости — стоит на месте и картинку не дёргает; ушло за
// край — бежит в центр событий, а не к ближайшей кромке.
//
// Обычный ensureCursorVisible прокручивает ровно на минимум, и отменённая правка
// оказывалась впритык к нижнему или верхнему краю экрана.
void checkUndoShowsEditPlace() {
    // Заметка нарочно длинная, а правка в первой её трети: прокрутившись до
    // конца, мы уходим от места правки дальше, чем на окно, и после отмены оно
    // остаётся за краем. На короткой заметке отмена возвращает место обратно в
    // окно сама, и проверять было бы нечего.
    QString source;
    for (int i = 0; i < 600; ++i)
        source += QStringLiteral("Абзац номер %1, чтобы заметка была длинной.\n\n").arg(i);
    const QString path = writeNote("отмена-вид.md", source);

    auto edit = [&path](bool scrollAway, int* percent, int* moved) {
        zametti::NoteEditor editor;
        editor.resize(900, 600);
        editor.show();
        QTest::qWait(20);
        editor.setFocus();
        editor.openFile(path);
        QTest::qWait(30);

        // Правка посередине заметки: только там и есть куда центрировать.
        QTextCursor cursor = editor.textCursor();
        cursor.setPosition(editor.document()->findBlockByNumber(200).position());
        cursor.movePosition(QTextCursor::NextBlock, QTextCursor::KeepAnchor, 3);
        editor.setTextCursor(cursor);
        QTest::keyClick(&editor, Qt::Key_Delete);
        QTest::qWait(30);

        // До самого низа: место правки посреди заметки заведомо уходит за край.
        //
        // Ждём, пока раскладка досчитает высоту: сразу после открытия максимум
        // прокрутки ещё мал (замер: 4402 против настоящих 28830), и «в конец»
        // уводит недалеко — проверка мерила бы не то, что думает. Признак
        // готовности — максимум перестал расти.
        // Ctrl+End: так уходят в конец руками. Заодно досчитывается раскладка —
        // сразу после открытия высота документа ещё не известна, и прокрутка «в
        // конец» уводила бы недалеко (замер: максимум 4402 против настоящих
        // 28830). Курсор при этом уезжает вместе с видом, но отмене он и не
        // нужен: место правки она берёт из истории.
        if (scrollAway) QTest::keyClick(&editor, Qt::Key_End, Qt::ControlModifier);
        QTest::qWait(30);
        const int before = editor.verticalScrollBar()->value();


        QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
        QTest::qWait(50);
        *percent = editor.cursorRect().center().y() * 100 / editor.viewport()->height();
        *moved = editor.verticalScrollBar()->value() - before;
    };

    int percent = 0;
    int moved = 0;
    edit(true, &percent, &moved);
    check(percent > 35 && percent < 65,
          "отменённая правка из-за края окна показывается по центру, а не у кромки (вышло " +
              std::to_string(percent) + "%)");

    edit(false, &percent, &moved);
    checkEqual(QStringLiteral("0"), QString::number(moved),
               "а правку, которая и так на виду, вид не дёргает");
}

// Курсор не должен упираться в кромку окна. Qt прокручивает ровно до касания
// края, и поле страницы при этом уезжает за кромку: строка, которую набираешь,
// оказывается вплотную к рамке окна.
void checkCaretKeepsOffEdge() {
    const zametti::Appearance saved = zametti::appearance();
    struct Restore {
        const zametti::Appearance& from;
        ~Restore() { zametti::appearance() = from; }
    } restore{saved};
    zametti::appearance().verticalMargin = 1.5;

    QString source;
    for (int i = 0; i < 200; ++i)
        source += QStringLiteral("Абзац номер %1, чтобы заметка была длинной.\n\n").arg(i);
    const QString path = writeNote("зазор.md", source);

    zametti::NoteEditor editor;
    editor.resize(900, 500);
    editor.show();
    QTest::qWait(20);
    editor.setFocus();
    editor.openFile(path);
    QTest::qWait(30);

    const int gap = qRound(1.5 * QFontMetricsF(editor.baseFont()).height());
    auto near = [](int a, int b) { return qAbs(a - b) <= 2; };

    // Вниз до конца документа: на самом конце зазор держится полем страницы.
    for (int i = 0; i < 400; ++i) QTest::keyClick(&editor, Qt::Key_Down);
    QTest::qWait(30);
    const int below = editor.viewport()->height() - editor.cursorRect().bottom();
    check(near(below, gap), "внизу под кареткой остаётся зазор (вышло " +
                                std::to_string(below) + " при " + std::to_string(gap) + ")");

    for (int i = 0; i < 400; ++i) QTest::keyClick(&editor, Qt::Key_Up);
    QTest::qWait(30);
    const int above = editor.cursorRect().top();
    check(near(above, gap), "и наверху над ней тоже (вышло " + std::to_string(above) +
                                " при " + std::to_string(gap) + ")");

    // И посреди заметки, где упереться не во что.
    QTextCursor cursor = editor.textCursor();
    cursor.setPosition(editor.document()->findBlockByNumber(100).position());
    editor.setTextCursor(cursor);
    QTest::qWait(20);
    for (int i = 0; i < 60; ++i) QTest::keyClick(&editor, Qt::Key_Down);
    QTest::qWait(30);
    check(near(editor.viewport()->height() - editor.cursorRect().bottom(), gap),
          "зазор снизу держится и посреди заметки");
    for (int i = 0; i < 60; ++i) QTest::keyClick(&editor, Qt::Key_Up);
    QTest::qWait(30);
    check(near(editor.cursorRect().top(), gap), "и сверху посреди заметки");
}

// Пустые строки — содержимое заметки, а не мусор: ими отбивают куски текста.
// Набрали, сохранили, открыли заново — они на месте, и ровно в том же числе.
// В файл они уходят настоящими пустыми строками, без единого хитрого знака.
void checkBlankLinesSurviveSaving() {
    const QString path = writeNote("пустые-строки.md",
                                   QStringLiteral("- [ ] дело\n\nдо 19 июля:\n"));

    zametti::NoteEditor editor;
    editor.resize(700, 500);
    editor.show();
    QTest::qWait(20);
    editor.setFocus();
    editor.openFile(path);
    QTest::qWait(20);

    int target = -1;
    for (QTextBlock block = editor.document()->begin(); block.isValid();
         block = block.next())
        if (block.text().startsWith(QStringLiteral("до 19"))) {
            target = block.blockNumber();
            break;
        }
    check(target > 0, "строка \"до 19\" должна найтись");

    QTextCursor cursor = editor.textCursor();
    cursor.setPosition(editor.document()->findBlockByNumber(target).position());
    editor.setTextCursor(cursor);
    for (int i = 0; i < 10; ++i) {
        QTest::keyClick(&editor, Qt::Key_Return);
        QTest::qWait(5);
    }
    // Считаем пустые СТРОКИ, а не блоки: десять Enter дают десять пустых строк
    // внутри одного абзаца, а не десять абзацев. Разрезает абзац только Enter на
    // пустой строке.
    auto blankLines = [&editor] {
        int count = 0;
        for (QTextBlock block = editor.document()->begin(); block.isValid();
             block = block.next()) {
            const QStringList lines =
                block.text().split(QChar::LineSeparator);
            for (const QString& line : lines)
                if (line.trimmed().isEmpty()) ++count;
        }
        return count;
    };
    // Одиннадцать, а не десять: одна пустая строка была в файле с самого начала,
    // и теперь она видна — это отдельный блок, а не невидимая отбивка.
    const int before = blankLines();
    checkEqual(QStringLiteral("11"), QString::number(before),
               "десять Enter дают десять пустых строк сверх бывшей в файле");

    editor.save(false);
    QTest::qWait(20);
    editor.openFile(writeNote("другая.md", QStringLiteral("другая\n")));
    QTest::qWait(20);
    editor.openFile(path);
    QTest::qWait(20);

    checkEqual(QString::number(before), QString::number(blankLines()),
               "пустые строки пережили запись и перечитывание");

    // А хвост пустых строк в конце заметки сохраняться не должен: он набирается
    // случайно и ничего не отбивает.
    QTextCursor tail = editor.textCursor();
    tail.movePosition(QTextCursor::End);
    editor.setTextCursor(tail);
    const int blocks = blankLines();
    for (int i = 0; i < 6; ++i) {
        QTest::keyClick(&editor, Qt::Key_Return);
        QTest::qWait(5);
    }
    editor.save(false);
    QTest::qWait(20);
    editor.openFile(writeNote("третья.md", QStringLiteral("третья\n")));
    QTest::qWait(20);
    editor.openFile(path);
    QTest::qWait(20);
    checkEqual(QString::number(blocks), QString::number(blankLines()),
               "хвост пустых строк в конце не сохраняется");
}

// Разделитель — прогон пустых строк, поставленный руками. Каждая пустая строка
// файла — свой блок, поэтому высота разделителя предсказуема сама собой: поле
// сверху, n высот строки, поле снизу. Курсор идёт по его строкам ровным шагом.
void checkSeparatorGeometry() {
    const zametti::Appearance saved = zametti::appearance();
    zametti::appearance().separatorSpacingBefore = 0.5;
    zametti::appearance().separatorSpacingAfter = 0.5;
    struct Restore {
        const zametti::Appearance& from;
        ~Restore() { zametti::appearance() = from; }
    } restore{saved};

    const QString path =
        writeNote("разделитель.md", QStringLiteral("- пункт\n\n\n\nабзац\n"));

    zametti::NoteEditor editor;
    editor.resize(700, 500);
    editor.show();
    QTest::qWait(20);
    editor.setFocus();
    editor.openFile(path);
    QTest::qWait(20);

    // Три пустые строки — три блока, а не один блок в три строки.
    checkEqual(QStringLiteral("5"), QString::number(editor.document()->blockCount()),
               "каждая пустая строка — свой блок");
    for (int i = 1; i <= 3; ++i)
        check(zametti::isVSpaceBlock(editor.document()->findBlockByNumber(i)),
              "блок " + std::to_string(i) + " — пустая строка");

    // Поля обрамляют прогон целиком: сверху перед первой пустой строкой, снизу
    // после последней. Внутри прогона полей нет — иначе высота трёх строк
    // перестала бы быть тремя высотами строки.
    auto marginOf = [&editor](int number) {
        return editor.document()->findBlockByNumber(number).blockFormat().topMargin();
    };
    const qreal line = editor.document()->findBlockByNumber(1).blockFormat().lineHeight();
    const qreal above = marginOf(1);
    const qreal below = marginOf(4);
    check(above > 0.0 && qAbs(above - below) < 0.01,
          "поля над и под разделителем равны между собой");
    check(qAbs(above / line - 0.5) < 0.1, "и составляют половину высоты строки");
    check(marginOf(2) <= 0.01 && marginOf(3) <= 0.01, "внутри разделителя полей нет");

    // Шаги курсора по пустым строкам одинаковы и равны высоте строки.
    QTextCursor cursor = editor.textCursor();
    cursor.setPosition(editor.document()->findBlockByNumber(1).position());
    editor.setTextCursor(cursor);
    QTest::qWait(10);

    auto lineTop = [&editor] {
        const QTextBlock block = editor.textCursor().block();
        const QRectF rect =
            editor.document()->documentLayout()->blockBoundingRect(block);
        const QTextLine at =
            block.layout()->lineForTextPosition(editor.textCursor().positionInBlock());
        return rect.top() + (at.isValid() ? at.y() : 0.0);
    };

    const qreal first = lineTop();
    QTest::keyClick(&editor, Qt::Key_Down);
    QTest::qWait(10);
    const qreal second = lineTop();
    QTest::keyClick(&editor, Qt::Key_Down);
    QTest::qWait(10);
    const qreal third = lineTop();

    check(qAbs((second - first) - (third - second)) < 0.01,
          "шаги по строкам разделителя одинаковы");
    check(qAbs((second - first) - line) < 0.01,
          "и равны высоте строки");
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
    checkEmptyNoteCaret();
    checkCaretPainting();
    checkCaretWidth();
    checkUndoShowsEditPlace();
    checkCaretKeepsOffEdge();
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
    checkCodeTyping();
    checkCheckboxClick();
    checkScrollHolds();
    checkScrollHoldsWhenBlockChangesHeight();
    checkLinkDoesNotGrow();
    checkCheckboxClickWithSelection();
    checkUndoKeepsCursor();
    checkViewHoldsForEveryOperation();
    checkColumnAcrossMargins();
    checkSelectionHasNoGaps();
    checkBlankLinesSurviveSaving();
    checkSeparatorGeometry();

    std::printf("проверок %d, провалов %d\n", zt::g_checks, zt::g_failures);
    return zt::g_failures == 0 ? 0 : 1;
}
