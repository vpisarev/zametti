#include "note_search.h"

#include <QTextDocument>

#include <algorithm>

namespace zametti {

int NoteSearch::find(const QTextDocument& doc, const QString& text, bool caseSensitive) {
    text_ = text;
    caseSensitive_ = caseSensitive;
    hits_.clear();
    current_ = -1;
    if (text.isEmpty()) return 0;
    QTextDocument::FindFlags flags;
    if (caseSensitive) flags |= QTextDocument::FindCaseSensitively;
    QTextCursor at(const_cast<QTextDocument*>(&doc));
    while (true) {
        at = doc.find(text, at, flags);
        if (at.isNull()) break;
        hits_.push_back(at);
        // Со следующего знака после НАЧАЛА совпадения: перекрывающиеся
        // вхождения тоже вхождения.
        QTextCursor next(const_cast<QTextDocument*>(&doc));
        next.setPosition(at.selectionStart() + 1);
        if (next.position() >= doc.characterCount() - 1) break;
        at = next;
    }
    return count();
}

void NoteSearch::clear() {
    hits_.clear();
    current_ = -1;
    text_.clear();
}

void NoteSearch::setCurrent(int index) {
    if (hits_.empty()) {
        current_ = -1;
        return;
    }
    const int n = count();
    current_ = ((index % n) + n) % n;
}

int NoteSearch::nearestForward(int position) const {
    for (size_t i = 0; i < hits_.size(); ++i)
        if (hits_[i].selectionStart() >= position) return int(i);
    return -1;
}

int NoteSearch::nearestBackward(int position) const {
    for (size_t i = hits_.size(); i-- > 0;)
        if (hits_[i].selectionEnd() <= position) return int(i);
    return -1;
}

std::pair<int, int> NoteSearch::range(int from, int to) const {
    const auto lower = std::lower_bound(
        hits_.begin(), hits_.end(), from,
        [](const QTextCursor& match, int position) { return match.selectionEnd() < position; });
    const auto upper = std::upper_bound(
        hits_.begin(), hits_.end(), to,
        [](int position, const QTextCursor& match) { return position < match.selectionStart(); });
    return {int(lower - hits_.begin()), int(upper - hits_.begin())};
}

}  // namespace zametti
