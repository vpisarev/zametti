#include "note_search.h"

#include "doc_model.h"
#include "search.h"

#include <QTextBlock>
#include <QTextDocument>

#include <algorithm>

namespace zametti {

int NoteSearch::find(const QTextDocument& doc, const Query& query) {
    query_ = query;
    hits_.clear();
    current_ = -1;
    doc_ = &doc;
    revision_ = doc.revision();

    QTextDocument* mutableDoc = const_cast<QTextDocument*>(&doc);
    // Обход — общий (forEachHit): те же правила про объекты, про пустое
    // совпадение и про шаг, что у ZDocument::find. Здесь только своё — курсор
    // над найденным, который поедет с правками, как всякий курсор Qt.
    forEachHit(doc, query, [&](const HitPlace& place) {
        SearchHit hit;
        hit.cursor = QTextCursor(mutableDoc);
        if (place.match != nullptr) hit.match = *place.match;
        const QTextBlock& block = *place.block;
        if (place.inObject) {
            // Курсор — над самим знаком объекта; место внутри — числами.
            hit.cursor.setPosition(block.position());
            hit.cursor.setPosition(block.position() + 1, QTextCursor::KeepAnchor);
            hit.innerOffset = place.offset;
            hit.innerLength = place.length;
        } else if (place.span >= 0) {
            // Внутри СТРОЧНОГО объекта: курсор над его знаком, смещение —
            // в его исходнике; подсветка ляжет на вёрстку.
            const ObjectSpan& own = (*place.objects)[size_t(place.span)];
            hit.cursor.setPosition(own.position);
            hit.cursor.setPosition(own.position + 1, QTextCursor::KeepAnchor);
            hit.innerOffset = place.offset - own.from;
            hit.innerLength = place.length;
        } else {
            const int from = docPositionOf(block, *place.objects, place.offset);
            const int to = docPositionOf(block, *place.objects, place.offset + place.length);
            hit.cursor.setPosition(from);
            hit.cursor.setPosition(to, QTextCursor::KeepAnchor);
        }
        hits_.push_back(hit);
        return true;
    });
    return count();
}

void NoteSearch::clear() {
    hits_.clear();
    current_ = -1;
    query_ = Query{};
    doc_ = nullptr;
    revision_ = -1;
}

bool NoteSearch::isFreshFor(const QTextDocument& doc, const Query& query) const {
    // Признак выражения — часть свежести: щёлкнули тумблер, и найденное
    // прежним запросом больше не годится, хотя буквы в поле те же.
    return doc_ == &doc && revision_ == doc.revision() && query_.needle == query.needle &&
           query_.caseSensitive == query.caseSensitive && query_.regex == query.regex;
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
