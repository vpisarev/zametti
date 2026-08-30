// Окно хранилищ: раскладка и исполнение. Решения — в модели, работы — в
// StoreJobRunner; см. шапку store_manager_dialog.h.

#include "store_manager_dialog.h"

#include "dialog_font.h"
#include "icons.h"

#include <QAction>
#include <QDialogButtonBox>
#include <QDir>
#include <QEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QIcon>
#include <QKeyEvent>
#include <QMessageBox>
#include <QMetaObject>
#include <QStyle>
#include <QValidator>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>

namespace zametti {

// Приёмник-копилка для рабочего потока: ядро кладёт сюда ключ и пароли, а в
// настоящий keyring (DBus при главном цикле) их перекладывает главный поток
// по завершении. Чтения здесь не живут: пути ядра, которыми ходит работа,
// ключей не спрашивают — нужный работе ключ главный поток подсаживает ДО
// запуска (seedKey).
class TakenSecrets : public SecretStore {
public:
    bool available() const override { return true; }
    // Копилка — не связка: она знает лишь то, что ей самой положили в этом
    // прогоне. Спрашивать её о состоянии связки бессмысленно, и ответ честен:
    // сказать нечего.
    bool has(const QString&, Secret) override { return false; }
    // ЧТЕНИЕ ОДНО И ТОЛЬКО ПОДСАЖЕННОЕ. Настоящую связку из рабочего потока
    // не спросить (DBus живёт при главном цикле), но ключ работе бывает нужен
    // — например смене пароля, которая ничего не стирает именно потому, что
    // ключ уже под рукой. Поэтому главный поток кладёт его сюда ДО запуска.
    void seedKey(const Keyfile& keyfile) { seeded_ = keyfile; }
    bool loadKey(const QString&, Keyfile* out, QString* error) override {
        if (!seeded_.hasKey()) {
            if (error) *error = QStringLiteral("no keyring in the worker thread");
            return false;
        }
        *out = seeded_;
        return true;
    }
    bool storeKey(const Keyfile& keyfile, QString*) override {
        capturedKey = keyfile;
        return true;
    }
    bool clearKey(const QString& storeId, QString*) override {
        forgotten.append(storeId);
        return true;
    }
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
    // Что работа попросила ЗАБЫТЬ (отвязка): переложится тем же шагом.
    QStringList forgotten;

protected:
    Keyfile seeded_;
};


// Поле, по которому ездит каретка, но правки не проходят: валидатор отвергает
// любой текст, кроме замороженного. Почему не setReadOnly: read-only QLineEdit
// не показывает каретку вовсе, а путь длиннее поля, и по нему нужно ездить,
// чтобы прочитать целиком (решение владельца, 28.08.2026). Программный
// setText валидатор не спрашивает — показ строки работает как прежде; пустая
// заморозка (QString()) отпускает поле для режима новой строки.
class FrozenText : public QValidator {
public:
    using QValidator::QValidator;
    void freeze(const QString& text) { frozen_ = text; }
    State validate(QString& input, int&) const override {
        return frozen_.isNull() || input == frozen_ ? Acceptable : Invalid;
    }

protected:
    QString frozen_;
};


StoreManagerDialog::StoreManagerDialog(QWidget* parent, ZStorageManager& stores,
                                       const QString& currentRoot,
                                       std::shared_ptr<SecretStore> secrets,
                                       const Keyfile::KdfParams& mintParams)
    : QDialog(parent),
      stores_(stores),
      model_(stores, currentRoot),
      runner_(mintParams),
      secrets_(std::move(secrets)) {
    setWindowTitle(QStringLiteral("Storages"));
    // Кегль — из настроек, как у остальной программы: системный дефолт на
    // FullHD-ноуте владельца мельче всего окна (dialog_font.h).
    setFont(dialogFont());

    // --- слева: рамка со списком и его кнопками (п.1 брифа) -----------------
    // Список и «+ −» живут В ОДНОЙ РАМКЕ: левая колонка кончается той же
    // чертой, что и правая, и низ у окна один — Open рядом с Close.
    list_ = new QListWidget(this);
    list_->setObjectName(QStringLiteral("storeList"));
    list_->setSelectionMode(QAbstractItemView::SingleSelection);
    // Список — без собственной рамки: рамка у колонки одна, внешняя.
    list_->setFrameShape(QFrame::NoFrame);

    addButton_ = new QPushButton(QStringLiteral("+"), this);
    addButton_->setObjectName(QStringLiteral("addStore"));
    addButton_->setToolTip(QStringLiteral("Add a storage: an existing one, a new empty "
                                          "one, or a download from the cloud"));
    removeButton_ = new QPushButton(QStringLiteral("−"), this);
    removeButton_->setObjectName(QStringLiteral("removeStore"));
    removeButton_->setToolTip(QStringLiteral("Forget this row — the folder and the cloud "
                                             "are not touched"));

    listFrame_ = new QFrame(this);
    listFrame_->setObjectName(QStringLiteral("listFrame"));
    listFrame_->setFrameShape(QFrame::StyledPanel);
    {
        auto* column = new QVBoxLayout(listFrame_);
        column->addWidget(list_, 1);
        auto* buttons = new QHBoxLayout;
        buttons->addWidget(addButton_);
        buttons->addWidget(removeButton_);
        buttons->addStretch(1);
        column->addLayout(buttons);
    }

    // --- справа: форма выбранной строки -------------------------------------
    folder_ = new QLineEdit(this);
    folder_->setObjectName(QStringLiteral("folder"));
    folderFreeze_ = new FrozenText(folder_);
    folder_->setValidator(folderFreeze_);
    // КНОПКА ВЫБОРА ПАПКИ. Папку у новой строки спрашивает сам «+» (п.3
    // брифа: плюс немедленно открывает выбор каталога), а эта нужна, чтобы
    // ПОМЕНЯТЬ путь: у новой строки — если ткнули не туда, у существующей
    // НЕОТКРЫТОЙ — если хранилище переехало (решение владельца, 28.08.2026).
    // У открытого хранилища погашена: его папка под замком. Троеточие говорит
    // «выбрать» на всех системах разом.
    //
    // ВЫСОТА У НЕЁ НАТУРАЛЬНАЯ, А СИДИТ ОНА НЕ В РАЗМЕТКЕ: в строке пути стоит
    // РАСПОРКА её размеров, сама же кнопка плавает над рамкой формы и ставится
    // по координатам распорки (placeBrowseButton). Владелец подгонял положение
    // и высоту живым полем прямо в окне и назвал оптимумом ровно такую посадку;
    // кнопка ЖЕ ВНУТРИ РАЗМЕТКИ сажает соседей иначе — замер 28.08.2026 по
    // снимкам окна: разметка ровняет по прямоугольникам элемента
    // (SE_PushButtonLayoutItem урезает кнопке поля фокусного кольца), и строка
    // от этого пересчитывается — поле подскакивает на две точки.
    //
    // ФИКСИРОВАННУЮ ВЫСОТУ НЕ СТАВИТЬ. Замер 28.08.2026: кнопку НЕСТАНДАРТНОЙ
    // высоты маковский стиль не умеет — рисует родной бэзель и ОБРЕЗАЕТ его.
    browseButton_ = new QPushButton(this);
    browseButton_->setObjectName(QStringLiteral("browse"));
    browseButton_->setToolTip(QStringLiteral("Choose another folder"));
    {
        const int points = style()->pixelMetric(QStyle::PM_SmallIconSize, nullptr, this);
        browseButton_->setIcon(QIcon(toolbarIcon(QStringLiteral("ellipsis"), points,
                                                 palette().color(QPalette::Text),
                                                 devicePixelRatioF())));
        // ШИРИНУ НЕ ТРОГАЕМ, И ЭТО ВАЖНО: сплющенная кнопка маковского стиля
        // теряет фаску и садится иначе (замер 28.08.2026 — расхождение
        // видимых центров у нетронутой кнопки 0.0 точек).
    }
    browseHolder_ = new QWidget(this);
    browseHolder_->setFixedSize(browseButton_->sizeHint());
    browseHolder_->installEventFilter(this);

    auto* folderRow = new QHBoxLayout;
    folderRow->addWidget(folder_, 1);
    folderRow->addWidget(browseHolder_);

    server_ = new QLineEdit(this);
    server_->setObjectName(QStringLiteral("server"));
    // ПАПКА ХРАНИЛИЩА ВНУТРИ СЕРВЕРА (решение владельца, 28.08.2026; пп.7-8
    // брифа): база провайдера сама бывает с путём (https://server/webdav), и
    // хранилищу нужен свой сегмент. Пустое поле — имя локальной папки; живой
    // дефолт показывает placeholder из снимка — форма значения, не поучение.
    serverDir_ = new QLineEdit(this);
    serverDir_->setObjectName(QStringLiteral("serverDir"));
    user_ = new QLineEdit(this);
    user_->setObjectName(QStringLiteral("user"));
    serverPassword_ = new QLineEdit(this);
    serverPassword_->setObjectName(QStringLiteral("serverPassword"));
    serverPassword_->setEchoMode(QLineEdit::Password);

    passwordLabel_ = new QLabel(QStringLiteral("Encryption password"), this);
    password_ = new QLineEdit(this);
    password_->setObjectName(QStringLiteral("password"));
    password_->setEchoMode(QLineEdit::Password);
    // Глаза-переключатели: включён — пароль виден и остаётся видимым (его
    // копируют не торопясь — просьба владельца); пустое поле наполняется
    // хранимым значением из keyring — затем keychain и служит местом, где
    // свой пароль можно подсмотреть.
    serverEye_ = addEyeToggle(serverPassword_, StoreManagerModel::FieldId::ServerPassword);
    passwordEye_ = addEyeToggle(password_, StoreManagerModel::FieldId::EncryptionPassword);
    password2Label_ = new QLabel(QStringLiteral("Repeat password"), this);
    password2_ = new QLineEdit(this);
    password2_->setObjectName(QStringLiteral("password2"));
    password2_->setEchoMode(QLineEdit::Password);

    checkButton_ = new QPushButton(QStringLiteral("Check"), this);
    checkButton_->setObjectName(QStringLiteral("check"));
    checkButton_->setToolTip(
        QStringLiteral("Reach the cloud and show what it holds — nothing is changed"));
    resetButton_ = new QPushButton(QStringLiteral("Reset cloud…"), this);
    resetButton_->setObjectName(QStringLiteral("resetCloud"));
    resetButton_->setToolTip(
        QStringLiteral("Change the encryption password, or erase the cloud copy"));

    // ДВЕ РАМКИ ФАКТОВ ПО ДВЕ СТРОКИ И ОДНА СТРОКА ПРО СОБЫТИЕ (п.5 брифа).
    // Факт и событие — разное, и делить им один ярлык значило бы «статус то
    // говорит, то молчит»: у рамок текст есть всегда, у строки обычно пусто.
    const auto makeFactFrame = [this](QFrame** frame, QLabel** line, const char* name) {
        *frame = new QFrame(this);
        (*frame)->setObjectName(QLatin1String(name) + QStringLiteral("Frame"));
        (*frame)->setFrameShape(QFrame::StyledPanel);
        *line = new QLabel(*frame);
        (*line)->setObjectName(QLatin1String(name));
        (*line)->setTextInteractionFlags(Qt::TextSelectableByMouse);
        auto* box = new QVBoxLayout(*frame);
        box->setContentsMargins(8, 4, 8, 4);
        box->addWidget(*line);
        // Рамка держит ДВЕ строки всегда, даже когда факт короткий: скачущая
        // при переключении строк высота — худшее, что может делать форма.
        (*line)->setMinimumHeight(2 * (*line)->fontMetrics().lineSpacing());
    };
    makeFactFrame(&localFrame_, &localLine_, "localLine");
    makeFactFrame(&cloudFrame_, &cloudLine_, "cloudLine");
    status_ = new QLabel(this);
    status_->setObjectName(QStringLiteral("status"));
    status_->setWordWrap(true);

    // РАМКА ВОКРУГ ФОРМЫ (решение владельца). Диалог тянется мышью, и рамка
    // даёт правой половине тело, равное по весу списку слева, и границу,
    // внутри которой поля растягиваются по горизонтали (п.9 брифа).
    formFrame_ = new QFrame(this);
    formFrame_->setObjectName(QStringLiteral("formFrame"));
    formFrame_->setFrameShape(QFrame::StyledPanel);
    // Кнопка при поле пути — ребёнок рамки, вне разметок (почему — у её
    // создания выше); гасится она вместе с формой как раз потому, что живёт
    // в рамке. После setParent виджет спрятан — показать.
    browseButton_->setParent(formFrame_);
    browseButton_->show();

    // СТРОКИ ФОРМЫ ДЕРЖАТ СВОЮ ВЫСОТУ, А ПУСТОТА УХОДИТ ВНИЗ РАМКИ:
    // промежуточная колонка (форма сверху, stretch снизу) не даёт QFormLayout
    // растягивать строки по высоте рамки — иначе стиль macOS сажает кнопку
    // при поле пути выше поля на две точки (замер 28.08.2026).
    auto* frameColumn = new QVBoxLayout(formFrame_);
    auto* form = new QFormLayout;
    frameColumn->addLayout(form);
    frameColumn->addStretch(1);
    // ДВЕ НАСТРОЙКИ ПРОТИВ ПОДСКАЗОК СТИЛЯ macOS, И БЕЗ НИХ ОКНО РАЗВАЛИВАЕТСЯ
    // (замер 28.08.2026, Qt 6.11.1): маковский стиль отвечает форме
    // FieldsStayAtSizeHint + AlignHCenter — поля не растут вовсе (125 точек,
    // путь обрезается посреди слова), а форма прижата к середине колонки.
    // Ровно то, что владелец увидел на маке (п.9 брифа). Выравнивание
    // ПОДПИСЕЙ не трогаем: справа — родная маковская привычка.
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setFormAlignment(Qt::AlignLeft | Qt::AlignTop);
    form->addRow(QStringLiteral("Folder"), folderRow);
    form->addRow(QStringLiteral("Cloud server (WebDAV)"), server_);
    form->addRow(QStringLiteral("Cloud dir"), serverDir_);
    form->addRow(QStringLiteral("Login"), user_);
    form->addRow(QStringLiteral("Server password"), serverPassword_);
    form->addRow(passwordLabel_, password_);
    form->addRow(password2Label_, password2_);
    auto* actionRow = new QHBoxLayout;
    actionRow->addWidget(checkButton_);
    actionRow->addWidget(resetButton_);
    actionRow->addStretch(1);
    form->addRow(QString(), actionRow);

    // Рамки фактов — сразу за кнопками, во всю ширину формы; строка события
    // под ними: читать её надо там же, где нажимал.
    form->addRow(localFrame_);
    form->addRow(cloudFrame_);
    form->addRow(status_);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    closeButton_ = buttons->button(QDialogButtonBox::Close);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    // ДВЕ РАМКИ НА ОБЩУЮ ВЫСОТУ, ВЫХОДЫ — ОДНИМ РЯДОМ ВНИЗУ (пп.1-2 брифа).
    // «Open» и «Close» стоят вместе: это два способа ИЗ ОКНА ВЫЙТИ, а не
    // действие над списком; «+ −» правят список и живут при нём, под ним.
    //
    // Open — обычной кнопкой, а не третьей в QDialogButtonBox: у коробки
    // кнопка с ролью Accept становится кнопкой по умолчанию, и Enter в поле
    // пароля переключал бы хранилище.
    openButton_ = new QPushButton(QStringLiteral("Open"), this);
    openButton_->setObjectName(QStringLiteral("openStore"));

    auto* columns = new QHBoxLayout;
    columns->addWidget(listFrame_, 2);
    columns->addWidget(formFrame_, 3);

    auto* bottom = new QHBoxLayout;
    bottom->addStretch(1);
    bottom->addWidget(openButton_);
    bottom->addWidget(buttons);

    auto* whole = new QVBoxLayout(this);
    whole->addLayout(columns, 1);
    whole->addLayout(bottom);
    // Ширина — под кегль из настроек: на 14pt прежние 760 обрезали и путь, и
    // подсказки полей. Высота — под форму с двумя рамками фактов.
    resize(900, 560);

    connect(addButton_, &QPushButton::clicked, this, &StoreManagerDialog::addStore);
    connect(removeButton_, &QPushButton::clicked, this,
            [this] { act(model_.forgetPressed()); });
    connect(openButton_, &QPushButton::clicked, this,
            [this] { act(model_.openPressed()); });
    connect(list_, &QListWidget::currentRowChanged, this, [this](int row) {
        if (busy_ || row < 0 || row == model_.selected()) return;
        model_.select(row);
        render();
    });
    connect(list_, &QListWidget::itemDoubleClicked, this,
            [this](QListWidgetItem*) { act(model_.openPressed()); });
    connect(checkButton_, &QPushButton::clicked, this,
            [this] { act(model_.checkPressed()); });
    connect(resetButton_, &QPushButton::clicked, this,
            [this] { act(model_.resetPressed()); });
    connect(browseButton_, &QPushButton::clicked, this, [this] {
        const QString dir = askFolder();
        if (dir.isEmpty()) return;
        model_.edit(StoreManagerModel::FieldId::Folder, dir);
        render();
    });

    // ПРАВКА ПОЛЯ УХОДИТ В МОДЕЛЬ, И ОТТУДА ЖЕ ВОЗВРАЩАЕТСЯ ВЕСЬ ВИД. Своих
    // решений у окна нет: доступность кнопок, цвет подсказки, видимость поля
    // повтора — всё это снимок, посчитанный один раз.
    //
    // textEdited, а НЕ textChanged: программный setText внутри render() не
    // должен выглядеть правкой человека — иначе заглушка связки стиралась бы
    // сама собой, и «пароль здесь есть» превращалось бы в красное требование.
    const auto wire = [this](QLineEdit* field, StoreManagerModel::FieldId which) {
        connect(field, &QLineEdit::textEdited, this, [this, which](const QString& text) {
            model_.edit(which, text);
            render();
        });
    };
    wire(folder_, StoreManagerModel::FieldId::Folder);
    wire(server_, StoreManagerModel::FieldId::Server);
    wire(serverDir_, StoreManagerModel::FieldId::ServerDir);
    wire(user_, StoreManagerModel::FieldId::Login);
    wire(serverPassword_, StoreManagerModel::FieldId::ServerPassword);
    wire(password_, StoreManagerModel::FieldId::EncryptionPassword);
    wire(password2_, StoreManagerModel::FieldId::Repeat);

    // Заглушку связки стирает ПЕРВОЕ НАЖАТИЕ КЛАВИШИ, а не фокус: пройти по
    // полям табом человек вправе, ничего при этом не потеряв.
    serverPassword_->installEventFilter(this);
    password_->installEventFilter(this);

    render();
}

StoreManagerDialog::~StoreManagerDialog() {
    if (worker_.joinable()) worker_.join();
    stashAll();
}

// ЛЕКАРСТВО ОТ СКЛЕРОЗА (закон владельца, 30.08.2026: «что бы я ни ввёл —
// пусть даже неправильно и соединение не состоялось — должно сохраняться и
// восстанавливаться на следующем запуске»). Адрес и логин уезжают в строки
// списка (state.json пишет ZApp на выходе); набранные пароли — в связку, по
// одному правилу: У КОГО ЗАПИСЬ УЖЕ ЕСТЬ, ТОГО ОБНОВЛЯЮТ ТОЛЬКО УДАЧНЫЕ
// РАБОТЫ — иначе опечатка затёрла бы проверенный пароль.
void StoreManagerDialog::stashAll() {
    model_.stashDrafts();
    const QHash<QString, StoreManagerModel::Draft>& drafts = model_.drafts();
    for (auto it = drafts.constBegin(); it != drafts.constEnd(); ++it) {
        const StoreManagerModel::Draft& d = *it;
        const bool typedServer = d.serverPasswordTouched && !d.serverPassword.isEmpty();
        const bool typedCrypt = d.encryptionTouched && !d.encryptionPassword.isEmpty();
        if (!typedServer && !typedCrypt) continue;
        if (ZStorage::inspect(it.key()) != ZStorage::DirKind::Store) continue;
        const QString id = ZStorage(it.key()).identity().storeId();
        if (id.isEmpty()) continue;
        QString why;
        if (typedServer && !secrets_->has(id, SecretStore::Secret::ServerPassword))
            secrets_->setServerPassword(id, d.serverPassword, &why);
        if (typedCrypt && !secrets_->has(id, SecretStore::Secret::EncryptionPassword))
            secrets_->setEncryptionPassword(id, d.encryptionPassword, &why);
    }
}

void StoreManagerDialog::adoptDrafts(QHash<QString, StoreManagerModel::Draft>* drafts) {
    model_.adoptDrafts(drafts);
    render();
}

// --- ЕДИНСТВЕННАЯ ДОРОГА ОТ МОДЕЛИ К ЭКРАНУ ---------------------------------

void StoreManagerDialog::rebuildList(const StoreManagerModel::Snapshot& snap) {
    QStringList roots;
    for (const StoreManagerModel::Row& r : snap.rows) roots.append(r.root);
    if (roots == shownRoots_ && list_->count() == snap.rows.size()) {
        // Состав тот же — переписываем только подписи: имя корневой заметки
        // могло приехать после работы.
        for (int i = 0; i < snap.rows.size(); ++i) {
            const StoreManagerModel::Row& r = snap.rows.at(i);
            const QString title = r.open ? r.title + QStringLiteral("  •  open") : r.title;
            if (list_->item(i)->text() != title) list_->item(i)->setText(title);
        }
        return;
    }
    shownRoots_ = roots;
    const QSignalBlocker quiet(list_);
    list_->clear();
    for (const StoreManagerModel::Row& r : snap.rows) {
        auto* item = new QListWidgetItem(
            r.open ? r.title + QStringLiteral("  •  open") : r.title, list_);
        item->setToolTip(r.root);
    }
}

void StoreManagerDialog::render() {
    const StoreManagerModel::Snapshot snap = model_.snapshot();
    rebuildList(snap);
    if (list_->currentRow() != snap.selected) {
        const QSignalBlocker quiet(list_);
        list_->setCurrentRow(snap.selected);
    }

    // Поле не переписывается, когда текст тот же: иначе каретка прыгала бы в
    // начало на каждом нажатии клавиши.
    const auto put = [](QLineEdit* field, const QString& text) {
        if (field->text() != text) field->setText(text);
    };
    put(folder_, snap.folder);
    // Путь ОТКРЫТОГО хранилища под замком: по нему ездят кареткой, но не
    // правят (заморозка валидатором, а не setReadOnly — иначе каретки нет).
    folderFreeze_->freeze(snap.folderFrozen ? snap.folder : QString());
    folder_->setToolTip(snap.folder);
    put(password2_, snap.repeat);

    // Обычные поля тоже ведёт снимок: доступность и подсказка (каталог-облако
    // гасит логин и серверную папку и говорит об этом сам).
    const auto putField = [&put](QLineEdit* field, const StoreManagerModel::Field& state) {
        put(field, state.text);
        field->setPlaceholderText(state.placeholder);
        field->setEnabled(state.enabled);
    };
    putField(server_, snap.server);
    putField(serverDir_, snap.serverDir);
    putField(user_, snap.login);

    // ПАРОЛЬ В СВЯЗКЕ ПОКАЗЫВАЕТСЯ КРУЖОЧКАМИ — как настоящий (поправка
    // владельца). Это подстановка показа: наружу она не уходит никогда, а
    // первое нажатие клавиши её стирает (eventFilter ниже).
    const auto putSecret = [&put](QLineEdit* field, QAction* eye,
                                  const StoreManagerModel::Field& state) {
        put(field, state.stub ? QString(8, QChar(0x2022)) : state.text);
        field->setPlaceholderText(state.placeholder);
        field->setEnabled(state.enabled);
        field->setStyleSheet(state.placeholderAlarm ? QStringLiteral("color: #c03030;")
                                                    : QString());
        eye->setEnabled(state.eyeEnabled);
        if (!state.eyeEnabled) eye->setChecked(false);
    };
    putSecret(serverPassword_, serverEye_, snap.serverPassword);
    putSecret(password_, passwordEye_, snap.encryptionPassword);

    password2Label_->setVisible(snap.repeatVisible);
    password2_->setVisible(snap.repeatVisible);

    const auto show = [](QPushButton* button, const StoreManagerModel::Button& state) {
        if (!state.label.isEmpty() && button->text() != state.label)
            button->setText(state.label);
        button->setEnabled(state.enabled);
    };
    show(addButton_, snap.add);
    show(removeButton_, snap.remove);
    show(browseButton_, snap.browse);
    show(checkButton_, snap.check);
    show(resetButton_, snap.reset);
    show(openButton_, snap.open);
    closeButton_->setEnabled(snap.close.enabled);

    // ЗАНЯТОСТЬ — ПРОСТО ЕЩЁ ОДНО СЛАГАЕМОЕ, а не свой набор правил: пока
    // работа идёт, форма глохнет целиком, а решает всё тот же снимок.
    if (busy_) {
        formFrame_->setEnabled(false);
        addButton_->setEnabled(false);
        removeButton_->setEnabled(false);
        openButton_->setEnabled(false);
    } else {
        formFrame_->setEnabled(!snap.rows.isEmpty());
    }

    const auto line = [](QLabel* label, const StoreManagerModel::Line& state) {
        label->setText(state.text);
        label->setStyleSheet(state.alarm ? QStringLiteral("color: #c03030;") : QString());
    };
    line(localLine_, snap.local);
    line(cloudLine_, snap.cloud);
    line(status_, snap.message);
    placeBrowseButton();
}

// --- намерение модели -------------------------------------------------------

int StoreManagerDialog::ask(const StoreManagerModel::Question& question) {
    QMessageBox box(this);
    box.setWindowTitle(QStringLiteral("zametti"));
    box.setText(question.text);
    if (!question.detail.isEmpty()) box.setInformativeText(question.detail);
    QList<QPushButton*> buttons;
    for (int i = 0; i < question.choices.size(); ++i) {
        const bool last = i + 1 == question.choices.size();
        buttons.append(box.addButton(question.choices.at(i),
                                     last ? QMessageBox::RejectRole
                                          : QMessageBox::DestructiveRole));
    }
    if (!buttons.isEmpty()) box.setDefaultButton(buttons.last());
    box.exec();
    for (int i = 0; i < buttons.size(); ++i)
        if (box.clickedButton() == buttons.at(i)) return i;
    return int(buttons.size()) - 1;   // закрыли крестиком — это отказ
}

void StoreManagerDialog::act(const StoreManagerModel::Reaction& reaction) {
    if (reaction.question.kind != StoreManagerModel::Question::Kind::None) {
        const int choice = ask(reaction.question);
        act(model_.answered(reaction.question.kind, choice));
        return;
    }
    if (reaction.job.kind != StoreManagerModel::Job::Kind::None) {
        runJob(reaction.job);
        return;
    }
    if (!reaction.switchToRoot.isEmpty()) {
        result_.switchToRoot = reaction.switchToRoot;
        accept();
        return;
    }
    if (reaction.close) {
        // «−» по открытой строке отцепляет хранилище НЕМЕДЛЕННО, не дожидаясь
        // закрытия окна: окно программы живёт без хранилища (папка Info).
        if (detachCurrent_) detachCurrent_();
        render();
        return;
    }
    render();
}

void StoreManagerDialog::runJob(const StoreManagerModel::Job& job) {
    // СВЯЗКУ СПРАШИВАЕМ ЗДЕСЬ, В ГЛАВНОМ ПОТОКЕ, и только по явному жесту.
    // Признак «возьми из связки» снимается тут же: дальше едут настоящие
    // секреты, а кружочки показа наружу не выходят никогда.
    StoreManagerModel::Job resolved = job;
    const QString root = ZStorageManager::canonicalRoot(job.root);
    const QString storeId = ZStorage::inspect(root) == ZStorage::DirKind::Store
                                ? ZStorage(root).identity().storeId()
                                : QString();
    taken_ = std::make_shared<TakenSecrets>();
    if (!storeId.isEmpty()) {
        // ЧТЕНИЙ — РОВНО СТОЛЬКО, СКОЛЬКО НУЖНО РАБОТЕ, И НИ ОДНИМ БОЛЬШЕ:
        // каждое чтение ДАННЫХ из связки на ad-hoc сборке — системный вопрос
        // макоси, и первая живая проба владельца получала его на каждый жест
        // (30.08). Пароль сервера — только когда он серверу нужен; пароль
        // шифрования из связки работам не нужен вовсе (запечатывание и смена
        // пароля требуют НАБРАННОГО); ключ — только смене пароля и
        // подключению без пароля.
        if (job.serverPasswordFromKeyring && !job.cfg.cloudUrl.isEmpty())
            resolved.serverPassword = secrets_->serverPassword(storeId);
        // Ключ подсаживается в копилку целиком: спросить настоящую связку из
        // рабочего потока нельзя.
        const bool jobWantsKey =
            job.kind == StoreManagerModel::Job::Kind::ChangePassword ||
            (job.kind == StoreManagerModel::Job::Kind::Check &&
             resolved.encryptionPassword.isEmpty());
        if (jobWantsKey) {
            Keyfile key;
            if (secrets_->loadKey(storeId, &key, nullptr)) taken_->seedKey(key);
        }
    }
    resolved.serverPasswordFromKeyring = false;
    resolved.encryptionFromKeyring = false;
    runningJob_ = resolved;

    startWork(
        QStringLiteral("Working…"),
        [this] { outcome_ = runner_.run(runningJob_, *taken_); },
        [this] {
            // ДОБЫТОЕ — В НАСТОЯЩУЮ СВЯЗКУ, ВСЕ ТРИ ЗАПИСИ. Прежде ветка
            // сброса переносила ключ и пароль сервера, а пароль шифрования
            // забывала — и человек, только что введший его дважды, получал
            // красное «set encryption password» (жалоба владельца, 29.08.2026).
            QString why;
            if (taken_->capturedKey.hasKey() && !secrets_->storeKey(taken_->capturedKey, &why))
                model_.setMessage(QStringLiteral("The keyring refused the key: %1").arg(why),
                                  true);
            if (!taken_->capturedServerPassword.isEmpty() &&
                !taken_->capturedServerPasswordFor.isEmpty())
                secrets_->setServerPassword(taken_->capturedServerPasswordFor,
                                            taken_->capturedServerPassword, &why);
            if (!taken_->capturedEncryptionPassword.isEmpty() &&
                !taken_->capturedEncryptionPasswordFor.isEmpty())
                secrets_->setEncryptionPassword(taken_->capturedEncryptionPasswordFor,
                                                taken_->capturedEncryptionPassword, &why);
            for (const QString& forgotten : taken_->forgotten) {
                secrets_->clearKey(forgotten, &why);
                secrets_->clearServerPassword(forgotten, &why);
                secrets_->clearEncryptionPassword(forgotten, &why);
            }
            taken_.reset();

            const bool wasOurCloud =
                runningJob_.kind != StoreManagerModel::Job::Kind::None && outcome_.ok;
            act(model_.jobFinished(runningJob_.kind, outcome_));
            if (wasOurCloud && model_.isOpenRow()) result_.cloudChangedForCurrent = true;
            if (outcome_.ok && outcome_.downloadedNew) result_.downloadedNew = true;
            // Заведённое хранилище — то, ради чего жали кнопку: открываем его
            // и уходим. Скачивание при этом ведёт фоновый прогон.
            if (outcome_.ok && runningJob_.kind == StoreManagerModel::Job::Kind::Create) {
                result_.switchToRoot = runningJob_.root;
                accept();
                return;
            }
            render();
        });
}

// --- жесты окна -------------------------------------------------------------

QString StoreManagerDialog::askFolder() {
    const QString start = folder_->text().trimmed();
    return QFileDialog::getExistingDirectory(this, QStringLiteral("Storage folder"), start);
}

void StoreManagerDialog::addStore() {
    // «+» немедленно открывает выбор каталога (п.3 брифа): пустая ли папка,
    // хранилище или чужая — решает модель по факту выбора.
    const QString dir = askFolder();
    if (dir.isEmpty()) return;
    addFolder(dir);
}

void StoreManagerDialog::addFolder(const QString& dir) {
    act(model_.addFolder(dir));
}

void StoreManagerDialog::dropSelected() {
    act(model_.answered(StoreManagerModel::Question::Kind::Forget, 0));
}

void StoreManagerDialog::chooseReset(int road) {
    act(model_.answered(StoreManagerModel::Question::Kind::ResetCloud, road));
}

QAction* StoreManagerDialog::addEyeToggle(QLineEdit* field, StoreManagerModel::FieldId which) {
    QIcon icon;
    const int points = style()->pixelMetric(QStyle::PM_SmallIconSize, nullptr, this);
    const QColor color = palette().color(QPalette::Text);
    const qreal dpr = devicePixelRatioF();
    icon.addPixmap(toolbarIcon(QStringLiteral("eye"), points, color, dpr), QIcon::Normal,
                   QIcon::On);
    icon.addPixmap(toolbarIcon(QStringLiteral("eye-off"), points, color, dpr), QIcon::Normal,
                   QIcon::Off);
    QAction* eye = field->addAction(icon, QLineEdit::TrailingPosition);
    eye->setCheckable(true);
    eye->setToolTip(QStringLiteral("Show the password"));
    connect(eye, &QAction::toggled, this, [this, field, which](bool on) {
        field->setEchoMode(on ? QLineEdit::Normal : QLineEdit::Password);
        if (!on) return;
        // ГЛАЗ ДОСТАЁТ НАСТОЯЩИЙ ПАРОЛЬ ИЗ СВЯЗКИ — ради этого связка и
        // хранит его третьей записью (решение владельца): подсмотреть и
        // скопировать свой пароль больше негде. Спрашиваем ЯВНЫМ жестом и в
        // главном потоке: на маке чтение секрета вправе поднять системный
        // вопрос, и на переключение строки его звать нельзя.
        const QString root = ZStorageManager::canonicalRoot(folder_->text().trimmed());
        if (ZStorage::inspect(root) != ZStorage::DirKind::Store) return;
        const QString id = ZStorage(root).identity().storeId();
        if (id.isEmpty()) return;
        const QString kept = which == StoreManagerModel::FieldId::ServerPassword
                                 ? secrets_->serverPassword(id)
                                 : secrets_->encryptionPassword(id);
        if (kept.isEmpty()) return;
        // Показанное становится набранным: поле и модель обязаны говорить об
        // одном, иначе следующая работа поедет с заглушкой.
        model_.edit(which, kept);
        render();
    });
    return eye;
}

bool StoreManagerDialog::eventFilter(QObject* watched, QEvent* event) {
    if (watched == browseHolder_ &&
        (event->type() == QEvent::Move || event->type() == QEvent::Resize))
        placeBrowseButton();

    // ПЕРВОЕ НАЖАТИЕ КЛАВИШИ СТИРАЕТ ЗАГЛУШКУ СВЯЗКИ. Не фокус: пройти по
    // полям табом человек вправе, ничего не потеряв, — а вот начал печатать,
    // значит вводит свой пароль, и кружочкам там больше не место.
    if (event->type() == QEvent::KeyPress &&
        (watched == serverPassword_ || watched == password_)) {
        const auto* key = static_cast<QKeyEvent*>(event);
        const bool typing = !key->text().isEmpty() || key->key() == Qt::Key_Backspace ||
                            key->key() == Qt::Key_Delete;
        const StoreManagerModel::Snapshot snap = model_.snapshot();
        const bool stub = watched == serverPassword_ ? snap.serverPassword.stub
                                                     : snap.encryptionPassword.stub;
        if (typing && stub) {
            model_.edit(watched == serverPassword_
                            ? StoreManagerModel::FieldId::ServerPassword
                            : StoreManagerModel::FieldId::EncryptionPassword,
                        QString());
            static_cast<QLineEdit*>(watched)->clear();
            render();
        }
    }
    return QDialog::eventFilter(watched, event);
}

void StoreManagerDialog::placeBrowseButton() {
    if (browseHolder_ == nullptr || browseButton_ == nullptr || formFrame_ == nullptr) return;
    const QPoint at = formFrame_->mapFrom(browseHolder_->parentWidget(),
                                          browseHolder_->pos());
    browseButton_->setGeometry(QRect(at, browseHolder_->size()));
}

// --- потоки -----------------------------------------------------------------

void StoreManagerDialog::startWork(const QString& status, std::function<void()> job,
                                   std::function<void()> done) {
    setBusy(true);
    model_.setMessage(status);
    render();
    if (worker_.joinable()) worker_.join();
    worker_ = std::thread([this, job = std::move(job), done = std::move(done)] {
        job();
        // Диалог живёт дольше потока: деструктор ждёт join.
        QMetaObject::invokeMethod(
            this,
            [this, done] {
                setBusy(false);
                done();
            },
            Qt::QueuedConnection);
    });
}

void StoreManagerDialog::setBusy(bool on) {
    busy_ = on;
    render();
}

void StoreManagerDialog::reject() {
    if (busy_) return;   // рабочий поток держит хранилище — его дожидаются
    QDialog::reject();
}

void StoreManagerDialog::accept() {
    if (busy_) return;
    QDialog::accept();
}

}  // namespace zametti
