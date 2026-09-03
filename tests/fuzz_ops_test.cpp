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

#include <QFile>
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

QString g_scratch;   // куда фаззер пишет заметку по-настоящему

// ЗАПИСЫВАЕТСЯ ЛИ ЗАМЕТКА БЕЗ АВАРИЙНОГО ФАЙЛА — спрошено НАСТОЯЩЕЙ ЗАПИСЬЮ, той
// же, какой пишет приложение (ZDocument::saveTo). Своей сверки здесь больше нет:
// пока она была своей, она отвечала на СОСЕДНИЙ вопрос — «совпадут ли байты
// побитово», — а запись такого не обещает и обещать не может. Голую ссылку
// человек набирает текстом, файл читает её ссылкой, и запрещать это значило бы
// запретить писать ссылки (набор Save держит это отдельным случаем).
//
// Условие аварийного файла у записи одно: перечитанное разошлось СТРОЕНИЕМ ИЛИ
// ТЕКСТОМ. Тогда рядом ложится копия буфера, и человеку говорится, что не
// сошлось; это и есть «сохранить заметку нельзя».
//
// И ВТОРОЕ, ЧЕГО ЗАПИСЬ ОБЯЗАНА ДЕРЖАТЬ: файл должен УСТОЯТЬСЯ. Открыть
// записанное и записать снова — значит не тронуть файл вовсе (Unchanged). Иначе
// заметка меняется на диске сама, от одного открытия, отпечаток пляшет, а
// история копит слепки без единой правки человека.
int g_saved = 0;

// Байты падения — ФАЙЛОМ РЯДОМ, а не только в отчёте. По тексту в отчёте случай
// не сузишь: он длинный, а сужать его надо инструментом (mddump --file).
void keepFailure(const std::string& text) {
    const QString path =
        g_scratch + QStringLiteral("/случай-%1.md").arg(++g_saved, 3, 10, QLatin1Char('0'));
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return;
    file.write(text.data(), qint64(text.size()));
}

bool savable(ZDocument& note, std::string& report) {
    const QString path = g_scratch + QStringLiteral("/фаззер.md");
    QFile::remove(path);
    const SaveOutcome first = note.saveTo(path, QStringLiteral("fuzz"));
    if (first.result == SaveResult::Failed) {
        report = "\n  запись не удалась: " + first.message.toStdString();
        return false;
    }
    if (!first.rescuePath.isEmpty()) {
        QFile::remove(first.rescuePath);
        const std::string written(first.written.constData(), size_t(first.written.size()));
        std::vector<Piece> forFile;
        note.fileBytes(NoteHeader{}, &forFile);
        report = "\n  " + first.message.toStdString();
        report += "\n  вышло бы в файл:\n" + written;
        report += "\n  а прочиталось бы:\n" + bodyOf(written).toMarkdown();
        // Расходиться могут и блоки при одинаковом тексте — тогда видно только
        // здесь: строение сравнивают и запись, и этот набор.
        report += "\n  блоки записи:\n" + dumpOf(forFile);
        report += "\n  блоки чтения:\n" + dumpOf(first.reread);
        keepFailure(written);
        return false;
    }

    const std::string written(first.written.constData(), size_t(first.written.size()));
    // ТРЕТЬЕ: служебных разделителей строк в файле не бывает (просьба
    // владельца 04.09.2026). U+2028 — мягкий перенос ВНУТРИ документа; в файл
    // он уходит переводом строки, а не собой.
    {
        const QString text = QString::fromUtf8(first.written);
        if (text.contains(QChar::LineSeparator) || text.contains(QChar::ParagraphSeparator)) {
            keepFailure(written);
            report = "\n  в файле служебный разделитель строки (U+2028/U+2029):\n" + written;
            report += "\n  живой документ:\n" + note.toJson();
            return false;
        }
    }
    ZDocument reread = bodyOf(written);
    const SaveOutcome second = reread.saveTo(path, QStringLiteral("fuzz"));
    if (!second.rescuePath.isEmpty()) QFile::remove(second.rescuePath);
    if (second.result != SaveResult::Unchanged) {
        keepFailure(written);
        report = "\n  файл не устоялся. записали:\n" + written;
        report += "\n  а перечитав и записав снова, получили:\n" +
                  std::string(second.written.constData(), size_t(second.written.size()));
        // Блоки обоих кругов: без них по двум текстам не видно, где именно
        // разошлись разметка или род блока.
        std::vector<Piece> forFile;
        note.fileBytes(NoteHeader{}, &forFile);
        std::vector<Piece> again;
        reread.fileBytes(NoteHeader{}, &again);
        report += "\n  блоки первой записи:\n" + dumpOf(forFile);
        report += "\n  блоки второй записи:\n" + dumpOf(again);
        // И живой документ до приведения: расхождение может родиться и на
        // ступени приведения, и в самом документе после операций.
        report += "\n  живой документ:\n" + note.toJson();
        return false;
    }
    return true;
}

int g_files = 0;
int g_operations = 0;

void fuzzFile(const fs::path& path, int rounds, uint32_t seed) {
    const std::string source = readFile(path);
    if (source.empty()) return;
    ++g_files;

    ZDocument note = bodyOf(source);

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

    // НЕ ПРОВАЛ, НО И НЕ МОЛЧАНИЕ: сколько раз файл после первой записи ещё не
    // устоялся — то есть открыть и записать заново дало бы другие байты. Все
    // известные случаи — обогащение разметки при чтении (голая ссылка стала
    // ссылкой). Число печатается, чтобы рост был виден.
    std::printf("файлов %d, операций %d\n", g_files, g_operations);
    return zt::report("фаззинг операций");
}

// Набор целиком одним TEST: тело не тронуто, argv ему собран здесь.
// Дробить на отдельные проверки — отдельная работа, по одному набору.
TEST(FuzzOps, All) {
    std::vector<QByteArray> ztArgs{QByteArrayLiteral("fuzz_ops_test")};
    ztArgs.push_back((zt::TestData::corpus(QStringLiteral("corpus"))).toLocal8Bit());
    g_scratch = zt::TestData::outDir(QStringLiteral("fuzz-ops"));
    std::vector<char*> ztArgv;
    for (QByteArray& a : ztArgs) ztArgv.push_back(a.data());
    EXPECT_EQ(0, ztRunSuite(int(ztArgv.size()), ztArgv.data()));
}
