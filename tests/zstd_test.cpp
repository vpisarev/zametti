// zstd: что склеенная в один файл библиотека — настоящий zstd.
//
// Проверять её саму собой мало: так же вёл бы себя и сломанный кодек, лишь бы
// он был обратим. Поэтому главная проверка — СОВМЕСТИМОСТЬ: наш поток
// распаковывается системной утилитой `zstd`, а её поток распаковывается нами.
// Если утилиты в системе нет, эти проверки пропускаются громко.
//
// Ядро без Qt: набор собирается и работает без него, как и core-without-qt.

#include "zstd.h"

#include "test_util.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

namespace {

std::string compress(const std::string& input, int level) {
    std::string out(ZSTD_compressBound(input.size()), '\0');
    const size_t size =
        ZSTD_compress(out.data(), out.size(), input.data(), input.size(), level);
    if (ZSTD_isError(size)) return {};
    out.resize(size);
    return out;
}

std::string decompress(const std::string& packed, size_t hint) {
    std::string out(hint + 64, '\0');
    const size_t size = ZSTD_decompress(out.data(), out.size(), packed.data(), packed.size());
    if (ZSTD_isError(size)) return {};
    out.resize(size);
    return out;
}

// Текст, похожий на заметку: повторяющаяся разметка, кириллица, отступы.
std::string noteLike(int lines) {
    std::string out = "<!-- zametti\ncreated: 2026-07-31T00:00:00Z\n-->\n\n# Заметка\n\n";
    for (int i = 0; i < lines; ++i) {
        out += "- [ ] пункт номер " + std::to_string(i) + " с **разметкой** и `кодом`\n";
        if (i % 7 == 0) out += "\n  вложенный абзац пункта, чтобы строение было не плоским\n\n";
    }
    return out;
}

bool haveTool() { return std::system("zstd --version > /dev/null 2>&1") == 0; }

// Запуск команды с ответом «получилось ли». Ответ спрашивается обязательно:
// молча брошенный код возврата превращает «утилита не запустилась» в
// «распакованное не совпало», и набор врёт о том, что именно сломалось.
bool runShell(const std::string& command) { return std::system(command.c_str()) == 0; }

std::string readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void writeFile(const std::string& path, const std::string& data) {
    std::ofstream out(path, std::ios::binary);
    out.write(data.data(), std::streamsize(data.size()));
}

}  // namespace

static int ztRunSuite() {
    ZT_EQ("версия совпадает с вендорингом", std::string("1.5.7"),
          std::string(ZSTD_versionString()));

    // Обратимость на том, что мы и будем сжимать, — на заметках.
    for (int lines : {0, 1, 40, 4000}) {
        const std::string source = noteLike(lines);
        for (int level : {1, 3, 19}) {
            const std::string packed = compress(source, level);
            ZT_TRUE("сжатие удалось: строк " + std::to_string(lines) + ", уровень " +
                        std::to_string(level),
                    !packed.empty() || source.empty());
            ZT_EQ("распаковка вернула то же: строк " + std::to_string(lines) + ", уровень " +
                      std::to_string(level),
                  source, decompress(packed, source.size()));
        }
    }

    // Пустой вход — законный.
    ZT_EQ("пустой вход переживает круг", std::string(), decompress(compress("", 3), 0));

    // Размер распакованного лежит в рамке потока: журналу это нужно, чтобы
    // выделить буфер, не гадая.
    {
        const std::string source = noteLike(400);
        const std::string packed = compress(source, 3);
        ZT_EQ("размер из рамки потока", std::to_string(source.size()),
              std::to_string(ZSTD_getFrameContentSize(packed.data(), packed.size())));
        ZT_TRUE("сжатие заметки хоть что-то даёт", packed.size() * 4 < source.size());
    }

    // Порча потока обязана быть замечена, а не молча дать мусор.
    {
        const std::string source = noteLike(100);
        std::string packed = compress(source, 3);
        packed[packed.size() / 2] ^= 0x5a;
        const size_t size =
            ZSTD_decompress(packed.data(), packed.size(), packed.data(), packed.size());
        ZT_TRUE("испорченный поток отвергается", ZSTD_isError(size));
    }

    // Главное: совместимость с настоящим zstd в обе стороны.
    if (!haveTool()) {
        std::fprintf(stderr, "ПРОПУЩЕНО: утилиты zstd в системе нет, "
                             "совместимость не проверена\n");
    } else {
        const std::string dir = "/tmp/zametti-zstd-test";
        ZT_TRUE("каталог под опыт заведён", runShell("rm -rf " + dir + " && mkdir -p " + dir));
        const std::string source = noteLike(500);

        // Наш поток → системная утилита.
        writeFile(dir + "/наш.zst", compress(source, 3));
        ZT_TRUE("утилита распаковала без жалоб",
                runShell("zstd -d -q -f " + dir + "/наш.zst -o " + dir + "/наш.out"));
        ZT_EQ("системная утилита распаковала наш поток", source, readFile(dir + "/наш.out"));

        // Системная утилита → мы.
        writeFile(dir + "/чужой.txt", source);
        ZT_TRUE("утилита сжала без жалоб",
                runShell("zstd -q -f -19 " + dir + "/чужой.txt -o " + dir + "/чужой.zst"));
        ZT_EQ("мы распаковали поток системной утилиты", source,
              decompress(readFile(dir + "/чужой.zst"), source.size()));
    }

    return zt::report("zstd");
}

// Набор целиком одним TEST: тело не тронуто, argv ему собран здесь.
// Дробить на отдельные проверки — отдельная работа, по одному набору.
TEST(Zstd, All) {
    EXPECT_EQ(0, ztRunSuite());
}
