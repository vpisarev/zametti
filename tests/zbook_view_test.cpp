// THE READING MODE (brief 18): the book as pages over the editor's live
// document.
//
// What is checked: a book opens in the reading mode by itself and an ordinary
// note does not; the pages start where the table says and turning forward and
// back lands on the very same line; the last line that did not fit whole is
// the first of the next page; a wide window shows two pages, a narrow one
// shows one; the search turns to the page of the hit; the editor's undo stack
// survives a trip through the reading mode; "switched and returned" draws the
// same pixels as "opened fresh" (the project's symmetry rule) for both the
// page and the editor; zoom recounts the pages; the status text follows.
#include "book_page.h"
#include "doc_model.h"
#include "book_pages.h"
#include "editor_widget.h"
#include "reading_controller.h"
#include "settings.h"
#include "test_util.h"
#include "testdata.h"
#include "zbook_view.h"
#include "zstorage.h"

#include <QApplication>
#include <QColor>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontMetricsF>
#include <QHelpEvent>
#include <QImage>
#include <QRect>
#include <QScrollBar>
#include <QSignalSpy>
#include <QStackedWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTextBlock>
#include <QToolTip>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFragment>
#include <QTextFrame>
#include <QTextLayout>
#include <QVBoxLayout>
#include <memory>
#include <cstdlib>
#include <string>

using namespace zametti;

namespace {

bool writeFile(const QString& path, const QString& text) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    file.write(text.toUtf8());
    return true;
}

QString bookText(int paragraphs) {
    QString out = QStringLiteral("<!-- zametti\nrole: book\nlock: yes\n-->\n\n# The Book\n\n");
    for (int i = 0; i < paragraphs; ++i) {
        if (i % 12 == 0) out += QStringLiteral("## Chapter %1\n\n").arg(i / 12 + 1);
        if (i == 1) out += QStringLiteral("A sentence with a note.[^n1]\n\n");
        out += QStringLiteral("Paragraph %1 of the book, long enough to wrap onto several lines of a "
                              "narrow page when the page is narrow, and to hold a word to find: "
                              "needle%1 sits here.\n\n")
                   .arg(i);
    }
    out += QStringLiteral("[^n1]: The footnote's own text.\n");
    return out;
}

// The window as main() builds it: the editor and the book view on one stack,
// the controller between them.
struct Rig {
    QTemporaryDir home;
    std::shared_ptr<ZStorage> storage;
    QWidget window;
    QStackedWidget* stack;
    NoteEditor* editor;
    ZBookView* book;
    ReadingController controller;
    QString bookPath;
    QString plainPath;

    explicit Rig(int width = 900, int height = 700)
        : stack(new QStackedWidget(&window)),
          editor(new NoteEditor(stack)),
          book(new ZBookView(stack)),
          controller(*editor, *book) {
        const QString root = home.path() + QStringLiteral("/store");
        QString error;
        ZStorage(root).init(&error);
        storage = std::make_shared<ZStorage>(root);
        storage->reload();
        const QString bookId = storage->createNote(QString(), false, &error);
        const QString plainId = storage->createNote(QString(), false, &error);
        bookPath = storage->pathOf(bookId);
        plainPath = storage->pathOf(plainId);
        writeFile(bookPath, bookText(160));
        writeFile(plainPath, QStringLiteral("<!-- zametti\n-->\n\n# Plain\n\nA line.\n\nAnother.\n"));
        storage->reload();
        editor->setStorage(storage);
        auto* layout = new QVBoxLayout(&window);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->addWidget(stack);
        stack->addWidget(editor);
        stack->addWidget(book);
        QObject::connect(&controller, &ReadingController::modeChanged, &window,
                         [this](bool on) { stack->setCurrentWidget(on ? static_cast<QWidget*>(book)
                                                                       : static_cast<QWidget*>(editor)); });
        window.resize(width, height);
        window.show();
        QTest::qWait(30);
    }
    void open(const QString& path) {
        editor->openFile(path);
        QTest::qWait(120);
    }
    BookPage& left() { return book->page(0); }
};

qreal lineTopOf(const QTextDocument& doc, const PageStart& start) {
    return lineSpanOf(*doc.documentLayout(), doc.findBlockByNumber(start.block), start.line).top;
}

void checkPagesAndKeys() {
    Rig rig;
    rig.open(rig.bookPath);
    ZT_TRUE("книга открылась в чтении сама", rig.controller.active());
    ZT_TRUE("на стеке — разворот", rig.stack->currentWidget() == rig.book);
    ZT_TRUE("одна страница в узком окне", rig.book->pagesShown() == 1);
    ZT_TRUE("ведущая страница держит заметку", rig.left().note() != nullptr && rig.left().lead());
    ZT_TRUE("начало — первая строка", rig.left().start() == PageStart{});

    const QTextDocument& doc = *rig.left().document();
    // Turn forward: the second page is the first chapter — a heading of the
    // chapters' level turns the page (books2), so the title stays alone on
    // the first one.
    rig.book->pageStep(+1);
    const PageStart second = rig.left().start();
    ZT_TRUE("вторая страница дальше первой", PageStart{} < second);
    {
        int chapter = -1;
        for (const ZDocument::OutlineEntry& entry : rig.left().note()->outline())
            if (entry.level == 2) { chapter = entry.block; break; }
        const PageStart chapterStart{chapter, 0};
        ZT_TRUE("вторая страница начинается первой главой", second == chapterStart);
    }
    // One more: the third page starts by the geometric rule — the first line
    // that did not fit whole on the chapter's page.
    rig.book->pageStep(+1);
    const PageStart third = rig.left().start();
    ZT_TRUE("третья страница дальше второй", second < third);
    const qreal height = rig.left().pageHeight();
    const qreal first = lineTopOf(doc, second);
    const qreal top = lineTopOf(doc, third);
    // The line before it fitted whole (checked below); this one begins within
    // a line of the page's bottom — right at it when the previous line ended
    // exactly there.
    ZT_TRUE("начало третьей — строка, не влезшая на вторую (её верх у нижней кромки)",
            top <= first + height + QFontMetricsF(rig.left().font()).height() &&
                top > first + height - 4 * QFontMetricsF(rig.left().font()).height());
    // The line before it fits whole on the page before.
    {
        PageStart before = third;
        if (before.line > 0) --before.line;
        else --before.block;
        const qreal bottom =
            lineSpanOf(*doc.documentLayout(), doc.findBlockByNumber(before.block), before.line).bottom;
        ZT_TRUE("строка перед ней влезла целиком", bottom <= first + height + 0.5);
    }
    rig.book->pageStep(-1);
    ZT_TRUE("назад — та же вторая страница", rig.left().start() == second);
    rig.book->pageStep(+1);
    rig.book->pageStep(-1);
    ZT_TRUE("вперёд-назад — та же вторая страница", rig.left().start() == second);
    rig.book->pageStep(-1);
    ZT_TRUE("и назад к началу", rig.left().start() == PageStart{});
    rig.book->pageStep(-1);
    ZT_TRUE("раньше начала не бывает", rig.left().start() == PageStart{});

    // Keys on the page: Space / Right / PageDown forward, Shift+Space /
    // Left / PageUp back, End / Home.
    rig.left().setFocus();
    QTest::keyClick(&rig.left(), Qt::Key_Space);
    ZT_TRUE("Space листает вперёд", rig.left().start() == second);
    QTest::keyClick(&rig.left(), Qt::Key_Left);
    ZT_TRUE("← листает назад", rig.left().start() == PageStart{});
    QTest::keyClick(&rig.left(), Qt::Key_PageDown);
    QTest::keyClick(&rig.left(), Qt::Key_Space, Qt::ShiftModifier);
    ZT_TRUE("PgDn и Shift+Space — туда и обратно", rig.left().start() == PageStart{});
    QTest::keyClick(&rig.left(), Qt::Key_End);
    const PageStart last = rig.left().start();
    ZT_TRUE("End — последняя страница", second < last);
    ZT_TRUE("число страниц посчитано", rig.book->pageCount() > 2);
    ZT_TRUE("последняя страница и есть последняя",
            rig.book->currentPage() == rig.book->pageCount() - 1);
    QTest::keyClick(&rig.left(), Qt::Key_Home);
    ZT_TRUE("Home — начало", rig.left().start() == PageStart{});
    // A printing key changes nothing.
    const int revision = doc.revision();
    QTest::keyClick(&rig.left(), Qt::Key_A);
    ZT_TRUE("буква на странице ничего не меняет", doc.revision() == revision);

    // The status text: chapter, page, count, percent.
    QSignalSpy spy(rig.book, &ZBookView::positionChanged);
    rig.book->pageStep(+1);
    ZT_TRUE("положение объявлено", spy.count() >= 1);
    if (spy.count() >= 1) {
        const QList<QVariant> last = spy.last();
        // The chapter is the last one that began before the next page's top
        // (owner's rule): asked of the block just before that start.
        PageStart boundary;
        rig.book->pageStep(+1);
        boundary = rig.left().start();
        rig.book->pageStep(-1);
        const int named = boundary.line > 0 ? boundary.block : boundary.block - 1;
        ZT_EQ("глава над второй страницей — последняя, начавшаяся до её низа",
              rig.left().note()->doc().headingAbove(named).toStdString(),
              last.at(0).toString().toStdString());
        ZT_TRUE("и это глава", last.at(0).toString().startsWith(QStringLiteral("Chapter")));
        ZT_TRUE("номер страницы — 2", last.at(1).toInt() == 2);
        ZT_TRUE("число страниц известно", last.at(2).toInt() > 2);
        ZT_TRUE("процент между нулём и сотней", last.at(3).toInt() >= 0 && last.at(3).toInt() <= 100);
    }
    // An ordinary note goes back to the editor.
    rig.open(rig.plainPath);
    ZT_TRUE("обычная заметка — в редакторе", !rig.controller.active());
    ZT_TRUE("страницы отдали документ", rig.left().note() == nullptr);
    // And back to the book: the place read is remembered this session.
    rig.open(rig.bookPath);
    ZT_TRUE("книга снова в чтении", rig.controller.active());
    ZT_TRUE("место чтения вернулось", rig.left().start() == second);
}

void checkSpread() {
    Rig rig(1600, 900);
    rig.open(rig.bookPath);
    ZT_TRUE("широкое окно — две страницы", rig.book->pagesShown() == 2);
    ZT_TRUE("правая страница видна", rig.book->page(1).isVisible());
    ZT_TRUE("обе над одним документом", rig.book->page(1).document() == rig.left().document());
    // The spread hugs the gutter (books2): the pages stand pageGap apart, no
    // wider than the column with its margins, and the spare width of the
    // window lies outside the spread, split evenly.
    {
        const zametti::ZSettings::Reading& reading = zametti::settings().reading();
        const qreal unit = QFontMetricsF(rig.left().font()).horizontalAdvance(QLatin1Char('A'));
        const auto check = [&](const char* when, bool expectSpare) {
            const QRect l = rig.left().geometry();
            const QRect r = rig.book->page(1).geometry();
            ZT_EQ(std::string(when) + ": между страницами — ровно pageGap",
                  std::to_string(int(reading.pageGap() * unit)),
                  std::to_string(r.left() - l.right() - 1));
            const int hugged = int((reading.maxContentWidth() + 2.0 * reading.sideMargin()) * unit);
            ZT_TRUE(std::string(when) + ": страница не шире колонки с полями",
                    l.width() <= hugged && r.width() == l.width());
            ZT_TRUE(std::string(when) + ": лишнее — снаружи разворота, поровну",
                    std::abs(l.left() - (rig.book->width() - r.right() - 1)) <= 1);
            if (expectSpare)
                ZT_TRUE(std::string(when) + ": страницы прижаты к корешку, а не растянуты",
                        l.width() == hugged && l.left() > 0);
        };
        check("1600", false);
        // Wider than two hugged pages: the spare goes outside.
        rig.window.resize(2400, 900);
        QTest::qWait(80);
        check("2400", true);
        rig.window.resize(1600, 900);
        QTest::qWait(80);
    }
    const PageStart left0 = rig.left().start();
    const PageStart right0 = rig.book->page(1).start();
    ZT_TRUE("правая идёт за левой", left0 < right0);
    rig.book->pageStep(+1);
    ZT_TRUE("разворот листается парами: слева — бывшая третья", right0 < rig.left().start());
    const PageStart left1 = rig.left().start();
    rig.book->pageStep(-1);
    ZT_TRUE("и назад — тот же разворот", rig.left().start() == left0 && rig.book->page(1).start() == right0);
    // The left page's cut line is the right page's first line: nothing lost.
    {
        const QTextDocument& doc = *rig.left().document();
        const qreal top = lineTopOf(doc, right0);
        const qreal first = lineTopOf(doc, left0);
        ZT_TRUE("правая начинается строкой, не влезшей на левую",
                top <= first + rig.left().pageHeight() && top > first);
    }
    (void)left1;
    // Narrow the window: one page, the place kept.
    rig.window.resize(900, 700);
    QTest::qWait(80);
    ZT_TRUE("узкое окно — одна страница", rig.book->pagesShown() == 1);
    ZT_TRUE("место на месте", rig.left().start() == left0);
}

void checkSearchAndUndo() {
    Rig rig;
    rig.open(rig.bookPath);
    // Search: a hit deep in the book turns to its page.
    TextSearchTarget& target = rig.book->activePage();
    zametti::Query query;
    query.needle = QStringLiteral("needle120");
    target.findMatches(query);
    ZT_TRUE("находка есть", target.matchCount() == 1);
    target.stepMatch(+1);
    QTest::qWait(30);
    ZT_TRUE("страница перевернулась к находке", rig.book->currentPage() > 0);
    const QTextCursor hit = rig.left().textCursor();
    const PageStart hitLine = rig.left().lineAt(
        rig.left().document()->documentLayout()->blockBoundingRect(hit.block()).top());
    ZT_TRUE("находка на видимой странице",
            !(hitLine < rig.left().start()) &&
                lineTopOf(*rig.left().document(), hitLine) - lineTopOf(*rig.left().document(), rig.left().start()) <
                    rig.left().pageHeight());
    // The editor's undo stack survives a trip through the reading mode: an
    // ordinary note, an edit, read it, come back, undo.
    rig.open(rig.plainPath);
    QTextCursor at = rig.editor->textCursor();
    at.movePosition(QTextCursor::End);
    rig.editor->setTextCursor(at);
    QTest::keyClicks(rig.editor, QStringLiteral(" typed"));
    ZT_TRUE("набор прошёл", rig.editor->document()->toPlainText().contains(QStringLiteral("typed")));
    ZT_TRUE("вход в чтение обычной заметки", rig.controller.enter());
    ZT_TRUE("страница показывает тот же документ", rig.left().document() == rig.editor->document());
    ZT_TRUE("шрифт страницы — шрифт чтения",
            rig.left().document()->defaultFont().family() != settings().style().fontFamily() ||
                settings().reading().fontFamily().isEmpty());
    rig.controller.leave();
    ZT_TRUE("редактор вернул себе документ", rig.editor->document()->textWidth() == rig.editor->viewport()->width());
    ZT_TRUE("стек отмены цел", rig.editor->document()->isUndoAvailable());
    rig.editor->undo();
    ZT_TRUE("отмена сняла набор", !rig.editor->document()->toPlainText().contains(QStringLiteral("typed")));
}

void checkSymmetry() {
    Rig rig(1600, 900);
    rig.open(rig.bookPath);
    rig.book->pageStep(+1);
    const PageStart place = rig.left().start();
    QTest::qWait(60);
    // The two pages must lay the shared document out for ONE width: a pixel
    // of difference between their viewports showed up as justified lines a
    // pixel apart after a round trip (the symmetry shot below).
    ZT_TRUE("обе страницы одной ширины",
            rig.left().viewport()->width() == rig.book->page(1).viewport()->width());
    ZT_TRUE("документ свёрстан по ширине страницы",
            qFuzzyCompare(rig.left().document()->textWidth(), qreal(rig.left().viewport()->width())));
    const QImage fresh = rig.book->grab().toImage();
    // Away to the editor and back.
    rig.controller.leave();
    QTest::qWait(60);
    const QImage editorFresh = rig.editor->grab().toImage();
    rig.controller.enter();
    QTest::qWait(60);
    ZT_TRUE("место после возврата то же", rig.left().start() == place);
    const QImage back = rig.book->grab().toImage();
    ZT_TRUE("переключился-вернулся == свежий показ (разворот, пиксель в пиксель)", back == fresh);
    rig.controller.leave();
    QTest::qWait(60);
    const QImage editorBack = rig.editor->grab().toImage();
    ZT_TRUE("и редактор после чтения рисуется как до него", editorBack == editorFresh);
    if (back != fresh || editorBack != editorFresh) {
        const QString dir = zt::TestData::outDir(QStringLiteral("book-view"));
        fresh.save(dir + QStringLiteral("/spread-fresh.png"));
        back.save(dir + QStringLiteral("/spread-back.png"));
        editorFresh.save(dir + QStringLiteral("/editor-fresh.png"));
        editorBack.save(dir + QStringLiteral("/editor-back.png"));
        std::printf("снимки: %s\n", dir.toUtf8().constData());
        // Where the two differ — the box of differing pixels, for the report.
        const auto box = [](const QImage& a, const QImage& b) {
            QRect out;
            const int w = std::min(a.width(), b.width());
            const int h = std::min(a.height(), b.height());
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                    if (a.pixel(x, y) != b.pixel(x, y)) out |= QRect(x, y, 1, 1);
            return out;
        };
        QImage diff(fresh.size(), QImage::Format_RGB32);
        diff.fill(Qt::white);
        for (int y = 0; y < std::min(fresh.height(), back.height()); ++y)
            for (int x = 0; x < std::min(fresh.width(), back.width()); ++x)
                if (fresh.pixel(x, y) != back.pixel(x, y)) diff.setPixel(x, y, 0xff000000);
        diff.save(dir + QStringLiteral("/spread-diff.png"));
        const QRect spread = box(fresh, back);
        const QRect ed = box(editorFresh, editorBack);
        std::printf("разворот: разница в %d,%d %dx%d (размеры %dx%d / %dx%d); редактор: %d,%d %dx%d\n",
                    spread.x(), spread.y(), spread.width(), spread.height(), fresh.width(),
                    fresh.height(), back.width(), back.height(), ed.x(), ed.y(), ed.width(),
                    ed.height());
    }
    // Zoom recounts the pages and keeps the place.
    rig.controller.enter();
    const int before = rig.book->pageCount();
    rig.book->applyZoom(1.5);
    QTest::qWait(400);
    ZT_TRUE("после зума страниц стало больше", rig.book->pageCount() > before);
    // The page now shown holds the line read before the zoom.
    const PageStart after = rig.left().start();
    ZT_TRUE("зум показал страницу с прежней строкой",
            !(place < after) &&
                lineTopOf(*rig.left().document(), place) - lineTopOf(*rig.left().document(), after) <
                    rig.left().pageHeight());
}

// A click on a footnote reference shows the note's text.
// CHAPTERS BEGIN A PAGE (books2): the shallowest heading level that repeats
// is the level of the chapters, and every heading down to it turns the page —
// unless it is the page's first line already. An fb2 book (one H1, chapters as
// H2) turns at both; a manual with many H1 parts turns at the parts alone.
void checkChapterPages() {
    Rig rig;
    QString error;
    const auto note = [&](const QString& text) {
        const QString id = rig.storage->createNote(QString(), false, &error);
        const QString path = rig.storage->pathOf(id);
        writeFile(path, QStringLiteral("<!-- zametti\nrole: book\n-->\n\n") + text);
        rig.storage->reload();
        return path;
    };
    const auto startsOwnPage = [&](int block) {
        rig.book->showBlock(block);
        return rig.left().start() == PageStart{block, 0};
    };
    // One H1, three short H2 chapters: each chapter on a page of its own.
    rig.open(note(QStringLiteral("# Title\n\nIntro.\n\n## One\n\nA.\n\n## Two\n\nB.\n\n## Three\n\nC.\n")));
    ZT_TRUE("книга открылась в чтении", rig.controller.active());
    ZT_TRUE("уровень глав — второй (H1 один, H2 повторяется)", rig.book->chapterLevel() == 2);
    int chapters = 0;
    for (const ZDocument::OutlineEntry& entry : rig.left().note()->outline()) {
        if (entry.level != 2) continue;
        ++chapters;
        ZT_TRUE("глава «" + entry.text.toStdString() + "» начинает страницу", startsOwnPage(entry.block));
    }
    ZT_TRUE("три главы найдены", chapters == 3);
    ZT_TRUE("первая страница по-прежнему с начала", startsOwnPage(0));
    // THE PAGE BEFORE A CHAPTER ENDS AT THE CHAPTER (owner's report,
    // 06.09.2026): the heading that turns the page must not show at the
    // foot of the page before it as well. The page knows its end, and the
    // band of the heading's line on it is blank paper.
    {
        int chapter = -1;
        for (const ZDocument::OutlineEntry& entry : rig.left().note()->outline())
            if (entry.level == 2 && entry.text == QStringLiteral("Two")) chapter = entry.block;
        rig.book->showBlock(chapter);
        rig.book->pageStep(-1);
        QTest::qWait(30);
        BookPage& page = rig.left();
        const PageStart chapterStart{chapter, 0};
        ZT_TRUE("страница перед главой кончается на главе",
                page.end().has_value() && *page.end() == chapterStart);
        const QTextDocument& doc = *page.document();
        const LineSpan span = lineSpanOf(*doc.documentLayout(), doc.findBlockByNumber(chapter), 0);
        const int scroll = page.verticalScrollBar()->value();
        const QImage shot = page.viewport()->grab().toImage();
        const QRgb paper = page.palette().color(QPalette::Base).rgb();
        const auto inkIn = [&](qreal top, qreal bottom) {
            for (int y = int(top) - scroll; y < int(bottom) - scroll; ++y) {
                if (y < 0 || y >= shot.height()) continue;
                for (int x = 0; x < shot.width(); ++x)
                    if (shot.pixel(x, y) != paper) return true;
            }
            return false;
        };
        ZT_TRUE("заголовок главы влез бы на эту страницу (иначе проверка пуста)",
                span.top - scroll < page.viewport()->height());
        ZT_TRUE("но полоса его строки закрыта бумагой", !inkIn(span.top, span.bottom));
        // And the page's own text above it is there.
        const LineSpan own = lineSpanOf(*doc.documentLayout(), doc.findBlockByNumber(page.start().block), 0);
        ZT_TRUE("собственный текст страницы на месте", inkIn(own.top, own.bottom));
    }
    // Many H1 parts with H3 sections and H4 parts inside: headings down to
    // reading.pageBreakLevel (3) turn the page, the H4 right after an H3
    // shares its page (a novel's parts inside chapters).
    rig.open(note(QStringLiteral("# Part A\n\nText.\n\n### Sec 1\n\nText.\n\n#### Small 1\n\nText.\n\n"
                                 "### Sec 2\n\nText.\n\n# Part B\n\nText.\n\n### Sec 3\n\nText.\n")));
    ZT_TRUE("уровень глав — первый (H1 повторяется)", rig.book->chapterLevel() == 1);
    ZT_TRUE("порог разрыва по умолчанию — третий уровень",
            zametti::settings().reading().pageBreakLevel() == 3);
    int parts = 0;
    int sections = 0;
    int small = -1;
    for (const ZDocument::OutlineEntry& entry : rig.left().note()->outline()) {
        if (entry.level == 1) {
            ++parts;
            ZT_TRUE("часть «" + entry.text.toStdString() + "» начинает страницу", startsOwnPage(entry.block));
        } else if (entry.level == 3) {
            ++sections;
            ZT_TRUE("раздел «" + entry.text.toStdString() + "» начинает страницу", startsOwnPage(entry.block));
        } else if (entry.level == 4 && small < 0) {
            small = entry.block;
        }
    }
    ZT_TRUE("две части и три раздела найдены", parts == 2 && sections == 3);
    ZT_TRUE("часть внутри раздела (H4) страницу не начинает", small > 0 && !startsOwnPage(small));
}

// THE COVER LEAF (books2): a book with `cover:` opens on its cover — the
// picture alone on the page (one page shown), page 1 next to it on a spread;
// turning forward shows page 1, back again the cover, Home the cover; a
// remembered place away from the start opens without the cover.
void checkCover() {
    Rig rig;
    QString error;
    const QString id = rig.storage->createNote(QString(), false, &error);
    const QString path = rig.storage->pathOf(id);
    const QString coverName = QStringLiteral("cover-test.png");
    QImage picture(40, 60, QImage::Format_RGB32);
    picture.fill(QColor(200, 30, 30));
    ZT_TRUE("картинка обложки записана",
            picture.save(QFileInfo(path).dir().filePath(coverName)));
    writeFile(path, QStringLiteral("<!-- zametti\nrole: book\ncover: %1\n-->\n\n").arg(coverName) +
                        bookText(40).mid(int(QStringLiteral("<!-- zametti\nrole: book\nlock: yes\n-->\n\n").size())));
    rig.storage->reload();
    rig.open(path);
    ZT_TRUE("книга открылась в чтении", rig.controller.active());
    ZT_TRUE("у книги есть обложка", rig.book->hasCover());
    ZT_TRUE("свежая книга открыта на обложке", rig.book->currentPage() == -1 && rig.left().coverShown());
    {
        const QImage shot = rig.left().viewport()->grab().toImage();
        const QColor mid = shot.pixelColor(shot.width() / 2, shot.height() / 2);
        ZT_TRUE("в середине листа — картинка", mid.red() > 150 && mid.green() < 80);
    }
    rig.book->pageStep(+1);
    ZT_TRUE("вперёд — первая страница текста", rig.book->currentPage() == 0 && !rig.left().coverShown());
    ZT_TRUE("и она с начала", rig.left().start() == PageStart{});
    rig.book->pageStep(-1);
    ZT_TRUE("назад — снова обложка", rig.book->currentPage() == -1 && rig.left().coverShown());
    rig.book->jump(true);
    ZT_TRUE("End — не обложка", rig.book->currentPage() > 0);
    rig.book->jump(false);
    ZT_TRUE("Home — обложка", rig.book->currentPage() == -1);
    // Read on, leave, come back: the place is remembered, not the cover.
    rig.book->pageStep(+1);
    rig.book->pageStep(+1);
    const PageStart place = rig.left().start();
    rig.open(rig.plainPath);
    rig.open(path);
    ZT_TRUE("возврат к книге — на место чтения, не на обложку",
            rig.book->currentPage() >= 0 && rig.left().start() == place);
}

void checkFootnote() {
    Rig rig;
    rig.open(rig.bookPath);
    BookPage& page = rig.left();
    const QTextDocument& doc = *page.document();
    int position = -1;
    for (QTextBlock b = doc.begin(); b.isValid() && position < 0; b = b.next())
        for (QTextBlock::iterator it = b.begin(); !it.atEnd(); ++it)
            if (it.fragment().charFormat().hasProperty(zametti::FootnoteIdProperty)) {
                position = it.fragment().position();
                break;
            }
    ZT_TRUE("ссылка на сноску есть в документе", position >= 0);
    if (position < 0) return;
    QTextCursor at(page.document());
    at.setPosition(position);
    const QRect rect = page.cursorRect(at);
    const QPointF where(rect.left() + 2, rect.center().y());
    ZT_EQ("под точкой — текст сноски", std::string("The footnote's own text."),
          page.footnoteAt(where).toStdString());
    ZT_TRUE("а рядом с текстом сноски нет", page.footnoteAt(QPointF(where.x() - 60, where.y())).isEmpty());
    // The reference reads as a number (books2): the label of `[^n1]` is «1»,
    // the id stays in the format for the file.
    {
        const QTextBlock block = doc.findBlock(position);
        QString label;
        QString id;
        for (QTextBlock::iterator it = block.begin(); !it.atEnd(); ++it)
            if (it.fragment().charFormat().hasProperty(zametti::FootnoteIdProperty)) {
                label = it.fragment().text();
                id = it.fragment().charFormat().stringProperty(zametti::FootnoteIdProperty);
                break;
            }
        ZT_EQ("ярлык ссылки — номер", std::string("1"), label.toStdString());
        ZT_EQ("id в формате — целиком", std::string("n1"), id.toStdString());
        // And the file gets the id, not the label.
        const std::string written = page.note()->toMarkdown();
        ZT_TRUE("в файл уходит [^n1], а не ярлык",
                written.find("with a note.[^n1]") != std::string::npos &&
                    written.find("[^1]") == std::string::npos);
    }
    // Hover: the tooltip event on the viewport shows the note, escaped and
    // wrapped as a paragraph; off the reference it shows nothing.
    {
        QHelpEvent hover(QEvent::ToolTip, where.toPoint(), page.viewport()->mapToGlobal(where.toPoint()));
        QApplication::sendEvent(page.viewport(), &hover);
        ZT_TRUE("наведение показало плашку со сноской",
                hover.isAccepted() && QToolTip::text().contains(QStringLiteral("The footnote's own text.")));
        ZT_TRUE("в плашке нет id", !QToolTip::text().contains(QStringLiteral("n1")));
        const QPoint away(int(where.x()) - 60, int(where.y()));
        QHelpEvent none(QEvent::ToolTip, away, page.viewport()->mapToGlobal(away));
        QApplication::sendEvent(page.viewport(), &none);
        ZT_TRUE("рядом с текстом плашки нет", !none.isAccepted());
    }
}

// BOOKMARKS ON THE PAGE (brief 18): a double click in the left margin sets
// one on the paragraph and paints the glyph; the key takes it off; two of
// them and Ctrl+] / Ctrl+[ turn to the next / previous; the store's file
// holds them; the editor shows the same glyphs; after closing and opening
// the note the bookmark is found again by its text.
void checkBookmarks() {
    Rig rig;
    rig.open(rig.bookPath);
    ZT_TRUE("в чтении", rig.controller.active());
    BookPage& page = rig.left();
    const QTextDocument& doc = *page.document();
    // The first paragraph on the page with text: its first line's y.
    int block = page.start().block;
    while (block < doc.blockCount() && doc.findBlockByNumber(block).text().trimmed().isEmpty()) ++block;
    const QRectF rect = doc.documentLayout()->blockBoundingRect(doc.findBlockByNumber(block));
    const QPointF inMargin(rect.left() / 2.0, rect.top() + 4.0 - page.verticalScrollBar()->value());
    ZT_TRUE("точка в левом поле над абзацем", page.marginBlockAt(inMargin) == block);
    QTest::mouseDClick(page.viewport(), Qt::LeftButton, Qt::NoModifier, inMargin.toPoint());
    QTest::qWait(30);
    const std::shared_ptr<ZNote> note = page.note();
    ZT_TRUE("двойной клик по полю поставил букмарк", note->bookmarks().at(block) != nullptr);
    ZT_TRUE("файл букмарков появился", QFile::exists(rig.storage->bookmarksPath()));
    ZT_TRUE("глиф рисуется в поле: ячейка внутри поля документа",
            page.bookmarkCell(doc.findBlockByNumber(block), rect).right() <= rect.left() &&
                page.bookmarkCell(doc.findBlockByNumber(block), rect).width() > 4.0);
    // The key on the page's first paragraph takes it off again.
    page.setFocus();
    QTest::keyClick(&page, Qt::Key_B, Qt::ControlModifier | Qt::ShiftModifier);
    QTest::qWait(20);
    ZT_TRUE("Ctrl+Shift+B снял букмарк с первого абзаца страницы", note->bookmarks().at(block) == nullptr);
    ZT_TRUE("в файле — надгробие", rig.storage->bookmarks().forNote(note->id()).empty() &&
                                    !rig.storage->bookmarks().all().empty());
    // Two bookmarks deep in the book; Ctrl+] from the start turns to the first.
    const int deep1 = block + 40;
    const int deep2 = block + 80;
    rig.editor->toggleBookmark(deep1);
    rig.editor->toggleBookmark(deep2);
    // A blank line carries no bookmark: it went to the next paragraph with text.
    const std::vector<int> set = note->bookmarks().blocks();
    ZT_TRUE("два букмарка", set.size() == 2);
    ZT_TRUE("оба на абзацах с текстом",
            set.size() == 2 && !doc.findBlockByNumber(set[0]).text().trimmed().isEmpty() &&
                !doc.findBlockByNumber(set[1]).text().trimmed().isEmpty());
    QTest::keyClick(&page, Qt::Key_BracketRight, Qt::ControlModifier);
    QTest::qWait(30);
    ZT_TRUE("Ctrl+] — страница с первым букмарком",
            set.size() == 2 && !(PageStart{set[0], 0} < page.start()) && rig.book->currentPage() > 0);
    const int pageOfDeep1 = rig.book->currentPage();
    QTest::keyClick(&page, Qt::Key_BracketRight, Qt::ControlModifier);
    QTest::qWait(30);
    ZT_TRUE("ещё Ctrl+] — дальше", rig.book->currentPage() > pageOfDeep1);
    QTest::keyClick(&page, Qt::Key_BracketLeft, Qt::ControlModifier);
    QTest::qWait(30);
    ZT_TRUE("Ctrl+[ — назад к первому", rig.book->currentPage() == pageOfDeep1);
    // Back in the editor: the same glyphs, a double click in the margin toggles.
    rig.controller.leave();
    QTest::qWait(30);
    ZT_TRUE("редактор видит букмарки",
            set.size() == 2 && rig.editor->blockBookmarked(set[0]) && rig.editor->blockBookmarked(set[1]));
    // Close and open the note again: found by text. The book was left in the
    // editor, so it opens in the editor — the mode is remembered by note.
    rig.open(rig.plainPath);
    rig.open(rig.bookPath);
    ZT_TRUE("книга, оставленная в редакторе, открылась в редакторе", !rig.controller.active());
    const std::shared_ptr<ZNote> again = rig.editor->noteHandle();
    ZT_TRUE("после повторного открытия букмарки найдены по тексту",
            again != nullptr && again->bookmarks().blocks() == set);
    ZT_TRUE("и в чтение — по кнопке", rig.controller.enter() && rig.left().note() == again);
}

}  // namespace

TEST(BookView, ChapterPages) {
    checkChapterPages();
    EXPECT_EQ(0, zt::report("book-chapter-pages"));
}

TEST(BookView, Cover) {
    checkCover();
    EXPECT_EQ(0, zt::report("book-cover"));
}

TEST(BookView, Footnote) {
    checkFootnote();
    EXPECT_EQ(0, zt::report("book-footnote"));
}

TEST(BookView, Bookmarks) {
    checkBookmarks();
    EXPECT_EQ(0, zt::report("book-bookmarks"));
}

TEST(BookView, PagesAndKeys) {
    checkPagesAndKeys();
    EXPECT_EQ(0, zt::report("book-pages"));
}

TEST(BookView, Spread) {
    checkSpread();
    EXPECT_EQ(0, zt::report("book-spread"));
}

TEST(BookView, SearchAndUndo) {
    checkSearchAndUndo();
    EXPECT_EQ(0, zt::report("book-search"));
}

TEST(BookView, Symmetry) {
    checkSymmetry();
    EXPECT_EQ(0, zt::report("book-symmetry"));
}
