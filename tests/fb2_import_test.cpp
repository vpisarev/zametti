// BOOKS FROM FB2 (brief 18): the reader, the store's importBook and the
// corpus.
//
// Two tiers, as everywhere (tests/testdata.h):
//   * the public sample — a hand-written fb2 that exercises every row of the
//     brief's table (nested sections, an epigraph with a poem, a cite, a poem
//     with a hanging indent, notes of one and two paragraphs, a bad note id,
//     inline and block pictures, a missing picture, a table, sub/sup, an
//     external and a local link, escaped HTML in the annotation) and its
//     windows-1251 twin. The result is compared with a golden markdown,
//     attachment names and the `modified` stamp set aside;
//   * the owner's private corpus (.testdata/books, eleven real books): every
//     one imports whole, passes the canonical self-check, and every
//     `a[type=note]` of the source becomes exactly one reference with a
//     definition to match. Timings are printed for the report.

#include "fb2_book.h"
#include "note_tree.h"
#include "scratch_files.h"
#include "znote.h"
#include "zstorage.h"
#include "test_util.h"
#include "testdata.h"

#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QRegularExpression>
#include <QTemporaryDir>

#include <cstdio>
#include <set>
#include <string>

namespace {

using zametti::Fb2Book;
using zametti::ImportLimits;
using zametti::Kind;
using zametti::Piece;
using zametti::Run;
using zametti::ZNote;
using zametti::ZStorage;

std::string n(long long v) { return std::to_string(v); }

QString readAll(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QString::fromUtf8(f.readAll());
}

// What the golden cannot pin down: attachment ids are minted at import, the
// `modified` stamp is "now", the source's hash names the exact bytes.
QString normalised(QString text) {
    static const QRegularExpression attachment(
        QStringLiteral("01[0-9a-hjkmnp-tv-z]{12}\\.(jxl|png|webp|jpe?g)"));
    static const QRegularExpression modified(QStringLiteral("^modified: .*\\n"),
                                             QRegularExpression::MultilineOption);
    text.replace(attachment, QStringLiteral("<attachment>"));
    text.remove(modified);
    return text;
}

QString withoutSourceHash(QString text) {
    static const QRegularExpression source(QStringLiteral("^source: .*\\n"),
                                           QRegularExpression::MultilineOption);
    return text.remove(source);
}

// Every reference has a definition; orphans among the definitions are fine.
int danglingReferences(const std::vector<Piece>& pieces) {
    std::set<QString> defined;
    for (const Piece& piece : pieces)
        if (!piece.raw && piece.kind == Kind::Footnote) defined.insert(piece.info);
    int dangling = 0;
    for (const Piece& piece : pieces) {
        for (const Run& run : piece.runs) {
            if (!run.footnote()) continue;
            const QStringView written = piece.view(run);
            const QString id = written.mid(2, written.size() - 3).toString();
            if (defined.find(id) == defined.end()) ++dangling;
        }
    }
    return dangling;
}

int countNoteAnchors(const QString& fb2Path) {
    QFile f(fb2Path);
    if (!f.open(QIODevice::ReadOnly)) return -1;
    const QByteArray bytes = f.readAll();
    int count = 0;
    qsizetype at = 0;
    for (;;) {
        at = bytes.indexOf("type=\"note\"", at);
        if (at < 0) break;
        ++count;
        at += 11;
    }
    return count;
}

struct Store {
    QTemporaryDir dir;
    ZStorage storage{dir.path()};
    bool ok = false;
    Store() {
        QString error;
        ok = dir.isValid() && storage.init(&error);
    }
};

void checkSample() {
    const QString sample = zt::TestData::file(QStringLiteral("fb2/sample.fb2"));
    const QString golden = zt::TestData::file(QStringLiteral("fb2/sample.md"));
    const QString twin = zt::TestData::file(QStringLiteral("fb2/sample-cp1251.fb2"));
    ZT_TRUE("образец и эталон на месте", !sample.isEmpty() && !golden.isEmpty() && !twin.isEmpty());
    if (sample.isEmpty() || golden.isEmpty() || twin.isEmpty()) return;

    // The reader alone.
    {
        QFile f(sample);
        ZT_TRUE("образец читается", f.open(QIODevice::ReadOnly));
        Fb2Book book;
        QString error;
        ZT_TRUE("образец разобран: " + error.toStdString(), book.load(f.readAll(), &error));
        ZT_EQ("заголовок", "Пробная книга: всё, что умеет fb2", book.title().toStdString());
        ZT_EQ("ссылок на сноски", "3", n(book.stats().references));
        ZT_EQ("определений сносок", "3", n(book.stats().footnotes));
        ZT_EQ("перенумерован один id", "1", n(book.stats().renumberedIds));
        ZT_EQ("картинок в тексте (включая пропавшую)", "3", n(book.stats().images));
        ZT_EQ("таблица одна", "1", n(book.stats().tables));
        ZT_EQ("обложка", "cover.png", book.coverId().toStdString());
        ZT_EQ("дата документа", "2021-10-22", book.created().toStdString());
        ZT_EQ("висячих ссылок нет", "0", n(danglingReferences(book.pieces())));
        ZT_EQ("двоичных данных две", "2", n(int(book.binaries().size())));
        ZT_EQ("на них ссылаются: картинка, пропавшая, обложка", "3", n(int(book.referencedBinaries().size())));
    }

    Store store;
    ZT_TRUE("временное хранилище", store.ok);
    if (!store.ok) return;
    QString error;
    ZStorage::BookImport report;
    const QString id = store.storage.importBook(QString(), sample, ImportLimits{}, &error, &report);
    ZT_TRUE("ввоз удался: " + error.toStdString(), !id.isEmpty());
    if (id.isEmpty()) return;
    ZT_EQ("две картинки легли вложениями", "2", n(report.images));
    ZT_EQ("одна пропала (нет binary)", "1", n(report.imagesFailed));

    const QString path = store.storage.pathOf(id);
    const QString text = readAll(path);
    ZT_EQ("эталон markdown (без id вложений и modified)", readAll(golden).toStdString(),
          normalised(text).toStdString());

    // The canon holds on what was written.
    {
        const QByteArray bytes = text.toUtf8();
        ZNote back;
        back.load(std::string_view(bytes.constData(), size_t(bytes.size())));
        ZT_EQ("записанное читается в себя", bytes.toStdString(), back.toMarkdown());
        ZT_TRUE("книга", back.isBook());
        ZT_TRUE("заперта", back.isLocked());
        ZT_EQ("автор", "Иван Петрович Пробников, соавтор", back.bookAuthor().toStdString());
        ZT_EQ("год издания", "2001", back.bookYear().toStdString());
        const QString cover = back.headerValue(QStringLiteral("cover"));
        ZT_TRUE("обложка названа", !cover.isEmpty());
        ZT_TRUE("и лежит рядом", QFile::exists(store.dir.path() + QLatin1Char('/') + cover));
        ZT_TRUE("source с отпечатком", back.headerValue(QStringLiteral("source")).startsWith(
                                          QStringLiteral("sample.fb2 blake3:")));
    }
    // The index knows the book without opening it.
    {
        store.storage.reload();
        const ZStorage::NoteInfo* info = store.storage.info(id);
        ZT_TRUE("заметка в индексе", info != nullptr);
        ZT_TRUE("в индексе — книга и замок", info != nullptr && info->book() && info->locked());
        ZT_EQ("автор в индексе", "Иван Петрович Пробников, соавтор",
              info != nullptr ? info->author().toStdString() : std::string());
    }

    // The windows-1251 twin gives the same note, byte for byte apart from the
    // hash of the source.
    const QString twinId = store.storage.importBook(QString(), twin, ImportLimits{}, &error);
    ZT_TRUE("cp1251-близнец ввезён: " + error.toStdString(), !twinId.isEmpty());
    if (!twinId.isEmpty()) {
        ZT_EQ("cp1251 == utf-8", withoutSourceHash(normalised(text)).toStdString(),
              withoutSourceHash(normalised(readAll(store.storage.pathOf(twinId)))).toStdString());
    }
    // A second import is a second note with the same text — no "update".
    const QString again = store.storage.importBook(QString(), sample, ImportLimits{}, &error);
    ZT_TRUE("повторный ввоз — новая заметка", !again.isEmpty() && again != id);
    if (!again.isEmpty())
        ZT_EQ("с тем же текстом", normalised(text).toStdString(),
              normalised(readAll(store.storage.pathOf(again))).toStdString());

    ZStorage::Report verify;
    store.storage.verify(verify);
    for (const QString& line : verify.lines)
        if (line.startsWith(QStringLiteral("PROBLEM"))) std::printf("  %s\n", line.toUtf8().constData());
    ZT_EQ("verify без бед", "0", n(verify.problems));

    // THE CARD IN THE LIST: a book says who and when where a note shows how
    // its text begins. The tree model reads the store anew (the lock file of
    // the store above goes first, as tree_test does).
    {
        zt::dropFile(store.dir.path(), store.dir.path() + QStringLiteral("/.zametti/store.lock"));
        zametti::NoteTreeModel model(store.dir.path());
        ZT_TRUE("модель дерева видит хранилище", model.isStore());
        const zametti::NoteRow row = model.rowOf(id);
        ZT_EQ("карточка книги — автор и год", "Иван Петрович Пробников, соавтор · 2001",
              row.snippet.toStdString());
        ZT_EQ("а заголовок — название", "Пробная книга: всё, что умеет fb2", row.title.toStdString());
    }
}

void checkCorpus() {
    const QString dir = zt::TestData::corpus(QStringLiteral("books"));
    if (dir.isEmpty()) {
        std::printf("ПРОПУСК: нет корпуса books (.testdata/books/*.fb2)\n");
        return;
    }
    QStringList files;
    QDirIterator it(dir, {QStringLiteral("*.fb2")}, QDir::Files);
    while (it.hasNext()) files.append(it.next());
    files.sort();
    if (files.isEmpty()) {
        std::printf("ПРОПУСК: корпус books пуст\n");
        return;
    }
    Store store;
    ZT_TRUE("временное хранилище", store.ok);
    if (!store.ok) return;

    std::printf("корпус books: %lld книг\n", (long long)files.size());
    for (const QString& file : files) {
        const std::string name = QFileInfo(file).fileName().toStdString();
        QString error;
        ZStorage::BookImport report;
        QElapsedTimer clock;
        clock.start();
        const QString id = store.storage.importBook(QString(), file, ImportLimits{}, &error, &report);
        const qint64 total = clock.elapsed();
        ZT_TRUE("ввезена " + name + ": " + error.toStdString(), !id.isEmpty());
        if (id.isEmpty()) continue;

        // The pairing invariant, on the source and on the result.
        QFile f(file);
        f.open(QIODevice::ReadOnly);
        Fb2Book book;
        QString why;
        ZT_TRUE("разобрана заново " + name, book.load(f.readAll(), &why));
        ZT_EQ("каждый a[type=note] — ссылка, " + name, n(countNoteAnchors(file)), n(book.stats().references));
        ZT_EQ("у каждой ссылки есть определение, " + name, "0", n(danglingReferences(book.pieces())));
        ZT_TRUE("картинки не терялись, " + name + ": " + std::to_string(report.imagesFailed),
                report.imagesFailed == 0);

        std::printf("  %-60.60s %5lld ms: parse %4lld, images %5lld (%3d, %6lld KB), write %4lld; "
                    "sections %3d, notes %3d, refs %3d, note %5lld KB\n",
                    name.c_str(), (long long)total, (long long)report.parseMs, (long long)report.imagesMs,
                    report.images, (long long)(report.attachmentBytes / 1024), (long long)report.writeMs,
                    report.sections, report.footnotes, report.references,
                    (long long)(report.noteBytes / 1024));
    }
    ZStorage::Report verify;
    store.storage.verify(verify);
    ZT_EQ("verify без бед на корпусе", "0", n(verify.problems));
}

}  // namespace

TEST(Fb2Import, Sample) {
    checkSample();
    EXPECT_EQ(0, zt::report("fb2-sample"));
}

TEST(Fb2Import, Corpus) {
    checkCorpus();
    EXPECT_EQ(0, zt::report("fb2-corpus"));
}
