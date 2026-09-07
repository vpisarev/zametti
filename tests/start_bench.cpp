// The cost of a start, measured (CLAUDE.md: O(1) — a key press, a frame and a
// START must not grow with the size of the store).
//
//   zametti-bench start <root>
//       the phases the window pays before it shows: the catalogue scan
//       (reload), the migrations, the tree; then, per note, the light path
//       (ZNote::Metadata::fromFile) against the full build, the ten slowest
//       files, the bytes read and how many notes needed a second read.
//   zametti-bench start --synth <dir> <small> <large>
//       makes a store of <small> short notes and <large> two-megabyte ones in
//       <dir> (left there; the caller's directory) and times the scan of
//       three stores: the small ones alone, all of them, a tenth of the small
//       with all the large — the time must follow the count, not the bytes.

#include "note_id.h"
#include "note_tree.h"
#include "znote.h"
#include "zstorage.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

using zametti::ZNote;
using zametti::ZStorage;

namespace {

qint64 us(const QElapsedTimer& t) { return t.nsecsElapsed() / 1000; }

int benchStore(const QString& root) {
    QElapsedTimer t;
    t.start();
    auto storage = std::make_shared<ZStorage>(root);
    const qint64 ctor = us(t);
    t.restart();
    storage->reload();
    const qint64 reload = us(t);
    t.restart();
    const QStringList said = storage->migrate();
    const qint64 migrate = us(t);
    t.restart();
    zametti::NoteTreeModel model(storage);
    const qint64 tree = us(t);
    std::printf("store %s: %lld notes\n", root.toUtf8().constData(), (long long)storage->ids().size());
    std::printf("  ctor %lld us, reload %lld us, migrate %lld us (%lld notes), tree %lld us\n",
                (long long)ctor, (long long)reload, (long long)migrate, (long long)said.size(),
                (long long)tree);

    struct Row {
        QString name;
        qint64 size = 0;
        qint64 light = 0;
        qint64 full = 0;
        qint64 read = 0;
    };
    std::vector<Row> rows;
    qint64 bytesRead = 0;
    qint64 bytesTotal = 0;
    int grown = 0;
    int mismatches = 0;
    for (const QString& id : storage->ids()) {
        const QString path = storage->pathOf(id);
        Row row;
        row.name = QFileInfo(path).fileName();
        row.size = QFileInfo(path).size();
        t.restart();
        const ZNote::Metadata light = ZNote::Metadata::fromFile(path, &row.read);
        row.light = us(t);
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) continue;
        const QByteArray bytes = f.readAll();
        t.restart();
        ZNote note(path, bytes, zametti::Digest{}, nullptr);
        note.load(std::string_view(bytes.constData(), size_t(bytes.size())));
        const ZNote::Metadata full = note.metadata();
        row.full = us(t);
        if (full.title() != light.title() || full.snippet() != light.snippet()) {
            ++mismatches;
            std::printf("  MISMATCH %s\n    full : %s | %s\n    light: %s | %s\n",
                        row.name.toUtf8().constData(), full.title().toUtf8().constData(),
                        full.snippet().toUtf8().constData(), light.title().toUtf8().constData(),
                        light.snippet().toUtf8().constData());
        }
        bytesRead += row.read;
        bytesTotal += row.size;
        if (row.read > ZNote::Metadata::kPrefixBytes) ++grown;
        rows.push_back(row);
    }
    qint64 lightSum = 0;
    qint64 fullSum = 0;
    for (const Row& r : rows) {
        lightSum += r.light;
        fullSum += r.full;
    }
    std::printf("  per note: light %lld us total, full %lld us total; bytes read %lld of %lld; "
                "%d of %zu notes needed more than %lld bytes; %d mismatches\n",
                (long long)lightSum, (long long)fullSum, (long long)bytesRead, (long long)bytesTotal,
                grown, rows.size(), (long long)ZNote::Metadata::kPrefixBytes, mismatches);
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.full > b.full; });
    std::printf("  slowest by the full build (file, bytes, full us, light us, read bytes):\n");
    for (size_t i = 0; i < rows.size() && i < 10; ++i)
        std::printf("    %s %lld %lld %lld %lld\n", rows[i].name.toUtf8().constData(),
                    (long long)rows[i].size, (long long)rows[i].full, (long long)rows[i].light,
                    (long long)rows[i].read);
    return 0;
}

bool writeNote(const QString& dir, int index, qint64 bodyBytes, bool book) {
    const QString id = QString::fromStdString(zametti::newNoteId());
    QFile f(dir + QLatin1Char('/') + id + QStringLiteral(".md"));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    QByteArray text("<!-- zametti\ncreated: 2026-01-01T00:00:00Z\nmodified: 2026-01-02T00:00:00Z\n");
    if (book) text += "role: book\nauthor: Author\nyear: 2026\n";
    text += "-->\n\n# Note " + QByteArray::number(index) + "\n\n";
    const QByteArray paragraph =
        "Слова, слова, слова, слова, слова, слова, слова, слова, слова, слова, слова.\n\n";
    while (text.size() < bodyBytes) text += paragraph;
    return f.write(text) == text.size();
}

qint64 timeReload(const QString& dir) {
    QElapsedTimer t;
    t.start();
    ZStorage s(dir);
    s.reload();
    return us(t);
}

int benchSynthetic(const QString& dir, int small, int large) {
    const QString a = dir + QStringLiteral("/small-only");
    const QString b = dir + QStringLiteral("/small-and-large");
    const QString c = dir + QStringLiteral("/tenth-and-large");
    QString error;
    for (const QString& root : {a, b, c}) {
        QDir().mkpath(root);
        if (!ZStorage(root).init(&error)) {
            std::fprintf(stderr, "cannot init %s: %s\n", root.toUtf8().constData(),
                         error.toUtf8().constData());
            return 1;
        }
    }
    for (int i = 0; i < small; ++i) {
        writeNote(a, i, 1024, false);
        writeNote(b, i, 1024, false);
        if (i % 10 == 0) writeNote(c, i, 1024, false);
    }
    for (int i = 0; i < large; ++i) {
        writeNote(b, small + i, 2 * 1024 * 1024, true);
        writeNote(c, small + i, 2 * 1024 * 1024, true);
    }
    std::printf("reload, three runs each (us):\n");
    for (const auto& [name, root] : {std::pair{"small only", a}, std::pair{"small + large", b},
                                     std::pair{"tenth + large", c}}) {
        std::printf("  %-14s", name);
        for (int run = 0; run < 3; ++run) std::printf(" %8lld", (long long)timeReload(root));
        ZStorage counted(root);
        counted.reload();
        std::printf("   (%lld notes)\n", (long long)counted.ids().size());
    }
    std::printf("the stores are left in %s\n", dir.toUtf8().constData());
    return 0;
}

}  // namespace

int ztStartBench(int argc, char** argv) {

    if (argc >= 5 && std::string(argv[1]) == "--synth")
        return benchSynthetic(QString::fromLocal8Bit(argv[2]), std::atoi(argv[3]), std::atoi(argv[4]));
    if (argc >= 2) return benchStore(QString::fromLocal8Bit(argv[1]));
    std::printf("zametti-bench start <root> | start --synth <dir> <small> <large>\n");
    return 2;
}
