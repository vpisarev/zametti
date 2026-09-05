// ПОДКЛЮЧЕНИЕ ОБЛАКА: set-cloud, useLastCloud, бутстрап, отвязка.
//
// Все ветки знакомства с облаком в одном месте (ZStorage::connectCloud):
// пустое облако чеканит keyfile, своё облако разворачивает его паролем,
// чужое — честная остановка до единой записи, пустое хранилище наследует
// идентичность манифеста (бутстрап). Облако здесь — каталог (FolderCloud):
// логика подключения от транспорта не зависит.

#include "blob_cipher.h"
#include "keyfile.h"
#include "secret_store.h"
#include "zstorage.h"

#include "fake_secrets.h"
#include "mini_store.h"
#include "test_util.h"
#include "scratch_files.h"

#include <QDir>
#include <QFile>
#include <QHash>
#include <QJsonObject>
#include <string>
#include <vector>

using namespace zametti;

namespace {

// Аргон с боевыми порогами съел бы полторы секунды на каждый разворот.
const Keyfile::KdfParams kTiny{1, 1 << 20};

// Keyring в памяти — общая подделка из fake_secrets.h (до 29.08.2026 таких
// классов по наборам лежало три, слово в слово).
using FakeSecrets = zt::FakeSecrets;

void checkConnectMintsAndRemembers() {
    zt::MiniStore store, cloudHome;
    const QString cloud = cloudHome.root() + QStringLiteral("/облако");
    ZStorage s(store.root());
    FakeSecrets secrets;
    ZStorage::Config cfg;
    cfg.cloudDir = cloud;

    ZStorage::ConnectOutcome out;
    QString err;
    ZT_TRUE(("подключение прошло: " + err.toStdString()).c_str(),
            s.connectCloud(cfg, QStringLiteral("пароль-шифра"), QString(), secrets, kTiny,
                            &out, &err));
    ZT_TRUE("keyfile отчеканен", out.mintedKeyfile);
    ZT_TRUE("облако подключено", s.isConnected());
    ZT_TRUE("keyfile лежит в облаке",
            QFile::exists(cloud + QStringLiteral("/keyfile")));
    ZT_TRUE("адрес запомнен", s.cloudConfig().hasCloudAddress());
    ZT_TRUE("ключ лёг в keyring", secrets.keys_.size() == 1);
    // Пароль шифрования хранится (решение владельца 28.08.2026): он для глаз
    // человека, и кладётся только после удачи.
    ZT_TRUE("пароль шифрования лёг в keyring",
            secrets.cryptPasswords_.value(s.identity().storeId()) ==
                QStringLiteral("пароль-шифра"));

    // Пустой пароль шифрования не бывает паролем.
    ZT_TRUE("пустой пароль отвергнут",
            !s.connectCloud(cfg, QString(), QString(), secrets, kTiny, nullptr, &err));
}

void checkReconnectUnwrapsExistingKeyfile() {
    zt::MiniStore store, cloudHome;
    const QString cloud = cloudHome.root() + QStringLiteral("/облако");
    ZStorage::Config cfg;
    cfg.cloudDir = cloud;
    FakeSecrets secrets;
    QString err;
    {
        ZStorage s(store.root());
        ZT_TRUE("первое подключение прошло",
                s.connectCloud(cfg, QStringLiteral("пароль-шифра"), QString(), secrets, kTiny,
                                nullptr, &err));
    }
    // Переподключение (утраченный keyring): тот же пароль будит тот же ключ.
    {
        ZStorage s(store.root());
        FakeSecrets fresh;
        ZStorage::ConnectOutcome out;
        ZT_TRUE(("переподключение прошло: " + err.toStdString()).c_str(),
                s.connectCloud(cfg, QStringLiteral("пароль-шифра"), QString(), fresh, kTiny,
                                &out, &err));
        ZT_TRUE("keyfile не перечеканен", !out.mintedKeyfile);
        ZT_TRUE("ключ снова в keyring", fresh.keys_.size() == 1);
    }
    // Неверный пароль — отказ с честными словами.
    {
        ZStorage s(store.root());
        FakeSecrets fresh;
        ZT_TRUE("неверный пароль отвергнут",
                !s.connectCloud(cfg, QStringLiteral("не тот пароль"), QString(), fresh, kTiny,
                                 nullptr, &err));
        ZT_TRUE("сказано про пароль", err.contains(QStringLiteral("password")));
        ZT_TRUE("ключ в keyring не лёг", fresh.keys_.isEmpty());
    }
}

void checkUseLastCloud() {
    zt::MiniStore store, cloudHome;
    const QString cloud = cloudHome.root() + QStringLiteral("/облако");
    ZStorage::Config cfg;
    cfg.cloudDir = cloud;
    FakeSecrets secrets;
    QString err;
    {
        ZStorage s(store.root());
        ZT_TRUE("подключение прошло",
                s.connectCloud(cfg, QStringLiteral("пароль-шифра"), QString(), secrets, kTiny,
                                nullptr, &err));
    }
    // Новый запуск: адрес из cloud.json, ключ из keyring, ноль вопросов.
    {
        ZStorage s(store.root());
        ZT_TRUE(("useLastCloud прошёл: " + err.toStdString()).c_str(),
                s.useLastCloud(secrets, &err));
        ZT_TRUE("облако подключено", s.isConnected());
    }
    // Пустой keyring — тихий отказ с понятной причиной, не падение.
    {
        ZStorage s(store.root());
        FakeSecrets empty;
        ZT_TRUE("без ключа не подключается", !s.useLastCloud(empty, &err));
        ZT_TRUE("сказано про keyring", err.contains(QStringLiteral("keyring")));
    }
    // Отвязка: адрес забыт, подключение больше не восстанавливается.
    {
        ZStorage s(store.root());
        ZT_TRUE("отвязка прошла", s.clearCloudConfig(&err));
        ZT_TRUE("конфиг без облака", !s.cloudConfig().hasCloudAddress());
        ZT_TRUE("после отвязки не подключается", !s.useLastCloud(secrets, &err));
        ZT_TRUE("сказано «не настроен»", err.contains(QStringLiteral("not configured")));
    }
}

void checkBootstrapInheritsIdentity() {
    zt::MiniStore first, cloudHome, second;
    const QString cloud = cloudHome.root() + QStringLiteral("/облако");
    ZStorage::Config cfg;
    cfg.cloudDir = cloud;
    FakeSecrets secrets;
    QString err;
    QString firstId;
    QString rootId;
    {
        ZStorage s(first.root());
        ZT_TRUE("первое устройство подключилось",
                s.connectCloud(cfg, QStringLiteral("пароль-шифра"), QString(), secrets, kTiny,
                                nullptr, &err));
        firstId = s.identity().storeId();
        // Корень — настоящая заметка: бутстрап обязан привезти её сразу.
        rootId = s.ensureRootNote(&err);
        ZT_TRUE("корень завёлся", !rootId.isEmpty());
        // Манифест кладёт заливка — без него бутстрапу неоткуда узнать id.
        ZT_TRUE("заливка прошла", s.pushAll(nullptr, &err));
    }
    {
        // Каркас руками — прежний путь; он обязан работать и дальше.
        ZStorage s(second.root());
        FakeSecrets fresh;
        ZStorage::ConnectOutcome out;
        ZT_TRUE(("бутстрап прошёл: " + err.toStdString()).c_str(),
                s.connectCloud(cfg, QStringLiteral("пароль-шифра"), QString(), fresh, kTiny,
                                &out, &err));
        ZT_TRUE("идентичность унаследована", out.inheritedIdentity);
        ZT_TRUE("keyfile не перечеканен", !out.mintedKeyfile);
        ZT_EQ("id совпал с первым устройством", firstId.toStdString(),
              s.identity().storeId().toStdString());
        ZT_TRUE("корневая заметка уже на диске", out.rootMaterialized);
        ZT_TRUE("файл корня существует",
                QFile::exists(second.root() + QStringLiteral("/") + rootId +
                              QStringLiteral(".md")));
        // Сводка облака — суммы, не перечисление.
        ZT_TRUE("заметки посчитаны", out.cloudNotes >= 1);
        ZT_TRUE("объём посчитан", out.cloudBytes > 0);
    }
}

void checkBootstrapIntoEmptyDir() {
    // Новое устройство: каталога ещё НЕТ ВОВСЕ — set-cloud сам заводит
    // каркас (без чеканки идентичности!) и наследует id из манифеста.
    zt::MiniStore first, cloudHome, home;
    const QString cloud = cloudHome.root() + QStringLiteral("/облако");
    ZStorage::Config cfg;
    cfg.cloudDir = cloud;
    FakeSecrets secrets;
    QString err;
    QString firstId;
    {
        ZStorage s(first.root());
        ZT_TRUE("первое устройство подключилось",
                s.connectCloud(cfg, QStringLiteral("пароль-шифра"), QString(), secrets, kTiny,
                                nullptr, &err));
        firstId = s.identity().storeId();
        ZT_TRUE("корень завёлся", !s.ensureRootNote(&err).isEmpty());
        ZT_TRUE("заливка прошла", s.pushAll(nullptr, &err));
    }
    const QString fresh = home.root() + QStringLiteral("/новые-заметки");
    {
        // Статический вход — тот самый, что позовут CLI и будущий диалог.
        FakeSecrets mine;
        ZStorage::ConnectOutcome out;
        auto s = ZStorage::initFromCloud(fresh, cfg, QStringLiteral("пароль-шифра"),
                                          QString(), mine, kTiny, &out, &err);
        ZT_TRUE(("бутстрап в пустоту прошёл: " + err.toStdString()).c_str(), s != nullptr);
        if (s == nullptr) return;
        ZT_TRUE("теперь это хранилище", s->isStore());
        ZT_TRUE("хранилище уже подключено", s->isConnected());
        ZT_TRUE("идентичность унаследована, не отчеканена", out.inheritedIdentity);
        ZT_EQ("id — облачный", firstId.toStdString(), s->identity().storeId().toStdString());
        ZT_TRUE("каркас на месте",
                QDir(fresh + QStringLiteral("/.zametti")).exists() &&
                    QDir(fresh + QStringLiteral("/history")).exists());
        ZT_TRUE("корень материализован", out.rootMaterialized);
    }
    // ПУСТО С ОБЕИХ СТОРОН — отказ (решение владельца): так в жизни почти
    // не бывает, так выглядит опечатка в адресе облака или в локальном пути.
    // И никаких огрызков после отказа: каталог не создаётся.
    {
        FakeSecrets mine;
        const QString typo = home.root() + QStringLiteral("/каталог-с-опечаткой");
        ZStorage::Config empty;
        empty.cloudDir = cloudHome.root() + QStringLiteral("/облако-с-опечаткой");
        ZT_TRUE("пусто с обеих сторон отвергнуто",
                ZStorage::initFromCloud(typo, empty, QStringLiteral("пароль-шифра"),
                                         QString(), mine, kTiny, nullptr, &err) == nullptr);
        ZT_TRUE("причина говорит про опечатку", err.contains(QStringLiteral("mistyped")));
        ZT_TRUE("огрызков не осталось", !QDir(typo).exists());
    }

    // НЕПУСТОЙ каталог без метки хранилища — по-прежнему отказ.
    {
        zt::MiniStore junkHome;
        const QString junk = junkHome.root() + QStringLiteral("/бумаги");
        QDir().mkpath(junk);
        QFile f(junk + QStringLiteral("/письмо.txt"));
        ZT_TRUE("файл завёлся", f.open(QIODevice::WriteOnly));
        f.write("не заметка");
        f.close();
        FakeSecrets mine;
        ZT_TRUE("случайная папка отвергнута",
                ZStorage::initFromCloud(junk, cfg, QStringLiteral("пароль-шифра"), QString(),
                                         mine, kTiny, nullptr, &err) == nullptr);
        ZT_TRUE("причина — непустой каталог", err.contains(QStringLiteral("not empty")));
    }
}

void checkBootstrapFromLegacyCloud() {
    // Новое устройство встаёт и с НАСЛЕДНОГО облака (журналы <id>.log):
    // бутстрап пробует новое имя корня, затем старое. Наследное облако
    // лепится честно — пере-запечатыванием под старым именем (AAD).
    zt::MiniStore first, cloudHome, home;
    const QString cloud = cloudHome.root() + QStringLiteral("/облако");
    ZStorage::Config cfg;
    cfg.cloudDir = cloud;
    FakeSecrets secrets;
    QString err;
    QString rootId;
    {
        ZStorage s(first.root());
        ZT_TRUE("первое устройство подключилось",
                s.connectCloud(cfg, QStringLiteral("пароль-шифра"), QString(), secrets, kTiny,
                                nullptr, &err));
        rootId = s.ensureRootNote(&err);
        ZT_TRUE("корень завёлся", !rootId.isEmpty());
        ZT_TRUE("заливка прошла", s.pushAll(nullptr, &err));
        // Журнал корня — под наследное имя, нового не оставляем.
        auto cipher =
            XChaChaCipher::make(secrets.keys_.value(s.identity().storeId()), nullptr);
        ZT_TRUE("шифр родился", cipher != nullptr);
        QByteArray journalBytes;
        ZT_TRUE("журнал корня прочитался", s.readJournalBytes(rootId, &journalBytes, &err));
        QByteArray blob;
        const QString legacy = rootId + QStringLiteral(".log");
        ZT_TRUE("наследный блоб запечатался",
                cipher->seal(journalBytes,
                             BlobAad{BlobKind::Journal, s.identity().storeId(), legacy},
                             &blob, &err));
        QFile f(cloud + QStringLiteral("/") + legacy);
        ZT_TRUE("наследный блоб записался", f.open(QIODevice::WriteOnly));
        f.write(blob);
        f.close();
        ZT_TRUE("новое имя убрано",
                zt::dropFile(cloud, cloud + QStringLiteral("/") + rootId +
                                        QStringLiteral(".zm")));
    }
    FakeSecrets mine;
    ZStorage::ConnectOutcome out;
    auto s = ZStorage::initFromCloud(home.root() + QStringLiteral("/копия"), cfg,
                                      QStringLiteral("пароль-шифра"), QString(), mine, kTiny,
                                      &out, &err);
    ZT_TRUE(("бутстрап с наследного прошёл: " + err.toStdString()).c_str(), s != nullptr);
    if (s != nullptr) ZT_TRUE("корень материализован через наследное имя", out.rootMaterialized);
}

void checkForeignCloudRefused() {
    zt::MiniStore mine, cloudHome, foreign;
    const QString cloud = cloudHome.root() + QStringLiteral("/облако");
    ZStorage::Config cfg;
    cfg.cloudDir = cloud;
    FakeSecrets secrets;
    QString err;
    QString mineId;
    QString mineRootTitle;
    {
        ZStorage s(mine.root());
        ZT_TRUE("своё облако заведено",
                s.connectCloud(cfg, QStringLiteral("пароль-шифра"), QString(), secrets, kTiny,
                                nullptr, &err));
        mineId = s.identity().storeId();
        // Корень — настоящая заметка: его заголовок и есть читаемое имя,
        // которое обязана назвать диагностика чужого облака.
        const QString rootId = s.ensureRootNote(&err);
        ZT_TRUE("корень завёлся", !rootId.isEmpty());
        mineRootTitle = s.info(rootId)->title();
        ZT_TRUE("манифест уехал", s.pushAll(nullptr, &err));
    }
    // У чужого хранилища СВОЯ идентичность: манифест облака не совпадёт.
    {
        ZStorage s(foreign.root());
        ZT_TRUE("чужая идентичность отчеканилась", !s.ensureIdentity(&err).isEmpty());
        FakeSecrets fresh;
        const QByteArray keyfileBefore = [&] {
            QFile f(cloud + QStringLiteral("/keyfile"));
            if (!f.open(QIODevice::ReadOnly)) return QByteArray();
            return f.readAll();
        }();
        ZT_TRUE("чужое облако отвергнуто",
                !s.connectCloud(cfg, QStringLiteral("пароль-шифра"), QString(), fresh, kTiny,
                                 nullptr, &err));
        ZT_TRUE("причина называет чужой store",
                err.contains(QStringLiteral("another store")));
        // Диагностика — не голые id (решение владельца): оба id, дата
        // создания в UTC, подсказка про адрес, и — раз пароль общий —
        // ЧИТАЕМОЕ ИМЯ чужого облака, вскрытое best-effort.
        ZT_TRUE("назван id облака", err.contains(mineId));
        ZT_TRUE("назван свой id", err.contains(s.identity().storeId()));
        ZT_TRUE("названы даты создания", err.contains(QStringLiteral("(created 20")));
        ZT_TRUE("дата — в UTC", err.contains(QStringLiteral("Z (")) || err.contains(QStringLiteral("Z)")));
        ZT_TRUE("подсказка про адрес",
                err.contains(QStringLiteral("check the cloud address")));
        ZT_TRUE("имя чужого облака вскрыто общим паролем",
                err.contains(QStringLiteral("\"%1\"").arg(mineRootTitle)));
        ZT_TRUE("cloud.json не записан", !s.cloudConfig().hasCloudAddress());
        QFile f(cloud + QStringLiteral("/keyfile"));
        ZT_TRUE("keyfile открылся", f.open(QIODevice::ReadOnly));
        ZT_TRUE("keyfile облака не тронут", f.readAll() == keyfileBefore);
    }
}

void checkConfigReadsLegacyKeys() {
    // remote.json прежних сборок (и с совсем старыми ключами url/dir/user, и
    // с remoteUrl/…) обязан читаться, а при следующей записи мигрировать: имя
    // файла становится cloud.json, ключи — cloudUrl/cloudDir/cloudUser,
    // старый файл исчезает ТЕМ ЖЕ шагом.
    zt::MiniStore store;
    ZStorage s(store.root());
    {
        QFile f(store.root() + QStringLiteral("/.zametti/remote.json"));
        ZT_TRUE("старый remote.json записался", f.open(QIODevice::WriteOnly));
        f.write("{ \"url\": \"https://host/dav/\", \"user\": \"юзер\", "
                "\"timeoutMs\": 7000 }");
    }
    ZStorage::Config cfg = s.cloudConfig();
    ZT_TRUE("облако прочитано", cfg.hasCloudAddress());
    ZT_EQ("url со старого ключа", std::string("https://host/dav/"),
          cfg.cloudUrl.toStdString());
    ZT_EQ("логин со старого ключа", std::string("юзер"), cfg.cloudUser.toStdString());
    ZT_TRUE("таймаут прочитан", cfg.timeoutMs == 7000);
    ZT_EQ("root — корень копии, не из файла", store.root().toStdString(),
          cfg.root.toStdString());

    QString err;
    ZT_TRUE("перезапись прошла", s.writeCloudConfig(cfg, &err));
    ZT_TRUE("старого remote.json больше нет",
            !QFile::exists(store.root() + QStringLiteral("/.zametti/remote.json")));
    QFile f(store.root() + QStringLiteral("/.zametti/cloud.json"));
    ZT_TRUE("cloud.json открылся", f.open(QIODevice::ReadOnly));
    const QByteArray bytes = f.readAll();
    ZT_TRUE("мигрировал на новый ключ", bytes.contains("cloudUrl"));
    ZT_TRUE("логин на новом ключе", bytes.contains("cloudUser"));
    // Путь в файл не пишется: переехал бы вместе с каталогом и врал.
    ZT_TRUE("root в cloud.json не уезжает", !bytes.contains("\"root\""));
    // Содержимое пережило переезд: перечитанный конфиг равен записанному.
    const ZStorage::Config again = s.cloudConfig();
    ZT_TRUE("круг миграции сходится",
            again.cloudUrl == cfg.cloudUrl && again.cloudUser == cfg.cloudUser &&
                again.timeoutMs == cfg.timeoutMs);

    // Ключи remoteUrl/… промежуточных сборок — тоже запасной путь чтения.
    {
        QFile mid(store.root() + QStringLiteral("/.zametti/cloud.json"));
        ZT_TRUE("cloud.json переписался", mid.open(QIODevice::WriteOnly));
        mid.write("{ \"remoteUrl\": \"https://host2/dav/\", \"remoteUser\": \"ю\" }");
    }
    const ZStorage::Config mid = s.cloudConfig();
    ZT_EQ("remoteUrl прочитан запасным путём", std::string("https://host2/dav/"),
          mid.cloudUrl.toStdString());
    ZT_EQ("remoteUser прочитан запасным путём", std::string("ю"),
          mid.cloudUser.toStdString());

    // Строка списка хранилищ — наоборот, с корнем.
    const QJsonObject entry = cfg.entryJson();
    ZT_EQ("строка списка несёт root", store.root().toStdString(),
          entry.value(QStringLiteral("root")).toString().toStdString());
    ZStorage::Config back;
    back.parse(entry);
    ZT_TRUE("круг строки списка сходится",
            back.root == cfg.root && back.cloudUrl == cfg.cloudUrl &&
                back.cloudUser == cfg.cloudUser && back.timeoutMs == cfg.timeoutMs);
}

void checkProbeCloud() {
    // Разведка диалога первичной настройки: адрес и оба пароля проверяются ДО
    // выбора местного каталога, одной статической функцией.
    zt::MiniStore store, cloudHome;
    const QString cloud = cloudHome.root() + QStringLiteral("/облако");
    ZStorage::Config cfg;
    cfg.cloudDir = cloud;
    FakeSecrets secrets;
    QString err;

    // Недостижимое облако — честный отказ, а не «там пусто».
    ZStorage::CloudProbe probe;
    ZT_TRUE("недостижимое облако отвергнуто",
            !ZStorage::probeCloud(cfg, QString(), QStringLiteral("пароль-шифра"), &probe, &err));

    QString rootTitle;
    {
        ZStorage s(store.root());
        ZT_TRUE("облако заведено",
                s.connectCloud(cfg, QStringLiteral("пароль-шифра"), QString(), secrets, kTiny,
                                nullptr, &err));
        const QString rootId = s.ensureRootNote(&err);
        ZT_TRUE("корень завёлся", !rootId.isEmpty());
        rootTitle = s.info(rootId)->title();
        ZT_TRUE("заливка прошла", s.pushAll(nullptr, &err));
    }

    // Верный пароль: сошлось всё, и разведка называет имя и объём.
    ZT_TRUE(("разведка прошла: " + err.toStdString()).c_str(),
            ZStorage::probeCloud(cfg, QString(), QStringLiteral("пароль-шифра"), &probe, &err));
    ZT_TRUE("манифест увиден", probe.hasManifest);
    ZT_TRUE("конверт увиден", probe.hasKeyfile);
    ZT_TRUE("конверт развернулся", probe.keyOpened);
    ZT_TRUE("id облака назван", !probe.identity.storeId().isEmpty());
    ZT_TRUE("журналы посчитаны", probe.stats.notes >= 1);
    ZT_TRUE("объём посчитан", probe.stats.bytes > 0);
    ZT_TRUE("имя хранилища вскрыто", probe.name == rootTitle);

    // Неверный пароль шифрования — отказ со словами про пароль, но СВОДКА
    // уже заполнена: имена открыты, счёт и объём известны и без пароля.
    ZT_TRUE("неверный пароль отвергнут",
            !ZStorage::probeCloud(cfg, QString(), QStringLiteral("не тот"), &probe, &err));
    ZT_TRUE("сказано про пароль", err.contains(QStringLiteral("password")));
    ZT_TRUE("сводка при неверном пароле есть",
            probe.stats.notes >= 1 && probe.stats.bytes > 0);

    // ПУСТОЙ пароль — не попытка, а отказ от неё (п.9 брифа): сводка без
    // вскрытия конверта, keyOpened ложь, имени честно нет.
    ZT_TRUE("без пароля разведка проходит",
            ZStorage::probeCloud(cfg, QString(), QString(), &probe, &err));
    ZT_TRUE("конверт увиден, но не вскрыт", probe.hasKeyfile && !probe.keyOpened);
    ZT_TRUE("счёт и объём есть", probe.stats.notes >= 1 && probe.stats.bytes > 0);
    ZT_TRUE("имени без ключа нет", probe.name.isEmpty());

    // Пустое, но существующее облако — правда: «там пусто» диалог решает сам.
    const QString blank = cloudHome.root() + QStringLiteral("/пусто");
    QDir().mkpath(blank);
    ZStorage::Config empty;
    empty.cloudDir = blank;
    ZT_TRUE("пустое облако — не ошибка",
            ZStorage::probeCloud(empty, QString(), QStringLiteral("любой"), &probe, &err));
    ZT_TRUE("и в нём ничего нет",
            !probe.hasManifest && !probe.hasKeyfile && probe.stats.notes == 0);
}

void checkResetCloudEncryption() {
    // Сброс пароля шифрования: облачная копия заменяется целиком под новым
    // ключом; старый пароль перестаёт подходить, новый — открывает всё.
    zt::MiniStore store, cloudHome, second;
    const QString cloud = cloudHome.root() + QStringLiteral("/облако");
    ZStorage::Config cfg;
    cfg.cloudDir = cloud;
    FakeSecrets secrets;
    QString err;

    ZStorage s(store.root());
    ZT_TRUE("облако заведено",
            s.connectCloud(cfg, QStringLiteral("старый"), QString(), secrets, kTiny, nullptr,
                            &err));
    const QString rootId = s.ensureRootNote(&err);
    ZT_TRUE("корень завёлся", !rootId.isEmpty());
    const QString noteId = s.createNote(QString(), false, &err);
    ZT_TRUE("заметка завелась", !noteId.isEmpty());
    ZT_TRUE("заливка прошла", s.pushAll(nullptr, &err));

    const auto blobBytes = [&](const QString& name) {
        QFile f(cloud + QLatin1Char('/') + name);
        if (!f.open(QIODevice::ReadOnly)) return QByteArray();
        return f.readAll();
    };
    const QByteArray journalBefore = blobBytes(rootId + QStringLiteral(".zm"));
    ZT_TRUE("журнал корня в облаке", !journalBefore.isEmpty());

    // Пустой новый пароль не бывает паролем.
    ZT_TRUE("пустой пароль отвергнут",
            !s.resetCloudEncryption(cfg, QString(), QString(), secrets, kTiny, nullptr, &err));

    ZStorage::ResetOutcome out;
    ZT_TRUE(("сброс прошёл: " + err.toStdString()).c_str(),
            s.resetCloudEncryption(cfg, QStringLiteral("новый"), QString(), secrets, kTiny, &out,
                                   &err));
    // Стёрто всё прежнее: манифест, конверт и журналы обеих заметок.
    ZT_TRUE("стёрто всё прежнее", out.wiped >= 4);
    ZT_TRUE("журналы залиты заново", out.push.journals >= 2);
    ZT_TRUE("облако осталось подключённым", s.isConnected());
    ZT_TRUE("новый пароль лёг в keyring",
            secrets.cryptPasswords_.value(s.identity().storeId()) ==
                QStringLiteral("новый"));

    // Старый пароль больше не открывает облако, новый — открывает.
    ZStorage::CloudProbe probe;
    ZT_TRUE("старый пароль отвергнут",
            !ZStorage::probeCloud(cfg, QString(), QStringLiteral("старый"), &probe, &err));
    ZT_TRUE(("новый пароль подошёл: " + err.toStdString()).c_str(),
            ZStorage::probeCloud(cfg, QString(), QStringLiteral("новый"), &probe, &err));
    ZT_TRUE("манифест на месте", probe.hasManifest);
    ZT_EQ("storeId не сменился", s.identity().storeId().toStdString(),
          probe.identity.storeId().toStdString());
    // Байты блобов другие: новый ключ, а не переупаковка старого.
    ZT_TRUE("журнал перешифрован",
            blobBytes(rootId + QStringLiteral(".zm")) != journalBefore);

    // Второе устройство встаёт с новым паролем — облако после сброса цельное.
    {
        FakeSecrets fresh;
        ZStorage::ConnectOutcome boot;
        const QString dir = second.root() + QStringLiteral("/копия");
        auto other = ZStorage::initFromCloud(dir, cfg, QStringLiteral("новый"), QString(),
                                              fresh, kTiny, &boot, &err);
        ZT_TRUE(("бутстрап после сброса прошёл: " + err.toStdString()).c_str(),
                other != nullptr);
        if (other != nullptr)
            ZT_TRUE("идентичность унаследована", boot.inheritedIdentity);
    }
}

void checkPushAllPrimesLedger() {
    // ЗАЛИВКА ПИШЕТ БУХГАЛТЕРИЮ САМА (жалоба владельца 30.08.2026: без этого
    // первый прогон после заливки перечитывал всё облако, и лечило его только
    // переоткрытие программы). Ждём: следующий Full-прогон не скачивает ни
    // одного своего блоба и не заливает ничего заново — один листинг, один
    // mkdir и одна проба ключа, а не GET на каждый блоб.
    zt::MiniStore store, cloudHome;
    const QString cloud = cloudHome.root() + QStringLiteral("/облако");
    ZStorage::Config cfg;
    cfg.cloudDir = cloud;
    FakeSecrets secrets;
    QString err;

    ZStorage s(store.root());
    ZT_TRUE("облако заведено",
            s.connectCloud(cfg, QStringLiteral("пароль"), QString(), secrets, kTiny, nullptr,
                            &err));
    const QString rootId = s.ensureRootNote(&err);
    ZT_TRUE("корень завёлся", !rootId.isEmpty());
    ZT_TRUE("заметка завелась", !s.createNote(QString(), false, &err).isEmpty());
    {
        QFile pic(store.root() + QStringLiteral("/") + rootId + QStringLiteral(".webp"));
        ZT_TRUE("вложение записалось", pic.open(QIODevice::WriteOnly));
        pic.write("картинка для заливки");
    }
    s.reload();
    ZT_TRUE("заливка прошла", s.pushAll(nullptr, &err));

    ZStorage::SyncReport report;
    ZT_TRUE(("прогон после заливки прошёл: " + err.toStdString()).c_str(),
            s.sync({}, &report, &err));
    ZT_TRUE("своё не скачивается заново",
            report.takenWhole == 0 && report.attachmentsDown == 0 &&
                report.mergedJournals == 0);
    ZT_TRUE("и не заливается заново", report.pushedWhole == 0 && report.attachmentsUp == 0);
    // Цена прогона: mkdir + листинг + одна проба ключа (у первой встречи с
    // конвертом), БЕЗ GET на каждый блоб. Четыре — потолок с запасом в один.
    ZT_TRUE(("прогон дешёв: " + std::to_string(report.traffic.requests) + " запросов")
                .c_str(),
            report.traffic.requests <= 4);
}

void checkChangeEncryptionPassword() {
    // Смена пароля при живом ключе НИЧЕГО не стирает и не перезаливает:
    // блобы зашифрованы ключом, пароль лишь заворачивает ключ в конверт.
    zt::MiniStore store, cloudHome;
    const QString cloud = cloudHome.root() + QStringLiteral("/облако");
    ZStorage::Config cfg;
    cfg.cloudDir = cloud;
    FakeSecrets secrets;
    QString err;

    ZStorage s(store.root());
    ZT_TRUE("облако заведено",
            s.connectCloud(cfg, QStringLiteral("старый"), QString(), secrets, kTiny, nullptr,
                            &err));
    const QString rootId = s.ensureRootNote(&err);
    ZT_TRUE("корень завёлся", !rootId.isEmpty());
    ZT_TRUE("заливка прошла", s.pushAll(nullptr, &err));

    const auto blobBytes = [&](const QString& name) {
        QFile f(cloud + QLatin1Char('/') + name);
        if (!f.open(QIODevice::ReadOnly)) return QByteArray();
        return f.readAll();
    };
    const QByteArray journalBefore = blobBytes(rootId + QStringLiteral(".zm"));
    ZT_TRUE("журнал корня в облаке", !journalBefore.isEmpty());
    const int objectsBefore = int(QDir(cloud).entryList(QDir::Files).size());

    // Без ключа в связке дороги нет — честный отказ словами про связку.
    {
        FakeSecrets empty;
        ZT_TRUE("без ключа пароль не сменить",
                !s.changeEncryptionPassword(cfg, QStringLiteral("новый"), QString(), empty,
                                            kTiny, &err));
        ZT_TRUE("сказано про keyring", err.contains(QStringLiteral("keyring")));
    }

    ZT_TRUE(("смена пароля прошла: " + err.toStdString()).c_str(),
            s.changeEncryptionPassword(cfg, QStringLiteral("новый"), QString(), secrets,
                                       kTiny, &err));
    // Стёрто 0 объектов, залит 1 конверт: блобы не тронуты побайтово.
    ZT_TRUE("число объектов то же",
            int(QDir(cloud).entryList(QDir::Files).size()) == objectsBefore);
    ZT_TRUE("журнал не перезаливался",
            blobBytes(rootId + QStringLiteral(".zm")) == journalBefore);
    ZT_TRUE("новый пароль лёг в keyring",
            secrets.cryptPasswords_.value(s.identity().storeId()) ==
                QStringLiteral("новый"));

    // Старый пароль конверт больше не открывает, новый — открывает всё.
    ZStorage::CloudProbe probe;
    ZT_TRUE("старый пароль отвергнут",
            !ZStorage::probeCloud(cfg, QString(), QStringLiteral("старый"), &probe, &err));
    ZT_TRUE(("новый пароль подошёл: " + err.toStdString()).c_str(),
            ZStorage::probeCloud(cfg, QString(), QStringLiteral("новый"), &probe, &err));
    ZT_TRUE("конверт развернулся", probe.keyOpened);

    // Другое устройство с ТЕМ ЖЕ ключом в связке продолжает работать: ключ
    // не менялся, сменился только конверт.
    {
        ZStorage again(store.root());
        ZT_TRUE("useLastCloud с прежним ключом жив", again.useLastCloud(secrets, &err));
        ZT_TRUE("подключено", again.isConnected());
    }
}

void checkEraseCloudStorage() {
    zt::MiniStore store, cloudHome;
    const QString cloud = cloudHome.root() + QStringLiteral("/облако");
    ZStorage::Config cfg;
    cfg.cloudDir = cloud;
    FakeSecrets secrets;
    QString err;

    ZStorage s(store.root());
    ZT_TRUE("облако заведено",
            s.connectCloud(cfg, QStringLiteral("пароль"), QString(), secrets, kTiny, nullptr,
                            &err));
    ZT_TRUE("корень завёлся", !s.ensureRootNote(&err).isEmpty());
    ZT_TRUE("заливка прошла", s.pushAll(nullptr, &err));

    // ОТНОСИТЕЛЬНЫЙ каталог-облако отвергается СЛОВАМИ, а не резолвится по
    // cwd (набранное «../..» стоило владельцу каталога, 30.08.2026).
    {
        ZStorage::Config typo;
        typo.cloudDir = QStringLiteral("../..");
        ZT_TRUE("относительный путь отвергнут",
                !s.eraseCloudStorage(typo, QString(), true, nullptr, &err));
        ZT_TRUE("сказано про абсолютный путь", err.contains(QStringLiteral("absolute")));
    }
    // ПРЕДОК корня хранилища не стирается, даже если путь абсолютный.
    {
        ZStorage::Config parent;
        parent.cloudDir = QFileInfo(store.root()).absolutePath();
        ZT_TRUE("предок корня отвергнут",
                !s.eraseCloudStorage(parent, QString(), true, nullptr, &err));
        ZT_TRUE("сказано, что внутри лежит своё",
                err.contains(QStringLiteral("contains")));
    }
    // Каталог без манифеста и с ПОСТОРОННИМ файлом — отказ, называющий файл.
    {
        const QString junk = cloudHome.root() + QStringLiteral("/бумаги");
        QDir().mkpath(junk);
        QFile f(junk + QStringLiteral("/письмо.txt"));
        ZT_TRUE("посторонний файл завёлся", f.open(QIODevice::WriteOnly));
        f.write("не блоб");
        f.close();
        ZStorage::Config other;
        other.cloudDir = junk;
        ZT_TRUE("посторонняя папка отвергнута",
                !s.eraseCloudStorage(other, QString(), true, nullptr, &err));
        ZT_TRUE("файл назван по имени", err.contains(QStringLiteral("письмо.txt")));
        ZT_TRUE("файл цел", QFile::exists(junk + QStringLiteral("/письмо.txt")));
    }

    // Своё облако: erase(keepFolder) оставляет ПУСТУЮ папку, готовую принять
    // заливку; повторный erase по пустой — не беда (идемпотентно).
    ZStorage::EraseOutcome out;
    ZT_TRUE(("стирание прошло: " + err.toStdString()).c_str(),
            s.eraseCloudStorage(cfg, QString(), /*keepFolder=*/true, &out, &err));
    ZT_TRUE("объекты были посчитаны", out.wiped >= 3);
    ZT_TRUE("папка осталась", QDir(cloud).exists());
    ZT_TRUE("и пуста", QDir(cloud).entryList(QDir::Files | QDir::Hidden).isEmpty());
    ZT_TRUE("облако отцеплено", !s.isConnected());
    ZT_TRUE("повтор по пустому — не беда",
            s.eraseCloudStorage(cfg, QString(), true, nullptr, &err));

    // erase + init + pushAll == первая заливка с нуля: бутстрап встаёт.
    ZT_TRUE(("засев прошёл: " + err.toStdString()).c_str(),
            s.initCloudStorage(cfg, QStringLiteral("свежий"), QString(), secrets, kTiny,
                               &err));
    ZT_TRUE("после засева подключено", s.isConnected());
    ZT_TRUE("конверт в облаке", QFile::exists(cloud + QStringLiteral("/keyfile")));
    // Манифест НЕ заливается засевом — его несёт заливка данных, последним.
    ZT_TRUE("манифеста после засева нет",
            !QFile::exists(cloud + QStringLiteral("/zametti.json")));
    ZT_TRUE("заливка прошла", s.pushAll(nullptr, &err));
    ZT_TRUE("теперь манифест на месте",
            QFile::exists(cloud + QStringLiteral("/zametti.json")));
    // Живое облако не засеивают поверх.
    ZT_TRUE("повторный засев отвергнут",
            !s.initCloudStorage(cfg, QStringLiteral("ещё"), QString(), secrets, kTiny, &err));
    ZT_TRUE("сказано про существующий конверт",
            err.contains(QStringLiteral("keyfile")));

    // Чужой манифест — отказ до единого удаления.
    {
        zt::MiniStore foreignHome;
        ZT_TRUE("чужое хранилище завелось",
                ZStorage(foreignHome.root() + QStringLiteral("/чужое")).init(&err));
        ZStorage reopened(foreignHome.root() + QStringLiteral("/чужое"));
        ZT_TRUE("чужая идентичность отчеканилась",
                !reopened.ensureIdentity(&err).isEmpty());
        ZT_TRUE("чужому стирать наше нельзя",
                !reopened.eraseCloudStorage(cfg, QString(), true, nullptr, &err));
        ZT_TRUE("причина называет чужой store",
                err.contains(QStringLiteral("another store")));
        ZT_TRUE("манифест цел", QFile::exists(cloud + QStringLiteral("/zametti.json")));
    }
}

void checkResetRefusesForeignCloud() {
    // Сброс — единственная стирающая операция, и стирает он ТОЛЬКО своё:
    // опечатка в адресе не должна стоить человеку чужой облачной копии.
    zt::MiniStore mine, cloudHome, foreign;
    const QString cloud = cloudHome.root() + QStringLiteral("/облако");
    ZStorage::Config cfg;
    cfg.cloudDir = cloud;
    FakeSecrets secrets;
    QString err;
    {
        ZStorage s(mine.root());
        ZT_TRUE("своё облако заведено",
                s.connectCloud(cfg, QStringLiteral("пароль-шифра"), QString(), secrets, kTiny,
                                nullptr, &err));
        ZT_TRUE("корень завёлся", !s.ensureRootNote(&err).isEmpty());
        ZT_TRUE("манифест уехал", s.pushAll(nullptr, &err));
    }
    ZStorage other(foreign.root());
    ZT_TRUE("чужая идентичность отчеканилась", !other.ensureIdentity(&err).isEmpty());
    const QByteArray manifestBefore = [&] {
        QFile f(cloud + QStringLiteral("/zametti.json"));
        if (!f.open(QIODevice::ReadOnly)) return QByteArray();
        return f.readAll();
    }();
    ZT_TRUE("манифест облака есть", !manifestBefore.isEmpty());
    FakeSecrets fresh;
    ZT_TRUE("чужое облако не стёрто",
            !other.resetCloudEncryption(cfg, QStringLiteral("новый"), QString(), fresh, kTiny,
                                        nullptr, &err));
    ZT_TRUE("причина называет чужой store", err.contains(QStringLiteral("another store")));
    QFile f(cloud + QStringLiteral("/zametti.json"));
    ZT_TRUE("манифест открылся", f.open(QIODevice::ReadOnly));
    ZT_TRUE("манифест облака не тронут", f.readAll() == manifestBefore);
}

}  // namespace

static int ztRunSuite(int argc, char** argv) {
    (void)argc;
    (void)argv;
    checkConnectMintsAndRemembers();
    checkReconnectUnwrapsExistingKeyfile();
    checkUseLastCloud();
    checkBootstrapInheritsIdentity();
    checkBootstrapIntoEmptyDir();
    checkBootstrapFromLegacyCloud();
    checkForeignCloudRefused();
    checkConfigReadsLegacyKeys();
    checkProbeCloud();
    checkResetCloudEncryption();
    checkPushAllPrimesLedger();
    checkChangeEncryptionPassword();
    checkEraseCloudStorage();
    checkResetRefusesForeignCloud();
    return zt::report("set_cloud");
}

TEST(SetCloud, All) {
    std::vector<QByteArray> ztArgs{QByteArrayLiteral("set_cloud_test")};
    std::vector<char*> ztArgv;
    for (QByteArray& a : ztArgs) ztArgv.push_back(a.data());
    EXPECT_EQ(0, ztRunSuite(int(ztArgv.size()), ztArgv.data()));
}
