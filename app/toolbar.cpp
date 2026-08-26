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
    // Своя группа, и она одна: открыть или завести хранилище — не операция над
    // заметкой, и стоять вплотную к «новой заметке» ей незачем.
    {B::OpenStore, "database", "Open storage", "", 0, false},

    {B::NewNote, "file-plus-corner", "New note", "Ctrl+N", 1, false},
    {B::NewFolder, "folder-plus", "New folder", "", 1, false},
    {B::ImportNotes, "folder-input", "Import .md", "", 1, false},
    {B::InsertImages, "image-down", "Insert images", "", 1, false},

    {B::Export, "square-arrow-out-up-right", "Export note", "", 2, false},
    {B::Cloud, "cloud-sync", "Sync", "", 2, false},

    {B::Panels, "columns-3", "Hide side panels", "", 3, true},
    // Начальные значки и подсказки: направление у сортировок меняется на ходу
    // (setIcon/setTip), и здесь записано лишь то, с чего они начинают.
    {B::SortByName, "arrow-down-a-z", "By name, A→Z", "", 3, true},
    {B::SortByDate, "clock-arrow-down", "By modified date, newest first", "", 3, true},
    {B::SortByCreated, "calendar-arrow-down", "By created date, newest first", "", 3, true},

    // На месте трёх прежних кнопок навигации по истории (вернуть, к последней
    // версии, назад к посещённому). Из них вернулась одна — вход в историю;
    // ходят по ней дальше баннер и полоса времени, которые и так на виду, пока
    // режим идёт.
    // ПЕРЕКЛЮЧАТЕЛЬ, а не кнопка-действие: пока горит — идёт режим истории,
    // нажали снова — вернулись к текущей версии (решение владельца). Так у
    // режима есть видимый признак, а не только баннер над текстом.
    {B::History, "rotate-ccw-clock", "Note history", "", 4, true},
    // Сочетание у этой кнопки НАСТРАИВАЕМОЕ (editor.markdownModeKey) и по
    // умолчанию пустое: в тултип оно подставляется из настроек (tipFor), а не
    // литералом — иначе тултип врал бы при любой правке конфига.
    {B::MarkdownEdit, "square-m", "Edit source", "", 4, true},
    {B::Search, "search", "Find in note", "Ctrl+F", 4, false},
    {B::SearchInStore, "database-search", "Find in all notes", "Ctrl+Shift+F", 4, false},

    // ПЕРЕКЛЮЧАТЕЛЬ, как история и правка исходника: горит — на месте
    // редактора правится config.json (refactor3).
    {B::Settings, "settings", "Settings", "", 5, true},
    {B::Help, "circle-question-mark", "Help", "", 5, false},
};

constexpr Toolbar::SortSpec kSortSpecs[] = {
    {SortKey::Name, B::SortByName, "arrow-down-a-z", "arrow-up-a-z"},
    {SortKey::Modified, B::SortByDate, "clock-arrow-down", "clock-arrow-up"},
    {SortKey::Created, B::SortByCreated, "calendar-arrow-down", "calendar-arrow-up"},
};

// Сочетание для тултипа: у настраиваемых кнопок — из настроек (список через
// точку с запятой человеку читается через запятую), у прочих — из спецификации.
QString shortcutFor(const Toolbar::Spec& spec) {
    if (spec.id == B::MarkdownEdit)
        return QString(settings().editor().markdownModeKey())
            .replace(QStringLiteral("; "), QStringLiteral(", "));
    return QString::fromLatin1(spec.shortcut);
}

// ПОДПИСЬ КНОПКИ ЦЕЛИКОМ: живая (или из таблицы, если своей нет), шорткат и,
// если кнопка погашена, причина. Собирается в одном месте — иначе setTip и
// setPromise писали бы в тултип каждый своё и затирали друг друга.
QString tipFor(const Toolbar::Spec& spec, const QString& own, const QString& promise) {
    QString tip = own.isEmpty() ? QString::fromUtf8(spec.tip) : own;
    const QString shortcut = shortcutFor(spec);
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
        tip += active ? (fromMark ? QStringLiteral("\norder set by the folder mark")
                                  : QStringLiteral("\ncommon order"))
                      : QStringLiteral("\nclick to enable, click again to flip");
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
    const ZSettings& a = settings();
    layout->setContentsMargins(a.ui().toolbarGroupSpacing() / 2, 4, a.ui().toolbarGroupSpacing() / 2, 4);
    layout->setSpacing(0);

    int previousGroup = -1;
    for (const Spec& spec : kSpecs) {
        if (previousGroup >= 0 && spec.group != previousGroup)
            layout->addSpacing(a.ui().toolbarGroupSpacing());
        previousGroup = spec.group;

        auto* button = new QToolButton(this);
        button->setAutoRaise(true);
        button->setCheckable(spec.checkable);
        button->setFocusPolicy(Qt::NoFocus);   // тулбар не крадёт каретку
        button->setToolTip(tipFor(spec, QString(), QString()));
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
    const ZSettings& a = settings();
    // Плотность берётся у окна, а не у экрана: окно переезжает между экранами
    // разной плотности, и на этапе 8 именно плавающий devicePixelRatio уже
    // портил картинки. Пока окна нет (виджет ещё не показан), берём единицу и
    // перерисуемся по DevicePixelRatioChange.
    const qreal dpr = window() && window()->windowHandle()
                          ? window()->windowHandle()->devicePixelRatio()
                          : devicePixelRatioF();

    const int side = a.ui().toolbarIconSize() + 2 * a.ui().toolbarButtonPadding();
    for (const Spec& spec : kSpecs) {
        QToolButton* button = buttons_.value(int(spec.id));
        if (!button) continue;
        // Значок берётся из памяти виджета, а не из списка: направление
        // сортировки успело смениться, и перерисовка по kSpecs откатила бы
        // стрелку назад — на смене плотности экрана или после правки конфига.
        const QString name = icons_.value(int(spec.id), QString::fromLatin1(spec.icon));
        const QColor onColour = marked_.value(int(spec.id), false) ? a.ui().toolbarIconMarkColor()
                                                                   : a.ui().toolbarIconOnColor();

        QIcon icon;
        icon.addPixmap(toolbarIcon(name, a.ui().toolbarIconSize(), a.ui().toolbarIconColor(), dpr),
                       QIcon::Normal, QIcon::Off);
        icon.addPixmap(toolbarIcon(name, a.ui().toolbarIconSize(), a.ui().toolbarIconHoverColor(), dpr),
                       QIcon::Active, QIcon::Off);
        icon.addPixmap(toolbarIcon(name, a.ui().toolbarIconSize(), a.ui().toolbarIconDisabledColor(), dpr),
                       QIcon::Disabled, QIcon::Off);
        // Нажатое состояние переключателя — цветом. Рамка на иконке в двадцать
        // точек спорит с самим рисунком, а цвет виден сразу и издалека.
        icon.addPixmap(toolbarIcon(name, a.ui().toolbarIconSize(), onColour, dpr),
                       QIcon::Normal, QIcon::On);
        icon.addPixmap(toolbarIcon(name, a.ui().toolbarIconSize(), onColour, dpr),
                       QIcon::Active, QIcon::On);
        icon.addPixmap(toolbarIcon(name, a.ui().toolbarIconSize(), a.ui().toolbarIconDisabledColor(), dpr),
                       QIcon::Disabled, QIcon::On);
        button->setIcon(icon);
        button->setIconSize(QSize(a.ui().toolbarIconSize(), a.ui().toolbarIconSize()));
        button->setFixedSize(side, side);
    }

    setAutoFillBackground(true);
    QPalette palette = this->palette();
    palette.setColor(QPalette::Window, a.ui().toolbarBackground());
    setPalette(palette);

    setStyleSheet(QStringLiteral("QToolButton { border: none; border-radius: 4px; "
                                 "background: transparent; }"
                                 "QToolButton:hover:enabled { background: rgba(%1,%2,%3,%4); }"
                                 "QToolButton:checked:enabled { background: rgba(%1,%2,%3,%4); }")
                      .arg(a.ui().toolbarHoverBackground().red())
                      .arg(a.ui().toolbarHoverBackground().green())
                      .arg(a.ui().toolbarHoverBackground().blue())
                      .arg(a.ui().toolbarHoverBackground().alpha()));
}

void Toolbar::paintEvent(QPaintEvent* e) {
    QWidget::paintEvent(e);
    QPainter painter(this);
    painter.setPen(settings().ui().toolbarSeparatorColor());
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
    tips_.insert(int(id), tip);
    applyTip(id);
}

QString Toolbar::promiseFor(Button id) const { return promises_.value(int(id)); }

// Написать в тултип то, что о кнопке известно сейчас. Одно место: и живая
// подпись, и причина погашения приходят сюда, а не пишут в тултип порознь.
void Toolbar::applyTip(Button id) {
    QToolButton* button = buttons_.value(int(id));
    if (button == nullptr) return;
    for (const Spec& spec : kSpecs)
        if (spec.id == id)
            button->setToolTip(tipFor(spec, tips_.value(int(id)), promises_.value(int(id))));
}

void Toolbar::setAccent(Button id, bool ownMark) {
    if (marked_.value(int(id), false) == ownMark) return;
    marked_.insert(int(id), ownMark);
    restyle();
}

void Toolbar::setPromise(Button id, const QString& why) {
    promises_.insert(int(id), why);
    setEnabled(id, why.isEmpty());
    applyTip(id);
}

}  // namespace zametti
