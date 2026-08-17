// Фаззер уборки: случайные правки ЛЮБОГО вида вперемешку со случайными
// перемещениями каретки. После каждого действия — инвариант: ни в одной
// строке документа нет хвостовых пробелов, кроме строки, на которой стоит
// каретка. Какая именно правка испортила строку, инварианту безразлично —
// ровно так правило и сформулировано владельцем.
//
// Зерно фиксировано: каждый запуск гоняет одни и те же сценарии.

#include "doc_model.h"
#include "editor_ops.h"
#include "editor_widget.h"
#include "settings.h"

#include "test_util.h"
#include "testdata.h"

#include <QApplication>
#include <QClipboard>
#include <QGuiApplication>
#include <QTest>
#include <QTextBlock>
#include <QTextCursor>

#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static int ztRunSuite(int argc, char** argv) {
    (void)argc;
    (void)argv;
    zametti::loadSettings(nullptr);
    const fs::path dir = fs::temp_directory_path() / "zametti-tidy-fuzz";
    fs::create_directories(dir);
    const fs::path note = dir / "т.md";

    zametti::NoteEditor editor;
    editor.resize(700, 500);
    editor.show();
    QTest::qWait(20);
    editor.setFocus();

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

    std::mt19937 rng(20260731);
    const int kDocs = 60;
    for (int doc = 0; doc < kDocs && zt::g_failures < 8; ++doc) {
        QString source;
        const int atoms = 2 + int(rng() % 6);
        for (int i = 0; i < atoms; ++i) source += atom(rng, i);
        {
            std::ofstream out(note, std::ios::binary);
            const QByteArray bytes = source.toUtf8();
            out.write(bytes.constData(), bytes.size());
        }
        editor.document()->setModified(false);
        editor.openFile(QString::fromStdString(note.string()));

        std::vector<std::string> steps;
        const auto act = [&](int pick) -> const char* {
            QTextCursor cursor(editor.document());
            const int characters = editor.document()->characterCount() - 1;
            switch (pick) {
                case 0:
                    // Кириллицу QTest клавишами не умеет: вставка. Путь
                    // contentsChanged она проходит тот же.
                    editor.insertPlainText(QStringLiteral("сло"));
                    return "набор";
                case 1:
                    QTest::keyClick(&editor, Qt::Key_Space);
                    QTest::keyClick(&editor, Qt::Key_Space);
                    return "пробелы";
                case 2:
                    QTest::keyClick(&editor, Qt::Key_Backspace);
                    return "backspace";
                case 3:
                    QTest::keyClick(&editor, Qt::Key_Delete);
                    return "delete";
                case 4:
                    QTest::keyClick(&editor, Qt::Key_Return);
                    return "enter";
                case 5:
                    QTest::keyClick(&editor, Qt::Key_End, Qt::ShiftModifier);
                    QTest::keyClick(&editor, Qt::Key_Delete);
                    return "shift-end del";
                case 6: {
                    // Случайное выделение и удаление.
                    if (characters <= 1) return "пусто";
                    cursor.setPosition(int(rng() % uint32_t(characters)));
                    const int to = int(rng() % uint32_t(characters));
                    cursor.setPosition(to, QTextCursor::KeepAnchor);
                    editor.setTextCursor(cursor);
                    QTest::keyClick(&editor, Qt::Key_Delete);
                    return "выделение del";
                }
                case 7: {
                    if (characters <= 1) return "пусто";
                    cursor.setPosition(int(rng() % uint32_t(characters)));
                    editor.setTextCursor(cursor);
                    return "прыжок";
                }
                case 8:
                    QTest::keyClick(&editor, Qt::Key_Down);
                    return "вниз";
                case 9:
                    QTest::keyClick(&editor, Qt::Key_Up);
                    return "вверх";
                case 10:
                    QTest::keyClick(&editor, Qt::Key_Home);
                    return "home";
                default:
                    QTest::keyClick(&editor, Qt::Key_End);
                    return "end";
            }
        };

        for (int step = 0; step < 25 && zt::g_failures < 8; ++step) {
            steps.push_back(act(int(rng() % 12)));
            const QString problem =
                zametti::tidyProblem(*editor.document(), editor.textCursor());
            ++zt::g_checks;
            if (problem.isEmpty()) continue;
            ++zt::g_failures;
            std::string trail;
            for (const std::string& s : steps) trail += s + ", ";
            QString blocks;
            for (QTextBlock b = editor.document()->begin(); b.isValid(); b = b.next())
                blocks += QStringLiteral("[k%1 «%2»]")
                              .arg(int(zametti::kindOf(b)))
                              .arg(QString(b.text()).replace(QChar::LineSeparator,
                                                             QStringLiteral("↵")));
            std::printf(
                "FAIL уборка: %s\n  шаги: %s\n  каретка: блок %d смещ %d\n  блоки: %s\n"
                "  исходник:\n%s\n",
                problem.toUtf8().constData(), trail.c_str(),
                editor.textCursor().blockNumber(), editor.textCursor().positionInBlock(),
                blocks.toUtf8().constData(), source.toUtf8().constData());
            break;
        }
        editor.document()->setModified(false);
    }

    return zt::report("фаззер уборки");
}

// Набор целиком одним TEST: тело не тронуто, argv ему собран здесь.
// Дробить на отдельные проверки — отдельная работа, по одному набору.
TEST(TidyFuzz, All) {
    std::vector<QByteArray> ztArgs{QByteArrayLiteral("tidy_fuzz_test")};
    std::vector<char*> ztArgv;
    for (QByteArray& a : ztArgs) ztArgv.push_back(a.data());
    EXPECT_EQ(0, ztRunSuite(int(ztArgv.size()), ztArgv.data()));
}
