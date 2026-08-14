// Каретка ходит. Матрица самых обычных нажатий — по всем видам блоков.
//
// Набор появился по требованию владельца, и требование справедливое: «тестов у
// нас явно маловато — что стрелки влево, вправо, вверх, вниз работают, что
// буквы вставляются, что заметка при загрузке не растёт в десять раз». Каждая
// беда этого дня была именно такой: не хитрый край, а самое обычное действие,
// которого никто не спрашивал.
//
// Проверяется поведение, а не реализация: из начала заметки до конца стрелкой
// вниз и обратно вверх, по строке вправо и влево, буква на каждом блоке. Виды
// блоков — все, что у нас есть, включая объекты (картинка, таблица, формула):
// именно на них каретка и застревала.

#include "block_object.h"
#include "doc_model.h"
#include "editor_widget.h"
#include "formula.h"
#include "resources.h"

#include "test_util.h"
#include "testdata.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QTest>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>

#include <string>
#include <vector>

namespace {

QString g_dir;

zametti::NoteEditor* openNote(const QString& name, const char* text) {
    const QString path = QDir(g_dir).filePath(name + QStringLiteral(".md"));
    QFile file(path);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) file.write(text);
    file.close();

    auto* editor = new zametti::NoteEditor;
    editor->resize(900, 700);
    editor->show();
    QTest::qWait(20);
    editor->openFile(path);
    QTest::qWait(60);
    QTextCursor at = editor->textCursor();
    at.setPosition(0);
    editor->setTextCursor(at);
    QTest::qWait(20);
    return editor;
}

// Заметка со всеми видами блоков подряд. Формула — и выключная, и одиночная
// строчная; таблица; картинка (файла нет, показывается рамкой — для хода
// каретки это то же самое); код; список; цитата; черта.
const char* kNote = R"(# Заголовок

Обычный абзац.

$$\frac{a}{b}$$

| a | b |
|---|---|
| 1 | 2 |

```python
код = 1
```

- пункт списка
- второй пункт

> цитата

![снимок](нет-такого.jxl)

___

Хвост заметки.
)";

// Сколько нажатий нужно, чтобы дойти от начала до последнего блока.
// Возвращает -1, если каретка застряла.
int walk(zametti::NoteEditor& editor, Qt::Key key, int limit) {
    QTextCursor at = editor.textCursor();
    at.setPosition(key == Qt::Key_Down ? 0 : editor.document()->characterCount() - 1);
    editor.setTextCursor(at);
    QTest::qWait(20);

    const int target = key == Qt::Key_Down ? editor.document()->blockCount() - 1 : 0;
    int wasPos = editor.textCursor().position();
    int stuck = 0;
    for (int i = 0; i < limit; ++i) {
        QTest::keyClick(&editor, key);
        QTest::qWait(4);
        const int nowBlock = editor.textCursor().blockNumber();
        const int nowPos = editor.textCursor().position();
        if (nowBlock == target) return i + 1;
        // Застревание: позиция не меняется два раза подряд.
        stuck = nowPos == wasPos ? stuck + 1 : 0;
        if (stuck >= 2) {
            std::printf("  застряла в блоке %d («%s»), нажатий %d\n", nowBlock,
                        editor.document()->findBlockByNumber(nowBlock).text().left(30).toUtf8().constData(),
                        i + 1);
            return -1;
        }
        wasPos = nowPos;
    }
    std::printf("  не дошла: остановилась в блоке %d из %d\n",
                editor.textCursor().blockNumber(), target);
    return -1;
}

// --- ход по вертикали -------------------------------------------------------

void checkVerticalWalk() {
    zametti::NoteEditor* editor = openNote(QStringLiteral("ходьба"), kNote);
    const int blocks = editor->document()->blockCount();

    const int down = walk(*editor, Qt::Key_Down, blocks * 4);
    ZT_TRUE("стрелкой вниз каретка доходит до конца заметки", down > 0);
    const int up = walk(*editor, Qt::Key_Up, blocks * 4);
    ZT_TRUE("стрелкой вверх — до начала", up > 0);

    delete editor;
}

// --- ход по горизонтали через объект ----------------------------------------
//
// Объект атомарен: вправо и влево он проходится, а не грызётся по буквам.
void checkHorizontalWalk() {
    zametti::NoteEditor* editor = openNote(QStringLiteral("вправо"), kNote);
    const int chars = editor->document()->characterCount();

    QTextCursor at = editor->textCursor();
    at.setPosition(0);
    editor->setTextCursor(at);
    int last = -1;
    int stuck = 0;
    int steps = 0;
    for (; steps < chars * 2; ++steps) {
        QTest::keyClick(editor, Qt::Key_Right);
        QTest::qWait(2);
        const int now = editor->textCursor().position();
        if (now >= chars - 1) break;
        stuck = now == last ? stuck + 1 : 0;
        if (stuck >= 2) break;
        last = now;
    }
    ZT_TRUE("стрелкой вправо каретка доходит до конца: шагов " + std::to_string(steps),
            editor->textCursor().position() >= chars - 2);

    for (steps = 0; steps < chars * 2; ++steps) {
        QTest::keyClick(editor, Qt::Key_Left);
        QTest::qWait(2);
        if (editor->textCursor().position() <= 0) break;
    }
    ZT_TRUE("и стрелкой влево обратно в начало", editor->textCursor().position() == 0);

    delete editor;
}

// --- буквы печатаются на каждом блоке ---------------------------------------

void checkTypingEverywhere() {
    zametti::NoteEditor* editor = openNote(QStringLiteral("печать"), kNote);
    const QString before = editor->document()->toPlainText();
    // ПО НОМЕРАМ, А НЕ ПО ИТЕРАТОРУ. Набор меняет документ, и обход блоков
    // ссылками разваливается на первой же букве: моя первая редакция
    // напечатала три буквы вместо тринадцати и молча остановилась.
    std::vector<int> targets;
    for (QTextBlock b = editor->document()->firstBlock(); b.isValid(); b = b.next()) {
        // На объект каретку ставить незачем: там буква означает не букву, и
        // правила у объектов свои (слой объекта, своя матрица).
        if (zametti::objectOf(b).valid()) continue;
        if (zametti::isVSpaceBlock(b)) continue;   // пустая строка — не место для буквы
        targets.push_back(b.blockNumber());
    }
    int typed = 0;
    for (const int number : targets) {
        const QTextBlock b = editor->document()->findBlockByNumber(number);
        if (!b.isValid()) continue;
        QTextCursor at(b);
        at.movePosition(QTextCursor::EndOfBlock);
        editor->setTextCursor(at);
        QTest::keyClicks(editor, QStringLiteral("z"));
        QTest::qWait(3);
        ++typed;
    }
    const QString after = editor->document()->toPlainText();
    // РОВНО СТОЛЬКО, СКОЛЬКО НАЖАЛИ. Допуск «минус два» я поставил, не разобрав
    // случай, — это и есть заглушение: проверка перестала бы замечать, что две
    // буквы куда-то делись.
    ZT_EQ("букв напечатано столько, сколько блоков", std::to_string(typed),
          std::to_string(after.count(QLatin1Char('z'))));
    ZT_TRUE("и блоков было не два-три: " + std::to_string(typed), typed > 5);
    ZT_TRUE("и текст не потерялся", after.size() >= before.size());
    delete editor;
}

// --- заметка от открытия не растёт ------------------------------------------
//
// Та самая проверка, которой не было: открыли — сохранили — байты те же. На
// заметке со всеми видами блоков и три раза подряд, потому что беда владельца
// была не в первом заходе, а в накоплении.
void checkOpenDoesNotGrow() {
    const QString path = QDir(g_dir).filePath(QStringLiteral("рост.md"));
    QFile file(path);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) file.write(kNote);
    file.close();

    zametti::NoteEditor editor;
    editor.resize(900, 700);
    editor.show();
    QTest::qWait(20);
    editor.openFile(path);
    QTest::qWait(60);

    QFile first(path);
    const QByteArray canon = first.open(QIODevice::ReadOnly) ? first.readAll() : QByteArray();
    first.close();

    for (int round = 1; round <= 3; ++round) {
        editor.openFile(path);
        QTest::qWait(40);
        editor.save(false, true);
        QTest::qWait(20);
        QFile again(path);
        const QByteArray now = again.open(QIODevice::ReadOnly) ? again.readAll() : QByteArray();
        ZT_EQ("заметка не изменилась от открытия, круг " + std::to_string(round),
              std::to_string(canon.size()), std::to_string(now.size()));
        ZT_TRUE("и байты те же, круг " + std::to_string(round), now == canon);
    }
}

}  // namespace

static int ztRunSuite(int argc, char** argv) {
    g_dir = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QDir::tempPath();
    QDir().mkpath(g_dir);
    zametti::loadEmbeddedFonts();
    QString error;
    if (!zametti::Formulas::init(&error))
        std::printf("движок формул не поднялся (%s) — формулы будут рамкой\n",
                    qPrintable(error));

    checkVerticalWalk();
    checkHorizontalWalk();
    checkTypingEverywhere();
    checkOpenDoesNotGrow();

    return zt::report("каретка и обычные нажатия");
}

// Набор целиком одним TEST: тело не тронуто, argv ему собран здесь.
// Дробить на отдельные проверки — отдельная работа, по одному набору.
TEST(CaretMatrix, All) {
    std::vector<QByteArray> ztArgs{QByteArrayLiteral("caret_matrix_test")};
    ztArgs.push_back((zt::TestData::outDir(QStringLiteral("caret-matrix"))).toLocal8Bit());
    std::vector<char*> ztArgv;
    for (QByteArray& a : ztArgs) ztArgv.push_back(a.data());
    EXPECT_EQ(0, ztRunSuite(int(ztArgv.size()), ztArgv.data()));
}
