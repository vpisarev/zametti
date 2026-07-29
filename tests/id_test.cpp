// Генератор id заметки: длина, алфавит, кодирование известного времени в
// известный префикс, валидация, O_EXCL-перегенерация при коллизии.

#include "note_id.h"

#include "test_util.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>

namespace fs = std::filesystem;
using namespace zametti;

int main() {
    // Известное время из брифа: 2019-03-14T09:26:53Z = 1552555613.
    ZT_EQ("известное время в известный префикс", std::string("01e8m7jx"),
          makeNoteId(1552555613, 0).substr(0, 8));
    ZT_EQ("нулевое время — восемь нулей", std::string("00000000000000"),
          makeNoteId(0, 0));
    ZT_EQ("случайная часть добивается нулями", std::string("000001"),
          makeNoteId(0, 1).substr(8));
    ZT_EQ("максимум случайной части", std::string("zzzzzz"),
          makeNoteId(0, (1ull << 30) - 1).substr(8));
    ZT_EQ("время берётся по модулю 32^8", std::string("00000000"),
          makeNoteId(1ull << 40, 0).substr(0, 8));

    // Боевой генератор: длина, алфавит, валидация, начинается с нуля (до
    // ближайшего тысячелетия — признак формата).
    for (int i = 0; i < 200; ++i) {
        const std::string id = newNoteId();
        ZT_TRUE("длина 14", id.size() == 14);
        ZT_TRUE("проходит валидацию", isValidNoteId(id));
        ZT_TRUE("начинается с нуля", id[0] == '0');
        ZT_TRUE("нет запрещённых знаков",
                id.find_first_of("ilouILOU") == std::string::npos);
    }

    ZT_TRUE("валидация: короткое мимо", !isValidNoteId("0123"));
    ZT_TRUE("валидация: буква l мимо", !isValidNoteId("0123456789ablc"));
    ZT_TRUE("валидация: верхний регистр мимо", !isValidNoteId("0123456789ABCD"));
    ZT_TRUE("валидация: правильный проходит", isValidNoteId("01e8m7jx2q3r4s"));

    // Коллизия: генератор нарочно дважды выдаёт занятый id — файл создаётся
    // на третьем, свежем. O_EXCL, а не проверка существования: создание и
    // проверка — одно действие ядра ОС.
    const fs::path dir = fs::temp_directory_path() / "zametti-id-test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    {
        std::ofstream busy(dir / "00000000000001.md");
        busy << "занято";
    }
    int calls = 0;
    const auto rigged = [&calls]() {
        ++calls;
        return makeNoteId(0, calls <= 2 ? 1 : 7);
    };
    std::string path;
    const std::string id =
        createNoteFile(dir.string(), "тело\n", &path, rigged);
    ZT_EQ("коллизия перегенерирована", std::string("00000000000007"), id);
    ZT_TRUE("понадобилось три попытки", calls == 3);
    ZT_TRUE("файл на месте и с телом", fs::exists(path) && fs::file_size(path) == 9);
    {
        std::ifstream in(dir / "00000000000001.md");
        std::string keep;
        std::getline(in, keep);
        ZT_EQ("занятый файл не тронут", std::string("занято"), keep);
    }

    // Генератор, который никогда не находит свежего имени, не крутится вечно.
    const auto stuck = [] { return std::string("00000000000001"); };
    ZT_TRUE("вечная коллизия — пустой id, а не вечный цикл",
            createNoteFile(dir.string(), "", nullptr, stuck).empty());

    fs::remove_all(dir);
    return zt::report("генератор id");
}
