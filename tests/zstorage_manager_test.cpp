// ZStorageManager: факты строк списка хранилищ.
//
// Круг «запомнили — записали — прочитали» секции "stores" живёт в
// session_test (там же канонизация и дедупликация); здесь — то, что менеджер
// знает сверх списка: какого рода каталог, сводка хранилища и что про него
// говорит связка. Главный инвариант — §3.16 матрицы: факты СПРАШИВАЮТ связку
// (has), но НИ РАЗУ не читают секрет — на маке чтение вправе поднять
// системный вопрос, а факты пересчитываются на каждое переключение строки.

#include "zstorage_manager.h"

#include "fake_secrets.h"
#include "mini_store.h"
#include "test_util.h"

#include <QDir>
#include <QFile>

#include <memory>
#include <string>
#include <vector>

using namespace zametti;

namespace {

void checkKinds() {
    zt::MiniStore home;
    ZStorageManager manager;

    ZT_TRUE("пустой путь — Missing",
            manager.facts(QString()).kind == ZStorage::DirKind::Missing);
    ZT_TRUE("несуществующий каталог — Missing",
            manager.facts(home.root() + QStringLiteral("/нет-такого")).kind ==
                ZStorage::DirKind::Missing);

    const QString empty = home.root() + QStringLiteral("/пустой");
    QDir().mkpath(empty);
    ZT_TRUE("пустой каталог — Empty",
            manager.facts(empty).kind == ZStorage::DirKind::Empty);

    const QString foreign = home.root() + QStringLiteral("/чужой");
    QDir().mkpath(foreign);
    QFile junk(foreign + QStringLiteral("/письмо.txt"));
    ZT_TRUE("чужой файл завёлся", junk.open(QIODevice::WriteOnly));
    junk.close();
    ZT_TRUE("непустой каталог без метки — Foreign",
            manager.facts(foreign).kind == ZStorage::DirKind::Foreign);
    ZT_TRUE("у не-хранилища сводка пуста", manager.facts(foreign).stats.isEmpty());
}

void checkStoreFactsAndSecrets() {
    zt::MiniStore home;
    const QString root = home.root() + QStringLiteral("/хранилище");
    QString err;
    ZT_TRUE("хранилище завелось", ZStorage(root).init(&err));
    QString storeId;
    {
        ZStorage s(root);
        ZT_TRUE("корень завёлся", !s.ensureRootNote(&err).isEmpty());
        storeId = s.identity().storeId();
    }
    auto secrets = std::make_shared<zt::FakeSecrets>();
    secrets->pretendKey(storeId);
    secrets->setServerPassword(storeId, QStringLiteral("пароль"), nullptr);

    ZStorageManager manager(secrets);
    const ZStorageManager::Facts f = manager.facts(root);
    ZT_TRUE("это хранилище", f.kind == ZStorage::DirKind::Store);
    ZT_TRUE("заметки посчитаны", f.stats.notes >= 1);
    ZT_TRUE("дата правки есть", f.stats.lastModified.isValid());
    ZT_TRUE("ключ: связка сказала «да»", f.key == ZStorageManager::Known::Yes);
    ZT_TRUE("пароль сервера: «да»",
            f.serverPassword == ZStorageManager::Known::Yes);
    ZT_TRUE("пароль шифрования: «нет»",
            f.encryptionPassword == ZStorageManager::Known::No);
    // §3.16: спрошено — да; ПРОЧИТАНО — ноль. Показ не читает секретов.
    ZT_TRUE("связку спрашивали", secrets->asked > 0);
    ZT_EQ("секретов не читали ни разу", std::string("0"),
          std::to_string(secrets->reads));

    // Сводка легла и в строку списка (Config::local) — показ, не формат.
    ZStorage::Config row;
    row.root = root;
    manager.remember(row);
    manager.facts(root);
    ZT_TRUE("строка списка несёт сводку",
            manager.storeFor(root).local.notes >= 1);
    ZT_TRUE("в JSON сводка не уезжает",
            !manager.storesToJson().first().toObject().contains(
                QStringLiteral("local")));

    // Без связки — честное «неизвестно», а не выдуманное «нет».
    ZStorageManager blind;
    ZT_TRUE("без связки — Unknown",
            blind.facts(root).key == ZStorageManager::Known::Unknown);
}

void checkFactsCacheAndRefresh() {
    zt::MiniStore home;
    const QString root = home.root() + QStringLiteral("/хранилище");
    QString err;
    ZT_TRUE("хранилище завелось", ZStorage(root).init(&err));
    {
        ZStorage s(root);
        ZT_TRUE("корень завёлся", !s.ensureRootNote(&err).isEmpty());
    }
    ZStorageManager manager;
    const int before = manager.facts(root).stats.notes;
    {
        ZStorage s(root);
        s.reload();
        ZT_TRUE("вторая заметка завелась",
                !s.createNote(QString(), false, &err).isEmpty());
    }
    // Кэш держит прежний ответ, пока работу не назвали (refresh) — факты не
    // пересчитываются на каждое нажатие клавиши в поле пути.
    ZT_TRUE("до refresh — прежний ответ", manager.facts(root).stats.notes == before);
    manager.refresh(root);
    ZT_TRUE("после refresh заметка видна",
            manager.facts(root).stats.notes == before + 1);
}

}  // namespace

static int ztRunSuite(int, char**) {
    checkKinds();
    checkStoreFactsAndSecrets();
    checkFactsCacheAndRefresh();
    return zt::report("zstorage_manager");
}

TEST(ZStorageManager, All) {
    std::vector<QByteArray> ztArgs{QByteArrayLiteral("zstorage_manager_test")};
    std::vector<char*> ztArgv;
    for (QByteArray& a : ztArgs) ztArgv.push_back(a.data());
    EXPECT_EQ(0, ztRunSuite(int(ztArgv.size()), ztArgv.data()));
}
