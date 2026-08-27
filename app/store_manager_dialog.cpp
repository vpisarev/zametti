#include "store_manager_dialog.h"

#include "icons.h"

#include <QAction>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QMetaObject>
#include <QPushButton>
#include <QVBoxLayout>

namespace zametti {

namespace {

// Ключ строки — канонический вид пути, тот же, что у ZAppState::rememberStore.
QString canonicalRoot(const QString& root) {
    if (root.isEmpty()) return {};
    return QDir::cleanPath(QFileInfo(root).absoluteFilePath());
}

// Приёмник-копилка для рабочего потока: ядро кладёт сюда ключ и пароль
// сервера, а в настоящий keyring (DBus при главном цикле) их перекладывает
// главный поток по завершении. Чтения здесь не живут: пути подключения ядра,
// которыми ходит диалог, ключей не спрашивают.
class TakenSecrets : public SecretStore {
public:
    bool available() const override { return true; }
    bool loadKey(const QString&, Keyfile*, QString* error) override {
        if (error) *error = QStringLiteral("no keyring in the worker thread");
        return false;
    }
    bool storeKey(const Keyfile& keyfile, QString*) override {
        capturedKey = keyfile;
        return true;
    }
    bool clearKey(const QString&, QString*) override { return true; }
    QString serverPassword(const QString&, QString*) override { return {}; }
    bool setServerPassword(const QString& storeId, const QString& password,
                           QString*) override {
        capturedServerPasswordFor = storeId;
        capturedServerPassword = password;
        return true;
    }
    bool clearServerPassword(const QString&, QString*) override { return true; }
    QString encryptionPassword(const QString&, QString*) override { return {}; }
    bool setEncryptionPassword(const QString& storeId, const QString& password,
                               QString*) override {
        capturedEncryptionPasswordFor = storeId;
        capturedEncryptionPassword = password;
        return true;
    }
    bool clearEncryptionPassword(const QString&, QString*) override { return true; }

    Keyfile capturedKey;
    QString capturedServerPasswordFor;
    QString capturedServerPassword;
    QString capturedEncryptionPasswordFor;
    QString capturedEncryptionPassword;
};

}  // namespace

void StoreManagerDialog::setCloudAddress(ZStorage::Config& cfg, const QString& server,
                                         const QString& user, bool allowInsecureHttp) {
    cfg.remoteUrl.clear();
    cfg.remoteDir.clear();
    const QString address = server.trimmed();
    if (address.startsWith(QStringLiteral("http://")) ||
        address.startsWith(QStringLiteral("https://")))
        // Хвостовой «/» — как у CLI: WebDAV-коллекция без него резолвится
        // относительно родителя.
        cfg.remoteUrl = address.endsWith(QLatin1Char('/')) ? address : address + QLatin1Char('/');
    else if (!address.isEmpty())
        cfg.remoteDir = QDir(address).absolutePath();
    cfg.remoteUser = user.trimmed();
    cfg.allowInsecureHttp = allowInsecureHttp;
}

QString StoreManagerDialog::cloudAddressText(const ZStorage::Config& cfg) {
    return cfg.remoteUrl.isEmpty() ? cfg.remoteDir : cfg.remoteUrl;
}

StoreManagerDialog::StoreManagerDialog(QWidget* parent, const QList<ZStorage::Config>& stores,
                                       const QString& currentRoot,
                                       std::shared_ptr<ZStorage> storage,
                                       std::shared_ptr<SecretStore> secrets,
                                       const Keyfile::KdfParams& mintParams)
    : QDialog(parent),
      storage_(std::move(storage)),
      secrets_(std::move(secrets)),
      currentRoot_(canonicalRoot(currentRoot)),
      mintParams_(mintParams) {
    setWindowTitle(QStringLiteral("Storages"));
    result_.stores = stores;

    // --- слева: список и его кнопки -----------------------------------------
    list_ = new QListWidget(this);
    list_->setObjectName(QStringLiteral("storeList"));
    list_->setSelectionMode(QAbstractItemView::SingleSelection);

    addButton_ = new QPushButton(QStringLiteral("+"), this);
    addButton_->setObjectName(QStringLiteral("addStore"));
    addButton_->setToolTip(QStringLiteral("Add a storage: an existing one, a new empty "
                                          "one, or a download from the cloud"));
    removeButton_ = new QPushButton(QStringLiteral("−"), this);
    removeButton_->setObjectName(QStringLiteral("removeStore"));
    removeButton_->setToolTip(QStringLiteral("Forget this row — the folder and the cloud "
                                             "are not touched"));
    openButton_ = new QPushButton(QStringLiteral("Open"), this);
    openButton_->setObjectName(QStringLiteral("openStore"));

    // --- справа: форма выбранной строки -------------------------------------
    folder_ = new QLineEdit(this);
    folder_->setObjectName(QStringLiteral("folder"));
    browseButton_ = new QPushButton(QStringLiteral("Browse…"), this);
    browseButton_->setObjectName(QStringLiteral("browse"));
    auto* folderRow = new QHBoxLayout;
    folderRow->addWidget(folder_, 1);
    folderRow->addWidget(browseButton_);

    server_ = new QLineEdit(this);
    server_->setObjectName(QStringLiteral("server"));
    server_->setPlaceholderText(
        QStringLiteral("https://server/dav/notes — or a folder path; empty = no cloud"));
    user_ = new QLineEdit(this);
    user_->setObjectName(QStringLiteral("user"));
    serverPassword_ = new QLineEdit(this);
    serverPassword_->setObjectName(QStringLiteral("serverPassword"));
    serverPassword_->setEchoMode(QLineEdit::Password);
    serverPassword_->setPlaceholderText(QStringLiteral("empty = the stored one"));
    insecureHttp_ = new QCheckBox(QStringLiteral("Allow plain http (password travels open)"),
                                  this);
    insecureHttp_->setObjectName(QStringLiteral("insecureHttp"));

    passwordLabel_ = new QLabel(QStringLiteral("Encryption password"), this);
    password_ = new QLineEdit(this);
    password_->setObjectName(QStringLiteral("password"));
    password_->setEchoMode(QLineEdit::Password);
    password_->setPlaceholderText(QStringLiteral("empty = use the key from the keyring"));
    // Глаза-переключатели: включён — пароль виден и остаётся видимым (его
    // копируют не торопясь — просьба владельца); пустое поле наполняется
    // хранимым значением из keyring — затем keychain и служит местом, где
    // свой пароль можно подсмотреть.
    serverEye_ = addEyeToggle(serverPassword_, [this] {
        const QString id = shownStoreId();
        return id.isEmpty() ? QString() : secrets_->serverPassword(id);
    });
    passwordEye_ = addEyeToggle(password_, [this] {
        const QString id = shownStoreId();
        return id.isEmpty() ? QString() : secrets_->encryptionPassword(id);
    });
    password2Label_ = new QLabel(QStringLiteral("Repeat password"), this);
    password2_ = new QLineEdit(this);
    password2_->setObjectName(QStringLiteral("password2"));
    password2_->setEchoMode(QLineEdit::Password);

    applyButton_ = new QPushButton(QStringLiteral("Apply"), this);
    applyButton_->setObjectName(QStringLiteral("apply"));
    resetButton_ = new QPushButton(QStringLiteral("Reset password…"), this);
    resetButton_->setObjectName(QStringLiteral("resetPassword"));
    resetButton_->setToolTip(
        QStringLiteral("The encryption password is lost: replace the cloud copy, "
                       "encrypted with a new password"));

    status_ = new QLabel(this);
    status_->setObjectName(QStringLiteral("status"));
    status_->setWordWrap(true);

    auto* form = new QFormLayout;
    form->addRow(QStringLiteral("Folder"), folderRow);
    form->addRow(QStringLiteral("Cloud (WebDAV)"), server_);
    form->addRow(QStringLiteral("Login"), user_);
    form->addRow(QStringLiteral("Server password"), serverPassword_);
    form->addRow(QString(), insecureHttp_);
    form->addRow(passwordLabel_, password_);
    form->addRow(password2Label_, password2_);
    auto* actionRow = new QHBoxLayout;
    actionRow->addWidget(applyButton_);
    actionRow->addWidget(resetButton_);
    actionRow->addStretch(1);
    form->addRow(QString(), actionRow);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    closeButton_ = buttons->button(QDialogButtonBox::Close);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* leftButtons = new QHBoxLayout;
    leftButtons->addWidget(addButton_);
    leftButtons->addWidget(removeButton_);
    leftButtons->addWidget(openButton_);
    leftButtons->addStretch(1);
    auto* left = new QVBoxLayout;
    left->addWidget(list_, 1);
    left->addLayout(leftButtons);

    auto* right = new QVBoxLayout;
    right->addLayout(form);
    right->addStretch(1);
    right->addWidget(status_);

    auto* columns = new QHBoxLayout;
    columns->addLayout(left, 2);
    columns->addLayout(right, 3);
    auto* whole = new QVBoxLayout(this);
    whole->addLayout(columns, 1);
    whole->addWidget(buttons);
    resize(760, 420);

    connect(addButton_, &QPushButton::clicked, this, &StoreManagerDialog::beginNewEntry);
    connect(removeButton_, &QPushButton::clicked, this, &StoreManagerDialog::forgetSelected);
    connect(openButton_, &QPushButton::clicked, this, &StoreManagerDialog::openSelected);
    connect(list_, &QListWidget::currentRowChanged, this, [this](int row) {
        if (!busy_ && row >= 0) showEntry(row);
    });
    connect(list_, &QListWidget::itemDoubleClicked, this,
            [this](QListWidgetItem*) { openSelected(); });
    connect(applyButton_, &QPushButton::clicked, this, &StoreManagerDialog::onApply);
    connect(resetButton_, &QPushButton::clicked, this, &StoreManagerDialog::enterResetMode);
    connect(server_, &QLineEdit::textEdited, this, [this] {
        // Другой адрес — другая свежесть: повтор пароля мог относиться к
        // прежнему облаку.
        freshnessKnown_ = false;
        if (!resetMode_) {
            password2Label_->hide();
            password2_->hide();
            password2_->clear();
        }
    });
    connect(browseButton_, &QPushButton::clicked, this, [this] {
        const QString start = folder_->text().isEmpty()
                                  ? QFileInfo(currentRoot_).absolutePath()
                                  : folder_->text();
        const QString dir = QFileDialog::getExistingDirectory(
            this, QStringLiteral("Storage folder"), start);
        if (!dir.isEmpty()) folder_->setText(dir);
    });

    // Пустой список — сразу форма добавления: человеку, запустившему
    // программу впервые, делать здесь больше нечего.
    if (result_.stores.isEmpty()) {
        rebuildList(-1);
        beginNewEntry();
    } else {
        int current = 0;
        for (int i = 0; i < result_.stores.size(); ++i)
            if (result_.stores[i].root == currentRoot_) current = i;
        rebuildList(current);
    }
}

StoreManagerDialog::~StoreManagerDialog() {
    if (worker_.joinable()) worker_.join();
}

void StoreManagerDialog::reject() {
    if (busy_) return;  // рабочий поток держит хранилище — его дожидаются
    QDialog::reject();
}

bool StoreManagerDialog::isCurrentRoot(const QString& root) const {
    return !currentRoot_.isEmpty() && canonicalRoot(root) == currentRoot_;
}

QAction* StoreManagerDialog::addEyeToggle(QLineEdit* field, std::function<QString()> stored) {
    const qreal dpr = devicePixelRatioF();
    const QColor color = palette().color(QPalette::Text);
    QIcon icon;
    icon.addPixmap(toolbarIcon(QStringLiteral("eye"), 12, color, dpr), QIcon::Normal,
                   QIcon::Off);
    icon.addPixmap(toolbarIcon(QStringLiteral("eye-off"), 12, color, dpr), QIcon::Normal,
                   QIcon::On);
    QAction* eye = field->addAction(icon, QLineEdit::TrailingPosition);
    eye->setCheckable(true);
    eye->setToolTip(QStringLiteral("Show the password"));
    connect(eye, &QAction::toggled, this, [field, stored](bool on) {
        // Keyring спрашивается ЗДЕСЬ, в главном потоке (DBus живёт при
        // главном цикле), и только по явному жесту человека.
        if (on && field->text().isEmpty() && stored) field->setText(stored());
        field->setEchoMode(on ? QLineEdit::Normal : QLineEdit::Password);
    });
    return eye;
}

QString StoreManagerDialog::shownStoreId() const {
    const QString named = newEntry_ ? folder_->text().trimmed()
                          : (selected_ >= 0 ? result_.stores[selected_].root : QString());
    if (named.isEmpty()) return {};
    const QString root = canonicalRoot(named);
    if (ZStorage::inspect(root) != ZStorage::DirKind::Store) return {};
    return isCurrentRoot(root) && storage_ != nullptr ? storage_->identity().storeId()
                                                      : ZStorage(root).identity().storeId();
}

void StoreManagerDialog::rebuildList(int selectRow) {
    list_->clear();
    for (const ZStorage::Config& e : result_.stores) {
        const QString title = e.name.isEmpty() ? QFileInfo(e.root).fileName() : e.name;
        auto* item = new QListWidgetItem(
            isCurrentRoot(e.root) ? title + QStringLiteral("  •  open") : title, list_);
        QString tip = e.root;
        if (e.hasCloud()) tip += QStringLiteral("\ncloud: ") + cloudAddressText(e);
        item->setToolTip(tip);
    }
    if (selectRow >= 0 && selectRow < list_->count()) {
        list_->setCurrentRow(selectRow);
        showEntry(selectRow);
    }
}

void StoreManagerDialog::showEntry(int row) {
    if (row < 0 || row >= result_.stores.size()) return;
    newEntry_ = false;
    selected_ = row;
    leaveResetMode();
    const ZStorage::Config& e = result_.stores[row];
    // Папка существующей строки заморожена: строка ПРО эту папку, а «та же
    // строка, другая папка» была бы вторым способом добавить хранилище.
    folder_->setText(e.root);
    folder_->setReadOnly(true);
    browseButton_->setEnabled(false);
    server_->setText(cloudAddressText(e));
    user_->setText(e.remoteUser);
    serverPassword_->clear();
    insecureHttp_->setChecked(e.allowInsecureHttp);
    password_->clear();
    password2_->clear();
    // Глаза — закрыть: чужая строка не должна открываться с показанным
    // паролем прежней.
    serverEye_->setChecked(false);
    passwordEye_->setChecked(false);
    applyButton_->setText(QStringLiteral("Apply"));
    resetButton_->setEnabled(true);
    const bool current = isCurrentRoot(e.root);
    openButton_->setEnabled(!current);
    // «−» у открытого погашен: строка вернулась бы при следующем прицеплении.
    removeButton_->setEnabled(!current);
    say(QString(), false);
}

void StoreManagerDialog::beginNewEntry() {
    if (busy_) return;
    newEntry_ = true;
    selected_ = -1;
    leaveResetMode();
    list_->setCurrentRow(-1);
    folder_->clear();
    folder_->setReadOnly(false);
    browseButton_->setEnabled(true);
    server_->clear();
    user_->clear();
    serverPassword_->clear();
    insecureHttp_->setChecked(false);
    password_->clear();
    password2_->clear();
    serverEye_->setChecked(false);
    passwordEye_->setChecked(false);
    applyButton_->setText(QStringLiteral("Add"));
    resetButton_->setEnabled(false);
    openButton_->setEnabled(false);
    removeButton_->setEnabled(false);
    say(QStringLiteral("Name a folder: a storage — it joins the list; an empty one — a new "
                       "storage is created there, or downloaded, if the cloud is named."),
        false);
    folder_->setFocus();
}

void StoreManagerDialog::forgetSelected() {
    if (busy_ || selected_ < 0 || selected_ >= result_.stores.size()) return;
    if (isCurrentRoot(result_.stores[selected_].root)) return;
    result_.stores.removeAt(selected_);
    const int next = qMin(selected_, int(result_.stores.size()) - 1);
    selected_ = -1;
    if (result_.stores.isEmpty()) {
        rebuildList(-1);
        beginNewEntry();
    } else {
        rebuildList(next);
    }
}

void StoreManagerDialog::openSelected() {
    if (busy_ || selected_ < 0 || selected_ >= result_.stores.size()) return;
    const QString root = result_.stores[selected_].root;
    if (isCurrentRoot(root)) return;
    result_.switchToRoot = root;
    accept();
}

void StoreManagerDialog::enterResetMode() {
    if (busy_ || resetMode_ || selected_ < 0) return;
    // ПЕРЕСПРОС — исключение, названное владельцем: операция стирает облачную
    // копию, и слова обязаны это говорить.
    auto* ask = new QMessageBox(this);
    ask->setAttribute(Qt::WA_DeleteOnClose);
    ask->setIcon(QMessageBox::Warning);
    ask->setWindowTitle(QStringLiteral("zametti"));
    ask->setText(QStringLiteral("Reset the encryption password?"));
    ask->setInformativeText(
        QStringLiteral("The cloud copy (if any) will be deleted and replaced with this "
                       "computer's copy, encrypted with the new password. The old password "
                       "stops working; other devices will need the new one."));
    QPushButton* go =
        ask->addButton(QStringLiteral("Reset"), QMessageBox::DestructiveRole);
    ask->addButton(QMessageBox::Cancel);
    ask->setDefaultButton(QMessageBox::Cancel);
    ask->exec();
    if (ask->clickedButton() != go) return;
    armResetMode();
}

void StoreManagerDialog::armResetMode() {
    if (busy_ || resetMode_ || selected_ < 0) return;
    resetMode_ = true;
    passwordLabel_->setText(QStringLiteral("New encryption password"));
    password_->setPlaceholderText(QString());
    password_->clear();
    password2_->clear();
    password2Label_->show();
    password2_->show();
    applyButton_->setText(QStringLiteral("Reset && upload"));
    say(QStringLiteral("Type the new password twice, then press «Reset & upload»."), false);
    password_->setFocus();
}

void StoreManagerDialog::leaveResetMode() {
    resetMode_ = false;
    freshnessKnown_ = false;
    passwordLabel_->setText(QStringLiteral("Encryption password"));
    password_->setPlaceholderText(QStringLiteral("empty = use the key from the keyring"));
    password2Label_->hide();
    password2_->hide();
    password2_->clear();
}

void StoreManagerDialog::say(const QString& text, bool trouble) {
    status_->setStyleSheet(trouble ? QStringLiteral("color: #c03030;") : QString());
    status_->setText(text);
}

void StoreManagerDialog::setBusy(bool on) {
    busy_ = on;
    for (QWidget* w :
         std::initializer_list<QWidget*>{list_, addButton_, removeButton_, openButton_,
                                         folder_, browseButton_, server_, user_,
                                         serverPassword_, insecureHttp_, password_,
                                         password2_, applyButton_, resetButton_,
                                         closeButton_})
        w->setEnabled(!on);
    if (!on && selected_ >= 0) {
        // Доступность кнопок списка — по выбранной строке, не «всё вернуть».
        const bool current = isCurrentRoot(result_.stores[selected_].root);
        openButton_->setEnabled(!current);
        removeButton_->setEnabled(!current);
    }
    if (!on && (newEntry_ || selected_ >= 0)) resetButton_->setEnabled(!newEntry_);
    if (!on) {
        folder_->setReadOnly(!newEntry_);
        browseButton_->setEnabled(newEntry_);
    }
}

void StoreManagerDialog::startWork(const QString& status, std::function<QString()> job,
                                   std::function<void(const QString&)> done) {
    setBusy(true);
    say(status, false);
    if (worker_.joinable()) worker_.join();
    worker_ = std::thread([this, job = std::move(job), done = std::move(done)] {
        const QString error = job();
        // Диалог живёт дольше потока: деструктор ждёт join, а занятое окно
        // не закрывается (reject глушится).
        QMetaObject::invokeMethod(
            this,
            [this, error, done] {
                setBusy(false);
                done(error);
            },
            Qt::QueuedConnection);
    });
}

void StoreManagerDialog::settleEntry(const ZStorage::Config& entry) {
    int row = -1;
    for (int i = 0; i < result_.stores.size(); ++i)
        if (result_.stores[i].root == entry.root) row = i;
    if (row < 0) {
        result_.stores.append(entry);
        row = int(result_.stores.size()) - 1;
    } else {
        ZStorage::Config kept = entry;
        if (kept.name.isEmpty()) kept.name = result_.stores[row].name;
        result_.stores[row] = kept;
    }
    newEntry_ = false;
    rebuildList(row);
}

// --- ветки Apply ------------------------------------------------------------

void StoreManagerDialog::onApply() {
    if (busy_) return;
    const QString named =
        (newEntry_ ? folder_->text() : (selected_ >= 0 ? result_.stores[selected_].root
                                                       : QString()))
            .trimmed();
    if (named.isEmpty()) {
        say(QStringLiteral("Name a folder for the storage."), true);
        return;
    }
    const QString root = canonicalRoot(named);
    ZStorage::Config cfg = selected_ >= 0 ? result_.stores[selected_] : ZStorage::Config{};
    cfg.root = root;
    setCloudAddress(cfg, server_->text(), user_->text(), insecureHttp_->isChecked());
    const QString serverPassword = serverPassword_->text();
    const QString password = password_->text();

    if (resetMode_) {
        resetPassword(cfg, serverPassword, password);
        return;
    }

    switch (ZStorage::inspect(root)) {
        case ZStorage::DirKind::Store:
            applyToStore(cfg, serverPassword, password);
            return;
        case ZStorage::DirKind::Foreign:
            say(QStringLiteral("The folder is not empty and is not a zametti storage:\n%1")
                    .arg(root),
                true);
            return;
        case ZStorage::DirKind::Missing:
            if (!cfg.hasCloud()) {
                say(QStringLiteral("The folder does not exist:\n%1").arg(root), true);
                return;
            }
            [[fallthrough]];  // облако назвало хранилище — скачивание заведёт каталог
        case ZStorage::DirKind::Empty:
            if (cfg.hasCloud()) {
                addFromCloud(cfg, serverPassword, password);
                return;
            }
            // ВТОРОЕ НАЗВАННОЕ ВЛАДЕЛЬЦЕМ ИСКЛЮЧЕНИЕ из «никаких диалогов
            // подтверждения»: пустой каталог мог быть выбран по ошибке, а
            // засеянное хранилище потом ищут глазами и гадают, откуда оно.
            if (QMessageBox::question(
                    this, QStringLiteral("zametti"),
                    QStringLiteral("Create a new zametti storage in \"%1\"?").arg(root)) !=
                QMessageBox::Yes)
                return;
            {
                // Засев локальный и мгновенный — без рабочего потока.
                ZStorage fresh(root);
                QString error;
                if (!fresh.init(&error)) {
                    say(error, true);
                    return;
                }
                ZStorage::Config entry;
                entry.root = root;
                settleEntry(entry);
                say(QStringLiteral("The storage is created."), false);
                if (currentRoot_.isEmpty()) {
                    // Окно без хранилища — открыть созданное сразу: пустое окно
                    // никому не нужно.
                    result_.switchToRoot = root;
                    accept();
                }
            }
            return;
    }
}

void StoreManagerDialog::applyToStore(const ZStorage::Config& cfg,
                                      const QString& serverPassword,
                                      const QString& password) {
    const bool current = isCurrentRoot(cfg.root);
    // Идентичность — дешёвое чтение файла; keyring дальше спрашивается только
    // в главном потоке и только когда он вправду нужен.
    const QString storeId =
        current ? storage_->identity().storeId() : ZStorage(cfg.root).identity().storeId();

    // Пароль сервера: пустое поле = хранящийся в keyring.
    QString effectiveServerPassword = serverPassword;
    if (effectiveServerPassword.isEmpty() && !cfg.remoteUrl.isEmpty() && !storeId.isEmpty())
        effectiveServerPassword = secrets_->serverPassword(storeId);

    // --- облако убрали: отвязка, как CLI --reset (неразрушительная) ---------
    if (!cfg.hasCloud()) {
        const bool hadCloud = selected_ >= 0 && result_.stores[selected_].hasCloud();
        startWork(
            QStringLiteral("Reading the storage…"),
            [this, cfg, current, hadCloud]() -> QString {
                auto temp = current ? storage_ : std::make_shared<ZStorage>(cfg.root);
                if (!current && hadCloud) {
                    const ZStorage::LockReport lock = temp->lock();
                    if (!lock.locked)
                        return QStringLiteral("The storage is open by another copy of "
                                              "zametti (pid %1 on \"%2\").")
                            .arg(lock.holderPid)
                            .arg(lock.holderHost);
                }
                QString error;
                if (hadCloud && !temp->clearRemoteConfig(&error)) return error;
                if (current && hadCloud) temp->dropRemote();
                settled_ = temp->remoteConfig();
                settled_.name = temp->localStoreName();
                return {};
            },
            [this, storeId, hadCloud, current](const QString& error) {
                if (!error.isEmpty()) {
                    say(error, true);
                    return;
                }
                if (hadCloud && !storeId.isEmpty()) {
                    // Секреты отвязанного — вон из keyring, как у CLI --reset.
                    secrets_->clearKey(storeId);
                    secrets_->clearServerPassword(storeId);
                    secrets_->clearEncryptionPassword(storeId);
                }
                if (current && hadCloud) result_.cloudChangedForCurrent = true;
                settleEntry(settled_);
                say(hadCloud ? QStringLiteral("The cloud address and the secrets are "
                                              "forgotten; the cloud copy itself is intact.")
                             : QStringLiteral("Added to the list."),
                    false);
            });
        return;
    }

    // --- облако есть, пароль пуст: обновление адреса ключом из keyring ------
    if (password.isEmpty()) {
        if (storeId.isEmpty()) {
            say(QStringLiteral("Enter the encryption password: the storage has no identity "
                               "yet, and there is nothing to look up in the keyring."),
                true);
            return;
        }
        Keyfile known;
        QString why;
        if (!secrets_->loadKey(storeId, &known, &why)) {
            say(QStringLiteral("Enter the encryption password — the key is not in the "
                               "keyring (%1).")
                    .arg(why),
                true);
            return;
        }
        startWork(
            QStringLiteral("Checking the cloud…"),
            [this, cfg, current, known, effectiveServerPassword]() -> QString {
                auto temp = current ? storage_ : std::make_shared<ZStorage>(cfg.root);
                if (!current) {
                    const ZStorage::LockReport lock = temp->lock();
                    if (!lock.locked)
                        return QStringLiteral("The storage is open by another copy of "
                                              "zametti (pid %1 on \"%2\").")
                            .arg(lock.holderPid)
                            .arg(lock.holderHost);
                }
                QString error;
                auto remote = ZStorage::makeRemote(cfg, effectiveServerPassword, &error);
                // setRemote сверяет манифест: чужое облако — честный отказ до
                // единой записи.
                if (remote == nullptr || !temp->setRemote(remote, known, &error)) return error;
                if (!temp->writeRemoteConfig(cfg, &error)) return error;
                settled_ = temp->remoteConfig();
                settled_.name = temp->localStoreName();
                return {};
            },
            [this, storeId, serverPassword, cfg, current](const QString& error) {
                if (!error.isEmpty()) {
                    say(error, true);
                    return;
                }
                if (!serverPassword.isEmpty() && !cfg.remoteUrl.isEmpty())
                    secrets_->setServerPassword(storeId, serverPassword);
                if (current) result_.cloudChangedForCurrent = true;
                settleEntry(settled_);
                say(QStringLiteral("Connected: %1").arg(cloudAddressText(cfg)), false);
            });
        return;
    }

    // --- облако есть, пароль введён: полное подключение ---------------------
    // Дважды или один раз — решает конверт в облаке; вопрос дешёвый (один GET)
    // и задаётся один раз на адрес.
    if (!freshnessKnown_) {
        startWork(
            QStringLiteral("Checking the cloud…"),
            [this, cfg, effectiveServerPassword]() -> QString {
                QString error;
                cloudFresh_ =
                    !ZStorage::cloudHasKeyfile(cfg, effectiveServerPassword, &error);
                return error;  // непусто только у негодного адреса
            },
            [this](const QString& error) {
                if (!error.isEmpty()) {
                    say(error, true);
                    return;
                }
                freshnessKnown_ = true;
                if (cloudFresh_) {
                    // Опечатка при СОЗДАНИИ запечатала бы облако навсегда —
                    // потому пароль дважды; при развороте существующего она
                    // безобидна, и второго ввода не спрашивается.
                    password2Label_->show();
                    password2_->show();
                    say(QStringLiteral("The cloud looks empty (or is not reachable yet): "
                                       "this password will seal it. Repeat the password "
                                       "and press the button again."),
                        false);
                    password2_->setFocus();
                    return;
                }
                onApply();  // конверт есть: пароль один, подключаемся сразу
            });
        return;
    }
    if (cloudFresh_ && password != password2_->text()) {
        say(QStringLiteral("The passwords do not match."), true);
        return;
    }

    startWork(
        QStringLiteral("Connecting… (unwrapping the key takes a moment)"),
        [this, cfg, current, password, effectiveServerPassword]() -> QString {
            auto temp = current ? storage_ : std::make_shared<ZStorage>(cfg.root);
            if (!current) {
                const ZStorage::LockReport lock = temp->lock();
                if (!lock.locked)
                    return QStringLiteral("The storage is open by another copy of zametti "
                                          "(pid %1 on \"%2\").")
                        .arg(lock.holderPid)
                        .arg(lock.holderHost);
            }
            TakenSecrets taken;
            QString error;
            ZStorage::ConnectOutcome outcome;
            if (!temp->connectRemote(cfg, password, effectiveServerPassword, taken,
                                     mintParams_, &outcome, &error))
                return error;
            takenKey_ = taken.capturedKey;
            takenServerPassword_ = taken.capturedServerPassword;
            takenStoreId_ = taken.capturedServerPasswordFor;
            settled_ = temp->remoteConfig();
            settled_.name = temp->localStoreName();
            return {};
        },
        [this, cfg, current](const QString& error) {
            if (!error.isEmpty()) {
                say(error, true);
                return;
            }
            QString why;
            if (takenKey_.hasKey() && !secrets_->storeKey(takenKey_, &why))
                say(QStringLiteral("The keyring refused the key (%1) — the password will "
                                   "be asked again next time.")
                        .arg(why),
                    true);
            else
                say(QStringLiteral("Connected: %1").arg(cloudAddressText(cfg)), false);
            if (!takenServerPassword_.isEmpty() && !takenStoreId_.isEmpty())
                secrets_->setServerPassword(takenStoreId_, takenServerPassword_);
            takenKey_ = Keyfile();
            if (current) result_.cloudChangedForCurrent = true;
            leaveResetMode();
            password_->clear();
            password2_->clear();
            settleEntry(settled_);
        });
}

void StoreManagerDialog::addFromCloud(const ZStorage::Config& cfg,
                                      const QString& serverPassword,
                                      const QString& password) {
    if (password.isEmpty()) {
        say(QStringLiteral("Enter the encryption password of the cloud storage."), true);
        return;
    }
    // РАЗВЕДКА ДО СКАЧИВАНИЯ: адрес и оба пароля проверяются раньше, чем
    // хоть что-то ляжет в папку, — человек не остаётся с пустым каталогом
    // и загадкой «что пошло не так».
    startWork(
        QStringLiteral("Checking the cloud…"),
        [this, cfg, serverPassword, password]() -> QString {
            QString error;
            probe_ = ZStorage::CloudProbe();
            if (!ZStorage::probeCloud(cfg, serverPassword, password, &probe_, &error))
                return error;
            return {};
        },
        [this, cfg, serverPassword, password](const QString& error) {
            if (!error.isEmpty()) {
                say(error, true);
                return;
            }
            if (!probe_.hasManifest || !probe_.hasKeyfile) {
                say(probe_.hasManifest
                        ? QStringLiteral("The cloud has no keyfile — it is incomplete; "
                                         "there is nothing to download safely.")
                        : QStringLiteral(
                              "There is no storage in this cloud — nothing to download. "
                              "To start a new storage here, clear the cloud field, create "
                              "the storage, then connect it."),
                    true);
                return;
            }
            const QString found =
                probe_.name.isEmpty() ? probe_.identity.storeId() : probe_.name;
            say(QStringLiteral("Found \"%1\": %2 notes, %3 attachments, %4 MB. "
                               "Downloading…")
                    .arg(found)
                    .arg(probe_.notes)
                    .arg(probe_.attachments)
                    .arg(double(probe_.bytes) / (1024.0 * 1024.0), 0, 'f', 1),
                false);
            startWork(
                QStringLiteral("Downloading the storage…"),
                [this, cfg, serverPassword, password]() -> QString {
                    TakenSecrets taken;
                    QString error;
                    ZStorage::ConnectOutcome outcome;
                    auto fresh =
                        ZStorage::initFromRemote(cfg.root, cfg, password, serverPassword,
                                                 taken, mintParams_, &outcome, &error);
                    if (fresh == nullptr) return error;
                    takenKey_ = taken.capturedKey;
                    takenServerPassword_ = taken.capturedServerPassword;
                    takenStoreId_ = taken.capturedServerPasswordFor;
                    settled_ = fresh->remoteConfig();
                    settled_.name = fresh->localStoreName();
                    return {};
                },
                [this](const QString& error) {
                    if (!error.isEmpty()) {
                        say(error, true);
                        return;
                    }
                    QString why;
                    if (takenKey_.hasKey() && !secrets_->storeKey(takenKey_, &why))
                        fprintf(stderr, "zametti: the keyring refused the key: %s\n",
                                qPrintable(why));
                    if (!takenServerPassword_.isEmpty() && !takenStoreId_.isEmpty())
                        secrets_->setServerPassword(takenStoreId_, takenServerPassword_);
                    takenKey_ = Keyfile();
                    settleEntry(settled_);
                    // Скачивали, чтобы открыть: манифест и корень уже на месте,
                    // остальное привезёт первый прогон синка.
                    result_.switchToRoot = settled_.root;
                    result_.downloadedNew = true;
                    accept();
                });
        });
}

void StoreManagerDialog::resetPassword(const ZStorage::Config& cfg,
                                       const QString& serverPassword,
                                       const QString& password) {
    if (!cfg.hasCloud()) {
        say(QStringLiteral("Name the cloud to hold the re-encrypted copy."), true);
        return;
    }
    if (password.isEmpty() || password != password2_->text()) {
        say(QStringLiteral("Type the new password twice — the two must match."), true);
        return;
    }
    const bool current = isCurrentRoot(cfg.root);
    const QString storeId =
        current ? storage_->identity().storeId() : ZStorage(cfg.root).identity().storeId();
    QString effectiveServerPassword = serverPassword;
    if (effectiveServerPassword.isEmpty() && !cfg.remoteUrl.isEmpty() && !storeId.isEmpty())
        effectiveServerPassword = secrets_->serverPassword(storeId);
    startWork(
        QStringLiteral("Replacing the cloud copy…"),
        [this, cfg, current, password, effectiveServerPassword]() -> QString {
            auto temp = current ? storage_ : std::make_shared<ZStorage>(cfg.root);
            if (!current) {
                const ZStorage::LockReport lock = temp->lock();
                if (!lock.locked)
                    return QStringLiteral("The storage is open by another copy of zametti "
                                          "(pid %1 on \"%2\").")
                        .arg(lock.holderPid)
                        .arg(lock.holderHost);
            }
            TakenSecrets taken;
            QString error;
            ZStorage::ResetOutcome outcome;
            if (!temp->resetCloudEncryption(cfg, password, effectiveServerPassword, taken,
                                            mintParams_, &outcome, &error))
                return error;
            takenKey_ = taken.capturedKey;
            takenServerPassword_ = taken.capturedServerPassword;
            takenStoreId_ = taken.capturedServerPasswordFor;
            settled_ = temp->remoteConfig();
            settled_.name = temp->localStoreName();
            resetSummary_ = QStringLiteral("The cloud copy is replaced: %1 blobs removed, "
                                           "%2 journals and %3 attachments uploaded anew.")
                                .arg(outcome.wiped)
                                .arg(outcome.push.journals)
                                .arg(outcome.push.attachments);
            return {};
        },
        [this](const QString& error) {
            if (!error.isEmpty()) {
                say(error, true);
                return;
            }
            QString why;
            if (takenKey_.hasKey() && !secrets_->storeKey(takenKey_, &why))
                fprintf(stderr, "zametti: the keyring refused the key: %s\n", qPrintable(why));
            if (!takenServerPassword_.isEmpty() && !takenStoreId_.isEmpty())
                secrets_->setServerPassword(takenStoreId_, takenServerPassword_);
            takenKey_ = Keyfile();
            leaveResetMode();
            password_->clear();
            settleEntry(settled_);
            say(resetSummary_, false);
        });
}

}  // namespace zametti
