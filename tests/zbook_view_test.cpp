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
#include "book_pages.h"
#include "editor_widget.h"
#include "reading_controller.h"
#include "settings.h"
#include "test_util.h"
#include "testdata.h"
#include "zbook_view.h"
#include "zstorage.h"

#include <QApplication>
#include <QFile>
#include <QImage>
#include <QRect>
#include <QScrollBar>
#include <QSignalSpy>
#include <QStackedWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFrame>
#include <QTextLayout>
#include <QVBoxLayout>
#include <memory>
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
        out += QStringLiteral("Paragraph %1 of the book, long enough to wrap onto several lines of a "
                              "narrow page when the page is narrow, and to hold a word to find: "
                              "needle%1 sits here.\n\n")
                   .arg(i);
    }
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
    // Turn forward: the new start is the first line that did not fit whole.
    rig.book->pageStep(+1);
    const PageStart second = rig.left().start();
    ZT_TRUE("вторая страница дальше первой", PageStart{} < second);
    const qreal height = rig.left().pageHeight();
    const qreal first = lineTopOf(doc, PageStart{});
    const qreal top = lineTopOf(doc, second);
    // The line before it fitted whole (checked below); this one begins within
    // a line of the page's bottom — right at it when the previous line ended
    // exactly there.
    ZT_TRUE("начало второй — строка, не влезшая на первую (её верх у нижней кромки)",
            top <= first + height + QFontMetricsF(rig.left().font()).height() &&
                top > first + height - 4 * QFontMetricsF(rig.left().font()).height());
    // The line before it fits whole on page one.
    {
        PageStart before = second;
        if (before.line > 0) --before.line;
        else --before.block;
        const qreal bottom =
            lineSpanOf(*doc.documentLayout(), doc.findBlockByNumber(before.block), before.line).bottom;
        ZT_TRUE("строка перед ней влезла целиком", bottom <= first + height + 0.5);
    }
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
    ZT_TRUE("два букмарка", note->bookmarks().blocks().size() == 2);
    QTest::keyClick(&page, Qt::Key_BracketRight, Qt::ControlModifier);
    QTest::qWait(30);
    ZT_TRUE("Ctrl+] — страница с первым букмарком",
            !(PageStart{deep1, 0} < page.start()) && rig.book->currentPage() > 0);
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
    ZT_TRUE("редактор видит букмарки", rig.editor->blockBookmarked(deep1) && rig.editor->blockBookmarked(deep2));
    // Close and open the note again: found by text.
    rig.open(rig.plainPath);
    rig.open(rig.bookPath);
    const std::shared_ptr<ZNote> again = rig.left().note();
    ZT_TRUE("после повторного открытия букмарки найдены по тексту",
            again != nullptr && (again->bookmarks().blocks() == std::vector<int>{deep1, deep2}));
}

}  // namespace

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
