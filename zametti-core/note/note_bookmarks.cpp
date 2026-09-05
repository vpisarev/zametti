#include "note_bookmarks.h"

#include "note_id.h"

#include <QTextBlock>
#include <algorithm>
#include <cstdlib>
#include <map>

namespace zametti {

QString NoteBookmarks::snippetOf(const QString& blockText) {
    QString out;
    out.reserve(ZBookmarks::kSnippetChars + 1);
    bool space = true;   // leading spaces fold away
    for (const QChar ch : blockText) {
        if (ch == QChar::ObjectReplacementCharacter) continue;
        const bool blank = ch.isSpace() || ch == QChar::LineSeparator;
        if (blank) {
            if (space) continue;
            space = true;
            out.append(QLatin1Char(' '));
        } else {
            space = false;
            out.append(ch);
        }
        if (out.size() >= ZBookmarks::kSnippetChars) break;
    }
    while (!out.isEmpty() && out.back() == QLatin1Char(' ')) out.chop(1);
    return out;
}

QTextCursor NoteBookmarks::anchorAt(ZDocument& doc, int block) const {
    QTextCursor cursor = doc.caretAtBlock(block);
    cursor.setKeepPositionOnInsert(true);
    return cursor;
}

void NoteBookmarks::resolve(const std::vector<ZBookmarks::Entry>& entries, ZDocument& doc) {
    anchors_.clear();
    if (entries.empty()) return;
    // One walk over the blocks for all entries: the snippets wanted, and the
    // blocks that carry each of them.
    std::map<QString, std::vector<int>> found;
    for (const ZBookmarks::Entry& e : entries) found[e.snippet];
    const int count = doc.blockCount();
    for (int i = 0; i < count; ++i) {
        const QString snippet = snippetOf(doc.blockAt(i).text);
        if (snippet.isEmpty()) continue;
        auto it = found.find(snippet);
        if (it != found.end()) it->second.push_back(i);
    }
    // The line of a block is asked only when a snippet is found more than
    // once: the map of lines is a walk of its own.
    std::vector<SourceLine> lines;
    const auto lineOf = [&](int block) {
        if (lines.empty()) lines = doc.sourceLines();
        for (size_t i = 0; i < lines.size(); ++i)
            if (lines[i].block == block) return int(i) + 1;
        return 0;
    };
    for (const ZBookmarks::Entry& e : entries) {
        Anchor a;
        a.entry = e;
        const std::vector<int>& blocks = found[e.snippet];
        if (!blocks.empty() && !e.snippet.isEmpty()) {
            int best = blocks.front();
            if (blocks.size() > 1) {
                int distance = -1;
                for (int b : blocks) {
                    const int d = std::abs(lineOf(b) - e.line);
                    if (distance < 0 || d < distance) {
                        distance = d;
                        best = b;
                    }
                }
            }
            a.cursor = anchorAt(doc, best);
        }
        anchors_.push_back(std::move(a));
    }
}

bool NoteBookmarks::repair(ZDocument& doc) {
    bool stale = false;
    for (const Anchor& a : anchors_) {
        if (a.lost()) continue;
        QTextCursor point = a.cursor;
        point.setPosition(point.position());
        if (snippetOf(point.block().text()) != a.entry.snippet) {
            stale = true;
            break;
        }
    }
    if (!stale) return false;
    std::vector<ZBookmarks::Entry> entries;
    for (const Anchor& a : anchors_) entries.push_back(a.entry);
    resolve(entries, doc);
    return true;
}

const NoteBookmarks::Anchor* NoteBookmarks::at(int block) const {
    for (const Anchor& a : anchors_)
        if (!a.lost() && a.block() == block) return &a;
    return nullptr;
}

const NoteBookmarks::Anchor* NoteBookmarks::byId(const QString& id) const {
    for (const Anchor& a : anchors_)
        if (a.entry.id == id) return &a;
    return nullptr;
}

std::vector<int> NoteBookmarks::blocks() const {
    std::vector<int> out;
    for (const Anchor& a : anchors_)
        if (!a.lost()) out.push_back(a.block());
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

int NoteBookmarks::next(int block) const {
    int best = -1;
    for (int b : blocks())
        if (b > block && (best < 0 || b < best)) best = b;
    return best;
}

int NoteBookmarks::previous(int block) const {
    int best = -1;
    for (int b : blocks())
        if (b < block && b > best) best = b;
    return best;
}

ZBookmarks::Entry NoteBookmarks::add(ZDocument& doc, const QString& noteId, int block,
                                     const QString& nowIso) {
    ZBookmarks::Entry e;
    e.id = QString::fromStdString(newNoteId());
    e.note = noteId;
    e.snippet = snippetOf(doc.blockAt(block).text);
    e.heading = doc.headingAbove(block);
    e.line = doc.sourcePosOf(doc.caretAtBlock(block)).line;
    e.created = nowIso;
    e.updated = nowIso;
    Anchor a;
    a.entry = e;
    a.cursor = anchorAt(doc, block);
    anchors_.push_back(std::move(a));
    return e;
}

bool NoteBookmarks::drop(const QString& id) {
    const auto it = std::find_if(anchors_.begin(), anchors_.end(),
                                 [&id](const Anchor& a) { return a.entry.id == id; });
    if (it == anchors_.end()) return false;
    anchors_.erase(it);
    return true;
}

std::vector<ZBookmarks::Entry> NoteBookmarks::healed(ZDocument& doc, const QString& nowIso) {
    std::vector<ZBookmarks::Entry> changed;
    for (Anchor& a : anchors_) {
        if (a.lost()) continue;
        const int block = a.block();
        ZBookmarks::Entry fresh = a.entry;
        fresh.snippet = snippetOf(doc.blockAt(block).text);
        fresh.heading = doc.headingAbove(block);
        fresh.line = doc.sourcePosOf(doc.caretAtBlock(block)).line;
        if (fresh.snippet.isEmpty()) continue;   // an empty paragraph names nothing; leave the record
        if (fresh.snippet == a.entry.snippet && fresh.heading == a.entry.heading &&
            fresh.line == a.entry.line)
            continue;
        fresh.updated = nowIso;
        a.entry = fresh;
        changed.push_back(fresh);
    }
    return changed;
}

}  // namespace zametti
