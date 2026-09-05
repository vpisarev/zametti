// THE TABLE OF CONTENTS (brief 18): the outline of a note and the list that
// shows it at the right edge of the text.
//
// The outline lists every heading in order with its level; a `#` inside a
// code block is not a heading; the outline follows the document (a heading
// renamed — the next ask sees the new name). The list opens with the heading
// above the caret selected, goes away on Enter (and the text goes to the
// heading) or on Esc (and the text stays), and follows its anchor's geometry.
#include "editor_widget.h"
#include "test_util.h"
#include "toc_list.h"
#include "zstorage.h"

#include <QApplication>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTextBlock>
#include <QTextCursor>
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

const char* kNote =
    "<!-- zametti\n-->\n\n# Title\n\nIntro.\n\n## First\n\nText one.\n\n```\n# not a heading\n```\n\n"
    "### Deeper\n\nText two.\n\n## Second\n\nText three.\n";

struct Rig {
    QTemporaryDir home;
    std::shared_ptr<ZStorage> storage;
    QWidget window;
    NoteEditor* editor;
    TocList* toc;
    QString path;

    Rig() : editor(new NoteEditor(&window)), toc(new TocList(&window)) {
        const QString root = home.path() + QStringLiteral("/store");
        QString error;
        ZStorage(root).init(&error);
        storage = std::make_shared<ZStorage>(root);
        storage->reload();
        path = storage->pathOf(storage->createNote(QString(), false, &error));
        writeFile(path, QString::fromUtf8(kNote));
        storage->reload();
        editor->setStorage(storage);
        auto* layout = new QVBoxLayout(&window);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->addWidget(editor, 1);
        toc->follow(editor);
        window.resize(900, 700);
        window.show();
        QTest::qWait(30);
        editor->openFile(path);
        QTest::qWait(80);
    }
};

void checkOutline() {
    Rig rig;
    const auto& outline = rig.editor->noteHandle()->outline();
    ZT_TRUE("четыре заголовка, код не в счёт", outline.size() == 4);
    if (outline.size() == 4) {
        ZT_EQ("первый — заголовок заметки", std::string("Title"), outline[0].text.toStdString());
        ZT_TRUE("уровни 1, 2, 3, 2",
                outline[0].level == 1 && outline[1].level == 2 && outline[2].level == 3 &&
                    outline[3].level == 2);
        ZT_TRUE("блоки по порядку", outline[0].block < outline[1].block && outline[1].block < outline[2].block &&
                                       outline[2].block < outline[3].block);
        ZT_EQ("третий — Deeper", std::string("Deeper"), outline[2].text.toStdString());
    }
    // Rename a heading: the next ask sees it.
    QTextCursor at = rig.editor->note().caretAtBlock(outline.size() == 4 ? outline[1].block : 0);
    at.movePosition(QTextCursor::EndOfBlock);
    rig.editor->setTextCursor(at);
    QTest::keyClicks(rig.editor, QStringLiteral(" chapter"));
    const auto& fresh = rig.editor->noteHandle()->outline();
    ZT_TRUE("после правки оглавление свежее", fresh.size() == 4 && fresh[1].text == QStringLiteral("First chapter"));
}

void checkList() {
    Rig rig;
    const auto& outline = rig.editor->noteHandle()->outline();
    ZT_TRUE("оглавление есть", outline.size() == 4);
    if (outline.size() != 4) return;
    // The caret in "Text two." — under Deeper.
    QTextCursor at = rig.editor->note().caretAtBlock(outline[2].block + 2);
    rig.editor->setTextCursor(at);
    const int caretBlock = rig.editor->textCursor().blockNumber();
    QSignalSpy chosen(rig.toc, &TocList::headingChosen);
    QSignalSpy closed(rig.toc, &TocList::closed);

    rig.toc->open(rig.editor->noteHandle(), caretBlock);
    QTest::qWait(30);
    ZT_TRUE("список показан", rig.toc->isOpen());
    ZT_TRUE("список у правого края текста",
            rig.toc->geometry().right() == rig.editor->geometry().right() &&
                rig.toc->geometry().top() == rig.editor->geometry().top());
    ZT_TRUE("список уже трети текста", rig.toc->width() <= rig.editor->width() / 3);
    ZT_TRUE("фокус у списка", rig.toc->isAncestorOf(QApplication::focusWidget()));
    // Esc: gone, the text where it was.
    QTest::keyClick(QApplication::focusWidget(), Qt::Key_Escape);
    QTest::qWait(20);
    ZT_TRUE("Esc прячет список", !rig.toc->isOpen());
    ZT_TRUE("и говорит об этом", closed.count() == 1);
    ZT_TRUE("каретка на месте", rig.editor->textCursor().blockNumber() == caretBlock);
    ZT_TRUE("выбора не было", chosen.count() == 0);

    // Open again: Deeper is selected; Up, Enter → First.
    rig.toc->open(rig.editor->noteHandle(), caretBlock);
    QTest::qWait(20);
    QTest::keyClick(QApplication::focusWidget(), Qt::Key_Up);
    QTest::keyClick(QApplication::focusWidget(), Qt::Key_Return);
    QTest::qWait(20);
    ZT_TRUE("Enter прячет список", !rig.toc->isOpen());
    ZT_TRUE("выбран заголовок над предыдущим", chosen.count() == 1 &&
                                                 chosen.last().at(0).toInt() == outline[1].block);
    // Resize the anchor: the list follows while open.
    rig.toc->open(rig.editor->noteHandle(), caretBlock);
    rig.window.resize(1200, 800);
    QTest::qWait(40);
    ZT_TRUE("после resize список у нового правого края",
            rig.toc->geometry().right() == rig.editor->geometry().right());
    // A click elsewhere (focus away) closes it.
    rig.editor->setFocus();
    QTest::qWait(20);
    ZT_TRUE("ушёл фокус — список закрылся", !rig.toc->isOpen());
}

}  // namespace

TEST(Toc, Outline) {
    checkOutline();
    EXPECT_EQ(0, zt::report("toc-outline"));
}

TEST(Toc, List) {
    checkList();
    EXPECT_EQ(0, zt::report("toc-list"));
}
