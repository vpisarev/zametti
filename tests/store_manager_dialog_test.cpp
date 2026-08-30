// Диалог управления хранилищами: список, добавление, скачивание из облака,
// свежее облако с двойным паролем, сброс пароля шифрования, «−».
//
// Облако — каталог (FolderCloud), keyring — в памяти, Аргон — крошечный:
// проверяется ПРОВОДКА диалога и то, что каждая ветка доезжает до ядра и
// возвращается итогом. Рабочий поток настоящий — ожидание идёт по кнопке
// Apply: занятое окно её гасит, освободившееся возвращает.

#include "store_manager_dialog.h"

#include "keyfile.h"
#include "secret_store.h"
#include "zstorage.h"

#include "fake_secrets.h"
#include "mini_store.h"
#include "test_util.h"
#include "testdata.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QHash>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QThread>

#include <memory>
#include <string>
#include <vector>

using namespace zametti;

namespace {

const Keyfile::KdfParams kTiny{1, 1 << 20};

std::string s(const QString& q) { return q.toStdString(); }

// Keyring в памяти — общая подделка из fake_secrets.h (до 29.08.2026 таких
// классов по наборам лежало три, слово в слово).
using FakeSecrets = zt::FakeSecrets;

// Наборам — вход в режим сброса мимо модального переспроса.
class TestDialog : public StoreManagerDialog {
public:
    using StoreManagerDialog::StoreManagerDialog;
    using StoreManagerDialog::armResetMode;
};

// Каталог снимков — ОДИН РАЗ на набор: outDir чистит каталог при каждом
// вызове, и второй снимок стирал бы первый.
const QString& shotDir() {
    static const QString dir = zt::TestData::outDir(QStringLiteral("store-manager"));
    return dir;
}

// Дождаться конца рабочего потока: занятое окно гасит Apply.
bool waitIdle(StoreManagerDialog& dialog, int budgetMs = 20000) {
    auto* apply = dialog.findChild<QPushButton*>(QStringLiteral("apply"));
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < budgetMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        if (apply->isEnabled()) return true;
        QThread::msleep(10);
    }
    return false;
}

QString statusText(StoreManagerDialog& dialog) {
    return dialog.findChild<QLabel*>(QStringLiteral("status"))->text();
}

void checkCloudAddress() {
    ZStorage::Config cfg;
    StoreManagerDialog::setCloudAddress(cfg, QStringLiteral("https://host/dav/notes"),
                                        QStringLiteral(" вадим "));
    ZT_EQ("url получает хвостовой слэш", std::string("https://host/dav/notes/"),
          s(cfg.cloudUrl));
    ZT_TRUE("каталог пуст при url", cfg.cloudDir.isEmpty());
    ZT_EQ("логин обрезан", std::string("вадим"), s(cfg.cloudUser));

    StoreManagerDialog::setCloudAddress(cfg, QStringLiteral("/mnt/nas/облако"), QString());
    ZT_TRUE("путь стал каталогом-облаком", cfg.cloudUrl.isEmpty() &&
                cfg.cloudDir == QStringLiteral("/mnt/nas/облако"));

    StoreManagerDialog::setCloudAddress(cfg, QString(), QString());
    ZT_TRUE("пустая строка — облака нет", !cfg.hasCloudAddress());
}

void checkListAndForget() {
    zt::MiniStore a, b;
    ZStorage::Config first;
    first.root = a.root();
    first.name = QStringLiteral("Первое");
    ZStorage::Config second;
    second.root = b.root();
    auto secrets = std::make_shared<FakeSecrets>();

    StoreManagerDialog dialog(nullptr, {first, second}, a.root(), nullptr, secrets, kTiny);
    auto* list = dialog.findChild<QListWidget*>(QStringLiteral("storeList"));
    ZT_EQ("в списке две строки", std::string("2"), std::to_string(list->count()));
    ZT_TRUE("открытое помечено", list->item(0)->text().contains(QStringLiteral("open")));
    ZT_TRUE("выбрана строка открытого", list->currentRow() == 0);
    auto* remove = dialog.findChild<QPushButton*>(QStringLiteral("removeStore"));
    ZT_TRUE("«−» у открытого погашен", !remove->isEnabled());

    // Снимок приёмки: список с двумя строками и форма выбранной.
    dialog.resize(760, 420);
    dialog.grab().save(QDir(shotDir()).filePath(QStringLiteral("store-manager.png")));

    list->setCurrentRow(1);
    ZT_TRUE("«−» у другого горит", remove->isEnabled());
    remove->click();
    ZT_EQ("строка забыта", std::string("1"), std::to_string(int(dialog.result().stores.size())));
    ZT_TRUE("папка цела", QDir(b.root()).exists());
}

void checkDownloadFromCloud() {
    // Облако с настоящим хранилищем: первое устройство залило всё.
    zt::MiniStore src, cloudHome, targetHome;
    const QString cloud = cloudHome.root() + QStringLiteral("/облако");
    QString err;
    {
        ZStorage s(src.root());
        FakeSecrets boot;
        ZStorage::Config cfg;
        cfg.cloudDir = cloud;
        ZT_TRUE("облако заведено",
                s.connectCloud(cfg, QStringLiteral("пароль-шифра"), QString(), boot, kTiny,
                                nullptr, &err));
        ZT_TRUE("корень завёлся", !s.ensureRootNote(&err).isEmpty());
        ZT_TRUE("заливка прошла", s.pushAll(nullptr, &err));
    }

    const QString dest = targetHome.root() + QStringLiteral("/копия");
    auto secrets = std::make_shared<FakeSecrets>();
    StoreManagerDialog dialog(nullptr, {}, QString(), nullptr, secrets, kTiny);
    // Пустой список — диалог сам в форме добавления.
    dialog.findChild<QLineEdit*>(QStringLiteral("folder"))->setText(dest);
    dialog.findChild<QLineEdit*>(QStringLiteral("server"))->setText(cloud);

    // Неверный пароль — отказ ДО скачивания, папка не тронута.
    dialog.findChild<QLineEdit*>(QStringLiteral("password"))
        ->setText(QStringLiteral("не тот"));
    dialog.findChild<QPushButton*>(QStringLiteral("apply"))->click();
    ZT_TRUE("проверка адреса дождалась", waitIdle(dialog));
    ZT_TRUE("сказано про пароль", statusText(dialog).contains(QStringLiteral("password")));
    ZT_TRUE("папка не заведена", !QDir(dest).exists());

    // Верный пароль — скачивание и вердикт «переключиться».
    dialog.findChild<QLineEdit*>(QStringLiteral("password"))
        ->setText(QStringLiteral("пароль-шифра"));
    dialog.findChild<QPushButton*>(QStringLiteral("apply"))->click();
    ZT_TRUE("скачивание дождалось", waitIdle(dialog));
    ZT_TRUE(("скачалось без жалоб: " + s(statusText(dialog))).c_str(),
            dialog.result().downloadedNew);
    ZT_EQ("вердикт зовёт переключиться", s(QDir::cleanPath(dest)),
          s(dialog.result().switchToRoot));
    ZT_TRUE("папка стала хранилищем",
            ZStorage::inspect(dest) == ZStorage::DirKind::Store);
    ZT_EQ("строка легла в список", std::string("1"),
          std::to_string(int(dialog.result().stores.size())));
    ZT_TRUE("ключ перекочевал в keyring", secrets->keys_.size() == 1);
}

void checkFreshCloudAndReset() {
    // Хранилище без облака подключается к ПУСТОМУ облаку: пароль спрашивается
    // дважды, конверт чеканится; затем пароль сбрасывается — облачная копия
    // заменяется под новым ключом.
    zt::MiniStore home, cloudHome;
    const QString root = home.root() + QStringLiteral("/архив");
    QDir().mkpath(root);
    QString err;
    {
        ZStorage s(root);
        ZT_TRUE(("хранилище завелось: " + err.toStdString()).c_str(), s.init(&err));
    }
    const QString cloud = cloudHome.root() + QStringLiteral("/свежее-облако");
    QDir().mkpath(cloud);
    ZStorage::Config entry;
    entry.root = root;
    auto secrets = std::make_shared<FakeSecrets>();

    TestDialog dialog(nullptr, {entry}, QString(), nullptr, secrets, kTiny);
    auto* password2 = dialog.findChild<QLineEdit*>(QStringLiteral("password2"));
    dialog.findChild<QLineEdit*>(QStringLiteral("server"))->setText(cloud);
    dialog.findChild<QLineEdit*>(QStringLiteral("password"))
        ->setText(QStringLiteral("первый-пароль"));
    dialog.findChild<QPushButton*>(QStringLiteral("apply"))->click();
    ZT_TRUE("проверка свежести дождалась", waitIdle(dialog));
    ZT_TRUE("свежее облако просит повторить пароль", !password2->isHidden());
    ZT_TRUE("конверта ещё нет", !QFile::exists(cloud + QStringLiteral("/keyfile")));
    // Снимок приёмки: свежее облако, второе поле пароля на виду.
    dialog.resize(760, 420);
    dialog.grab().save(
        QDir(shotDir()).filePath(QStringLiteral("store-manager-свежее-облако.png")));

    // Опечатка в повторе — отказ на месте.
    password2->setText(QStringLiteral("первый-парол"));
    dialog.findChild<QPushButton*>(QStringLiteral("apply"))->click();
    ZT_TRUE("несовпавшие пароли отвергнуты",
            statusText(dialog).contains(QStringLiteral("match")));

    password2->setText(QStringLiteral("первый-пароль"));
    dialog.findChild<QPushButton*>(QStringLiteral("apply"))->click();
    ZT_TRUE("подключение дождалось", waitIdle(dialog));
    ZT_TRUE(("конверт отчеканен: " + s(statusText(dialog))).c_str(),
            QFile::exists(cloud + QStringLiteral("/keyfile")));
    ZT_TRUE("адрес записан в хранилище", ZStorage(root).cloudConfig().hasCloudAddress());
    ZT_TRUE("ключ в keyring", secrets->keys_.size() == 1);
    ZT_TRUE("строка списка несёт облако",
            dialog.result().stores.first().hasCloudAddress());

    // --- сброс пароля -------------------------------------------------------
    ZStorage::Config cfgProbe = ZStorage(root).cloudConfig();
    ZT_TRUE("старый пароль подходит",
            ZStorage::probeCloud(cfgProbe, QString(), QStringLiteral("первый-пароль"),
                                 nullptr, &err));
    dialog.armResetMode();
    ZT_TRUE("режим сброса показал два поля", !password2->isHidden());
    dialog.findChild<QLineEdit*>(QStringLiteral("password"))
        ->setText(QStringLiteral("второй-пароль"));
    password2->setText(QStringLiteral("второй-пароль"));
    dialog.findChild<QPushButton*>(QStringLiteral("apply"))->click();
    ZT_TRUE("сброс дождался", waitIdle(dialog));
    ZT_TRUE(("облако заменено: " + s(statusText(dialog))).c_str(),
            statusText(dialog).contains(QStringLiteral("replaced")));
    ZT_TRUE("старый пароль больше не подходит",
            !ZStorage::probeCloud(cfgProbe, QString(), QStringLiteral("первый-пароль"),
                                  nullptr, &err));
    ZT_TRUE("новый пароль подходит",
            ZStorage::probeCloud(cfgProbe, QString(), QStringLiteral("второй-пароль"),
                                 nullptr, &err));
}

}  // namespace

static int ztRunSuite(int argc, char** argv) {
    (void)argc;
    (void)argv;
    checkCloudAddress();
    checkListAndForget();
    checkDownloadFromCloud();
    checkFreshCloudAndReset();
    return zt::report("store_manager_dialog");
}

TEST(StoreManagerDialog, All) {
    std::vector<QByteArray> ztArgs{QByteArrayLiteral("store_manager_dialog_test")};
    std::vector<char*> ztArgv;
    for (QByteArray& a : ztArgs) ztArgv.push_back(a.data());
    EXPECT_EQ(0, ztRunSuite(int(ztArgv.size()), ztArgv.data()));
}
