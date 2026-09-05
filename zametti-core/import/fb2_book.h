// A BOOK IN FB2 (FictionBook 2.0), read into the note's own blocks.
//
// The reader produces what the markdown writer takes — logical blocks with
// runs (Piece) — and never a markdown string of its own: the writer escapes,
// picks delimiters and settles edges by ITS rules, and a second set of those
// rules here would drift from the first on the first odd title. The store
// (ZStorage::importBook) writes the pieces, reads them back as a note and
// stamps the header; the pictures travel as base64 binaries and become
// attachments there too.
//
// The element mapping is the one of brief 18 §2, with what the corpus taught:
//
//   body                          the text; a `name`d body → footnote definitions
//   section, depth d              title → heading #×min(d+1, 6); no title → a rule
//   title of several <p>          joined by « · »
//   p                             paragraph;  subtitle → bold paragraph
//   empty-line                    a blank line
//   epigraph / cite / poem        ONE flat quote (the model has no nested
//                                 quotes); poem lines are soft breaks inside
//                                 a stanza, stanzas are the quote's paragraphs,
//                                 an epigraph is italic, text-author → «— Author»
//   emphasis/strong/strikethrough *…* / **…** / ~~…~~;  sub/sup → <sub>/<sup>;
//   code                          `…`
//   a[type=note] → [^id]          ids outside [A-Za-z0-9_-] are renumbered n1…
//   a                             [text](href); a local #id link stays text
//   image                         an image run whose href is the BINARY id —
//                                 the store rewrites it to the attachment name
//   table                         a GFM table, verbatim block; spans flattened
//   coverpage/image               coverId(), not inserted into the text
//   title-info/annotation         the first block of the body, as a quote
//
// Encoding: the XML declaration is honoured for what Qt decodes (UTF-8,
// UTF-16); windows-1251 is decoded by a table of our own, because ICU is off
// in the shipped Qt and QStringDecoder would not know it there.

#pragma once

#include "document_pieces.h"

#include <QByteArray>
#include <QHash>
#include <QString>
#include <QStringList>

#include <string>
#include <utility>
#include <vector>

namespace zametti {

class Fb2Book {
public:
    struct Binary {
        QString id;
        QString contentType;
        QByteArray bytes;
    };
    struct Stats {
        int sections = 0;
        int paragraphs = 0;
        int footnotes = 0;    // definitions
        int references = 0;   // [^id] in the text
        int images = 0;       // image runs in the text (the cover not counted)
        int tables = 0;
        int poems = 0;
        int renumberedIds = 0;
    };

    // Parses the file's bytes. False — not a book: the error names the line.
    bool load(const QByteArray& bytes, QString* error);

    const QString& title() const { return title_; }
    // Header lines ready for NoteHeader::set (values already safe): author,
    // translator, year, isbn, publisher, series, lang, genre — those found.
    const std::vector<std::pair<std::string, std::string>>& headerFields() const {
        return fields_;
    }
    // document-info/date as written ("2019-08-30" usually); empty — none.
    const QString& created() const { return created_; }
    // The binary id of the cover; empty — no cover.
    const QString& coverId() const { return coverId_; }

    std::vector<Piece>& pieces() { return pieces_; }
    const std::vector<Piece>& pieces() const { return pieces_; }
    const std::vector<Binary>& binaries() const { return binaries_; }
    const Binary* binary(const QString& id) const;
    // Binary ids the text refers to, in order of first use; the cover last if
    // it is not among them.
    QStringList referencedBinaries() const;
    // Image runs get the attachment file names in place of binary ids. A
    // picture whose binary is not in the map loses its run — the caption, if
    // any, stays as text. Returns how many were dropped.
    int rewriteImages(const QHash<QString, QString>& fileNames);

    const Stats& stats() const { return stats_; }

    // windows-1251 → UTF-8, the 128-entry table. Public for the tests.
    static QByteArray decodeCp1251(const QByteArray& bytes);
    // The encoding named by the XML declaration, lower-case; empty — none.
    static QString declaredEncoding(const QByteArray& bytes);

protected:
    QString title_;
    QString created_;
    QString coverId_;
    std::vector<std::pair<std::string, std::string>> fields_;
    std::vector<Piece> pieces_;
    std::vector<Binary> binaries_;
    Stats stats_;
};

}  // namespace zametti
