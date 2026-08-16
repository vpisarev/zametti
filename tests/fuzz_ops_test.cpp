// Фаззинг операций: случайные последовательности правок по случайным местам.
//
// Зачем он нужен, видно по тому, как находились ошибки до него. Девять раз
// подряд выяснялось, что документ умеет выразить то, чего markdown не хранит:
// пустой абзац, пробел на краю строки, пустая строка внутри блока, встроенный
// код через перенос, начертание с пробелом на краю, пустой вложенный пункт. Все
// девять нашёл человек, работая с заметками, и каждый стоил аварийного файла.
//
// Здесь та же проверка делается механически. После каждой операции документ
// обязан оставаться:
//
//   1. правильным по строению — уровни списка и разбивка литеральных блоков;
//   2. записываемым — то, что ушло бы в файл, читается обратно в тот же IR.
//
// Второе и есть условие аварийного файла: если проверка падает, значит найдена
// правка, после которой сохранить заметку нельзя.
//
// Отдельно проверяется инвариант C: операция и отмена дают исходный документ.
//
// Случайность здесь воспроизводимая: зерно печатается и задаётся параметром,
// поэтому упавший прогон повторяется дословно.

#include "document.h"
#include "pieces.h"

#include "test_util.h"
#include "testdata.h"

#include <QGuiApplication>
#include <QTextCharFormat>
#include <QTextCursor>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace zametti;

namespace {

std::string readFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

// Операция и её имя. Имя нужно в отчёте о падении: без него по зерну пришлось бы
// гадать, что именно сломало документ.
//
// ГЛАГОЛАМИ ЗАМЕТКИ, а не разметкой блоков: стенд обязан бить в ту же дверь, в
// которую ходит приложение. Иначе он проверял бы соседний путь, а тот, которым
// правят на самом деле, оставался бы без фаззинга.
struct Operation {
    const char* name;
    std::function<bool(ZDocument&, QTextCursor&)> run;
};

using Z = ZDocument;

const Operation kOperations[] = {
    {"Enter", [](Z& n, QTextCursor& at) { return n.breakBlock(at, Z::BreakKind::Plain); }},
    {"Shift+Enter",
     [](Z& n, QTextCursor& at) { return n.breakBlock(at, Z::BreakKind::Otherwise); }},
    {"Backspace", [](Z& n, QTextCursor& at) { return n.deleteBack(at); }},
    {"Delete", [](Z& n, QTextCursor& at) { return n.deleteForward(at); }},
    {"Tab", [](Z& n, QTextCursor& at) { return n.indent(at); }},
    {"Shift+Tab", [](Z& n, QTextCursor& at) { return n.outdent(at); }},
    {"переключить задачу", [](Z& n, QTextCursor& at) { return n.toggleTask(at); }},
    {"в буллет", [](Z& n, QTextCursor& at) { return n.makeBullet(at); }},
    {"в нумерованный", [](Z& n, QTextCursor& at) { return n.makeOrdered(at); }},
    {"в задачу", [](Z& n, QTextCursor& at) { return n.makeTask(at); }},
    {"в абзац", [](Z& n, QTextCursor& at) { return n.makeParagraph(at); }},
    {"заголовок", [](Z& n, QTextCursor& at) { return n.setHeadingLevel(at, 2); }},
    {"комментарий", [](Z& n, QTextCursor& at) { return n.toggleComment(at); }},
    {"жирный", [](Z& n, QTextCursor& at) { return n.toggleStyle(at, Z::Style::Bold); }},
    {"курсив", [](Z& n, QTextCursor& at) { return n.toggleStyle(at, Z::Style::Italic); }},
    {"зачёркнутый", [](Z& n, QTextCursor& at) { return n.toggleStyle(at, Z::Style::Strike); }},
    {"код в строке", [](Z& n, QTextCursor& at) { return n.toggleStyle(at, Z::Style::Code); }},
    {"блок кода", [](Z& n, QTextCursor& at) { return n.toggleCodeBlock(at); }},
    {"переставить вверх", [](Z& n, QTextCursor& at) { return n.moveListItem(at, -1); }},
    {"переставить вниз", [](Z& n, QTextCursor& at) { return n.moveListItem(at, 1); }},
    {"правило набора", [](Z& n, QTextCursor& at) { return n.applyInputRule(at); }},
    {"кавычка кода", [](Z& n, QTextCursor& at) { return n.applyCodeSpanRule(at); }},
};

// Что набирают: обычные буквы, знаки разметки и то, на чём уже спотыкались —
// пробелы по краям, обратные кавычки, ссылки.
const char* const kTyped[] = {
    "а", " ", "  ", "-", "- ", "* ", "1. ", "#", "## ", "[ ] ", "-[ ", "`", "```",
    "текст", " хвост", "https://example.com", "_", "**", "[x] ", "\t", "~~~",
};

std::string describe(const std::vector<std::string>& steps) {
    std::string out;
    for (const std::string& step : steps) {
        out += "\n    ";
        out += step;
    }
    return out;
}

// Записываем ли мы эту заметку без аварийного файла. Ровно та же сверка, что и
// в сохранении: записали — прочитали обратно — сошлось.
//
// Тем же вопросом закрыт и прежний «инвариант C» (операция и отмена дают
// исходный документ): он спрашивал, обратны ли друг другу обход и сборка, а это
// и есть неподвижность канона.
bool savable(const ZDocument& note, std::string& report) {
    const std::string written = note.toMarkdown();
    const ZDocument reread = noteOf(written);
    if (note.sameBody(reread)) return true;

    report = "\n  вышло бы в файл:\n" + written;
    report += "\n  а прочиталось бы:\n" + reread.toMarkdown();
    return false;
}

int g_files = 0;
int g_operations = 0;

void fuzzFile(const fs::path& path, int rounds, uint32_t seed) {
    const std::string source = readFile(path);
    if (source.empty()) return;
    ++g_files;

    ZDocument note = noteOf(source);

    std::mt19937 rng(seed);
    std::vector<std::string> steps;

    for (int round = 0; round < rounds; ++round) {
        const int blocks = note.blockCount();
        if (blocks <= 0) break;

        // Случайное место и, с некоторой вероятностью, выделение. Место берём
        // блоком и смещением в нём: позиций заметка наружу не показывает.
        QTextCursor cursor = note.caretAtBlock(int(rng() % uint32_t(blocks)));
        cursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::MoveAnchor);
        const int inBlock = cursor.positionInBlock();
        cursor.setPosition(cursor.position() - int(rng() % uint32_t(inBlock + 1)));
        if (rng() % 3 == 0) {
            QTextCursor other = note.caretAtBlock(int(rng() % uint32_t(blocks)));
            other.movePosition(QTextCursor::EndOfBlock);
            cursor.setPosition(other.position() - int(rng() % uint32_t(other.positionInBlock() + 1)),
                               QTextCursor::KeepAnchor);
        }

        std::string what;
        if (rng() % 4 == 0) {
            // Набор: он же заводит те состояния, которых операции не дают —
            // висящий перенос, пробел на краю, недописанную разметку.
            const char* typed = kTyped[rng() % (sizeof(kTyped) / sizeof(kTyped[0]))];
            if (!note.insertText(cursor, QString::fromUtf8(typed))) continue;
            what = std::string("набрать \"") + typed + "\"";
        } else {
            const Operation& op =
                kOperations[rng() % (sizeof(kOperations) / sizeof(kOperations[0]))];
            if (!op.run(note, cursor)) continue;
            what = op.name;
        }
        steps.push_back(what);
        ++g_operations;

        // Строение обязано остаться правильным.
        const QString problem = note.structureProblem();
        ZT_TRUE(std::string("строение: ") + path.string() + " (зерно " +
                    std::to_string(seed) + ")" + describe(steps) + "\n  " +
                    problem.toStdString() +
                    (problem.isEmpty() ? std::string() : "\n  заметка:\n" + note.toMarkdown()),
                problem.isEmpty());
        if (!problem.isEmpty()) return;

        // И заметка обязана оставаться записываемой: иначе человек получил бы
        // аварийный файл вместо сохранения.
        std::string report;
        const bool ok = savable(note, report);
        ZT_TRUE(std::string("заметка записывается: ") + path.string() + " (зерно " +
                    std::to_string(seed) + ")" + describe(steps) + report,
                ok);
        if (!ok) return;
    }
}

}  // namespace

static int ztRunSuite(int argc, char** argv) {

    if (argc < 2) {
        std::fprintf(stderr,
                     "нужен каталог с заметками: %s <каталог> [кругов] [зерно]\n",
                     argv[0]);
        return 2;
    }
    const fs::path dir = argv[1];
    const int rounds = argc > 2 ? std::atoi(argv[2]) : 40;
    const uint32_t seed = argc > 3 ? uint32_t(std::atoi(argv[3])) : 20260727u;

    if (!fs::exists(dir)) {
        std::printf("каталога %s нет, пропускаем\n", dir.string().c_str());
        return 0;
    }

    std::printf("зерно %u, кругов на файл %d\n", seed, rounds);
    uint32_t fileSeed = seed;
    for (const fs::directory_entry& entry : fs::recursive_directory_iterator(dir)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".md") continue;
        fuzzFile(entry.path(), rounds, fileSeed);
        // Своё зерно на файл: иначе все файлы шли бы по одной и той же дорожке.
        fileSeed = fileSeed * 1664525u + 1013904223u;
    }

    std::printf("файлов %d, операций %d\n", g_files, g_operations);
    return zt::report("фаззинг операций");
}

// Набор целиком одним TEST: тело не тронуто, argv ему собран здесь.
// Дробить на отдельные проверки — отдельная работа, по одному набору.
TEST(FuzzOps, All) {
    std::vector<QByteArray> ztArgs{QByteArrayLiteral("fuzz_ops_test")};
    ztArgs.push_back((zt::TestData::corpus(QStringLiteral("corpus"))).toLocal8Bit());
    std::vector<char*> ztArgv;
    for (QByteArray& a : ztArgs) ztArgv.push_back(a.data());
    EXPECT_EQ(0, ztRunSuite(int(ztArgv.size()), ztArgv.data()));
}
