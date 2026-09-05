// Фаззинг обоих инвариантов по корпусу реальных заметок и по мутациям из него.
//
// Первый инвариант на произвольном файле не обязан выполняться: файл может быть
// не в каноническом виде. Обязательны:
//
//   pieces(noteOf(x).toMarkdown()) == pieces(x)     — приведение ничего не теряет
//   markdownOf(pieces(noteOf(x).toMarkdown())) == noteOf(x).toMarkdown()  — канон неподвижен
//
// Плюс отдельно считается доля файлов, которые уже каноничны: это единственная
// честная мера того, насколько вывод похож на то, что люди пишут руками.

#include "pieces.h"

#include "test_util.h"
#include "testdata.h"

#include <algorithm>
#include <cstdint>
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

int g_canonical = 0;
int g_total = 0;
fs::path g_saveDir;   // куда складывать падающие входы, чтобы их можно было сузить

void checkInvariants(const std::string& what, const std::string& src) {
    ++g_total;
    int before = zt::g_failures;
    // Читаем той же дверью, что файл (normaliseSpaces + разбор): устойчивость
    // спрашивается у того IR, который получает программа, а не у голого разбора.
    std::vector<Piece> d1 = piecesOfFile(src);
    std::string once = markdownOf(d1);
    std::vector<Piece> d2 = piecesOfFile(once);
    std::string twice = markdownOf(d2);

    ZT_EQ("устойчивость IR: " + what, dumpOf(d1), dumpOf(d2));
    ZT_EQ("неподвижная точка: " + what, once, twice);
    if (src == once) ++g_canonical;

    if (zt::g_failures != before && !g_saveDir.empty()) {
        std::string name = what;
        for (char& ch : name)
            if (ch == '/' || ch == ' ') ch = '_';
        std::ofstream out(g_saveDir / (name + ".md"), std::ios::binary);
        out << src;
    }
}

// Детерминированные мутации: режем, склеиваем, дублируем строки. Цель — не
// «случайный шум», а стыки блоков, на которых ломается восстановление границ.
uint64_t nextRandom(uint64_t& state) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
}

std::vector<std::string> mutations(const std::string& src, uint64_t seed, int count) {
    std::vector<std::string> lines = zt::splitLines(src);
    std::vector<std::string> out;
    if (lines.empty()) return out;

    uint64_t state = seed | 1;
    for (int i = 0; i < count; ++i) {
        std::vector<std::string> copy = lines;
        int op = static_cast<int>(nextRandom(state) % 4);
        size_t at = static_cast<size_t>(nextRandom(state) % copy.size());
        switch (op) {
            case 0:   // выкинуть строку — рвёт таблицы и огороженный код
                copy.erase(copy.begin() + static_cast<long>(at));
                break;
            case 1:   // задвоить строку
                copy.insert(copy.begin() + static_cast<long>(at), copy[at]);
                break;
            case 2:   // склеить блоки: убрать пустую строку
                if (copy[at].empty()) copy.erase(copy.begin() + static_cast<long>(at));
                else copy.insert(copy.begin() + static_cast<long>(at), "");
                break;
            case 3: {   // переставить две строки
                size_t other = static_cast<size_t>(nextRandom(state) % copy.size());
                std::swap(copy[at], copy[other]);
                break;
            }
            default:
                break;
        }
        std::string joined;
        for (const std::string& l : copy) {
            joined += l;
            joined += '\n';
        }
        out.push_back(std::move(joined));
    }
    return out;
}

}  // namespace

static int ztRunSuite(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
                     "использование: corpus_test <каталог с .md> [мутаций на файл] [куда сохранять падающие]\n");
        return 2;
    }
    fs::path root = argv[1];
    int mutationsPerFile = argc > 2 ? std::atoi(argv[2]) : 16;
    if (argc > 3) {
        g_saveDir = argv[3];
        fs::create_directories(g_saveDir);
    }

    // The corpus is not in the repository (see tests/testdata.h): say so and
    // pass empty rather than go red, like fuzz_ops_test and image_shots_test.
    if (root.empty() || !fs::exists(root)) {
        std::fprintf(stderr, "корпуса нет рядом: %s — набор пропущен\n", root.string().c_str());
        return 0;
    }

    std::vector<fs::path> files;
    if (fs::is_regular_file(root)) {
        files.push_back(root);
    } else {
        for (const auto& e : fs::recursive_directory_iterator(root))
            if (e.is_regular_file() && e.path().extension() == ".md") files.push_back(e.path());
        std::sort(files.begin(), files.end());
    }

    ZT_TRUE("корпус непуст", !files.empty());

    uint64_t seed = 0x9e3779b97f4a7c15ull;
    for (const fs::path& f : files) {
        std::string src = readFile(f);
        std::string name = f.filename().string();
        checkInvariants(name, src);
        int i = 0;
        for (const std::string& m : mutations(src, seed + files.size() + src.size(),
                                              mutationsPerFile)) {
            checkInvariants(name + " ~ мутация " + std::to_string(i++), m);
        }
    }

    std::fprintf(stderr, "корпус: %zu файлов, %d прогонов, каноничны без правок: %d (%.0f%%)\n",
                 files.size(), g_total, g_canonical,
                 g_total ? 100.0 * g_canonical / g_total : 0.0);
    return zt::report("corpus");
}

// Набор целиком одним TEST: тело не тронуто, argv ему собран здесь.
// Дробить на отдельные проверки — отдельная работа, по одному набору.
TEST(Corpus, All) {
    std::vector<QByteArray> ztArgs{QByteArrayLiteral("corpus_test")};
    ztArgs.push_back((zt::TestData::corpus(QStringLiteral("corpus"))).toLocal8Bit());
    std::vector<char*> ztArgv;
    for (QByteArray& a : ztArgs) ztArgv.push_back(a.data());
    EXPECT_EQ(0, ztRunSuite(int(ztArgv.size()), ztArgv.data()));
}
