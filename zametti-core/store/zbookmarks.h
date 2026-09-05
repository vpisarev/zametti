// ZBookmarks — the bookmarks of a store, one file for all its notes (brief 18,
// owner's decision 05.09.2026: a bookmark must not rewrite a multi-megabyte
// book, so it lives in <store>/bookmarks.json, not in the note's header).
//
// A bookmark points at a paragraph by its TEXT — the first characters of the
// paragraph, whitespace folded — with the heading above and the source line
// as hints, never as the anchor: text survives edits above and below it, a
// line number does not. Records are never deleted in place: a removed
// bookmark keeps its id with a tombstone, so that two copies of the store can
// be merged without a deletion coming back to life (mergedWith). The file is
// human-readable JSON; keys it does not know are kept and written back.
#ifndef ZAMETTI_ZBOOKMARKS_H
#define ZAMETTI_ZBOOKMARKS_H

#include <QJsonObject>
#include <QString>
#include <vector>

namespace zametti {

class ZBookmarks {
public:
    static constexpr char kFile[] = "bookmarks.json";
    static constexpr int kFormatVersion = 1;
    // How much of a paragraph names it. Forty characters tell paragraphs
    // apart in a book and still fit a list line.
    static constexpr int kSnippetChars = 40;

    struct Entry {
        QString id;         // a fresh note-style id, minted when the bookmark is set
        QString note;       // id of the note
        QString snippet;    // the paragraph's first characters, whitespace folded
        QString heading;    // the heading above the paragraph, a hint
        int line = 0;       // the source line of the paragraph, a hint
        QString name;       // what the person called it; empty — the snippet speaks
        QString created;    // ISO-8601 with offset (store::isoNow)
        QString updated;    // when any field changed; the winner in a merge
        bool deleted = false;
        QJsonObject extra;  // keys we do not know, kept for the file
    };

    bool parse(const QByteArray& bytes, QString* error);
    QByteArray toBytes() const;

    // The live bookmarks of one note, in file order; tombstones left out.
    std::vector<Entry> forNote(const QString& noteId) const;
    const std::vector<Entry>& all() const { return entries_; }
    // Insert or replace by id; updated is stamped by the caller.
    void set(const Entry& entry);
    // Leave a tombstone: the record stays, marked, with the given stamp.
    bool remove(const QString& id, const QString& updatedIso);
    const Entry* find(const QString& id) const;
    bool empty() const { return entries_.empty(); }

    // UNION BY ID, THE LATER UPDATE WINS, A TOMBSTONE NEVER COMES BACK TO
    // LIFE: commutative and idempotent, so two copies merge the same way from
    // either side. Times compare as store::comparableTime, never as raw text.
    ZBookmarks mergedWith(const ZBookmarks& other) const;

private:
    std::vector<Entry> entries_;
    QJsonObject extra_;
};

}  // namespace zametti

#endif  // ZAMETTI_ZBOOKMARKS_H
