#include "toolbar.h"

#include "icons.h"
#include "settings.h"

#include <QEvent>
#include <QHBoxLayout>
#include <QIcon>
#include <QPainter>
#include <QToolButton>
#include <QWindow>

namespace zametti {
namespace {

using B = Toolbar::Button;

// Единственное место, где сказано, из чего состоит тулбар. Список владельца из
// zametti_todo.md, раздел «Тулбар с кнопочками», в его же порядке.
constexpr Toolbar::Spec kSpecs[] = {
    {B::NewNote, "file-plus-corner", "Новая заметка", "Ctrl+N", 0, false},
    {B::NewFolder, "folder-plus", "Новая папка", "", 0, false},
    {B::ImportNotes, "folder-input", "Импортировать .md", "", 0, false},
    {B::InsertImages, "image-down", "Вставить картинки", "", 0, false},

    {B::Export, "square-arrow-out-up-right", "Экспорт заметки", "", 1, false},
    {B::Cloud, "cloud-sync", "Синхронизация", "", 1, false},

    {B::Panels, "columns-3", "Скрыть боковые панели", "", 2, true},
    // Начальные значки и подсказки: направление у сортировок меняется на ходу
    // (setIcon/setTip), и здесь записано лишь то, с чего они начинают.
    {B::SortByName, "arrow-down-a-z", "По имени, А→Я", "", 2, true},
    {B::SortByDate, "clock-arrow-down", "По дате правки, новые сверху", "", 2, true},
    {B::SortByCreated, "calendar-arrow-down", "По дате создания, новые сверху", "", 2, true},

    // На месте трёх прежних кнопок навигации по истории (вернуть, к последней
    // версии, назад к посещённому). Из них вернулась одна — вход в историю;
    // ходят по ней дальше баннер и полоса времени, которые и так на виду, пока
    // режим идёт.
    // ПЕРЕКЛЮЧАТЕЛЬ, а не кнопка-действие: пока горит — идёт режим истории,
    // нажали снова — вернулись к текущей версии (решение владельца). Так у
    // режима есть видимый признак, а не только баннер над текстом.
    {B::History, "rotate-ccw-clock", "История заметки", "", 3, true},
    {B::Search, "search", "Найти в заметке", "Ctrl+F", 3, false},
    {B::SearchInStore, "database-search", "Найти по всем заметкам", "Ctrl+Shift+F", 3, false},

    {B::Settings, "settings", "Настройки", "", 4, false},
    {B::Help, "circle-question-mark", "Справка", "", 4, false},
};

constexpr Toolbar::SortSpec kSortSpecs[] = {
    {SortKey::Name, B::SortByName, "arrow-down-a-z", "arrow-up-a-z"},
    {SortKey::Modified, B::SortByDate, "clock-arrow-down", "clock-arrow-up"},
    {SortKey::Created, B::SortByCreated, "calendar-arrow-down", "calendar-arrow-up"},
};

QString tipFor(const Toolbar::Spec& spec, const QString& promise) {
    QString tip = QString::fromUtf8(spec.tip);
    const QString shortcut = QString::fromLatin1(spec.shortcut);
    if (!shortcut.isEmpty()) tip += QStringLiteral(" (") + shortcut + QLatin1Char(')');
    // Погашенная кнопка без объяснения читается как поломка, а не как обещание.
    if (!promise.isEmpty()) tip += QStringLiteral("\n") + promise;
    return tip;
}

}  // namespace

std::span<const Toolbar::Spec> Toolbar::specs() {
    return std::span<const Spec>(kSpecs, std::size(kSpecs));
}

std::span<const Toolbar::SortSpec> Toolbar::sortSpecs() {
    return std::span<const SortSpec>(kSortSpecs, std::size(kSortSpecs));
}

void Toolbar::showSort(SortOrder order, bool fromMark) {
    for (const SortSpec& item : kSortSpecs) {
        const bool active = item.key == order.key;
        // У неактивной кнопки нарисовано её УМОЛЧАНИЕ: значок — обещание того,
        // что будет по нажатию, а не память о том, как было когда-то.
        const bool ascending = active ? order.ascending : defaultAscending(item.key);
        const bool flipped = ascending != defaultAscending(item.key);
        setChecked(item.button, active);
        setAccent(item.button, active && fromMark);
        setIcon(item.button, QString::fromLatin1(flipped ? item.iconFlipped : item.iconDefault));
        QString tip = sortOrderTitle(SortOrder{item.key, ascending});
        tip += active ? (fromMark ? QStringLiteral("\nпорядок задан меткой папки")
                                  : QStringLiteral("\nобщий порядок"))
                      : QStringLiteral("\nнажать — включить, ещё раз — перевернуть");
        setTip(item.button, tip);
    }
}

QString Toolbar::iconName(Button id) const {
    const QString set = icons_.value(int(id));
    if (!set.isEmpty()) return set;
    for (const Spec& spec : kSpecs)
        if (spec.id == id) return QString::fromLatin1(spec.icon);
    return QString();
}

Toolbar::Toolbar(QWidget* parent) : QWidget(parent) {
    build();
    restyle();
}

void Toolbar::build() {
    auto* layout = new QHBoxLayout(this);
    const Appearance& a = appearance();
    layout->setContentsMargins(a.toolbarGroupSpacing / 2, 4, a.toolbarGroupSpacing / 2, 4);
    layout->setSpacing(0);

    int previousGroup = -1;
    for (const Spec& spec : kSpecs) {
        if (previousGroup >= 0 && spec.group != previousGroup)
            layout->addSpacing(a.toolbarGroupSpacing);
        previousGroup = spec.group;

        auto* button = new QToolButton(this);
        button->setAutoRaise(true);
        button->setCheckable(spec.checkable);
        button->setFocusPolicy(Qt::NoFocus);   // тулбар не крадёт каретку
        button->setToolTip(tipFor(spec, QString()));
        buttons_.insert(int(spec.id), button);
        layout->addWidget(button);

        const Button id = spec.id;
        connect(button, &QToolButton::clicked, this, [this, id] { emit pressed(id); });
    }
    // Пустота справа. Прежде туда распоркой уезжал поиск — «в правом верхнем
    // углу», как просил бриф; владелец решил иначе: поиск встал вместе с
    // остальными, а полоса кончается там же, где кончаются кнопки.
    layout->addStretch(1);
}

void Toolbar::restyle() {
    const Appearance& a = appearance();
    // Плотность берётся у окна, а не у экрана: окно переезжает между экранами
    // разной плотности, и на этапе 8 именно плавающий devicePixelRatio уже
    // портил картинки. Пока окна нет (виджет ещё не показан), берём единицу и
    // перерисуемся по DevicePixelRatioChange.
    const qreal dpr = window() && window()->windowHandle()
                          ? window()->windowHandle()->devicePixelRatio()
                          : devicePixelRatioF();

    const int side = a.toolbarIconSize + 2 * a.toolbarButtonPadding;
    for (const Spec& spec : kSpecs) {
        QToolButton* button = buttons_.value(int(spec.id));
        if (!button) continue;
        // Значок берётся из памяти виджета, а не из списка: направление
        // сортировки успело смениться, и перерисовка по kSpecs откатила бы
        // стрелку назад — на смене плотности экрана или после правки конфига.
        const QString name = icons_.value(int(spec.id), QString::fromLatin1(spec.icon));
        const QColor onColour = marked_.value(int(spec.id), false) ? a.toolbarIconMarkColor
                                                                   : a.toolbarIconOnColor;

        QIcon icon;
        icon.addPixmap(toolbarIcon(name, a.toolbarIconSize, a.toolbarIconColor, dpr),
                       QIcon::Normal, QIcon::Off);
        icon.addPixmap(toolbarIcon(name, a.toolbarIconSize, a.toolbarIconHoverColor, dpr),
                       QIcon::Active, QIcon::Off);
        icon.addPixmap(toolbarIcon(name, a.toolbarIconSize, a.toolbarIconDisabledColor, dpr),
                       QIcon::Disabled, QIcon::Off);
        // Нажатое состояние переключателя — цветом. Рамка на иконке в двадцать
        // точек спорит с самим рисунком, а цвет виден сразу и издалека.
        icon.addPixmap(toolbarIcon(name, a.toolbarIconSize, onColour, dpr),
                       QIcon::Normal, QIcon::On);
        icon.addPixmap(toolbarIcon(name, a.toolbarIconSize, onColour, dpr),
                       QIcon::Active, QIcon::On);
        icon.addPixmap(toolbarIcon(name, a.toolbarIconSize, a.toolbarIconDisabledColor, dpr),
                       QIcon::Disabled, QIcon::On);
        button->setIcon(icon);
        button->setIconSize(QSize(a.toolbarIconSize, a.toolbarIconSize));
        button->setFixedSize(side, side);
    }

    setAutoFillBackground(true);
    QPalette palette = this->palette();
    palette.setColor(QPalette::Window, a.toolbarBackground);
    setPalette(palette);

    setStyleSheet(QStringLiteral("QToolButton { border: none; border-radius: 4px; "
                                 "background: transparent; }"
                                 "QToolButton:hover:enabled { background: rgba(%1,%2,%3,%4); }"
                                 "QToolButton:checked:enabled { background: rgba(%1,%2,%3,%4); }")
                      .arg(a.toolbarHoverBackground.red())
                      .arg(a.toolbarHoverBackground.green())
                      .arg(a.toolbarHoverBackground.blue())
                      .arg(a.toolbarHoverBackground.alpha()));
}

void Toolbar::paintEvent(QPaintEvent* e) {
    QWidget::paintEvent(e);
    QPainter painter(this);
    painter.setPen(appearance().toolbarSeparatorColor);
    // Ровно одна ЛОГИЧЕСКАЯ точка: на плотном экране Qt сама положит её в
    // нужное число пикселей, а нарисованная в пикселях черта была бы то
    // толстой, то невидимой.
    painter.drawLine(0, height() - 1, width(), height() - 1);
}

bool Toolbar::event(QEvent* e) {
    if (e->type() == QEvent::DevicePixelRatioChange || e->type() == QEvent::Show)
        restyle();
    return QWidget::event(e);
}

void Toolbar::refreshAppearance() {
    clearIconCache();
    restyle();
}

QToolButton* Toolbar::buttonFor(Button id) const { return buttons_.value(int(id)); }

void Toolbar::setEnabled(Button id, bool on) {
    if (QToolButton* button = buttons_.value(int(id))) button->setEnabled(on);
}

bool Toolbar::isEnabled(Button id) const {
    QToolButton* button = buttons_.value(int(id));
    return button && button->isEnabled();
}

void Toolbar::setChecked(Button id, bool on) {
    if (QToolButton* button = buttons_.value(int(id))) button->setChecked(on);
}

bool Toolbar::isChecked(Button id) const {
    QToolButton* button = buttons_.value(int(id));
    return button && button->isChecked();
}

void Toolbar::setIcon(Button id, const QString& iconName) {
    if (icons_.value(int(id)) == iconName) return;
    icons_.insert(int(id), iconName);
    restyle();
}

void Toolbar::setTip(Button id, const QString& tip) {
    if (QToolButton* button = buttons_.value(int(id))) button->setToolTip(tip);
}

void Toolbar::setAccent(Button id, bool ownMark) {
    if (marked_.value(int(id), false) == ownMark) return;
    marked_.insert(int(id), ownMark);
    restyle();
}

void Toolbar::setPromise(Button id, const QString& why) {
    promises_.insert(int(id), why);
    setEnabled(id, why.isEmpty());
    QToolButton* button = buttons_.value(int(id));
    if (!button) return;
    for (const Spec& spec : kSpecs)
        if (spec.id == id) button->setToolTip(tipFor(spec, why));
}

}  // namespace zametti
