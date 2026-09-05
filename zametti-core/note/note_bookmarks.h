// NoteBookmarks — the bookmarks of ONE note while it is open (brief 18).
//
// The store keeps bookmarks as text (ZBookmarks: the paragraph's first
// characters); an open note keeps them as QTextCursors on its live document:
// Qt moves a cursor with every edit and every undo, so a bookmark stays on
// its paragraph while the text above it grows or shrinks, and nothing has to
// listen to the document (no QTextBlockUserData, no contentsChange in the
// core — neither survives the rebuilds a note goes through). The cursor is
// kept before inserts at its own position (setKeepPositionOnInsert), or a
// paragraph rebuilt in place would push it to the paragraph's end; and it is
// read as a point (position only): the anchor half ignores that setting.
//
// A cursor is trusted only while the paragraph still begins with the snippet:
// a rebuild of the whole document sends every cursor to zero, an undo of a
// deletion re-inserts the paragraph AFTER the cursor — so the paragraph is
// checked when asked, and looked up by its text again when it does not match
// (resolve). Lookups walk the blocks: O(N), once per open and per repair, not
// on a key press.
#ifndef ZAMETTI_NOTE_BOOKMARKS_H
#define ZAMETTI_NOTE_BOOKMARKS_H

#include "document.h"
#include "zbookmarks.h"

#include <QString>
#include <QTextBlock>
#include <QTextCursor>
#include <vector>

namespace zametti {

class NoteBookmarks {
public:
    struct Anchor {
        ZBookmarks::Entry entry;
        QTextCursor cursor;   // at the paragraph's start; null when lost
        bool lost() const { return cursor.isNull(); }
        int block() const { return lost() ? -1 : cursor.block().blockNumber(); }
    };

    // Forget the anchors and find the entries in the document by their
    // snippets: the exact snippet, the candidate nearest to the remembered
    // line when there are several, lost otherwise.
    void resolve(const std::vector<ZBookmarks::Entry>& entries, ZDocument& doc);
    // Re-anchor the ones whose paragraph no longer begins with the snippet
    // (after a full rebuild or an undo); true when anything moved.
    bool repair(ZDocument& doc);
    void clear() { anchors_.clear(); }

    const std::vector<Anchor>& anchors() const { return anchors_; }
    bool empty() const { return anchors_.empty(); }
    // The anchor on the block, or null.
    const Anchor* at(int block) const;
    const Anchor* byId(const QString& id) const;
    // The next / previous bookmarked block strictly after / before the given
    // one, in document order; -1 when there is none.
    int next(int block) const;
    int previous(int block) const;
    // The bookmarked blocks in document order (lost ones left out).
    std::vector<int> blocks() const;

    // A fresh entry for the block (id minted, snippet, heading, line, stamps)
    // and its anchor added; the entry to be given to the store.
    ZBookmarks::Entry add(ZDocument& doc, const QString& noteId, int block, const QString& nowIso);
    // Drop the anchor with the id (the store gets the tombstone from the caller).
    bool drop(const QString& id);
    // The entries with snippet, heading and line rewritten from where the
    // anchors are NOW — the self-healing at save time; only the ones that
    // changed, stamped with nowIso.
    std::vector<ZBookmarks::Entry> healed(ZDocument& doc, const QString& nowIso);

    // What names a paragraph: its first characters, whitespace folded, no
    // object placeholders. The same function on both roads — writing and
    // finding — or they would disagree on the first stray space.
    static QString snippetOf(const QString& blockText);

private:
    QTextCursor anchorAt(ZDocument& doc, int block) const;
    std::vector<Anchor> anchors_;
};

}  // namespace zametti

#endif  // ZAMETTI_NOTE_BOOKMARKS_H
