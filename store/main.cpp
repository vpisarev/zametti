// zametti-store: консольная утилита плоского хранилища.
//
//   zametti-store init <dir>
//   zametti-store new --root <dir> [--parent <id>]
//   zametti-store import --root <dir> --from <srcdir> [--apple-manifest <json>] [--dry-run]
//   zametti-store verify --root <dir>
//   zametti-store thin --root <dir> [--dry-run]
//   zametti-store history compress <id | путь к .md> [--root <dir>]
//   zametti-store recompress --root <dir> --id <id|all> [--max-size N]

#include "history_rules.h"
#include "journal.h"
#include "store.h"
#ifdef ZAMETTI_HAVE_IMAGEIO
#include "recompress.h"
#endif

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
                 "  zametti-store thin --root <dir> [--dry-run]\n"
                 "  zametti-store history compress <id | путь к .md> [--root <dir>]\n"
                 "  zametti-store recompress --root <dir> --id <id|all>\n"
                 "\n"
                 "  У recompress НЕТ умолчания для --id: пережатие необратимо, и\n"
                 "  переехать всё хранилище одной забытой опцией быть не должно.\n"
                 "  Все картинки — только явным «--id all».\n");
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
    QString positional2;
    QString id;
    QString maxSize, maxFileMb, quality;
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
        else if (a == QStringLiteral("--id")) id = next();
        else if (a == QStringLiteral("--max-size")) maxSize = next();
        else if (a == QStringLiteral("--quality")) quality = next();
        else if (a == QStringLiteral("--dry-run")) dryRun = true;
        else if (!a.startsWith(QStringLiteral("--")) && positional.isEmpty()) positional = a;
        else if (!a.startsWith(QStringLiteral("--")) && positional2.isEmpty()) positional2 = a;
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
            // Различаем два разных отказа: замок держат — и замок не завести
            // вовсе. Второе случается на каталоге, который хранилищем не
            // является, и списывать это на занятость было бы враньём.
            if (lock.error() == QLockFile::LockFailedError)
                std::fprintf(stderr,
                             "хранилище занято: похоже, открыта программа. "
                             "Прореживание идёт фоном при её запуске.\n");
            else
                std::fprintf(stderr, "замок хранилища не завести: %s\n",
                             zametti::journal::storeLockPath(root).toUtf8().constData());
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

    // ТЕСТОВЫЙ ЛЮК. Существует ровно для того, чтобы гонять миграцию без UI:
    // форсирует ТУ ЖЕ функцию, что зовут автосохранение и вход в историю, а не
    // параллельную реализацию «как бы того же самого».
    //
    // Штатного пути чистить историю руками у человека нет и не будет: чистка
    // ленивая и пер-заметочная (решение владельца).
    if (command == QStringLiteral("history")) {
        if (positional != QStringLiteral("compress") || positional2.isEmpty()) return usage();
        // Цель — id или путь к файлу заметки. По пути хранилище видно само;
        // голому id нужен --root.
        QString noteId = positional2;
        QString target = root;
        if (positional2.endsWith(QStringLiteral(".md"))) {
            const QFileInfo info(positional2);
            noteId = info.completeBaseName();
            if (target.isEmpty()) target = info.absolutePath();
        }
        if (target.isEmpty()) {
            std::fprintf(stderr, "не сказано, какое хранилище: нужен --root или путь к .md\n");
            return 1;
        }

        // Тот же межпроцессный замок, что у thin: пока открыта программа,
        // журналы правит она.
        QLockFile lock(zametti::journal::storeLockPath(target));
        if (!lock.tryLock(0)) {
            // Занято и «замок негде завести» — разные беды, и валить вторую на
            // первую значит врать: чаще всего это просто не хранилище.
            if (lock.error() == QLockFile::LockFailedError)
                std::fprintf(stderr, "хранилище занято: похоже, открыта программа\n");
            else
                std::fprintf(stderr, "замок хранилища не завести: %s\n",
                             zametti::journal::storeLockPath(target).toUtf8().constData());
            return 1;
        }

        zametti::journal::History history(target);
        zametti::history::Report report;
        QString error;
        // force: люк на то и люк, чтобы прогонять чистку и по уже чищеному
        // журналу — так проверяется идемпотентность.
        if (!zametti::history::compressJournal(history, noteId, {}, true, &report, &error)) {
            std::fprintf(stderr, "%s\n", error.toUtf8().constData());
            return 1;
        }
        const auto name = [](const QString& v) {
            return v.isEmpty() ? QStringLiteral("0 (не чищен)") : v;
        };
        std::printf("%s: версия %s -> %s\n", noteId.toUtf8().constData(),
                    name(report.versionBefore).toUtf8().constData(),
                    name(report.versionAfter).toUtf8().constData());
        std::printf("записей %d -> %d (дубликатов %d, схлопнуто %d)%s\n", report.recordsBefore,
                    report.recordsAfter, report.duplicates, report.merged,
                    report.rewritten ? "" : "; файл не тронут");
        return 0;
    }

#ifdef ZAMETTI_HAVE_IMAGEIO
    // Пережатие вложений. Числа берутся из ключей, а не из конфига программы:
    // утилита должна уметь то, чего в конфиге нет, — например прогнать с другим
    // качеством и сравнить глазами.
    if (command == QStringLiteral("recompress")) {
        if (root.isEmpty()) return usage();
        zametti::RecompressOptions options;
        options.root = root;
        options.id = id;
        options.dryRun = dryRun;
        bool bad = false;
        if (!maxSize.isEmpty()) options.limits.maxSize = maxSize.toInt(&bad), bad = !bad;
        if (!bad && !maxFileMb.isEmpty())
        if (!bad && !quality.isEmpty()) options.limits.quality = quality.toInt(&bad), bad = !bad;
        if (bad) {
            std::fprintf(stderr, "непонятное число в ключах\n");
            return 1;
        }

        zametti::RecompressReport report;
        const bool ok = zametti::recompressStore(options, report);
        for (const QString& line : report.lines)
            std::printf("%s\n", line.toUtf8().constData());
        for (const QString& p : report.problems)
            std::fprintf(stderr, "%s\n", p.toUtf8().constData());
        if (report.examined > 0) {
            std::printf("\nосмотрено %d, переписано %d, оставлено %d, не вышло %d\n",
                        report.examined, report.rewritten, report.untouched, report.failed);
            std::printf("было %.1f МБ, стало %.1f МБ\n",
                        double(report.bytesBefore) / 1048576.0,
                        double(report.bytesAfter) / 1048576.0);
            if (dryRun) std::printf("(это была примерка, ничего не записано)\n");
        }
        return ok ? 0 : 1;
    }
#endif

    if (command == QStringLiteral("verify")) {
        if (root.isEmpty()) return usage();
        zametti::store::Report report;
        const bool ok = zametti::store::verifyStore(root, report);
        printLines(report);
        return ok ? 0 : 1;
    }

    return usage();
}
