// Окно хранилищ: раскладка и исполнение. Решения и работы — в ZStorageManager;
// см. шапку store_manager_dialog.h.

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
#include <QPainter>
#include <QStyle>
#include <QValidator>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>

namespace zametti {
namespace {

// Цвет тревоги — ИЗ ТЕМЫ (роль danger), а не «#c03030» строкой. Три места в
// этом окне красили текст красным вручную, и в тёмной теме они остались бы
// единственными, кто не знает про тему вовсе.
QString alarmStyle() {
    return QStringLiteral("color: %1;")
        .arg(settings().ui().statusSuspectColor().name(QColor::HexRgb));
}

}  // namespace

namespace {

// Слабая горизонтальная черта: разделители внутри окна не должны кричать.
QFrame* faintRule(QWidget* parent) {
    auto* rule = new QFrame(parent);
    rule->setFrameShape(QFrame::HLine);
    rule->setFrameShadow(QFrame::Plain);
    QPalette faint = rule->palette();
    QColor c = faint.color(QPalette::Text);
    c.setAlphaF(0.2);
    faint.setColor(QPalette::WindowText, c);
    rule->setPalette(faint);
    return rule;
}

}  // namespace

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
                                       std::shared_ptr<SecretStore> secrets)
    : QDialog(parent), stores_(stores), secrets_(std::move(secrets)) {
    stores_.beginSession();
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
    // Иконная колонка под галочку открытого (openMark): глиф ниже строчных
    // букв, полглифа воздуха справа — имена всех строк стоят по одной букве.
    {
        const int mark = list_->fontMetrics().ascent();
        list_->setIconSize(QSize(mark + mark / 2, mark));
    }

    // «+ −» — МАЛЕНЬКИЕ КВАДРАТНЫЕ, футер списка (решение владельца 31.08 по
    // docs/zametti-storages-dialog-refactoring.md): операции над списком, а не
    // команды диалога. Квадрат — только шириной: фиксированную ВЫСОТУ кнопке
    // ставить нельзя, маковский стиль рисует родной бэзель и обрезает его
    // (замер 28.08.2026 у кнопки browse ниже).
    addButton_ = new QPushButton(QStringLiteral("+"), this);
    addButton_->setObjectName(QStringLiteral("addStore"));
    addButton_->setToolTip(QStringLiteral("Add a storage: an existing one, a new empty "
                                          "one, or a download from the cloud"));
    addButton_->setFixedWidth(addButton_->sizeHint().height());
    removeButton_ = new QPushButton(QStringLiteral("−"), this);
    removeButton_->setObjectName(QStringLiteral("removeStore"));
    removeButton_->setToolTip(QStringLiteral("Forget this row — the folder and the cloud "
                                             "are not touched"));
    removeButton_->setFixedWidth(removeButton_->sizeHint().height());

    listFrame_ = new QFrame(this);
    listFrame_->setObjectName(QStringLiteral("listFrame"));
    listFrame_->setFrameShape(QFrame::StyledPanel);
    {
        auto* column = new QVBoxLayout(listFrame_);
        column->addWidget(list_, 1);
        // Слабая черта отделяет футер с кнопками от строк: «список + операции
        // над списком» читается одним взглядом.
        column->addWidget(faintRule(listFrame_));
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
    serverEye_ = addEyeToggle(serverPassword_, ZStorageManager::FieldId::ServerPassword);
    passwordEye_ = addEyeToggle(password_, ZStorageManager::FieldId::EncryptionPassword);
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

    // ДВЕ СТРОКИ ФАКТОВ ПО ДВЕ СТРОКИ ТЕКСТА И ОДНА СТРОКА ПРО СОБЫТИЕ (п.5
    // брифа). Факт и событие — разное, и делить им один ярлык значило бы
    // «статус то говорит, то молчит»: у фактов текст есть всегда, у строки
    // события обычно пусто. Рамок вокруг фактов больше нет (решение владельца
    // 31.08): прямоугольники выглядели как поля ввода; факт — просто текст.
    const auto makeFactLine = [this](QLabel** line, const char* name) {
        *line = new QLabel(this);
        (*line)->setObjectName(QLatin1String(name));
        (*line)->setTextInteractionFlags(Qt::TextSelectableByMouse);
        (*line)->setAlignment(Qt::AlignLeft | Qt::AlignTop);
        // Факт отодвинут от полей/кнопок своей секции на ЛИШНИЙ шаг формы
        // (третья проба владельца 31.08: было dy — стало 2·dy). Ровно на этот
        // же dy ниже урезаны отступы заголовка Cloud и строки события — общие
        // вертикальные расстояния секций не изменились.
        (*line)->setContentsMargins(0, formStep(), 0, 0);
        // Резерв на ДВЕ строки всегда, даже когда факт короткий: скачущая
        // при переключении строк высота — худшее, что может делать форма.
        // ВМЕСТЕ С ОТСТУПОМ ВЫШЕ: отступ лежит внутри высоты ярлыка, и резерв
        // «две строки» без него — это полторы строки. Так и было (живая
        // жалоба владельца 05.09.2026, .testdata/zametti_storages_dialog_defect.png):
        // при его кегле форма не влезала в стартовую высоту окна, единственной
        // сжимаемой строкой формы был факт, и «modified …» срезалось пополам.
        // Минимум ярлыка — это и минимум окна: ниже него окно не сожмётся.
        (*line)->setMinimumHeight((*line)->contentsMargins().top() +
                                  2 * (*line)->fontMetrics().lineSpacing());
    };
    makeFactLine(&localLine_, "localLine");
    makeFactLine(&cloudLine_, "cloudLine");
    status_ = new QLabel(this);
    status_->setObjectName(QStringLiteral("status"));
    status_->setWordWrap(true);
    // Строке события — воздух сверху: она про жест, а не продолжение фактов
    // (просьба владельца 31.08; «ещё больше» — вторая проба; минус dy,
    // ушедший факту выше, — третья).
    status_->setContentsMargins(
        0, qMax(0, status_->fontMetrics().lineSpacing() - formStep()), 0, 0);

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
    // Подписи — по правому краю на всех системах (просьба владельца 31.08;
    // на маке это и так родная привычка стиля).
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);

    // ДВЕ СЕКЦИИ ВМЕСТО ОДНОЙ ДЛИННОЙ ФОРМЫ (решение владельца 31.08 по
    // docs/zametti-storages-dialog-refactoring.md): Local и Cloud
    // synchronization — концептуально разные сущности, и путь в каждой зовётся
    // просто Folder — секция сама говорит, чей он. Факт каждой сущности живёт
    // в её секции: локальный — под папкой, облачный — под кнопками, строка
    // события — в самом низу, там же, где нажимали.
    const auto sectionHeader = [this](const QString& text, bool gapAbove) {
        auto* label = new QLabel(text, this);
        QFont bold = label->font();
        bold.setBold(true);
        label->setFont(bold);
        // Минус dy, ушедший локальному факту (третья проба владельца 31.08):
        // расстояние Local ↔ Cloud synchronization в сумме прежнее.
        const int gap = qMax(0, label->fontMetrics().lineSpacing() - formStep());
        label->setContentsMargins(0, gapAbove ? gap : 0, 0, 2);
        return label;
    };
    form->addRow(sectionHeader(QStringLiteral("Local"), false));
    form->addRow(QStringLiteral("Folder"), folderRow);
    form->addRow(localLine_);
    // Неброская черта между секциями и воздух вокруг неё (просьбы владельца
    // 31.08, обе пробы): заголовок второй секции несёт отступ сверху сам.
    form->addRow(faintRule(formFrame_));
    form->addRow(sectionHeader(QStringLiteral("Cloud synchronization"), true));
    form->addRow(QStringLiteral("Server (WebDAV)"), server_);
    form->addRow(QStringLiteral("Folder"), serverDir_);
    form->addRow(QStringLiteral("Login"), user_);
    form->addRow(QStringLiteral("Server password"), serverPassword_);
    form->addRow(passwordLabel_, password_);
    form->addRow(password2Label_, password2_);
    auto* actionRow = new QHBoxLayout;
    actionRow->addWidget(checkButton_);
    actionRow->addWidget(resetButton_);
    actionRow->addStretch(1);
    form->addRow(QString(), actionRow);
    form->addRow(cloudLine_);
    form->addRow(status_);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    closeButton_ = buttons->button(QDialogButtonBox::Close);
    // Без иконки: тема Linux вешает на Close красный крест, и безобидное
    // «закрыть окно» выглядит деструктивным (решение владельца 31.08).
    closeButton_->setIcon(QIcon());
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
    // подсказки полей. Высота — под форму с двумя рамками фактов, а просит
    // форма больше (крупный кегль оболочки) — по её просьбе: окно, которому
    // форма не влезла, сжимает строки до минимума, и первым — факт.
    const QSize wanted = sizeHint();
    resize(qMax(900, wanted.width()), qMax(560, wanted.height()));

    connect(addButton_, &QPushButton::clicked, this, &StoreManagerDialog::addStore);
    connect(removeButton_, &QPushButton::clicked, this,
            [this] { act(stores_.forgetPressed()); });
    connect(openButton_, &QPushButton::clicked, this,
            [this] { act(stores_.openPressed()); });
    connect(list_, &QListWidget::currentRowChanged, this, [this](int row) {
        if (busy_ || row < 0 || row == stores_.selected()) return;
        stores_.select(row);
        render();
        // Строке с паролем в связке Check выполняется сам (владелец, 31.08).
        act(stores_.maybeAutoCheck());
    });
    connect(list_, &QListWidget::itemDoubleClicked, this,
            [this](QListWidgetItem*) { act(stores_.openPressed()); });
    connect(checkButton_, &QPushButton::clicked, this,
            [this] { act(stores_.checkPressed()); });
    connect(resetButton_, &QPushButton::clicked, this,
            [this] { act(stores_.resetPressed()); });
    connect(browseButton_, &QPushButton::clicked, this, [this] {
        const QString dir = askFolder();
        if (dir.isEmpty()) return;
        stores_.edit(ZStorageManager::FieldId::Folder, dir);
        render();
    });

    // ПРАВКА ПОЛЯ УХОДИТ В МОДЕЛЬ, И ОТТУДА ЖЕ ВОЗВРАЩАЕТСЯ ВЕСЬ ВИД. Своих
    // решений у окна нет: доступность кнопок, цвет подсказки, видимость поля
    // повтора — всё это снимок, посчитанный один раз.
    //
    // textEdited, а НЕ textChanged: программный setText внутри render() не
    // должен выглядеть правкой человека — иначе заглушка связки стиралась бы
    // сама собой, и «пароль здесь есть» превращалось бы в красное требование.
    const auto wire = [this](QLineEdit* field, ZStorageManager::FieldId which) {
        connect(field, &QLineEdit::textEdited, this, [this, which](const QString& text) {
            stores_.edit(which, text);
            render();
        });
    };
    wire(folder_, ZStorageManager::FieldId::Folder);
    wire(server_, ZStorageManager::FieldId::Server);
    wire(serverDir_, ZStorageManager::FieldId::ServerDir);
    wire(user_, ZStorageManager::FieldId::Login);
    wire(serverPassword_, ZStorageManager::FieldId::ServerPassword);
    wire(password_, ZStorageManager::FieldId::EncryptionPassword);
    wire(password2_, ZStorageManager::FieldId::Repeat);

    // Заглушку связки стирает ПЕРВОЕ НАЖАТИЕ КЛАВИШИ, а не фокус: пройти по
    // полям табом человек вправе, ничего при этом не потеряв.
    serverPassword_->installEventFilter(this);
    password_->installEventFilter(this);

    render();
    // Первый показ — то же правило, что и выбор строки: у кого пароль сервера
    // в связке, тому Check выполняется сам (владелец, 31.08).
    act(stores_.maybeAutoCheck());
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
    stores_.stashDrafts();
    const QHash<QString, ZStorageManager::Draft>& drafts = stores_.drafts();
    for (auto it = drafts.constBegin(); it != drafts.constEnd(); ++it) {
        const ZStorageManager::Draft& d = *it;
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

int StoreManagerDialog::formStep() const {
    const int step =
        style()->pixelMetric(QStyle::PM_LayoutVerticalSpacing, nullptr, this);
    return step > 0 ? step : 6;
}

// --- ЕДИНСТВЕННАЯ ДОРОГА ОТ МЕНЕДЖЕРА К ЭКРАНУ ------------------------------

void StoreManagerDialog::rebuildList(const ZStorageManager::Snapshot& snap) {
    QStringList roots;
    for (const ZStorageManager::Row& r : snap.rows) roots.append(r.root);
    if (roots == shownRoots_ && list_->count() == snap.rows.size()) {
        // Состав тот же — переписываем только подписи: имя корневой заметки
        // могло приехать после работы.
        for (int i = 0; i < snap.rows.size(); ++i) {
            const ZStorageManager::Row& r = snap.rows.at(i);
            if (list_->item(i)->text() != r.title) list_->item(i)->setText(r.title);
            list_->item(i)->setIcon(openMark(r.open));
        }
        return;
    }
    shownRoots_ = roots;
    const QSignalBlocker quiet(list_);
    list_->clear();
    for (const ZStorageManager::Row& r : snap.rows) {
        auto* item = new QListWidgetItem(openMark(r.open), r.title, list_);
        item->setToolTip(r.root);
    }
}

// Открытое хранилище — галочка ПЕРЕД именем, а не слово после (решение
// владельца 31.08): состояние строки, не часть имени, и не спорит с кнопками
// Open/Close внизу, как спорил суффикс «• open». Галочка — иконкой (lucide
// check: меньше и жирнее текстовой «✓» — уточнение владельца), а у прочих
// строк — прозрачная заглушка ТОГО ЖЕ размера: колонка иконок общая, и имена
// всех строк выравниваются по буквам.
QIcon StoreManagerDialog::openMark(bool open) const {
    // Коробка шире глифа: справа воздух, галочка не липнет к имени
    // (уточнение владельца).
    const QSize box = list_->iconSize();
    const qreal dpr = devicePixelRatioF();
    QPixmap wide(qRound(box.width() * dpr), qRound(box.height() * dpr));
    wide.setDevicePixelRatio(dpr);
    wide.fill(Qt::transparent);
    if (open) {
        const QPixmap mark = toolbarIcon(QStringLiteral("check"), box.height(),
                                         palette().color(QPalette::Text), dpr);
        QPainter paint(&wide);
        paint.drawPixmap(0, 0, mark);
    }
    return QIcon(wide);
}

void StoreManagerDialog::render() {
    const ZStorageManager::Snapshot snap = stores_.snapshot();
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
    // Папка не нашлась — путь красным (сценарий 3 владельца): дальше Browse.
    folder_->setStyleSheet(snap.folderMissing ? alarmStyle()
                                              : QString());
    put(password2_, snap.repeat);

    // Обычные поля тоже ведёт снимок: доступность и подсказка (каталог-облако
    // гасит логин и серверную папку и говорит об этом сам).
    const auto putField = [&put](QLineEdit* field, const ZStorageManager::Field& state) {
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
                                  const ZStorageManager::Field& state) {
        put(field, state.stub ? QString(8, QChar(0x2022)) : state.text);
        field->setPlaceholderText(state.placeholder);
        field->setEnabled(state.enabled);
        field->setStyleSheet(state.placeholderAlarm ? alarmStyle()
                                                    : QString());
        eye->setEnabled(state.eyeEnabled);
        if (!state.eyeEnabled) eye->setChecked(false);
    };
    putSecret(serverPassword_, serverEye_, snap.serverPassword);
    putSecret(password_, passwordEye_, snap.encryptionPassword);

    password2Label_->setVisible(snap.repeatVisible);
    password2_->setVisible(snap.repeatVisible);

    const auto show = [](QPushButton* button, const ZStorageManager::Button& state) {
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

    const auto line = [this](QLabel* label, const ZStorageManager::Line& state) {
        const qsizetype cut = state.text.indexOf(QLatin1Char('\n'));
        if (state.alarm || cut < 0) {
            label->setTextFormat(Qt::PlainText);
            label->setText(state.text);
        } else {
            // Хвост факта (дата правки и прочее второстепенное) — приглушённым
            // цветом из палитры (решение владельца 31.08): работает в обеих
            // темах, тревога остаётся цельно-красной и плоской.
            QColor dim = palette().color(QPalette::Text);
            dim.setAlphaF(0.55);
            QString tail = state.text.mid(cut + 1).toHtmlEscaped();
            tail.replace(QStringLiteral("\n"), QStringLiteral("<br/>"));
            label->setTextFormat(Qt::RichText);
            label->setText(QStringLiteral("%1<br/><span style=\"color:%2;\">%3</span>")
                               .arg(state.text.left(cut).toHtmlEscaped(),
                                    dim.name(QColor::HexArgb), tail));
        }
        label->setStyleSheet(state.alarm ? alarmStyle() : QString());
    };
    line(localLine_, snap.local);
    line(cloudLine_, snap.cloud);
    line(status_, snap.message);
    placeBrowseButton();
}

// --- намерение модели -------------------------------------------------------

int StoreManagerDialog::ask(const ZStorageManager::Question& question) {
    // Через одну дверь окон сообщений (ZApp::messageBox) — облик общий,
    // кнопки — по вопросу модели.
    QMessageBox* box = ZApp::instance().messageBox(this, question.text, ZApp::Notice::Question);
    if (!question.detail.isEmpty()) box->setInformativeText(question.detail);
    QList<QPushButton*> buttons;
    for (int i = 0; i < question.choices.size(); ++i) {
        const bool last = i + 1 == question.choices.size();
        buttons.append(box->addButton(question.choices.at(i),
                                      last ? QMessageBox::RejectRole
                                           : QMessageBox::DestructiveRole));
    }
    if (!buttons.isEmpty()) box->setDefaultButton(buttons.last());
    box->exec();
    int chosen = int(buttons.size()) - 1;   // закрыли крестиком — это отказ
    for (int i = 0; i < buttons.size(); ++i)
        if (box->clickedButton() == buttons.at(i)) chosen = i;
    delete box;
    return chosen;
}

void StoreManagerDialog::act(const ZStorageManager::Reaction& reaction) {
    if (reaction.question.kind != ZStorageManager::Question::Kind::None) {
        const int choice = ask(reaction.question);
        act(stores_.answered(reaction.question.kind, choice));
        return;
    }
    if (reaction.job.kind != ZStorageManager::Job::Kind::None) {
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

void StoreManagerDialog::runJob(const ZStorageManager::Job& job) {
    // СВЯЗКУ СПРАШИВАЕМ ЗДЕСЬ, В ГЛАВНОМ ПОТОКЕ, и только по явному жесту.
    // Признак «возьми из связки» снимается тут же: дальше едут настоящие
    // секреты, а кружочки показа наружу не выходят никогда.
    ZStorageManager::Job resolved = job;
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
            job.kind == ZStorageManager::Job::Kind::ChangePassword ||
            (job.kind == ZStorageManager::Job::Kind::Check &&
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
        [this] { outcome_ = stores_.runJob(runningJob_, *taken_); },
        [this] {
            // ДОБЫТОЕ — В НАСТОЯЩУЮ СВЯЗКУ, ВСЕ ТРИ ЗАПИСИ. Прежде ветка
            // сброса переносила ключ и пароль сервера, а пароль шифрования
            // забывала — и человек, только что введший его дважды, получал
            // красное «set encryption password» (жалоба владельца, 29.08.2026).
            QString why;
            if (taken_->capturedKey.hasKey() && !secrets_->storeKey(taken_->capturedKey, &why))
                stores_.setMessage(QStringLiteral("The keyring refused the key: %1").arg(why),
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
                runningJob_.kind != ZStorageManager::Job::Kind::None && outcome_.ok;
            act(stores_.jobFinished(runningJob_.kind, outcome_));
            if (wasOurCloud && stores_.isOpenRow()) result_.cloudChangedForCurrent = true;
            if (outcome_.ok && outcome_.downloadedNew) result_.downloadedNew = true;
            // Заведённое хранилище — то, ради чего жали кнопку: открываем его
            // и уходим. Скачивание при этом ведёт фоновый прогон.
            if (outcome_.ok && runningJob_.kind == ZStorageManager::Job::Kind::Create) {
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
    act(stores_.addFolder(dir));
}

void StoreManagerDialog::dropSelected() {
    act(stores_.answered(ZStorageManager::Question::Kind::Forget, 0));
}

void StoreManagerDialog::chooseReset(int road) {
    act(stores_.answered(ZStorageManager::Question::Kind::ResetCloud, road));
}

QAction* StoreManagerDialog::addEyeToggle(QLineEdit* field, ZStorageManager::FieldId which) {
    const int points = style()->pixelMetric(QStyle::PM_SmallIconSize, nullptr, this);
    const QColor color = palette().color(QPalette::Text);
    const qreal dpr = devicePixelRatioF();
    // Кружочки в поле — глаз ЗАКРЫТ (eye-closed); нажали — пароль виден, глаз
    // открыт (решение владельца 31.08). Иконка меняется РУКАМИ в toggled:
    // кнопка QLineEdit-действия состояний QIcon::On/Off не рисует — двухфазная
    // иконка стояла закрытой навсегда (живая проба владельца 31.08).
    const QIcon opened(toolbarIcon(QStringLiteral("eye"), points, color, dpr));
    const QIcon closed(toolbarIcon(QStringLiteral("eye-closed"), points, color, dpr));
    QAction* eye = field->addAction(closed, QLineEdit::TrailingPosition);
    eye->setCheckable(true);
    eye->setToolTip(QStringLiteral("Show the password"));
    connect(eye, &QAction::toggled, this,
            [this, field, which, eye, opened, closed](bool on) {
        eye->setIcon(on ? opened : closed);
        if (!on) {
            field->setEchoMode(QLineEdit::Password);
            return;
        }
        // ОТКАЗ НЕ ОТКРЫВАЕТ ГЛАЗ: в поле может стоять заглушка из литеральных
        // кружочков, и показать её как текст значило бы выдать плейсхолдер за
        // пароль. Галка снимается — тот же toggled вернёт echo назад, а
        // render() пересинхронизирует поле со свежим снимком.
        const auto refuse = [this, eye](const char* why) {
            qWarning("store window: eye kept shut — %s", why);
            eye->setChecked(false);
            render();
        };
        // НАБРАННОЕ ПРОСТО ПОКАЗЫВАЕМ. Подменять его хранимым нельзя: человек
        // видит не то, что набрал, а модель — не то, что видит человек.
        const ZStorageManager::Snapshot snap = stores_.snapshot();
        const ZStorageManager::Field& state =
            which == ZStorageManager::FieldId::ServerPassword ? snap.serverPassword
                                                              : snap.encryptionPassword;
        if (!state.stub) {
            // Пустое поле без заглушки показывать нечего; сюда попадаем только
            // при рассинхроне (связка изменилась за спиной окна) — глаз у
            // пустого поля обычно погашен снимком.
            if (state.text.isEmpty()) return refuse("nothing to show");
            field->setEchoMode(QLineEdit::Normal);
            return;
        }
        // ГЛАЗ ДОСТАЁТ НАСТОЯЩИЙ ПАРОЛЬ ИЗ СВЯЗКИ — ради этого связка и
        // хранит его третьей записью (решение владельца): подсмотреть и
        // скопировать свой пароль больше негде. Спрашиваем ЯВНЫМ жестом и в
        // главном потоке: на маке чтение секрета вправе поднять системный
        // вопрос, и на переключение строки его звать нельзя.
        const QString root = ZStorageManager::canonicalRoot(folder_->text().trimmed());
        if (ZStorage::inspect(root) != ZStorage::DirKind::Store)
            return refuse("the folder is not a storage");
        const QString id = ZStorage(root).identity().storeId();
        if (id.isEmpty()) return refuse("the storage has no id");
        const QString kept = which == ZStorageManager::FieldId::ServerPassword
                                 ? secrets_->serverPassword(id)
                                 : secrets_->encryptionPassword(id);
        if (kept.isEmpty()) return refuse("the keyring gave no password");
        // Показанное становится набранным: поле и модель обязаны говорить об
        // одном, иначе следующая работа поедет с заглушкой.
        stores_.edit(which, kept);
        field->setEchoMode(QLineEdit::Normal);
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
        const ZStorageManager::Snapshot snap = stores_.snapshot();
        const bool stub = watched == serverPassword_ ? snap.serverPassword.stub
                                                     : snap.encryptionPassword.stub;
        if (typing && stub) {
            stores_.edit(watched == serverPassword_
                            ? ZStorageManager::FieldId::ServerPassword
                            : ZStorageManager::FieldId::EncryptionPassword,
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
    // НАВЕРХ СТЕКА ОБЯЗАТЕЛЬНО: поля формы реparent'ятся в рамку ПОЗЖЕ
    // кнопки (addRow) и встают выше — распорка съедала клик, кнопка выглядела
    // мёртвой (живая жалоба владельца, 30.08.2026: «не могу нажать "…"»;
    // ловится настоящим кликом мыши в наборе, click() тут не судья).
    browseButton_->raise();
}

// --- потоки -----------------------------------------------------------------

void StoreManagerDialog::startWork(const QString& status, std::function<void()> job,
                                   std::function<void()> done) {
    setBusy(true);
    stores_.setMessage(status);
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
