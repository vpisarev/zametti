// Инвариант A этапа 2: read(build(ir)) == ir.
//
// Круг замыкается на IR, а не на markdown: сборщик отбрасывает всё, что не
// выражено моделью (маркеры, оформление, отступы), и если читатель хоть что-то
// не восстановит — расхождение видно здесь, а не после сохранения файла.
//
// Сравнение идёт через JSON-дамп ядра: он показывает все поля, а не только
// текст, и расхождение читается глазами.

#include "document_builder.h"
#include "pieces.h"
#include "test_util.h"
#include "testdata.h"

#include <QGuiApplication>
#include <QTextDocument>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::string readFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::vector<std::filesystem::path> markdownFiles(const std::filesystem::path& root) {
    std::vector<std::filesystem::path> files;
    std::error_code ec;
    for (std::filesystem::recursive_directory_iterator it(root, ec), end; it != end;
         it.increment(ec)) {
        if (ec) break;
        if (it->is_regular_file() && it->path().extension() == ".md") files.push_back(it->path());
    }
    std::sort(files.begin(), files.end());
    return files;
}

// Один прогон: markdown → IR → документ → IR. Сравниваются два IR, исходный
// текст в проверке не участвует.
bool roundtrip(const std::string& source, const std::string& label) {
    const std::vector<zametti::Piece> ir = pieces(source);

    QTextDocument doc;
    zametti::buildDocument(ir, doc);
    const std::vector<zametti::Piece> back = blocksOf(doc);

    const std::string expected = dumpOf(ir);
    const std::string actual = dumpOf(back);
    ++zt::g_checks;
    if (expected == actual) return true;

    ++zt::g_failures;
    std::printf("\n%s: IR не совпал\n%s", label.c_str(), zt::diff(expected, actual).c_str());
    return false;
}

// Случаи, где сборщик неизбежно теряет часть вида и восстановление держится на
// свойствах документа. Здесь их проще перечислить руками, чем ждать, пока такой
// файл попадётся в корпусе.
const char* const kCases[] = {
    "просто абзац\n",
    "# заголовок\n",
    "###### шестой уровень\n",
    "# заголовок с **жирным** и `кодом`\n",
    "*курсив* и **жирный** и ~~зачёркнутый~~\n",
    "текст с [ссылкой](https://example.com) внутри\n",
    "ссылка https://example.com голышом\n",
    "`код в строке` и **жирный `код` внутри жирного**\n",
    "- буллет\n- второй\n  - вложенный\n",
    "1. первый\n2. второй\n",
    "- [ ] не сделано\n- [x] сделано\n",
    "> цитата\n",
    "```py\nx = 1\n```\n",
    "```\n```\n",
    "```\n\n```\n",
    "```cpp\nint main() {}\n```\n",
    "| a | b |\n|---|---|\n| 1 | 2 |\n",
    "текст\nс мягким переносом\n",
    "кириллица: ёжик, **жирный ёжик**, `код с ё`\n",
    "эмодзи 🎉 и **жирное 🎉 эмодзи**\n",
    "смайл в ссылке [🎉 тут](https://example.com)\n",
    "---\n",
    "[foo]: /url\n\nтекст\n",
    "![картинка](a.png)\n",
    "![с заголовком](b.png \"Вечер\")\n",
    "в тексте ![фото](фото.jpg#w=300) с фрагментом\n",

    // Пустой документ: у QTextDocument всегда есть блок, а у IR блоков нет.
    "",
    "\n\n",

    // Знаки, на которых Qt рвёт блок в insertText. В заметках из Apple Notes
    // U+2028 и U+2029 встречаются россыпью, и путать их с мягким переносом
    // нельзя: чужой U+2028 обязан вернуться собой.
    "строка\u2028вторая строка\n",
    "строка\u2029вторая строка\n",
    "строка\rвторая строка\n",
    "**жирный\u2028с переносом** и `код\u2029внутри`\n",
    "- пункт\u2028с переносом\n- второй\n",
    "```\nкод\u2028с разделителем\n```\n",
    "текст\nмягкий перенос и \u2028 чужой разделитель\n",

    // Литеральные блоки лежат в документе построчно. Главное, что здесь
    // проверяется: разрезанный блок не должен путаться с двумя соседними —
    // это разный markdown, а в документе они выглядят почти одинаково.
    "```\nодна\nдве\nтри\n```\n",
    "```\nодна\n```\n\n```\nдве\n```\n",
    "```py\nодна\n```\n\n```sh\nдве\n```\n",
    "```\nодна\n```\n\nмежду\n\n```\nдве\n```\n",
    "```\n\nпустая строка сверху\n```\n",
    "```\nпустая строка снизу\n\n```\n",
    "```\nдве\n\n\nпустые внутри\n```\n",
    "```py\nx = 1\ny = 2\n```\n\nхвост\n",
    "```\nкод с эмодзи 🎉 и ё\nвторая строка\n```\n",

    // Дословные куски: то же самое, но их рядом бывает два разного рода.
    "| a | b |\n|---|---|\n| 1 | 2 |\n",
    "| a |\n|---|\n| 1 |\n\n| c |\n|---|\n| 2 |\n",
    "| a |\n|---|\n| 1 |\n\n---\n\n| b |\n|---|\n| 2 |\n",
};

}  // namespace

static int ztRunSuite(int argc, char** argv) {

    for (const char* source : kCases) roundtrip(source, std::string("случай: ") + source);

    for (int i = 1; i < argc; ++i) {
        const std::filesystem::path root = argv[i];
        const std::vector<std::filesystem::path> files = markdownFiles(root);
        if (files.empty()) {
            std::printf("в каталоге %s нет .md\n", argv[i]);
            return 1;
        }
        for (const std::filesystem::path& file : files)
            roundtrip(readFile(file), file.string());
        std::printf("%s: файлов %zu\n", argv[i], files.size());
    }

    std::printf("проверок %d, провалов %d\n", zt::g_checks, zt::g_failures);
    return zt::g_failures == 0 ? 0 : 1;
}

// Набор целиком одним TEST: тело не тронуто, argv ему собран здесь.
// Дробить на отдельные проверки — отдельная работа, по одному набору.
TEST(Reader, All) {
    std::vector<QByteArray> ztArgs{QByteArrayLiteral("reader_test")};
    std::vector<char*> ztArgv;
    for (QByteArray& a : ztArgs) ztArgv.push_back(a.data());
    EXPECT_EQ(0, ztRunSuite(int(ztArgv.size()), ztArgv.data()));
}
