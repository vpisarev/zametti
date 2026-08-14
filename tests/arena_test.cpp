// Арена IR: построение, перенос между документами, независимость копии,
// границы обоих координатных пространств.
//
// Проверки на срабатывание validate() — отдельным процессом: validate состоит
// из ассертов, и «поймал» здесь значит «упал с SIGABRT». В сборке с NDEBUG
// ассертов нет, и эта часть набора пропускается.

#include "json_dump.h"
#include "parser.h"
#include "serializer.h"

#include "test_util.h"

#include <string>
#include <string_view>
#include <vector>

// Смертельные проверки стерегут assert внутри validate(), а в сборке с NDEBUG
// его нет вовсе: ронять там нечему, и проверять нечего. Признак сборки, а не
// молчаливый пропуск, — иначе набор был бы «зелёным» в release по недосмотру.
#if (defined(__unix__) || defined(__APPLE__)) && !defined(NDEBUG)
#include <sys/wait.h>
#include <unistd.h>
#define ZAMETTI_CAN_FORK 1
#endif

using zametti::Block;
using zametti::Document;
using zametti::Inline;
using zametti::Kind;
using zametti::Range;

namespace {

// --- арена и границы -------------------------------------------------------

void checkAppend() {
    Document doc;
    ZT_TRUE("пустой документ — пустая арена", doc.chars.empty() && doc.spans.empty());

    const Range first = doc.append("раз");
    const Range second = doc.append("два");
    ZT_TRUE("первый кусок с нуля", first.start == 0 && first.size() == 6);
    ZT_TRUE("второй кусок сразу за первым", second.start == first.end);
    ZT_EQ("первый читается", "раз", doc.view(first));
    ZT_EQ("второй читается", "два", doc.view(second));

    // Пустая допись места не занимает и даёт пустой Range — так выражается
    // «нет адреса», «нет info».
    const Range nothing = doc.append("");
    ZT_TRUE("пустая допись пуста", nothing.empty() && nothing.size() == 0);
    ZT_TRUE("и арену не двигает", nothing.start == second.end);
    ZT_EQ("пустой Range читается пустотой", "", doc.view(nothing));

    // Кусок собственной арены — законный вход: replace-by-append только этим и
    // занимается.
    const Range again = doc.append(doc.view(first));
    ZT_EQ("допись вида на самого себя", "раз", doc.view(again));
    ZT_EQ("источник не пострадал", "раз", doc.view(first));
}

void checkNewBlock() {
    Document doc;
    Block heading = doc.newBlock(Kind::Heading, "Заголовок");
    heading.headingLevel = 1;
    doc.blocks.push_back(heading);

    ZT_EQ("текст блока в арене", "Заголовок", doc.text(doc.blocks[0]));
    ZT_TRUE("у нового блока нет ни info, ни разметки",
            doc.blocks[0].info.empty() && doc.blocks[0].inlines.empty());
    ZT_TRUE("и он не дословный", !doc.blocks[0].raw);
    ZT_EQ("info пустого Range читается пустотой", "", doc.info(doc.blocks[0]));

    // Дословный кусок: род ему не положен, перевод строки на конце обязателен —
    // без него IR последнего блока не совпал бы сам с собой после круга.
    doc.blocks.push_back(doc.newRaw("| a |"));
    ZT_TRUE("дословный кусок помечен", doc.blocks[1].raw);
    ZT_TRUE("рода у дословного нет", doc.blocks[1].kind == Kind::Paragraph);
    ZT_EQ("перевод строки дописан", "| a |\n", doc.text(doc.blocks[1]));
    doc.blocks.push_back(doc.newRaw("| b |\n"));
    ZT_EQ("второй перевод не дописывается", "| b |\n", doc.text(doc.blocks[2]));
    doc.validate();
}

// Правка текста — это допись в хвост и перенацеливание Range. Соседи, чьи
// смещения лежат до места правки, обязаны остаться целыми.
void checkReplaceByAppend() {
    Document doc = zametti::parse("первый **жирный** абзац\n\nвторой абзац\n");
    ZT_TRUE("разобралось три блока", doc.blocks.size() == 3);
    if (doc.blocks.size() != 3) return;

    const std::string secondBefore(doc.text(doc.blocks[2]));
    const Range spanBefore = doc.inlines(doc.blocks[0])[0].text;

    doc.blocks[0].text = doc.append("первый жирный абзац, переписанный целиком");
    ZT_EQ("переписанный блок читается по-новому", "первый жирный абзац, переписанный целиком",
          doc.text(doc.blocks[0]));
    ZT_EQ("сосед не пострадал", secondBefore, doc.text(doc.blocks[2]));
    ZT_TRUE("смещение спана относительное и потому не устарело",
            doc.inlines(doc.blocks[0])[0].text == spanBefore);
    ZT_EQ("спан указывает в новый текст", "жирный",
          doc.text(doc.blocks[0], doc.inlines(doc.blocks[0])[0]));
}

// --- перенос между документами --------------------------------------------

void checkAdopt() {
    const Document from = zametti::parse(
        "текст со [ссылкой](https://example.org/a \"\") и ![подписью](img.png \"Заголовок\")\n"
        "\n"
        "```py\nx = 1\n```\n");
    ZT_TRUE("донор разобрался", from.blocks.size() >= 3);
    if (from.blocks.size() < 3) return;

    Document home;
    home.blocks.push_back(home.newBlock(Kind::Paragraph, "своё"));

    // Абзац с картинкой ядро оставляет дословным — берём тот блок, что с
    // разметкой, и блок кода: у первого есть href, у второго info.
    for (const Block& b : from.blocks) home.blocks.push_back(home.adopt(from, b));
    home.validate();

    ZT_EQ("своё на месте", "своё", home.text(home.blocks[0]));
    for (size_t i = 0; i < from.blocks.size(); ++i) {
        const Block& src = from.blocks[i];
        const Block& dst = home.blocks[i + 1];
        ZT_EQ("текст переехал", from.text(src), home.text(dst));
        ZT_EQ("info переехала", from.info(src), home.info(dst));
        ZT_TRUE("число спанов то же",
                from.inlines(src).size() == home.inlines(dst).size());
        for (size_t k = 0; k < from.inlines(src).size(); ++k) {
            const Inline& a = from.inlines(src)[k];
            const Inline& b = home.inlines(dst)[k];
            ZT_TRUE("начертание то же", a.flags == b.flags);
            ZT_TRUE("смещение спана относительное — не менялось", a.text == b.text);
            ZT_EQ("href переехал", from.href(a), home.href(b));
            ZT_EQ("title переехал", from.title(a), home.title(b));
        }
    }

    // Вывод должен совпасть: перенос — это про представление, не про смысл.
    Document plain;
    for (const Block& b : from.blocks) plain.blocks.push_back(plain.adopt(from, b));
    plain.meta = from.meta;
    ZT_EQ("сериализация после переноса та же", zametti::serialize(from),
          zametti::serialize(plain));
    ZT_EQ("и дамп IR тот же", zametti::toJson(from), zametti::toJson(plain));

    // Перенос внутрь себя — тоже законный случай: байты берутся из той же арены.
    Document self = zametti::parse("абзац с **разметкой**\n");
    const std::string selfText(self.text(self.blocks[0]));
    const Range selfSpan = self.inlines(self.blocks[0])[0].text;
    self.blocks.push_back(self.adopt(self, self.blocks[0]));
    self.validate();
    ZT_EQ("оригинал не испортился", selfText, self.text(self.blocks[0]));
    ZT_TRUE("и его спан на месте", self.inlines(self.blocks[0])[0].text == selfSpan);
    ZT_EQ("копия читается так же", self.text(self.blocks[0]), self.text(self.blocks[1]));
    ZT_EQ("и её разметка тоже", self.text(self.blocks[0], self.inlines(self.blocks[0])[0]),
          self.text(self.blocks[1], self.inlines(self.blocks[1])[0]));
}

// Document копируется тривиально, и копия ни от чего не зависит: дописать в
// оригинал — копия обязана остаться прежней.
void checkCopyIndependence() {
    Document original = zametti::parse("раз **два** три\n");
    const Document copy = original;

    const std::string copyDump = zametti::toJson(copy);
    const std::string copyText(copy.text(copy.blocks[0]));

    for (int i = 0; i < 64; ++i) original.append("мусор, от которого арена переезжает");
    original.blocks.push_back(original.newBlock(Kind::Paragraph, "новый блок"));
    original.blocks[0].text = original.append("совсем другой текст");

    ZT_EQ("дамп копии не изменился", copyDump, zametti::toJson(copy));
    ZT_EQ("текст копии не изменился", copyText, copy.text(copy.blocks[0]));
    ZT_TRUE("у копии по-прежнему один блок", copy.blocks.size() == 1);
    copy.validate();
}

// --- инвариант B: ноль реаллокаций арены ------------------------------------

std::string bigSource() {
    std::string out;
    for (int i = 0; i < 4000; ++i) {
        out += "## Заголовок " + std::to_string(i) + "\n\n";
        out += "Абзац с **жирным**, _курсивом_, `кодом` и [ссылкой](https://example.org/";
        out += std::to_string(i);
        out += ") внутри.\n\n";
        out += "- пункт раз\n- [x] задача\n  - вложенный\n\n";
        out += "```cpp\nint x" + std::to_string(i) + " = 1;\n```\n\n";
        out += "| a | b |\n|---|---|\n| 1 | 2 |\n\n";
        out += "> цитата\n\n";
    }
    return out;
}

void checkNoArenaRegrowth() {
    const std::string source = bigSource();
    const Document doc = zametti::parse(source);
    ZT_TRUE("большой источник разобрался", doc.blocks.size() > 10000);
    ZT_TRUE("арена уместилась в резерв без единой реаллокации",
            doc.chars.capacity() == zametti::arenaReserveFor(source.size()));
    ZT_TRUE("и арена не больше исходника", doc.chars.size() <= source.size());
    doc.validate();
}

// --- validate(): оба координатных пространства ------------------------------

#ifdef ZAMETTI_CAN_FORK
// Запускает порчу в отдельном процессе и говорит, упал ли он. Иначе ассерт
// уронил бы сам набор.
bool abortsOn(void (*damage)()) {
    std::fflush(stdout);
    std::fflush(stderr);
    const pid_t pid = fork();
    if (pid == 0) {
        damage();
        _exit(0);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFSIGNALED(status);
}

Document sample() { return zametti::parse("абзац с **разметкой** и [ссылкой](/a)\n"); }

void damageBlockTextPastArena() {
    Document doc = sample();
    doc.blocks[0].text.end = int32_t(doc.chars.size()) + 1;
    doc.validate();
}

void damageSpanPastBlockText() {
    Document doc = sample();
    doc.spans[0].text.end = doc.blocks[0].text.size() + 1;
    doc.validate();
}

void damageInlinesPastSpans() {
    Document doc = sample();
    doc.blocks[0].inlines.end = int32_t(doc.spans.size()) + 1;
    doc.validate();
}

void damageSharedSpans() {
    Document doc = sample();
    // Один и тот же спан у двух блоков: скопировать блок мало — надо adopt.
    doc.blocks.push_back(doc.blocks[0]);
    doc.validate();
}

void damageHrefPastArena() {
    Document doc = sample();
    for (Inline& s : doc.inlines(doc.blocks[0]))
        if (!s.href.empty()) s.href.end = int32_t(doc.chars.size()) + 1;
    doc.validate();
}

void damageRawWithKind() {
    Document doc = sample();
    doc.blocks[0].raw = true;
    doc.blocks[0].kind = Kind::Heading;
    doc.validate();
}

void healthy() {
    Document doc = sample();
    doc.validate();
}

void checkValidate() {
    ZT_TRUE("здоровый документ проверку проходит", !abortsOn(healthy));
    ZT_TRUE("текст блока за границей арены ловится", abortsOn(damageBlockTextPastArena));
    ZT_TRUE("спан за границей текста своего блока ловится", abortsOn(damageSpanPastBlockText));
    ZT_TRUE("диапазон спанов за границей spans ловится", abortsOn(damageInlinesPastSpans));
    ZT_TRUE("спан у двух блоков сразу ловится", abortsOn(damageSharedSpans));
    ZT_TRUE("href за границей арены ловится", abortsOn(damageHrefPastArena));
    ZT_TRUE("дословный кусок с родом ловится", abortsOn(damageRawWithKind));
}
#else
// Без assert проверять нечего: validate() в release не роняет по построению.
void checkValidate() {}
#endif

// --- разбор держит оба пространства ----------------------------------------

void checkParsedInvariants() {
    const char* const sources[] = {
        "",
        "просто текст\n",
        "# заголовок\n\nабзац с **жирным** и `кодом`\n\n- пункт\n- [ ] задача\n",
        "```cpp\nint main() {}\n```\n",
        "| a | b |\n|---|---|\n| 1 | 2 |\n",
        "<!-- комментарий -->\n\n<div>сырое</div>\n",
        "![подпись](путь.png \"Заголовок\")\n",
        "текст со <!-- строчным --> внутри\n",
        "> цитата\n\n> вторая\n",
        "<!-- zametti\nparent: abc\n-->\n\nтело\n",
    };
    for (const char* source : sources) {
        const Document doc = zametti::parse(source);
        doc.validate();   // упадёт ассертом, если что-то не так
        for (const Block& b : doc.blocks) {
            ZT_TRUE("текст блока внутри арены",
                    b.text.start >= 0 && b.text.end <= int32_t(doc.chars.size()));
            for (const Inline& s : doc.inlines(b)) {
                ZT_TRUE("спан внутри текста своего блока",
                        s.text.start >= 0 && s.text.end <= b.text.size());
                ZT_TRUE("href внутри арены", s.href.end <= int32_t(doc.chars.size()));
            }
        }
    }
}

}  // namespace

static int ztRunSuite() {
    checkAppend();
    checkNewBlock();
    checkReplaceByAppend();
    checkAdopt();
    checkCopyIndependence();
    checkNoArenaRegrowth();
    checkValidate();
    checkParsedInvariants();
    return zt::report("arena");
}

// Набор целиком одним TEST: тело не тронуто, argv ему собран здесь.
// Дробить на отдельные проверки — отдельная работа, по одному набору.
TEST(Arena, All) {
    EXPECT_EQ(0, ztRunSuite());
}
