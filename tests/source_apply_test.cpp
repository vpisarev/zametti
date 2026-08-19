// НАЛОЖЕНИЕ ПРАВЛЕНОГО ИСХОДНИКА: ZDocument::applySourceText.
//
// Человек ушёл править markdown заметки как текст и вернулся. Проверяется не
// «получилось ли» — этого мало, — а ТРИ ЧИСЛА разом:
//
//   1. канон после наложения побайтово равен канону правленого текста;
//   2. кусков наложено столько, сколько мест человек тронул (а не «один кусок
//      во всю заметку»);
//   3. шагов отмены прибавилось РОВНО ОДИН, и один Ctrl+Z возвращает прежнее
//      побайтово.
//
// Первое число само по себе НИЧЕГО НЕ СТЕРЕЖЁТ: замени точечное наложение на
// полную пересборку — и оно останется зелёным. Краснеть при снятой точечности
// обязаны второе и третье, поэтому они спрашиваются в каждом случае.

#include "document.h"

#include "test_util.h"
#include "testdata.h"

#include <QDir>
#include <QFile>
#include <QString>
#include <QTextBlock>
#include <QTextCursor>

#include <cstdio>
#include <string>

namespace {

using namespace zametti;

ZDocument noteOf(const char* markdown) {
    ZDocument doc;
    doc.loadMarkdown(std::string(markdown));
    // Стек отмены сборщик гасит; живой заметке его включает вид, а здесь —
    // набор. Без этого «шагов отмены прибавилось ровно один» не проверить
    // вовсе: их не прибавляется никогда.
    doc.setUndoEnabled(true);
    return doc;
}

QString canonOf(const QString& text) {
    ZDocument probe;
    probe.loadMarkdown(text.toStdString());
    return probe.toMarkdownText();
}

// СРАВНИВАЕМ БЕЗ ВОЗВРАТА КАРЕТКИ — ровно там же, где его не считает правкой
// само наложение. CRLF внутри блока человек в тексте не видит и набрать не
// может; нетронутый блок сохраняет его, а правленый — законно теряет. Инвариант
// потому и звучит «с точностью до CR», а не «побайтово».
std::string same(const QString& text) {
    QString out = text;
    out.remove(QLatin1Char('\r'));
    return out.toStdString();
}

// Один случай матрицы: исходник до, правленый текст, сколько кусков ждём.
struct Case {
    const char* what;
    const char* before;
    const char* edited;
    int hunks;
};

void run(const Case& c) {
    ZDocument doc = noteOf(c.before);
    const QString wasCanonical = doc.toMarkdownText();
    const int stepsBefore = doc.undoSteps();

    const QString edited = QString::fromUtf8(c.edited);
    const int hunks = doc.applySourceText(edited);

    ZT_EQ(std::string(c.what) + ": кусков", std::to_string(c.hunks), std::to_string(hunks));
    if (hunks < 0) return;

    ZT_EQ(std::string(c.what) + ": канон совпал", same(canonOf(edited)),
          same(doc.toMarkdownText()));

    // ОТМЕНА ОДНИМ НАЖАТИЕМ — И МЕРИТЬ ЕЁ НАДО ПОВЕДЕНИЕМ, А НЕ СЧЁТЧИКОМ.
    // QTextDocument::availableUndoSteps() считает КОМАНДЫ, а не группы: у
    // наложения их пять (вырезать, влить, формат блока, формат знаков, шов), и
    // все пять лежат в одной скобке. Значит проверять надо так: ОДИН вызов
    // undo() вернул содержимое побайтово, И счётчик вернулся туда, где был, —
    // второй группы не осталось.
    if (c.hunks == 0) {
        ZT_EQ(std::string(c.what) + ": стек отмены не тронут", std::to_string(stepsBefore),
              std::to_string(doc.undoSteps()));
        return;
    }
    ZT_TRUE(std::string(c.what) + ": наложение попало в стек отмены",
            doc.undoSteps() > stepsBefore);
    ZT_TRUE(std::string(c.what) + ": отмена сработала", doc.undo());
    ZT_EQ(std::string(c.what) + ": и вернула прежнее побайтово", wasCanonical.toStdString(),
          doc.toMarkdownText().toStdString());
    ZT_EQ(std::string(c.what) + ": одним нажатием, без остатка", std::to_string(stepsBefore),
          std::to_string(doc.undoSteps()));
    ZT_TRUE(std::string(c.what) + ": возврат сработал", doc.redo());
    ZT_EQ(std::string(c.what) + ": и вернул правленое", same(canonOf(edited)),
          same(doc.toMarkdownText()));
}

const Case kCases[] = {
    // --- ничего не менялось: ни документа, ни ревизии, ни стека отмены -------
    {"текст тот же", "# А\n\nтекст\n", "# А\n\nтекст\n", 0},
    {"хвостовые пробелы незначащи", "# А\n\nтекст\n", "# А\n\nтекст   \n", 0},
    {"звёздочка вместо дефиса — не правка", "- пункт\n", "* пункт\n", 0},
    {"другая черта — не правка", "___\n", "---\n", 0},

    // --- одно место ---------------------------------------------------------
    {"слово в середине", "раз\n\nдва\n\nтри\n", "раз\n\nДВА\n\nтри\n", 1},
    {"первый блок", "раз\n\nдва\n\nтри\n", "РАЗ\n\nдва\n\nтри\n", 1},
    {"последний блок", "раз\n\nдва\n\nтри\n", "раз\n\nдва\n\nТРИ\n", 1},
    {"вставка абзаца в середину", "раз\n\nдва\n", "раз\n\nполтора\n\nдва\n", 1},
    {"вставка перед первым", "раз\n\nдва\n", "ноль\n\nраз\n\nдва\n", 1},
    {"дописка в конец", "раз\n\nдва\n", "раз\n\nдва\n\nтри\n", 1},
    {"удалён абзац из середины", "раз\n\nдва\n\nтри\n", "раз\n\nтри\n", 1},
    {"удалён первый", "раз\n\nдва\n\nтри\n", "два\n\nтри\n", 1},
    {"удалён последний", "раз\n\nдва\n\nтри\n", "раз\n\nдва\n", 1},

    // --- два места и склейка ------------------------------------------------
    {"две правки далеко друг от друга",
     "раз\n\nдва\n\nтри\n\nчетыре\n\nпять\n\nшесть\n\nсемь\n",
     "РАЗ\n\nдва\n\nтри\n\nчетыре\n\nпять\n\nшесть\n\nСЕМЬ\n", 2},
    {"две правки через пустую строку — один кусок", "раз\n\nдва\n",
     "РАЗ\n\nДВА\n", 1},

    // --- строение -----------------------------------------------------------
    {"абзац стал заголовком", "раз\n\nдва\n", "# раз\n\nдва\n", 1},
    {"пункт получил уровень", "- раз\n- два\n", "- раз\n  - два\n", 1},
    {"задача отмечена", "- [ ] дело\n\nхвост\n", "- [x] дело\n\nхвост\n", 1},
    {"вставка пункта в нумерованный список",
     "1. раз\n2. два\n3. три\n", "1. раз\n2. полтора\n3. два\n4. три\n", 1},

    // --- блок кода ----------------------------------------------------------
    {"строка внутри блока кода", "```cpp\nint a = 1;\nint b = 2;\n```\n\nхвост\n",
     "```cpp\nint a = 1;\nint b = 3;\n```\n\nхвост\n", 1},
    {"забор пропал — остаток заметки стал кодом", "```\nкод\n```\n\nхвост\n",
     "```\nкод\n\nхвост\n", 1},

    // --- всё сразу ----------------------------------------------------------
    {"заметка переписана целиком", "раз\n\nдва\n", "совсем\n\nдругое\n\nсодержимое\n", 1},
    {"заметка опустела", "раз\n\nдва\n", "", 1},
    {"пустая заметка наполнилась", "", "раз\n\nдва\n", 1},

    // --- невидимые знаки ---------------------------------------------------
    //
    // CRLF внутри блока человек в тексте не видит и набрать не может: плоский
    // виджет превращает его в LF. Блок, отличающийся ТОЛЬКО этим, обязан
    // считаться нетронутым — иначе один заход в режим съедал бы байты.
    {"CRLF внутри формулы — не правка", "$$\\begin{aligned}\r\na=b\r\n\\end{aligned}$$\n",
     "$$\\begin{aligned}\na=b\n\\end{aligned}$$\n", 0},
    {"а рядом с настоящей правкой — кусок один",
     "текст\n\n$$\\begin{aligned}\r\na=b\r\n\\end{aligned}$$\n",
     "ТЕКСТ\n\n$$\\begin{aligned}\na=b\n\\end{aligned}$$\n", 1},

    // --- шапку молча съедать нельзя ----------------------------------------
    {"шапка в тексте отвергается", "раз\n",
     "<!-- zametti\nparent: x\n-->\n\nраз\n", -1},
};

// Наложить одно и то же дважды: второй раз менять нечего.
// И ПРЯМО: возврат каретки в НЕТРОНУТОМ блоке обязан уцелеть, а в тронутом —
// законно исчезнуть. Без этой проверки «с точностью до CR» звучало бы как
// разрешение их терять.
void checkReturnsSurvive() {
    ZDocument doc = noteOf("текст\n\n$$\\begin{aligned}\r\na=b\r\n\\end{aligned}$$\n");
    ZT_TRUE("CR в заметке есть", doc.toMarkdownText().contains(QLatin1Char('\r')));
    doc.applySourceText(QStringLiteral("ТЕКСТ\n\n$$\\begin{aligned}\na=b\n\\end{aligned}$$\n"));
    ZT_TRUE("CR нетронутого блока уцелел", doc.toMarkdownText().contains(QLatin1Char('\r')));
    ZT_TRUE("а правка легла", doc.toMarkdownText().contains(QStringLiteral("ТЕКСТ")));

    // Тронули сам блок с CR — и он законно ушёл: человек перенабрал строку.
    ZDocument other = noteOf("$$\\begin{aligned}\r\na=b\r\n\\end{aligned}$$\n");
    other.applySourceText(QStringLiteral("$$\\begin{aligned}\na=c\n\\end{aligned}$$\n"));
    ZT_TRUE("в перенабранном блоке CR не осталось",
            !other.toMarkdownText().contains(QLatin1Char('\r')));
}

void checkTwice() {
    ZDocument doc = noteOf("раз\n\nдва\n");
    const QString edited = QStringLiteral("раз\n\nДВА\n");
    ZT_EQ("первый раз — кусок", std::string("1"), std::to_string(doc.applySourceText(edited)));
    const int steps = doc.undoSteps();
    ZT_EQ("второй раз — ничего", std::string("0"), std::to_string(doc.applySourceText(edited)));
    ZT_EQ("и стек отмены не тронут", std::to_string(steps), std::to_string(doc.undoSteps()));
}

// Каретка встаёт на первый наложенный кусок: виду есть куда прокрутить.
void checkCaretGoesToFirstHunk() {
    ZDocument doc = noteOf("раз\n\nдва\n\nтри\n");
    QTextCursor caret;
    ZT_EQ("кусок один", std::string("1"),
          std::to_string(doc.applySourceText(QStringLiteral("раз\n\nДВА\n\nтри\n"), &caret)));
    ZT_EQ("каретка на тронутом блоке", std::string("2"),
          std::to_string(caret.block().blockNumber()));
}

// Тронутое накладывается, НЕТРОНУТОЕ НЕ ПЕРЕСОБИРАЕТСЯ. Спрашиваем это у самих
// блоков: у нетронутого обязан остаться его прежний формат знаков — полная
// пересборка сделала бы его новым объектом с теми же свойствами, а вот
// РЕВИЗИЯ документа выросла бы одинаково, и по ней это не различить.
void checkUntouchedStaysPut() {
    ZDocument doc = noteOf("# заголовок\n\nраз\n\nдва\n");
    const std::vector<BlockInfo> before = doc.blocks();
    ZT_EQ("кусок один", std::string("1"),
          std::to_string(doc.applySourceText(QStringLiteral("# заголовок\n\nраз\n\nДВА\n"))));
    const std::vector<BlockInfo> after = doc.blocks();
    ZT_EQ("блоков столько же", std::to_string(before.size()), std::to_string(after.size()));
    if (before.size() != after.size()) return;
    for (size_t i = 0; i + 1 < before.size(); ++i)
        ZT_EQ("нетронутый блок " + std::to_string(i) + " цел",
              before[i].text.toStdString(), after[i].text.toStdString());
    ZT_TRUE("а последний правда изменился",
            before.back().text != after.back().text);
}

// НЕПОДВИЖНАЯ ТОЧКА НА ЗАМЕТКАХ ВЛАДЕЛЬЦА: наложить собственный канон — значит
// не изменить ничего. Это не украшение матрицы, а её основание: режим правки
// исходника показывает человеку РОВНО toMarkdownText(), и если такой текст,
// наложенный обратно, что-то меняет, то один заход в режим и выход из него
// молча переписывают заметку.
void checkOwnerNotesAreFixedPoints() {
    const QString dir = zt::TestData::corpus(QStringLiteral("owner-copy"));
    if (dir.isEmpty()) {
        std::printf("owner-copy: корпуса нет, неподвижная точка не проверена\n");
        return;
    }
    QDir d(dir);
    int checked = 0;
    for (const QString& name : d.entryList({QStringLiteral("*.md")}, QDir::Files, QDir::Name)) {
        QFile f(d.filePath(name));
        if (!f.open(QIODevice::ReadOnly)) continue;
        const QByteArray bytes = f.readAll();
        ZDocument doc;
        doc.loadMarkdown(std::string(bytes.constData(), size_t(bytes.size())));
        const QString canonical = doc.toMarkdownText();
        const int hunks = doc.applySourceText(canonical);
        ZT_EQ("свой канон ничего не меняет: " + name.toStdString(), std::string("0"),
              std::to_string(hunks));
        if (hunks != 0)
            ZT_EQ("  и вот чем разошлось: " + name.toStdString(), canonical.toStdString(),
                  doc.toMarkdownText().toStdString());
        ++checked;
    }
    std::printf("неподвижная точка проверена на %d заметках владельца\n", checked);
}

}  // namespace

TEST(SourceApply, All) {
    for (const Case& c : kCases) run(c);
    checkTwice();
    checkReturnsSurvive();
    checkCaretGoesToFirstHunk();
    checkUntouchedStaysPut();
    checkOwnerNotesAreFixedPoints();
}
