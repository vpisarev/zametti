// ФОРМУЛЫ И ТАБЛИЦЫ ИЗ-ПОД КЛАВИАТУРЫ (просьба владельца, 02.09.2026).
//
// Три правила набора: закрывающий `$` замыкает строчную формулу по канону
// math_scan (пара помечается раскрытой, сворачивает судья при уходе каретки);
// `$$` в начале строки раскрывает выключную с кареткой между заборами; Enter
// после строки-шапки `|…|` достраивает разделитель и новый ряд и раскрывает
// таблицу. Проверяется ЖИВЫМ редактором и настоящими нажатиями — теми же
// дверями, что у человека: keyClicks → keyPressEvent → правило.
//
// Матрица краёв — до починок: отказы («цена $5 и $7», `\$`, доллар в
// код-спане, `текст $$`, `|` одиночкой, шапка без хвостовой палки, код-блок)
// стерегутся наравне со срабатываниями, и первый Ctrl+Z после срабатывания
// возвращает набранное литералом.

#include "doc_model.h"
#include "editor_widget.h"
#include "scratch_files.h"
#include "settings_hook.h"
#include "table.h"
#include "test_util.h"
#include "testdata.h"

#include <QDir>
#include <QFile>
#include <QTest>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QStringList>
#include <QTextFragment>

#include <cstdio>
#include <string>
#include <vector>

namespace {

QString g_dir;

void check(bool ok, const std::string& what) {
    ++zt::g_checks;
    if (ok) return;
    ++zt::g_failures;
    std::printf("провал: %s\n", what.c_str());
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

// Свежий редактор на заметке из этих байтов; каретка — в конец блока block.
struct Rig {
    zametti::NoteEditor editor;
    QString path;

    Rig(const char* name, const QString& text, int block = 0) {
        path = writeNote(name, text);
        editor.resize(700, 500);
        editor.show();
        QTest::qWait(20);
        editor.openFile(path);
        QTest::qWait(20);
        toBlockEnd(block);
    }
    void toBlockEnd(int block) {
        const QTextBlock at = editor.document()->findBlockByNumber(block);
        QTextCursor cursor = editor.textCursor();
        cursor.setPosition(at.position() + at.length() - 1);
        editor.setTextCursor(cursor);
        QTest::qWait(10);
    }
    QTextBlock block(int number) const {
        return editor.document()->findBlockByNumber(number);
    }
};

int inlineObjectsIn(const QTextBlock& block) {
    int count = 0;
    for (QTextBlock::iterator it = block.begin(); !it.atEnd(); ++it) {
        const QTextFragment fragment = it.fragment();
        if (fragment.isValid() &&
            fragment.charFormat().objectType() == zametti::InlineFormulaObject)
            count += fragment.length();
    }
    return count;
}

// --- (а) строчная формула ---------------------------------------------------

void checkInlineDollar() {
    // Замкнутая пара в середине строки: после `$` кусок помечен раскрытым, а
    // уход каретки из блока сворачивает его в объект. В файл — без экрана.
    {
        Rig rig("инлайн.md", QStringLiteral("энергия это\n\nвторой абзац\n"));
        QTest::keyClicks(&rig.editor, QStringLiteral(" $E=mc^2$"));
        QTest::qWait(10);
        check(zametti::hasOpenInlineFormula(rig.block(0)),
              "закрывающий доллар пометил пару раскрытой");
        check(inlineObjectsIn(rig.block(0)) == 0, "под руками набора объекта ещё нет");
        // Набор продолжается СНАРУЖИ формулы: следующая буква не math.
        QTest::keyClicks(&rig.editor, QStringLiteral("!"));
        QTest::qWait(10);
        rig.toBlockEnd(2);   // каретка ушла — судья свернул
        QTest::qWait(20);
        check(inlineObjectsIn(rig.block(0)) == 1, "уход каретки свернул пару в объект");
        check(!zametti::hasOpenInlineFormula(rig.block(0)), "раскрытых кусков не осталось");
        check(rig.block(0).text().endsWith(QStringLiteral("!")),
              "буква после формулы осталась текстом");
        rig.editor.save(false, true);
        QTest::qWait(10);
        check(readFile(rig.path).contains(QStringLiteral("энергия это $E=mc^2$!")),
              "в файле — формула без экрана: " + readFile(rig.path).toStdString());
    }

    // Отказы канона: цены (закрывающий перед цифрой), экранированный `\$`,
    // пробел после открывающего.
    {
        Rig rig("цены.md", QStringLiteral("тут\n"));
        // Только ASCII: keyClicks не умеет знаков вне клавиш (qasciikey).
        QTest::keyClicks(&rig.editor, QStringLiteral(" price $5 and $7"));
        QTest::qWait(10);
        check(!zametti::hasOpenInlineFormula(rig.block(0)), "цены — не формула");
    }
    {
        Rig rig("экран.md", QStringLiteral("тут\n"));
        QTest::keyClicks(&rig.editor, QStringLiteral(" \\$x$"));
        QTest::qWait(10);
        check(!zametti::hasOpenInlineFormula(rig.block(0)),
              "руками набранный \\$ — литерал, пара не замыкается");
    }
    {
        Rig rig("пробел.md", QStringLiteral("тут\n"));
        QTest::keyClicks(&rig.editor, QStringLiteral(" $ x$"));
        QTest::qWait(10);
        check(!zametti::hasOpenInlineFormula(rig.block(0)),
              "открывающий перед пробелом — не граница");
    }

    // Доллар внутри код-спана открывателем не считается: пара не замыкается
    // поверх размеченного куска.
    {
        Rig rig("код.md", QStringLiteral("тут\n"));
        QTest::keyClicks(&rig.editor, QStringLiteral(" `a $b` and $"));
        QTest::qWait(10);
        check(!zametti::hasOpenInlineFormula(rig.block(0)),
              "доллар в код-спане пару не открывает");
    }

    // В блоке кода правило молчит.
    {
        Rig rig("кодблок.md", QStringLiteral("```\nint a;\n```\n"));
        rig.toBlockEnd(0);
        QTest::keyClicks(&rig.editor, QStringLiteral("$x$"));
        QTest::qWait(10);
        check(!zametti::hasOpenInlineFormula(rig.block(0)), "в коде долларам нет дела");
    }

    // Отмена. Первый Ctrl+Z по конвейеру проекта сперва СОХРАНЯЕТ, а
    // сохранение сворачивает раскрытое под кареткой судьёй (см.
    // NoteEditor::save) — и отменяется именно это сворачивание: формула снова
    // раскрыта. Второй Ctrl+Z снимает пометку — доллары литеральны.
    {
        Rig rig("отмена.md", QStringLiteral("до\n"));
        QTest::keyClicks(&rig.editor, QStringLiteral(" $a$"));
        QTest::qWait(60);
        check(zametti::hasOpenInlineFormula(rig.block(0)), "пара замкнулась");
        QTest::keyClick(&rig.editor, Qt::Key_Z, Qt::ControlModifier);
        QTest::qWait(10);
        QTest::keyClick(&rig.editor, Qt::Key_Z, Qt::ControlModifier);
        QTest::qWait(10);
        check(!zametti::hasOpenInlineFormula(rig.block(0)) &&
                  rig.block(0).text() == QStringLiteral("до $a$"),
              "Ctrl+Z вернул литеральные доллары: " +
                  rig.block(0).text().toStdString() +
                  (zametti::hasOpenInlineFormula(rig.block(0)) ? " [open]" : ""));
    }
}

// --- (б) выключная формула --------------------------------------------------

void checkDisplayDollar() {
    // `$$` хвостовой строкой абзаца — раскрытая выключная: оба забора уже
    // стоят, каретка на пустой средней строке; голова абзаца остаётся собой
    // (шов вправе отделить её пустой строкой — канон файла); Esc судит
    // текстом.
    {
        Rig rig("выключная.md", QStringLiteral("строка\n"));
        QTest::keyClick(&rig.editor, Qt::Key_Return);   // мягкий перенос
        QTest::qWait(10);
        QTest::keyClicks(&rig.editor, QStringLiteral("$$"));
        QTest::qWait(10);
        const QTextBlock opened = rig.editor.textCursor().block();
        check(opened.text().count(QStringLiteral("$$")) == 2,
              "оба забора выключной стоят: " + opened.text().toStdString());
        check(zametti::hasOpenInlineFormula(opened), "формула раскрыта");
        check(rig.block(0).text() == QStringLiteral("строка"),
              "голова абзаца осталась собой: " + rig.block(0).text().toStdString());
        QTest::keyClicks(&rig.editor, QStringLiteral("x^2"));
        QTest::qWait(10);
        QTest::keyClick(&rig.editor, Qt::Key_Escape);
        QTest::qWait(20);
        const QTextBlock judged = rig.editor.textCursor().block();
        const zametti::BlockFormulaRef ref = zametti::blockFormulaRef(judged);
        check(ref.valid && ref.display, "Esc свернул в выключную формулу");
        check(ref.source.contains(QStringLiteral("x^2")), "набранное — в исходнике");
    }

    // Отказ: перед `$$` не только пробельные.
    {
        Rig rig("непусто.md", QStringLiteral("слово\n"));
        QTest::keyClicks(&rig.editor, QStringLiteral(" $$"));
        QTest::qWait(10);
        check(!zametti::hasOpenInlineFormula(rig.block(0)),
              "текст $$ — не заявка на выключную");
    }

    // Отмена: первый Ctrl+Z снимает сворачивание, сделанное сохранением (см.
    // комментарий у строчной), второй — саму достройку: `$$` снова литеральный
    // хвост абзаца.
    {
        Rig rig("отмена2.md", QStringLiteral("строка\n"));
        QTest::keyClick(&rig.editor, Qt::Key_Return);
        QTest::qWait(60);
        QTest::keyClicks(&rig.editor, QStringLiteral("$$"));
        QTest::qWait(60);
        check(zametti::hasOpenInlineFormula(rig.editor.textCursor().block()),
              "пара заборов раскрылась");
        QTest::keyClick(&rig.editor, Qt::Key_Z, Qt::ControlModifier);
        QTest::qWait(10);
        QTest::keyClick(&rig.editor, Qt::Key_Z, Qt::ControlModifier);
        QTest::qWait(10);
        const QString after = rig.block(0).text().replace(QChar::LineSeparator,
                                                          QLatin1Char('\n'));
        check(after == QStringLiteral("строка\n$$"),
              "Ctrl+Z вернул литеральные $$: " + after.toStdString());
    }
}

// --- (в) таблица по Enter ---------------------------------------------------

void checkTableEnter() {
    // Шапка `|a|b|` и Enter: разделитель на две колонки, новый ряд, каретка в
    // нём; Esc судит блок объектом-таблицей.
    {
        Rig rig("таблица.md", QStringLiteral("выше\n\n.\n"), 2);
        // Отдельный абзац под шапку: третий блок «.» переписываем.
        QTest::keyClick(&rig.editor, Qt::Key_Backspace);
        QTest::qWait(10);
        QTest::keyClicks(&rig.editor, QStringLiteral("|a|b|"));
        QTest::qWait(10);
        QTest::keyClick(&rig.editor, Qt::Key_Return);
        QTest::qWait(10);
        const QTextBlock opened = rig.editor.textCursor().block();
        const QString shown = opened.text().replace(QChar::LineSeparator, QLatin1Char('\n'));
        check(shown == QStringLiteral("|a|b|\n|:--:|:--:|\n| "),
              "Enter достроил разделитель и новый ряд: " + shown.toStdString());
        check(rig.editor.textCursor().atBlockEnd(), "каретка — в конце нового ряда");
        QTest::keyClicks(&rig.editor, QStringLiteral("1"));
        QTest::qWait(10);
        QTest::keyClick(&rig.editor, Qt::Key_Escape);
        QTest::qWait(20);
        const QTextBlock judged = rig.editor.textCursor().block();
        check(zametti::isTableObjectBlock(judged), "Esc свернул таблицу объектом");
        const zametti::Table table = zametti::parseTable(zametti::tableSourceOf(judged));
        check(table.valid && table.columns == 2 && table.bodyRows() == 1,
              "две колонки и один ряд тела");
        rig.editor.save(false, true);
        QTest::qWait(10);
        check(readFile(rig.path).contains(QStringLiteral("|a|b|\n|:--:|:--:|\n| 1")),
              "таблица дошла до файла как набрана");
    }

    // `\|a\|` — палки экранированы: колонка одна.
    {
        Rig rig("экранпалка.md", QStringLiteral("выше\n\n.\n"), 2);
        QTest::keyClick(&rig.editor, Qt::Key_Backspace);
        QTest::qWait(10);
        QTest::keyClicks(&rig.editor, QStringLiteral("\\|a\\|"));
        QTest::qWait(10);
        QTest::keyClick(&rig.editor, Qt::Key_Return);
        QTest::qWait(10);
        const QString shown = rig.editor.textCursor().block().text().replace(
            QChar::LineSeparator, QLatin1Char('\n'));
        check(shown == QStringLiteral("\\|a\\|\n|:--:|\n| "),
              "экранированные палки — одна колонка: " + shown.toStdString());
    }

    // Отказы: `|` одиночкой, шапка без хвостовой палки, шапка в блоке кода.
    {
        Rig rig("палка.md", QStringLiteral("выше\n\n.\n"), 2);
        QTest::keyClick(&rig.editor, Qt::Key_Backspace);
        QTest::keyClicks(&rig.editor, QStringLiteral("|"));
        QTest::keyClick(&rig.editor, Qt::Key_Return);
        QTest::qWait(10);
        check(!zametti::isRawBlock(rig.block(2)), "одинокая палка таблицей не стала");
    }
    {
        Rig rig("безхвоста.md", QStringLiteral("выше\n\n.\n"), 2);
        QTest::keyClick(&rig.editor, Qt::Key_Backspace);
        QTest::keyClicks(&rig.editor, QStringLiteral("|a|b"));
        QTest::keyClick(&rig.editor, Qt::Key_Return);
        QTest::qWait(10);
        check(!zametti::isRawBlock(rig.block(2)), "шапка без хвостовой палки — не шапка");
    }
    {
        Rig rig("вкоде.md", QStringLiteral("```\n|a|b|\n```\n"));
        rig.toBlockEnd(0);
        // Каретка в конце строки |a|b| внутри кода: Enter — строка кода.
        const QTextBlock code = rig.block(0);
        QTextCursor cursor = rig.editor.textCursor();
        const int line = code.text().indexOf(QStringLiteral("|a|b|")) + 5;
        cursor.setPosition(code.position() + line);
        rig.editor.setTextCursor(cursor);
        QTest::keyClick(&rig.editor, Qt::Key_Return);
        QTest::qWait(10);
        check(zametti::kindOf(rig.block(0)) == zametti::Kind::Code &&
                  !zametti::isTableObjectBlock(rig.block(0)),
              "в коде Enter таблиц не строит");
    }

    // Отмена: первый Ctrl+Z снимает сворачивание, сделанное сохранением,
    // второй — саму достройку: абзац-шапка как набрана.
    {
        Rig rig("отмена3.md", QStringLiteral("выше\n\n.\n"), 2);
        QTest::keyClick(&rig.editor, Qt::Key_Backspace);
        QTest::qWait(10);
        QTest::keyClicks(&rig.editor, QStringLiteral("|a|b|"));
        QTest::qWait(60);
        QTest::keyClick(&rig.editor, Qt::Key_Return);
        QTest::qWait(10);
        QTest::keyClick(&rig.editor, Qt::Key_Z, Qt::ControlModifier);
        QTest::qWait(10);
        QTest::keyClick(&rig.editor, Qt::Key_Z, Qt::ControlModifier);
        QTest::qWait(10);
        check(!zametti::isRawBlock(rig.block(2)) &&
                  rig.block(2).text() == QStringLiteral("|a|b|"),
              "Ctrl+Z вернул абзац-шапку: " + rig.block(2).text().toStdString());
    }
}

// --- (г) Shift+Enter в раскрытой таблице — новый ряд --------------------------
//
// Просьба владельца (04.09.2026): из ЛЮБОГО места ряда Shift+Enter вставляет
// под ним пустой ряд с числом колонок шапки, каретка — в первой ячейке
// («Shift+Enter == End + Shift+Enter»). С шапки и с разделителя ряд идёт после
// разделителя (первым рядом тела). В конце последнего ряда — тоже ряд, а не
// абзац под таблицей (тот — Esc и Ctrl+Enter на объекте).

const char* kTableNote =
    "выше\n\n| a | b |\n|---|---|\n| 1 | 2 |\n| 3 | 4 |\n\nниже\n";

QStringList linesOf(const QTextBlock& block) {
    return block.text().split(QChar::LineSeparator);
}

// Раскрыть таблицу-объект в блоке 2 (Enter на объекте), каретка — в начало.
bool openTableIn(Rig& rig) {
    QTextCursor cursor(rig.block(2));
    rig.editor.setTextCursor(cursor);
    QTest::qWait(10);
    QTest::keyClick(&rig.editor, Qt::Key_Return);
    QTest::qWait(20);
    return zametti::isRawBlock(rig.block(2)) && !zametti::isTableObjectBlock(rig.block(2)) &&
           linesOf(rig.block(2)).size() == 4;
}

void caretAtLine(Rig& rig, int line, int column) {
    const QTextBlock block = rig.block(2);
    const QStringList lines = linesOf(block);
    int offset = 0;
    for (int i = 0; i < line && i < lines.size(); ++i) offset += int(lines.at(i).size()) + 1;
    QTextCursor cursor(block);
    cursor.setPosition(block.position() + offset + column);
    rig.editor.setTextCursor(cursor);
    QTest::qWait(10);
}

void checkTableShiftEnter() {
    // Из середины ряда тела: ряд целиком остаётся, пустой ряд под ним, каретка
    // в первой ячейке; набор ложится в неё; Esc сворачивает; файл получает ряд.
    {
        Rig rig("ряд.md", QString::fromUtf8(kTableNote));
        check(openTableIn(rig), "таблица раскрыта четырьмя строками");
        caretAtLine(rig, 2, 3);   // «| 1| 2 |» — каретка после «| 1»
        QTest::keyClick(&rig.editor, Qt::Key_Return, Qt::ShiftModifier);
        QTest::qWait(20);
        const QStringList lines = linesOf(rig.block(2));
        check(lines.size() == 5 && lines.at(2) == QStringLiteral("| 1 | 2 |") &&
                  lines.at(3) == QStringLiteral("|  |  |") &&
                  lines.at(4) == QStringLiteral("| 3 | 4 |"),
              "пустой ряд встал под рядом каретки целиком: " +
                  lines.join(QLatin1Char('/')).toStdString());
        const QTextBlock block = rig.block(2);
        const int rowStart = int(lines.at(0).size() + lines.at(1).size() + lines.at(2).size()) + 3;
        check(rig.editor.textCursor().position() == block.position() + rowStart + 2,
              "каретка — в первой ячейке нового ряда");
        QTest::keyClicks(&rig.editor, QStringLiteral("x"));
        QTest::qWait(10);
        check(linesOf(rig.block(2)).value(3) == QStringLiteral("| x |  |"),
              "набор лёг в первую ячейку: " + linesOf(rig.block(2)).value(3).toStdString());
        QTest::keyClick(&rig.editor, Qt::Key_Escape);
        QTest::qWait(20);
        check(zametti::isTableObjectBlock(rig.block(2)), "Esc свернул таблицу объектом");
        const zametti::Table table = zametti::parseTable(zametti::tableSourceOf(rig.block(2)));
        check(table.valid && table.columns == 2 && table.bodyRows() == 3, "три ряда тела");
        rig.editor.save(false, true);
        QTest::qWait(10);
        check(readFile(rig.path).contains(QStringLiteral("| 1 | 2 |\n| x |  |\n| 3 | 4 |")),
              "ряд дошёл до файла: " + readFile(rig.path).toStdString());
    }

    // С шапки и с разделителя ряд идёт ПОСЛЕ разделителя.
    for (int line = 0; line < 2; ++line) {
        Rig rig(line == 0 ? "шапка.md" : "разделитель.md", QString::fromUtf8(kTableNote));
        check(openTableIn(rig), "таблица раскрыта");
        caretAtLine(rig, line, 2);
        QTest::keyClick(&rig.editor, Qt::Key_Return, Qt::ShiftModifier);
        QTest::qWait(20);
        const QStringList lines = linesOf(rig.block(2));
        check(lines.size() == 5 && lines.at(1) == QStringLiteral("|---|---|") &&
                  lines.at(2) == QStringLiteral("|  |  |") &&
                  lines.at(3) == QStringLiteral("| 1 | 2 |"),
              std::string(line == 0 ? "с шапки" : "с разделителя") +
                  " ряд встал первым рядом тела: " + lines.join(QLatin1Char('/')).toStdString());
    }

    // В конце последнего ряда — ряд, а не абзац под таблицей.
    {
        Rig rig("хвост.md", QString::fromUtf8(kTableNote));
        check(openTableIn(rig), "таблица раскрыта");
        caretAtLine(rig, 3, 9);   // конец «| 3 | 4 |»
        check(rig.editor.textCursor().atBlockEnd(), "каретка в конце блока");
        QTest::keyClick(&rig.editor, Qt::Key_Return, Qt::ShiftModifier);
        QTest::qWait(20);
        const QStringList lines = linesOf(rig.block(2));
        check(lines.size() == 5 && lines.at(4) == QStringLiteral("|  |  |"),
              "в конце последнего ряда — новый ряд: " + lines.join(QLatin1Char('/')).toStdString());
        check(zametti::kindOf(rig.block(3)) == zametti::Kind::VSpace &&
                  rig.block(4).text() == QStringLiteral("ниже"),
              "абзаца под таблицей не завелось");
    }

    // Отмена: первый Ctrl+Z снимает сворачивание, сделанное сохранением, второй
    // — сам ряд (тот же конвейер, что у формул и шапки).
    {
        Rig rig("отмена4.md", QString::fromUtf8(kTableNote));
        check(openTableIn(rig), "таблица раскрыта");
        caretAtLine(rig, 2, 3);
        QTest::qWait(60);
        QTest::keyClick(&rig.editor, Qt::Key_Return, Qt::ShiftModifier);
        QTest::qWait(10);
        QTest::keyClick(&rig.editor, Qt::Key_Z, Qt::ControlModifier);
        QTest::qWait(10);
        QTest::keyClick(&rig.editor, Qt::Key_Z, Qt::ControlModifier);
        QTest::qWait(10);
        const QString source = zametti::sourceTextOf(rig.block(2));
        check(!source.contains(QStringLiteral("|  |  |")) && source.count(QLatin1Char('\n')) <= 4,
              "Ctrl+Z убрал ряд: " + source.toStdString());
    }

    // Отказы. Блок кода: Shift+Enter в конце — по-прежнему абзац под блоком.
    {
        Rig rig("код.md", QStringLiteral("```\n| a | b |\n|---|---|\n```\n"));
        rig.toBlockEnd(0);
        QTest::keyClick(&rig.editor, Qt::Key_Return, Qt::ShiftModifier);
        QTest::qWait(20);
        check(zametti::kindOf(rig.block(0)) == zametti::Kind::Code &&
                  linesOf(rig.block(0)).size() == 2,
              "в коде ряд не вставляется: " + linesOf(rig.block(0)).join(QLatin1Char('/')).toStdString());
        // Абзац под кодом — сразу или через пустую строку шва.
        std::string kinds;
        bool paragraphBelow = false;
        for (int i = 1; i <= 2; ++i) {
            const QTextBlock b = rig.block(i);
            if (!b.isValid()) break;
            kinds += std::to_string(int(zametti::kindOf(b))) + (zametti::isRawBlock(b) ? "r " : " ");
            if (!zametti::isRawBlock(b) && zametti::kindOf(b) == zametti::Kind::Paragraph)
                paragraphBelow = true;
        }
        check(paragraphBelow, "а Shift+Enter в конце кода дал абзац под ним: " + kinds);
    }
    // Дословный блок, который таблицей не читается: строка внутри блока, палок нет.
    {
        Rig rig("html.md", QStringLiteral("<div>\nтекст\n</div>\n"));
        check(zametti::isRawBlock(rig.block(0)), "html — дословный блок");
        caretAtLine(rig, 0, 0);
        QTextCursor cursor(rig.block(0));
        cursor.setPosition(rig.block(0).position() + 2);
        rig.editor.setTextCursor(cursor);
        QTest::keyClick(&rig.editor, Qt::Key_Return, Qt::ShiftModifier);
        QTest::qWait(20);
        check(!rig.block(0).text().contains(QLatin1Char('|')),
              "не-таблица ряда не получает: " + rig.block(0).text().toStdString());
    }
    // Свёрнутая таблица-объект: Shift+Enter её не раскрывает и не меняет.
    {
        Rig rig("объект.md", QString::fromUtf8(kTableNote));
        QTextCursor cursor(rig.block(2));
        rig.editor.setTextCursor(cursor);
        QTest::qWait(10);
        QTest::keyClick(&rig.editor, Qt::Key_Return, Qt::ShiftModifier);
        QTest::qWait(20);
        bool same = false;
        for (QTextBlock b = rig.editor.document()->begin(); b.isValid(); b = b.next())
            if (zametti::isTableObjectBlock(b) &&
                zametti::tableSourceOf(b).count(QLatin1Char('\n')) <= 4 &&
                !zametti::tableSourceOf(b).contains(QStringLiteral("|  |  |")))
                same = true;
        check(same, "объект остался объектом с прежним исходником");
    }
}

// --- (д) Shift+Enter не оставляет U+2028 там, где заметка отказала ----------
//
// Заголовок многострочным не бывает: Shift+Enter в нём режет блок (хвост —
// абзац). Qt-шный InsertLineSeparator, в который нажатие проваливалось бы при
// отказе заметки, вставлял бы разделитель БЕЗ пометки — и он уезжал в файл
// собой (долг отчёта 04.09.2026). Разделителя в документе быть не должно ни
// в одном блоке.
bool anyLineSeparator(const zametti::NoteEditor& editor) {
    for (QTextBlock b = editor.document()->begin(); b.isValid(); b = b.next())
        if (b.text().contains(QChar::LineSeparator) && zametti::kindOf(b) == zametti::Kind::Heading)
            return true;
    return false;
}

void checkHeadingShiftEnter() {
    {
        Rig rig("заголовок.md", QStringLiteral("# Заголовок\n\nтекст\n"), 0);
        QTest::keyClick(&rig.editor, Qt::Key_Return, Qt::ShiftModifier);
        QTest::qWait(20);
        check(!anyLineSeparator(rig.editor) &&
                  zametti::kindOf(rig.block(0)) == zametti::Kind::Heading &&
                  rig.block(0).text() == QStringLiteral("Заголовок"),
              "Shift+Enter в конце заголовка не оставил разделителя: " +
                  rig.block(0).text().toStdString());
    }
    {
        Rig rig("заголовок2.md", QStringLiteral("# Заголовок\n\nтекст\n"), 0);
        QTextCursor cursor(rig.block(0));
        cursor.setPosition(rig.block(0).position() + 4);
        rig.editor.setTextCursor(cursor);
        QTest::keyClick(&rig.editor, Qt::Key_Return, Qt::ShiftModifier);
        QTest::qWait(20);
        check(!anyLineSeparator(rig.editor) && rig.block(0).text() == QStringLiteral("Заго"),
              "Shift+Enter в середине заголовка разрезал его: " + rig.block(0).text().toStdString());
        QTest::keyClicks(&rig.editor, QStringLiteral("x"));
        QTest::qWait(10);
        rig.editor.save(false, true);
        QTest::qWait(10);
        const QString file = readFile(rig.path);
        check(!file.contains(QChar::LineSeparator) && file.contains(QStringLiteral("# Заго\n")),
              "в файле заголовок без разделителя: " + file.toStdString());
    }
}

}  // namespace

static int ztRunSuite(int argc, char** argv) {
    if (argc < 2) {
        std::printf("использование: type_rules_test <каталог для временных файлов>\n");
        return 2;
    }
    zametti::mutableSettingsForTests().editor().setUndoCoalesceMs(40);

    g_dir = QString::fromLocal8Bit(argv[1]) + QStringLiteral("/type-rules-data");
    zt::dropTree(g_dir);
    if (!QDir().mkpath(g_dir)) {
        std::printf("не создать каталог %s\n", g_dir.toUtf8().constData());
        return 2;
    }

    checkInlineDollar();
    checkDisplayDollar();
    checkTableEnter();
    checkTableShiftEnter();
    checkHeadingShiftEnter();

    std::printf("проверок %d, провалов %d\n", zt::g_checks, zt::g_failures);
    return zt::freshFailures();
}

TEST(TypeRules, All) {
    std::vector<QByteArray> ztArgs{QByteArrayLiteral("type_rules_test")};
    ztArgs.push_back((zt::TestData::outDir(QStringLiteral("type-rules"))).toLocal8Bit());
    std::vector<char*> ztArgv;
    for (QByteArray& a : ztArgs) ztArgv.push_back(a.data());
    EXPECT_EQ(0, ztRunSuite(int(ztArgv.size()), ztArgv.data()));
}
