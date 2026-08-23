// zametti-store: консольная утилита плоского хранилища.
//
//   zametti-store init <dir>
//   zametti-store new --root <dir> [--parent <id>]
//   zametti-store import --root <dir> --from <srcdir> [--apple-manifest <json>] [--dry-run]
//   zametti-store verify --root <dir>
//   zametti-store thin --root <dir> [--dry-run]
//   zametti-store history compress <id | путь к .md> [--root <dir>]
//   zametti-store recompress --root <dir> --id <id|all> [--max-size N]
//   zametti-store resurrect --root <dir> --id <id>
//   zametti-store archive --root <dir> --id <id> [--restore]

#include "journal.h"
#include "zstorage.h"
#include "recompress.h"

#include <QGuiApplication>
#include <QDateTime>
#include <QLockFile>
#include <QFileInfo>
#include <QDir>

#include <cstdio>

namespace {

void printLines(const zametti::ZStorage::Report& report) {
    for (const QString& line : report.lines)
        std::printf("%s\n", line.toUtf8().constData());
}

int usage() {
    std::fprintf(stderr,
                 "usage:\n"
                 "  zametti-store init <dir>\n"
                 "  zametti-store new --root <dir> [--parent <id>]\n"
                 "  zametti-store import --root <dir> --from <srcdir>"
                 " [--apple-manifest <json>] [--dry-run]\n"
                 "  zametti-store verify --root <dir>\n"
                 "  zametti-store thin --root <dir> [--dry-run]\n"
                 "  zametti-store history compress <id | path to .md> [--root <dir>]\n"
                 "  zametti-store recompress --root <dir> --id <id|all>\n"
                 "  zametti-store resurrect --root <dir> --id <id>\n"
                 "  zametti-store archive --root <dir> --id <id> [--restore]\n"
                 "  zametti-store root show|init|fix --root <dir>\n"
                 "\n"
                 "  recompress has NO default for --id: recompression is irreversible,\n"
                 "  and one forgotten option must not migrate the whole store.\n"
                 "  All images only with an explicit '--id all'.\n");
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    // ПЛАТФОРМА — ДО СОЗДАНИЯ ПРИЛОЖЕНИЯ. Ядро зависит от QtGui (там живёт
    // QTextDocument), а QGuiApplication без платформенного плагина не
    // стартует вовсе. offscreen даёт его там, где нет ни X-сервера, ни
    // wayland: в контейнере, в эмуляторе, на сборочной машине.
    //
    // Под условием, и это не педантизм: заданную снаружи платформу перебивать
    // нельзя, иначе приёмочные снимки под Xvfb молча уехали бы в offscreen.
    // Спрашиваем «пуста ли», а не «задана ли»: qEnvironmentVariableIsSet
    // считает установленной и ПУСТУЮ переменную, а пустая платформа для Qt не
    // платформа — он уходит в автоопределение и без дисплея падает.
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
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
    bool restore = false;
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
        else if (a == QStringLiteral("--restore")) restore = true;
        else if (!a.startsWith(QStringLiteral("--")) && positional.isEmpty()) positional = a;
        else if (!a.startsWith(QStringLiteral("--")) && positional2.isEmpty()) positional2 = a;
        else return usage();
    }

    if (command == QStringLiteral("init")) {
        if (positional.isEmpty()) return usage();
        QString error;
        if (!zametti::ZStorage(positional).init(&error)) {
            std::fprintf(stderr, "%s\n", error.toUtf8().constData());
            return 1;
        }
        return 0;
    }

    // КОРНЕВАЯ ЗАМЕТКА: посмотреть, завести, вылечить расхождение. Он же
    // тестовый люк: адрес в zametti.json и роль в шапке — две независимые
    // записи одного факта, и разъехаться они могут (файл приехал с другой
    // машины, шапку правили руками).
    if (command == QStringLiteral("root")) {
        if (root.isEmpty() || positional.isEmpty()) return usage();
        zametti::ZStorage storage(root);
        if (!storage.isStore()) {
            std::fprintf(stderr, "not a store: %s\n", root.toUtf8().constData());
            return 1;
        }
        storage.reload();
        QString error;
        const auto show = [&storage] {
            const zametti::ZStorage::Identity identity = storage.identity();
            const QString byRole = storage.rootId();
            std::printf("storeId:      %s\n",
                        identity.storeId().isEmpty() ? "(none)"
                                                     : identity.storeId().toUtf8().constData());
            std::printf("format:       %d\n", identity.formatVersion());
            std::printf("created:      %s\n", identity.created().toUtf8().constData());
            std::printf("rootNote:     %s\n",
                        identity.rootNote().isEmpty() ? "(none)"
                                                      : identity.rootNote().toUtf8().constData());
            std::printf("by role:      %s\n",
                        byRole.isEmpty() ? "(none)" : byRole.toUtf8().constData());
            if (!byRole.isEmpty())
                std::printf("store name:   %s\n", storage.titleOf(byRole).toUtf8().constData());
            if (identity.rootNote() != byRole)
                std::printf("MISMATCH: the json and the role disagree (run: root fix)\n");
        };

        if (positional == QStringLiteral("show")) {
            show();
            return 0;
        }
        if (positional == QStringLiteral("init") || positional == QStringLiteral("fix")) {
            // Одно и то же действие с разных сторон: init заводит, если нет;
            // fix называет в json тот корень, который нашёлся по роли. Обе
            // дороги ведут в ensureRootNote — параллельной реализации нет.
            const QString id = storage.ensureRootNote(&error);
            if (id.isEmpty()) {
                std::fprintf(stderr, "%s\n", error.toUtf8().constData());
                return 1;
            }
            show();
            return 0;
        }
        return usage();
    }

    if (command == QStringLiteral("new")) {
        if (root.isEmpty()) return usage();
        zametti::ZStorage storage(root);
        if (!storage.isStore()) {
            std::fprintf(stderr, "not a store: %s\n", root.toUtf8().constData());
            return 1;
        }
        storage.reload();
        // Названный родитель обязан существовать: окно уводит несуществующего
        // родителя в корень молча (правило владельца для Ctrl+N), а утилите с
        // явным --parent молчать нельзя — опечатка в id должна быть видна.
        if (!parent.isEmpty() && !storage.has(parent)) {
            std::fprintf(stderr, "parent is not in the store: %s\n", parent.toUtf8().constData());
            return 1;
        }
        QString error;
        const QString id = storage.createNote(parent, false, &error);
        if (id.isEmpty()) {
            std::fprintf(stderr, "%s\n", error.toUtf8().constData());
            return 1;
        }
        std::printf("%s\n", storage.pathOf(id).toUtf8().constData());
        return 0;
    }

    if (command == QStringLiteral("import")) {
        if (root.isEmpty() || from.isEmpty()) return usage();
        zametti::ZStorage::ImportOptions options;
        options.from = from;
        options.appleManifest = manifest;
        options.dryRun = dryRun;
        zametti::ZStorage::Report report;
        const bool ok = zametti::ZStorage(root).importTree(options, report);
        printLines(report);
        return ok ? 0 : 1;
    }

    // УБРАТЬ В АРХИВ И ВЕРНУТЬ ОТТУДА. Тем же путём, что окно: хранилище
    // знает про папки с содержимым, про пометку и про запись в журнал. Нужно
    // и человеку (скрипты), и наборам приёмки: воспроизвести жалобу владельца
    // без окна иначе нечем.
    if (command == QStringLiteral("archive")) {
        if (root.isEmpty() || id.isEmpty()) return usage();
        QLockFile lock(zametti::ZStorage(root).lockPath());
        if (!lock.tryLock(0)) {
            std::fprintf(stderr, "store is busy: the app seems to be open.\n");
            return 1;
        }
        zametti::ZStorage storage(root);
        storage.reload();
        QStringList failed;
        const bool back = restore;
        const bool ok = back ? storage.restore(id, &failed)
                             : storage.archive(id, zametti::ZJournal::Rules{}, &failed);
        if (!ok || !failed.isEmpty()) {
            std::fprintf(stderr, "%s\n", failed.join(QLatin1Char('\n')).toUtf8().constData());
            return 1;
        }
        std::printf("%s %s\n", id.toUtf8().constData(), back ? "is back" : "is archived");
        return 0;
    }

    // ПОДНЯТЬ УДАЛЁННУЮ ЗАМЕТКУ. Удаление насовсем — второе осознанное решение
    // подряд, поэтому в окне такой команды нет и не будет: это работа с
    // журналом, а не с деревом заметок. Заметка возвращается В АРХИВ, как и
    // лежала, с посмертными (уменьшенными) картинками.
    if (command == QStringLiteral("resurrect")) {
        if (root.isEmpty() || id.isEmpty()) return usage();
        QLockFile lock(zametti::ZStorage(root).lockPath());
        if (!lock.tryLock(0)) {
            std::fprintf(stderr, "store is busy: the app seems to be open.\n");
            return 1;
        }
        QString error;
        if (!zametti::ZStorage(root).resurrect(id, &error)) {
            std::fprintf(stderr, "%s\n", error.toUtf8().constData());
            return 1;
        }
        if (!error.isEmpty()) std::fprintf(stderr, "%s\n", error.toUtf8().constData());
        std::printf("%s is back in the archive\n", id.toUtf8().constData());
        return 0;
    }

    // Прореживание журналов. Отдельной командой, а не только фоном при старте:
    // владелец должен уметь прогнать его руками и увидеть, что именно уйдёт.
    if (command == QStringLiteral("thin")) {
        if (root.isEmpty()) return usage();
        // Межпроцессный замок: пока открыта программа, прореживание руками не
        // запускается. Внутрипроцессный замок журнала от чужого процесса не
        // бережёт, а ставить файловый на каждую запись — 4.4 мс на ровном месте.
        QLockFile lock(zametti::ZStorage(root).lockPath());
        if (!lock.tryLock(0)) {
            // Различаем два разных отказа: замок держат — и замок не завести
            // вовсе. Второе случается на каталоге, который хранилищем не
            // является, и списывать это на занятость было бы враньём.
            if (lock.error() == QLockFile::LockFailedError)
                std::fprintf(stderr,
                             "store is busy: the app seems to be open. "
                             "Thinning runs in the background at app startup.\n");
            else
                std::fprintf(stderr, "cannot take the store lock: %s\n",
                             zametti::ZStorage(root).lockPath().toUtf8().constData());
            return 1;
        }
        zametti::ZStorage storage(root);
        const zametti::ZJournal::ThinReport report =
            storage.thinAllJournals(QDateTime::currentMSecsSinceEpoch(), dryRun);
        for (const QString& name : report.trimmed)
            std::printf("%s: truncated tail cut off\n", name.toUtf8().constData());
        for (const QString& line : report.problems)
            std::fprintf(stderr, "PROBLEM: %s\n", line.toUtf8().constData());
        std::printf("journals %d, records %lld -> %lld", report.journals,
                    (long long)report.recordsBefore, (long long)report.recordsAfter);
        if (!dryRun)
            std::printf(", bytes %lld -> %lld", (long long)report.bytesBefore,
                        (long long)report.bytesAfter);
        std::printf("%s\n", dryRun ? " (dry run)" : "");
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
            std::fprintf(stderr, "no store given: need --root or a path to .md\n");
            return 1;
        }

        // Тот же межпроцессный замок, что у thin: пока открыта программа,
        // журналы правит она.
        QLockFile lock(zametti::ZStorage(target).lockPath());
        if (!lock.tryLock(0)) {
            // Занято и «замок негде завести» — разные беды, и валить вторую на
            // первую значит врать: чаще всего это просто не хранилище.
            if (lock.error() == QLockFile::LockFailedError)
                std::fprintf(stderr, "store is busy: the app seems to be open\n");
            else
                std::fprintf(stderr, "cannot take the store lock: %s\n",
                             zametti::ZStorage(target).lockPath().toUtf8().constData());
            return 1;
        }

        zametti::ZStorage storage(target);
        zametti::ZStorage::CompressReport report;
        QString error;
        // force: люк на то и люк, чтобы прогонять чистку и по уже чищеному
        // журналу — так проверяется идемпотентность.
        if (!storage.compressJournal(noteId, {}, true, &report, &error)) {
            std::fprintf(stderr, "%s\n", error.toUtf8().constData());
            return 1;
        }
        const auto name = [](const QString& v) {
            return v.isEmpty() ? QStringLiteral("0 (not cleaned)") : v;
        };
        std::printf("%s: version %s -> %s\n", noteId.toUtf8().constData(),
                    name(report.versionBefore).toUtf8().constData(),
                    name(report.versionAfter).toUtf8().constData());
        std::printf("records %d -> %d (duplicates %d, merged %d)%s\n", report.recordsBefore,
                    report.recordsAfter, report.duplicates, report.merged,
                    report.rewritten ? "" : "; file untouched");
        return 0;
    }

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
            std::fprintf(stderr, "bad number in options\n");
            return 1;
        }

        zametti::RecompressReport report;
        const bool ok = zametti::recompressStore(options, report);
        for (const QString& line : report.lines)
            std::printf("%s\n", line.toUtf8().constData());
        for (const QString& p : report.problems)
            std::fprintf(stderr, "%s\n", p.toUtf8().constData());
        if (report.examined > 0) {
            std::printf("\nexamined %d, rewritten %d, left as is %d, failed %d\n",
                        report.examined, report.rewritten, report.untouched, report.failed);
            std::printf("was %.1f MB, now %.1f MB\n",
                        double(report.bytesBefore) / 1048576.0,
                        double(report.bytesAfter) / 1048576.0);
            if (dryRun) std::printf("(dry run, nothing written)\n");
        }
        return ok ? 0 : 1;
    }

    if (command == QStringLiteral("verify")) {
        if (root.isEmpty()) return usage();
        zametti::ZStorage::Report report;
        const bool ok = zametti::ZStorage(root).verify(report);
        printLines(report);
        return ok ? 0 : 1;
    }

    return usage();
}
