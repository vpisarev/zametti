#include "doc_model.h"

#include <QTextBlock>

namespace zametti {

bool isRawBlock(const QTextBlock& block) {
    return block.blockFormat().boolProperty(RawProperty);
}

Kind kindOf(const QTextBlock& block) {
    // Блок без свойства — не дословный кусок, а обычный абзац: так выглядят
    // блоки, которые Qt завёл сам, помимо сборщика.
    return static_cast<Kind>(block.blockFormat().intProperty(KindProperty));
}

int levelOf(const QTextBlock& block) {
    return block.blockFormat().intProperty(LevelProperty);
}

bool isListBlock(const QTextBlock& block) {
    return !isRawBlock(block) && isList(kindOf(block));
}

int ordinalOf(const QTextBlock& block) {
    if (!isListBlock(block)) return 0;
    const int level = levelOf(block);
    const bool ordered = isOrdered(kindOf(block));

    int ordinal = 1;
    for (QTextBlock prev = block.previous(); prev.isValid(); prev = prev.previous()) {
        if (!isListBlock(prev)) break;               // абзац или дословный кусок рвёт прогон
        const int prevLevel = levelOf(prev);
        if (prevLevel > level) continue;             // вложенный подсписок прогон не рвёт
        if (prevLevel < level) break;                // вышли из своего уровня
        if (isOrdered(kindOf(prev)) != ordered) break;
        ++ordinal;
    }
    return ordinal;
}

}  // namespace zametti
