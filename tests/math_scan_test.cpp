// Канон границ `$`: где математика, а где просто доллары.
//
// Правило взято у pandoc и совпадает с тем, как рендерит GitHub, — значит
// заметки едут туда без конверсий. Проверяется оно здесь целиком, включая все
// спорные края, и на живом корпусе: 98 формул разведки (13 выключных и 85
// строчных), из которых две перенесены через строку исходника — наивный сканер
// теряет именно их.

#include "math_scan.h"

#include "test_util.h"

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using zametti::MathSpan;
using zametti::scanMath;

namespace {

std::string bodies(std::string_view text) {
    std::string out;
    for (const MathSpan& span : scanMath(text)) {
        if (!out.empty()) out += "|";
        out += std::string(span.display ? "$$" : "$");
        out += std::string(span.body(text));
    }
    return out;
}

int countOf(std::string_view text, bool display) {
    int n = 0;
    for (const MathSpan& span : scanMath(text))
        if (span.display == display) ++n;
    return n;
}

// --- канон -----------------------------------------------------------------

void checkCanon() {
    ZT_EQ("простая инлайн-формула", "$x^2", bodies("степень $x^2$ в тексте"));
    ZT_EQ("две подряд", "$a|$b", bodies("$a$ и $b$"));
    ZT_EQ("выключная", "$$\\frac{a}{b}", bodies("$$\\frac{a}{b}$$"));

    // ГЛАВНОЕ ПРАВИЛО КАНОНА: закрывающий доллар не перед цифрой. Иначе всё,
    // что между двумя ценами, оказалось бы математикой.
    ZT_EQ("«цена $5 и $10» — не математика", "", bodies("цена $5 и $10"));
    ZT_EQ("и в середине фразы тоже", "", bodies("билет стоит $5, обед $12"));

    // Открывающий не перед пробелом, закрывающий не после пробела.
    ZT_EQ("доллар перед пробелом не открывает", "", bodies("$ x + y$"));
    ZT_EQ("доллар после пробела не закрывает", "", bodies("$x + y $"));
    ZT_EQ("но внутри пробелы законны", "$x + y", bodies("$x + y$"));

    // Экранированный доллар — литеральный, границей не бывает.
    ZT_EQ("экранированные доллары не формула", "", bodies("\\$5 и \\$10"));
    ZT_EQ("экранированный внутри формулы не кончает её", "$a\\$b",
          bodies("$a\\$b$"));
    // А вот экранированная КОСАЯ доллар не экранирует: `\\$` это косая и
    // настоящий доллар. На нечётности и держится разбор.
    ZT_EQ("двойная косая доллар не прячет", "$x", bodies("a\\\\$x$"));

    ZT_EQ("пустая пара — не формула", "", bodies("$$"));
    ZT_EQ("одинокий доллар — не формула", "", bodies("сумма $ и всё"));

    // Формула вправе переехать через перенос строки — в корпусе таких две.
    ZT_EQ("формула через перенос строки", "$N \\approx\nM",
          bodies("текст $N \\approx\nM$ дальше"));
    // Но не через пустую строку: иначе одинокий доллар склеил бы полдокумента.
    ZT_EQ("через границу абзаца — не формула", "", bodies("$a\n\nb$"));

    // Выключная многострочная — законна и обязана собираться целиком.
    ZT_EQ("многострочная выключная", "$$\n\\begin{aligned}\na &= b\n\\end{aligned}\n",
          bodies("$$\n\\begin{aligned}\na &= b\n\\end{aligned}\n$$"));

    // Вложенности не бывает: за концом формулы поиск продолжается.
    ZT_EQ("доллары внутри выключной не рвут её", "$$a $ b",
          bodies("$$a $ b$$"));
}

// --- живой корпус ----------------------------------------------------------
//
// 98 формул, 13 выключных и 85 строчных — числа сошлись у двух независимых
// сканеров разведки. Их и спрашиваем.
void checkCorpus(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::printf("корпуса нет рядом (%s) — эта часть пропущена\n", path.c_str());
        return;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    const std::string text = buffer.str();

    const int display = countOf(text, true);
    const int inline_ = countOf(text, false);
    ZT_EQ("выключных в корпусе", std::to_string(13), std::to_string(display));
    ZT_EQ("строчных в корпусе", std::to_string(85), std::to_string(inline_));
    ZT_EQ("всего формул", std::to_string(98), std::to_string(display + inline_));

    // Ни одна не пуста и ни одна не съела пол-абзаца: длина тела в разумных
    // пределах. Проверка не придирка — именно так выглядит сканер, склеивший
    // две формулы в одну через текст между ними.
    int longest = 0;
    for (const MathSpan& span : scanMath(text))
        longest = std::max(longest, int(span.body(text).size()));
    ZT_TRUE("самая длинная формула не безразмерна: " + std::to_string(longest) + " байт",
            longest > 0 && longest < 400);

    // ФОРМУЛЫ, ПЕРЕНЕСЁННЫЕ ЧЕРЕЗ СТРОКУ ИСХОДНИКА — то, что теряет наивный
    // сканер. В отчёте разведки сказано «три», в самом файле их ДВЕ:
    // `$N \approx M$` и `$A \subseteq B$` (строки 61–64, единственные с
    // непарным долларом). Числа корпуса 98/13/85 при этом сходятся с отчётом
    // до единицы, так что расходится не корпус, а фраза в отчёте — и лучше
    // верить файлу, чем пересказу.
    int wrapped = 0;
    for (const MathSpan& span : scanMath(text))
        if (span.body(text).find('\n') != std::string_view::npos && !span.display) ++wrapped;
    ZT_EQ("строчных формул с переносом строки", std::to_string(2), std::to_string(wrapped));
}

}  // namespace

int main(int argc, char** argv) {
    checkCanon();
    checkCorpus(argc > 1 ? argv[1] : "../.testdata/Typesetting Math in Texts.md");
    return zt::report("границы формул");
}
