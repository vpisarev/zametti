// Golden-тесты: .md рядом с ожидаемым JSON-дампом строения заметки.
//
// Дамп односторонний. from_json нет и не будет: иначе он незаметно станет
// вторым форматом хранения.
//
// СУДЯТ ОНИ ТЕПЕРЬ ЖИВОЙ ДОКУМЕНТ. Раньше дампилось промежуточное
// представление, то есть выход разбора; теперь — обход живого документа, то
// есть та самая граница «документ → файл», через которую проходят данные
// владельца. Эталоны при переводе не тронуты ни байтом: это и есть проверка,
// что живая модель держит ровно то же, что держало представление.
//
// Перезаписать эталоны: ./golden_test <каталог> --update

#include "document.h"

#include "test_util.h"
#include "testdata.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace zametti;

namespace {

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void writeFile(const fs::path& p, const std::string& s) {
    std::ofstream out(p, std::ios::binary);
    out << s;
}

}  // namespace

static int ztRunSuite(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "использование: golden_test <каталог> [--update]\n");
        return 2;
    }
    fs::path dir = argv[1];
    bool update = argc > 2 && std::string(argv[2]) == "--update";

    if (!fs::is_directory(dir)) {
        std::fprintf(stderr, "нет каталога с эталонами: %s\n", dir.string().c_str());
        return 2;
    }

    std::vector<fs::path> cases;
    for (const auto& e : fs::directory_iterator(dir))
        if (e.is_regular_file() && e.path().extension() == ".md") cases.push_back(e.path());
    std::sort(cases.begin(), cases.end());

    ZT_TRUE("эталоны найдены", !cases.empty());

    for (const fs::path& md : cases) {
        std::string name = md.filename().string();
        std::string src = readFile(md);
        ZDocument note;
        note.loadMarkdown(src);
        std::string json = note.toJson();

        fs::path expectedPath = md;
        expectedPath.replace_extension(".json");

        if (update) {
            writeFile(expectedPath, json);
            continue;
        }

        if (!fs::exists(expectedPath)) {
            ZT_TRUE("нет эталона для " + name, false);
            continue;
        }
        ZT_EQ("строение для " + name, readFile(expectedPath), json);

        // Эталоны лежат в каноническом виде, значит первый инвариант обязан
        // выполняться на них побайтово.
        ZT_EQ("канон для " + name, src, note.toMarkdown());
    }

    if (update) {
        std::fprintf(stderr, "эталоны обновлены: %zu файлов\n", cases.size());
        return 0;
    }
    return zt::report("golden");
}

// Набор целиком одним TEST: тело не тронуто, argv ему собран здесь.
// Дробить на отдельные проверки — отдельная работа, по одному набору.
TEST(Golden, All) {
    std::vector<QByteArray> ztArgs{QByteArrayLiteral("golden_test")};
    ztArgs.push_back((QStringLiteral(ZAMETTI_SOURCE_DIR "/tests/golden")).toLocal8Bit());
    std::vector<char*> ztArgv;
    for (QByteArray& a : ztArgs) ztArgv.push_back(a.data());
    EXPECT_EQ(0, ztRunSuite(int(ztArgv.size()), ztArgv.data()));
}
