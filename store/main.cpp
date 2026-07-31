// zametti-store: консольная утилита плоского хранилища.
//
//   zametti-store init <dir>
//   zametti-store new --root <dir> [--parent <id>]
//   zametti-store import --root <dir> --from <srcdir> [--apple-manifest <json>] [--dry-run]
//   zametti-store verify --root <dir>
//   zametti-store thin --root <dir> [--dry-run]

#include "journal.h"
#include "store.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QFileInfo>
#include <QDir>

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
                 "  zametti-store verify --root <dir>\n"
                 "  zametti-store thin --root <dir> [--dry-run]\n");
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

    // Прореживание журналов. Отдельной командой, а не только фоном при старте:
    // владелец должен уметь прогнать его руками и увидеть, что именно уйдёт.
    if (command == QStringLiteral("thin")) {
        if (root.isEmpty()) return usage();
        const QDir history(QDir(root).filePath(QStringLiteral("history")));
        if (!history.exists()) {
            std::fprintf(stderr, "в хранилище нет каталога history/\n");
            return 1;
        }
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        int problems = 0;
        qint64 wasBytes = 0, nowBytes = 0, wasRecords = 0, nowRecords = 0;
        const QStringList names = history.entryList({QStringLiteral("*.log")}, QDir::Files);
        for (const QString& name : names) {
            const QString path = history.filePath(name);
            zametti::journal::Journal before;
            QString error;
            if (!zametti::journal::read(path, &before, &error)) {
                std::fprintf(stderr, "БЕДА: %s: %s\n", name.toUtf8().constData(),
                             error.toUtf8().constData());
                ++problems;
                continue;
            }
            const qint64 size = QFileInfo(path).size();
            const qsizetype keep = zametti::journal::survivors(before.entries, now).size();
            wasBytes += size;
            wasRecords += before.entries.size();
            if (before.tailTrimmed)
                std::printf("%s: оборванный хвост будет отрезан\n", name.toUtf8().constData());
            if (dryRun) {
                nowRecords += keep;
                std::printf("%s: %lld записей -> %lld\n", name.toUtf8().constData(),
                            (long long)before.entries.size(), (long long)keep);
                continue;
            }
            if (!zametti::journal::thin(path, now, &error)) {
                std::fprintf(stderr, "БЕДА: %s: %s\n", name.toUtf8().constData(),
                             error.toUtf8().constData());
                ++problems;
                continue;
            }
            zametti::journal::Journal after;
            zametti::journal::read(path, &after, &error);
            nowBytes += QFileInfo(path).size();
            nowRecords += after.entries.size();
        }
        std::printf("журналов %lld, записей %lld -> %lld", (long long)names.size(),
                    (long long)wasRecords, (long long)nowRecords);
        if (!dryRun)
            std::printf(", байт %lld -> %lld", (long long)wasBytes, (long long)nowBytes);
        std::printf("%s\n", dryRun ? " (только показ)" : "");
        return problems == 0 ? 0 : 1;
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
