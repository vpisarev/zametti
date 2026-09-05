#include "fb2_book.h"

#include "note_header.h"

#include <QRegularExpression>
#include <QXmlStreamReader>

#include <algorithm>

namespace zametti {
namespace {

// The local part of a qualified name: `l:href` → `href`. Namespace processing
// is off (some files bind no prefix at all, and the reader would refuse them
// whole), so names may come with prefixes.
QStringView localOf(QStringView name) {
    const qsizetype colon = name.lastIndexOf(u':');
    return colon < 0 ? name : name.mid(colon + 1);
}

QString attributeOf(const QXmlStreamReader& reader, QLatin1String local) {
    for (const QXmlStreamAttribute& a : reader.attributes())
        if (localOf(a.qualifiedName()) == local) return a.value().toString();
    return {};
}

bool isSpace(QChar c) {
    return c == u' ' || c == u'\t' || c == u'\n' || c == u'\r' || c == QChar::Nbsp ||
           c == QChar(0x0B) || c == QChar(0x0C);
}

bool footnoteIdChar(QChar ch) {
    const char16_t c = ch.unicode();
    return (c >= u'0' && c <= u'9') || (c >= u'a' && c <= u'z') || (c >= u'A' && c <= u'Z') ||
           c == u'_' || c == u'-';
}

// ONE PARAGRAPH BEING BUILT: text with runs of style. Whitespace is
// normalised as it arrives — a run of blanks becomes one space, edge blanks
// go — except the leading spaces of a verse line (keepLead), which are the
// poet's hanging indent and the reader keeps them as plain spaces.
struct Para {
    Piece piece;
    bool keepLead = false;
    bool atStart = true;
    bool pendingSpace = false;

    bool empty() const { return piece.text.isEmpty() && piece.runs.empty(); }

    void styled(int32_t from, int32_t to, uint16_t flags, const QString& href) {
        if (to <= from || (flags == 0 && href.isEmpty())) return;
        if (!piece.runs.empty()) {
            Run& last = piece.runs.back();
            if (last.end == from && last.flags == flags && last.href == href && !last.image() &&
                !last.footnote() && (flags & (InlineImage | InlineFootnote)) == 0) {
                last.end = to;
                return;
            }
        }
        Run run;
        run.start = from;
        run.end = to;
        run.flags = flags;
        run.href = href;
        piece.runs.push_back(std::move(run));
    }

    void text(QStringView s, uint16_t flags, const QString& href) {
        // The blank between two chunks belongs to neither style: `с *курсивом*`,
        // not `с* курсивом*`. It is written plain, before the styled range
        // starts — and only once a real character is about to follow it.
        bool anyInk = false;
        for (const QChar c : s)
            if (!isSpace(c)) { anyInk = true; break; }
        if (!anyInk) {
            if (atStart && keepLead) {
                for (const QChar c : s)
                    if (c != u'\n' && c != u'\r') piece.text += u' ';
            } else if (!atStart) {
                pendingSpace = true;
            }
            return;
        }
        int32_t from = int32_t(piece.text.size());
        bool leadDone = false;
        for (const QChar c : s) {
            if (isSpace(c)) {
                if (atStart && keepLead && !leadDone && c != u'\n' && c != u'\r') {
                    piece.text += u' ';
                    from = int32_t(piece.text.size());
                    continue;
                }
                if (!atStart) pendingSpace = true;
                continue;
            }
            leadDone = true;
            if (pendingSpace) {
                piece.text += u' ';
                pendingSpace = false;
                if (from == int32_t(piece.text.size()) - 1) from = int32_t(piece.text.size());
            }
            piece.text += c;
            atStart = false;
        }
        styled(from, int32_t(piece.text.size()), flags, href);
    }

    // Verbatim, no normalisation: a footnote reference, a caption.
    void literal(QStringView s, uint16_t flags, const QString& href) {
        if (pendingSpace) {
            piece.text += u' ';
            pendingSpace = false;
        }
        const int32_t from = int32_t(piece.text.size());
        piece.text += s;
        atStart = false;
        styled(from, int32_t(piece.text.size()), flags, href);
    }

    void image(const QString& binaryId, QStringView alt) {
        if (pendingSpace) {
            piece.text += u' ';
            pendingSpace = false;
        }
        const int32_t from = int32_t(piece.text.size());
        for (const QChar c : alt) piece.text += (c == u'[' || c == u']' || c == u'\n') ? u' ' : c;
        Run run;
        run.start = from;
        run.end = int32_t(piece.text.size());
        run.flags = InlineImage;
        run.href = binaryId;
        piece.runs.push_back(std::move(run));
        atStart = false;
    }

    // A new line inside the same block (a verse, a note's second paragraph).
    void newline() {
        pendingSpace = false;
        piece.text += u'\n';
        atStart = true;
    }

    Piece take(Kind kind) {
        piece.kind = kind;
        pendingSpace = false;
        // A trailing blank cannot survive the file anyway.
        while (!piece.text.isEmpty() && piece.text.back() == u' ') {
            piece.text.chop(1);
            const int32_t size = int32_t(piece.text.size());
            for (Run& run : piece.runs) {
                if (run.end > size) run.end = size;
                if (run.start > size) run.start = size;
            }
        }
        piece.runs.erase(std::remove_if(piece.runs.begin(), piece.runs.end(),
                                        [](const Run& r) { return r.end <= r.start && !r.image(); }),
                         piece.runs.end());
        Piece out = std::move(piece);
        piece = Piece{};
        atStart = true;
        return out;
    }
};

Piece vspacePiece() {
    Piece v;
    v.kind = Kind::VSpace;
    return v;
}

// THE PARSER. Recursive descent over QXmlStreamReader: every parse… function
// is entered right after the element's StartElement and leaves after its
// EndElement. Unknown elements are transparent for inline content and skipped
// as blocks — a foreign tag must not lose the text around it.
class Fb2Parser {
public:
    Fb2Parser(QXmlStreamReader& reader, std::vector<Piece>& pieces, std::vector<Fb2Book::Binary>& binaries,
              Fb2Book::Stats& stats)
        : r_(reader), pieces_(pieces), binaries_(binaries), stats_(stats) {}

    bool run(QString* error);

    QString title;
    QString created;
    QString coverId;
    std::vector<std::pair<std::string, std::string>> fields;

private:
    enum class Mode { Body, Notes };

    QStringView name() const { return localOf(r_.qualifiedName()); }

    // Reads the element's text content whole (a metadata leaf).
    QString leafText() { return r_.readElementText(QXmlStreamReader::IncludeChildElements).simplified(); }

    void add(Piece piece, bool continueQuote = false) {
        if (!pieces_.empty() && !continueQuote && pieces_.back().kind != Kind::VSpace)
            pieces_.push_back(vspacePiece());
        pieces_.push_back(std::move(piece));
    }

    // --- description ---------------------------------------------------
    void parseDescription();
    void parseTitleInfo();
    void parsePublishInfo();
    void parseDocumentInfo();
    QString parsePersonName();
    void parseAnnotation();

    // --- bodies ---------------------------------------------------------
    void parseBody();
    void parseSection(int depth, Mode mode);
    void parseNoteSection();
    void collectNoteLines(Para& acc, bool& first);
    void parseTitleInto(Para& para);
    void parseEpigraph(bool& quoteOpen);
    void parseCite(bool& quoteOpen);
    void parsePoem(bool& quoteOpen, uint16_t flags);
    void parseStanza(bool& quoteOpen, uint16_t flags);
    void parseTable();
    void parseInline(Para& para, uint16_t flags, const QString& href);
    void parseParagraph(Kind kind, uint16_t flags, bool& quoteOpen);
    void blockImage();
    void parseBinary();

    QString footnoteId(const QString& raw);

    QXmlStreamReader& r_;
    std::vector<Piece>& pieces_;
    std::vector<Fb2Book::Binary>& binaries_;
    Fb2Book::Stats& stats_;

    QStringList authors_;
    QStringList translators_;
    QStringList genres_;
    QString year_;
    QString dateValue_;
    QString isbn_;
    QString publisher_;
    QString city_;
    QString series_;
    QString seriesNumber_;
    QString lang_;
    std::vector<Piece> annotation_;
    bool mainBodySeen_ = false;
    QHash<QString, QString> footnoteIds_;
    int renumbered_ = 0;
};

bool Fb2Parser::run(QString* error) {
    while (!r_.atEnd()) {
        r_.readNext();
        if (r_.hasError()) break;
        if (!r_.isStartElement()) continue;
        const QStringView n = name();
        if (n == u"FictionBook") continue;
        if (n == u"description") parseDescription();
        else if (n == u"body") parseBody();
        else if (n == u"binary") parseBinary();
        else if (n != u"stylesheet") r_.skipCurrentElement();
        else r_.skipCurrentElement();
    }
    if (r_.hasError() && r_.error() != QXmlStreamReader::PrematureEndOfDocumentError) {
        if (error != nullptr)
            *error = QStringLiteral("line %1: %2").arg(r_.lineNumber()).arg(r_.errorString());
        return false;
    }
    if (title.isEmpty() && pieces_.empty()) {
        if (error != nullptr) *error = QStringLiteral("no FictionBook body or title");
        return false;
    }

    // The header lines, in a stable order.
    const auto put = [&](const char* key, const QString& value) {
        const std::string safe = NoteHeader::safeValue(value.toStdString());
        if (!safe.empty()) fields.emplace_back(key, safe);
    };
    put("author", authors_.join(QStringLiteral(", ")));
    put("translator", translators_.join(QStringLiteral(", ")));
    QString year = year_;
    if (year.isEmpty() && dateValue_.size() >= 4) year = dateValue_.left(4);
    put("year", year);
    put("isbn", isbn_);
    put("publisher", city_.isEmpty() ? publisher_
                                     : (publisher_.isEmpty() ? city_ : publisher_ + QStringLiteral(", ") + city_));
    put("series", seriesNumber_.isEmpty() ? series_ : series_ + QStringLiteral(" #") + seriesNumber_);
    put("lang", lang_);
    put("genre", genres_.join(QStringLiteral(", ")));
    stats_.renumberedIds = renumbered_;
    return true;
}

// --- description ---------------------------------------------------------

void Fb2Parser::parseDescription() {
    while (r_.readNextStartElement()) {
        const QStringView n = name();
        if (n == u"title-info") parseTitleInfo();
        else if (n == u"publish-info") parsePublishInfo();
        else if (n == u"document-info") parseDocumentInfo();
        else r_.skipCurrentElement();
    }
}

QString Fb2Parser::parsePersonName() {
    QString first;
    QString middle;
    QString last;
    QString nick;
    while (r_.readNextStartElement()) {
        const QStringView n = name();
        if (n == u"first-name") first = leafText();
        else if (n == u"middle-name") middle = leafText();
        else if (n == u"last-name") last = leafText();
        else if (n == u"nickname") nick = leafText();
        else r_.skipCurrentElement();
    }
    QStringList parts;
    for (const QString& p : {first, middle, last})
        if (!p.isEmpty()) parts.append(p);
    if (parts.isEmpty() && !nick.isEmpty()) parts.append(nick);
    return parts.join(QLatin1Char(' '));
}

void Fb2Parser::parseTitleInfo() {
    while (r_.readNextStartElement()) {
        const QStringView n = name();
        if (n == u"genre") {
            const QString g = leafText();
            if (!g.isEmpty()) genres_.append(g);
        } else if (n == u"author") {
            const QString a = parsePersonName();
            if (!a.isEmpty()) authors_.append(a);
        } else if (n == u"translator") {
            const QString t = parsePersonName();
            if (!t.isEmpty()) translators_.append(t);
        } else if (n == u"book-title") {
            title = leafText();
        } else if (n == u"annotation") {
            parseAnnotation();
        } else if (n == u"coverpage") {
            while (r_.readNextStartElement()) {
                if (name() == u"image") {
                    QString href = attributeOf(r_, QLatin1String("href"));
                    if (href.startsWith(u'#')) href.remove(0, 1);
                    if (coverId.isEmpty()) coverId = href;
                }
                r_.skipCurrentElement();
            }
        } else if (n == u"lang") {
            lang_ = leafText();
        } else if (n == u"sequence") {
            if (series_.isEmpty()) {
                series_ = attributeOf(r_, QLatin1String("name")).simplified();
                seriesNumber_ = attributeOf(r_, QLatin1String("number")).simplified();
            }
            r_.skipCurrentElement();
        } else if (n == u"date") {
            const QString value = attributeOf(r_, QLatin1String("value"));
            const QString text = leafText();
            if (dateValue_.isEmpty()) dateValue_ = value.isEmpty() ? text : value;
        } else {
            r_.skipCurrentElement();
        }
    }
}

void Fb2Parser::parsePublishInfo() {
    while (r_.readNextStartElement()) {
        const QStringView n = name();
        if (n == u"year") year_ = leafText();
        else if (n == u"isbn") isbn_ = leafText();
        else if (n == u"publisher") publisher_ = leafText();
        else if (n == u"city") city_ = leafText();
        else r_.skipCurrentElement();
    }
}

void Fb2Parser::parseDocumentInfo() {
    while (r_.readNextStartElement()) {
        if (name() == u"date") {
            const QString value = attributeOf(r_, QLatin1String("value"));
            const QString text = leafText();
            created = value.isEmpty() ? text : value;
        } else {
            r_.skipCurrentElement();
        }
    }
}

// The annotation lands as a quote at the top of the body — after the title,
// before the text. Some files carry HTML escaped inside it (`&lt;p&gt;`): the
// tags are dropped from the text, no markup survives such a paragraph.
void Fb2Parser::parseAnnotation() {
    std::vector<Piece> saved;
    saved.swap(pieces_);
    bool quoteOpen = false;
    while (r_.readNextStartElement()) {
        const QStringView n = name();
        if (n == u"p" || n == u"subtitle") parseParagraph(Kind::Quote, n == u"subtitle" ? InlineBold : 0, quoteOpen);
        else if (n == u"poem") parsePoem(quoteOpen, 0);
        else if (n == u"cite") parseCite(quoteOpen);
        else if (n == u"empty-line") { r_.skipCurrentElement(); }
        else r_.skipCurrentElement();
    }
    static const QRegularExpression tag(QStringLiteral("<[^<>]{1,60}>"));
    for (Piece& piece : pieces_) {
        if (!piece.text.contains(u'<')) continue;
        const QString stripped = piece.text.remove(tag);
        if (stripped != piece.text) {
            piece.text = stripped;
            piece.runs.clear();
        }
        piece.text = piece.text.simplified();
        piece.runs.clear();
    }
    annotation_.swap(pieces_);
    pieces_.swap(saved);
}

// --- bodies ------------------------------------------------------------------

void Fb2Parser::parseBody() {
    const QString bodyName = attributeOf(r_, QLatin1String("name")).trimmed();
    const bool notes = mainBodySeen_ && !bodyName.isEmpty();
    // The first body without a name is the text; a named body after it holds
    // footnotes (notes, comments) — its sections are definitions.
    if (!mainBodySeen_ && bodyName.isEmpty()) {
        mainBodySeen_ = true;
        // The title first, the annotation next — the way a title page reads.
        if (!title.isEmpty()) {
            Piece h;
            h.kind = Kind::Heading;
            h.headingLevel = 1;
            h.text = title;
            add(std::move(h));
        }
        bool first = true;
        for (Piece& piece : annotation_) {
            add(std::move(piece), !first);
            first = false;
        }
        annotation_.clear();
    }
    const Mode mode = notes ? Mode::Notes : Mode::Body;
    bool quoteOpen = false;
    while (r_.readNextStartElement()) {
        const QStringView n = name();
        if (n == u"title") {
            Para para;
            parseTitleInto(para);
            // The body's own title usually repeats the book title; the notes
            // body says «Примечания» — that one is worth a heading.
            if (!para.empty() && para.piece.text.simplified() != title.simplified()) {
                Piece h = para.take(Kind::Heading);
                h.headingLevel = 2;
                add(std::move(h));
            }
        } else if (n == u"epigraph") {
            parseEpigraph(quoteOpen);
        } else if (n == u"image") {
            blockImage();
        } else if (n == u"section") {
            if (mode == Mode::Notes) parseNoteSection();
            else parseSection(1, mode);
        } else if (n == u"p") {
            // Text straight in the body (non-standard, seen in the wild).
            quoteOpen = false;
            parseParagraph(Kind::Paragraph, 0, quoteOpen);
        } else {
            r_.skipCurrentElement();
        }
    }
}

void Fb2Parser::parseTitleInto(Para& para) {
    while (r_.readNextStartElement()) {
        if (name() == u"p") {
            if (!para.empty()) para.literal(u" · ", 0, {});
            parseInline(para, 0, {});
        } else {
            r_.skipCurrentElement();
        }
    }
}

void Fb2Parser::parseSection(int depth, Mode mode) {
    ++stats_.sections;
    bool titled = false;
    bool quoteOpen = false;
    bool contentSeen = false;
    while (r_.readNextStartElement()) {
        const QStringView n = name();
        if (n == u"title") {
            Para para;
            parseTitleInto(para);
            if (!para.empty()) {
                Piece h = para.take(Kind::Heading);
                h.headingLevel = std::min(depth + 1, 6);
                add(std::move(h));
                titled = true;
            }
            continue;
        }
        if (!titled && !contentSeen) {
            // A section without a title: a rule marks where it starts.
            Piece rule;
            rule.kind = Kind::Divider;
            add(std::move(rule));
            titled = true;
        }
        contentSeen = true;
        if (n == u"section") {
            quoteOpen = false;
            parseSection(depth + 1, mode);
        } else if (n == u"p") {
            quoteOpen = false;
            parseParagraph(Kind::Paragraph, 0, quoteOpen);
        } else if (n == u"subtitle") {
            quoteOpen = false;
            parseParagraph(Kind::Paragraph, InlineBold, quoteOpen);
        } else if (n == u"empty-line") {
            r_.skipCurrentElement();
            if (!pieces_.empty() && pieces_.back().kind != Kind::VSpace) pieces_.push_back(vspacePiece());
            pieces_.push_back(vspacePiece());
            quoteOpen = false;
        } else if (n == u"epigraph") {
            quoteOpen = false;
            parseEpigraph(quoteOpen);
            quoteOpen = false;
        } else if (n == u"cite") {
            quoteOpen = false;
            parseCite(quoteOpen);
            quoteOpen = false;
        } else if (n == u"poem") {
            quoteOpen = false;
            parsePoem(quoteOpen, 0);
            quoteOpen = false;
        } else if (n == u"image") {
            quoteOpen = false;
            blockImage();
        } else if (n == u"table") {
            quoteOpen = false;
            parseTable();
        } else if (n == u"annotation") {
            quoteOpen = false;
            while (r_.readNextStartElement()) {
                if (name() == u"p") parseParagraph(Kind::Quote, 0, quoteOpen);
                else r_.skipCurrentElement();
            }
            quoteOpen = false;
        } else {
            r_.skipCurrentElement();
        }
    }
}

// A section of a notes body: `<section id="X">` → `[^X]: the paragraphs`,
// one line per paragraph. Its title (the note's number) is dropped — the id
// is the label. Nested sections become definitions of their own.
void Fb2Parser::parseNoteSection() {
    const QString rawId = attributeOf(r_, QLatin1String("id")).trimmed();
    Para acc;
    bool first = true;
    while (r_.readNextStartElement()) {
        const QStringView n = name();
        if (n == u"title") {
            r_.skipCurrentElement();
        } else if (n == u"section") {
            parseNoteSection();
        } else {
            collectNoteLines(acc, first);
        }
    }
    if (rawId.isEmpty()) {
        // No id — nothing can refer to it; the text is kept as a paragraph.
        if (!acc.empty()) add(acc.take(Kind::Paragraph));
        return;
    }
    Piece note = acc.take(Kind::Footnote);
    note.info = footnoteId(rawId);
    ++stats_.footnotes;
    add(std::move(note));
}

// One block-level child of a note, positioned at its StartElement: its text
// becomes one more line of the accumulating definition.
void Fb2Parser::collectNoteLines(Para& acc, bool& first) {
    const QStringView n = name();
    const auto line = [&](uint16_t flags) {
        if (!first) acc.newline();
        first = false;
        parseInline(acc, flags, {});
    };
    if (n == u"p" || n == u"text-author" || n == u"v") {
        line(0);
    } else if (n == u"subtitle") {
        line(InlineBold);
    } else if (n == u"poem" || n == u"stanza" || n == u"cite" || n == u"epigraph") {
        while (r_.readNextStartElement()) collectNoteLines(acc, first);
    } else if (n == u"empty-line") {
        r_.skipCurrentElement();
    } else if (n == u"image") {
        if (!first) acc.newline();
        first = false;
        QString href = attributeOf(r_, QLatin1String("href"));
        if (href.startsWith(u'#')) href.remove(0, 1);
        const QString alt = attributeOf(r_, QLatin1String("alt"));
        if (!href.isEmpty()) {
            acc.image(href, alt);
            ++stats_.images;
        }
        r_.skipCurrentElement();
    } else {
        r_.skipCurrentElement();
    }
}

void Fb2Parser::parseParagraph(Kind kind, uint16_t flags, bool& quoteOpen) {
    Para para;
    parseInline(para, flags, {});
    ++stats_.paragraphs;
    if (para.empty()) return;
    add(para.take(kind), kind == Kind::Quote && quoteOpen);
    if (kind == Kind::Quote) quoteOpen = true;
}

void Fb2Parser::parseEpigraph(bool& quoteOpen) {
    while (r_.readNextStartElement()) {
        const QStringView n = name();
        if (n == u"p") parseParagraph(Kind::Quote, InlineItalic, quoteOpen);
        else if (n == u"poem") parsePoem(quoteOpen, InlineItalic);
        else if (n == u"cite") parseCite(quoteOpen);
        else if (n == u"text-author") {
            Para para;
            para.literal(u"— ", 0, {});
            parseInline(para, 0, {});
            add(para.take(Kind::Quote), quoteOpen);
            quoteOpen = true;
        } else r_.skipCurrentElement();
    }
}

void Fb2Parser::parseCite(bool& quoteOpen) {
    while (r_.readNextStartElement()) {
        const QStringView n = name();
        if (n == u"p") parseParagraph(Kind::Quote, 0, quoteOpen);
        else if (n == u"subtitle") parseParagraph(Kind::Quote, InlineBold, quoteOpen);
        else if (n == u"poem") parsePoem(quoteOpen, 0);
        else if (n == u"text-author") {
            Para para;
            para.literal(u"— ", 0, {});
            parseInline(para, 0, {});
            add(para.take(Kind::Quote), quoteOpen);
            quoteOpen = true;
        } else if (n == u"table") {
            // A table cannot live inside a quote: it follows the quote.
            parseTable();
            quoteOpen = false;
        } else r_.skipCurrentElement();
    }
}

void Fb2Parser::parsePoem(bool& quoteOpen, uint16_t flags) {
    ++stats_.poems;
    while (r_.readNextStartElement()) {
        const QStringView n = name();
        if (n == u"title") {
            Para para;
            parseTitleInto(para);
            if (!para.empty()) {
                for (Run& run : para.piece.runs) run.flags |= InlineBold;
                if (para.piece.runs.empty()) para.styled(0, int32_t(para.piece.text.size()), InlineBold, {});
                add(para.take(Kind::Quote), quoteOpen);
                quoteOpen = true;
            }
        } else if (n == u"epigraph") {
            parseEpigraph(quoteOpen);
        } else if (n == u"stanza") {
            parseStanza(quoteOpen, flags);
        } else if (n == u"text-author" || n == u"date") {
            Para para;
            if (n == u"text-author") para.literal(u"— ", 0, {});
            parseInline(para, flags, {});
            if (!para.empty()) {
                add(para.take(Kind::Quote), quoteOpen);
                quoteOpen = true;
            }
        } else {
            r_.skipCurrentElement();
        }
    }
}

// A stanza is one quote paragraph: the verses are its lines.
void Fb2Parser::parseStanza(bool& quoteOpen, uint16_t flags) {
    Para para;
    para.keepLead = true;
    bool first = true;
    while (r_.readNextStartElement()) {
        const QStringView n = name();
        if (n == u"v") {
            if (!first) para.newline();
            first = false;
            parseInline(para, flags, {});
        } else if (n == u"title" || n == u"subtitle") {
            Para t;
            parseTitleInto(t);
            if (!t.empty()) {
                if (t.piece.runs.empty()) t.styled(0, int32_t(t.piece.text.size()), InlineBold, {});
                add(t.take(Kind::Quote), quoteOpen);
                quoteOpen = true;
            }
        } else {
            r_.skipCurrentElement();
        }
    }
    if (para.empty()) return;
    add(para.take(Kind::Quote), quoteOpen);
    quoteOpen = true;
}

void Fb2Parser::blockImage() {
    QString href = attributeOf(r_, QLatin1String("href"));
    if (href.startsWith(u'#')) href.remove(0, 1);
    const QString alt = attributeOf(r_, QLatin1String("alt"));
    r_.skipCurrentElement();
    if (href.isEmpty()) return;
    Para para;
    para.image(href, alt);
    ++stats_.images;
    add(para.take(Kind::Paragraph));
}

// A GFM table, verbatim: the first row is the header (GFM needs one), every
// row is padded to the widest, spans are flattened, `|` in a cell escaped.
void Fb2Parser::parseTable() {
    std::vector<QStringList> rows;
    while (r_.readNextStartElement()) {
        if (name() != u"tr") {
            r_.skipCurrentElement();
            continue;
        }
        QStringList cells;
        while (r_.readNextStartElement()) {
            const QStringView n = name();
            if (n == u"td" || n == u"th") {
                Para cell;
                parseInline(cell, 0, {});
                QString text = cell.take(Kind::Paragraph).text;
                text.replace(u'\n', u' ');
                text.replace(QStringLiteral("|"), QStringLiteral("\\|"));
                cells.append(text.simplified());
            } else {
                r_.skipCurrentElement();
            }
        }
        rows.push_back(cells);
    }
    if (rows.empty()) return;
    qsizetype columns = 0;
    for (const QStringList& row : rows) columns = std::max(columns, row.size());
    if (columns == 0) return;
    QString out;
    for (size_t i = 0; i < rows.size(); ++i) {
        QStringList row = rows[i];
        while (row.size() < columns) row.append(QString());
        out += u'|';
        for (const QString& cell : row) {
            out += u' ';
            out += cell.isEmpty() ? QStringLiteral(" ") : cell;
            out += QStringLiteral(" |");
        }
        out += u'\n';
        if (i == 0) {
            out += u'|';
            for (qsizetype k = 0; k < columns; ++k) out += QStringLiteral(" --- |");
            out += u'\n';
        }
    }
    Piece table;
    table.raw = true;
    table.table = true;
    table.text = out;
    table.trailingNewline = true;
    ++stats_.tables;
    add(std::move(table));
}

QString Fb2Parser::footnoteId(const QString& raw) {
    const auto it = footnoteIds_.constFind(raw);
    if (it != footnoteIds_.constEnd()) return *it;
    bool clean = !raw.isEmpty();
    for (const QChar c : raw)
        if (!footnoteIdChar(c)) { clean = false; break; }
    QString id = raw;
    // A clean id that a renumbered one already took is not clean any more.
    if (clean && std::any_of(footnoteIds_.constBegin(), footnoteIds_.constEnd(),
                             [&](const QString& v) { return v == id; }))
        clean = false;
    if (!clean) {
        id = QStringLiteral("n%1").arg(++renumbered_);
        // Must not collide with a clean id used elsewhere in the file.
        while (std::any_of(footnoteIds_.constBegin(), footnoteIds_.constEnd(),
                           [&](const QString& v) { return v == id; }))
            id = QStringLiteral("n%1").arg(++renumbered_);
    }
    footnoteIds_.insert(raw, id);
    return id;
}

// Inline content of the element just entered, up to its EndElement.
void Fb2Parser::parseInline(Para& para, uint16_t flags, const QString& href) {
    while (!r_.atEnd()) {
        r_.readNext();
        if (r_.hasError()) return;
        if (r_.isCharacters() || r_.isEntityReference()) {
            para.text(r_.text(), flags, href);
            continue;
        }
        if (r_.isEndElement()) return;
        if (!r_.isStartElement()) continue;

        const QStringView n = name();
        if (n == u"emphasis") parseInline(para, flags | InlineItalic, href);
        else if (n == u"strong") parseInline(para, flags | InlineBold, href);
        else if (n == u"strikethrough") parseInline(para, flags | InlineStrike, href);
        else if (n == u"sup") parseInline(para, flags | InlineSup, href);
        else if (n == u"sub") parseInline(para, flags | InlineSub, href);
        else if (n == u"code") parseInline(para, flags | InlineCode, href);
        else if (n == u"a") {
            const QString type = attributeOf(r_, QLatin1String("type"));
            QString target = attributeOf(r_, QLatin1String("href")).trimmed();
            if (type == QLatin1String("note") && target.startsWith(u'#') && target.size() > 1) {
                // The visible text ("[1]") is the number the file chose; the
                // reference carries the id, the view shows it.
                const QString id = footnoteId(target.mid(1));
                para.literal(QStringLiteral("[^") + id + u']', InlineFootnote, {});
                ++stats_.references;
                r_.skipCurrentElement();
            } else if (!target.isEmpty() && !target.startsWith(u'#') && href.isEmpty()) {
                parseInline(para, flags, target);
            } else {
                parseInline(para, flags, href);   // a local link: its text only
            }
        } else if (n == u"image") {
            QString target = attributeOf(r_, QLatin1String("href"));
            if (target.startsWith(u'#')) target.remove(0, 1);
            const QString alt = attributeOf(r_, QLatin1String("alt"));
            if (!target.isEmpty()) {
                para.image(target, alt);
                ++stats_.images;
            }
            r_.skipCurrentElement();
        } else {
            parseInline(para, flags, href);   // style, date, unknown: transparent
        }
    }
}

void Fb2Parser::parseBinary() {
    Fb2Book::Binary b;
    b.id = attributeOf(r_, QLatin1String("id")).trimmed();
    b.contentType = attributeOf(r_, QLatin1String("content-type")).trimmed().toLower();
    const QString text = r_.readElementText(QXmlStreamReader::SkipChildElements);
    if (b.id.isEmpty()) return;
    b.bytes = QByteArray::fromBase64(text.toLatin1());
    binaries_.push_back(std::move(b));
}

}  // namespace

// --- Fb2Book ------------------------------------------------------------------

QString Fb2Book::declaredEncoding(const QByteArray& bytes) {
    const QByteArray head = bytes.left(300);
    static const QRegularExpression decl(QStringLiteral("encoding\\s*=\\s*[\"']([A-Za-z0-9_.:-]+)[\"']"));
    const QRegularExpressionMatch m = decl.match(QString::fromLatin1(head));
    return m.hasMatch() ? m.captured(1).toLower() : QString();
}

QByteArray Fb2Book::decodeCp1251(const QByteArray& bytes) {
    // windows-1251, 0x80–0xFF. 0x98 is unassigned and becomes U+FFFD.
    static const char16_t table[128] = {
        0x0402, 0x0403, 0x201A, 0x0453, 0x201E, 0x2026, 0x2020, 0x2021, 0x20AC, 0x2030, 0x0409,
        0x2039, 0x040A, 0x040C, 0x040B, 0x040F, 0x0452, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
        0x2013, 0x2014, 0xFFFD, 0x2122, 0x0459, 0x203A, 0x045A, 0x045C, 0x045B, 0x045F, 0x00A0,
        0x040E, 0x045E, 0x0408, 0x00A4, 0x0490, 0x00A6, 0x00A7, 0x0401, 0x00A9, 0x0404, 0x00AB,
        0x00AC, 0x00AD, 0x00AE, 0x0407, 0x00B0, 0x00B1, 0x0406, 0x0456, 0x0491, 0x00B5, 0x00B6,
        0x00B7, 0x0451, 0x2116, 0x0454, 0x00BB, 0x0458, 0x0405, 0x0455, 0x0457,
        // 0xC0–0xFF: А…Я, а…я
        0x0410, 0x0411, 0x0412, 0x0413, 0x0414, 0x0415, 0x0416, 0x0417, 0x0418, 0x0419, 0x041A,
        0x041B, 0x041C, 0x041D, 0x041E, 0x041F, 0x0420, 0x0421, 0x0422, 0x0423, 0x0424, 0x0425,
        0x0426, 0x0427, 0x0428, 0x0429, 0x042A, 0x042B, 0x042C, 0x042D, 0x042E, 0x042F, 0x0430,
        0x0431, 0x0432, 0x0433, 0x0434, 0x0435, 0x0436, 0x0437, 0x0438, 0x0439, 0x043A, 0x043B,
        0x043C, 0x043D, 0x043E, 0x043F, 0x0440, 0x0441, 0x0442, 0x0443, 0x0444, 0x0445, 0x0446,
        0x0447, 0x0448, 0x0449, 0x044A, 0x044B, 0x044C, 0x044D, 0x044E, 0x044F};
    QString out;
    out.reserve(bytes.size());
    for (const char ch : bytes) {
        const unsigned char c = static_cast<unsigned char>(ch);
        out += c < 0x80 ? QChar(c) : QChar(table[c - 0x80]);
    }
    return out.toUtf8();
}

bool Fb2Book::load(const QByteArray& given, QString* error) {
    *this = Fb2Book{};
    QByteArray bytes = given;
    const QString encoding = declaredEncoding(bytes);
    if (encoding == QLatin1String("windows-1251") || encoding == QLatin1String("cp1251") ||
        encoding == QLatin1String("win-1251") || encoding == QLatin1String("cp-1251")) {
        bytes = decodeCp1251(bytes);
        // The declaration now lies; say what the bytes are.
        static const QRegularExpression decl(QStringLiteral("encoding\\s*=\\s*[\"'][A-Za-z0-9_.:-]+[\"']"));
        const qsizetype headSize = std::min<qsizetype>(300, bytes.size());
        QString head = QString::fromUtf8(bytes.left(headSize));
        head.replace(decl, QStringLiteral("encoding=\"utf-8\""));
        bytes = head.toUtf8() + bytes.mid(headSize);
    }

    QXmlStreamReader reader(bytes);
    reader.setNamespaceProcessing(false);
    Fb2Parser parser(reader, pieces_, binaries_, stats_);
    QString why;
    if (!parser.run(&why)) {
        if (error != nullptr) *error = why;
        return false;
    }
    title_ = parser.title;
    created_ = parser.created;
    coverId_ = parser.coverId;
    fields_ = std::move(parser.fields);
    // No trailing blank line.
    while (!pieces_.empty() && pieces_.back().kind == Kind::VSpace && pieces_.back().text.isEmpty())
        pieces_.pop_back();
    return true;
}

const Fb2Book::Binary* Fb2Book::binary(const QString& id) const {
    for (const Binary& b : binaries_)
        if (b.id == id) return &b;
    return nullptr;
}

QStringList Fb2Book::referencedBinaries() const {
    QStringList out;
    for (const Piece& piece : pieces_)
        for (const Run& run : piece.runs)
            if (run.image() && !run.href.isEmpty() && !out.contains(run.href)) out.append(run.href);
    if (!coverId_.isEmpty() && !out.contains(coverId_)) out.append(coverId_);
    return out;
}

int Fb2Book::rewriteImages(const QHash<QString, QString>& fileNames) {
    int dropped = 0;
    for (Piece& piece : pieces_) {
        for (size_t i = 0; i < piece.runs.size();) {
            Run& run = piece.runs[i];
            if (!run.image()) { ++i; continue; }
            const auto it = fileNames.constFind(run.href);
            if (it == fileNames.constEnd()) {
                piece.runs.erase(piece.runs.begin() + qsizetype(i));
                ++dropped;
                continue;
            }
            run.href = *it;
            ++i;
        }
    }
    if (dropped == 0) return 0;
    // A picture without a caption that lost its run is an empty paragraph;
    // it goes, and so does the blank line that separated it.
    std::vector<Piece> kept;
    kept.reserve(pieces_.size());
    for (Piece& piece : pieces_) {
        const bool emptyParagraph = !piece.raw && piece.kind == Kind::Paragraph &&
                                    piece.text.isEmpty() && piece.runs.empty();
        if (emptyParagraph) {
            if (!kept.empty() && kept.back().kind == Kind::VSpace) kept.pop_back();
            continue;
        }
        kept.push_back(std::move(piece));
    }
    pieces_ = std::move(kept);
    return dropped;
}

}  // namespace zametti
