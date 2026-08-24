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

#include "folder_remote.h"
#include "journal.h"
#include "keyfile.h"
#include "keyring_secrets.h"
#include "secret_store.h"
#include "webdav_remote.h"
#include "zstorage.h"
#include "recompress.h"

#include <QGuiApplication>
#include <QDateTime>
#include <QLockFile>
#include <QFileInfo>
#include <QDir>

#include <cstdio>
#include <iostream>
#include <string>

#include <termios.h>
#include <unistd.h>

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
                 "  zametti-store push-all --root <dir> --url <webdav-url>\n"
                 "                         [--user <name>] [--allow-insecure-http]\n"
                 "                         (or --to <dir> to push into a local folder)\n"
                 "  zametti-store set-remote --root <dir> (--url <webdav-url> | --to <dir>)\n"
                 "                           [--user <name>] [--allow-insecure-http] [--reset]\n"
                 "\n"
                 "  set-remote is the one-time setup: it asks the two passwords (typed,\n"
                 "  echo off; the encryption password twice when the cloud is fresh),\n"
                 "  stores the key and the server password in the system keyring and\n"
                 "  the address in <store>/.zametti/remote.json. --reset forgets both.\n"
                 "  On a fresh device pointed at an existing cloud it inherits the\n"
                 "  store identity and the next 'sync' downloads everything.\n"
                 "\n"
                 "  push-all encrypts and uploads every journal and attachment; it is\n"
                 "  the probe ancestor of 'sync' and knows nothing about merging yet.\n"
                 "  Secrets come from the environment, never from the command line:\n"
                 "    ZAMETTI_WEBDAV_PASSWORD   the server password\n"
                 "    ZAMETTI_SYNC_PASSWORD     the encryption password (a new keyfile\n"
                 "                              is minted when the cloud has none)\n"
                 "    ZAMETTI_SYNC_KEY          or the master key itself, base64\n"
                 "\n"
                 "  recompress has NO default for --id: recompression is irreversible,\n"
                 "  and one forgotten option must not migrate the whole store.\n"
                 "  All images only with an explicit '--id all'.\n");
    return 2;
}

// Пароль с клавиатуры, БЕЗ эха. Кодировка называется явно (урок сессии 3:
// байты, переходящие границу, читаются как UTF-8, а не «как получится»).
// Не терминал (обвязка наборов) — просто строка со stdin.
QString askPassword(const char* prompt) {
    std::fprintf(stderr, "%s", prompt);
    std::fflush(stderr);
    termios old{};
    const bool tty = isatty(STDIN_FILENO) != 0 && tcgetattr(STDIN_FILENO, &old) == 0;
    if (tty) {
        termios off = old;
        off.c_lflag &= ~tcflag_t(ECHO);
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &off);
    }
    std::string line;
    std::getline(std::cin, line);
    if (tty) tcsetattr(STDIN_FILENO, TCSAFLUSH, &old);
    std::fprintf(stderr, "\n");
    return QString::fromUtf8(line.data(), qsizetype(line.size()));
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
    QString url, user, to;
    bool allowInsecure = false;
    bool reset = false;
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
        else if (a == QStringLiteral("--url")) url = next();
        else if (a == QStringLiteral("--user")) user = next();
        else if (a == QStringLiteral("--to")) to = next();
        else if (a == QStringLiteral("--allow-insecure-http")) allowInsecure = true;
        else if (a == QStringLiteral("--reset")) reset = true;
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

    // ПЕРВИЧНАЯ НАСТРОЙКА ОБЛАКА — один раз за жизнь устройства (m17,
    // сессия 4). Два пароля спрашиваются с клавиатуры без эха (или берутся из
    // среды — для обвязки); ключ и пароль сервера ложатся в системный keyring,
    // адрес — в <store>/.zametti/remote.json. Все ветки знакомства с облаком
    // решает ZStorage::connectRemote, здесь только ввод.
    if (command == QStringLiteral("set-remote")) {
        if (root.isEmpty()) return usage();
        zametti::ZStorage storage(root);
        if (!storage.isStore()) {
            std::fprintf(stderr, "not a store: %s\n", root.toUtf8().constData());
            return 1;
        }
        const zametti::ZStorage::LockReport locked = storage.lock();
        if (!locked.locked) {
            std::fprintf(stderr, "the store is busy (pid %lld on %s)\n",
                         static_cast<long long>(locked.holderPid),
                         locked.holderHost.toUtf8().constData());
            return 1;
        }
        zametti::KeyringSecrets keyring;
        QString error;

        if (reset) {
            const zametti::ZStorage::Identity identity = storage.identity();
            if (!identity.isEmpty()) {
                keyring.clearKey(identity.storeId());
                keyring.clearServerPassword(identity.storeId());
            }
            if (!storage.clearRemoteConfig(&error)) {
                std::fprintf(stderr, "%s\n", error.toUtf8().constData());
                return 1;
            }
            std::printf("the cloud address and the secrets are forgotten\n");
            return 0;
        }
        if (url.isEmpty() == to.isEmpty()) return usage();  // ровно один адрес

        zametti::ZStorage::RemoteConfig cfg;
        if (!to.isEmpty())
            cfg.dir = QDir(to).absolutePath();
        else
            cfg.url = url.endsWith(QLatin1Char('/')) ? url : url + QLatin1Char('/');
        cfg.user = user;
        cfg.allowInsecureHttp = allowInsecure;

        QString serverPassword = qEnvironmentVariable("ZAMETTI_WEBDAV_PASSWORD");
        if (!cfg.url.isEmpty() && serverPassword.isEmpty())
            serverPassword = askPassword("server password: ");

        // Дважды или один раз — зависит от того, есть ли в облаке конверт:
        // опечатка в пароле при СОЗДАНИИ запечатала бы облако навсегда, а при
        // развороте существующего она безобидна — конверт просто не откроется.
        auto probe = storage.makeRemote(cfg, serverPassword, &error);
        if (!probe) {
            std::fprintf(stderr, "%s\n", error.toUtf8().constData());
            return 1;
        }
        QByteArray envelope;
        const bool freshCloud = !probe->get(QLatin1String(zametti::Keyfile::kRemoteName),
                                            &envelope, nullptr, nullptr);
        QString password = qEnvironmentVariable("ZAMETTI_SYNC_PASSWORD");
        if (password.isEmpty()) {
            password = askPassword(freshCloud ? "new encryption password: "
                                              : "encryption password: ");
            if (freshCloud && password != askPassword("repeat the encryption password: ")) {
                std::fprintf(stderr, "the passwords do not match\n");
                return 1;
            }
        }

        zametti::ZStorage::ConnectOutcome outcome;
        if (!storage.connectRemote(cfg, password, serverPassword, keyring,
                                   zametti::Keyfile::defaults(), &outcome, &error)) {
            std::fprintf(stderr, "%s\n", error.toUtf8().constData());
            return 1;
        }
        if (!keyring.available())
            std::fprintf(stderr,
                         "warning: no system keyring — the key is not remembered, and the "
                         "password will be asked again\n");
        if (outcome.inheritedIdentity)
            std::printf("inherited the store identity from the cloud; "
                        "'zametti-store sync' will download everything\n");
        if (outcome.mintedKeyfile) std::printf("minted a new keyfile and uploaded it\n");
        std::printf("connected: %s\n",
                    (cfg.url.isEmpty() ? cfg.dir : cfg.url).toUtf8().constData());
        return 0;
    }

    // ЗАЛИТЬ ВСЁ В ОБЛАКО — люк разведки (m17). Прародитель `sync`: только
    // исходящее, ни скачиваний, ни слияний. Секреты берутся из среды, а не из
    // командной строки: командная строка видна всей машине (`ps`) и оседает в
    // истории оболочки.
    if (command == QStringLiteral("push-all")) {
        if (root.isEmpty() || (url.isEmpty() && to.isEmpty())) return usage();
        zametti::ZStorage storage(root);
        if (!storage.isStore()) {
            std::fprintf(stderr, "not a store: %s\n", root.toUtf8().constData());
            return 1;
        }
        const zametti::ZStorage::LockReport locked = storage.lock();
        if (!locked.locked) {
            std::fprintf(stderr, "the store is busy (pid %lld on %s)\n",
                         static_cast<long long>(locked.holderPid),
                         locked.holderHost.toUtf8().constData());
            return 1;
        }
        QString error;
        const zametti::ZStorage::Identity identity = storage.ensureIdentity(&error);
        if (identity.isEmpty()) {
            std::fprintf(stderr, "%s\n", error.toUtf8().constData());
            return 1;
        }

        // Адаптер: настоящий WebDAV или локальный каталог (замеры и наборы —
        // тот же путь без сети).
        std::shared_ptr<zametti::RemoteStore> remote;
        if (!to.isEmpty()) {
            remote = std::make_shared<zametti::FolderRemote>(to);
        } else {
            zametti::WebDavRemote::Config config;
            config.base = QUrl(url.endsWith(QLatin1Char('/')) ? url : url + QLatin1Char('/'));
            config.user = user;
            config.password = qEnvironmentVariable("ZAMETTI_WEBDAV_PASSWORD");
            config.allowInsecureHttp = allowInsecure;
            if (!zametti::WebDavRemote::checkUrl(config, &error)) {
                std::fprintf(stderr, "%s\n", error.toUtf8().constData());
                return 1;
            }
            remote = std::make_shared<zametti::WebDavRemote>(config);
        }

        // Ключ. Готовый — из среды; иначе разворачиваем конверт с сервера
        // паролем, а если конверта нет — чеканим новый ключ и заливаем его.
        zametti::EnvSecrets secrets;
        zametti::Keyfile keyfile;
        const QString password = qEnvironmentVariable("ZAMETTI_SYNC_PASSWORD");
        if (secrets.loadKey(identity.storeId(), &keyfile, nullptr)) {
            // ключ пришёл из среды — конверта нет и не нужно
        } else if (password.isEmpty()) {
            std::fprintf(stderr,
                         "no key: set ZAMETTI_SYNC_KEY or ZAMETTI_SYNC_PASSWORD\n");
            return 1;
        } else {
            QByteArray envelope;
            if (remote->get(QLatin1String(zametti::Keyfile::kRemoteName), &envelope,
                            nullptr, nullptr)) {
                if (!keyfile.parse(envelope, &error) || !keyfile.unwrap(password, &error)) {
                    std::fprintf(stderr, "%s\n", error.toUtf8().constData());
                    return 1;
                }
            } else {
                std::fprintf(stderr, "the cloud has no keyfile yet — minting one\n");
                if (!zametti::Keyfile::create(identity.storeId(), password,
                                              zametti::Keyfile::defaults(), &keyfile,
                                              &error)) {
                    std::fprintf(stderr, "%s\n", error.toUtf8().constData());
                    return 1;
                }
                if (!remote->mkdirOnce(&error) ||
                    !remote->put(QLatin1String(zametti::Keyfile::kRemoteName),
                                 keyfile.toBytes(), nullptr, &error)) {
                    std::fprintf(stderr, "%s\n", error.toUtf8().constData());
                    return 1;
                }
            }
        }

        if (!storage.setRemote(remote, keyfile, &error)) {
            std::fprintf(stderr, "%s\n", error.toUtf8().constData());
            return 1;
        }
        zametti::ZStorage::PushReport report;
        const bool ok = storage.pushAll(&report, &error);
        // Числа печатаются в любом случае: половина работы, сделанная до
        // обрыва, — тоже результат, и следующий прогон её достроит.
        std::printf("baselined %d, journals %d, attachments %d\n",
                    report.baselined, report.journals, report.attachments);
        std::printf("plaintext %lld B, ciphertext %lld B (overhead %lld B)\n",
                    static_cast<long long>(report.plainBytes),
                    static_cast<long long>(report.sealedBytes),
                    static_cast<long long>(report.sealedBytes - report.plainBytes));
        std::printf("time: baseline %.1f ms, seal %.1f ms, upload %.1f ms\n",
                    report.usBaseline / 1000.0, report.usSeal / 1000.0,
                    report.usPut / 1000.0);
        const zametti::RemoteStore::Traffic& traffic = remote->traffic();
        std::printf("traffic: %lld requests, %lld B up, %lld B down\n",
                    static_cast<long long>(traffic.requests),
                    static_cast<long long>(traffic.bytesUp),
                    static_cast<long long>(traffic.bytesDown));
        if (!ok) {
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
