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

void EditHistory::dropOldestIfNeeded() {
    while (steps_.size() > size_t(limit_)) {
        steps_.pop_front();
        --position_;
    }
}

}  // namespace zametti
