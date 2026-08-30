// ЖИВЫЕ СЦЕНАРИИ ОКНА ХРАНИЛИЩ против НАСТОЯЩЕГО облака (заказ владельца,
// 30.08.2026) — пять сценариев одним путешествием одного хранилища:
//
//   0. хранилище + пустая папка в облаке + пароль + Open → заливка; повторный
//      прогон — мгновенный (листинг и ничего больше);
//   1. «перезапуск»: всё введённое на месте, у паролей кружочки, глаз
//      показывает их без ввода; прогон снова мгновенный;
//   2. правка заметки + «выход из программы» → правка уезжает в облако
//      (push-only, как pushOnExit);
//   3. локальную папку перенесли: путь красный, Browse жив, Open погашен;
//      выбрали новое место — строка ПЕРЕЕХАЛА (без сироты), прогон мгновенный;
//   4. «папку случайно удалили»: пустая папка + тот же адрес + пароль →
//      Create → голова приехала, полный прогон привозит всё, включая правку
//      из сценария 2; повторный прогон мгновенный.
//
// БЕЗ РЕКВИЗИТОВ НАБОР ГРОМКО ПРОПУСКАЕТСЯ (сервер владельца в общем прогоне
// не гоняют):
//
//   ZAMETTI_LIVE_WEBDAV_URL='https://host/webdav' \
//   ZAMETTI_LIVE_WEBDAV_USER='u…' ZAMETTI_LIVE_WEBDAV_PASSWORD='…' \
//   zametti-tests --gtest_filter='StoreLive.*'
//
// Работает в СЛУЖЕБНОЙ коллекции zametti-live-scen (сносится в начале и в
// конце нашим же removeTree); чужие папки сервера не трогаются. Виджетов
// здесь нет: проводку модель↔окно держит store_manager_dialog_test, здесь —
// модель+работы+движок, ровно те, что зовёт окно.

#include "store_job_runner.h"
#include "store_manager_model.h"

#include "webdav_cloud.h"
#include "zstorage.h"
#include "zstorage_manager.h"

#include "fake_secrets.h"
#include "mini_store.h"
#include "test_util.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QUrl>

#include <memory>
#include <string>
#include <vector>

using namespace zametti;

namespace {

using Model = StoreManagerModel;

const Keyfile::KdfParams kTiny{1, 1 << 20};
constexpr char kCollection[] = "zametti-live-scen";

std::string s(const QString& q) { return q.toStdString(); }

struct Live {
    QString url;      // база, как вводит человек: https://host/webdav
    QString user;
    QString password;
};

// Эмуляция runJob окна БЕЗ окна: разрешить «взять из связки», исполнить.
Model::Reaction execute(Model& model, StoreJobRunner& runner, zt::FakeSecrets& secrets,
                        const Model::Reaction& reaction) {
    if (reaction.job.kind == Model::Job::Kind::None) return reaction;
    Model::Job job = reaction.job;
    const QString root = ZStorageManager::canonicalRoot(job.root);
    if (ZStorage::inspect(root) == ZStorage::DirKind::Store) {
        const QString id = ZStorage(root).identity().storeId();
        if (!id.isEmpty() && job.serverPasswordFromKeyring && !job.cfg.cloudUrl.isEmpty())
            job.serverPassword = secrets.serverPassword(id, nullptr);
    }
    job.serverPasswordFromKeyring = false;
    job.encryptionFromKeyring = false;
    const Model::Outcome outcome = runner.run(job, secrets);
    return model.jobFinished(reaction.job.kind, outcome);
}

// «Обвязка Open»: подключиться из cloud.json и прогнать движок; вернуть
// отчёт. Требование владельца «мгновенно, 1–3 секунды» меряется числом
// запросов (сеть плавает, листинг — нет) и печатается временем.
bool runSync(const QString& root, zt::FakeSecrets& secrets, ZStorage::SyncReport* report,
             qint64* ms, QString* error, ZStorage::SyncOptions::Mode mode =
                                             ZStorage::SyncOptions::Full) {
    ZStorage storage(root);
    storage.reload();
    if (!storage.useLastCloud(secrets, error)) return false;
    ZStorage::SyncOptions options;
    options.mode = mode;
    QElapsedTimer clock;
    clock.start();
    const bool ok = storage.sync(options, report, error);
    if (ms != nullptr) *ms = clock.elapsed();
    return ok;
}

void wipeCollection(const Live& live) {
    WebDavCloud::Config config;
    QString base = live.url;
    if (!base.endsWith(QLatin1Char('/'))) base += QLatin1Char('/');
    config.base = QUrl(base + QLatin1String(kCollection) + QLatin1Char('/'));
    config.user = live.user;
    config.password = live.password;
    WebDavCloud cloud(config);
    cloud.removeTree(nullptr);
}

void runScenarios(const Live& live) {
    zt::MiniStore home;
    const QString localA = home.root() + QStringLiteral("/машина-А");
    QString err;
    ZT_TRUE("хранилище A завелось", ZStorage(localA).init(&err));
    QString noteId;
    {
        ZStorage a(localA);
        a.reload();
        ZT_TRUE("корень завёлся", !a.ensureRootNote(&err).isEmpty());
        noteId = a.createNote(QString(), false, &err);
        ZT_TRUE("заметка завелась", !noteId.isEmpty());
    }
    auto secrets = std::make_shared<zt::FakeSecrets>();
    ZStorageManager stores(secrets);
    StoreJobRunner runner(kTiny);
    const QString password = QStringLiteral("живой-пароль-сценариев");

    // ==== 0: хранилище + пустая облачная папка + пароль + Open =============
    {
        Model model(stores, QString());
        model.addFolder(localA);
        model.edit(Model::FieldId::Server, live.url);
        model.edit(Model::FieldId::ServerDir, QLatin1String(kCollection));
        model.edit(Model::FieldId::Login, live.user);
        model.edit(Model::FieldId::ServerPassword, live.password);
        model.edit(Model::FieldId::EncryptionPassword, password);

        // Первый Open по неизвестному адресу СМОТРИТ (защита от опечатки,
        // запечатывающей навсегда) и просит повтор; второй — запечатывает.
        Model::Reaction first = execute(model, runner, *secrets, model.openPressed());
        ZT_TRUE("после разведки просят повтор", model.snapshot().repeatVisible);
        ZT_TRUE("облако увидено пустым",
                model.snapshot().cloud.text == QStringLiteral("Cloud: empty"));
        Q_UNUSED(first);
        model.edit(Model::FieldId::Repeat, password);
        Model::Reaction opened = execute(model, runner, *secrets, model.openPressed());
        ZT_TRUE(("Open доделал и открыл: " + s(model.snapshot().message.text)).c_str(),
                !opened.switchToRoot.isEmpty());

        ZStorage::SyncReport up;
        qint64 ms = 0;
        ZT_TRUE(("заливка прошла: " + err.toStdString()).c_str(),
                runSync(localA, *secrets, &up, &ms, &err));
        ZT_TRUE("что-то уехало", up.pushedWhole >= 2);
        std::printf("[живое] сценарий 0: заливка %d блобов, %lld мс\n",
                    up.pushedWhole, ms);

        // «Ещё раз на облако — мгновенно»: закон владельца меряется временем.
        // Apache не возвращает etag на PUT, потому первый повтор ещё доедает
        // метки из листинга (без переписываний), совсем тихим станет третий.
        ZStorage::SyncReport quiet;
        ZT_TRUE("повторный прогон прошёл", runSync(localA, *secrets, &quiet, &ms, &err));
        ZT_TRUE("повторный прогон ничего не возит",
                quiet.takenWhole == 0 && quiet.pushedWhole == 0 &&
                    quiet.attachmentsUp == 0 && quiet.attachmentsDown == 0);
        ZT_TRUE(("и мгновенен по закону владельца (1–3 с): " + std::to_string(ms) +
                 " мс")
                    .c_str(),
                ms <= 3000);
        std::printf("[живое] сценарий 0: повтор %lld мс, %lld запросов\n", ms,
                    quiet.traffic.requests);
        ZT_TRUE("третий прогон прошёл", runSync(localA, *secrets, &quiet, &ms, &err));
        ZT_TRUE("и совсем тих", quiet.traffic.requests <= 4);
        std::printf("[живое] сценарий 0: тихий прогон %lld мс, %lld запросов\n", ms,
                    quiet.traffic.requests);
    }

    // ==== 1: «перезапуск программы» ========================================
    {
        ZStorageManager restored(secrets);
        restored.storesFromJson(stores.storesToJson());   // выход → запуск
        Model model(restored, QString());
        model.select(0);
        const Model::Snapshot snap = model.snapshot();
        ZT_EQ("сервер на месте", s(live.url), s(snap.server.text));
        ZT_EQ("папка на месте", std::string(kCollection), s(snap.serverDir.text));
        ZT_EQ("логин на месте", s(live.user), s(snap.login.text));
        ZT_TRUE("кружочки у пароля сервера", snap.serverPassword.stub);
        ZT_TRUE("кружочки у пароля шифрования", snap.encryptionPassword.stub);
        ZT_TRUE("оба глаза живы",
                snap.serverPassword.eyeEnabled && snap.encryptionPassword.eyeEnabled);
        // «Нажал на глаз — показался без ввода»: глаз читает связку.
        const QString id = ZStorage(localA).identity().storeId();
        ZT_EQ("глаз пароля сервера", s(live.password),
              s(secrets->serverPassword(id, nullptr)));
        ZT_EQ("глаз пароля шифрования", s(password),
              s(secrets->encryptionPassword(id, nullptr)));

        ZStorage::SyncReport quiet;
        qint64 ms = 0;
        ZT_TRUE("прогон после «перезапуска» прошёл",
                runSync(localA, *secrets, &quiet, &ms, &err));
        ZT_TRUE("и ничего не возит", quiet.takenWhole == 0 && quiet.pushedWhole == 0);
        ZT_TRUE("и дёшев", quiet.traffic.requests <= 4);
        std::printf("[живое] сценарий 1: тихий прогон %lld мс, %lld запросов\n", ms,
                    quiet.traffic.requests);
    }

    // ==== 2: правка заметки + «выход» ======================================
    const QByteArray mark("\n\nправка с машины А перед выходом\n");
    {
        QFile note(localA + QStringLiteral("/") + noteId + QStringLiteral(".md"));
        ZT_TRUE("заметка открылась", note.open(QIODevice::Append));
        note.write(mark);
        note.close();
        ZStorage::SyncReport push;
        qint64 ms = 0;
        ZT_TRUE("выходной push прошёл",
                runSync(localA, *secrets, &push, &ms, &err,
                        ZStorage::SyncOptions::PushOnly));
        ZT_TRUE("правка уехала", push.pushedWhole >= 1);
        std::printf("[живое] сценарий 2: выходной push %d блобов, %lld мс\n",
                    push.pushedWhole, ms);
    }

    // ==== 3: локальная папка переехала =====================================
    const QString localB = home.root() + QStringLiteral("/машина-А-переехала");
    ZT_TRUE("папка переехала", QDir().rename(localA, localB));
    {
        Model model(stores, QString());
        model.select(0);
        Model::Snapshot snap = model.snapshot();
        ZT_TRUE("путь горит красным", snap.folderMissing);
        ZT_TRUE("строка фактов — missing", snap.local.alarm &&
                    snap.local.text.contains(QStringLiteral("missing")));
        ZT_TRUE("Browse жив", snap.browse.enabled);
        ZT_TRUE("Open погашен", !snap.open.enabled);

        model.edit(Model::FieldId::Folder, localB);   // жест Browse
        snap = model.snapshot();
        ZT_TRUE("новая папка подхватилась", !snap.folderMissing && snap.open.enabled);
        const Model::Reaction go = execute(model, runner, *secrets, model.openPressed());
        ZT_EQ("Open переключает на новое место", s(QDir::cleanPath(localB)),
              s(QDir::cleanPath(go.switchToRoot)));
        ZT_EQ("строка ПЕРЕЕХАЛА, сироты нет", std::string("1"),
              std::to_string(stores.size()));
        ZT_TRUE("облако при строке",
                stores.storeFor(localB).hasCloudAddress());

        // Бухгалтерия синка ключуется ПУТЁМ (нарочно: две копии одного
        // хранилища не должны делить кэш) — после переезда первый прогон один
        // раз сверяет хеши по листингу, не скачивая содержимое журналов
        // целиком и ничего не переписывая; тихим становится следующий.
        ZStorage::SyncReport heal;
        qint64 ms = 0;
        ZT_TRUE("прогон на новом месте прошёл",
                runSync(localB, *secrets, &heal, &ms, &err));
        ZT_TRUE("cloud connection на месте: ничего не переписано",
                heal.takenWhole == 0 && heal.pushedWhole == 0 &&
                    heal.attachmentsUp == 0 && heal.attachmentsDown == 0);
        std::printf("[живое] сценарий 3: сверка после переезда %lld мс, %lld запросов\n",
                    ms, heal.traffic.requests);
        ZStorage::SyncReport quiet;
        ZT_TRUE("второй прогон прошёл", runSync(localB, *secrets, &quiet, &ms, &err));
        ZT_TRUE("и мгновенен", quiet.takenWhole == 0 && quiet.pushedWhole == 0 &&
                    quiet.traffic.requests <= 4);
        std::printf("[живое] сценарий 3: тихий прогон %lld мс, %lld запросов\n", ms,
                    quiet.traffic.requests);
    }

    // ==== 4: «папку случайно удалили» → новая пустая + то же облако ========
    const QString hidden = home.root() + QStringLiteral("/спрятано-не-удалено");
    ZT_TRUE("папка спрятана (НЕ удалена)", QDir().rename(localB, hidden));
    const QString localD = home.root() + QStringLiteral("/машина-Б");
    QDir().mkpath(localD);
    {
        Model model(stores, QString());
        model.select(0);
        model.edit(Model::FieldId::Folder, localD);   // жест Browse
        // Пароли — заново, ОБА: у свежей пустой папки нет storeId, и достать
        // их из связки не по чему (кружочки живут только у хранилищ). Так же
        // их введёт и человек в этом сценарии.
        model.edit(Model::FieldId::ServerPassword, live.password);
        model.edit(Model::FieldId::EncryptionPassword, password);
        Model::Snapshot snap = model.snapshot();
        ZT_TRUE("пустая папка + облако: Open жив", snap.open.enabled);
        const Model::Reaction ask = model.openPressed();
        ZT_TRUE("создание переспрашивается",
                ask.question.kind == Model::Question::Kind::Create);
        const Model::Reaction made =
            execute(model, runner, *secrets,
                    model.answered(Model::Question::Kind::Create, 0));
        Q_UNUSED(made);
        ZT_TRUE(("голова приехала: " + s(model.snapshot().message.text)).c_str(),
                ZStorage::inspect(localD) == ZStorage::DirKind::Store);

        ZStorage::SyncReport down;
        qint64 ms = 0;
        ZT_TRUE("полный прогон привёз всё", runSync(localD, *secrets, &down, &ms, &err));
        std::printf("[живое] сценарий 4: скачано %d блобов, %lld мс\n", down.takenWhole,
                    ms);
        QFile note(localD + QStringLiteral("/") + noteId + QStringLiteral(".md"));
        ZT_TRUE("заметка на месте", note.open(QIODevice::ReadOnly));
        ZT_TRUE("недавняя правка внутри", note.readAll().contains(mark.trimmed()));

        ZStorage::SyncReport quiet;
        ZT_TRUE("повторный прогон прошёл", runSync(localD, *secrets, &quiet, &ms, &err));
        ZT_TRUE("и мгновенен", quiet.takenWhole == 0 && quiet.pushedWhole == 0 &&
                    quiet.traffic.requests <= 4);
        std::printf("[живое] сценарий 4: тихий прогон %lld мс, %lld запросов\n", ms,
                    quiet.traffic.requests);
    }
}

}  // namespace

TEST(StoreLive, Scenarios) {
    Live live;
    live.url = qEnvironmentVariable("ZAMETTI_LIVE_WEBDAV_URL");
    live.user = qEnvironmentVariable("ZAMETTI_LIVE_WEBDAV_USER");
    live.password = qEnvironmentVariable("ZAMETTI_LIVE_WEBDAV_PASSWORD");
    if (live.url.isEmpty() || live.user.isEmpty() || live.password.isEmpty())
        GTEST_SKIP() << "живые сценарии пропущены: нет ZAMETTI_LIVE_WEBDAV_URL/USER/"
                        "PASSWORD (сервер владельца в общий прогон не ходит)";
    wipeCollection(live);
    runScenarios(live);
    wipeCollection(live);
    EXPECT_EQ(0, zt::report("store_live"));
}
