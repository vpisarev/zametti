// Правка блоков кода: выход из блока и табуляция.
//
// Главное здесь — МАТРИЦА «операция × край блока». Написана она до починок, и
// это не формальность: на картинках этапа 9 матрица нашла шесть бед там, где
// глазами виделось три, и все три лишние жили именно по краям — в первой
// строке, в последней, до блока и после него.
//
// Каждая клетка матрицы проверяется дважды: что вышло на экране (текст блоков
// и место каретки) и что после этой правки документ остался ЗАКОННЫМ — то
// есть инварианты дословных кусков, пустых строк и списков держатся, а файл
// читается обратно в то же самое.
//
// Клавиши нажимаются по-настоящему (QTest::keyClick), а не зовутся функциями:
// между «операция работает» и «клавиша работает» лежит весь разбор нажатия, и
// ломалось у нас до сих пор именно там.

#include "doc_model.h"
#include "pieces.h"
#include "editor_ops.h"
#include "editor_widget.h"
#include "lang_editor.h"
#include "settings.h"
#include "test_util.h"

#include <vector>
#include "testdata.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QTest>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextLayout>

#include <cmath>
#include <string>

namespace {

QString g_dir;

std::string num(double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.1f", v);
    return buf;
}

void check(bool ok, const std::string& what) {
    ++zt::g_checks;
    if (ok) return;
    ++zt::g_failures;
    std::printf("провал: %s\n", what.c_str());
}

void checkEq(const std::string& expected, const std::string& got, const std::string& what) {
    ++zt::g_checks;
    if (expected == got) return;
    ++zt::g_failures;
    std::printf("провал: %s\n%s", what.c_str(), zt::diff(expected, got).c_str());
}

QString writeNote(const QString& name, const QString& text) {
    const QString path = QDir(g_dir).filePath(name);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return {};
    f.write(text.toUtf8());
    f.close();
    return path;
}

// Заметка матрицы: абзац, блок кода из двух строк, абзац. Всё по-латински —
// QTest::keyClicks роняет ассерт на кириллице (qasciikey.cpp).
const char* kNote = R"(before

```py
one
two
```

after
)";

std::string editorMarkdown(const zametti::NoteEditor& editor) {
    return markdownOf(blocksOf(*editor.document()));
}

// Документ законен: три инварианта плюс чтение файла обратно в то же самое.
void checkStillLegal(zametti::NoteEditor& editor, const std::string& where) {
    QString problem;
    check(zametti::literalInvariantHolds(*editor.document(), &problem),
          where + ": инвариант дословных кусков (" + problem.toStdString() + ")");
    check(zametti::gapInvariantHolds(*editor.document(), &problem),
          where + ": инвариант пустых строк (" + problem.toStdString() + ")");
    check(zametti::listInvariantHolds(*editor.document(), &problem),
          where + ": инвариант списков (" + problem.toStdString() + ")");

    // Круг: то, что мы записали бы в файл, читается обратно в тот же markdown.
    const std::string text = editorMarkdown(editor);
    checkEq(text, noteOf(text).toMarkdown(), where + ": файл читается в себя");
}

class Editor : public zametti::NoteEditor {
public:
    void openText(const QString& name, const char* text) {
        const QString path = writeNote(name, QString::fromUtf8(text));
        resize(900, 700);
        show();
        QTest::qWait(10);
        openFile(path);
        QTest::qWait(20);
    }

    // Каретка в строку с этим текстом; column — сколько знаков от начала строки.
    void caretTo(const QString& lineText, int column) {
        for (QTextBlock b = document()->firstBlock(); b.isValid(); b = b.next()) {
            if (b.text() != lineText) continue;
            QTextCursor at(b);
            at.setPosition(b.position() + qMin(column, b.length() - 1));
            setTextCursor(at);
            return;
        }
        check(false, ("нет строки «" + lineText + "»").toStdString());
    }

    QString caretLine() const { return textCursor().block().text(); }
    int caretColumn() const {
        return textCursor().position() - textCursor().block().position();
    }
    zametti::Kind caretKind() const { return zametti::kindOf(textCursor().block()); }
};

// --- матрица «операция × край блока» ----------------------------------------
//
// Строки матрицы — четыре места внутри и вокруг блока кода, столбцы — четыре
// операции. В каждой клетке спрашивается не «как оно сейчас», а два свойства,
// которые обязаны держаться при любом ответе: документ остался законным и
// содержимое блока кода изменилось только там, где операция это обещала.
struct Place {
    const char* name;
    const char* line;
    int column;
};

const Place kPlaces[] = {
    {"до блока", "before", 6},
    {"первая строка, начало", "one", 0},
    {"первая строка, конец", "one", 3},
    {"последняя строка, конец", "two", 3},
    {"после блока", "after", 0},
};

void runMatrix() {
    struct Op {
        const char* name;
        // Qt::Key, а не int: у QTest::keyClick есть перегрузка на char, и int
        // уезжает именно в неё — Key_Return (0x01000004) обрезается до знака
        // с кодом 4, и набор падает ассертом внутри Qt.
        Qt::Key key;
        Qt::KeyboardModifiers mods;
    };
    const Op ops[] = {
        {"Enter", Qt::Key_Return, Qt::NoModifier},
        {"Backspace", Qt::Key_Backspace, Qt::NoModifier},
        {"Delete", Qt::Key_Delete, Qt::NoModifier},
        {"Ctrl+Enter", Qt::Key_Return, Qt::ControlModifier},
        {"Tab", Qt::Key_Tab, Qt::NoModifier},
        {"Shift+Tab", Qt::Key_Backtab, Qt::ShiftModifier},
    };

    int cell = 0;
    for (const Place& place : kPlaces) {
        for (const Op& op : ops) {
            Editor editor;
            editor.openText(QStringLiteral("матрица-%1.md").arg(++cell), kNote);
            editor.caretTo(QString::fromUtf8(place.line), place.column);
            QTest::keyClick(&editor, op.key, op.mods);
            QTest::qWait(5);
            checkStillLegal(editor, std::string(op.name) + " в «" + place.name + "»");
        }
    }
}

// --- Ctrl+Enter: выход из блока ---------------------------------------------
//
// Договор брифа: из ЛЮБОГО места блока кода каретка встаёт в пустую строку
// сразу после него, а содержимое блока не меняется вовсе.
void checkLeaveCodeBlock() {
    const Place inside[] = {
        {"первая строка, начало", "one", 0},
        {"первая строка, конец", "one", 3},
        {"последняя строка, начало", "two", 0},
        {"последняя строка, конец", "two", 3},
    };
    int n = 0;
    for (const Place& place : inside) {
        Editor editor;
        editor.openText(QStringLiteral("выход-%1.md").arg(++n), kNote);
        editor.caretTo(QString::fromUtf8(place.line), place.column);
        QTest::keyClick(&editor, Qt::Key_Return, Qt::ControlModifier);
        QTest::qWait(5);

        check(editor.caretKind() != zametti::Kind::Code,
              std::string("Ctrl+Enter из «") + place.name + "»: каретка вышла из кода");
        check(editor.caretLine().isEmpty(),
              std::string("Ctrl+Enter из «") + place.name + "»: строка пустая");

        // Код не тронут — ни одной буквой.
        const std::string text = editorMarkdown(editor);
        check(text.find("one\ntwo\n```") != std::string::npos,
              std::string("Ctrl+Enter из «") + place.name + "»: код цел");

        // И печатать после этого можно обычным текстом, а не кодом.
        QTest::keyClicks(&editor, QStringLiteral("tail"));
        QTest::qWait(5);
        check(editor.caretKind() == zametti::Kind::Paragraph,
              std::string("Ctrl+Enter из «") + place.name + "»: печатается абзацем");
        checkStillLegal(editor, std::string("Ctrl+Enter из «") + place.name + "» и набор");
    }

    // Блок в конце заметки — та самая ловушка владельца: после него не встать.
    Editor tail;
    tail.openText(QStringLiteral("выход-конец.md"), "head\n\n```py\none\n```\n");
    tail.caretTo(QStringLiteral("one"), 3);
    QTest::keyClick(&tail, Qt::Key_Return, Qt::ControlModifier);
    QTest::qWait(5);
    check(tail.caretKind() != zametti::Kind::Code, "блок в конце заметки: вышли из кода");
    QTest::keyClicks(&tail, QStringLiteral("tail"));
    QTest::qWait(5);
    // Пустая строка, в которую встала каретка, ПРЕВРАЩАЕТСЯ в набранный абзац
    // — отдельной пустой строки после блока не остаётся, и это верно: между
    // забором и абзацем markdown её не требует.
    checkEq("head\n\n```py\none\n```\ntail\n", editorMarkdown(tail),
            "блок в конце: после него встал абзац");

    // Один Ctrl+Z возвращает всё как было — включая пустую строку.
    Editor undo;
    undo.openText(QStringLiteral("выход-отмена.md"), kNote);
    const std::string before = editorMarkdown(undo);
    undo.caretTo(QStringLiteral("one"), 3);
    QTest::keyClick(&undo, Qt::Key_Return, Qt::ControlModifier);
    QTest::qWait(5);
    check(editorMarkdown(undo) != before, "Ctrl+Enter что-то изменил");
    undo.undo();
    QTest::qWait(5);
    checkEq(before, editorMarkdown(undo), "один Ctrl+Z возвращает прежнее");
}

// --- табуляция --------------------------------------------------------------
void checkCodeTabs() {
    const int width = zametti::appearance().codeTabWidth;
    check(width == 4, "ширина стопа по умолчанию — четыре");

    // Из начала строки Tab даёт ровно стоп пробелов, и это ПРОБЕЛЫ, а не знак
    // табуляции: иначе набранное нами и старое из файлов разъезжались бы.
    Editor editor;
    editor.openText(QStringLiteral("таб-начало.md"), kNote);
    editor.caretTo(QStringLiteral("one"), 0);
    QTest::keyClick(&editor, Qt::Key_Tab, Qt::NoModifier);
    QTest::qWait(5);
    checkEq("    one", editor.caretLine().toStdString(), "Tab из начала строки — стоп пробелов");
    check(!editor.caretLine().contains(QLatin1Char('\t')), "знака табуляции не появилось");
    check(editor.caretColumn() == width, "каретка за вставленными пробелами");

    // Из середины — ДО СТОПА, а не ширина стопа: после одного знака остаётся
    // три пробела, а не четыре.
    Editor middle;
    middle.openText(QStringLiteral("таб-середина.md"), kNote);
    middle.caretTo(QStringLiteral("one"), 1);
    QTest::keyClick(&middle, Qt::Key_Tab, Qt::NoModifier);
    QTest::qWait(5);
    checkEq("o   ne", middle.caretLine().toStdString(), "Tab из середины — до ближайшего стопа");

    // Shift+Tab без выделения снимает отступ своей строки, но не глубже нуля.
    Editor back;
    back.openText(QStringLiteral("таб-снять.md"), "head\n\n```py\n        deep\n```\n");
    back.caretTo(QStringLiteral("        deep"), 12);
    QTest::keyClick(&back, Qt::Key_Backtab, Qt::ShiftModifier);
    QTest::qWait(5);
    checkEq("    deep", back.caretLine().toStdString(), "Shift+Tab снял один стоп");
    QTest::keyClick(&back, Qt::Key_Backtab, Qt::ShiftModifier);
    QTest::qWait(5);
    checkEq("deep", back.caretLine().toStdString(), "и второй");
    QTest::keyClick(&back, Qt::Key_Backtab, Qt::ShiftModifier);
    QTest::qWait(5);
    checkEq("deep", back.caretLine().toStdString(), "глубже нуля не уходит");

    // Отступ не кратен стопу: снимается ДО БЛИЖАЙШЕГО стопа, а не на целый
    // стоп. Шесть пробелов дают четыре, а не два.
    Editor odd;
    odd.openText(QStringLiteral("таб-неровно.md"), "head\n\n```py\n      deep\n```\n");
    odd.caretTo(QStringLiteral("      deep"), 10);
    QTest::keyClick(&odd, Qt::Key_Backtab, Qt::ShiftModifier);
    QTest::qWait(5);
    checkEq("    deep", odd.caretLine().toStdString(), "снятие идёт до ближайшего стопа");

    // Выделение по строкам: отступ всех задетых строк, каретка и выделение
    // переживают правку.
    Editor many;
    many.openText(QStringLiteral("таб-выделение.md"), kNote);
    {
        QTextCursor at(many.document());
        QTextBlock first;
        for (QTextBlock b = many.document()->firstBlock(); b.isValid(); b = b.next())
            if (b.text() == QStringLiteral("one")) first = b;
        at.setPosition(first.position() + 1);
        at.setPosition(first.next().position() + 1, QTextCursor::KeepAnchor);
        many.setTextCursor(at);
    }
    QTest::keyClick(&many, Qt::Key_Tab, Qt::NoModifier);
    QTest::qWait(5);
    check(editorMarkdown(many).find("    one\n    two") != std::string::npos,
          "Tab по выделению отступил обе строки");
    check(many.textCursor().hasSelection(), "выделение пережило правку");
    QTest::keyClick(&many, Qt::Key_Backtab, Qt::ShiftModifier);
    QTest::qWait(5);
    check(editorMarkdown(many).find("one\ntwo") != std::string::npos,
          "Shift+Tab по выделению вернул как было");

    // Вне блока кода Tab по-прежнему живёт списками: правило не должно было
    // задеть их вовсе.
    Editor list;
    list.openText(QStringLiteral("таб-список.md"), "- one\n- two\n");
    list.caretTo(QStringLiteral("two"), 0);
    QTest::keyClick(&list, Qt::Key_Tab, Qt::NoModifier);
    QTest::qWait(5);
    check(zametti::levelOf(list.textCursor().block()) == 1,
          "Tab в списке по-прежнему углубляет пункт");
    check(!list.caretLine().startsWith(QLatin1Char(' ')),
          "и пробелов в пункт не ставит");

    checkStillLegal(editor, "после табуляции");
}

// --- нарезка блока кода по строкам ------------------------------------------
//
// ПОЧИНЕНО. Блок кода лежит в документе построчно, по QTextBlock на строку, и
// это не прихоть: Qt переразмечает целиком тот блок, в который пишут, и правка
// внутри блока на 31 480 знаков стоила 4257 мкс против 109 мкс в блоке на сотню
// (замер этапа 5, doc_model.h).
//
// Слияние соседей (repairAfterTyping) ставило на месте границы блоков
// разделитель строк, и весь блок оказывался одним QTextBlock. Файл от этого не
// менялся — читался тот же набор блоков, — но нарезка пропадала, а вместе с ней
// и вся выгода построчного хранения.
//
// Чинит splitLiteralSoftBreaks в шве: мягких переносов внутри литерального
// блока не бывает, каждая строка — свой блок-продолжение. Прежние два захода
// (не сливать строки; резать в syncLiteralBlocks) меняли выход трёх фаззеров;
// этот не меняет — потому что режет только в шве и только литеральные блоки, а
// сверка со сборкой в отладочной сборке доказывает, что результат совпадает с
// тем, что собрал бы сборщик.
void checkCodeStaysSliced() {
    Editor editor;
    editor.openText(QStringLiteral("нарезка.md"), kNote);

    const auto codeBlocks = [&editor] {
        int n = 0;
        for (QTextBlock b = editor.document()->firstBlock(); b.isValid(); b = b.next())
            if (zametti::kindOf(b) == zametti::Kind::Code) ++n;
        return n;
    };
    check(codeBlocks() == 2, "после открытия строк кода две");

    editor.caretTo(QStringLiteral("two"), 3);
    QTest::keyClicks(&editor, QStringLiteral("x"));
    QTest::qWait(5);
    check(codeBlocks() == 2, "после набора строки блока остались нарезанными");
    checkStillLegal(editor, "набор в блоке кода");
}

// --- литеральные табы из старых файлов --------------------------------------
//
// Мы ставим пробелы, но в чужих файлах табы есть, и рисоваться они обязаны тем
// же стопом: иначе одинаковый на вид отступ на экране разъезжается. Спрашиваем
// не настройку, а РАССТАНОВКУ — где на экране оказалась буква после таба.
// СЛУЧАЙ ВЛАДЕЛЬЦА, нажатиями: «About the Scopes» в Ficus Tutorial. Список
// разорван блоком кода пополам, оттого нумерация за кодом начинается заново.
// Tab в самом начале блока кода вбирает его в пункт — и два списка сходятся в
// один.
//
// Проверяется здесь именно КЛАВИША, а не глагол: между «операция работает» и
// «клавиша работает» лежит весь разбор нажатия, и Tab по дороге легко достаётся
// то отступу кода, то смене фокуса.
void checkCodeIntoListItem() {
    Editor editor;
    editor.openText(QStringLiteral("код-в-пункт.md"),
                    "6. Names never conflict.\n"
                    "7. Types never conflict:\n"
                    "\n"
                    "```\n"
                    "type M=string\n"
                    "```\n"
                    "\n"
                    "1. Functions may have the same name.\n"
                    "2. Values may have the same name.\n");
    editor.caretTo(QStringLiteral("type M=string"), 0);
    QTest::keyClick(&editor, Qt::Key_Tab, Qt::NoModifier);
    QTest::qWait(5);

    // Номера не хранятся — их считает прогон, поэтому счёт идёт с единицы и до
    // правки: «6.» и «7.» стали первым и вторым пунктом ещё при чтении файла.
    checkEq("1. Names never conflict.\n"
            "2. Types never conflict:\n"
            "\n"
            "   ```\n"
            "   type M=string\n"
            "   ```\n"
            "\n"
            "3. Functions may have the same name.\n"
            "4. Values may have the same name.\n",
            editorMarkdown(editor), "Tab вбирает блок кода в пункт, список сходится");
    checkStillLegal(editor, "код внутри пункта");

    // И обратно — тем же Shift+Tab из того же места.
    QTest::keyClick(&editor, Qt::Key_Backtab, Qt::ShiftModifier);
    QTest::qWait(5);
    checkEq("1. Names never conflict.\n"
            "2. Types never conflict:\n"
            "\n"
            "```\n"
            "type M=string\n"
            "```\n"
            "\n"
            "1. Functions may have the same name.\n"
            "2. Values may have the same name.\n",
            editorMarkdown(editor), "Shift+Tab выводит блок кода обратно");
    checkStillLegal(editor, "код снаружи пункта");
}

void checkLiteralTabs() {
    Editor editor;
    // Первая строка — таб, вторая — пробелы до того же стопа (ширина 4).
    editor.openText(QStringLiteral("таб-старый.md"), "head\n\n```py\na\tb\na   b\n```\n");
    QTest::qWait(20);

    qreal withTab = -1;
    qreal withSpaces = -2;
    for (QTextBlock b = editor.document()->firstBlock(); b.isValid(); b = b.next()) {
        const QTextLayout* layout = b.layout();
        if (layout == nullptr || layout->lineCount() == 0) continue;
        if (b.text() == QStringLiteral("a\tb")) withTab = layout->lineAt(0).cursorToX(2);
        if (b.text() == QStringLiteral("a   b")) withSpaces = layout->lineAt(0).cursorToX(4);
    }
    check(withTab > 0, "строка с табом разложена");
    check(std::fabs(withTab - withSpaces) < 1.0,
          "таб доводит до того же стопа, что и пробелы (" + num(withTab) + " против " +
              num(withSpaces) + ")");
}

// --- авто-отступ и отмена (замечания владельца по этапу 11) ------------------
const char* kProgram = R"(head

```
#include <stdio.h>

int main(int argc, char** argv) {
    printf("Hello, darling!\n");
}
```
)";

void checkAutoIndent() {
    Editor editor;
    editor.openText(QStringLiteral("отступ.md"), kProgram);
    const QString line = QStringLiteral("    printf(\"Hello, darling!\\n\");");
    editor.caretTo(line, line.size());
    QTest::keyClick(&editor, Qt::Key_Return, Qt::NoModifier);
    QTest::qWait(10);
    check(editor.caretColumn() == 4, "каретка встала за скопированным отступом (" +
                                         std::to_string(editor.caretColumn()) + ")");
    // Набирать здесь дальше нечего: первый же знак склеивает строки блока в
    // одну (см. checkCodeStaysSliced) — беда известная и пока не починенная.
}

// Та же проверка, но на НАСТОЯЩЕЙ заметке владельца (копия): в ней есть
// картинка, задачи и блок кода в самом конце.
void checkAutoIndentInRealNote(const QString& source) {
    if (source.isEmpty() || !QFile::exists(source)) return;
    QFile in(source);
    if (!in.open(QIODevice::ReadOnly)) return;
    const QString text = QString::fromUtf8(in.readAll());
    in.close();
    Editor editor;
    const QString path = writeNote(QStringLiteral("живая.md"), text);
    editor.resize(900, 700);
    editor.show();
    QTest::qWait(10);
    editor.openFile(path);
    QTest::qWait(60);

    const QString line = QStringLiteral("    printf(\"Hello, darling!\\n\");");
    editor.caretTo(line, line.size());
    QTest::keyClick(&editor, Qt::Key_Return, Qt::NoModifier);
    QTest::qWait(10);
    check(editor.caretColumn() == 4, "живая заметка: каретка за отступом (" +
                                         std::to_string(editor.caretColumn()) + ")");
}

void checkUndoAfterLeaving() {
    Editor editor;
    editor.openText(QStringLiteral("отмена.md"), kProgram);
    // Сначала правка ВНУТРИ блока — как у владельца.
    editor.caretTo(QStringLiteral("}"), 1);
    QTest::keyClicks(&editor, QStringLiteral("y"));
    QTest::qWait(10);
    const std::string afterTyping = editorMarkdown(editor);

    QTest::keyClick(&editor, Qt::Key_Return, Qt::ControlModifier);
    // ЖДЁМ автосохранение: человек между нажатиями думает, и таймер успевает
    // сработать. Без этой паузы набор проверял не то, что делает владелец.
    QTest::qWait(1600);
    const std::string afterLeaving = editorMarkdown(editor);
    check(afterLeaving != afterTyping, "Ctrl+Enter что-то изменил");

    QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
    QTest::qWait(10);
    checkEq(afterTyping, editorMarkdown(editor), "первый Ctrl+Z отменяет именно выход из блока");
}

// --- язык блока кода --------------------------------------------------------
//
// Договор брифа: щелчок по месту языка заводит поле, Enter принимает, Esc
// отменяет, пустое имя убирает язык; правка идёт штатным путём — отменяется
// одним Ctrl+Z и сериализуется в ```lang.
const char* kThree = R"(head

```python
one
```

middle

```c++
two
```

tail

```
three
```
)";

int firstBlockOf(Editor& editor, const QString& lineText) {
    for (QTextBlock b = editor.document()->firstBlock(); b.isValid(); b = b.next())
        if (b.text() == lineText) return b.blockNumber();
    return -1;
}

void checkLanguageEditor() {
    Editor editor;
    editor.openText(QStringLiteral("язык.md"), kThree);

    // Кандидаты — из самой заметки, ближайший ВЫШЕ первым.
    const int third = firstBlockOf(editor, QStringLiteral("three"));
    const QStringList near = editor.note().codeLanguagesNear(third);
    checkEq("c++,python", near.join(QLatin1Char(',')).toStdString(),
            "кандидаты — языки этой заметки, ближайший выше первым");
    check(editor.note().codeLanguagesNear(firstBlockOf(editor, QStringLiteral("one")))
              .join(QLatin1Char(',')) == QStringLiteral("c++"),
          "у первого блока кандидат только нижний");

    // Что «выше» важнее, чем «ближе»: продолжают обычно то, что писали только
    // что. Здесь верхний кандидат ДАЛЬШЕ нижнего, и всё равно идёт первым.
    Editor mixed;
    mixed.openText(QStringLiteral("язык-порядок.md"),
                   "```python\na\n```\n\nодин\n\nдва\n\n```\ntarget\n```\n\n"
                   "```rust\nb\n```\n");
    checkEq("python,rust",
            mixed.note().codeLanguagesNear(firstBlockOf(mixed, QStringLiteral("target")))
                .join(QLatin1Char(',')).toStdString(),
            "верхний кандидат идёт первым, даже если он дальше");

    // Само поле: встаёт, дополняет серым, Enter применяет.
    zametti::LanguageEditor* field = editor.editCodeLanguage(third, QRect(10, 10, 120, 20));
    check(field != nullptr, "поле ввода языка открылось");
    if (field == nullptr) return;
    QTest::keyClicks(field, QStringLiteral("p"));
    QTest::qWait(5);
    checkEq("ython", field->completion().toStdString(), "по «p» дописалось «ython»");
    checkEq("python", field->language().toStdString(), "принятое имя — целиком python");
    QTest::keyClick(field, Qt::Key_Return);
    QTest::qWait(20);
    check(editor.codeLanguageEditor() == nullptr, "после Enter поле закрылось");
    check(editorMarkdown(editor).find("```python\nthree") != std::string::npos,
          "язык уехал в файл: " + editorMarkdown(editor));

    // Один Ctrl+Z возвращает прежнее.
    QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
    QTest::qWait(20);
    check(editorMarkdown(editor).find("```\nthree") != std::string::npos,
          "один Ctrl+Z вернул блок без языка");

    // Esc не меняет ничего.
    const std::string before = editorMarkdown(editor);
    field = editor.editCodeLanguage(third, QRect(10, 10, 120, 20));
    if (field != nullptr) {
        QTest::keyClicks(field, QStringLiteral("py"));
        QTest::keyClick(field, Qt::Key_Escape);
        QTest::qWait(20);
        check(editor.codeLanguageEditor() == nullptr, "после Esc поле закрылось");
        checkEq(before, editorMarkdown(editor), "Esc ничего не поменял");
    }

    // Пустое имя убирает язык.
    const int firstBlock = firstBlockOf(editor, QStringLiteral("one"));
    field = editor.editCodeLanguage(firstBlock, QRect(10, 10, 120, 20));
    if (field != nullptr) {
        field->clear();
        QTest::keyClick(field, Qt::Key_Return);
        QTest::qWait(20);
        check(editorMarkdown(editor).find("```\none") != std::string::npos,
              "пустое имя убрало язык: " + editorMarkdown(editor));
    }

    // Свободные имена: никаких встроенных списков (владельцу нужны свои).
    field = editor.editCodeLanguage(firstBlock, QRect(10, 10, 120, 20));
    if (field != nullptr) {
        QTest::keyClicks(field, QStringLiteral("pf"));
        QTest::keyClick(field, Qt::Key_Return);
        QTest::qWait(20);
        check(editorMarkdown(editor).find("```pf\none") != std::string::npos,
              "«pf» принят как есть: " + editorMarkdown(editor));
    }

    // Круг: сменённый язык переживает запись и чтение.
    const std::string text = editorMarkdown(editor);
    checkEq(text, noteOf(text).toMarkdown(), "файл с новым языком читается в себя");
    checkStillLegal(editor, "после смены языка");
}

// Заметка без языков: дополнять нечем, и поле молчит.
void checkNoCandidates() {
    Editor editor;
    editor.openText(QStringLiteral("языков-нет.md"), kNote);
    const int block = firstBlockOf(editor, QStringLiteral("one"));
    check(editor.note().codeLanguagesNear(block).isEmpty(),
          "в заметке без языков кандидатов нет");
    zametti::LanguageEditor* field = editor.editCodeLanguage(block, QRect(10, 10, 120, 20));
    if (field == nullptr) return;
    QTest::keyClicks(field, QStringLiteral("p"));
    QTest::qWait(5);
    check(field->completion().isEmpty(), "дополнять нечем — и не дописывается");
    QTest::keyClick(field, Qt::Key_Escape);
    QTest::qWait(10);
}

// Порядок для Esc: сперва закрывается то, что открыто поверх текста.
void checkEscapeOrder() {
    using zametti::EscapeAction;
    check(zametti::escapeActionFor(true, true, true) == EscapeAction::CloseLanguageEditor,
          "при открытом поле языка Esc закрывает его, а не поиск");
    check(zametti::escapeActionFor(true, false, false) == EscapeAction::CloseLanguageEditor,
          "поле языка закрывается и без панели поиска");
    check(zametti::escapeActionFor(false, true, true) == EscapeAction::LeaveTableEdit,
          "правка таблицы закрывается раньше панели поиска");
    check(zametti::escapeActionFor(false, false, true) == EscapeAction::CloseFindBar,
          "без поля языка и правки Esc закрывает панель поиска");
    check(zametti::escapeActionFor(false, false, false) == EscapeAction::Nothing,
          "когда закрывать нечего, Esc не делает ничего");
}

}  // namespace

static int ztRunSuite(int argc, char** argv) {
    g_dir = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QDir::tempPath();
    QDir().mkpath(g_dir);

    runMatrix();
    checkLeaveCodeBlock();
    checkCodeTabs();
    checkCodeIntoListItem();
    checkLiteralTabs();
    checkCodeStaysSliced();
    checkAutoIndent();
    checkAutoIndentInRealNote(argc > 2 ? QString::fromLocal8Bit(argv[2]) : QString());
    checkUndoAfterLeaving();
    checkLanguageEditor();
    checkNoCandidates();
    checkEscapeOrder();

    std::printf("правка кода: %d проверок, %s\n", zt::g_checks,
                zt::g_failures == 0 ? "всё зелено"
                                    : (std::to_string(zt::g_failures) + " провалов").c_str());
    return zt::freshFailures();
}

// Набор целиком одним TEST: тело не тронуто, argv ему собран здесь.
// Дробить на отдельные проверки — отдельная работа, по одному набору.
TEST(CodeEdit, All) {
    std::vector<QByteArray> ztArgs{QByteArrayLiteral("code_edit_test")};
    ztArgs.push_back((zt::TestData::outDir(QStringLiteral("code-edit"))).toLocal8Bit());
    std::vector<char*> ztArgv;
    for (QByteArray& a : ztArgs) ztArgv.push_back(a.data());
    EXPECT_EQ(0, ztRunSuite(int(ztArgv.size()), ztArgv.data()));
}
