#include "store_cli.h"

#include "diff.h"
#include "document_pieces.h"
#include "journal.h"
#include "keyfile.h"
#include "keyring_secrets.h"
#include "recompress.h"
#include "secret_store.h"
#include "settings.h"
#include "zlogs.h"

#include <QDateTime>
#include <QFileInfo>
#include <QSet>
#include <QDir>

#include <algorithm>
#include <cstdio>
#include <iostream>
#include <string>

#ifdef _WIN32
#include <windows.h>
#else
#include <termios.h>
#include <unistd.h>
#endif

namespace zametti {

void StoreCli::printLines(const ZStorage::Report& report) const {
    for (const QString& line : report.lines)
        std::printf("%s\n", line.toUtf8().constData());
}

// ЗАМОК ХРАНИЛИЩА — ОДНОЙ ДОРОГОЙ. Раньше половина команд брала его через
// ZStorage::lock() (та умеет снять забытый замок мёртвого процесса), а
// половина заводила голый QLockFile рядом — и вела себя иначе на том же
// хранилище. Дорога теперь одна, а печать двух разных отказов — здесь: занято
// живой программой или замка не завести вовсе (чаще всего просто не хранилище).
bool StoreCli::takeLock(ZStorage& storage, const char* busyHint) const {
    const ZStorage::LockReport locked = storage.lock();
    if (locked.locked) return true;
    if (locked.busy)
        std::fprintf(stderr, "store is busy: the app seems to be open (pid %lld on %s).%s\n",
                     static_cast<long long>(locked.holderPid),
                     locked.holderHost.toUtf8().constData(), busyHint);
    else
        std::fprintf(stderr, "cannot take the store lock: %s\n",
                     storage.lockPath().toUtf8().constData());
    return false;
}

ZStorage::Config StoreCli::addressFromFlags() const {
    ZStorage::Config cfg;
    if (to_.isEmpty() && url_.isEmpty()) return cfg;
    if (!to_.isEmpty())
        cfg.cloudDir = QDir(to_).absolutePath();
    else
        cfg.cloudUrl = url_.endsWith(QLatin1Char('/')) ? url_ : url_ + QLatin1Char('/');
    cfg.cloudUser = user_;
    return cfg;
}

int StoreCli::usage() const {
    // В stdout, если справку попросили словами, и в stderr, если это ответ на
    // ошибку употребления: осознанный вопрос — не беда, и в конвейере его
    // ответ должен идти туда же, куда весь вывод.
    const bool asked = command_ == QLatin1String("--help") ||
                       command_ == QLatin1String("-h") ||
                       command_ == QLatin1String("help");
    std::FILE* out = asked ? stdout : stderr;
    std::fprintf(out,
                 "usage:\n"
                 "  zametti store init <dir>\n"
                 "  zametti store new --root <dir> [--parent <id>]\n"
                 "  zametti store import --root <dir> --from <srcdir>"
                 " [--apple-manifest <json>] [--dry-run]\n"
                 "  zametti store verify --root <dir>\n"
                 "  zametti store thin --root <dir> [--dry-run]\n"
                 "  zametti store history compress <id | path to .md> [--root <dir>]\n"
                 "  zametti store history audit --root <dir>\n"
                 "  zametti store recompress --root <dir> --id <id|all>\n"
                 "  zametti store resurrect --root <dir> --id <id>\n"
                 "  zametti store remove --root <dir> --id <id>\n"
                 "  zametti store archive --root <dir> --id <id> [--restore]\n"
                 "  zametti store root show|init|fix --root <dir>\n"
                 "  zametti store push-all --root <dir> --url <webdav-url>\n"
                 "                         [--user <name>]\n"
                 "                         (or --to <dir> to push into a local folder)\n"
                 "  zametti store set-cloud --root <dir> (--url <webdav-url> | --to <dir>)\n"
                 "                          [--user <name>] [--reset]\n"
                 "  zametti store sync --root <dir> [--full | --push-only]\n"
                 "                     [--allow-mass-delete | --keep-all]\n"
                 "                     [--url <webdav-url> | --to <dir>] [--user <name>]\n"
                 "\n"
                 "  sync runs the engine: align, exchange, merge, materialize. The cloud\n"
                 "  address comes from .zametti/cloud.json (set-cloud) unless --url or\n"
                 "  --to overrides it; secrets come from the keyring or the environment.\n"
                 "  When a run wants to delete more notes than the guard allows it stops,\n"
                 "  lists them, and asks for an explicit decision: --allow-mass-delete\n"
                 "  applies the deletions, --keep-all declares the notes alive instead.\n"
                 "\n"
                 "  set-cloud is the one-time setup: it asks the two passwords (typed,\n"
                 "  echo off; the encryption password twice when the cloud is fresh),\n"
                 "  stores the key and the server password in the system keyring and\n"
                 "  the address in <store>/.zametti/cloud.json. --reset forgets both.\n"
                 "  set-remote is the old name and still works.\n"
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
    return asked ? 0 : 2;
}

// Пароль с клавиатуры, БЕЗ эха. Кодировка называется явно (урок сессии 3:
// байты, переходящие границу, читаются как UTF-8, а не «как получится»).
// Не терминал (обвязка наборов) — просто строка со stdin.
QString StoreCli::askPassword(const char* prompt) {
    std::fprintf(stderr, "%s", prompt);
    std::fflush(stderr);
#ifdef _WIN32
    // У Windows эхо гасится не у файлового описателя, а у КОНСОЛИ: режим
    // снимается с самого дескриптора ввода, и если ввод перенаправлен (обвязка
    // наборов, конвейер), GetConsoleMode честно отвечает отказом — это и есть
    // здешняя проверка «терминал ли».
    const HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    const bool tty = in != INVALID_HANDLE_VALUE && GetConsoleMode(in, &mode) != 0;
    if (tty) SetConsoleMode(in, mode & ~DWORD(ENABLE_ECHO_INPUT));
#else
    termios old{};
    const bool tty = isatty(STDIN_FILENO) != 0 && tcgetattr(STDIN_FILENO, &old) == 0;
    if (tty) {
        termios off = old;
        off.c_lflag &= ~tcflag_t(ECHO);
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &off);
    }
#endif
    std::string line;
    std::getline(std::cin, line);
#ifdef _WIN32
    if (tty) SetConsoleMode(in, mode);
#else
    if (tty) tcsetattr(STDIN_FILENO, TCSAFLUSH, &old);
#endif
    std::fprintf(stderr, "\n");
    return QString::fromUtf8(line.data(), qsizetype(line.size()));
}

bool StoreCli::parse() {
    for (qsizetype i = 2; i < args_.size(); ++i) {
        const QString& a = args_.at(i);
        const auto next = [&]() -> QString {
            return i + 1 < args_.size() ? args_.at(++i) : QString();
        };
        if (a == QStringLiteral("--root")) root_ = next();
        else if (a == QStringLiteral("--from")) from_ = next();
        else if (a == QStringLiteral("--apple-manifest")) manifest_ = next();
        else if (a == QStringLiteral("--parent")) parent_ = next();
        else if (a == QStringLiteral("--id")) id_ = next();
        else if (a == QStringLiteral("--max-size")) maxSize_ = next();
        else if (a == QStringLiteral("--quality")) quality_ = next();
        else if (a == QStringLiteral("--dry-run")) dryRun_ = true;
        else if (a == QStringLiteral("--restore")) restore_ = true;
        else if (a == QStringLiteral("--url")) url_ = next();
        else if (a == QStringLiteral("--user")) user_ = next();
        else if (a == QStringLiteral("--to")) to_ = next();
        else if (a == QStringLiteral("--reset")) reset_ = true;
        else if (a == QStringLiteral("--full")) pushOnly_ = false;
        else if (a == QStringLiteral("--push-only")) pushOnly_ = true;
        else if (a == QStringLiteral("--allow-mass-delete")) allowMassDelete_ = true;
        else if (a == QStringLiteral("--keep-all")) keepAll_ = true;
        else if (!a.startsWith(QStringLiteral("--")) && positional_.isEmpty()) positional_ = a;
        else if (!a.startsWith(QStringLiteral("--")) && positional2_.isEmpty()) positional2_ = a;
        else return false;
    }
    return true;
}

int StoreCli::run() {
    if (args_.size() < 2) return usage();
    command_ = args_.at(1);
    if (command_ == QLatin1String("--help") || command_ == QLatin1String("-h") ||
        command_ == QLatin1String("help"))
        return usage();
    if (!parse()) return usage();

    if (command_ == QStringLiteral("init")) return cmdInit();
    // set-remote — прежнее имя, остаётся псевдонимом на одну версию (§0).
    if (command_ == QStringLiteral("set-cloud") ||
        command_ == QStringLiteral("set-remote"))
        return cmdSetCloud();
    if (command_ == QStringLiteral("sync")) return cmdSync();
    if (command_ == QStringLiteral("push-all")) return cmdPushAll();
    if (command_ == QStringLiteral("root")) return cmdRoot();
    if (command_ == QStringLiteral("new")) return cmdNew();
    if (command_ == QStringLiteral("import")) return cmdImport();
    if (command_ == QStringLiteral("archive")) return cmdArchive();
    if (command_ == QStringLiteral("remove")) return cmdRemove();
    if (command_ == QStringLiteral("resurrect")) return cmdResurrect();
    if (command_ == QStringLiteral("thin")) return cmdThin();
    if (command_ == QStringLiteral("history")) return cmdHistory();
    if (command_ == QStringLiteral("recompress")) return cmdRecompress();
    if (command_ == QStringLiteral("verify")) return cmdVerify();
    return usage();
}

int StoreCli::cmdInit() {
    if (positional_.isEmpty()) return usage();
    QString error;
    if (!ZStorage(positional_).init(&error)) {
        std::fprintf(stderr, "%s\n", error.toUtf8().constData());
        return 1;
    }
    return 0;
}

// ПЕРВИЧНАЯ НАСТРОЙКА ОБЛАКА — один раз за жизнь устройства (m17, сессия 4).
// Два пароля спрашиваются с клавиатуры без эха (или берутся из среды — для
// обвязки); ключ и пароль сервера ложатся в системный keyring, адрес — в
// <store>/.zametti/cloud.json. Все ветки знакомства с облаком решает
// ZStorage::connectCloud, здесь только ввод.
int StoreCli::cmdSetCloud() {
    if (root_.isEmpty()) return usage();
    ZStorage storage(root_);
    // «Не хранилище» здесь НЕ отказ: пустой или несуществующий каталог —
    // законный вход нового устройства, каркас заведёт connectCloud.
    // Замок — только у существующего хранилища: в пустом каталоге ещё
    // нечего охранять, а замку негде жить.
    if (storage.isStore()) {
        if (!takeLock(storage)) return 1;
    }
    // Секрет в среде = headless-намерение (обвязка, скрипты): в keyring не
    // пишем и разблокировку не выбиваем — ключ остаётся в среде вызвавшего.
    const bool headless = !qEnvironmentVariable("ZAMETTI_SYNC_PASSWORD").isEmpty() ||
                          !qEnvironmentVariable("ZAMETTI_SYNC_KEY").isEmpty();
    KeyringSecrets keyring;
    EnvSecrets envSink;
    SecretStore& secrets = headless ? static_cast<SecretStore&>(envSink)
                                    : static_cast<SecretStore&>(keyring);
    QString error;

    if (reset_) {
        const ZStorage::Identity identity = storage.identity();
        if (!identity.isEmpty()) {
            keyring.clearKey(identity.storeId());
            keyring.clearServerPassword(identity.storeId());
            keyring.clearEncryptionPassword(identity.storeId());
        }
        if (!storage.clearCloudConfig(&error)) {
            std::fprintf(stderr, "%s\n", error.toUtf8().constData());
            return 1;
        }
        std::printf("the cloud address and the secrets are forgotten\n");
        return 0;
    }
    if (url_.isEmpty() == to_.isEmpty()) return usage();  // ровно один адрес

    const ZStorage::Config cfg = addressFromFlags();

    QString serverPassword = qEnvironmentVariable("ZAMETTI_WEBDAV_PASSWORD");
    // Переподключение (тот же адрес после ротации, утраченный конверт):
    // пароль сервера не спрашивается заново, если он уже в keyring, —
    // человек вводит только пароль шифрования.
    if (!cfg.cloudUrl.isEmpty() && serverPassword.isEmpty() && !headless && keyring.available()) {
        const ZStorage::Identity mine = storage.identity();
        if (!mine.isEmpty()) serverPassword = keyring.serverPassword(mine.storeId());
    }
    if (!cfg.cloudUrl.isEmpty() && serverPassword.isEmpty())
        serverPassword = askPassword("server password: ");

    // Дважды или один раз — зависит от того, есть ли в облаке конверт;
    // спрашивает об этом хранилище (оно же проверяет адрес).
    const bool freshCloud = !ZStorage::cloudHasKeyfile(cfg, serverPassword, &error);
    if (!error.isEmpty()) {
        std::fprintf(stderr, "%s\n", error.toUtf8().constData());
        return 1;
    }
    QString password = qEnvironmentVariable("ZAMETTI_SYNC_PASSWORD");
    if (password.isEmpty()) {
        password = askPassword(freshCloud ? "new encryption password: "
                                          : "encryption password: ");
        if (freshCloud && password != askPassword("repeat the encryption password: ")) {
            std::fprintf(stderr, "the passwords do not match\n");
            return 1;
        }
    }

    ZStorage::ConnectOutcome outcome;
    // Статический вход: тот же метод позовёт диалог первичной настройки в
    // самом zametti (и на Android, где консоли нет).
    if (!ZStorage::initFromCloud(root_, cfg, password, serverPassword, secrets,
                                  Keyfile::defaults(), &outcome, &error)) {
        std::fprintf(stderr, "%s\n", error.toUtf8().constData());
        return 1;
    }
    if (!headless && !keyring.available())
        std::fprintf(stderr,
                     "warning: no system keyring — the key is not remembered, and the "
                     "password will be asked again\n");
    if (outcome.inheritedIdentity)
        std::printf("inherited the store identity from the cloud%s; "
                    "'zametti store sync' will download everything\n",
                    outcome.rootMaterialized ? " and fetched the root note" : "");
    if (outcome.mintedKeyfile) std::printf("minted a new keyfile and uploaded it\n");
    // Сводка одним числом на род — полного перечисления не бывает
    // (решение владельца): облако может быть большим.
    if (outcome.cloudNotes > 0 || outcome.cloudAttachments > 0)
        std::printf("the cloud holds: %d notes, %d attachments, %.1f MB\n",
                    outcome.cloudNotes, outcome.cloudAttachments,
                    double(outcome.cloudBytes) / (1024.0 * 1024.0));
    std::printf("connected: %s\n", (cfg.cloudUrl.isEmpty() ? cfg.cloudDir
                                                           : cfg.collectionUrl())
                                       .toUtf8()
                                       .constData());
    return 0;
}

// СИНХРОНИЗАЦИЯ — тот же движок, что у окна (m17, сессия 4). push-all
// остаётся люком замера первой заливки; здесь — полный цикл.
int StoreCli::cmdSync() {
    if (root_.isEmpty()) return usage();
    if (allowMassDelete_ && keepAll_) return usage();
    ZStorage storage(root_);
    if (!storage.isStore()) {
        std::fprintf(stderr, "not a store: %s\n", root_.toUtf8().constData());
        return 1;
    }
    if (!takeLock(storage)) return 1;
    QString error;
    const ZStorage::Identity identity = storage.ensureIdentity(&error);
    if (identity.isEmpty()) {
        std::fprintf(stderr, "%s\n", error.toUtf8().constData());
        return 1;
    }

    // Адрес: ключи командной строки сильнее cloud.json.
    ZStorage::Config cfg = addressFromFlags();
    if (!cfg.hasCloudAddress()) cfg = storage.cloudConfig();
    if (!cfg.hasCloudAddress()) {
        std::fprintf(stderr, "sync is not configured: run set-cloud once, or pass --url/--to\n");
        return 1;
    }

    // Подключение — одной лестницей ядра (адрес, пароль сервера, ключ,
    // конверт). Здесь остаётся только выбор чьих секретов спрашивать: секрет
    // в среде = headless-намерение (обвязка, скрипты), и keyring тогда не
    // трогается вовсе — иначе скриптовый прогон выбивал бы на экран диалог
    // разблокировки.
    const bool headless = !qEnvironmentVariable("ZAMETTI_SYNC_PASSWORD").isEmpty() ||
                          !qEnvironmentVariable("ZAMETTI_SYNC_KEY").isEmpty();
    KeyringSecrets keyring;
    EnvSecrets envSecrets;
    SecretStore& secrets = headless ? static_cast<SecretStore&>(envSecrets)
                                    : static_cast<SecretStore&>(keyring);
    ZStorage::AttachOptions how;
    how.cfg = cfg;
    how.serverPassword = qEnvironmentVariable("ZAMETTI_WEBDAV_PASSWORD");
    how.encryptionPassword = qEnvironmentVariable("ZAMETTI_SYNC_PASSWORD");
    if (!storage.attachCloud(how, secrets, nullptr, &error)) {
        std::fprintf(stderr, "%s\n", error.toUtf8().constData());
        return 1;
    }

    // Логи — те же, что у окна: настройки из config.json (битый или
    // отсутствующий конфиг = дефолты, то есть логи выключены).
    QString cfgWhy;
    loadSettings(&cfgWhy, nullptr);
    ZLogs& logs = ZLogs::instance();
    logs.configure({settings().logs().writeErrLog(), settings().logs().writeSyncLog(),
                    qint64(settings().logs().logSizeMb()) * 1024 * 1024});

    ZStorage::SyncOptions options;
    options.mode = pushOnly_ ? ZStorage::SyncOptions::PushOnly : ZStorage::SyncOptions::Full;
    options.allowMassDelete = allowMassDelete_;
    options.logs = &logs;
    ZStorage::SyncReport report;
    bool ok = storage.sync(options, &report, &error);

    // ОТКАЗ ОТ МАССОВОГО УДАЛЕНИЯ: --keep-all объявляет задержанные
    // заметки живыми и доделывает прогон — облако лечится записями
    // поверх надгробий.
    if (ok && !report.pendingDeletes.isEmpty() && keepAll_) {
        if (!storage.declareAlive(report.pendingDeletes, &error)) {
            std::fprintf(stderr, "%s\n", error.toUtf8().constData());
            return 1;
        }
        ok = storage.sync(options, &report, &error);
    }

    // Числа печатаются в любом случае: половина работы до обрыва — тоже
    // результат, следующий прогон её достроит.
    std::printf("align: %d checked, %d baselined, %d external, %d by stat-scan\n",
                report.dirtyChecked, report.baselined, report.externalRecorded,
                report.statScanned);
    std::printf("exchange: %d listed, %d skipped, %d etag-reissued, %d taken, %d pushed, "
                "%d merged, %d deferred, %d healed, %d corrupt-as-absence\n",
                report.listed, report.skipped, report.etagReissued, report.takenWhole,
                report.pushedWhole, report.mergedJournals, report.deferred, report.healedCloud,
                report.corruptLocalTreatedAsAbsence);
    std::printf("materialize: %d files, %d deletes; attachments: %d up, %d down\n",
                report.materialized, report.deletesApplied, report.attachmentsUp,
                report.attachmentsDown);
    std::printf("time: align %.1f ms, exchange %.1f ms, materialize %.1f ms\n",
                report.usAlign / 1000.0, report.usExchange / 1000.0,
                report.usMaterialize / 1000.0);
    std::printf("traffic: %lld requests, %lld B up, %lld B down\n",
                static_cast<long long>(report.traffic.requests),
                static_cast<long long>(report.traffic.bytesUp),
                static_cast<long long>(report.traffic.bytesDown));
    for (const QString& name : report.attachmentConflicts)
        std::printf("attachment conflict (kept both sides apart): %s\n",
                    name.toUtf8().constData());
    if (!report.pendingDeletes.isEmpty()) {
        std::printf("HELD: this run wants to delete %lld notes:\n",
                    static_cast<long long>(report.pendingDeletes.size()));
        for (const QString& held : report.pendingDeletes)
            std::printf("  %s\n", held.toUtf8().constData());
        std::printf("decide: rerun with --allow-mass-delete to apply, or --keep-all to "
                    "declare them alive\n");
    }
    if (!ok) {
        std::fprintf(stderr, "%s\n", error.toUtf8().constData());
        return 1;
    }
    return 0;
}

// ЗАЛИТЬ ВСЁ В ОБЛАКО — люк разведки (m17). Прародитель `sync`: только
// исходящее, ни скачиваний, ни слияний. Секреты берутся из среды, а не из
// командной строки: командная строка видна всей машине (`ps`) и оседает в
// истории оболочки.
int StoreCli::cmdPushAll() {
    if (root_.isEmpty() || (url_.isEmpty() && to_.isEmpty())) return usage();
    ZStorage storage(root_);
    if (!storage.isStore()) {
        std::fprintf(stderr, "not a store: %s\n", root_.toUtf8().constData());
        return 1;
    }
    if (!takeLock(storage)) return 1;
    QString error;
    const ZStorage::Identity identity = storage.ensureIdentity(&error);
    if (identity.isEmpty()) {
        std::fprintf(stderr, "%s\n", error.toUtf8().constData());
        return 1;
    }

    // Адрес и ключ — той же лестницей ядра, что у sync; разница одна и она
    // названа явно: люку первой заливки разрешено чеканить ключ в пустое
    // облако. Секреты только из среды: командная строка видна всей машине.
    ZStorage::AttachOptions how;
    how.cfg = addressFromFlags();
    how.serverPassword = qEnvironmentVariable("ZAMETTI_WEBDAV_PASSWORD");
    how.encryptionPassword = qEnvironmentVariable("ZAMETTI_SYNC_PASSWORD");
    how.mintIfCloudEmpty = true;
    EnvSecrets secrets;
    ZStorage::AttachOutcome attached;
    if (!storage.attachCloud(how, secrets, &attached, &error)) {
        std::fprintf(stderr, "%s\n", error.toUtf8().constData());
        return 1;
    }
    if (attached.mintedKeyfile)
        std::fprintf(stderr, "the cloud had no keyfile — minted one\n");
    ZStorage::PushReport report;
    const bool ok = storage.pushAll(&report, &error);
    // Числа печатаются в любом случае: половина работы, сделанная до
    // обрыва, — тоже результат, и следующий прогон её достроит.
    std::printf("baselined %d, journals %d, attachments %d\n", report.baselined, report.journals,
                report.attachments);
    std::printf("plaintext %lld B, ciphertext %lld B (overhead %lld B)\n",
                static_cast<long long>(report.plainBytes),
                static_cast<long long>(report.sealedBytes),
                static_cast<long long>(report.sealedBytes - report.plainBytes));
    std::printf("time: baseline %.1f ms, seal %.1f ms, upload %.1f ms\n",
                report.usBaseline / 1000.0, report.usSeal / 1000.0, report.usPut / 1000.0);
    const CloudStore::Traffic& traffic = storage.cloud()->traffic();
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
int StoreCli::cmdRoot() {
    if (root_.isEmpty() || positional_.isEmpty()) return usage();
    ZStorage storage(root_);
    if (!storage.isStore()) {
        std::fprintf(stderr, "not a store: %s\n", root_.toUtf8().constData());
        return 1;
    }
    storage.reload();
    QString error;
    const auto show = [&storage] {
        const ZStorage::Identity identity = storage.identity();
        const QString byRole = storage.rootId();
        std::printf("storeId:      %s\n", identity.storeId().isEmpty()
                                              ? "(none)"
                                              : identity.storeId().toUtf8().constData());
        std::printf("format:       %d\n", identity.formatVersion());
        std::printf("created:      %s\n", identity.created().toUtf8().constData());
        std::printf("rootNote:     %s\n", identity.rootNote().isEmpty()
                                              ? "(none)"
                                              : identity.rootNote().toUtf8().constData());
        std::printf("by role:      %s\n",
                    byRole.isEmpty() ? "(none)" : byRole.toUtf8().constData());
        if (!byRole.isEmpty())
            std::printf("store name:   %s\n", storage.titleOf(byRole).toUtf8().constData());
        if (identity.rootNote() != byRole)
            std::printf("MISMATCH: the json and the role disagree (run: root fix)\n");
    };

    if (positional_ == QStringLiteral("show")) {
        show();
        return 0;
    }
    if (positional_ == QStringLiteral("init") || positional_ == QStringLiteral("fix")) {
        // Одно и то же действие с разных сторон: init заводит, если нет;
        // fix называет в json тот корень, который нашёлся по роли. Обе
        // дороги ведут в ensureRootNote — параллельной реализации нет.
        const QString made = storage.ensureRootNote(&error);
        if (made.isEmpty()) {
            std::fprintf(stderr, "%s\n", error.toUtf8().constData());
            return 1;
        }
        show();
        return 0;
    }
    return usage();
}

int StoreCli::cmdNew() {
    if (root_.isEmpty()) return usage();
    ZStorage storage(root_);
    if (!storage.isStore()) {
        std::fprintf(stderr, "not a store: %s\n", root_.toUtf8().constData());
        return 1;
    }
    storage.reload();
    // Названный родитель обязан существовать: окно уводит несуществующего
    // родителя в корень молча (правило владельца для Ctrl+N), а утилите с
    // явным --parent молчать нельзя — опечатка в id должна быть видна.
    if (!parent_.isEmpty() && !storage.has(parent_)) {
        std::fprintf(stderr, "parent is not in the store: %s\n", parent_.toUtf8().constData());
        return 1;
    }
    QString error;
    const QString made = storage.createNote(parent_, false, &error);
    if (made.isEmpty()) {
        std::fprintf(stderr, "%s\n", error.toUtf8().constData());
        return 1;
    }
    std::printf("%s\n", storage.pathOf(made).toUtf8().constData());
    return 0;
}

int StoreCli::cmdImport() {
    if (root_.isEmpty() || from_.isEmpty()) return usage();
    ZStorage::ImportOptions options;
    options.from = from_;
    options.appleManifest = manifest_;
    options.dryRun = dryRun_;
    ZStorage::Report report;
    const bool ok = ZStorage(root_).importTree(options, report);
    printLines(report);
    return ok ? 0 : 1;
}

// УБРАТЬ В АРХИВ И ВЕРНУТЬ ОТТУДА. Тем же путём, что окно: хранилище
// знает про папки с содержимым, про пометку и про запись в журнал. Нужно
// и человеку (скрипты), и наборам приёмки: воспроизвести жалобу владельца
// без окна иначе нечем.
int StoreCli::cmdArchive() {
    if (root_.isEmpty() || id_.isEmpty()) return usage();
    ZStorage storage(root_);
    if (!takeLock(storage)) return 1;
    storage.reload();
    QStringList failed;
    const bool back = restore_;
    const bool ok = back ? storage.restore(id_, &failed)
                         : storage.archive(id_, ZJournal::Rules{}, &failed);
    if (!ok || !failed.isEmpty()) {
        std::fprintf(stderr, "%s\n", failed.join(QLatin1Char('\n')).toUtf8().constData());
        return 1;
    }
    std::printf("%s %s\n", id_.toUtf8().constData(), back ? "is back" : "is archived");
    return 0;
}

// УДАЛИТЬ НАСОВСЕМ — штатным глаголом хранилища: надгробие в журнал,
// файл в мусорку ОС, у папки — поддерево. Зеркало resurrect и тестовый
// люк сценариев синка.
int StoreCli::cmdRemove() {
    if (root_.isEmpty() || id_.isEmpty()) return usage();
    ZStorage storage(root_);
    if (!storage.isStore()) {
        std::fprintf(stderr, "not a store: %s\n", root_.toUtf8().constData());
        return 1;
    }
    QString error;
    storage.reload();
    if (!storage.remove(id_, ImportLimits(), &error)) {
        std::fprintf(stderr, "%s\n", error.toUtf8().constData());
        return 1;
    }
    return 0;
}

// ПОДНЯТЬ УДАЛЁННУЮ ЗАМЕТКУ. Удаление насовсем — второе осознанное решение
// подряд, поэтому в окне такой команды нет и не будет: это работа с
// журналом, а не с деревом заметок. Заметка возвращается В АРХИВ, как и
// лежала, с посмертными (уменьшенными) картинками.
int StoreCli::cmdResurrect() {
    if (root_.isEmpty() || id_.isEmpty()) return usage();
    ZStorage storage(root_);
    if (!takeLock(storage)) return 1;
    QString error;
    if (!storage.resurrect(id_, &error)) {
        std::fprintf(stderr, "%s\n", error.toUtf8().constData());
        return 1;
    }
    if (!error.isEmpty()) std::fprintf(stderr, "%s\n", error.toUtf8().constData());
    std::printf("%s is back in the archive\n", id_.toUtf8().constData());
    return 0;
}

// Прореживание журналов. Отдельной командой, а не только фоном при старте:
// владелец должен уметь прогнать его руками и увидеть, что именно уйдёт.
int StoreCli::cmdThin() {
    if (root_.isEmpty()) return usage();
    // Межпроцессный замок: пока открыта программа, прореживание руками не
    // запускается. Внутрипроцессный замок журнала от чужого процесса не
    // бережёт, а ставить файловый на каждую запись — 4.4 мс на ровном месте.
    ZStorage storage(root_);
    if (!takeLock(storage, " Thinning runs in the background at app startup.")) return 1;
    const ZJournal::ThinReport report =
        storage.thinAllJournals(QDateTime::currentMSecsSinceEpoch(), dryRun_);
    for (const QString& name : report.trimmed)
        std::printf("%s: truncated tail cut off\n", name.toUtf8().constData());
    for (const QString& line : report.problems)
        std::fprintf(stderr, "PROBLEM: %s\n", line.toUtf8().constData());
    std::printf("journals %d, records %lld -> %lld", report.journals,
                (long long)report.recordsBefore, (long long)report.recordsAfter);
    if (!dryRun_)
        std::printf(", bytes %lld -> %lld", (long long)report.bytesBefore,
                    (long long)report.bytesAfter);
    std::printf("%s\n", dryRun_ ? " (dry run)" : "");
    return report.problems.isEmpty() ? 0 : 1;
}

// ТЕСТОВЫЙ ЛЮК. Существует ровно для того, чтобы гонять миграцию без UI:
// форсирует ТУ ЖЕ функцию, что зовут автосохранение и вход в историю, а не
// параллельную реализацию «как бы того же самого».
//
// Штатного пути чистить историю руками у человека нет и не будет: чистка
// ленивая и пер-заметочная (решение владельца).
// АУДИТ ЖУРНАЛОВ: откуда берутся записи, у которых в истории «+0/−0».
//
// Правило журнала сравнивает слепки БАЙТАМИ (без строк modified/version), а
// вид истории показывает строки после разбора и записи нынешним каноном без
// шапки. Между этими двумя «одинаково» и живут пустые записи. Аудит идёт по
// всем журналам хранилища и раскладывает соседние пары слепков по классам:
// байты равны; только шапка (и какие ключи); только канон (шапка та же, тело
// после разбора то же, байты разные); настоящая правка. Плюс вид записи,
// которой пара кончается, — Save это или External, — и заметки, где пустых
// пар больше всего. Числа, не догадки.
int StoreCli::cmdHistoryAudit() {
    if (root_.isEmpty()) return usage();
    ZStorage storage(root_);
    if (!takeLock(storage)) return 1;

    const QDir history(QDir(root_).filePath(QStringLiteral("history")));
    // --id <id> — одна заметка, и по ней каждая запись строкой: номер, вид,
    // время, класс против предыдущего слепка, размер, первая разошедшаяся
    // строка. Без --id — сводка по хранилищу.
    const QStringList names = id_.isEmpty()
        ? history.entryList({QStringLiteral("*.zm")}, QDir::Files, QDir::Name)
        : QStringList{id_ + QStringLiteral(".zm")};
    const bool detail = !id_.isEmpty();

    // Шапка — строками, без штампов; тело — тем же каноном, что показывает
    // история (diff::bodyOf).
    struct Split {
        QStringList header;   // строки шапки без modified/version
        std::string body;
    };
    const auto split = [](const QByteArray& bytes) {
        Split out;
        std::vector<Piece> blocks;
        NoteHeader header;
        parsePieces(QString::fromUtf8(bytes), blocks, header);
        for (const std::string& line : header.lines()) {
            const QString text = QString::fromStdString(line);
            if (text.startsWith(QStringLiteral("modified:")) ||
                text.startsWith(QStringLiteral("version:")))
                continue;
            out.header.append(text);
        }
        out.body = diff::bodyOf(std::string_view(bytes.constData(), size_t(bytes.size())));
        return out;
    };
    const auto keyOf = [](const QString& line) { return line.section(QLatin1Char(':'), 0, 0).trimmed(); };

    enum Class { Identical, StampsOnly, HeaderOnly, CanonOnly, Real, ClassCount };
    const char* const classNames[ClassCount] = {"identical bytes", "stamps only (modified/version)",
                                                "header only", "canon only", "real change"};
    const char* const kindNames[6] = {"?", "Save", "External", "Restore", "Tombstone", "Amendment"};
    qint64 byClass[ClassCount] = {0, 0, 0, 0, 0};
    qint64 byClassAndKind[ClassCount][6] = {};
    QHash<QString, qint64> headerKeys;   // какие ключи шапки различали пары
    struct NoteScore {
        QString id;
        qint64 empty = 0;
        qint64 pairs = 0;
    };
    QVector<NoteScore> scores;
    qint64 journals = 0;
    qint64 records = 0;
    qint64 bytes = 0;
    qint64 emptyBytes = 0;   // сколько занимают в файлах слепки пустых пар

    for (const QString& name : names) {
        const QString id = name.left(name.size() - 3);
        ZJournal jrn;
        QString error;
        if (!storage.readJournal(id, &jrn, &error)) {
            std::fprintf(stderr, "%s: %s\n", name.toUtf8().constData(), error.toUtf8().constData());
            continue;
        }
        ++journals;
        records += jrn.size();
        bytes += QFileInfo(storage.journalPath(id)).size();
        NoteScore score;
        score.id = id;
        QByteArray prev;
        Split prevSplit;
        bool havePrev = false;
        for (int i = 0; i < jrn.size(); ++i) {
            const ZJournal::Record& entry = jrn.at(i);
            if (!entry.hasSnapshot() || jrn.isVoided(i) || jrn.isDamaged(i)) continue;
            QByteArray snap;
            if (!storage.journalSnapshot(id, i, &snap, &error)) {
                std::fprintf(stderr, "%s #%d: %s\n", name.toUtf8().constData(), i,
                             error.toUtf8().constData());
                break;
            }
            Split now = split(snap);
            if (detail && !havePrev)
                std::printf("  #%-3d %-9s %s  %-32s %7lld b\n", i,
                            kindNames[qBound(0, int(entry.kind()), 5)],
                            QDateTime::fromMSecsSinceEpoch(entry.time(), Qt::UTC)
                                .toString(Qt::ISODate)
                                .toUtf8()
                                .constData(),
                            "(first)", (long long)entry.plainSize());
            if (havePrev) {
                Class cls = Real;
                if (snap == prev) {
                    cls = Identical;
                } else if (now.body == prevSplit.body) {
                    if (now.header == prevSplit.header) {
                        cls = NoteHeader::sameFileApartFromStamps(prev, snap) ? StampsOnly : CanonOnly;
                    } else {
                        cls = HeaderOnly;
                        QSet<QString> a;
                        QSet<QString> b;
                        for (const QString& line : prevSplit.header) a.insert(line);
                        for (const QString& line : now.header) b.insert(line);
                        for (const QString& line : a)
                            if (!b.contains(line)) ++headerKeys[keyOf(line)];
                        for (const QString& line : b)
                            if (!a.contains(line)) ++headerKeys[keyOf(line)];
                    }
                }
                ++byClass[cls];
                ++byClassAndKind[cls][qBound(0, int(entry.kind()), 5)];
                if (detail) {
                    QString firstDiff;
                    if (cls != Identical && cls != Real) {
                        const QList<QByteArray> a = prev.split('\n');
                        const QList<QByteArray> b = snap.split('\n');
                        for (int k = 0; k < qMax(a.size(), b.size()); ++k) {
                            if (a.value(k) == b.value(k)) continue;
                            firstDiff = QStringLiteral("line %1: [%2] -> [%3]")
                                            .arg(k + 1)
                                            .arg(QString::fromUtf8(a.value(k)).left(60),
                                                 QString::fromUtf8(b.value(k)).left(60));
                            break;
                        }
                    }
                    std::printf("  #%-3d %-9s %s  %-32s %7lld b  %s\n", i,
                                kindNames[qBound(0, int(entry.kind()), 5)],
                                QDateTime::fromMSecsSinceEpoch(entry.time(), Qt::UTC)
                                    .toString(Qt::ISODate)
                                    .toUtf8()
                                    .constData(),
                                classNames[cls], (long long)entry.plainSize(),
                                firstDiff.toUtf8().constData());
                }
                ++score.pairs;
                if (cls != Real) {
                    ++score.empty;
                    emptyBytes += entry.packedSize();
                }
            }
            prev = snap;
            prevSplit = std::move(now);
            havePrev = true;
        }
        if (score.pairs > 0) scores.append(score);
    }

    std::printf("journals %lld, records %lld, %.1f MB on disk\n", (long long)journals,
                (long long)records, double(bytes) / 1048576.0);
    std::printf("adjacent snapshot pairs by class (and by kind of the newer record):\n");
    qint64 pairs = 0;
    for (int c = 0; c < ClassCount; ++c) pairs += byClass[c];
    for (int c = 0; c < ClassCount; ++c) {
        std::printf("  %-32s %6lld", classNames[c], (long long)byClass[c]);
        for (int k = 1; k < 6; ++k)
            if (byClassAndKind[c][k] > 0)
                std::printf("  %s %lld", kindNames[k], (long long)byClassAndKind[c][k]);
        std::printf("\n");
    }
    std::printf("  %-32s %6lld\n", "total", (long long)pairs);
    std::printf("empty pairs hold %.1f MB packed\n", double(emptyBytes) / 1048576.0);
    if (!headerKeys.isEmpty()) {
        std::printf("header keys that differed in header-only pairs:\n");
        for (auto it = headerKeys.cbegin(); it != headerKeys.cend(); ++it)
            std::printf("  %s: %lld\n", it.key().toUtf8().constData(), (long long)it.value());
    }
    std::sort(scores.begin(), scores.end(),
              [](const NoteScore& a, const NoteScore& b) { return a.empty > b.empty; });
    std::printf("notes with most empty pairs (empty/pairs):\n");
    int shown = 0;
    for (const NoteScore& s : scores) {
        if (s.empty == 0 || shown++ >= 15) break;
        std::printf("  %s  %lld/%lld  %s\n", s.id.toUtf8().constData(), (long long)s.empty,
                    (long long)s.pairs,
                    storage.titleOf(s.id).toUtf8().constData());
    }
    return 0;
}

int StoreCli::cmdHistory() {
    if (positional_ == QStringLiteral("audit")) return cmdHistoryAudit();
    if (positional_ != QStringLiteral("compress") || positional2_.isEmpty()) return usage();
    // Цель — id или путь к файлу заметки; разбирает хранилище (ZStorage::locate).
    const ZStorage::Target target = ZStorage::locate(positional2_, root_);
    if (target.isEmpty()) {
        std::fprintf(stderr, "no store given: need --root or a path to .md\n");
        return 1;
    }
    const QString& noteId = target.id;

    // Тот же межпроцессный замок, что у thin: пока открыта программа,
    // журналы правит она.
    ZStorage storage(target.root);
    if (!takeLock(storage)) return 1;
    ZStorage::CompressReport report;
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
int StoreCli::cmdRecompress() {
    if (root_.isEmpty()) return usage();
    RecompressOptions options;
    options.root = root_;
    options.id = id_;
    options.dryRun = dryRun_;
    bool bad = false;
    if (!maxSize_.isEmpty()) options.limits.maxSize = maxSize_.toInt(&bad), bad = !bad;
    if (!bad && !quality_.isEmpty()) options.limits.quality = quality_.toInt(&bad), bad = !bad;
    if (bad) {
        std::fprintf(stderr, "bad number in options\n");
        return 1;
    }

    RecompressReport report;
    const bool ok = recompressStore(options, report);
    for (const QString& line : report.lines)
        std::printf("%s\n", line.toUtf8().constData());
    for (const QString& p : report.problems)
        std::fprintf(stderr, "%s\n", p.toUtf8().constData());
    if (report.examined > 0) {
        std::printf("\nexamined %d, rewritten %d, left as is %d, failed %d\n", report.examined,
                    report.rewritten, report.untouched, report.failed);
        std::printf("was %.1f MB, now %.1f MB\n", double(report.bytesBefore) / 1048576.0,
                    double(report.bytesAfter) / 1048576.0);
        if (dryRun_) std::printf("(dry run, nothing written)\n");
    }
    return ok ? 0 : 1;
}

int StoreCli::cmdVerify() {
    if (root_.isEmpty()) return usage();
    ZStorage::Report report;
    const bool ok = ZStorage(root_).verify(report);
    printLines(report);
    return ok ? 0 : 1;
}

}  // namespace zametti
