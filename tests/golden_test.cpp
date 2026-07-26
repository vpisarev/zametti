// Golden-тесты: .md рядом с ожидаемым JSON-дампом IR.
//
// Дамп односторонний. from_json нет и не будет: иначе он незаметно станет
// вторым форматом хранения.
//
// Перезаписать эталоны: ./golden_test <каталог> --update

#include "json_dump.h"
#include "parser.h"
#include "serializer.h"

#include "test_util.h"

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

int main(int argc, char** argv) {
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
        Document doc = parse(src);
        std::string json = toJson(doc);

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
        ZT_EQ("IR для " + name, readFile(expectedPath), json);

        // Эталоны лежат в каноническом виде, значит первый инвариант обязан
        // выполняться на них побайтово.
        ZT_EQ("канон для " + name, src, serialize(doc));
    }

    if (update) {
        std::fprintf(stderr, "эталоны обновлены: %zu файлов\n", cases.size());
        return 0;
    }
    return zt::report("golden");
}
