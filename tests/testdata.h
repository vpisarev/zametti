// Where the test corpora live and where a suite writes its own output.
//
// Both used to arrive via argv: every binary got its own path, and CMakeLists
// looped over ZAMETTI_CORPUS adding five checks at a time. In a single process
// argv is shared by everyone, so the corpora have to be found differently.
//
// TWO TIERS OF CORPORA (05.09.2026, before opening the repository):
//   tests/testdata/  - public fixtures, committed: the CommonMark/GFM spec
//                      examples and synthetic notes. Nothing personal goes
//                      here (see the README there).
//   .testdata/       - the owner's private corpora (copies of real notes,
//                      images, screenshots); git-ignored, absent elsewhere.
// A relative path is looked up in the public tier first, then the private
// one: the owner's runs exercise exactly the copies that ship, so the public
// copy is the verified one. ZAMETTI_TESTDATA names WHERE THE PRIVATE TIER IS
// (default: .testdata); the public tier is always in the repository. Hence
// `ZAMETTI_TESTDATA=/nonexistent` is exactly a fresh clone.
//
// THE RULE ABOUT SKIPS. Private corpora are not committed: we have gigabytes
// of them (images alone are 1.6 GB), and another machine will not have them.
// A suite with nothing to check MUST SAY SO out loud and pass empty, not
// pretend to be green. I have been burned by this already: a silent skip is
// indistinguishable from a working check, and the "don't mute suites" rule
// rests first of all on skips being visible.

#pragma once

#include <gtest/gtest.h>

#include "zsystem.h"

#include <QDir>
#include <QFileInfo>
#include <QString>
#include <QStringList>

namespace zt {

class TestData {
public:
    // Corpus roots in lookup order: tests/testdata next to the sources
    // (public), then the private one - ZAMETTI_TESTDATA or .testdata next to
    // the sources. The source path is baked in by the build.
    static QStringList roots() {
        const QByteArray env = qgetenv("ZAMETTI_TESTDATA");
        const QString priv = env.isEmpty()
            ? QStringLiteral(ZAMETTI_SOURCE_DIR) + QStringLiteral("/.testdata")
            : QString::fromLocal8Bit(env);
        return {QStringLiteral(ZAMETTI_SOURCE_DIR) + QStringLiteral("/tests/testdata"), priv};
    }

    // The first existing root; if none exists, the private one (that is what
    // the skip message names). Suites should prefer corpus()/file(): those
    // search every tier, not a single one.
    static QString root() {
        const QStringList all = roots();
        for (const QString& r : all)
            if (QFileInfo::exists(r)) return r;
        return all.last();
    }

    // A corpus directory by name: corpus, commonmark, gfm, images/originals...
    // Searched across tiers; empty means the corpus exists nowhere.
    static QString corpus(const QString& name) { return find(name); }

    // A corpus file. Searched across tiers; empty means the file exists nowhere.
    static QString file(const QString& relative) { return find(relative); }

    // A suite's own directory for whatever it writes. Its OWN: eighty suites
    // share one process and would trample a shared directory. Wiped on
    // handout, so a suite always starts from an empty place.
    //
    // LIVES IN THE TEMP ZONE, NOT IN THE BUILD DIRECTORY (owner's decision,
    // 30.08.2026). The build directory sits in the home directory, and inside
    // /home and /Users the program never removes directories at all - the
    // suites live by the same rule as the program, with no exceptions for
    // themselves. A rule with an exception for suites protects nothing: it was
    // a PROBE that wiped the owner's directory.
    //
    // Where exactly - the suite prints it; the path is stable from run to run,
    // so acceptance screenshots are taken from the same place as before.
    static QString outDir(const QString& suite) {
        const QString path =
            QDir::tempPath() + QStringLiteral("/zametti-наборы/") + suite;
        zametti::ZSystem::removeScratchTree(path);
        QDir().mkpath(path);
        return path;
    }

private:
    static QString find(const QString& relative) {
        const QStringList all = roots();
        for (const QString& r : all) {
            const QString path = r + QLatin1Char('/') + relative;
            if (QFileInfo::exists(path)) return path;
        }
        return QString();
    }
};

}  // namespace zt

// Skip a suite, saying loudly why. The wrapper exists precisely so that a
// skip cannot be done silently with a bare GTEST_SKIP() and no explanation.
#define ZT_SKIP_NO_CORPUS(path, what)                                              \
    do {                                                                           \
        if ((path).isEmpty()) {                                                    \
            GTEST_SKIP() << "корпуса нет рядом: " << (what)                        \
                         << " (см. ZAMETTI_TESTDATA)";                             \
        }                                                                          \
    } while (false)
