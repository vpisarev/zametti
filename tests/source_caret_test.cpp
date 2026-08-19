// КАРЕТКА МЕЖДУ ДОКУМЕНТОМ И ИСХОДНИКОМ: sourcePosOf / cursorAtSourcePos.
//
// Нажал [M] — каретка обязана остаться там же, где была; нажал ещё раз —
// вернуться. Точность названа в document.h и проверяется здесь ДВУМЯ разными
// способами, потому что каждый ловит своё.
//
//   1. ИМЕНОВАННЫЕ СЛУЧАИ — что именно человек увидит: строка заголовка, строка
//      внутри блока кода, строка забора, объект.
//   2. ОБХОД ВСЕХ ПОЗИЦИЙ — свойство: где правило считает себя точным
//      (exact), туда и обратно обязано давать ТУ ЖЕ позицию документа. Обход
//      находит то, чего в именованных случаях нет по построению: их придумывает
//      тот же человек, который писал правило.

#include "document.h"

#include "test_util.h"

#include <QString>
#include <QTextBlock>
#include <QTextCursor>

#include <string>

namespace {

using namespace zametti;

ZDocument noteOf(const char* markdown) {
    ZDocument doc;
    doc.loadMarkdown(std::string(markdown));
    return doc;
}

// Каретка на блоке block со смещением offset.
QTextCursor caretAt(ZDocument& doc, int block, int offset) {
    QTextCursor at = doc.caretAtBlock(block);
    at.setPosition(at.position() + offset);
    return at;
}

struct Spot {
    const char* what;
    int block;
    int offset;
    int line;      // ждём эту строку исходника
    int column;    // и эту колонку; −1 — колонку не спрашиваем
    bool exact;
};

void checkSpots(const char* source, const Spot* spots, size_t count) {
    ZDocument doc = noteOf(source);
    for (size_t i = 0; i < count; ++i) {
        const Spot& s = spots[i];
        const SourcePos pos = doc.sourcePosOf(caretAt(doc, s.block, s.offset));
        ZT_EQ(std::string(s.what) + ": строка", std::to_string(s.line), std::to_string(pos.line));
        ZT_EQ(std::string(s.what) + ": точность", std::string(s.exact ? "точно" : "не точно"),
              std::string(pos.exact ? "точно" : "не точно"));
        if (s.column >= 0)
            ZT_EQ(std::string(s.what) + ": колонка", std::to_string(s.column),
                  std::to_string(pos.column));
    }
}

// СВОЙСТВО: туда и обратно. Обходим ВСЕ позиции документа; где правило считает
// себя точным, обратный перевод обязан вернуть ту же позицию.
void checkRoundTrip(const char* what, const char* source) {
    ZDocument doc = noteOf(source);
    int checked = 0;
    int inexact = 0;
    for (int block = 0; block < doc.blockCount(); ++block) {
        const QTextCursor head = doc.caretAtBlock(block);
        const int length = head.block().length();
        for (int offset = 0; offset < length; ++offset) {
            const QTextCursor at = caretAt(doc, block, offset);
            const SourcePos pos = doc.sourcePosOf(at);
            if (!pos.exact) {
                // Колонки нет — но СТРОКА обязана быть верной и там: обратный
                // перевод возвращает каретку в тот же блок, а не в соседний.
                ZT_EQ(std::string(what) + ": неточный блок " + std::to_string(block) +
                          " смещение " + std::to_string(offset) + " остался своим блоком",
                      std::to_string(block),
                      std::to_string(doc.cursorAtSourcePos(pos).block().blockNumber()));
                ++inexact;
                continue;
            }
            const QTextCursor back = doc.cursorAtSourcePos(pos);
            ZT_EQ(std::string(what) + ": блок " + std::to_string(block) + " смещение " +
                      std::to_string(offset) + " вернулось на место",
                  std::to_string(at.position()), std::to_string(back.position()));
            ++checked;
        }
    }
    ZT_TRUE(std::string(what) + ": проверено " + std::to_string(checked) + " точных и " +
                std::to_string(inexact) + " неточных позиций",
            checked + inexact > 0);
}

// Заголовок, абзац, пустая строка.
const Spot kProse[] = {
    {"начало заголовка", 0, 0, 0, 2, true},        // "# А" — решётка и пробел слева
    {"конец заголовка", 0, 1, 0, 3, true},
    {"пустая строка", 1, 0, 1, 0, true},
    {"начало абзаца", 2, 0, 2, 0, true},
    {"середина абзаца", 2, 3, 2, 3, true},
};

// Список: маркер в текст блока не входит, и колонка сдвинута на него.
const Spot kList[] = {
    {"начало пункта", 0, 0, 0, 2, true},           // "- раз"
    {"начало вложенного", 1, 0, 1, 4, true},       // "  - два"
    {"начало задачи", 2, 0, 2, 6, true},           // "- [ ] дело"
};

// Блок кода лежит ОДНИМ блоком; забор — строки, которых в блоке нет.
const Spot kCode[] = {
    {"первая строка кода", 0, 0, 1, 0, true},
    {"начало второй строки кода", 0, 4, 2, 0, true},
    {"середина второй строки", 0, 5, 2, 1, true},
};

// Разметка внутри строки: знаки строки и знаки блока — разные знаки.
const Spot kMarkup[] = {
    {"перед жирным", 0, 0, 0, 0, false},
    {"после жирного", 0, 8, 0, -1, false},
};

}  // namespace

TEST(SourceCaret, All) {
    checkSpots("# А\n\nтекст\n", kProse, std::size(kProse));
    checkSpots("- раз\n  - два\n- [ ] дело\n", kList, std::size(kList));
    checkSpots("```cpp\nраз\nдва\n```\n", kCode, std::size(kCode));
    checkSpots("**жирный** хвост\n", kMarkup, std::size(kMarkup));

    checkRoundTrip("проза", "# Заголовок\n\nобычный абзац\n\nи ещё один\n");
    checkRoundTrip("списки", "- раз\n  - два\n    - три\n- [ ] дело\n- [x] сделано\n");
    checkRoundTrip("код", "перед\n\n```cpp\nint a = 1;\nint b = 2;\n```\n\nпосле\n");
    checkRoundTrip("цитата и черта", "> цитата\n\n---\n\nхвост\n");
    checkRoundTrip("разметка", "**жирный** и _курсив_ и `код` и [ссылка](адрес)\n");
    checkRoundTrip("формулы", "текст с $a+b$ внутри\n\n$$x^2$$\n\nхвост\n");
    checkRoundTrip("картинка", "текст\n\n![подпись](фото.jpg)\n\nхвост\n");
    checkRoundTrip("таблица", "| а | б |\n|---|---|\n| 1 | 2 |\n\nхвост\n");
    checkRoundTrip("пустая заметка", "");
}
