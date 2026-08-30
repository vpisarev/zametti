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

// После слияния модель окна — сам ZStorageManager; псевдоним оставлен ради
// прежнего языка сценариев.
using Model = ZStorageManager;

const Keyfile::KdfParams kTiny{1, 1 << 20};
constexpr char kCollection[] = "zametti-live-scen";

std::string s(const QString& q) { return q.toStdString(); }

struct Live {
    QString url;      // база, как вводит человек: https://host/webdav
    QString user;
    QString password;
};

// Эмуляция act() окна: переспросы отвечаются заданной кнопкой, работы
// исполняются, цепочка (Reset заказал Check → переспрос → работа) идёт до
// конца. Ограничитель кругов — от вечного цикла.
Model::Reaction execute(Model& model, StoreJobRunner& runner, zt::FakeSecrets& secrets,
                        const Model::Reaction& reaction);

// Выбрать строку по корню — как это делает человек, тыкая в список.
void selectRoot(Model& model, ZStorageManager& stores, const QString& root) {
    const QString key = ZStorageManager::canonicalRoot(root);
    for (int i = 0; i < stores.size(); ++i)
        if (stores.stores().at(i).root == key) {
            model.select(i);
            return;
        }
    model.select(0);
}

Model::Reaction actAll(Model& model, StoreJobRunner& runner, zt::FakeSecrets& secrets,
                       Model::Reaction reaction, int answer) {
    for (int round = 0; round < 6; ++round) {
        if (reaction.question.kind != Model::Question::Kind::None) {
            reaction = model.answered(reaction.question.kind, answer);
            continue;
        }
        if (reaction.job.kind != Model::Job::Kind::None) {
            reaction = execute(model, runner, secrets, reaction);
            continue;
        }
        break;
    }
    return reaction;
}

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
    // РЕАЛИСТИЧНЫЙ ОБЪЁМ (урок 30.08: замер на двух заметках — не замер).
    // У владельца ~285 журналов и ~13 вложений; здесь 120 заметок с текстом и
    // 3 вложения по ~150 КБ — достаточно, чтобы GET-шторм не спрятался.
    constexpr int kNotes = 120;
    constexpr int kAttachments = 3;

    zt::MiniStore home;
    const QString localA = home.root() + QStringLiteral("/машина-А");
    QString err;
    ZT_TRUE("хранилище A завелось", ZStorage(localA).init(&err));
    QString noteId;
    {
        ZStorage a(localA);
        a.reload();
        ZT_TRUE("корень завёлся", !a.ensureRootNote(&err).isEmpty());
        QStringList ids;
        for (int i = 0; i < kNotes; ++i) {
            const QString id = a.createNote(QString(), false, &err);
            ZT_TRUE(("заметка завелась: " + err.toStdString()).c_str(), !id.isEmpty());
            if (id.isEmpty()) return;
            ids.append(id);
            QFile note(localA + QStringLiteral("/") + id + QStringLiteral(".md"));
            ZT_TRUE("заметка дописалась", note.open(QIODevice::Append));
            note.write(QStringLiteral("\n\nживой прогон, заметка №%1\n").arg(i).toUtf8());
            note.write(QByteArray(1200, 't'));
            note.close();
        }
        noteId = ids.first();
        for (int i = 0; i < kAttachments; ++i) {
            QFile pic(localA + QStringLiteral("/") + ids.at(i) +
                      QStringLiteral(".webp"));
            ZT_TRUE("вложение записалось", pic.open(QIODevice::WriteOnly));
            pic.write(QByteArray(150 * 1024, char('a' + i)));
            pic.close();
        }
    }
    auto secrets = std::make_shared<zt::FakeSecrets>();
    ZStorageManager stores(secrets);
    StoreJobRunner runner(kTiny);
    const QString password = QStringLiteral("живой-пароль-сценариев");

    // ==== 0: хранилище + пустая облачная папка + пароль + Open =============
    {
        stores.beginSession();
        Model& model = stores;
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
        ZT_TRUE("уехали ВСЕ журналы", up.pushedWhole >= kNotes);
        ZT_TRUE("уехали вложения", up.attachmentsUp >= kAttachments);
        std::printf("[живое] сценарий 0: заливка %d журналов + %d вложений, %.1f с\n",
                    up.pushedWhole, up.attachmentsUp, ms / 1000.0);

        // «Ещё раз на облако — мгновенно, 1–3 секунды» — ЗАКОН ВЛАДЕЛЬЦА, и
        // меряется он на этом объёме. GET-шторм (Apache молчит про etag на
        // PUT → «файл отличается» → перечитать всё) чинится сверкой размеров
        // по листингу; без починки здесь были бы минуты и сотни запросов.
        ZStorage::SyncReport quiet;
        ZT_TRUE("повторный прогон прошёл", runSync(localA, *secrets, &quiet, &ms, &err));
        ZT_TRUE("повторный прогон ничего не возит",
                quiet.takenWhole == 0 && quiet.pushedWhole == 0 &&
                    quiet.attachmentsUp == 0 && quiet.attachmentsDown == 0);
        ZT_TRUE(("без GET-шторма: " + std::to_string(quiet.traffic.requests) +
                 " запросов")
                    .c_str(),
                quiet.traffic.requests <= 6);
        ZT_TRUE(("закон 1–3 с: " + std::to_string(ms) + " мс").c_str(), ms <= 3000);
        std::printf("[живое] сценарий 0: повтор %lld мс, %lld запросов, %lld Б вниз\n",
                    ms, quiet.traffic.requests, quiet.traffic.bytesDown);
        ZT_TRUE("третий прогон прошёл", runSync(localA, *secrets, &quiet, &ms, &err));
        ZT_TRUE("и совсем тих", quiet.traffic.requests <= 4);
        std::printf("[живое] сценарий 0: тихий прогон %lld мс, %lld запросов\n", ms,
                    quiet.traffic.requests);
    }

    // ==== 1: «перезапуск программы» ========================================
    {
        ZStorageManager restored(secrets);
        restored.storesFromJson(stores.storesToJson());   // выход → запуск
        restored.beginSession();
        Model& model = restored;
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

        // «Закрыл программу, открыл, нажал синхронизацию»: свежий экземпляр
        // хранилища = новый процесс, бухгалтерия — с диска.
        ZStorage::SyncReport quiet;
        qint64 ms = 0;
        ZT_TRUE("прогон после «перезапуска» прошёл",
                runSync(localA, *secrets, &quiet, &ms, &err));
        ZT_TRUE("и ничего не возит", quiet.takenWhole == 0 && quiet.pushedWhole == 0);
        ZT_TRUE("и дёшев", quiet.traffic.requests <= 4);
        ZT_TRUE(("закон 1–3 с и после перезапуска: " + std::to_string(ms) + " мс")
                    .c_str(),
                ms <= 3000);
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
        stores.beginSession();
        Model& model = stores;
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
        stores.beginSession();
        Model& model = stores;
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
        ZT_TRUE("привёз ВСЕ журналы", down.takenWhole >= kNotes);
        ZT_TRUE("и вложения", down.attachmentsDown >= kAttachments);
        std::printf("[живое] сценарий 4: скачано %d журналов + %d вложений, %.1f с\n",
                    down.takenWhole, down.attachmentsDown, ms / 1000.0);
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

    // ==== 5: Reset cloud — все три дороги ==================================
    {
        // Прямой доступ к блобу — свидетель «блобы не тронуты/заменены».
        WebDavCloud::Config direct;
        QString base = live.url;
        if (!base.endsWith(QLatin1Char('/'))) base += QLatin1Char('/');
        direct.base = QUrl(base + QLatin1String(kCollection) + QLatin1Char('/'));
        direct.user = live.user;
        direct.password = live.password;
        WebDavCloud witness(direct);
        const QString journalName = noteId + QStringLiteral(".zm");
        const auto blobBytes = [&](const QString& name) {
            QByteArray bytes;
            witness.get(name, &bytes, nullptr, nullptr);
            return bytes;
        };
        const QString storeId = ZStorage(localD).identity().storeId();
        const ZStorage::Config probeCfg = ZStorage(localD).cloudConfig();

        // --- дорога 1: ключ в связке → [Change password], ноль стираний ----
        {
            stores.beginSession();
            Model& model = stores;
            selectRoot(model, stores, localD);
            const QByteArray journalBefore = blobBytes(journalName);
            const QByteArray keyfileBefore = blobBytes(QStringLiteral("keyfile"));
            ZT_TRUE("свидетель видит журнал", !journalBefore.isEmpty());
            model.edit(Model::FieldId::EncryptionPassword, QStringLiteral("пароль-2"));
            // Просто resetPressed: Check он делает сам, переспрос — итогом.
            Model::Reaction ask = model.resetPressed();
            ZT_TRUE("Reset сам заказывает проверку",
                    ask.job.kind == Model::Job::Kind::Check);
            ask = execute(model, runner, *secrets, ask);
            ZT_TRUE("переспрос пришёл",
                    ask.question.kind == Model::Question::Kind::ResetCloud);
            ZT_TRUE("первая дорога — смена без стирания",
                    ask.question.choices.value(0).contains(QStringLiteral("Change")));
            QElapsedTimer clock;
            clock.start();
            actAll(model, runner, *secrets, model.answered(ask.question.kind, 0), -1);
            const qint64 ms = clock.elapsed();
            ZT_TRUE(("пароль сменён: " + s(model.snapshot().message.text)).c_str(),
                    model.snapshot().message.text.contains(QStringLiteral("changed")));
            ZT_TRUE(("секунды, не минуты: " + std::to_string(ms) + " мс").c_str(),
                    ms <= 5000);
            ZT_TRUE("журнал не перезаливался",
                    blobBytes(journalName) == journalBefore);
            ZT_TRUE("конверт заменён",
                    blobBytes(QStringLiteral("keyfile")) != keyfileBefore);
            ZT_TRUE("старый пароль конверт не открывает",
                    !ZStorage::probeCloud(probeCfg, live.password,
                                          QStringLiteral("живой-пароль-сценариев"),
                                          nullptr, &err));
            ZT_TRUE("новый открывает",
                    ZStorage::probeCloud(probeCfg, live.password,
                                         QStringLiteral("пароль-2"), nullptr, &err));
            std::printf("[живое] сценарий 5а: смена пароля %lld мс, блобы целы\n", ms);
        }

        // --- дорога 2: ключа нет → [Erase and reset password] --------------
        {
            secrets->clearKey(storeId, nullptr);
            secrets->clearEncryptionPassword(storeId, nullptr);
            stores.refresh(localD);
            stores.beginSession();
            Model& model = stores;
            selectRoot(model, stores, localD);
            Model::Reaction ask = model.resetPressed();
            ask = execute(model, runner, *secrets, ask);
            ZT_TRUE("без ключа первая дорога — стирание с новым паролем",
                    ask.question.choices.value(0).contains(
                        QStringLiteral("reset password")));
            ZT_TRUE("текст называет полный адрес",
                    ask.question.text.contains(QLatin1String(kCollection)));
            model.edit(Model::FieldId::EncryptionPassword, QStringLiteral("пароль-3"));
            model.edit(Model::FieldId::Repeat, QStringLiteral("пароль-3"));
            actAll(model, runner, *secrets,
                   model.answered(Model::Question::Kind::ResetCloud, 0), -1);
            ZT_TRUE(("стёрто и запечатано: " + s(model.snapshot().message.text)).c_str(),
                    model.snapshot().message.text.contains(QStringLiteral("Erased")));
            ZT_TRUE("новый конверт на месте",
                    !blobBytes(QStringLiteral("keyfile")).isEmpty());
            ZT_TRUE("журналов больше нет (стёрты)", blobBytes(journalName).isEmpty());
            // Перезаливку ведёт прогон — как после Open.
            ZStorage::SyncReport up;
            qint64 ms = 0;
            ZT_TRUE("перезаливка прошла", runSync(localD, *secrets, &up, &ms, &err));
            ZT_TRUE("уехало всё заново", up.pushedWhole >= kNotes);
            std::printf("[живое] сценарий 5б: стирание+перезаливка %d блобов, %.1f с\n",
                        up.pushedWhole, ms / 1000.0);
            ZStorage::SyncReport quiet;
            ZT_TRUE("после перезаливки тихо", runSync(localD, *secrets, &quiet, &ms, &err));
            ZT_TRUE("и без шторма", quiet.traffic.requests <= 6 &&
                        quiet.takenWhole == 0 && quiet.pushedWhole == 0);
            std::printf("[живое] сценарий 5б: тихий прогон %lld мс, %lld запросов\n",
                        ms, quiet.traffic.requests);
        }

        // --- дорога 3: [Erase and disconnect] ------------------------------
        {
            stores.beginSession();
            Model& model = stores;
            selectRoot(model, stores, localD);
            Model::Reaction ask = model.resetPressed();
            ask = execute(model, runner, *secrets, ask);
            ZT_TRUE("вторая кнопка — стереть и отвязаться",
                    ask.question.choices.at(1).contains(QStringLiteral("disconnect")));
            actAll(model, runner, *secrets,
                   model.answered(Model::Question::Kind::ResetCloud, 1), -1);
            ZT_TRUE(("отвязано: " + s(model.snapshot().message.text)).c_str(),
                    model.snapshot().message.text.contains(
                        QStringLiteral("Disconnected")));
            ZT_TRUE("адрес забыт из строки",
                    !stores.storeFor(localD).hasCloudAddress());
            ZT_TRUE("cloud.json забыт",
                    !ZStorage(localD).cloudConfig().hasCloudAddress());
            ZT_TRUE("секреты забыты", !secrets->keys_.contains(storeId) &&
                        !secrets->passwords_.contains(storeId));
            QVector<CloudStore::Entry> listing;
            QString why;
            const bool listed = witness.list(&listing, &why);
            ZT_TRUE("облака на сервере больше нет",
                    !listed || listing.isEmpty());
            std::printf("[живое] сценарий 5в: отвязка, облако снесено одним DELETE\n");
        }
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
