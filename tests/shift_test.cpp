// ИНВАРИАНТ СДВИГА: разбор куска не зависит от того, сколько байт лежит перед ним.
//
// Найдено на живой заметке владельца («Копия Ficus Tutorial»): абзац с отступом
// внутри пункта списка, стоящий за блоком кода, терял уровень — но только при
// одной длине файла из каждых 256. Разбор смотрел на младший байт смещения и
// принимал его за род блока (md4c: «пункт начинается с двух пустых строк»
// заглядывал в последние восемь байт потока блоков, а там лежала строка кода,
// а не блок). Пять прогонов подряд, ASAN и UBSAN молчали, дистиллят из шести
// строк не ломался никогда — ловится это только перебором смещений.
//
// Отсюда сам инвариант и способ его проверять: один и тот же кусок ставится
// за префиксами ВСЕХ длин 0..300 (это больше периода 256 любого однобайтового
// счётчика), и канонический вывод куска обязан быть один и тот же — байт в байт.
// Не выборочные длины, а все подряд: дефект живёт на одной из 256.
//
// Два набора проверок: самодостаточные фрагменты (идут всегда) и копия
// заметки владельца (идёт при наличии корпуса; названный случай проверяется
// сам, а не его упрощение).

#include "document.h"
#include "test_util.h"
#include "testdata.h"

#include <QDir>
#include <QFile>
#include <QString>

#include <cstdio>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

using zametti::ZDocument;

namespace {

std::string canonical(std::string_view markdown) {
    ZDocument note;
    note.loadMarkdown(markdown);
    return note.toMarkdown();
}

// Канон целого без первого блока — то есть без самого префикса. Префикс —
// всегда один блок в одну строку, за ним пустая строка; хвост начинается со
// второй непустой строки.
std::string tailAfterPrefix(const std::string& whole) {
    const size_t nl = whole.find('\n');
    if (nl == std::string::npos) return {};
    size_t p = nl + 1;
    while (p < whole.size() && whole[p] == '\n') ++p;
    return whole.substr(p);
}

// Префиксы трёх родов: абзац, заголовок, пункт списка. Род важен: за пунктом
// списка кусок становится его продолжением или соседом, и путь разбора другой.
struct PrefixKind {
    const char* name;
    const char* head;
};
const PrefixKind kPrefixKinds[] = {
    {"абзац", ""},
    {"заголовок", "## "},
    {"пункт", "- "},
};

std::string prefixOf(const PrefixKind& kind, int length) {
    std::string out = kind.head;
    out.append(size_t(length), 'a');
    out += "\n\n";
    return out;
}

constexpr int kMaxLength = 300;

// Прогнать кусок за префиксами всех длин; вернуть число расхождений.
// kinds — сколько родов префикса брать: на маленьком куске все три, на
// заметке в четверть мегабайта хватает одного (период счётчика от рода
// префикса не зависит, а каждая длина там стоит десятки миллисекунд).
int checkShiftInvariant(std::string_view body, const std::string& label, int maxLength,
                        size_t kinds = std::size(kPrefixKinds)) {
    int failures = 0;
    for (size_t k = 0; k < kinds; ++k) {
        const PrefixKind& kind = kPrefixKinds[k];
        // Эталон — длина 1: при длине 0 префикса нет вовсе, и это другое строение.
        const std::string reference = tailAfterPrefix(canonical(prefixOf(kind, 1) + std::string(body)));
        for (int length = 1; length <= maxLength; ++length) {
            const std::string tail = tailAfterPrefix(canonical(prefixOf(kind, length) + std::string(body)));
            ++zt::g_checks;
            if (tail == reference) continue;
            ++failures;
            if (failures <= 3) {
                std::printf("\n%s: префикс «%s» длиной %d меняет разбор куска\n%s", label.c_str(),
                            kind.name, length, zt::diff(reference, tail).c_str());
            }
        }
    }
    return failures;
}

// Куски, где разбор оглядывается назад: пункт списка, за ним блок кода или
// дословный кусок, потом продолжение пункта. Ровно тот случай, что нашёлся, и
// его ближайшие соседи по строению.
const char* const kBodies[] = {
    // Найденный случай: пункт, пустая строка, код внутри пункта, пустая
    // строка, абзац-продолжение внутри пункта.
    "- **list**: `list[T]`. List is an immutable list.\n"
    "\n"
    "  <!-- doctut: fragment -->\n"
    "  ```\n"
    "  val mylist1 = [:: 1, 2, 3, 4, 5]\n"
    "  fun detect_objects(image: uint8 [,]) :\n"
    "      list[detected_object_info_t] = { ... }\n"
    "  ```\n"
    "\n"
    "  note that you cannot modify a list.\n"
    "\n"
    "- **vector**: `rrbvec[T]`. Vector is an immutable 1D array.\n",

    // Код внутри пункта без комментария перед ним.
    "1. раз\n\n   ```py\n   x = 1\n   y = 2\n   ```\n\n   хвост пункта\n\n2. два\n",

    // Код внутри пункта, за ним вложенный список.
    "- раз\n\n  ```\n  code\n  ```\n\n  - вложенный\n  - ещё\n- два\n",

    // Дословный кусок (таблица) внутри пункта, за ним абзац.
    "- раз\n\n  | a | b |\n  |---|---|\n  | 1 | 2 |\n\n  хвост\n- два\n",

    // Цитата после кода в пункте.
    "- раз\n\n  ```\n  code\n  ```\n\n  > цитата\n- два\n",

    // Выключная формула внутри пункта.
    "- раз\n\n  $$\n  x^2\n  $$\n\n  хвост\n- два\n",

    // Пункт, начинающийся с двух пустых строк, — законный случай CommonMark:
    // список кончается. Строение своё, но от сдвига не зависит.
    "- раз\n\n\n  текст\n",

    // Вложенный список с кодом на втором уровне.
    "- раз\n  - два\n\n    ```\n    code\n    ```\n\n    хвост два\n  - три\n- четыре\n",
};

}  // namespace

TEST(Shift, SnippetsSurviveAnyPrefix) {
    int failures = 0;
    int index = 0;
    for (const char* body : kBodies)
        failures += checkShiftInvariant(body, "кусок №" + std::to_string(++index), kMaxLength);
    std::printf("проверок %d, расхождений %d\n", zt::g_checks, failures);
    EXPECT_EQ(0, failures);
}

// Тот же инвариант на КОПИИ заметки владельца: `.testdata/shift/*.md`. Кладётся
// туда руками — копия, а не оригинал: хранилище владельца только читается.
TEST(Shift, OwnerNotesSurviveAnyPrefix) {
    const QString dir = zt::TestData::corpus(QStringLiteral("shift"));
    ZT_SKIP_NO_CORPUS(dir, "shift (копии заметок владельца для матрицы сдвига)");

    QStringList files = QDir(dir).entryList({QStringLiteral("*.md")}, QDir::Files, QDir::Name);
    if (files.isEmpty()) GTEST_SKIP() << "в корпусе нет ни одной заметки: " << dir.toStdString();

    int failures = 0;
    for (const QString& name : files) {
        QFile f(dir + QLatin1Char('/') + name);
        ASSERT_TRUE(f.open(QIODevice::ReadOnly)) << name.toStdString();
        const QByteArray bytes = f.readAll();
        // Шапка заметки обязана остаться первой: префикс встаёт ПОСЛЕ неё.
        ZDocument note;
        ASSERT_TRUE(note.loadMarkdown(std::string_view(bytes.constData(), size_t(bytes.size()))));
        note.setHasHeader(false);
        const std::string body = note.toMarkdown();
        // 260 длин — больше периода 256; на заметке в четверть мегабайта каждая
        // длина стоит десятки миллисекунд, потому не 300 и один род префикса.
        failures += checkShiftInvariant(body, name.toStdString(), 260, 1);
    }
    std::printf("заметок %d, проверок %d, расхождений %d\n", int(files.size()), zt::g_checks, failures);
    EXPECT_EQ(0, failures);
}
