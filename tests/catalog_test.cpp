// The catalogue reads a bounded prefix of a note (ZNote::Metadata::fromFile),
// never the whole file: a store of multi-megabyte books opens as fast as a
// store of short notes. The suite pins three things: the light path answers
// what the full build answers (over every fixture and corpus we have); a cut
// anywhere inside the file either gives the same answer or says "not
// complete"; and the bytes read are bounded.

#include "document.h"
#include "hash.h"
#include "test_util.h"
#include "testdata.h"
#include "znote.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include <cstdio>
#include <string>
#include <vector>

using zametti::ZDocument;
using zametti::ZNote;

namespace {

std::string s(const QString& text) { return text.toStdString(); }
std::string n(qint64 value) { return std::to_string(value); }

// The full path: the note is built whole, as the editor builds it.
ZNote::Metadata fullMetadata(const QString& path, const QByteArray& bytes) {
    ZNote note(path, bytes, zametti::Digest{}, nullptr);
    note.load(std::string_view(bytes.constData(), size_t(bytes.size())));
    ZNote::Metadata m = note.metadata();
    return m;
}

// Field by field; the differing field is named in the failure.
int compareMetadata(const std::string& what, const ZNote::Metadata& full, const ZNote::Metadata& light) {
    int failures = 0;
    const auto eq = [&](const char* field, const std::string& a, const std::string& b) {
        if (a == b) return;
        ++failures;
        std::fprintf(stderr, "%s: %s differs\n  full : %s\n  light: %s\n", what.c_str(), field,
                     a.c_str(), b.c_str());
    };
    eq("id", s(full.id()), s(light.id()));
    eq("parent", s(full.parent()), s(light.parent()));
    eq("title", s(full.title()), s(light.title()));
    eq("snippet", s(full.snippet()), s(light.snippet()));
    eq("archived", n(full.archived()), n(light.archived()));
    eq("readOnly", n(full.readOnly()), n(light.readOnly()));
    eq("locked", n(full.locked()), n(light.locked()));
    eq("book", n(full.book()), n(light.book()));
    eq("author", s(full.author()), s(light.author()));
    eq("year", s(full.year()), s(light.year()));
    eq("cover", s(full.cover()), s(light.cover()));
    eq("folder", n(full.folder()), n(light.folder()));
    eq("lostFound", n(full.lostFound()), n(light.lostFound()));
    eq("root", n(full.root()), n(light.root()));
    eq("role", s(full.role()), s(light.role()));
    eq("sortMark", n(full.sortMark().has_value()), n(light.sortMark().has_value()));
    // Times: the light path falls back to the file's mtime when the header
    // has none; the full path does not — compare only what the header says.
    if (!full.modified().isEmpty()) eq("modified", s(full.modified()), s(light.modified()));
    if (!full.created().isEmpty()) eq("created", s(full.created()), s(light.created()));
    return failures;
}

// Every .md under the public fixtures and, when present, the private corpora.
QStringList corpusFiles() {
    QStringList files;
    const QStringList roots = zt::TestData::roots();
    QStringList dirs{roots.first()};
    const QString priv = roots.last();
    for (const char* name : {"corpus", "books-store"}) {
        const QString dir = priv + QLatin1Char('/') + QLatin1String(name);
        if (QFileInfo::exists(dir)) dirs << dir;
        else std::fprintf(stderr, "catalog: private corpus %s is absent — skipped aloud\n", name);
    }
    for (const QString& dir : dirs) {
        QDirIterator it(dir, {QStringLiteral("*.md")}, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) files << it.next();
    }
    files.sort();
    return files;
}

void checkLightEqualsFull() {
    const QStringList files = corpusFiles();
    ZT_TRUE("there are fixtures to compare", files.size() > 100);
    int failures = 0;
    qint64 maxRead = 0;
    int grown = 0;
    for (const QString& path : files) {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) continue;
        const QByteArray bytes = f.readAll();
        qint64 read = 0;
        const ZNote::Metadata light = ZNote::Metadata::fromFile(path, &read);
        if (!light.valid()) {
            std::fprintf(stderr, "catalog: %s not readable by the light path\n",
                         path.toUtf8().constData());
            ++failures;
            continue;
        }
        failures += compareMetadata(s(QFileInfo(path).fileName()), fullMetadata(path, bytes), light);
        maxRead = qMax(maxRead, read);
        if (read > ZNote::Metadata::kPrefixBytes) ++grown;
    }
    std::fprintf(stderr, "catalog: %lld files, max read %lld bytes, %d needed more than one read\n",
                 (long long)files.size(), (long long)maxRead, grown);
    ZT_EQ("light metadata equals the full build on every fixture", "0", n(failures));
}

// --- the cut matrix -----------------------------------------------------------

struct Fixture {
    const char* name;
    std::string text;
    // The byte offset from which a cut gives the same answer as the whole
    // (0 = from the header's end): a reference-style link needs its definition.
    size_t agreeFrom = 0;
};

const char* kHeader =
    "<!-- zametti\ncreated: 2026-01-01T00:00:00Z\nmodified: 2026-01-02T00:00:00Z\n-->\n\n";

std::vector<Fixture> fixtures() {
    std::vector<Fixture> out;
    const std::string h = kHeader;
    // A tail long enough for the snippet to fill up (200 characters) well
    // before the end: only then can a cut be complete.
    std::string tail;
    for (int i = 0; i < 8; ++i)
        tail += "Хвост номер " + std::to_string(i) + ", слова про то и про сё, и про погоду за окном, "
                "и про кота на подоконнике.\n\n";
    out.push_back({"plain", h + "# Заголовок\n\nПервый абзац с текстом, который тянется и тянется, "
                                "чтобы сниппет набрал свои двести знаков не сразу.\n\n" + tail});
    out.push_back({"code first", h + "```cpp\nint main() { return 0; }\nint x = 1;\n```\n\n# После кода\n\n" +
                                     tail});
    out.push_back({"table first", h + "| a | b |\n|---|---|\n| 1 | 2 |\n| 3 | 4 |\n\n# Таблица была\n\n" +
                                      tail});
    out.push_back({"comment first", h + "<!-- набросок, не показывать -->\n\n# Настоящий заголовок\n\n" +
                                        tail});
    out.push_back({"formula", h + "# Формула\n\n$$\nE = mc^2\n\nx = y\n$$\n\n" + tail});
    out.push_back({"setext", h + "Заголовок подчёркнутый\n======================\n\n" + tail});
    out.push_back({"list", h + "# Список\n\n- первый пункт\n  с продолжением на второй строке\n- второй "
                               "пункт\n\n  и абзац внутри него\n- третий\n\n" + tail});
    out.push_back({"table inside", h + "# Таблица в теле\n\nАбзац.\n\n| a | b |\n|---|---|\n| 1 | 2 |\n"
                                       "| 3 | 4 |\n| 5 | 6 |\n\n" + tail});
    out.push_back({"fence inside", h + "# Код в теле\n\nАбзац.\n\n```\nодна строка\nдве строки\nтри "
                                       "строки\n```\n\n" + tail});
    // A reference-style link: with the definition beyond the cut the text
    // renders literally — the documented deviation. Cuts past the definition
    // line agree with the whole.
    Fixture ref{"reference link", h + "# Ссылка\n\nСмотри [описание][doc] и дальше.\n\n" + tail +
                                      "[doc]: https://example.org/doc\n\n" + tail};
    ref.agreeFrom = ref.text.find("[doc]: ");
    ref.agreeFrom = ref.text.find('\n', ref.agreeFrom) + 1;
    out.push_back(std::move(ref));
    return out;
}

bool utf8Boundary(const std::string& text, size_t at) {
    return at >= text.size() || (uchar(text[at]) & 0xC0) != 0x80;
}

void checkCutMatrix() {
    for (const Fixture& fx : fixtures()) {
        const std::string_view all(fx.text);
        const ZDocument::Summary whole = ZDocument::summarise(all, false, 200);
        ZT_TRUE(std::string(fx.name) + ": the whole has a title", !whole.title.isEmpty());
        // The whole through summarise() equals the whole through the document.
        QTemporaryDir home;
        const QString path = home.path() + QStringLiteral("/01aaaaaaaaaa01.md");
        {
            QFile f(path);
            ZT_TRUE("fixture written", f.open(QIODevice::WriteOnly) &&
                                           f.write(fx.text.data(), qint64(fx.text.size())) >= 0);
        }
        const ZNote::Metadata full = fullMetadata(path, QByteArray::fromStdString(fx.text));
        ZT_EQ(std::string(fx.name) + ": title, summarise vs document", s(full.title()), s(whole.title));
        ZT_EQ(std::string(fx.name) + ": snippet, summarise vs document", s(full.snippet()), s(whole.snippet));

        const size_t headerEnd = fx.text.find("-->\n") + 4;
        int completeCuts = 0;
        int wrongWhenComplete = 0;
        int deviations = 0;
        for (size_t cut = headerEnd; cut < fx.text.size(); ++cut) {
            if (!utf8Boundary(fx.text, cut)) continue;
            const ZDocument::Summary part = ZDocument::summarise(all.substr(0, cut), true, 200);
            if (!part.complete) continue;
            ++completeCuts;
            const bool same = part.title == whole.title && part.snippet == whole.snippet;
            if (same) continue;
            if (fx.agreeFrom != 0 && cut < fx.agreeFrom) {
                ++deviations;   // the documented one: the definition is beyond the cut
                continue;
            }
            if (++wrongWhenComplete == 1)
                std::fprintf(stderr, "%s: cut at %zu is complete but differs\n  whole: %s | %s\n  part : %s | %s\n",
                             fx.name, cut, whole.title.toUtf8().constData(),
                             whole.snippet.toUtf8().constData(), part.title.toUtf8().constData(),
                             part.snippet.toUtf8().constData());
        }
        ZT_TRUE(std::string(fx.name) + ": some cuts are complete", completeCuts > 0);
        ZT_EQ(std::string(fx.name) + ": a complete cut never differs from the whole", "0",
              n(wrongWhenComplete));
        if (fx.agreeFrom != 0)
            ZT_TRUE(std::string(fx.name) + ": the deviation before the definition is real",
                    deviations > 0);
    }
}

// --- bounded reads ------------------------------------------------------------

std::string paragraph(int i) {
    return "Абзац номер " + std::to_string(i) +
           ": слова, слова, слова, слова, слова, слова, слова, слова, слова, слова.\n\n";
}

void checkBoundedRead() {
    QTemporaryDir home;
    const QString path = home.path() + QStringLiteral("/01aaaaaaaaaa02.md");
    std::string text = kHeader;
    text += "# Большая заметка\n\n";
    for (int i = 0; text.size() < 5 * 1024 * 1024; ++i) text += paragraph(i);
    {
        QFile f(path);
        ZT_TRUE("5 MB note written", f.open(QIODevice::WriteOnly) &&
                                         f.write(text.data(), qint64(text.size())) >= 0);
    }
    qint64 read = 0;
    const ZNote::Metadata light = ZNote::Metadata::fromFile(path, &read);
    ZT_TRUE("read at most the first prefix: " + n(read), read <= ZNote::Metadata::kPrefixBytes);
    ZT_EQ("title of the big note", "Большая заметка", s(light.title()));
    ZT_EQ("and the same as the full build", "0",
          n(compareMetadata("big", fullMetadata(path, QByteArray::fromStdString(text)), light)));
}

void checkGrowsWhenNeeded() {
    QTemporaryDir home;
    const QString path = home.path() + QStringLiteral("/01aaaaaaaaaa03.md");
    std::string text = kHeader;
    // A closed comment of 100 KB before the title: the first prefix holds no
    // meaningful block, and the read must grow until the title is found.
    text += "<!--\n";
    while (text.size() < 100 * 1024) text += "черновик черновик черновик черновик черновик\n";
    text += "-->\n\n# После длинного комментария\n\nТекст, слова, слова, слова.\n";
    {
        QFile f(path);
        ZT_TRUE("note with a long comment written",
                f.open(QIODevice::WriteOnly) && f.write(text.data(), qint64(text.size())) >= 0);
    }
    qint64 read = 0;
    const ZNote::Metadata light = ZNote::Metadata::fromFile(path, &read);
    ZT_TRUE("read more than the first prefix: " + n(read), read > ZNote::Metadata::kPrefixBytes);
    ZT_EQ("the title is behind the comment", "После длинного комментария", s(light.title()));
    ZT_EQ("and the same as the full build", "0",
          n(compareMetadata("comment", fullMetadata(path, QByteArray::fromStdString(text)), light)));

    // One giant line without a break: the cut backs off to a UTF-8 boundary
    // and the read grows to the whole file.
    const QString line = home.path() + QStringLiteral("/01aaaaaaaaaa04.md");
    std::string one = kHeader;
    one += "# Одна строка\n\n";
    while (one.size() < 20 * 1024) one += "ёжик ";
    one += "\n";
    {
        QFile f(line);
        ZT_TRUE("one-line note written",
                f.open(QIODevice::WriteOnly) && f.write(one.data(), qint64(one.size())) >= 0);
    }
    const ZNote::Metadata oneLight = ZNote::Metadata::fromFile(line, &read);
    ZT_EQ("one-line note equals the full build", "0",
          n(compareMetadata("one line", fullMetadata(line, QByteArray::fromStdString(one)), oneLight)));
}

int ztRunSuite() {
    checkLightEqualsFull();
    checkCutMatrix();
    checkBoundedRead();
    checkGrowsWhenNeeded();
    return zt::report("catalogue: light metadata");
}

}  // namespace

TEST(Catalog, All) { EXPECT_EQ(0, ztRunSuite()); }
