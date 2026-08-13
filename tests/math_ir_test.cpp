// Формула в IR: спан с ЛИТЕРАЛЬНЫМ исходником и побайтовый круг.
//
// Ради этого набора этап и затевался в том числе. До того, как ядро узнало про
// формулы, круг «разбор → запись» портил математику молча:
//
//     $\int_0^1 x^2 \, dx$   →   $\int_0^1 x^2 , dx$      тонкий пробел исчез
//     $\gamma$               →   $\\gamma$                 в LaTeX это перенос
//     $\sum_{k=0}^\infty$    →   $\sum\_{k=0}^\infty$      индекс сломан
//
// Владелец наткнулся на это, сравнив свою заметку в хранилище с исходным
// файлом: ему пришлось править её руками. Здесь проверяется, что править
// больше нечего.

#include "ir.h"
#include "math_scan.h"
#include "parser.h"
#include "serializer.h"

#include "test_util.h"

#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using zametti::Document;
using zametti::Inline;
using zametti::parse;
using zametti::serialize;

namespace {

// Круг: разобрали, записали — обязано выйти то же самое.
void roundTrip(const std::string& what, const std::string& source) {
    ZT_EQ(what, source, serialize(parse(source)));
}

// Все формулы документа: текст спана как есть, через «|».
std::string mathSpans(const Document& doc) {
    std::string out;
    for (const zametti::Block& b : doc.blocks) {
        if (b.raw) continue;
        for (const Inline& s : doc.inlines(b)) {
            if (!s.math()) continue;
            if (!out.empty()) out += "|";
            out += std::string(doc.text(b, s));
        }
    }
    return out;
}

// --- то, что раньше терялось ------------------------------------------------

void checkNothingIsLost() {
    roundTrip("тонкий пробел \\, переживает круг",
              "Интеграл $\\int_0^1 x^2 \\, dx$ в строке.\n");
    roundTrip("команда \\gamma не удваивается", "Буквы $\\gamma$ и $\\Gamma$ рядом.\n");
    roundTrip("подчёркивание индекса не экранируется",
              "Сумма $\\sum_{k=0}^\\infty \\frac{x^k}{k!}$ тут.\n");
    roundTrip("двойная косая внутри формулы — это перенос строки LaTeX",
              "$$\\begin{aligned}\na &= b \\\\\nc &= d\n\\end{aligned}$$\n");
    roundTrip("выключная многострочная целиком",
              "$$|x| =\n\\begin{cases}\nx & \\text{when}\\ \\ x > 0, \\\\\n-x & "
              "\\text{when}\\ \\ x < 0\n\\end{cases}$$\n");
    roundTrip("формула через перенос строки исходника",
              "Список: $N\n\\approx M$, дальше текст.\n");

    // Спан несёт исходник ВМЕСТЕ с долларами — иначе при записи их пришлось бы
    // дописывать, а вид формулы держать отдельным признаком.
    const Document doc = parse("Степень $x^2$ и дробь $$\\frac{a}{b}$$.\n");
    ZT_EQ("текст спана — исходник с долларами", "$x^2$|$$\\frac{a}{b}$$", mathSpans(doc));
}

// --- канон границ: то, что формулой НЕ является ------------------------------
//
// У md4c свои правила границ, и на этих случаях они с нашим каноном расходятся.
// Байты при этом обязаны пережить круг в любом случае: показ не вправе менять
// файл, что бы он там ни распознал.
void checkCanonBorders() {
    const Document prices = parse("Цена $5 и $10 за штуку.\n");
    ZT_EQ("«цена $5 и $10» формулой не считается", "", mathSpans(prices));
    roundTrip("и байты целы", "Цена $5 и $10 за штуку.\n");

    const Document spaced = parse("Тут $ x + y$ пробел.\n");
    ZT_EQ("доллар перед пробелом формулу не открывает", "", mathSpans(spaced));
    roundTrip("и байты целы", "Тут $ x + y$ пробел.\n");

    const Document tail = parse("Тут $x + y $ пробел.\n");
    ZT_EQ("доллар после пробела формулу не закрывает", "", mathSpans(tail));
    roundTrip("и байты целы", "Тут $x + y $ пробел.\n");

    const Document escaped = parse("Экранировано \\$5 и \\$10.\n");
    ZT_EQ("экранированные доллары не формула", "", mathSpans(escaped));
    // ЛИШНЕЕ ЭКРАНИРОВАНИЕ СНИМАЕТСЯ — это канон, а не потеря: «$5 и $10» по
    // нашим же правилам математикой не является, значит косые перед долларами
    // ничего не держат. Круг после этого неподвижен.
    ZT_EQ("ненужные косые уходят", "Экранировано $5 и $10.\n",
          serialize(parse("Экранировано \\$5 и \\$10.\n")));
    roundTrip("и второй круг ничего не меняет", "Экранировано $5 и $10.\n");

    // А НУЖНОЕ ОСТАЁТСЯ. Литеральные доллары вокруг буквы при чтении стали бы
    // формулой — здесь косая держит смысл, и сериализатор обязан её вернуть.
    const std::string literal = serialize(parse("Литерально \\$x\\$ тут.\n"));
    ZT_TRUE("нужная косая сохранена: " + literal,
            literal.find("\\$x") != std::string::npos);
    ZT_EQ("формулы в нём нет", "", mathSpans(parse(literal)));
    roundTrip("и круг неподвижен", literal);
}

// --- соседство с другой разметкой -------------------------------------------

void checkNeighbours() {
    // Внутри кода формулы нет: там всё буквально.
    const Document inCode = parse("В коде `$x^2$` формулы нет.\n");
    ZT_EQ("внутри кода математика не разбирается", "", mathSpans(inCode));
    roundTrip("и байты целы", "В коде `$x^2$` формулы нет.\n");

    // Рядом с разметкой — законно.
    const Document beside = parse("**Жирно** и формула $x^2$ рядом.\n");
    ZT_EQ("формула рядом с жирным разбирается", "$x^2$", mathSpans(beside));
    roundTrip("круг с соседями", "**Жирно** и формула $x^2$ рядом.\n");

    // Формула ВНУТРИ жирного плоским спаном не выражается — блок уходит
    // дословно, и это законная деградация: байты целы.
    roundTrip("формула внутри жирного — дословно", "**жирно $x^2$ жирно**\n");

    // Две подряд и формула в конце абзаца.
    const Document pair = parse("$a$ и $b$\n");
    ZT_EQ("две формулы подряд", "$a$|$b$", mathSpans(pair));
    roundTrip("круг двух подряд", "$a$ и $b$\n");
}

// --- живой корпус -----------------------------------------------------------
//
// Тот самый файл, на котором разведка проверяла движок: 98 формул.
//
// Побайтового круга от НЕОБРАБОТАННОГО файла требовать нельзя, и это выяснилось
// прямо здесь: ядро приводит списки к канону (`-   пункт` → `- пункт`, отступ
// продолжения 4 → 2, `*курсив*` → `_курсив_`). Всё это было в ядре задолго до
// формул. Поэтому спрашивается то, что действительно обязано выполняться:
//
//   * КАЖДАЯ формула корпуса доходит до канонического текста ДОСЛОВНО;
//   * канон неподвижен: второй круг ничего не меняет.
void checkCorpus(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::printf("корпуса нет рядом (%s) — эта часть пропущена\n", path.c_str());
        return;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();

    std::string source;
    std::istringstream lines(buffer.str());
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        source += line;
        source += '\n';
    }

    const std::vector<zametti::MathSpan> spans = zametti::scanMath(source);
    ZT_EQ("формул в корпусе", std::to_string(98), std::to_string(spans.size()));

    const Document doc = parse(source);
    const std::string canon = serialize(doc);

    // ГЛАВНОЕ: ни одна формула не пострадала. Именно здесь ловились `\,`, `\gamma`
    // и `_` — все три беды видны как отсутствие исходной строки в каноне.
    std::string lost;
    for (const zametti::MathSpan& span : spans) {
        const std::string literal(source.substr(size_t(span.start), size_t(span.end - span.start)));
        if (canon.find(literal) == std::string::npos) lost += "\n    " + literal;
    }
    ZT_EQ("все 98 формул дошли до канона дословно", "", lost);

    // Канон неподвижен — иначе каждая запись заметки шевелила бы файл.
    ZT_EQ("второй круг ничего не меняет", canon, serialize(parse(canon)));

    // Сколько из них ядро выразило спанами. Девять не выражены, и причина
    // ОДНА, найденная здесь же: в разделе «Delimiters» выключная формула
    // записана в три строки, и средняя строка — одинокое `=`:
    //
    //     $$\left((x_1+1)(x_2-1)\right)
    //     =
    //     \bigl((x_1+1)(x_2-1)\bigl).$$
    //
    // Для markdown это setext-заголовок: строка текста, подчёркнутая равными.
    // Решается это ДО всякой строчной разметки, поэтому ни md4c, ни мы формулы
    // здесь не видим вовсе. Заголовок внутри пункта списка ядро не выражает —
    // весь список остаётся дословным, а вместе с ним и девять формул.
    //
    // Байты при этом целы (проверено выше), но показать эти формулы нельзя —
    // разбирать `$$` на уровне БЛОКА придётся отдельно, вместе с выключными.
    int expressed = 0;
    for (const zametti::Block& b : doc.blocks) {
        if (b.raw) continue;
        for (const Inline& s : doc.inlines(b))
            if (s.math()) ++expressed;
    }
    ZT_EQ("формул выражено спанами", std::to_string(89), std::to_string(expressed));

    // И это ровно тот случай: если убрать из формулы одинокое `=`, дословный
    // список исчезает и все 98 выражаются. Проверка держит диагноз — не «девять
    // потерялись», а «потерялись из-за setext-подчёркивания».
    std::string patched = source;
    const size_t underline = patched.find("\\right)\n    =\n");
    ZT_TRUE("подчёркивание найдено", underline != std::string::npos);
    patched.replace(underline, std::strlen("\\right)\n    =\n"), "\\right) =\n");
    const Document fixed = parse(patched);
    int afterPatch = 0;
    for (const zametti::Block& b : fixed.blocks) {
        if (b.raw) continue;
        for (const Inline& s : fixed.inlines(b))
            if (s.math()) ++afterPatch;
    }
    ZT_EQ("без setext-подчёркивания выражаются все", std::to_string(98),
          std::to_string(afterPatch));
}

}  // namespace

int main(int argc, char** argv) {
    checkNothingIsLost();
    checkCanonBorders();
    checkNeighbours();
    checkCorpus(argc > 1 ? argv[1] : "../.testdata/Typesetting Math in Texts.md");
    return zt::report("формулы в IR");
}
