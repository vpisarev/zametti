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
#include <QLockFile>
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
        // Межпроцессный замок: пока открыта программа, прореживание руками не
        // запускается. Внутрипроцессный замок журнала от чужого процесса не
        // бережёт, а ставить файловый на каждую запись — 4.4 мс на ровном месте.
        QLockFile lock(zametti::journal::storeLockPath(root));
        if (!lock.tryLock(0)) {
            std::fprintf(stderr,
                         "хранилище занято: похоже, открыта программа. "
                         "Прореживание идёт фоном при её запуске.\n");
            return 1;
        }
        zametti::journal::History history(root);
        const zametti::journal::ThinReport report =
            history.thinAll(QDateTime::currentMSecsSinceEpoch(), dryRun);
        for (const QString& name : report.trimmed)
            std::printf("%s: оборванный хвост отрезан\n", name.toUtf8().constData());
        for (const QString& line : report.problems)
            std::fprintf(stderr, "БЕДА: %s\n", line.toUtf8().constData());
        std::printf("журналов %d, записей %lld -> %lld", report.journals,
                    (long long)report.recordsBefore, (long long)report.recordsAfter);
        if (!dryRun)
            std::printf(", байт %lld -> %lld", (long long)report.bytesBefore,
                        (long long)report.bytesAfter);
        std::printf("%s\n", dryRun ? " (только показ)" : "");
        return report.problems.isEmpty() ? 0 : 1;
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
