// ПОДКЛЮЧЕНИЕ ОБЛАКА: set-remote, useLastRemote, бутстрап, отвязка.
//
// Все ветки знакомства с облаком в одном месте (ZStorage::connectRemote):
// пустое облако чеканит keyfile, своё облако разворачивает его паролем,
// чужое — честная остановка до единой записи, пустое хранилище наследует
// идентичность манифеста (бутстрап). Облако здесь — каталог (FolderRemote):
// логика подключения от транспорта не зависит.

#include "keyfile.h"
#include "secret_store.h"
#include "zstorage.h"

#include "mini_store.h"
#include "test_util.h"

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

// Keyring в памяти: наборам не место в настоящем Secret Service.
class FakeSecrets : public SecretStore {
public:
    bool available() const override { return true; }
    bool loadKey(const QString& storeId, Keyfile* out, QString* error) override {
        if (!keys_.contains(storeId)) {
            if (error) *error = QStringLiteral("нет ключа для %1").arg(storeId);
            return false;
        }
        *out = keys_.value(storeId);
        return true;
    }
    bool storeKey(const Keyfile& keyfile, QString*) override {
        keys_.insert(keyfile.storeId(), keyfile);
        return true;
    }
    bool clearKey(const QString& storeId, QString*) override {
        keys_.remove(storeId);
        return true;
    }
    QString serverPassword(const QString& storeId, QString*) override {
        return passwords_.value(storeId);
    }
    bool setServerPassword(const QString& storeId, const QString& password, QString*) override {
        passwords_.insert(storeId, password);
        return true;
    }
    bool clearServerPassword(const QString& storeId, QString*) override {
        passwords_.remove(storeId);
        return true;
    }
    QString encryptionPassword(const QString& storeId, QString*) override {
        return cryptPasswords_.value(storeId);
    }
    bool setEncryptionPassword(const QString& storeId, const QString& password,
                               QString*) override {
        cryptPasswords_.insert(storeId, password);
        return true;
    }
    bool clearEncryptionPassword(const QString& storeId, QString*) override {
        cryptPasswords_.remove(storeId);
        return true;
    }

    QHash<QString, Keyfile> keys_;
    QHash<QString, QString> passwords_;
    QHash<QString, QString> cryptPasswords_;
};

void checkConnectMintsAndRemembers() {
    zt::MiniStore store, cloudHome;
    const QString cloud = cloudHome.root() + QStringLiteral("/облако");
    ZStorage s(store.root());
    FakeSecrets secrets;
    ZStorage::Config cfg;
    cfg.remoteDir = cloud;

    ZStorage::ConnectOutcome out;
    QString err;
    ZT_TRUE(("подключение прошло: " + err.toStdString()).c_str(),
            s.connectRemote(cfg, QStringLiteral("пароль-шифра"), QString(), secrets, kTiny,
                            &out, &err));
    ZT_TRUE("keyfile отчеканен", out.mintedKeyfile);
    ZT_TRUE("облако подключено", s.hasRemote());
    ZT_TRUE("keyfile лежит в облаке",
            QFile::exists(cloud + QStringLiteral("/keyfile")));
    ZT_TRUE("адрес запомнен", s.remoteConfig().hasCloud());
    ZT_TRUE("ключ лёг в keyring", secrets.keys_.size() == 1);
    // Пароль шифрования хранится (решение владельца 28.08.2026): он для глаз
    // человека, и кладётся только после удачи.
    ZT_TRUE("пароль шифрования лёг в keyring",
            secrets.cryptPasswords_.value(s.identity().storeId()) ==
                QStringLiteral("пароль-шифра"));

    // Пустой пароль шифрования не бывает паролем.
    ZT_TRUE("пустой пароль отвергнут",
            !s.connectRemote(cfg, QString(), QString(), secrets, kTiny, nullptr, &err));
}

void checkReconnectUnwrapsExistingKeyfile() {
    zt::MiniStore store, cloudHome;
    const QString cloud = cloudHome.root() + QStringLiteral("/облако");
    ZStorage::Config cfg;
    cfg.remoteDir = cloud;
    FakeSecrets secrets;
    QString err;
    {
        ZStorage s(store.root());
        ZT_TRUE("первое подключение прошло",
                s.connectRemote(cfg, QStringLiteral("пароль-шифра"), QString(), secrets, kTiny,
                                nullptr, &err));
    }
    // Переподключение (утраченный keyring): тот же пароль будит тот же ключ.
    {
        ZStorage s(store.root());
        FakeSecrets fresh;
        ZStorage::ConnectOutcome out;
        ZT_TRUE(("переподключение прошло: " + err.toStdString()).c_str(),
                s.connectRemote(cfg, QStringLiteral("пароль-шифра"), QString(), fresh, kTiny,
                                &out, &err));
        ZT_TRUE("keyfile не перечеканен", !out.mintedKeyfile);
        ZT_TRUE("ключ снова в keyring", fresh.keys_.size() == 1);
    }
    // Неверный пароль — отказ с честными словами.
    {
        ZStorage s(store.root());
        FakeSecrets fresh;
        ZT_TRUE("неверный пароль отвергнут",
                !s.connectRemote(cfg, QStringLiteral("не тот пароль"), QString(), fresh, kTiny,
                                 nullptr, &err));
        ZT_TRUE("сказано про пароль", err.contains(QStringLiteral("password")));
        ZT_TRUE("ключ в keyring не лёг", fresh.keys_.isEmpty());
    }
}

void checkUseLastRemote() {
    zt::MiniStore store, cloudHome;
    const QString cloud = cloudHome.root() + QStringLiteral("/облако");
    ZStorage::Config cfg;
    cfg.remoteDir = cloud;
    FakeSecrets secrets;
    QString err;
    {
        ZStorage s(store.root());
        ZT_TRUE("подключение прошло",
                s.connectRemote(cfg, QStringLiteral("пароль-шифра"), QString(), secrets, kTiny,
                                nullptr, &err));
    }
    // Новый запуск: адрес из remote.json, ключ из keyring, ноль вопросов.
    {
        ZStorage s(store.root());
        ZT_TRUE(("useLastRemote прошёл: " + err.toStdString()).c_str(),
                s.useLastRemote(secrets, &err));
        ZT_TRUE("облако подключено", s.hasRemote());
    }
    // Пустой keyring — тихий отказ с понятной причиной, не падение.
    {
        ZStorage s(store.root());
        FakeSecrets empty;
        ZT_TRUE("без ключа не подключается", !s.useLastRemote(empty, &err));
        ZT_TRUE("сказано про keyring", err.contains(QStringLiteral("keyring")));
    }
    // Отвязка: адрес забыт, подключение больше не восстанавливается.
    {
        ZStorage s(store.root());
        ZT_TRUE("отвязка прошла", s.clearRemoteConfig(&err));
        ZT_TRUE("конфиг без облака", !s.remoteConfig().hasCloud());
        ZT_TRUE("после отвязки не подключается", !s.useLastRemote(secrets, &err));
        ZT_TRUE("сказано «не настроен»", err.contains(QStringLiteral("not configured")));
    }
}

void checkBootstrapInheritsIdentity() {
    zt::MiniStore first, cloudHome, second;
    const QString cloud = cloudHome.root() + QStringLiteral("/облако");
    ZStorage::Config cfg;
    cfg.remoteDir = cloud;
    FakeSecrets secrets;
    QString err;
    QString firstId;
    QString rootId;
    {
        ZStorage s(first.root());
        ZT_TRUE("первое устройство подключилось",
                s.connectRemote(cfg, QStringLiteral("пароль-шифра"), QString(), secrets, kTiny,
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
                s.connectRemote(cfg, QStringLiteral("пароль-шифра"), QString(), fresh, kTiny,
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
    // Новое устройство: каталога ещё НЕТ ВОВСЕ — set-remote сам заводит
    // каркас (без чеканки идентичности!) и наследует id из манифеста.
    zt::MiniStore first, cloudHome, home;
    const QString cloud = cloudHome.root() + QStringLiteral("/облако");
    ZStorage::Config cfg;
    cfg.remoteDir = cloud;
    FakeSecrets secrets;
    QString err;
    QString firstId;
    {
        ZStorage s(first.root());
        ZT_TRUE("первое устройство подключилось",
                s.connectRemote(cfg, QStringLiteral("пароль-шифра"), QString(), secrets, kTiny,
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
        auto s = ZStorage::initFromRemote(fresh, cfg, QStringLiteral("пароль-шифра"),
                                          QString(), mine, kTiny, &out, &err);
        ZT_TRUE(("бутстрап в пустоту прошёл: " + err.toStdString()).c_str(), s != nullptr);
        if (s == nullptr) return;
        ZT_TRUE("теперь это хранилище", s->isStore());
        ZT_TRUE("хранилище уже подключено", s->hasRemote());
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
        empty.remoteDir = cloudHome.root() + QStringLiteral("/облако-с-опечаткой");
        ZT_TRUE("пусто с обеих сторон отвергнуто",
                ZStorage::initFromRemote(typo, empty, QStringLiteral("пароль-шифра"),
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
                ZStorage::initFromRemote(junk, cfg, QStringLiteral("пароль-шифра"), QString(),
                                         mine, kTiny, nullptr, &err) == nullptr);
        ZT_TRUE("причина — непустой каталог", err.contains(QStringLiteral("not empty")));
    }
}

void checkForeignCloudRefused() {
    zt::MiniStore mine, cloudHome, foreign;
    const QString cloud = cloudHome.root() + QStringLiteral("/облако");
    ZStorage::Config cfg;
    cfg.remoteDir = cloud;
    FakeSecrets secrets;
    QString err;
    QString mineId;
    QString mineRootTitle;
    {
        ZStorage s(mine.root());
        ZT_TRUE("своё облако заведено",
                s.connectRemote(cfg, QStringLiteral("пароль-шифра"), QString(), secrets, kTiny,
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
                !s.connectRemote(cfg, QStringLiteral("пароль-шифра"), QString(), fresh, kTiny,
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
        ZT_TRUE("remote.json не записан", !s.remoteConfig().hasCloud());
        QFile f(cloud + QStringLiteral("/keyfile"));
        ZT_TRUE("keyfile открылся", f.open(QIODevice::ReadOnly));
        ZT_TRUE("keyfile облака не тронут", f.readAll() == keyfileBefore);
    }
}

void checkConfigReadsLegacyKeys() {
    // remote.json прежних сборок писал ключи url/dir/user — он обязан
    // читаться (запасной путь читателя), а при следующей записи мигрировать
    // на новые ключи remoteUrl/remoteDir/remoteUser.
    zt::MiniStore store;
    ZStorage s(store.root());
    {
        QFile f(store.root() + QStringLiteral("/.zametti/remote.json"));
        ZT_TRUE("старый remote.json записался", f.open(QIODevice::WriteOnly));
        f.write("{ \"url\": \"https://host/dav/\", \"user\": \"вадим\", "
                "\"timeoutMs\": 7000 }");
    }
    ZStorage::Config cfg = s.remoteConfig();
    ZT_TRUE("облако прочитано", cfg.hasCloud());
    ZT_EQ("url со старого ключа", std::string("https://host/dav/"),
          cfg.remoteUrl.toStdString());
    ZT_EQ("логин со старого ключа", std::string("вадим"), cfg.remoteUser.toStdString());
    ZT_TRUE("таймаут прочитан", cfg.timeoutMs == 7000);
    ZT_EQ("root — корень копии, не из файла", store.root().toStdString(),
          cfg.root.toStdString());

    QString err;
    ZT_TRUE("перезапись прошла", s.writeRemoteConfig(cfg, &err));
    QFile f(store.root() + QStringLiteral("/.zametti/remote.json"));
    ZT_TRUE("файл открылся", f.open(QIODevice::ReadOnly));
    const QByteArray bytes = f.readAll();
    ZT_TRUE("мигрировал на новый ключ", bytes.contains("remoteUrl"));
    ZT_TRUE("логин на новом ключе", bytes.contains("remoteUser"));
    // Путь в файл не пишется: переехал бы вместе с каталогом и врал.
    ZT_TRUE("root в remote.json не уезжает", !bytes.contains("\"root\""));

    // Строка списка хранилищ — наоборот, с корнем.
    const QJsonObject entry = cfg.entryJson();
    ZT_EQ("строка списка несёт root", store.root().toStdString(),
          entry.value(QStringLiteral("root")).toString().toStdString());
    ZStorage::Config back;
    back.parse(entry);
    ZT_TRUE("круг строки списка сходится",
            back.root == cfg.root && back.remoteUrl == cfg.remoteUrl &&
                back.remoteUser == cfg.remoteUser && back.timeoutMs == cfg.timeoutMs);
}

void checkProbeCloud() {
    // Разведка диалога первичной настройки: адрес и оба пароля проверяются ДО
    // выбора местного каталога, одной статической функцией.
    zt::MiniStore store, cloudHome;
    const QString cloud = cloudHome.root() + QStringLiteral("/облако");
    ZStorage::Config cfg;
    cfg.remoteDir = cloud;
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
                s.connectRemote(cfg, QStringLiteral("пароль-шифра"), QString(), secrets, kTiny,
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
    ZT_TRUE("журналы посчитаны", probe.notes >= 1);
    ZT_TRUE("объём посчитан", probe.bytes > 0);
    ZT_TRUE("имя хранилища вскрыто", probe.name == rootTitle);

    // Неверный пароль шифрования — отказ со словами про пароль.
    ZT_TRUE("неверный пароль отвергнут",
            !ZStorage::probeCloud(cfg, QString(), QStringLiteral("не тот"), &probe, &err));
    ZT_TRUE("сказано про пароль", err.contains(QStringLiteral("password")));

    // Пустое, но существующее облако — правда: «там пусто» диалог решает сам.
    const QString blank = cloudHome.root() + QStringLiteral("/пусто");
    QDir().mkpath(blank);
    ZStorage::Config empty;
    empty.remoteDir = blank;
    ZT_TRUE("пустое облако — не ошибка",
            ZStorage::probeCloud(empty, QString(), QStringLiteral("любой"), &probe, &err));
    ZT_TRUE("и в нём ничего нет",
            !probe.hasManifest && !probe.hasKeyfile && probe.notes == 0);
}

void checkResetCloudEncryption() {
    // Сброс пароля шифрования: облачная копия заменяется целиком под новым
    // ключом; старый пароль перестаёт подходить, новый — открывает всё.
    zt::MiniStore store, cloudHome, second;
    const QString cloud = cloudHome.root() + QStringLiteral("/облако");
    ZStorage::Config cfg;
    cfg.remoteDir = cloud;
    FakeSecrets secrets;
    QString err;

    ZStorage s(store.root());
    ZT_TRUE("облако заведено",
            s.connectRemote(cfg, QStringLiteral("старый"), QString(), secrets, kTiny, nullptr,
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
    const QByteArray journalBefore = blobBytes(rootId + QStringLiteral(".log"));
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
    ZT_TRUE("облако осталось подключённым", s.hasRemote());
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
            blobBytes(rootId + QStringLiteral(".log")) != journalBefore);

    // Второе устройство встаёт с новым паролем — облако после сброса цельное.
    {
        FakeSecrets fresh;
        ZStorage::ConnectOutcome boot;
        const QString dir = second.root() + QStringLiteral("/копия");
        auto other = ZStorage::initFromRemote(dir, cfg, QStringLiteral("новый"), QString(),
                                              fresh, kTiny, &boot, &err);
        ZT_TRUE(("бутстрап после сброса прошёл: " + err.toStdString()).c_str(),
                other != nullptr);
        if (other != nullptr)
            ZT_TRUE("идентичность унаследована", boot.inheritedIdentity);
    }
}

void checkResetRefusesForeignCloud() {
    // Сброс — единственная стирающая операция, и стирает он ТОЛЬКО своё:
    // опечатка в адресе не должна стоить человеку чужой облачной копии.
    zt::MiniStore mine, cloudHome, foreign;
    const QString cloud = cloudHome.root() + QStringLiteral("/облако");
    ZStorage::Config cfg;
    cfg.remoteDir = cloud;
    FakeSecrets secrets;
    QString err;
    {
        ZStorage s(mine.root());
        ZT_TRUE("своё облако заведено",
                s.connectRemote(cfg, QStringLiteral("пароль-шифра"), QString(), secrets, kTiny,
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
    checkUseLastRemote();
    checkBootstrapInheritsIdentity();
    checkBootstrapIntoEmptyDir();
    checkForeignCloudRefused();
    checkConfigReadsLegacyKeys();
    checkProbeCloud();
    checkResetCloudEncryption();
    checkResetRefusesForeignCloud();
    return zt::report("set_remote");
}

TEST(SetRemote, All) {
    std::vector<QByteArray> ztArgs{QByteArrayLiteral("set_remote_test")};
    std::vector<char*> ztArgv;
    for (QByteArray& a : ztArgs) ztArgv.push_back(a.data());
    EXPECT_EQ(0, ztRunSuite(int(ztArgv.size()), ztArgv.data()));
}
