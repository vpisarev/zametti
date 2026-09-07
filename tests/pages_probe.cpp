// Empty and half-empty pages of a book, measured (the owner, 07.09.2026:
// "Robinson Crusoe" shows blank pages in the middle, and half-filled ones
// with nothing but plain paragraphs after them).
//
//   zametti-bench pages <note.md> [WIDTHxHEIGHT]
//
// Opens the note as a book (one page per spread), walks every page and prints
// the ones filled below 70 %: the page number, the fill, what the page
// starts and ends with, and what the next page begins with. A second walk
// after the layout settled says whether the table drifted.

#include "book_page.h"
#include "book_pages.h"
#include "doc_model.h"
#include "settings_hook.h"
#include "resources.h"
#include "zbook_view.h"
#include "znote.h"

#include <QAbstractTextDocumentLayout>
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QTest>
#include <QTextBlock>
#include <QTextDocument>
#include <QImage>
#include <QScrollBar>

#include <cstdio>
#include <string>
#include <vector>

using zametti::BookPage;
using zametti::PageStart;
using zametti::ZBookView;

namespace {

const char* kindName(const QTextBlock& block) {
    switch (zametti::kindOf(block)) {
        case zametti::Kind::Heading: return "heading";
        case zametti::Kind::VSpace: return "blank";
        case zametti::Kind::Code: return "code";
        case zametti::Kind::Quote: return "quote";
        default: break;
    }
    return "text";
}

struct Row {
    int page = 0;
    PageStart start;
    PageStart end;
    qreal fill = 0.0;
    QString startText, endText, nextText;
    QString endFull;
    std::string startKind, endKind, nextKind;
    int lines = 0;
    qreal nextHeight = 0.0;
    qreal pageHeight = 0.0;
};

std::vector<Row> walk(ZBookView& book, int limit) {
    std::vector<Row> rows;
    BookPage& page = book.page(0);
    const QTextDocument& doc = *page.document();
    const QAbstractTextDocumentLayout& layout = *doc.documentLayout();
    book.showBlock(0);
    QTest::qWait(30);
    // The cover leaf shows no text on the left: step past it.
    if (!page.end().has_value() && book.pageCount() > 1) {
        book.pageStep(1);
        QTest::qWait(30);
    }
    for (int k = 0; k < limit; ++k) {
        Row row;
        row.page = book.currentPage();
        row.start = page.start();
        if (!page.end().has_value()) {
            rows.push_back(row);
            break;
        }
        row.end = *page.end();
        // The last line of this page: the one before the next page's start.
        QTextBlock endBlock = doc.findBlockByNumber(row.end.block);
        int endLine = row.end.line - 1;
        if (endLine < 0) {
            endBlock = endBlock.previous();
            while (endBlock.isValid() && (endBlock.layout() == nullptr || endBlock.layout()->lineCount() == 0))
                endBlock = endBlock.previous();
            if (!endBlock.isValid()) {
                rows.push_back(row);
                break;
            }
            endLine = endBlock.layout()->lineCount() - 1;
        }
        const QTextBlock startBlock = doc.findBlockByNumber(row.start.block);
        const qreal top = zametti::lineSpanOf(layout, startBlock, row.start.line).top;
        const qreal bottom = zametti::lineSpanOf(layout, endBlock, endLine).bottom;
        row.fill = page.pageHeight() > 0 ? (bottom - top) / page.pageHeight() : 0.0;
        row.startText = startBlock.text().left(40);
        row.startKind = kindName(startBlock);
        row.endText = endBlock.text().left(40);
        row.endFull = endBlock.text();
        row.endKind = kindName(endBlock);
        const QTextBlock nextBlock = doc.findBlockByNumber(row.end.block);
        row.nextText = nextBlock.text().left(40);
        row.nextKind = kindName(nextBlock);
        const zametti::LineSpan nextSpan = zametti::lineSpanOf(layout, nextBlock, row.end.line);
        row.nextHeight = nextSpan.bottom - nextSpan.top;
        row.pageHeight = page.pageHeight();
        rows.push_back(row);
        if (book.pagesShown() == 2) {
            BookPage& right = book.page(1);
            Row r2;
            r2.page = book.currentPage() + 1;
            r2.start = right.start();
            if (right.end().has_value()) {
                r2.end = *right.end();
                QTextBlock eb = doc.findBlockByNumber(r2.end.block);
                int el = r2.end.line - 1;
                if (el < 0) {
                    eb = eb.previous();
                    while (eb.isValid() && (eb.layout() == nullptr || eb.layout()->lineCount() == 0)) eb = eb.previous();
                    el = eb.isValid() ? eb.layout()->lineCount() - 1 : 0;
                }
                const QTextBlock sb = doc.findBlockByNumber(r2.start.block);
                if (eb.isValid()) {
                    const qreal t = zametti::lineSpanOf(layout, sb, r2.start.line).top;
                    const qreal b = zametti::lineSpanOf(layout, eb, el).bottom;
                    r2.fill = right.pageHeight() > 0 ? (b - t) / right.pageHeight() : 0.0;
                    r2.startText = sb.text().left(40);
                    r2.startKind = kindName(sb);
                    r2.endText = eb.text().left(40);
                    r2.endFull = eb.text();
                    r2.endKind = kindName(eb);
                    const QTextBlock nb = doc.findBlockByNumber(r2.end.block);
                    r2.nextText = nb.text().left(40);
                    r2.nextKind = kindName(nb);
                    const zametti::LineSpan ns = zametti::lineSpanOf(layout, nb, r2.end.line);
                    r2.nextHeight = ns.bottom - ns.top;
                    r2.pageHeight = right.pageHeight();
                }
                if (r2.start == r2.end) r2.fill = 0.0;
                rows.push_back(r2);
            }
        }
        const int before = book.currentPage();
        book.pageStep(1);
        QTest::qWait(5);
        if (book.currentPage() == before) break;
    }
    return rows;
}

}  // namespace

int ztPagesProbe(int argc, char** argv) {
    if (argc < 2) {
        std::printf("zametti-bench pages <note.md> [WIDTHxHEIGHT]\n");
        return 2;
    }
    int width = 1400, height = 900;
    if (argc >= 3) {
        const QStringList wh = QString::fromLocal8Bit(argv[2]).split(QLatin1Char('x'));
        if (wh.size() == 2) {
            width = wh[0].toInt();
            height = wh[1].toInt();
        }
    }
    bool spread = false;
    bool fullscreen = false;
    for (int i = 3; i < argc; ++i) {
        if (std::string(argv[i]) == "spread") spread = true;
        if (std::string(argv[i]) == "fullscreen") fullscreen = true;
    }
    const QString path = QString::fromLocal8Bit(argv[1]);
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        std::printf("cannot read %s\n", argv[1]);
        return 1;
    }
    const QByteArray bytes = f.readAll();
    auto note = std::make_shared<zametti::ZNote>(path, bytes, zametti::Digest{}, nullptr);
    note->load(std::string_view(bytes.constData(), size_t(bytes.size())));
    zametti::loadEmbeddedFonts();
    zametti::mutableSettingsForTests().reading().setPagesPerSpread(spread ? 2 : 1);

    ZBookView book;
    book.resize(width, height);
    if (fullscreen) book.showFullScreen();
    else book.show();
    QTest::qWait(30);
    book.showNote(note);
    QTest::qWait(200);
    for (int i = 0; i < 400 && book.pageCount() < 0; ++i) QTest::qWait(25);
    std::printf("%s: %d pages at %dx%d, page height %.0f\n", QFileInfo(path).fileName().toUtf8().constData(),
                book.pageCount(), width, height, book.page(0).pageHeight());

    const std::vector<Row> first = walk(book, book.pageCount() + 1);   // spreads walk half as many steps
    int thin = 0;
    int empty = 0;
    int shown = 0;
    int beforeImage = 0;
    for (const Row& r : first) {
        if (r.fill >= 0.7 || r.end == PageStart{}) continue;
        ++thin;
        if (r.fill < 0.05) ++empty;
        if (r.fill < 0.05 || shown < 12)
            std::printf("  page %d: fill %.0f%%  starts [%s] %s | ends [%s] %s | next page starts [%s] %s "
                        "(next line %.0f of %.0f px)\n",
                        r.page + 1, r.fill * 100.0, r.startKind.c_str(), r.startText.toUtf8().constData(),
                        r.endKind.c_str(), r.endText.toUtf8().constData(), r.nextKind.c_str(),
                        r.nextText.toUtf8().constData(), r.nextHeight, r.pageHeight);
        ++shown;
        if (r.nextKind == "text" && r.nextText.startsWith(QChar(0xFFFC))) ++beforeImage;
    }
    std::printf("thin pages (< 70%%): %d of %zu walked; %d of them before a picture; empty (< 5%%): %d\n",
                thin, first.size(), beforeImage, empty);

    // A named place (argv[3]): the page that ends with that text and the one
    // after it, shot to PNG, with the share of ink on each — an "empty" page
    // is either a page of nothing or a picture that was not drawn.
    if (argc >= 4 && std::string(argv[argc - 1]) != "spread" && std::string(argv[argc - 1]) != "fullscreen") {
        const QString needle = QString::fromLocal8Bit(argv[argc - 1]);
        int needleBlock = -1;
        for (QTextBlock b = book.page(0).document()->begin(); b.isValid(); b = b.next())
            if (b.text().contains(needle)) {
                needleBlock = b.blockNumber();
                break;
            }
        std::printf("  needle block %d\n", needleBlock);
        for (const Row& r : first) {
            const bool inside = needleBlock >= 0 && r.start.block <= needleBlock &&
                                (needleBlock < r.end.block || (needleBlock == r.end.block && r.end.line > 0));
            if (!inside) continue;
            BookPage& page = book.page(0);
            const auto inkOf = [&](const QString& name) {
                QTest::qWait(60);
                const QImage shot = page.viewport()->grab().toImage();
                shot.save(name);
                const QRgb paper = page.palette().color(QPalette::Base).rgb();
                qint64 ink = 0;
                for (int y = 0; y < shot.height(); ++y)
                    for (int x = 0; x < shot.width(); ++x)
                        if (shot.pixel(x, y) != paper) ++ink;
                return double(ink) / double(shot.width() * shot.height());
            };
            book.showLine(r.start);
            std::printf("  place: page %d fill %.0f%% ink %.3f, next page starts [%s] %s (next line %.0f px)\n",
                        r.page + 1, r.fill * 100.0, inkOf(QStringLiteral("pages-probe-a.png")),
                        r.nextKind.c_str(), r.nextText.toUtf8().constData(), r.nextHeight);
            book.pageStep(1);
            std::printf("  next: page %d starts [%d,%d] ends [%d,%d] ink %.3f, scroll %d of max %d\n",
                        book.currentPage() + 1, page.start().block, page.start().line,
                        page.end().has_value() ? page.end()->block : -1,
                        page.end().has_value() ? page.end()->line : -1,
                        inkOf(QStringLiteral("pages-probe-b.png")), page.verticalScrollBar()->value(),
                        page.verticalScrollBar()->maximum());
            break;
        }
    }

    // PICTURES TALLER THAN THE PAGE: a page that starts with one — is anything
    // drawn on it? (the owner's empty right page in fullscreen, 07.09.2026)
    {
        int tall = 0;
        for (const Row& r : first) {
            if (!r.startText.startsWith(QChar(0xFFFC))) continue;
            BookPage& page = book.page(0);
            const QTextBlock sb = page.document()->findBlockByNumber(r.start.block);
            const zametti::LineSpan span = zametti::lineSpanOf(*page.document()->documentLayout(), sb, r.start.line);
            if (span.bottom - span.top <= page.pageHeight()) continue;
            if (++tall > 3) break;
            book.showLine(r.start);
            QTest::qWait(60);
            const QImage shot = page.viewport()->grab().toImage();
            const QRgb paper = page.palette().color(QPalette::Base).rgb();
            qint64 ink = 0;
            for (int y = 0; y < shot.height(); ++y)
                for (int x = 0; x < shot.width(); ++x)
                    if (shot.pixel(x, y) != paper) ++ink;
            std::printf("  tall picture page %d: line %.0f px on a %.0f px page, ink %.3f, scroll %d/%d, end [%d,%d]\n",
                        book.currentPage() + 1, span.bottom - span.top, page.pageHeight(),
                        double(ink) / double(shot.width() * shot.height()), page.verticalScrollBar()->value(),
                        page.verticalScrollBar()->maximum(), page.end() ? page.end()->block : -1,
                        page.end() ? page.end()->line : -1);
            shot.save(QStringLiteral("pages-probe-tall-%1.png").arg(tall));
        }
        std::printf("tall pictures looked at: %d\n", tall);
    }

    // Does the table drift once the layout has settled? Walk again.
    QTest::qWait(300);
    const std::vector<Row> second = walk(book, book.pageCount() + 1);
    int drifted = 0;
    for (size_t i = 0; i < first.size() && i < second.size(); ++i)
        if (first[i].start != second[i].start) ++drifted;
    std::printf("second walk: %zu pages, %d starts differ from the first walk\n", second.size(), drifted);
    return 0;
}
