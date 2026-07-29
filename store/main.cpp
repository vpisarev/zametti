// zametti-store: консольная утилита плоского хранилища.
//
//   zametti-store init <dir>
//   zametti-store new --root <dir> [--parent <id>]
//   zametti-store import --root <dir> --from <srcdir> [--apple-manifest <json>] [--dry-run]
//   zametti-store verify --root <dir>

#include "store.h"

#include <QCoreApplication>

#include <cstdio>

namespace {

void printLines(const zametti::store::Report& report) {
    for (const QString& line : report.lines)
        std::printf("%s\n", line.toUtf8().constData());
}

int usage() {
    std::fprintf(stderr,
                 "использование:\n"
                 "  zametti-store init <dir>\n"
                 "  zametti-store new --root <dir> [--parent <id>]\n"
                 "  zametti-store import --root <dir> --from <srcdir>"
                 " [--apple-manifest <json>] [--dry-run]\n"
                 "  zametti-store verify --root <dir>\n");
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();
    if (args.size() < 2) return usage();
    const QString command = args[1];

    // Разбор простых пар "--ключ значение"; порядок свободный.
    QString root;
    QString from;
    QString manifest;
    QString parent;
    QString positional;
    bool dryRun = false;
    for (qsizetype i = 2; i < args.size(); ++i) {
        const QString& a = args[i];
        const auto next = [&]() -> QString {
            return i + 1 < args.size() ? args[++i] : QString();
        };
        if (a == QStringLiteral("--root")) root = next();
        else if (a == QStringLiteral("--from")) from = next();
        else if (a == QStringLiteral("--apple-manifest")) manifest = next();
        else if (a == QStringLiteral("--parent")) parent = next();
        else if (a == QStringLiteral("--dry-run")) dryRun = true;
        else if (!a.startsWith(QStringLiteral("--")) && positional.isEmpty()) positional = a;
        else return usage();
    }

    if (command == QStringLiteral("init")) {
        if (positional.isEmpty()) return usage();
        QString error;
        if (!zametti::store::initStore(positional, &error)) {
            std::fprintf(stderr, "%s\n", error.toUtf8().constData());
            return 1;
        }
        return 0;
    }

    if (command == QStringLiteral("new")) {
        if (root.isEmpty()) return usage();
        QString error;
        const QString path = zametti::store::newNote(root, parent, &error);
        if (path.isEmpty()) {
            std::fprintf(stderr, "%s\n", error.toUtf8().constData());
            return 1;
        }
        std::printf("%s\n", path.toUtf8().constData());
        return 0;
    }

    if (command == QStringLiteral("import")) {
        if (root.isEmpty() || from.isEmpty()) return usage();
        zametti::store::ImportOptions options;
        options.root = root;
        options.from = from;
        options.appleManifest = manifest;
        options.dryRun = dryRun;
        zametti::store::Report report;
        const bool ok = zametti::store::importTree(options, report);
        printLines(report);
        return ok ? 0 : 1;
    }

    if (command == QStringLiteral("verify")) {
        if (root.isEmpty()) return usage();
        zametti::store::Report report;
        const bool ok = zametti::store::verifyStore(root, report);
        printLines(report);
        return ok ? 0 : 1;
    }

    return usage();
}
