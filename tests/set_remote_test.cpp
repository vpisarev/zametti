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

    QHash<QString, Keyfile> keys_;
    QHash<QString, QString> passwords_;
};

void checkConnectMintsAndRemembers() {
    zt::MiniStore store, cloudHome;
    const QString cloud = cloudHome.root() + QStringLiteral("/облако");
    ZStorage s(store.root());
    FakeSecrets secrets;
    ZStorage::RemoteConfig cfg;
    cfg.dir = cloud;

    ZStorage::ConnectOutcome out;
    QString err;
    ZT_TRUE(("подключение прошло: " + err.toStdString()).c_str(),
            s.connectRemote(cfg, QStringLiteral("пароль-шифра"), QString(), secrets, kTiny,
                            &out, &err));
    ZT_TRUE("keyfile отчеканен", out.mintedKeyfile);
    ZT_TRUE("облако подключено", s.hasRemote());
    ZT_TRUE("keyfile лежит в облаке",
            QFile::exists(cloud + QStringLiteral("/keyfile")));
    ZT_TRUE("адрес запомнен", !s.remoteConfig().isEmpty());
    ZT_TRUE("ключ лёг в keyring", secrets.keys_.size() == 1);

    // Пустой пароль шифрования не бывает паролем.
    ZT_TRUE("пустой пароль отвергнут",
            !s.connectRemote(cfg, QString(), QString(), secrets, kTiny, nullptr, &err));
}

void checkReconnectUnwrapsExistingKeyfile() {
    zt::MiniStore store, cloudHome;
    const QString cloud = cloudHome.root() + QStringLiteral("/облако");
    ZStorage::RemoteConfig cfg;
    cfg.dir = cloud;
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
    ZStorage::RemoteConfig cfg;
    cfg.dir = cloud;
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
        ZT_TRUE("конфиг пуст", s.remoteConfig().isEmpty());
        ZT_TRUE("после отвязки не подключается", !s.useLastRemote(secrets, &err));
        ZT_TRUE("сказано «не настроен»", err.contains(QStringLiteral("not configured")));
    }
}

void checkBootstrapInheritsIdentity() {
    zt::MiniStore first, cloudHome, second;
    const QString cloud = cloudHome.root() + QStringLiteral("/облако");
    ZStorage::RemoteConfig cfg;
    cfg.dir = cloud;
    FakeSecrets secrets;
    QString err;
    QString firstId;
    {
        ZStorage s(first.root());
        ZT_TRUE("первое устройство подключилось",
                s.connectRemote(cfg, QStringLiteral("пароль-шифра"), QString(), secrets, kTiny,
                                nullptr, &err));
        firstId = s.identity().storeId();
        // Манифест кладёт заливка — без него бутстрапу неоткуда узнать id.
        ZT_TRUE("заливка прошла", s.pushAll(nullptr, &err));
    }
    {
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
    }
}

void checkForeignCloudRefused() {
    zt::MiniStore mine, cloudHome, foreign;
    const QString cloud = cloudHome.root() + QStringLiteral("/облако");
    ZStorage::RemoteConfig cfg;
    cfg.dir = cloud;
    FakeSecrets secrets;
    QString err;
    {
        ZStorage s(mine.root());
        ZT_TRUE("своё облако заведено",
                s.connectRemote(cfg, QStringLiteral("пароль-шифра"), QString(), secrets, kTiny,
                                nullptr, &err));
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
        ZT_TRUE("remote.json не записан", s.remoteConfig().isEmpty());
        QFile f(cloud + QStringLiteral("/keyfile"));
        ZT_TRUE("keyfile открылся", f.open(QIODevice::ReadOnly));
        ZT_TRUE("keyfile облака не тронут", f.readAll() == keyfileBefore);
    }
}

}  // namespace

static int ztRunSuite(int argc, char** argv) {
    (void)argc;
    (void)argv;
    checkConnectMintsAndRemembers();
    checkReconnectUnwrapsExistingKeyfile();
    checkUseLastRemote();
    checkBootstrapInheritsIdentity();
    checkForeignCloudRefused();
    return zt::report("set_remote");
}

TEST(SetRemote, All) {
    std::vector<QByteArray> ztArgs{QByteArrayLiteral("set_remote_test")};
    std::vector<char*> ztArgv;
    for (QByteArray& a : ztArgs) ztArgv.push_back(a.data());
    EXPECT_EQ(0, ztRunSuite(int(ztArgv.size()), ztArgv.data()));
}
