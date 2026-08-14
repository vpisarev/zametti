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

#include "document_builder.h"
#include "document_reader.h"
#include "document_saver.h"
#include "doc_model.h"
#include "editor_ops.h"
#include "json_dump.h"
#include "parser.h"
#include "serializer.h"

#include "test_util.h"
#include "testdata.h"

#include <QGuiApplication>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
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
struct Operation {
    const char* name;
    bool (*run)(QTextDocument&, QTextCursor&);
};

const Operation kOperations[] = {
    {"Enter", splitBlockAtCursor},
    {"Shift+Enter", splitBlockOtherwiseAtCursor},
    {"Backspace у маркера", unwrapListItemAtCursor},
    {"Tab", indentListItems},
    {"Shift+Tab", outdentListItems},
    {"переключить задачу", toggleTaskAtCursor},
    {"в буллет", makeBullet},
    {"в нумерованный", makeOrdered},
    {"в задачу", makeTask},
    {"в абзац", makeParagraph},
    {"жирный", toggleBold},
    {"курсив", toggleItalic},
    {"зачёркнутый", toggleStrike},
    {"код в строке", toggleCode},
    {"правило набора", applyInputRuleAtCursor},
    {"кавычка кода", applyCodeSpanRuleAtCursor},
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

// Один блок в виде строки — для отчёта о расхождении. Блок приезжает из чужого
// документа, поэтому только через adopt: его Range в чужой арене указывали бы
// в произвольное место.
std::string oneLine(const Document& doc, const Block& block) {
    Document one;
    one.blocks.push_back(one.adopt(doc, block));
    std::string out;
    for (char c : toJson(one)) out += (c == '\n') ? ' ' : c;
    return out;
}

// Записываем ли мы этот документ без аварийного файла. Ровно та же сверка, что
// и в сохранении: нормализованный IR против разобранного обратно.
//
// При расхождении показываем первый разошедшийся блок: без этого по одному лишь
// тексту файла причину искать пришлось бы руками.
bool savable(const QTextDocument& doc, std::string& report) {
    const Document ir = documentForFile(readDocument(doc));
    const std::string written = serialize(ir);
    const Document reread = parse(written);
    if (sameSkeleton(ir, reread)) return true;

    report = "\n  вышло бы в файл:\n" + written;
    report += "\n  блоков: документ " + std::to_string(ir.blocks.size()) + ", обратно " +
              std::to_string(reread.blocks.size());
    for (size_t i = 0; i < ir.blocks.size() && i < reread.blocks.size(); ++i) {
        if (oneLine(ir, ir.blocks[i]) == oneLine(reread, reread.blocks[i])) continue;
        report += "\n  блок " + std::to_string(i) + " разошёлся:\n    документ: " +
                  oneLine(ir, ir.blocks[i]) + "\n    обратно:  " + oneLine(reread, reread.blocks[i]);
        break;
    }
    return false;
}

int g_files = 0;
int g_operations = 0;

void fuzzFile(const fs::path& path, int rounds, uint32_t seed) {
    const std::string source = readFile(path);
    if (source.empty()) return;
    ++g_files;

    QTextDocument doc;
    buildDocument(parse(source), doc);

    std::mt19937 rng(seed);
    std::vector<std::string> steps;

    for (int round = 0; round < rounds; ++round) {
        const int characters = doc.characterCount();
        if (characters <= 1) break;

        // Случайное место и, с некоторой вероятностью, выделение.
        QTextCursor cursor(&doc);
        const int from = int(rng() % uint32_t(characters));
        cursor.setPosition(qBound(0, from, characters - 1));
        if (rng() % 3 == 0) {
            const int to = int(rng() % uint32_t(characters));
            cursor.setPosition(qBound(0, to, characters - 1), QTextCursor::KeepAnchor);
        }

        std::string what;
        const bool fromTyping = rng() % 4 == 0;
        if (fromTyping) {
            // Набор: он же заводит те состояния, которых операции не дают —
            // висящий перенос, пробел на краю, недописанную разметку.
            const char* typed = kTyped[rng() % (sizeof(kTyped) / sizeof(kTyped[0]))];
            cursor.insertText(QString::fromUtf8(typed));
            // Ровно как в редакторе: набор на пустой строке разбирается сразу
            // после того, как знак введён.
            repairAfterTyping(doc, cursor);
            what = std::string("набрать \"") + typed + "\"";
        } else {
            const Operation& op = kOperations[rng() % (sizeof(kOperations) / sizeof(kOperations[0]))];
            const Document before = readDocument(doc);
            const std::string beforeJson = toJson(before);
            if (!op.run(doc, cursor)) continue;
            what = op.name;

            // Инвариант C: операция и отмена дают исходный документ. Отмена у нас
            // — пересборка из снимка, ровно как в редакторе.
            QTextDocument undone;
            buildDocument(before, undone);
            const Document back = readDocument(undone);
            if (toJson(back) != beforeJson) {
                steps.push_back(what);
                std::string diff;
                for (size_t i = 0; i < before.blocks.size() || i < back.blocks.size(); ++i) {
                    const std::string was =
                        i < before.blocks.size() ? oneLine(before, before.blocks[i]) : "<нет>";
                    const std::string now =
                        i < back.blocks.size() ? oneLine(back, back.blocks[i]) : "<нет>";
                    if (was == now) continue;
                    diff = "\n  блок " + std::to_string(i) + " разошёлся:\n    было:  " + was +
                           "\n    стало: " + now;
                    break;
                }
                ZT_TRUE(std::string("отмена не вернула документ: ") + path.string() +
                                 " (зерно " + std::to_string(seed) + ")" + describe(steps) + diff,
                        false);
                return;
            }
        }
        steps.push_back(what);
        ++g_operations;

        // Редактор после каждой операции пересобирает документ из IR. Без этого
        // стенд заводил бы состояния, которых в приложении не бывает, и ловил бы
        // не ошибки, а собственную неверность.
        if (!fromTyping) {
            const Document current = readDocument(doc);
            buildDocument(current, doc);
        }

        // Строение обязано остаться правильным.
        QString listProblem;
        const bool lists = listInvariantHolds(doc, &listProblem);
        ZT_TRUE(std::string("строение списка: ") + path.string() + " (зерно " +
                    std::to_string(seed) + ")" + describe(steps) + "\n  " +
                    listProblem.toStdString() +
                    (lists ? std::string() : "\n  документ: " + toJson(readDocument(doc))),
                lists);
        const bool literals = literalInvariantHolds(doc);
        ZT_TRUE(std::string("разбивка литерального блока: ") + path.string() + " (зерно " +
                    std::to_string(seed) + ")" + describe(steps),
                literals);
        QString gapProblem;
        const bool gaps = gapInvariantHolds(doc, &gapProblem);
        ZT_TRUE(std::string("пустые строки: ") + path.string() + " (зерно " +
                    std::to_string(seed) + ")" + describe(steps) + "\n  " +
                    gapProblem.toStdString() +
                    (gaps ? std::string() : "\n  документ: " + toJson(readDocument(doc))),
                gaps);
        if (!lists || !literals || !gaps) return;

        // И документ обязан оставаться записываемым: иначе человек получил бы
        // аварийный файл вместо сохранения.
        std::string report;
        const bool ok = savable(doc, report);
        ZT_TRUE(std::string("документ записывается: ") + path.string() + " (зерно " +
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
