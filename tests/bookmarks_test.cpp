// BOOKMARKS (brief 18): the store's file, the note's anchors, the healing.
//
// The file: parse and write back (unknown keys survive), tombstones, a merge
// that is commutative and never resurrects a deletion. The store: set, remove,
// reload from disk; verify says nothing about the file, and still calls a
// stranger a stranger. The note: a bookmark is found again by its text after
// the note is closed and opened; text inserted above it — found; the
// paragraph deleted — lost; an edit inside the paragraph keeps the anchor and
// the save heals the record; the order of anchors is the order of the
// document; walking next / previous.
#include "note_bookmarks.h"
#include "test_util.h"
#include "times.h"
#include "zbookmarks.h"
#include "znote.h"
#include "zstorage.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTextCursor>
#include <cstdio>
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

ZBookmarks::Entry entry(const char* id, const char* note, const char* snippet, const char* updated,
                        bool deleted = false) {
    ZBookmarks::Entry e;
    e.id = QString::fromLatin1(id);
    e.note = QString::fromLatin1(note);
    e.snippet = QString::fromUtf8(snippet);
    e.created = QStringLiteral("2026-09-05T20:00:00+03:00");
    e.updated = QString::fromLatin1(updated);
    e.deleted = deleted;
    return e;
}

void checkFile() {
    ZBookmarks a;
    a.set(entry("01aaaaaaaaaaaa", "01n1", "First words", "2026-09-05T20:00:00+03:00"));
    a.set(entry("01bbbbbbbbbbbb", "01n1", "Second words", "2026-09-05T20:01:00+03:00"));
    a.set(entry("01cccccccccccc", "01n2", "Other note", "2026-09-05T20:02:00+03:00"));
    ZT_TRUE("две у первой заметки", a.forNote(QStringLiteral("01n1")).size() == 2);
    ZT_TRUE("надгробие ставится", a.remove(QStringLiteral("01bbbbbbbbbbbb"), QStringLiteral("2026-09-05T20:05:00+03:00")));
    ZT_TRUE("второе надгробие — уже нет", !a.remove(QStringLiteral("01bbbbbbbbbbbb"), QStringLiteral("2026-09-05T20:06:00+03:00")));
    ZT_TRUE("удалённая не в списке заметки", a.forNote(QStringLiteral("01n1")).size() == 1);
    ZT_TRUE("но запись осталась", a.find(QStringLiteral("01bbbbbbbbbbbb")) != nullptr &&
                                   a.find(QStringLiteral("01bbbbbbbbbbbb"))->deleted);

    // Round trip with a stranger's keys, top-level and per record.
    const QByteArray hand = R"({ "version": 1, "theirs": "kept",
        "bookmarks": [ { "id": "01dddddddddddd", "note": "01n1", "snippet": "Hand", "line": 3,
                         "created": "2026-09-05T20:00:00+03:00", "updated": "2026-09-05T20:00:00+03:00",
                         "colour": "red" },
                       { "no id": true } ] })";
    ZBookmarks parsed;
    QString error;
    ZT_TRUE("рукописный файл читается: " + error.toStdString(), parsed.parse(hand, &error));
    ZT_TRUE("запись без id пропущена, с id — есть", parsed.all().size() == 1);
    const QByteArray back = parsed.toBytes();
    ZT_TRUE("чужой ключ файла пережил перезапись", back.contains("\"theirs\": \"kept\""));
    ZT_TRUE("чужой ключ записи пережил перезапись", back.contains("\"colour\": \"red\""));
    ZBookmarks again;
    ZT_TRUE("и читается снова", again.parse(back, &error) && again.all().size() == 1);
    ZBookmarks future;
    ZT_TRUE("файл из будущего отвергается вслух",
            !future.parse(R"({ "version": 7, "bookmarks": [] })", &error) && error.contains(QStringLiteral("newer")));
    ZBookmarks junk;
    ZT_TRUE("не JSON — ошибка, не падение", !junk.parse("{{{", &error));

    // Merge: union by id, the later update wins, a tombstone never comes back.
    ZBookmarks left;
    left.set(entry("01aaaaaaaaaaaa", "01n1", "Old text", "2026-09-05T20:00:00+03:00"));
    left.set(entry("01bbbbbbbbbbbb", "01n1", "Gone here", "2026-09-05T20:09:00+03:00", true));
    left.set(entry("01eeeeeeeeeeee", "01n1", "Only left", "2026-09-05T20:00:00+03:00"));
    ZBookmarks right;
    right.set(entry("01aaaaaaaaaaaa", "01n1", "New text", "2026-09-05T21:00:00+03:00"));
    right.set(entry("01bbbbbbbbbbbb", "01n1", "Gone here", "2026-09-05T20:01:00+03:00"));
    right.set(entry("01ffffffffffff", "01n1", "Only right", "2026-09-05T20:00:00+03:00"));
    const ZBookmarks lr = left.mergedWith(right);
    const ZBookmarks rl = right.mergedWith(left);
    ZT_EQ("слияние коммутативно", lr.toBytes().toStdString(), rl.toBytes().toStdString());
    ZT_TRUE("четыре id в объединении", lr.all().size() == 4);
    ZT_TRUE("побеждает позднее обновление", lr.find(QStringLiteral("01aaaaaaaaaaaa"))->snippet == QStringLiteral("New text"));
    ZT_TRUE("надгробие не воскресает", lr.find(QStringLiteral("01bbbbbbbbbbbb"))->deleted);
    ZT_TRUE("идемпотентно", lr.mergedWith(right).toBytes() == lr.toBytes());
    // Same stamp, one side deleted: the deletion wins whichever side it is on.
    ZBookmarks x, y;
    x.set(entry("01gggggggggggg", "01n1", "Tie", "2026-09-05T20:00:00+03:00", true));
    y.set(entry("01gggggggggggg", "01n1", "Tie", "2026-09-05T20:00:00+03:00"));
    ZT_TRUE("при равных метках побеждает надгробие", x.mergedWith(y).find(QStringLiteral("01gggggggggggg"))->deleted &&
                                                       y.mergedWith(x).find(QStringLiteral("01gggggggggggg"))->deleted);
}

const char* kBook =
    "<!-- zametti\nrole: book\n-->\n\n# Book\n\n## One\n\nAlpha paragraph, the first of the chapter.\n\n"
    "Beta paragraph follows the first.\n\n## Two\n\nGamma paragraph opens the second chapter.\n\n"
    "Delta paragraph closes it.\n";

void checkStoreAndNote() {
    QTemporaryDir home;
    const QString root = home.path() + QStringLiteral("/store");
    QString error;
    ZT_TRUE("хранилище заведено", ZStorage(root).init(&error));
    auto storage = std::make_shared<ZStorage>(root);
    storage->reload();
    const QString id = storage->createNote(QString(), false, &error);
    const QString path = storage->pathOf(id);
    ZT_TRUE("книга записана", writeFile(path, QString::fromUtf8(kBook)));
    storage->reload();
    ZT_TRUE("букмарков нет и это не ошибка", storage->bookmarks().empty());

    // Open the note, bookmark "Gamma…" (block of the third paragraph).
    auto note = std::make_shared<ZNote>();
    {
        QFile f(path);
        (void)f.open(QIODevice::ReadOnly);
        const QByteArray bytes = f.readAll();
        note->load(std::string_view(bytes.constData(), size_t(bytes.size())));
    }
    ZDocument& doc = note->doc();
    int gamma = -1, alpha = -1, delta = -1;
    for (int i = 0; i < doc.blockCount(); ++i) {
        const QString t = doc.blockAt(i).text;
        if (t.startsWith(QStringLiteral("Gamma"))) gamma = i;
        if (t.startsWith(QStringLiteral("Alpha"))) alpha = i;
        if (t.startsWith(QStringLiteral("Delta"))) delta = i;
    }
    ZT_TRUE("абзацы найдены", gamma > 0 && alpha > 0 && delta > gamma);
    const ZBookmarks::Entry made = note->bookmarks().add(doc, id, gamma, store::isoNow());
    // Forty characters: the period at the forty-first is cut.
    ZT_EQ("сниппет — сорок знаков начала абзаца", std::string("Gamma paragraph opens the second chapter"),
          made.snippet.toStdString());
    ZT_EQ("заголовок над абзацем", std::string("Two"), made.heading.toStdString());
    ZT_TRUE("строка источника известна", made.line > 0);
    ZT_TRUE("записано в хранилище", storage->setBookmark(made, &error));
    ZT_TRUE("файл появился", QFile::exists(storage->bookmarksPath()));
    const ZBookmarks::Entry second = note->bookmarks().add(doc, id, alpha, store::isoNow());
    ZT_TRUE("второй записан", storage->setBookmark(second, &error));
    ZT_TRUE("порядок якорей — порядок документа",
            (note->bookmarks().blocks() == std::vector<int>{alpha, gamma}));
    ZT_TRUE("следующий после alpha — gamma; после gamma — нет",
            note->bookmarks().next(alpha) == gamma && note->bookmarks().next(gamma) < 0);
    ZT_TRUE("предыдущий перед gamma — alpha; перед alpha — нет",
            note->bookmarks().previous(gamma) == alpha && note->bookmarks().previous(alpha) < 0);

    // verify: quiet about the file; a stranger is still a stranger.
    {
        ZStorage::Report report;
        ZStorage(root).verify(report);
        for (const QString& p : report.lines)
            if (p.startsWith(QStringLiteral("PROBLEM"))) std::printf("%s\n", p.toUtf8().constData());
        ZT_TRUE("verify молчит про bookmarks.json", report.problems == 0);
        writeFile(QDir(root).filePath(QStringLiteral("stranger.json")), QStringLiteral("{}"));
        ZStorage::Report again;
        ZStorage(root).verify(again);
        bool foreign = false;
        for (const QString& p : again.lines) foreign = foreign || p.contains(QStringLiteral("foreign file: stranger.json"));
        ZT_TRUE("а чужой файл в корне по-прежнему чужой", foreign);
    }

    // Reopen the store and the note: found again by text.
    auto fresh = std::make_shared<ZStorage>(root);
    fresh->reload();
    ZT_TRUE("после перечитывания две записи", fresh->bookmarks().forNote(id).size() == 2);
    auto reopened = std::make_shared<ZNote>();
    {
        QFile f(path);
        (void)f.open(QIODevice::ReadOnly);
        const QByteArray bytes = f.readAll();
        reopened->load(std::string_view(bytes.constData(), size_t(bytes.size())));
    }
    reopened->bookmarks().resolve(fresh->bookmarks().forNote(id), reopened->doc());
    ZT_TRUE("найдены обе", (reopened->bookmarks().blocks() == std::vector<int>{alpha, gamma}));

    // Text inserted above: the anchor follows the paragraph.
    ZDocument& d2 = reopened->doc();
    const int blocksBefore = d2.blockCount();
    {
        QTextCursor at = d2.caretAtBlock(alpha);
        at.movePosition(QTextCursor::EndOfBlock);
        ZT_TRUE("вставка прошла", d2.insertText(at, QStringLiteral(" Inserted words.")));
        at = d2.caretAtBlock(alpha);
        at.movePosition(QTextCursor::EndOfBlock);
        ZT_TRUE("разрез прошёл", d2.breakBlock(at, ZDocument::BreakKind::Plain));
        at = d2.caretAtBlock(alpha + 1);
        ZT_TRUE("новый абзац набран", d2.insertText(at, QStringLiteral("A whole new paragraph above.")));
    }
    (void)blocksBefore;
    // The edits above shifted the blocks (the blank-line invariant even
    // folded two of them); the anchor is still on its paragraph.
    const NoteBookmarks::Anchor* g = reopened->bookmarks().byId(made.id);
    ZT_TRUE("после правок выше якорь остался на своём абзаце",
            g != nullptr && !g->lost() &&
                d2.blockAt(g->block()).text.startsWith(QStringLiteral("Gamma")));
    ZT_TRUE("а номер блока другой", g != nullptr && g->block() != gamma);
    // An edit inside the bookmarked paragraph: the anchor stays; the save heals the record.
    {
        QTextCursor at = d2.caretAtBlock(g->block());
        d2.insertText(at, QStringLiteral("Changed "));
    }
    g = reopened->bookmarks().byId(made.id);
    ZT_TRUE("правка внутри абзаца — якорь на месте",
            g != nullptr && d2.blockAt(g->block()).text.startsWith(QStringLiteral("Changed Gamma")));
    const std::vector<ZBookmarks::Entry> healed = reopened->bookmarks().healed(d2, QStringLiteral("2026-09-06T10:00:00+03:00"));
    bool healedGamma = false;
    for (const ZBookmarks::Entry& e : healed)
        if (e.id == made.id) healedGamma = e.snippet.startsWith(QStringLiteral("Changed Gamma")) && e.line != made.line;
    ZT_TRUE("самовосстановление переписало сниппет и строку", healedGamma);
    // A lost one: the paragraph deleted.
    {
        QTextCursor at = d2.caretAtBlock(g->block());
        d2.removeBlocks(at, g->block(), g->block());
    }
    ZT_TRUE("после удаления абзаца якорь чинится по тексту — и не находит его",
            reopened->bookmarks().repair(d2) && reopened->bookmarks().byId(made.id)->lost());
    ZT_TRUE("вторая при этом цела", !reopened->bookmarks().byId(second.id)->lost());
    // Remove from the store: a tombstone in the file.
    ZT_TRUE("снята", fresh->removeBookmark(made.id, store::isoNow(), &error));
    ZT_TRUE("в списке заметки осталась одна", fresh->bookmarks().forNote(id).size() == 1);
    {
        QFile f(fresh->bookmarksPath());
        (void)f.open(QIODevice::ReadOnly);
        ZT_TRUE("надгробие в файле", f.readAll().contains("\"deleted\": true"));
    }
}

}  // namespace

TEST(Bookmarks, File) {
    checkFile();
    EXPECT_EQ(0, zt::report("bookmarks-file"));
}

TEST(Bookmarks, StoreAndNote) {
    checkStoreAndNote();
    EXPECT_EQ(0, zt::report("bookmarks-note"));
}
