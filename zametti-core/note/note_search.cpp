#include "note_search.h"

#include "doc_model.h"

#include <QTextBlock>
#include <QTextDocument>

#include <algorithm>

namespace zametti {

int NoteSearch::find(const QTextDocument& doc, const QString& text, bool caseSensitive) {
    text_ = text;
    caseSensitive_ = caseSensitive;
    hits_.clear();
    current_ = -1;
    doc_ = &doc;
    revision_ = doc.revision();
    if (text.isEmpty()) return 0;
    const Qt::CaseSensitivity sensitivity = caseSensitive ? Qt::CaseSensitive : Qt::CaseInsensitive;
    QTextDocument* mutableDoc = const_cast<QTextDocument*>(&doc);
    // Блок за блоком, тем же текстом, что и ZDocument::find: совпадение не
    // пересекает границу блока, а объект отдаёт исходник.
    for (QTextBlock block = doc.begin(); block.isValid(); block = block.next()) {
        bool inObject = false;
        const QString body = searchableTextOf(block, &inObject);
        if (body.isEmpty()) continue;
        qsizetype at = body.indexOf(text, 0, sensitivity);
        while (at >= 0) {
            SearchHit hit;
            hit.cursor = QTextCursor(mutableDoc);
            if (inObject) {
                // Курсор — над самим знаком объекта; место внутри — числами.
                hit.cursor.setPosition(block.position());
                hit.cursor.setPosition(block.position() + 1, QTextCursor::KeepAnchor);
                hit.innerOffset = int(at);
                hit.innerLength = int(text.size());
            } else {
                hit.cursor.setPosition(block.position() + int(at));
                hit.cursor.setPosition(block.position() + int(at) + int(text.size()),
                                       QTextCursor::KeepAnchor);
            }
            hits_.push_back(hit);
            // Со следующего знака после НАЧАЛА совпадения: перекрывающиеся
            // вхождения тоже вхождения.
            at = body.indexOf(text, at + 1, sensitivity);
        }
    }
    return count();
}

void NoteSearch::clear() {
    hits_.clear();
    current_ = -1;
    text_.clear();
    doc_ = nullptr;
    revision_ = -1;
}

bool NoteSearch::isFreshFor(const QTextDocument& doc, const QString& text,
                            bool caseSensitive) const {
    return doc_ == &doc && revision_ == doc.revision() && text_ == text &&
           caseSensitive_ == caseSensitive;
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
        if (hits_[i].cursor.selectionStart() >= position) return int(i);
    return -1;
}

int NoteSearch::nearestBackward(int position) const {
    for (size_t i = hits_.size(); i-- > 0;)
        if (hits_[i].cursor.selectionEnd() <= position) return int(i);
    return -1;
}

int NoteSearch::indexOfSelection(int from, int to) const {
    if (to <= from) return -1;
    for (size_t i = 0; i < hits_.size(); ++i)
        if (hits_[i].cursor.selectionStart() == from && hits_[i].cursor.selectionEnd() == to)
            return int(i);
    return -1;
}

std::pair<int, int> NoteSearch::range(int from, int to) const {
    const auto lower = std::lower_bound(
        hits_.begin(), hits_.end(), from,
        [](const SearchHit& match, int position) { return match.cursor.selectionEnd() < position; });
    const auto upper = std::upper_bound(
        hits_.begin(), hits_.end(), to,
        [](int position, const SearchHit& match) { return position < match.cursor.selectionStart(); });
    return {int(lower - hits_.begin()), int(upper - hits_.begin())};
}

}  // namespace zametti
