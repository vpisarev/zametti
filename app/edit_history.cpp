#include "edit_history.h"

namespace zametti {

void EditHistory::reset(Document doc, int cursor) {
    steps_.clear();
    steps_.push_back({std::move(doc), cursor});
    position_ = 0;
}

void EditHistory::push(Document doc, int cursor) {
    steps_.erase(steps_.begin() + long(position_) + 1, steps_.end());
    steps_.push_back({std::move(doc), cursor});
    position_ = steps_.size() - 1;
    dropOldestIfNeeded();
}

void EditHistory::amend(Document doc, int cursor) {
    steps_.erase(steps_.begin() + long(position_) + 1, steps_.end());
    steps_[position_] = {std::move(doc), cursor};
}

const HistoryStep* EditHistory::undo() {
    if (!canUndo()) return nullptr;
    --position_;
    return &steps_[position_];
}

const HistoryStep* EditHistory::redo() {
    if (!canRedo()) return nullptr;
    ++position_;
    return &steps_[position_];
}

// Вес шага — то, что снимок держит на самом деле: арена, спаны, блоки.
// Пересчитываем целиком, а не ведём счётчик: шагов две сотни, сложение
// дешевле любой ошибки в учёте.
namespace {

size_t weigh(const Document& doc) {
    return doc.chars.size() + doc.spans.size() * sizeof(Inline) +
           doc.blocks.size() * sizeof(Block);
}

}  // namespace

size_t EditHistory::bytes() const {
    size_t total = 0;
    for (const HistoryStep& step : steps_) total += weigh(step.doc);
    return total;
}

void EditHistory::dropOldestIfNeeded() {
    while (steps_.size() > size_t(limit_) && position_ > 0) {
        steps_.pop_front();
        --position_;
    }
    // Бюджет держим сверх счёта шагов. Два шага остаются всегда: без «куда
    // откатиться» история перестаёт быть историей, каким бы тяжёлым ни был
    // единственный снимок.
    while (steps_.size() > 2 && position_ > 0 && bytes() > budget_) {
        steps_.pop_front();
        --position_;
    }
}

}  // namespace zametti
