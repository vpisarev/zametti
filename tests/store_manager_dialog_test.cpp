// Окно хранилищ, ИНТЕГРАЦИЯ (§2.10): модель ↔ виджеты ↔ рабочий поток.
//
// Правила снимка, доступность кнопок и словарь строк проверяет
// store_manager_model_test БЕЗ виджетов; здесь — только проводка: жест окна
// доезжает до модели, работа — до ядра, итог возвращается в виджеты и в
// Result. Облако — каталог (FolderCloud), keyring — в памяти, Аргон крошечный;
// переспросы отвечает TestDialog::ask мимо модального окна (та же дверь — у
// обезьяны store-monkey).

#include "store_manager_dialog.h"

#include "keyfile.h"
#include "secret_store.h"
#include "zstorage.h"
#include "zstorage_manager.h"

#include "fake_secrets.h"
#include "mini_store.h"
#include "test_util.h"
#include "testdata.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QTest>
#include <QThread>

#include <memory>
#include <string>
#include <vector>

using namespace zametti;

namespace {

const Keyfile::KdfParams kTiny{1, 1 << 20};

std::string s(const QString& q) { return q.toStdString(); }

using FakeSecrets = zt::FakeSecrets;

// Наборам — жесты мимо модальных окон: переспрос отвечается заданной кнопкой,
// системный выбор папки подменяется заготовленным путём.
class TestDialog : public StoreManagerDialog {
public:
    using StoreManagerDialog::StoreManagerDialog;
    using StoreManagerDialog::addFolder;
    using StoreManagerDialog::model_;
    using StoreManagerDialog::render;
    using StoreManagerDialog::browseButton_;
    using StoreManagerDialog::formFrame_;

    int nextAnswer = -1;   // -1 — отказ (последняя кнопка)
    StoreManagerModel::Question lastQuestion;
    QString nextFolder;    // что «выберет» человек в системном диалоге
    int folderAsks = 0;

    int ask(const StoreManagerModel::Question& question) override {
        lastQuestion = question;
        return nextAnswer >= 0 ? nextAnswer : int(question.choices.size()) - 1;
    }
    QString askFolder() override {
        ++folderAsks;
        return nextFolder;
    }

    // Правка поля так, как её видит модель: setText не шлёт textEdited, и
    // прямой setText мимо модели был бы враньём набора.
    void type(StoreManagerModel::FieldId which, const QString& text) {
        model_.edit(which, text);
        render();
    }
};

// Каталог снимков — ОДИН РАЗ на набор: outDir чистит каталог при каждом
// вызове, и второй снимок стирал бы первый.
const QString& shotDir() {
    static const QString dir = zt::TestData::outDir(QStringLiteral("store-manager"));
    return dir;
}

// Дождаться конца рабочего потока: занятое окно гасит Check.
bool waitIdle(StoreManagerDialog& dialog, int budgetMs = 20000) {
    auto* check = dialog.findChild<QPushButton*>(QStringLiteral("check"));
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < budgetMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        if (check->isEnabled()) return true;
        QThread::msleep(10);
    }
    return false;
}

QString statusText(StoreManagerDialog& dialog) {
    return dialog.findChild<QLabel*>(QStringLiteral("status"))->text();
}

void checkListAndForget() {
    zt::MiniStore a, b;
    ZStorageManager stores;
    ZStorage::Config first;
    first.root = a.root();
    first.name = QStringLiteral("Первое");
    ZStorage::Config second;
    second.root = b.root();
    stores.remember(first);
    stores.remember(second);
    auto secrets = std::make_shared<FakeSecrets>();

    TestDialog dialog(nullptr, stores, a.root(), secrets, kTiny);
    auto* list = dialog.findChild<QListWidget*>(QStringLiteral("storeList"));
    ZT_EQ("в списке две строки", std::string("2"), std::to_string(list->count()));
    ZT_TRUE("открытое помечено", list->item(0)->text().contains(QStringLiteral("open")));
    ZT_TRUE("выбрана строка открытого", list->currentRow() == 0);
    auto* remove = dialog.findChild<QPushButton*>(QStringLiteral("removeStore"));
    // «−» жив ВСЕГДА, и у открытого тоже (п.14 брифа): подтверждённое
    // удаление открытой строки отцепляет хранилище немедленно.
    ZT_TRUE("«−» горит и у открытого", remove->isEnabled());

    // Снимок приёмки: список с двумя строками, форма и обе рамки фактов.
    dialog.resize(900, 560);
    dialog.grab().save(QDir(shotDir()).filePath(QStringLiteral("store-manager.png")));

    // «−» по другой строке: переспрос называет цену, Remove забывает строку у
    // МЕНЕДЖЕРА (строки правятся на месте, применять при закрытии нечего).
    list->setCurrentRow(1);
    dialog.nextAnswer = 0;
    remove->click();
    ZT_TRUE("переспрос называет цену",
            dialog.lastQuestion.detail.contains(QStringLiteral("stay on disk")));
    ZT_EQ("строка забыта у менеджера", std::string("1"), std::to_string(stores.size()));
    ZT_TRUE("папка цела", QDir(b.root()).exists());

    // Отказ ничего не забывает.
    dialog.nextAnswer = -1;
    remove->click();
    ZT_EQ("отказ ничего не забыл", std::string("1"), std::to_string(stores.size()));
}

void checkCreateFromCloud() {
    // Облако с настоящим хранилищем: первое устройство залило всё; пустая
    // папка + верный пароль встают бутстрапом ГОЛОВЫ — ни одного блоба
    // содержимого в диалоге (качает прогон после Open).
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
    QDir().mkpath(dest);
    ZStorageManager stores;
    auto secrets = std::make_shared<FakeSecrets>();
    TestDialog dialog(nullptr, stores, QString(), secrets, kTiny);
    // Пустой список говорит словами (беда G).
    ZT_TRUE("пустой список говорит словами",
            statusText(dialog).contains(QStringLiteral("press +")));
    dialog.addFolder(dest);
    ZT_EQ("строка добавилась", std::string("1"), std::to_string(stores.size()));
    dialog.type(StoreManagerModel::FieldId::Server, cloud);

    // Неверный пароль — отказ ДО первой записи: каркас не заводится.
    dialog.type(StoreManagerModel::FieldId::EncryptionPassword, QStringLiteral("не тот"));
    dialog.nextAnswer = 0;   // Create
    dialog.findChild<QPushButton*>(QStringLiteral("openStore"))->click();
    ZT_TRUE("создание дождалось", waitIdle(dialog));
    ZT_TRUE(("сказано про пароль: " + s(statusText(dialog))).c_str(),
            statusText(dialog).contains(QStringLiteral("password")));
    ZT_TRUE("каркас не заведён", ZStorage::inspect(dest) == ZStorage::DirKind::Empty);

    // Верный пароль — голова приехала, вердикт зовёт переключиться.
    dialog.type(StoreManagerModel::FieldId::EncryptionPassword,
                QStringLiteral("пароль-шифра"));
    dialog.findChild<QPushButton*>(QStringLiteral("openStore"))->click();
    ZT_TRUE("бутстрап дождался", waitIdle(dialog));
    ZT_TRUE(("голова приехала: " + s(statusText(dialog))).c_str(),
            dialog.result().downloadedNew);
    ZT_EQ("вердикт зовёт переключиться", s(QDir::cleanPath(dest)),
          s(QDir::cleanPath(dialog.result().switchToRoot)));
    ZT_TRUE("папка стала хранилищем",
            ZStorage::inspect(dest) == ZStorage::DirKind::Store);
    ZT_TRUE("адрес лёг в строку менеджера",
            stores.storeFor(dest).hasCloudAddress());
    ZT_TRUE("ключ перекочевал в keyring", secrets->keys_.size() == 1);
}

void checkBrowseOnMissingLocalFolder() {
    // СЦЕНАРИЙ 3 ВЛАДЕЛЬЦА, УРОВЕНЬ ВИДЖЕТА (живая жалоба 30.08.2026: «я не
    // могу нажать "…", чтобы указать новую локацию»). Папку переименовали;
    // строка горит красным, а кнопка «…» обязана быть живой — и не только
    // логически: она ПЛАВАЮЩАЯ (сидит вне разметки, по координатам распорки),
    // и настоящий КЛИК МЫШЬЮ по её видимой геометрии обязан открывать выбор
    // папки. Выбрали новую — путь подхватился, Open ожил.
    zt::MiniStore home;
    const QString was = home.root() + QStringLiteral("/хранилище");
    QString err;
    ZT_TRUE("хранилище завелось", ZStorage(was).init(&err));
    const QString gone = home.root() + QStringLiteral("/уехала");
    ZT_TRUE("папку переименовали", QDir().rename(was, gone));

    auto secrets = std::make_shared<FakeSecrets>();
    ZStorageManager stores(secrets);
    ZStorage::Config row;
    row.root = was;
    stores.remember(row);

    TestDialog dialog(nullptr, stores, QString(), secrets, kTiny);
    dialog.resize(900, 560);
    dialog.show();
    QCoreApplication::processEvents();
    QCoreApplication::processEvents();

    auto* folder = dialog.findChild<QLineEdit*>(QStringLiteral("folder"));
    ZT_TRUE("путь горит красным",
            folder->styleSheet().contains(QStringLiteral("c03030")));
    ZT_TRUE("«…» логически жива", dialog.browseButton_->isEnabled());
    ZT_TRUE("«…» видима", dialog.browseButton_->isVisible());
    ZT_TRUE("«…» не нулевого размера",
            dialog.browseButton_->width() > 0 && dialog.browseButton_->height() > 0);
    ZT_TRUE("«…» внутри рамки формы",
            dialog.formFrame_->rect().contains(dialog.browseButton_->geometry()));

    // НАСТОЯЩИЙ клик мышью — ЧЕРЕЗ ОКНО, с системным поиском виджета под
    // точкой: плавающую кнопку может нарисовать не там или НАКРЫТЬ соседом
    // по стеку (распорка съедала клик — живая жалоба), и логический click()
    // этого не поймал бы, как и клик, посланный виджету напрямую.
    dialog.nextFolder = gone;
    const QPoint inWindow = dialog.formFrame_->mapTo(
        &dialog, dialog.browseButton_->geometry().center());
    QTest::mouseClick(dialog.windowHandle(), Qt::LeftButton, {}, inWindow);
    QCoreApplication::processEvents();
    ZT_EQ("клик открыл выбор папки", std::string("1"),
          std::to_string(dialog.folderAsks));
    ZT_EQ("новый путь подхватился", s(QDir::cleanPath(gone)),
          s(QDir::cleanPath(folder->text())));
    ZT_TRUE("путь больше не красный",
            !folder->styleSheet().contains(QStringLiteral("c03030")));
    ZT_TRUE("Open ожил",
            dialog.findChild<QPushButton*>(QStringLiteral("openStore"))->isEnabled());
}

void checkCheckOnMissingCloudFolder() {
    // «Папки ещё нет» — это ПУСТОЕ облако, а не беда (живая проба владельца
    // 30.08: Check по свежему имени показывал голое «404»). Каталог-облако
    // отвечает той же бедой словами «does not exist» — и Check обязан сказать
    // Cloud: empty и позвать запечатывание.
    zt::MiniStore home, cloudHome;
    const QString root = home.root() + QStringLiteral("/архив");
    QDir().mkpath(root);
    QString err;
    ZT_TRUE("хранилище завелось", ZStorage(root).init(&err));
    ZStorageManager stores;
    ZStorage::Config entry;
    entry.root = root;
    stores.remember(entry);
    auto secrets = std::make_shared<FakeSecrets>();
    TestDialog dialog(nullptr, stores, QString(), secrets, kTiny);
    dialog.type(StoreManagerModel::FieldId::Server,
                cloudHome.root() + QStringLiteral("/этой-папки-нет"));
    dialog.findChild<QPushButton*>(QStringLiteral("check"))->click();
    ZT_TRUE("проверка дождалась", waitIdle(dialog));
    ZT_TRUE(("несуществующая папка — пустое облако: " + s(statusText(dialog))).c_str(),
            statusText(dialog).contains(QStringLiteral("Connected")));
    ZT_TRUE("строка фактов говорит empty",
            dialog.findChild<QLabel*>(QStringLiteral("cloudLine"))
                ->text()
                .contains(QStringLiteral("empty")));
}

void checkOpenAppliesPendingCloud() {
    // ЖИВАЯ БАГА ВЛАДЕЛЬЦА (30.08.2026): Check → «Cloud: empty» → ввёл пароль
    // шифрования дважды → нажал OPEN. Прежде окно молча переключалось, не
    // записав адрес: cloud.json нет, синк «не настроен», кнопка облака в
    // тулбаре погашена, автозапуск молчит. Теперь Open доделывает набранное:
    // запечатывает, пишет адрес — и только потом закрывается переключением.
    zt::MiniStore home, cloudHome;
    const QString root = home.root() + QStringLiteral("/архив");
    QDir().mkpath(root);
    QString err;
    ZT_TRUE("хранилище завелось", ZStorage(root).init(&err));
    const QString cloud = cloudHome.root() + QStringLiteral("/свежее");
    QDir().mkpath(cloud);
    auto secrets = std::make_shared<FakeSecrets>();
    ZStorageManager stores(secrets);
    ZStorage::Config entry;
    entry.root = root;
    stores.remember(entry);

    TestDialog dialog(nullptr, stores, QString(), secrets, kTiny);
    dialog.type(StoreManagerModel::FieldId::Server, cloud);
    dialog.type(StoreManagerModel::FieldId::EncryptionPassword, QStringLiteral("пароль"));
    dialog.findChild<QPushButton*>(QStringLiteral("check"))->click();
    ZT_TRUE("разведка дождалась", waitIdle(dialog));
    ZT_TRUE("облако пустое, просят повтор",
            !dialog.findChild<QLineEdit*>(QStringLiteral("password2"))->isHidden());

    dialog.type(StoreManagerModel::FieldId::Repeat, QStringLiteral("пароль"));
    dialog.findChild<QPushButton*>(QStringLiteral("openStore"))->click();
    ZT_TRUE("применение дождалось", waitIdle(dialog));
    ZT_EQ("окно закрылось переключением", s(QDir::cleanPath(root)),
          s(QDir::cleanPath(dialog.result().switchToRoot)));
    ZT_TRUE("конверт уехал", QFile::exists(cloud + QStringLiteral("/keyfile")));
    ZT_TRUE("адрес записан — синк настроен",
            ZStorage(root).cloudConfig().hasCloudAddress());
    ZT_TRUE("ключ в связке", secrets->keys_.size() == 1);
}

void checkSealFreshCloudAndChangePassword() {
    // Хранилище против ПУСТОГО облака: первый Check только смотрит и просит
    // повтор (опечатка запечатала бы облако навсегда), второй — запечатывает.
    // Затем смена пароля при живом ключе: один конверт, ноль стираний.
    zt::MiniStore home, cloudHome;
    const QString root = home.root() + QStringLiteral("/архив");
    QDir().mkpath(root);
    QString err;
    ZT_TRUE(("хранилище завелось: " + err.toStdString()).c_str(),
            ZStorage(root).init(&err));
    const QString cloud = cloudHome.root() + QStringLiteral("/свежее-облако");
    QDir().mkpath(cloud);
    // Связка — и окну, и менеджеру: факты строк («ключ есть?») спрашивает
    // менеджер, как это делает ZApp::setStoreSecrets в бою.
    auto secrets = std::make_shared<FakeSecrets>();
    ZStorageManager stores(secrets);
    ZStorage::Config entry;
    entry.root = root;
    stores.remember(entry);

    TestDialog dialog(nullptr, stores, QString(), secrets, kTiny);
    auto* password2 = dialog.findChild<QLineEdit*>(QStringLiteral("password2"));
    auto* check = dialog.findChild<QPushButton*>(QStringLiteral("check"));
    dialog.type(StoreManagerModel::FieldId::Server, cloud);
    dialog.type(StoreManagerModel::FieldId::EncryptionPassword,
                QStringLiteral("первый-пароль"));

    // Check №1: разведка. Облако пустое, конверт НЕ уехал, показался повтор.
    check->click();
    ZT_TRUE("разведка дождалась", waitIdle(dialog));
    ZT_TRUE("свежее облако просит повторить пароль", !password2->isHidden());
    ZT_TRUE("конверта ещё нет", !QFile::exists(cloud + QStringLiteral("/keyfile")));
    dialog.resize(900, 560);
    dialog.grab().save(
        QDir(shotDir()).filePath(QStringLiteral("store-manager-свежее-облако.png")));

    // Опечатка в повторе — отказ на месте, без работы.
    dialog.type(StoreManagerModel::FieldId::Repeat, QStringLiteral("первый-парол"));
    check->click();
    ZT_TRUE("несовпавшие пароли отвергнуты",
            statusText(dialog).contains(QStringLiteral("match")));

    // Check №2 с пройденным повтором — запечатывание.
    dialog.type(StoreManagerModel::FieldId::Repeat, QStringLiteral("первый-пароль"));
    check->click();
    ZT_TRUE("запечатывание дождалось", waitIdle(dialog));
    ZT_TRUE(("конверт отчеканен: " + s(statusText(dialog))).c_str(),
            QFile::exists(cloud + QStringLiteral("/keyfile")));
    ZT_TRUE("адрес записан в хранилище", ZStorage(root).cloudConfig().hasCloudAddress());
    ZT_TRUE("ключ в keyring", secrets->keys_.size() == 1);
    ZT_TRUE("пароль шифрования в keyring", secrets->cryptPasswords_.size() == 1);
    ZT_TRUE("адрес лёг в строку менеджера", stores.storeFor(root).hasCloudAddress());

    // --- смена пароля при живом ключе (Reset cloud → Change password) ------
    const QByteArray keyfileBefore = [&] {
        QFile f(cloud + QStringLiteral("/keyfile"));
        return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
    }();
    dialog.type(StoreManagerModel::FieldId::EncryptionPassword,
                QStringLiteral("второй-пароль"));
    dialog.nextAnswer = 0;   // [Change password]
    dialog.findChild<QPushButton*>(QStringLiteral("resetCloud"))->click();
    ZT_TRUE("смена пароля дождалась", waitIdle(dialog));
    ZT_TRUE("переспрос предлагал смену без стирания",
            dialog.lastQuestion.choices.first().contains(QStringLiteral("Change")));
    ZT_TRUE("детали называют полный адрес",
            dialog.lastQuestion.detail.contains(cloud));
    ZT_TRUE(("пароль сменён: " + s(statusText(dialog))).c_str(),
            statusText(dialog).contains(QStringLiteral("changed")));
    // Конверт другой, старый пароль не подходит, новый открывает.
    QFile f(cloud + QStringLiteral("/keyfile"));
    ZT_TRUE("конверт открылся", f.open(QIODevice::ReadOnly));
    ZT_TRUE("конверт заменён", f.readAll() != keyfileBefore);
    ZStorage::Config cfgProbe = ZStorage(root).cloudConfig();
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
    checkListAndForget();
    checkCreateFromCloud();
    checkBrowseOnMissingLocalFolder();
    checkCheckOnMissingCloudFolder();
    checkOpenAppliesPendingCloud();
    checkSealFreshCloudAndChangePassword();
    return zt::report("store_manager_dialog");
}

TEST(StoreManagerDialog, All) {
    std::vector<QByteArray> ztArgs{QByteArrayLiteral("store_manager_dialog_test")};
    std::vector<char*> ztArgv;
    for (QByteArray& a : ztArgs) ztArgv.push_back(a.data());
    EXPECT_EQ(0, ztRunSuite(int(ztArgv.size()), ztArgv.data()));
}
