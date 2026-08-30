// ZStorageManager, оконная часть: снимки, жесты и автоматы окна хранилищ —
// БЕЗ виджетов.
//
// Имена случаев — коды клеток матрицы (§1 docs/zametti-store-window-matrix.md):
// буква папки (V хранилище, E пустая, M нет, X чужая) + облако (L- каталог не
// назван, W назван; строчная — пароль сервера: w набран, k в связке, h нет) +
// что видели в облаке (E пусто, V наше, X чужое, ? не смотрели) + пароль
// шифрования (P набран, K ключ в связке, N нет) + o — строка открыта.
// Тест и документ читаются друг через друга.

#include "zstorage.h"
#include "zstorage_manager.h"

#include "fake_secrets.h"
#include "mini_store.h"
#include "test_util.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>

#include <memory>
#include <string>
#include <vector>

using namespace zametti;

namespace {

// После слияния (решение владельца, 30.08.2026) модель окна — сам менеджер;
// псевдоним оставлен, чтобы случаи читались прежним языком матрицы.
using Model = ZStorageManager;
using Known = ZStorageManager::Known;

std::string s(const QString& q) { return q.toStdString(); }

// Хранилище с идентичностью и корнем — «V» матрицы.
QString makeStore(const QString& root) {
    QString err;
    if (!ZStorage(root).init(&err)) return err;
    ZStorage stor(root);
    stor.ensureRootNote(&err);
    return {};
}

void checkZeroAndEmptiedList() {
    // Zero: две живые кнопки, слова вместо молчания (беда G).
    ZStorageManager stores;
    stores.beginSession();
    Model& model = stores;
    Model::Snapshot snap = model.snapshot();
    ZT_TRUE("Zero: «+» жив", snap.add.enabled);
    ZT_TRUE("Zero: Close жив", snap.close.enabled);
    ZT_TRUE("Zero: остальное погашено",
            !snap.remove.enabled && !snap.check.enabled && !snap.reset.enabled &&
                !snap.open.enabled && !snap.browse.enabled);
    ZT_TRUE("Zero: сказано словами",
            snap.message.text.contains(QStringLiteral("press +")));
    ZT_EQ("Zero — код клетки", std::string("Zero"), s(model.cellCode()));

    // Опустевший список: поля не остаются от ушедшей строки.
    zt::MiniStore home;
    const QString root = home.root() + QStringLiteral("/пустая");
    QDir().mkpath(root);
    model.addFolder(root);
    model.edit(Model::FieldId::Server, QStringLiteral("https://host/dav"));
    ZT_TRUE("строка есть", !model.snapshot().rows.isEmpty());
    const Model::Reaction gone =
        model.answered(Model::Question::Kind::Forget, 0);
    ZT_TRUE("строк не осталось", model.snapshot().rows.isEmpty());
    ZT_TRUE("поля вычищены", model.snapshot().folder.isEmpty() &&
                model.snapshot().server.text.isEmpty());
    ZT_TRUE("опустевший список говорит словами",
            model.snapshot().message.text.contains(QStringLiteral("press +")));
    ZT_TRUE("закрывать окно не велено", !gone.close);
}

void checkAddFolder() {
    zt::MiniStore home;
    ZStorageManager stores;
    stores.beginSession();
    Model& model = stores;

    // V: хранилище добавляется и выбирается.
    const QString store = home.root() + QStringLiteral("/хранилище");
    ZT_TRUE("хранилище завелось", makeStore(store).isEmpty());
    model.addFolder(store);
    ZT_EQ("строка одна", std::string("1"), std::to_string(stores.size()));

    // Дубль — перескок со словами, а не вторая строка.
    model.addFolder(store + QStringLiteral("/"));
    ZT_EQ("дубль не добавился", std::string("1"), std::to_string(stores.size()));
    ZT_TRUE("сказано «уже в списке»",
            model.snapshot().message.text.contains(QStringLiteral("Already")));

    // E: пустая папка получает строку (право завести хранилище).
    const QString empty = home.root() + QStringLiteral("/пустая");
    QDir().mkpath(empty);
    model.addFolder(empty);
    ZT_EQ("пустая папка добавилась", std::string("2"), std::to_string(stores.size()));

    // X: чужая папка строки НЕ получает, и сообщение — ПРО ЖЕСТ, а не слово в
    // слово строка фактов (находка обезьяны, 28-й жест первого круга).
    const QString foreign = home.root() + QStringLiteral("/чужая");
    QDir().mkpath(foreign);
    QFile junk(foreign + QStringLiteral("/письмо.txt"));
    ZT_TRUE("чужой файл завёлся", junk.open(QIODevice::WriteOnly));
    junk.close();
    model.addFolder(foreign);
    ZT_EQ("чужая не добавилась", std::string("2"), std::to_string(stores.size()));
    const Model::Snapshot snap = model.snapshot();
    ZT_TRUE("сообщение про жест",
            snap.message.text.contains(QStringLiteral("nothing added")));
    ZT_TRUE("и не повторяет строку фактов", snap.message.text != snap.local.text);
}

void checkButtonsTableA() {
    zt::MiniStore home, cloudHome;
    auto secrets = std::make_shared<zt::FakeSecrets>();
    ZStorageManager stores(secrets);
    const QString store = home.root() + QStringLiteral("/хранилище");
    ZT_TRUE("хранилище завелось", makeStore(store).isEmpty());
    stores.beginSession();
    Model& model = stores;
    model.addFolder(store);

    // VL-N: облака нет — Check и Reset погашены, Open жив.
    {
        const Model::Snapshot snap = model.snapshot();
        ZT_TRUE("VL-: check погашен", !snap.check.enabled);
        ZT_TRUE("VL-: reset погашен", !snap.reset.enabled);
        ZT_TRUE("VL-: open жив", snap.open.enabled);
        ZT_EQ("VL-: подпись Open", std::string("Open"), s(snap.open.label));
        ZT_TRUE("VL-: «−» жив", snap.remove.enabled);
        ZT_EQ("код клетки", std::string("VL-N"), s(model.cellCode()));
    }
    // VWh*: сервер назван, пароля нет нигде (связка сказала «нет») — Check
    // погашен И сказано чего не хватает (беды N, P).
    model.edit(Model::FieldId::Server, QStringLiteral("https://host/dav"));
    // ЖИВОЙ ДЕФОЛТ Cloud dir не только рисуется серым, но и УЕЗЖАЕТ В РАБОТУ:
    // пустое поле = имя локальной папки, а не корень провайдера (живая проба
    // владельца 30.08: Check уходил в /webdav/ и получал не свою папку).
    // База при этом хранится КАК ВВЕДЕНА — склейка только в collectionUrl.
    {
        model.edit(Model::FieldId::ServerPassword, QStringLiteral("временный"));
        const Model::Job job = model.checkPressed().job;
        ZT_EQ("база — как введена", std::string("https://host/dav"),
              s(job.cfg.cloudUrl));
        ZT_EQ("папка — имя локальной", s(QFileInfo(store).fileName()),
              s(job.cfg.cloudServerDir));
        ZT_TRUE("полный адрес собирается из двух полей",
                job.cfg.collectionUrl() ==
                    QStringLiteral("https://host/dav/") + QFileInfo(store).fileName() +
                        QLatin1Char('/'));
        model.edit(Model::FieldId::ServerPassword, QString());
    }
    {
        const Model::Snapshot snap = model.snapshot();
        ZT_TRUE("VWh: check погашен", !snap.check.enabled);
        ZT_TRUE("VWh: сказано про пароль сервера",
                snap.message.text.contains(QStringLiteral("server password")));
    }
    // VWw: пароль набран — Check и Reset живы (пароль ШИФРОВАНИЯ не нужен:
    // сценарий «новая машина с флешкой, пароль забыт»).
    model.edit(Model::FieldId::ServerPassword, QStringLiteral("пароль"));
    {
        const Model::Snapshot snap = model.snapshot();
        ZT_TRUE("VWw: check жив", snap.check.enabled);
        ZT_TRUE("VWw: reset жив БЕЗ пароля шифрования", snap.reset.enabled);
    }
    // Каталог-облако: логин и серверные поля гаснут и говорят это сами; Check
    // жив без пароля сервера.
    const QString cloud = cloudHome.root() + QStringLiteral("/облако");
    model.edit(Model::FieldId::Server, cloud);
    {
        const Model::Snapshot snap = model.snapshot();
        ZT_TRUE("каталог: login погашен", !snap.login.enabled);
        ZT_TRUE("каталог: серверная папка погашена", !snap.serverDir.enabled);
        ZT_TRUE("каталог: check жив", snap.check.enabled);
    }
    // Относительный каталог-облако — кнопки гаснут, сказано почему (урок
    // катастрофы 30.08.2026: «../..» не резолвится по cwd).
    model.edit(Model::FieldId::Server, QStringLiteral("../.."));
    {
        const Model::Snapshot snap = model.snapshot();
        ZT_TRUE("относительный: check погашен", !snap.check.enabled);
        ZT_TRUE("относительный: сказано про абсолютный путь",
                snap.message.text.contains(QStringLiteral("absolute")));
        ZT_TRUE("относительный: строка Cloud красная", snap.cloud.alarm);
    }
    model.edit(Model::FieldId::Server, cloud);

    // X + названное облако: Check погашен (беда K).
    const QString foreign = home.root() + QStringLiteral("/чужая");
    QDir().mkpath(foreign);
    QFile junk(foreign + QStringLiteral("/бумага.txt"));
    ZT_TRUE("чужой файл завёлся", junk.open(QIODevice::WriteOnly));
    junk.close();
    model.edit(Model::FieldId::Folder, foreign);
    {
        const Model::Snapshot snap = model.snapshot();
        ZT_TRUE("XW: check погашен", !snap.check.enabled);
        ZT_TRUE("XW: open погашен (П8)", !snap.open.enabled);
        ZT_TRUE("XW: строка фактов красная и словами владельца",
                snap.local.alarm &&
                    snap.local.text ==
                        QStringLiteral("Local: not a valid storage nor empty dir"));
    }
}

void checkOpenRow() {
    // V****o: открытая строка — Open жив и просто закрывает окно (беда Q),
    // папка под замком (browse погашен, folderFrozen), «−» жив (п.14).
    zt::MiniStore home;
    ZStorageManager stores;
    const QString store = home.root() + QStringLiteral("/открытое");
    ZT_TRUE("хранилище завелось", makeStore(store).isEmpty());
    ZStorage::Config row;
    row.root = store;
    stores.remember(row);
    stores.setOpenRoot(store);
    stores.beginSession();
    Model& model = stores;
    const Model::Snapshot snap = model.snapshot();
    ZT_TRUE("строка открытого выбрана", snap.rows.first().open);
    ZT_TRUE("open жив", snap.open.enabled);
    ZT_TRUE("папка под замком", snap.folderFrozen && !snap.browse.enabled);
    ZT_TRUE("«−» жив и у открытого", snap.remove.enabled);
    const Model::Reaction go = model.openPressed();
    ZT_TRUE("open закрывает окно с переключением",
            go.close && !go.switchToRoot.isEmpty());
    // «−» по открытой строке: подтверждение называет закрытие, ответ Remove
    // отцепляет немедленно.
    const Model::Reaction ask = model.forgetPressed();
    ZT_TRUE("цена названа: хранилище закроется",
            ask.question.detail.contains(QStringLiteral("closed")));
    const Model::Reaction gone = model.answered(Model::Question::Kind::Forget, 0);
    ZT_TRUE("отцепить немедленно", gone.close);
}

void checkFactLines() {
    // Таблица §1.9: строки фактов, их цвет и даты; сообщение — отдельно и в
    // одну строку (беды R, T).
    zt::MiniStore home;
    ZStorageManager stores;
    const QString store = home.root() + QStringLiteral("/хранилище");
    ZT_TRUE("хранилище завелось", makeStore(store).isEmpty());
    stores.beginSession();
    Model& model = stores;
    model.addFolder(store);

    {
        const Model::Snapshot snap = model.snapshot();
        ZT_TRUE("Local: суммы", snap.local.text.contains(QStringLiteral("notes")) &&
                    !snap.local.alarm);
        ZT_TRUE("Local: дата второй строкой",
                snap.local.text.contains(QStringLiteral("\nmodified ")));
        ZT_TRUE("Cloud: not set", snap.cloud.text == QStringLiteral("Cloud: not set"));
    }
    model.edit(Model::FieldId::Server, QStringLiteral("/mnt/облако"));
    ZT_TRUE("Cloud: not checked",
            model.snapshot().cloud.text == QStringLiteral("Cloud: not checked"));

    // Пустое облако — НЕ беда (беда R): обычным шрифтом.
    Model::CloudSeen seen;
    seen.state = Model::CloudSeen::State::Empty;
    seen.address = QStringLiteral("/mnt/облако");
    model.noteSeen(seen);
    {
        const Model::Snapshot snap = model.snapshot();
        ZT_TRUE("Cloud: empty не красным",
                snap.cloud.text == QStringLiteral("Cloud: empty") && !snap.cloud.alarm);
        // Совет словами: следующий шаг — запечатать.
        ZT_TRUE("совет про запечатывание",
                snap.message.text.contains(QStringLiteral("seal")));
        ZT_TRUE("сообщение — одна строка (П7)",
                !snap.message.text.contains(QLatin1Char('\n')));
    }

    // Наше облако — суммы и дата; WrongPassword — факт про облако, а не про
    // исход попытки (беда T).
    seen.state = Model::CloudSeen::State::Ours;
    seen.stats.notes = 128;
    seen.stats.attachments = 34;
    seen.stats.bytes = 43 << 20;
    seen.stats.lastModified = QDateTime::fromString(
        QStringLiteral("2026-08-30T12:00:00Z"), Qt::ISODate);
    model.noteSeen(seen);
    {
        const Model::Snapshot snap = model.snapshot();
        ZT_TRUE("Cloud: суммы", snap.cloud.text.contains(QStringLiteral("128 notes")));
        ZT_TRUE("Cloud: дата второй строкой",
                snap.cloud.text.contains(QStringLiteral("\nmodified ")));
    }
    seen.state = Model::CloudSeen::State::WrongPassword;
    model.noteSeen(seen);
    ZT_TRUE("WrongPassword: строка фактов — про облако",
            model.snapshot().cloud.text.contains(QStringLiteral("128 notes")) &&
                !model.snapshot().cloud.alarm);

    // Смена адреса забывает свежесть: снова «not checked».
    model.edit(Model::FieldId::Server, QStringLiteral("/mnt/другое"));
    ZT_TRUE("смена адреса забывает увиденное",
            model.snapshot().cloud.text == QStringLiteral("Cloud: not checked"));
}

void checkFreshnessAutomaton() {
    // Автомат свежести (беда F + §3.7): пусто → повтор → несовпадение →
    // совпадение → пропуск на запечатывание; переключение строк и возврат —
    // факты строки те же.
    zt::MiniStore home, cloudHome;
    ZStorageManager stores;
    const QString store = home.root() + QStringLiteral("/хранилище");
    const QString other = home.root() + QStringLiteral("/второе");
    ZT_TRUE("хранилище завелось", makeStore(store).isEmpty());
    ZT_TRUE("второе завелось", makeStore(other).isEmpty());
    stores.beginSession();
    Model& model = stores;
    model.addFolder(store);
    model.addFolder(other);
    // Выбор — на первом.
    model.select(0);
    const QString cloud = cloudHome.root() + QStringLiteral("/облако");
    model.edit(Model::FieldId::Server, cloud);
    model.edit(Model::FieldId::EncryptionPassword, QStringLiteral("пароль"));

    // Первый Check: повтора ещё нет (облако не видели) — job без пропуска.
    Model::Reaction first = model.checkPressed();
    ZT_TRUE("первый Check — работа", first.job.kind == Model::Job::Kind::Check);
    ZT_TRUE("без пропуска на запечатывание", !first.job.sealEmpty);

    // Работа увидела пустое облако.
    Model::Outcome outcome;
    outcome.ok = true;
    outcome.message = QStringLiteral("Connected.");
    outcome.seen.state = Model::CloudSeen::State::Empty;
    outcome.seen.address = cloud;
    model.jobFinished(Model::Job::Kind::Check, outcome);
    ZT_TRUE("повтор показался", model.snapshot().repeatVisible);

    // Пустой повтор и несовпадение — работа не заказывается, сказано словами.
    ZT_TRUE("пустой повтор не пускает",
            model.checkPressed().job.kind == Model::Job::Kind::None);
    model.edit(Model::FieldId::Repeat, QStringLiteral("парол"));
    ZT_TRUE("несовпадение не пускает",
            model.checkPressed().job.kind == Model::Job::Kind::None);
    ZT_TRUE("и сказано красным",
            model.snapshot().message.text.contains(QStringLiteral("match")));
    model.edit(Model::FieldId::Repeat, QStringLiteral("пароль"));
    Model::Reaction second = model.checkPressed();
    ZT_TRUE("совпавший повтор пускает", second.job.kind == Model::Job::Kind::Check);
    ZT_TRUE("и выдаёт пропуск на запечатывание", second.job.sealEmpty);

    // Переключился и вернулся — черновик и память строки на месте (беда F).
    model.select(1);
    ZT_TRUE("у второй строки своя жизнь", model.snapshot().server.text.isEmpty());
    model.select(0);
    ZT_TRUE("черновик на месте",
            model.snapshot().server.text == cloud &&
                model.snapshot().repeatVisible);
}

void checkResetRoads() {
    // Reset cloud: непроверенный адрес — сначала Check (владелец 29.08:
    // «check должен выполниться сам»); дороги — по ключу в связке; переспрос
    // называет ПОЛНЫЙ адрес (урок 30.08).
    zt::MiniStore home, cloudHome;
    auto secrets = std::make_shared<zt::FakeSecrets>();
    ZStorageManager stores(secrets);
    const QString store = home.root() + QStringLiteral("/хранилище");
    ZT_TRUE("хранилище завелось", makeStore(store).isEmpty());
    const QString storeId = ZStorage(store).identity().storeId();
    stores.beginSession();
    Model& model = stores;
    model.addFolder(store);
    const QString cloud = cloudHome.root() + QStringLiteral("/облако");
    model.edit(Model::FieldId::Server, cloud);

    // Не смотрели — Reset сам заказывает Check и помнит намерение. Набранный
    // пароль в разведку НЕ уезжает: человек мог уже набрать НОВЫЙ, и старый
    // конверт им не развернётся (живая проба 30.08).
    model.edit(Model::FieldId::EncryptionPassword, QStringLiteral("уже-новый"));
    Model::Reaction go = model.resetPressed();
    ZT_TRUE("сначала проверка", go.job.kind == Model::Job::Kind::Check);
    ZT_TRUE("и разведка парольно-слепая", go.job.encryptionPassword.isEmpty());
    model.edit(Model::FieldId::EncryptionPassword, QString());
    Model::Outcome sawOurs;
    sawOurs.ok = true;
    sawOurs.seen.state = Model::CloudSeen::State::Ours;
    sawOurs.seen.address = cloud;
    sawOurs.seen.stats.notes = 3;
    Model::Reaction next = model.jobFinished(Model::Job::Kind::Check, sawOurs);
    ZT_TRUE("переспрос пришёл сам",
            next.question.kind == Model::Question::Kind::ResetCloud);
    // Ключа в связке нет — дороги стирания, адрес назван целиком.
    ZT_TRUE("текст называет полный адрес", next.question.text.contains(cloud));
    ZT_TRUE("дорога стирания с новым паролем",
            next.question.choices.first().contains(QStringLiteral("reset password")));

    // [Erase and reset password] без набранного пароля — просьба словами.
    Model::Reaction refused = model.answered(Model::Question::Kind::ResetCloud, 0);
    ZT_TRUE("без пароля работы нет", refused.job.kind == Model::Job::Kind::None);
    ZT_TRUE("сказано набрать дважды",
            model.snapshot().message.text.contains(QStringLiteral("twice")));
    model.edit(Model::FieldId::EncryptionPassword, QStringLiteral("новый"));
    model.edit(Model::FieldId::Repeat, QStringLiteral("новый"));
    // Свежесть съела сообщение — намерение спрашивается заново тем же путём.
    Model::Reaction sealed = model.answered(Model::Question::Kind::ResetCloud, 0);
    ZT_TRUE("с паролем — стирание с посевом",
            sealed.job.kind == Model::Job::Kind::EraseAndReseed);

    // Ключ лёг в связку — дорога меняется на смену пароля без стирания.
    secrets->pretendKey(storeId);
    stores.refresh(store);
    Model::Reaction change = model.resetPressed();
    ZT_TRUE("ключ в связке — предлагается смена пароля",
            change.question.choices.first().contains(QStringLiteral("Change")));
    ZT_TRUE("вторая дорога — стереть и отвязаться",
            change.question.choices.at(1).contains(QStringLiteral("disconnect")));
    Model::Reaction changed = model.answered(Model::Question::Kind::ResetCloud, 0);
    ZT_TRUE("смена пароля с набранным паролем — работа",
            changed.job.kind == Model::Job::Kind::ChangePassword);

    // [Erase and disconnect] и его итог: адрес и память строки забыты.
    Model::Reaction drop = model.answered(Model::Question::Kind::ResetCloud, 1);
    ZT_TRUE("отвязка — работа", drop.job.kind == Model::Job::Kind::EraseAndDisconnect);
    Model::Outcome done;
    done.ok = true;
    done.message = QStringLiteral("Disconnected.");
    model.jobFinished(Model::Job::Kind::EraseAndDisconnect, done);
    ZT_TRUE("адрес забыт", model.snapshot().server.text.isEmpty());
    ZT_TRUE("строка менеджера без облака",
            !stores.storeFor(store).hasCloudAddress());
    ZT_TRUE("облако снова not set",
            model.snapshot().cloud.text == QStringLiteral("Cloud: not set"));
}

void checkAddressNeverBitten() {
    // ЗАКОН ВЛАДЕЛЬЦА (30.08.2026): что бы ни было введено в Cloud server и
    // Cloud dir, от сервера НИКОГДА не откусывается путь (/webdav). Прежде
    // адрес склеивался и разрезался «по последнему сегменту» — и /webdav
    // уезжал в Cloud dir при первом же показе строки.
    zt::MiniStore home;
    ZStorageManager stores;
    const QString store = home.root() + QStringLiteral("/хранилище");
    ZT_TRUE("хранилище завелось", makeStore(store).isEmpty());

    const auto shownAfterReopen = [&](const QString& server, const QString& dir)
        -> std::pair<QString, QString> {
        // Набрали — окно умерло (stash) — «новый запуск»: СВЕЖИЙ менеджер
        // без черновиков читает то, что сохранилось в строке (через JSON —
        // ровно дорога state.json).
        stores.beginSession();
        stores.select(0);
        stores.edit(Model::FieldId::Server, server);
        stores.edit(Model::FieldId::ServerDir, dir);
        stores.stashDrafts();
        Model reopened;
        reopened.storesFromJson(stores.storesToJson());
        reopened.beginSession();
        reopened.select(0);
        const Model::Snapshot snap = reopened.snapshot();
        return {snap.server.text, snap.serverDir.text};
    };
    ZStorage::Config row;
    row.root = store;
    stores.remember(row);

    // С папкой: сервер возвращается РОВНО как введён, папка — отдельно.
    auto shown = shownAfterReopen(QStringLiteral("https://host/webdav"),
                                  QStringLiteral("vpnotes"));
    ZT_EQ("сервер не тронут", std::string("https://host/webdav"), s(shown.first));
    ZT_EQ("папка отдельно", std::string("vpnotes"), s(shown.second));

    // Без папки: /webdav не откусывается тем более.
    shown = shownAfterReopen(QStringLiteral("https://host/webdav"), QString());
    ZT_EQ("сервер без папки не тронут", std::string("https://host/webdav"),
          s(shown.first));
    ZT_TRUE("папка так и пуста", shown.second.isEmpty());

    // Наследный СКЛЕЕННЫЙ адрес (CLI --url, прежние записи): показывается
    // целиком в поле сервера, и дефолт-папка к нему НЕ дописывается.
    ZStorage::Config legacy;
    legacy.root = store;
    legacy.cloudUrl = QStringLiteral("https://webdav.yandex.ru/зам/01abcdef/");
    stores.remember(legacy);
    Model reopened;
    reopened.storesFromJson(stores.storesToJson());
    reopened.beginSession();
    reopened.select(0);
    ZT_EQ("наследный адрес — целиком, как хранится",
          std::string("https://webdav.yandex.ru/зам/01abcdef/"),
          s(reopened.snapshot().server.text));
    const Model::Job legacyJob = reopened.checkPressed().job;
    ZT_TRUE("Check по наследному адресу жив",
            legacyJob.kind == Model::Job::Kind::Check);
    ZT_EQ("и в работу он уезжает без дописок",
          std::string("https://webdav.yandex.ru/зам/01abcdef/"),
          s(legacyJob.cfg.collectionUrl()));
}

void checkSclerosisCured() {
    // ЗАКОН ВЛАДЕЛЬЦА (30.08.2026): всё набранное в адресе и логине — пусть
    // даже с опечаткой и без связи с сервером — попадает в настройки и
    // ПЕРЕЖИВАЕТ ВЫХОД ИЗ ПРОГРАММЫ. Круг: набрали → окно умерло (stash) →
    // секция stores уехала в JSON (как в state.json) → «новый запуск» читает
    // её в свежий менеджер — и всё на месте.
    zt::MiniStore home;
    ZStorageManager stores;
    const QString store = home.root() + QStringLiteral("/хранилище");
    ZT_TRUE("хранилище завелось", makeStore(store).isEmpty());
    ZStorage::Config row;
    row.root = store;
    stores.remember(row);
    stores.beginSession();
    stores.select(0);
    stores.edit(Model::FieldId::Server,
                QStringLiteral("https://опечатка.example/webdav"));
    stores.edit(Model::FieldId::ServerDir, QStringLiteral("vpnotes2"));
    stores.edit(Model::FieldId::Login, QStringLiteral("u132748"));
    // Никакой работы: связи не было, и это не повод всё забыть.
    stores.stashDrafts();
    Model reopened;
    reopened.storesFromJson(stores.storesToJson());   // выход → state.json → запуск
    reopened.beginSession();
    reopened.select(0);
    const Model::Snapshot snap = reopened.snapshot();
    ZT_EQ("сервер пережил выход", std::string("https://опечатка.example/webdav"),
          s(snap.server.text));
    ZT_EQ("папка пережила выход", std::string("vpnotes2"), s(snap.serverDir.text));
    ZT_EQ("логин пережил выход", std::string("u132748"), s(snap.login.text));
}

void checkDraftsNeverSerialized() {
    // ПОСЛЕ СЛИЯНИЯ ЧЕРНОВИКИ ЖИВУТ В ТОМ ЖЕ ОБЪЕКТЕ, ЧТО И СЕРИАЛИЗУЕМЫЙ
    // СПИСОК, — граница «это на диск, это НИКОГДА» больше не держится типом.
    // Держит её этот страж: набранные пароли не смеют появиться ни в одном
    // значении секции stores, а адрес и логин обязаны уехать (склероз).
    zt::MiniStore home;
    ZStorageManager stores;
    const QString store = home.root() + QStringLiteral("/хранилище");
    ZT_TRUE("хранилище завелось", makeStore(store).isEmpty());
    ZStorage::Config row;
    row.root = store;
    stores.remember(row);
    stores.beginSession();
    stores.select(0);
    stores.edit(Model::FieldId::Server, QStringLiteral("https://host/webdav"));
    stores.edit(Model::FieldId::Login, QStringLiteral("логин-в-json"));
    stores.edit(Model::FieldId::ServerPassword, QStringLiteral("сверхтайна-сервера"));
    stores.edit(Model::FieldId::EncryptionPassword, QStringLiteral("сверхтайна-шифра"));
    stores.edit(Model::FieldId::Repeat, QStringLiteral("сверхтайна-шифра"));
    stores.stashDrafts();
    const QByteArray json =
        QJsonDocument(QJsonObject{{QStringLiteral("stores"), stores.storesToJson()}})
            .toJson();
    ZT_TRUE("паролей в секции stores нет",
            !json.contains(QByteArrayLiteral("сверхтайна")));
    ZT_TRUE("адрес уехал", json.contains(QByteArrayLiteral("https://host/webdav")));
    ZT_TRUE("логин уехал", json.contains(QByteArrayLiteral("логин-в-json")));
}

void checkStubsNeverRead() {
    // §3.16: связка ВИДНА (кружочки, глаза) и НЕ ЧИТАЕТСЯ — счётчик чтений
    // подделки остаётся нулём; заглушка не уезжает в работу как пароль.
    zt::MiniStore home, cloudHome;
    auto secrets = std::make_shared<zt::FakeSecrets>();
    ZStorageManager stores(secrets);
    const QString store = home.root() + QStringLiteral("/хранилище");
    ZT_TRUE("хранилище завелось", makeStore(store).isEmpty());
    const QString storeId = ZStorage(store).identity().storeId();
    secrets->pretendKey(storeId);
    secrets->setServerPassword(storeId, QStringLiteral("пароль"), nullptr);
    secrets->reads = 0;

    stores.beginSession();
    Model& model = stores;
    model.addFolder(store);
    model.edit(Model::FieldId::Server, QStringLiteral("https://host/dav"));
    const Model::Snapshot snap = model.snapshot();
    ZT_TRUE("кружочки у пароля сервера", snap.serverPassword.stub);
    ZT_TRUE("кружочки у пароля шифрования (ключ)", snap.encryptionPassword.stub);
    ZT_TRUE("глаз пароля сервера жив", snap.serverPassword.eyeEnabled);
    ZT_TRUE("красных требований нет", !snap.serverPassword.placeholderAlarm &&
                !snap.encryptionPassword.placeholderAlarm);
    ZT_EQ("секретов не читали ни разу", std::string("0"),
          std::to_string(secrets->reads));

    // Заглушка не уезжает в работу: пустое нетронутое поле — «возьми из
    // связки», а не пароль из кружочков.
    const Model::Job job = model.checkPressed().job;
    ZT_TRUE("пароль сервера в работе пуст, но помечен связкой",
            job.serverPassword.isEmpty() && job.serverPasswordFromKeyring);
    ZT_TRUE("пароль шифрования — так же",
            job.encryptionPassword.isEmpty() && job.encryptionFromKeyring);

    // Первое нажатие клавиши стирает заглушку: пустое поле = «пусто».
    model.edit(Model::FieldId::ServerPassword, QString());
    ZT_TRUE("заглушка стёрта", !model.snapshot().serverPassword.stub);
    ZT_TRUE("«возьми из связки» снят",
            !model.checkPressed().job.serverPasswordFromKeyring);
}

}  // namespace

static int ztRunSuite(int, char**) {
    checkZeroAndEmptiedList();
    checkAddFolder();
    checkButtonsTableA();
    checkOpenRow();
    checkFactLines();
    checkFreshnessAutomaton();
    checkResetRoads();
    checkAddressNeverBitten();
    checkSclerosisCured();
    checkDraftsNeverSerialized();
    checkStubsNeverRead();
    return zt::report("zstorage_manager_window");
}

TEST(ZStorageManagerWindow, All) {
    std::vector<QByteArray> ztArgs{QByteArrayLiteral("zstorage_manager_window_test")};
    std::vector<char*> ztArgv;
    for (QByteArray& a : ztArgs) ztArgv.push_back(a.data());
    EXPECT_EQ(0, ztRunSuite(int(ztArgv.size()), ztArgv.data()));
}
