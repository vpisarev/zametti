#include "doc_model.h"

#include <QTextBlock>
#include <QTextDocument>

namespace zametti {

bool isRawBlock(const QTextBlock& block) {
    return block.blockFormat().boolProperty(RawProperty);
}

bool isContinuationBlock(const QTextBlock& block) {
    return block.blockFormat().boolProperty(ContinuationProperty);
}

bool isVSpaceBlock(const QTextBlock& block) {
    return !isRawBlock(block) && kindOf(block) == Kind::VSpace;
}

bool blocksWouldMerge(const QTextBlock& previous, const QTextBlock& next) {
    if (!previous.isValid() || !next.isValid()) return false;
    // wouldMerge смотрит только на род и на дословность — большего для этого
    // вопроса и не нужно, поэтому обходимся заготовками, а не читаем блоки
    // целиком.
    auto stub = [](const QTextBlock& block) {
        Block out;
        if (isRawBlock(block)) out.rawSource = " ";
        else out.kind = kindOf(block);
        return out;
    };
    return wouldMerge(stub(previous), stub(next));
}

Kind kindOf(const QTextBlock& block) {
    // Блок без свойства — не дословный кусок, а обычный абзац: так выглядят
    // блоки, которые Qt завёл сам, помимо сборщика.
    return static_cast<Kind>(block.blockFormat().intProperty(KindProperty));
}

int levelOf(const QTextBlock& block) {
    // Отсутствие свойства и есть «вне списка»: нулевой уровень — настоящий,
    // это верхний уровень списка, и путать их нельзя.
    const QTextBlockFormat format = block.blockFormat();
    return format.hasProperty(LevelProperty) ? format.intProperty(LevelProperty) : -1;
}

bool isListBlock(const QTextBlock& block) {
    return !isRawBlock(block) && isList(kindOf(block));
}

int irIndexOfBlock(const QTextBlock& block) {
    // Считаем начала логических блоков до этого места включительно, а номер —
    // на единицу меньше. Начинать с нуля и считать только предыдущие нельзя:
    // строка-продолжение получила бы номер следующего блока IR, а не своего.
    int index = isContinuationBlock(block) ? -1 : 0;
    for (QTextBlock prev = block.previous(); prev.isValid(); prev = prev.previous())
        if (!isContinuationBlock(prev)) ++index;
    return index;
}

QTextBlock blockForIrIndex(const QTextDocument& doc, int index) {
    int seen = 0;
    for (QTextBlock block = doc.begin(); block.isValid(); block = block.next()) {
        if (isContinuationBlock(block)) continue;
        if (seen == index) return block;
        ++seen;
    }
    return QTextBlock();
}

MarkerStyle markerOf(const QTextBlock& block) {
    const QTextBlockFormat format = block.blockFormat();
    return {static_cast<Marker>(format.intProperty(MarkerProperty)),
            format.boolProperty(CheckedProperty)};
}

bool isTaskBlock(const QTextBlock& block) {
    return isListBlock(block) && markerOf(block).marker == Marker::Task;
}

bool isOrderedBlock(const QTextBlock& block) {
    return isListBlock(block) && markerOf(block).marker == Marker::Ordered;
}

int ordinalOf(const QTextBlock& block) {
    if (!isListBlock(block)) return 0;
    const int level = levelOf(block);
    const bool ordered = isOrderedBlock(block);

    int ordinal = 1;
    for (QTextBlock prev = block.previous(); prev.isValid(); prev = prev.previous()) {
        if (isVSpaceBlock(prev)) continue;           // просторный список — всё тот же список
        if (!isListBlock(prev)) {
            // Блок внутри пункта — второй абзац, код — список не заканчивает:
            // нумерация за ним продолжается. Так же смотрит и сериализатор.
            if (levelOf(prev) >= 0) continue;
            break;                                   // абзац или дословный кусок рвёт прогон
        }
        const int prevLevel = levelOf(prev);
        if (prevLevel > level) continue;             // вложенный подсписок прогон не рвёт
        if (prevLevel < level) break;                // вышли из своего уровня
        if (isOrderedBlock(prev) != ordered) break;
        ++ordinal;
    }
    return ordinal;
}

void ListRuns::reset() {
    for (Level& level : levels_) level.alive = false;
}

bool ListRuns::startsNewRun(int level, bool ordered) const {
    return level >= 0 && size_t(level) < levels_.size() && levels_[size_t(level)].alive &&
           levels_[size_t(level)].ordered != ordered;
}

int ListRuns::next(int level, bool ordered) {
    if (level < 0) level = 0;
    const size_t index = size_t(level);
    if (levels_.size() <= index + 1) levels_.resize(index + 2);

    Level& own = levels_[index];
    own.ordinal = (own.alive && own.ordered == ordered) ? own.ordinal + 1 : 1;
    own.alive = true;
    own.ordered = ordered;
    // Всё, что глубже, закончилось вместе с предыдущим пунктом этого уровня.
    for (size_t k = index + 1; k < levels_.size(); ++k) levels_[k].alive = false;
    return own.ordinal;
}

}  // namespace zametti
