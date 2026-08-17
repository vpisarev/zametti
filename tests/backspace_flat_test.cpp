// Плоский фаззер Backspace.
//
// Эталон — стародавний плоский буфер: текст это последовательность символов,
// перевод строки тоже символ, и Backspace удаляет символ слева от каретки.
// Точка. Документ редактора проецируется в такой буфер (маркеры пунктов — не
// символы, их рисует вид; тематическая черта — один символ вместе со своим
// переводом строки), нажатие делается по-настоящему, и результат обязан
// совпасть с эталоном — и текст, и каретка.
//
// Белый список отклонений — ровно два пункта, оба согласованы с владельцем:
//   1) Backspace в начале ПЕРВОГО пункта своего списка (выше не пункт того же
//      вида и уровня) — жест «снять маркер»: буфер не меняется, каретка на
//      месте. Не-первый пункт — обычное удаление '\n': текст приклеивается к
//      предыдущему пункту, маркер испаряется, потому что он не символ.
//   2) Удалить '\n' слева от черты, когда строка выше непустая, нельзя: черта
//      не живёт в строке текста. Отказ; каретка шагает в конец строки выше.
//
// Документы псевдослучайные с фиксированным зерном: каждый запуск гоняет одни
// и те же. При провале печатается репро: документ, позиция, «ждали/вышло» с
// пронумерованными переводами строк.

#include "doc_model.h"
#include "editor_widget.h"
#include "settings.h"

#include "test_util.h"

#include <vector>
#include "testdata.h"

#include <QApplication>
#include <QTest>
#include <QTextBlock>
#include <QTextCursor>

#include <filesystem>
#include <fstream>
#include <random>
#include <string>

namespace fs = std::filesystem;

namespace {

// Черта в плоском буфере — один символ (вместе со своим переводом строки).
const QChar kDivider = QChar(0x00A7);   // '§', в настоящих текстах не участвует

fs::path g_note;
fs::path g_dir;

// Проекция документа в плоский буфер.
QString flatten(const QTextDocument& doc) {
    QString out;
    for (QTextBlock block = doc.begin(); block.isValid(); block = block.next()) {
        if (!zametti::isRawBlock(block) && zametti::kindOf(block) == zametti::Kind::Divider) {
            out += kDivider;
            continue;
        }
        QString text = block.text();
        text.replace(QChar::LineSeparator, QLatin1Char('\n'));
        out += text;
        out += QLatin1Char('\n');
    }
    // Последний перевод строки — за краем документа, каретке там не бывать.
    if (out.endsWith(QLatin1Char('\n'))) out.chop(1);
    return out;
}

// Позиция каретки в плоском буфере.
int flattenCaret(const QTextDocument& doc, const QTextCursor& cursor) {
    int at = 0;
    for (QTextBlock block = doc.begin(); block.isValid(); block = block.next()) {
        const bool divider =
            !zametti::isRawBlock(block) && zametti::kindOf(block) == zametti::Kind::Divider;
        if (block.blockNumber() == cursor.blockNumber())
            return at + (divider ? 0 : cursor.positionInBlock());
        at += divider ? 1 : block.length();   // length учитывает разделитель блока
    }
    return at;
}

// Первый ли это пункт своего списка (выше не пункт того же вида и уровня).
bool firstOfItsList(const QTextBlock& block) {
    if (!zametti::isListBlock(block)) return false;
    const QTextBlock prev = block.previous();
    return !(prev.isValid() && zametti::isListBlock(prev) &&
             zametti::levelOf(prev) == zametti::levelOf(block) &&
             zametti::isOrderedBlock(prev) == zametti::isOrderedBlock(block) &&
             zametti::isTaskBlock(prev) == zametti::isTaskBlock(block));
}

// Эталон: что обязан дать Backspace на этом буфере в этой позиции.
struct Expected {
    QString text;
    int caret;
};

Expected oracle(const QString& flat, int caret, bool firstItemGesture, bool lineAboveDivider) {
    if (caret <= 0) return {flat, 0};                       // упёрлись в начало
    if (firstItemGesture) return {flat, caret};             // жест: буфер неизменен

    const QChar left = flat.at(caret - 1);
    if (left == QLatin1Char('\n') && lineAboveDivider) {
        // Черте не жить в строке текста: отказ, каретка в конец строки выше.
        return {flat, caret - 1};
    }
    QString cut = flat;
    cut.remove(caret - 1, 1);
    return {cut, caret - 1};
}

QString visible(const QString& flat) {
    QString out;
    int newline = 0;
    for (const QChar& c : flat) {
        if (c == QLatin1Char('\n')) out += QStringLiteral("\\n%1").arg(++newline);
        else if (c == kDivider) out += QStringLiteral("[___]");
        else out += c;
    }
    return out;
}

// Одно нажатие с проверкой. Возвращает false, чтобы серия остановилась на
// первом расхождении — дальше сравнивать бессмысленно.
bool pressAndCheck(zametti::NoteEditor& editor, const QString& source, const char* how) {
    const QString before = flatten(*editor.document());
    const int caret = flattenCaret(*editor.document(), editor.textCursor());

    const QTextBlock block = editor.textCursor().block();
    const bool gesture = editor.textCursor().atBlockStart() && firstOfItsList(block);
    // Строка выше каретки — черта, а строка каретки непуста? Тогда '\n' слева
    // неудаляем... наоборот: каретка стоит В НАЧАЛЕ строки-черты, а выше —
    // непустой текст. Считаем по буферу: слева '\n', а上 позиции caret стоит
    // черта.
    const bool caretOnDivider = caret < before.size() && before.at(caret) == kDivider &&
                                (caret == 0 || before.at(caret - 1) == QLatin1Char('\n'));
    const bool lineAboveNonEmpty =
        caretOnDivider && caret >= 2 && before.at(caret - 2) != QLatin1Char('\n') &&
        before.at(caret - 2) != kDivider;
    const Expected want = oracle(before, caret, gesture, caretOnDivider && lineAboveNonEmpty);

    QTest::keyClick(&editor, Qt::Key_Backspace);

    const QString after = flatten(*editor.document());
    const int landed = flattenCaret(*editor.document(), editor.textCursor());
    if (after == want.text && landed == want.caret) {
        ++zt::g_checks;
        return true;
    }
    ++zt::g_checks;
    ++zt::g_failures;
    std::printf(
        "FAIL %s\n  документ: %s\n  каретка:  %d\n  ждали:  «%s» каретка %d\n"
        "  вышло:  «%s» каретка %d\n",
        how, visible(before).toUtf8().constData(), caret,
        visible(want.text).toUtf8().constData(), want.caret,
        visible(after).toUtf8().constData(), landed);
    std::printf("  исходник:\n%s", source.toUtf8().constData());
    return false;
}

void openSource(zametti::NoteEditor& editor, const QString& source) {
    // КАЖДЫЙ СЛУЧАЙ — СВОЙ ФАЙЛ, и это не аккуратность ради аккуратности.
    //
    // Прежде все случаи шли через один файл, а правки предыдущего случая
    // выбрасывались через setModified(false). Выбросить их больше нельзя:
    // уходя из заметки, редактор пишет её НЕ СПРАШИВАЯ этот признак (страховка
    // от нашей же ошибки в расстановке признака — решение владельца). Оставь
    // тут один файл — и запись предыдущего случая ложилась бы поверх
    // исходника следующего.
    static int counter = 0;
    g_note = g_dir / ("ф" + std::to_string(++counter) + ".md");
    {
        std::ofstream out(g_note, std::ios::binary);
        const QByteArray bytes = source.toUtf8();
        out.write(bytes.constData(), bytes.size());
    }
    editor.openFile(QString::fromStdString(g_note.string()));
}

}  // namespace

static int ztRunSuite(int argc, char** argv) {
    (void)argc;
    (void)argv;
    zametti::loadSettings(nullptr);
    const fs::path dir = fs::temp_directory_path() / "zametti-backspace-flat";
    fs::create_directories(dir);
    g_dir = dir;
    g_note = dir / "ф.md";

    zametti::NoteEditor editor;
    editor.resize(700, 500);
    editor.show();
    QTest::qWait(20);
    editor.setFocus();

    // Атомы. Кириллица нарочно: смещения в единицах UTF-16 против байтов —
    // любимое место расхождений.
    const auto atom = [](std::mt19937& rng, int i) -> QString {
        switch (rng() % 8) {
            case 0: return QStringLiteral("строка %1\n").arg(i);
            case 1: return QStringLiteral("\n");
            case 2: return QStringLiteral("___\n");
            case 3: return QStringLiteral("- пункт %1\n").arg(i);
            case 4: return QStringLiteral("  - вложенный %1\n").arg(i);
            case 5: return QStringLiteral("1. номер %1\n").arg(i);
            case 6: return QStringLiteral("- [ ] дело %1\n").arg(i);
            default: return QStringLiteral("## глава %1\n").arg(i);
        }
    };

    std::mt19937 rng(20260730);
    const int kDocs = 120;
    for (int doc = 0; doc < kDocs; ++doc) {
        QString source;
        const int atoms = 2 + int(rng() % 6);
        for (int i = 0; i < atoms; ++i) source += atom(rng, i);

        // Случайные одиночные нажатия по случайным местам.
        for (int trial = 0; trial < 6; ++trial) {
            openSource(editor, source);
            const int characters = editor.document()->characterCount() - 1;
            if (characters <= 0) break;
            QTextCursor cursor(editor.document());
            cursor.setPosition(int(rng() % uint32_t(characters + 1)));
            editor.setTextCursor(cursor);
            pressAndCheck(editor, source, "одиночное");
        }

        // Серия с конца до упора: каждый шаг сверяется с эталоном.
        openSource(editor, source);
        QTextCursor cursor(editor.document());
        cursor.movePosition(QTextCursor::End);
        editor.setTextCursor(cursor);
        for (int press = 0; press < 40; ++press) {
            if (flattenCaret(*editor.document(), editor.textCursor()) == 0) break;
            if (!pressAndCheck(editor, source, "серия с конца")) break;
        }
        editor.document()->setModified(false);
        if (zt::g_failures >= 10) break;   // первых расхождений хватит
    }

    return zt::report("плоский Backspace");
}

// Набор целиком одним TEST: тело не тронуто, argv ему собран здесь.
// Дробить на отдельные проверки — отдельная работа, по одному набору.
TEST(BackspaceFlat, All) {
    std::vector<QByteArray> ztArgs{QByteArrayLiteral("backspace_flat_test")};
    std::vector<char*> ztArgv;
    for (QByteArray& a : ztArgs) ztArgv.push_back(a.data());
    EXPECT_EQ(0, ztRunSuite(int(ztArgv.size()), ztArgv.data()));
}
