// Пробник представления IR: аллокации, время разбора, время поиска, память.
//
// Тестом не является — это измерительный прибор. Один прогон = один набор
// данных: пиковый RSS процесса иначе не разложить по наборам.
//
// Счётчик аллокаций — подменный глобальный operator new. Он видит всё, что
// линкуется в этот бинарник, то есть ядро целиком; аллокации самого пробника
// (чтение файлов, вектор документов) считаются отдельно — счётчик включается
// ровно вокруг измеряемого куска.
//
//   ir_bench <метка> <файл-или-каталог> [ещё пути...]

#include "parser.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <new>
#include <string>
#include <vector>

namespace {

struct Counters {
    size_t allocations = 0;
    size_t bytes = 0;
    size_t live = 0;      // сколько сейчас не освобождено
    size_t peakLive = 0;
};

Counters g_counters;
bool g_counting = false;

// Размер блока по указателю: без него «сколько сейчас занято» не посчитать —
// operator delete размер знает не всегда. Кладём его перед блоком сами.
constexpr size_t kHeader = 32;   // кратно alignof(std::max_align_t) с запасом

void* allocate(size_t size) {
    void* raw = std::malloc(size + kHeader);
    if (raw == nullptr) throw std::bad_alloc();
    *static_cast<size_t*>(raw) = size;
    if (g_counting) {
        ++g_counters.allocations;
        g_counters.bytes += size;
        g_counters.live += size;
        if (g_counters.live > g_counters.peakLive) g_counters.peakLive = g_counters.live;
    }
    return static_cast<char*>(raw) + kHeader;
}

void release(void* p) noexcept {
    if (p == nullptr) return;
    void* raw = static_cast<char*>(p) - kHeader;
    const size_t size = *static_cast<size_t*>(raw);
    if (g_counting) g_counters.live -= size;
    std::free(raw);
}

}  // namespace

void* operator new(size_t size) { return allocate(size); }
void* operator new[](size_t size) { return allocate(size); }
void* operator new(size_t size, const std::nothrow_t&) noexcept {
    try { return allocate(size); } catch (...) { return nullptr; }
}
void* operator new[](size_t size, const std::nothrow_t&) noexcept {
    try { return allocate(size); } catch (...) { return nullptr; }
}
void operator delete(void* p) noexcept { release(p); }
void operator delete[](void* p) noexcept { release(p); }
void operator delete(void* p, size_t) noexcept { release(p); }
void operator delete[](void* p, size_t) noexcept { release(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { release(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { release(p); }

namespace {

using Clock = std::chrono::steady_clock;

double msSince(Clock::time_point from) {
    return std::chrono::duration<double, std::milli>(Clock::now() - from).count();
}

size_t readStatusKb(const char* key) {
    std::ifstream in("/proc/self/status");
    std::string line;
    while (std::getline(in, line)) {
        if (line.compare(0, std::strlen(key), key) != 0) continue;
        return static_cast<size_t>(std::strtoul(line.c_str() + std::strlen(key), nullptr, 10));
    }
    return 0;
}

std::string readFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void collect(const std::filesystem::path& path, std::vector<std::filesystem::path>& out) {
    std::error_code ec;
    if (std::filesystem::is_directory(path, ec)) {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(
                 path, std::filesystem::directory_options::skip_permission_denied, ec)) {
            if (entry.is_regular_file(ec) && entry.path().extension() == ".md")
                out.push_back(entry.path());
        }
        std::sort(out.begin(), out.end());
        return;
    }
    out.push_back(path);
}

// Поиск по IR в памяти — то, что делает поиск по хранилищу: пройти блоки и
// найти в тексте подстроку. Набор игл закрытый, чтобы «до» и «после»
// сравнивались один в один.
const char* const kNeedles[] = {"function", "заметка", "курсив", "return", "zzq-нет-такого"};

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "использование: ir_bench <метка> <путь>...\n");
        return 2;
    }
    const std::string label = argv[1];

    std::vector<std::filesystem::path> files;
    for (int i = 2; i < argc; ++i) collect(argv[i], files);
    if (files.empty()) {
        std::fprintf(stderr, "нет .md по указанным путям\n");
        return 2;
    }

    std::vector<std::string> sources;
    sources.reserve(files.size());
    size_t sourceBytes = 0;
    for (const auto& path : files) {
        sources.push_back(readFile(path));
        sourceBytes += sources.back().size();
    }

    // --- разбор -------------------------------------------------------------
    std::vector<zametti::Document> docs;
    docs.reserve(sources.size());

    g_counters = Counters{};
    g_counting = true;
    const Clock::time_point parseStart = Clock::now();
    for (const std::string& source : sources) docs.push_back(zametti::parse(source));
    const double parseMs = msSince(parseStart);
    g_counting = false;
    const Counters parseCounters = g_counters;

    size_t blocks = 0;
    size_t spans = 0;
    size_t arena = 0;
    size_t regrown = 0;   // документов, где арене не хватило резерва
    double worstK = 0.0;
    std::string worstFile;
    for (size_t i = 0; i < docs.size(); ++i) {
        const zametti::Document& doc = docs[i];
        blocks += doc.blocks.size();
        arena += doc.chars.size();
        spans += doc.spans.size();
        if (doc.chars.capacity() > zametti::arenaReserveFor(sources[i].size())) {
            ++regrown;
            if (regrown <= 8)
                std::printf("    реаллокация арены: %s (%zu Б → %zu Б, резерв %zu Б)\n",
                            files[i].string().c_str(), sources[i].size(), doc.chars.size(),
                            zametti::arenaReserveFor(sources[i].size()));
        }
        if (!sources[i].empty()) {
            const double k = double(doc.chars.size()) / double(sources[i].size());
            if (k > worstK) {
                worstK = k;
                worstFile = files[i].string();
            }
        }
    }

    // --- поиск по IR в памяти ----------------------------------------------
    size_t hits = 0;
    g_counters = Counters{};
    g_counting = true;
    const Clock::time_point searchStart = Clock::now();
    for (int round = 0; round < 4; ++round) {
        for (const char* needle : kNeedles) {
            for (const zametti::Document& doc : docs) {
                for (const zametti::Block& b : doc.blocks) {
                    // Дословный кусок и обычный текст лежат в арене одинаково.
                    if (doc.text(b).find(needle) != std::string_view::npos) ++hits;
                }
            }
        }
    }
    const double searchMs = msSince(searchStart);
    g_counting = false;
    const Counters searchCounters = g_counters;

    const size_t hwmKb = readStatusKb("VmHWM:");
    const size_t rssKb = readStatusKb("VmRSS:");

    std::printf("%-10s файлов %6zu  байт %9zu  блоков %8zu  спанов %8zu\n", label.c_str(),
                files.size(), sourceBytes, blocks, spans);
    std::printf("%-10s разбор: %8.1f мс  аллокаций %9zu  выделено %10zu Б  "
                "пик живого %9zu Б\n",
                "", parseMs, parseCounters.allocations, parseCounters.bytes,
                parseCounters.peakLive);
    std::printf("%-10s поиск:  %8.1f мс  аллокаций %9zu  совпадений %zu\n", "", searchMs,
                searchCounters.allocations, hits);
    std::printf("%-10s память: RSS %zu КБ, пик RSS %zu КБ\n", "", rssKb, hwmKb);
    std::printf("%-10s арена: %zu Б на %zu Б исходника (k = %.3f), реаллокаций у %zu "
                "документов\n",
                "", arena, sourceBytes, sourceBytes ? double(arena) / double(sourceBytes) : 0.0,
                regrown);
    std::printf("%-10s худший k = %.3f (%s)\n", "", worstK, worstFile.c_str());
    return 0;
}
