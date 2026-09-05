#include "find_bar.h"

#include "settings.h"

#include "icons.h"
#include "zapp.h"

#include <QApplication>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QIcon>
#include <QKeyEvent>
#include <QMenu>
#include <QPalette>
#include <QWindow>

namespace zametti {

FindBar::FindBar(QWidget* parent) : QWidget(parent) {
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(6, 4, 6, 4);
    layout->setSpacing(6);

    // История — слева от поля: по сути выпадающий список, только раскрывается
    // вверх (панель у нижней кромки окна, вниз списку некуда).
    historyButton_ = new QToolButton(this);
    historyButton_->setAutoRaise(true);
    historyButton_->setFocusPolicy(Qt::NoFocus);   // фокус остаётся в поле
    historyButton_->setToolTip(QStringLiteral("Previous queries"));
    connect(historyButton_, &QToolButton::clicked, this, &FindBar::showHistory);

    // Тумблер выражений — справа от истории. Западающий: смысл у него не
    // «сделать сейчас», а «искать вот так, пока не отожму».
    regexButton_ = new QToolButton(this);
    regexButton_->setAutoRaise(true);
    regexButton_->setCheckable(true);
    regexButton_->setFocusPolicy(Qt::NoFocus);
    regexButton_->setToolTip(QStringLiteral("Regular expression"));
    connect(regexButton_, &QToolButton::toggled, this, [this](bool on) {
        emit regexToggled(on);
    });

    find_ = new QLineEdit(this);
    find_->setPlaceholderText(QStringLiteral("Find"));
    find_->setClearButtonEnabled(true);
    status_ = new QLabel(this);
    status_->setMinimumWidth(70);

    previousButton_ = new QToolButton(this);
    previousButton_->setAutoRaise(true);
    previousButton_->setFocusPolicy(Qt::NoFocus);
    previousButton_->setToolTip(QStringLiteral("Previous (Shift+F3)"));
    nextButton_ = new QToolButton(this);
    nextButton_->setAutoRaise(true);
    nextButton_->setFocusPolicy(Qt::NoFocus);
    nextButton_->setToolTip(QStringLiteral("Next (F3)"));
    QToolButton* previous = previousButton_;
    QToolButton* next = nextButton_;

    replaceLabel_ = new QLabel(QStringLiteral("with"), this);
    replace_ = new QLineEdit(this);
    replace_->setPlaceholderText(QStringLiteral("Replace with"));
    // Отмена и повтор из обоих полей — тексту (см. сигнал textKeyPressed).
    find_->installEventFilter(this);
    replace_->installEventFilter(this);
    replaceButton_ = new QToolButton(this);
    replaceButton_->setText(QStringLiteral("Replace"));
    replaceAllButton_ = new QToolButton(this);
    replaceAllButton_->setText(QStringLiteral("All"));

    auto* close = new QToolButton(this);
    close->setText(QStringLiteral("✕"));
    close->setToolTip(QStringLiteral("Close (Esc)"));

    layout->addWidget(historyButton_);
    layout->addWidget(regexButton_);
    layout->addWidget(find_, 2);
    layout->addWidget(status_);
    layout->addWidget(previous);
    layout->addWidget(next);
    layout->addWidget(replaceLabel_);
    layout->addWidget(replace_, 2);
    layout->addWidget(replaceButton_);
    layout->addWidget(replaceAllButton_);
    layout->addWidget(close);

    connect(find_, &QLineEdit::textChanged, this, &FindBar::queryChanged);
    // Набрали что-то сами — мы больше не в истории, а правим свой запрос.
    // Именно textEdited: textChanged сработал бы и на нашу же подстановку.
    connect(find_, &QLineEdit::textEdited, this, [this] { historyAt_ = -1; });
    // Запрос попадает в историю, когда им воспользовались: перешли к
    // совпадению или заменили. Набор в поле — ещё не запрос.
    connect(previous, &QToolButton::clicked, this, [this] {
        rememberQuery();
        emit findPrevious();
    });
    connect(next, &QToolButton::clicked, this, [this] {
        rememberQuery();
        emit findNext();
    });
    connect(replaceButton_, &QToolButton::clicked, this, [this] {
        rememberQuery();
        emit replaceOne();
    });
    connect(replaceAllButton_, &QToolButton::clicked, this, [this] {
        rememberQuery();
        emit replaceAll();
    });
    connect(close, &QToolButton::clicked, this, [this] {
        rememberQuery();
        hide();
        emit closed();
    });

    refreshAppearance();
    hide();
}

// ОБЛИК ПЕРЕЧИТЫВАЕТСЯ, А НЕ СТАВИТСЯ ОДИН РАЗ. Прежде шрифт и значки панели
// поиска задавались в конструкторе, и правка конфига до них не доходила вовсе
// — а с масштабом оболочки (Ctrl+Alt+±) панель осталась бы единственным
// местом окна, которое не поехало.
void FindBar::refreshAppearance() {
    // Кегль чуть крупнее панельного: в поле поиска печатают, а не смотрят на
    // него, и мелкий шрифт здесь читается хуже (ui_style.h).
    const QFont panelFont = ZApp::instance().uiStyle().findFont();
    setFont(panelFont);
    restyleButtons();
    // ПОЛЯМ — НИЖНЯЯ ГРАНИЦА. В узком окне растяжки делили остаток так, что от
    // запроса оставалось три знака: на снимке 520 точек видно «v+)» вместо
    // «(\w+)@(\w+)». Граница — в знаках, а не в точках: кегль панели свой и
    // правится настройкой.
    const int minField = QFontMetrics(panelFont).horizontalAdvance(QLatin1Char('0')) * 10;
    find_->setMinimumWidth(minField);
    replace_->setMinimumWidth(minField);
}

// Значки кнопок — теми же средствами, что у тулбара: цвет из настроек,
// плотность у окна. Нажатое состояние тумблера показывается цветом, а не
// рамкой, — ровно как в тулбаре (рамка на значке в двадцать точек спорит с
// самим рисунком).
void FindBar::restyleButtons() {
    const ZSettings& a = settings();
    const qreal dpr = window() && window()->windowHandle()
                          ? window()->windowHandle()->devicePixelRatio()
                          : devicePixelRatioF();
    const int size = ZApp::instance().uiStyle().iconSize();
    const auto make = [&](const QString& name) {
        QIcon icon;
        icon.addPixmap(toolbarIcon(name, size, a.ui().toolbarIconColor(), dpr), QIcon::Normal,
                       QIcon::Off);
        icon.addPixmap(toolbarIcon(name, size, a.ui().toolbarIconHoverColor(), dpr), QIcon::Active,
                       QIcon::Off);
        icon.addPixmap(toolbarIcon(name, size, a.ui().toolbarIconOnColor(), dpr), QIcon::Normal,
                       QIcon::On);
        icon.addPixmap(toolbarIcon(name, size, a.ui().toolbarIconOnColor(), dpr), QIcon::Active,
                       QIcon::On);
        return icon;
    };
    historyButton_->setIcon(make(QStringLiteral("list-clock")));
    historyButton_->setIconSize(QSize(size, size));
    regexButton_->setIcon(make(QStringLiteral("regex")));
    regexButton_->setIconSize(QSize(size, size));
    // Обход найденного — вверх и вниз, а не влево и вправо: ходим по тексту, а
    // текст идёт сверху вниз (правило прежнее, глифы сменились значками).
    previousButton_->setIcon(make(QStringLiteral("arrow-up")));
    previousButton_->setIconSize(QSize(size, size));
    nextButton_->setIcon(make(QStringLiteral("arrow-down")));
    nextButton_->setIconSize(QSize(size, size));
}

bool FindBar::regexOn() const { return regexButton_->isChecked(); }

void FindBar::setRegexOn(bool on) { regexButton_->setChecked(on); }

void FindBar::setQueryUsable(bool usable) {
    if (queryUsable_ == usable) return;
    queryUsable_ = usable;
    // КРАСНЕЮТ БУКВЫ, А НЕ ФОН (решение владельца): фон поля остаётся своим,
    // человек продолжает править выражение, и нигде не написано, что это
    // ошибка, — он и так видит, что пишет.
    QPalette palette = find_->palette();
    palette.setColor(QPalette::Text, usable ? QApplication::palette().color(QPalette::Text)
                                            : settings().ui().findBadPatternColor());
    find_->setPalette(palette);
}

void FindBar::open(Mode mode, const QString& preset) {
    mode_ = mode;
    const bool replacing = mode == Mode::Replace;
    replaceLabel_->setVisible(replacing);
    replace_->setVisible(replacing);
    replaceButton_->setVisible(replacing);
    replaceAllButton_->setVisible(replacing);
    find_->setPlaceholderText(mode == Mode::Global ? QStringLiteral("Find in all notes")
                              : mode == Mode::History
                                  ? QStringLiteral("Find in snapshot and note history")
                                  : QStringLiteral("Find in note"));
    if (!preset.isEmpty()) find_->setText(preset);
    historyAt_ = -1;   // каждый заход в панель начинается со своего запроса
    show();
    find_->setFocus();
    find_->selectAll();
    // Запрос мог остаться с прошлого раза — пусть подсветка и список появятся
    // сразу, не дожидаясь, пока в поле что-нибудь наберут.
    emit queryChanged(find_->text());
}

void FindBar::setStatus(const QString& text) { status_->setText(text); }

void FindBar::setHistory(const QStringList& items) {
    history_.clear();
    for (const QString& item : items) {
        const QString trimmed = item.trimmed();
        if (trimmed.isEmpty() || history_.contains(trimmed)) continue;
        history_.append(trimmed);
        if (history_.size() >= settings().ui().findHistoryLimit()) break;
    }
}

void FindBar::rememberQuery() {
    const QString text = find_->text().trimmed();
    // Тот же порог, что у поиска: однобуквенные запросы не исполняются, и
    // помнить их незачем.
    if (text.size() < 2) return;
    history_.removeAll(text);
    history_.prepend(text);   // свежий сверху
    while (history_.size() > settings().ui().findHistoryLimit()) history_.removeLast();
}

void FindBar::stepHistory(int direction) {
    if (history_.isEmpty()) return;
    const int last = int(history_.size()) - 1;
    if (direction < 0) {
        if (historyAt_ < 0) {
            typed_ = find_->text();   // вернём, когда спустимся обратно
            historyAt_ = 0;
        } else if (historyAt_ < last) {
            ++historyAt_;
        }
        find_->setText(history_.at(historyAt_));
    } else {
        if (historyAt_ < 0) return;   // ниже свежего запроса ничего нет
        --historyAt_;
        find_->setText(historyAt_ < 0 ? typed_ : history_.at(historyAt_));
    }
    find_->selectAll();
}

void FindBar::showHistory() {
    QMenu menu(this);
    if (history_.isEmpty()) {
        menu.addAction(QStringLiteral("nothing yet"))->setEnabled(false);
    } else {
        for (const QString& item : history_) {
            const QString text = item;
            const int at = int(history_.indexOf(text));
            menu.addAction(text, this, [this, text, at] {
                historyAt_ = at;
                find_->setText(text);
                find_->setFocus();
                find_->selectAll();
            });
        }
    }
    // Раскрываем ВВЕРХ: панель стоит у нижней кромки окна, и список, выпавший
    // вниз, ушёл бы за экран. Qt переворачивает меню сам, только когда места
    // не хватает физически; здесь место есть — оно за пределами окна.
    const QSize size = menu.sizeHint();
    const QPoint at = historyButton_->mapToGlobal(QPoint(0, 0));
    menu.exec(QPoint(at.x(), at.y() - size.height()));
}

bool FindBar::eventFilter(QObject* watched, QEvent* event) {
    if ((watched == find_ || watched == replace_) && event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->matches(QKeySequence::Undo) || key->matches(QKeySequence::Redo)) {
            emit textKeyPressed(key);
            return true;   // полю нажатие не достаётся: своей отмены у него нет
        }
    }
    return QWidget::eventFilter(watched, event);
}

void FindBar::setStepKeys(const QList<QKeySequence>& next,
                          const QList<QKeySequence>& previous) {
    const auto hint = [](const QString& what, const QList<QKeySequence>& keys) {
        QStringList names;
        for (const QKeySequence& k : keys) names << k.toString(QKeySequence::NativeText);
        return names.isEmpty() ? what : what + QStringLiteral(" (") + names.join(QStringLiteral(", ")) +
                                            QLatin1Char(')');
    };
    nextButton_->setToolTip(hint(QStringLiteral("Next"), next));
    previousButton_->setToolTip(hint(QStringLiteral("Previous"), previous));
}

void FindBar::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape) {
        rememberQuery();
        hide();
        emit closed();
        return;
    }
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        rememberQuery();
        if ((event->modifiers() & Qt::ShiftModifier) != 0) emit findPrevious();
        else emit findNext();
        return;
    }
    // Стрелки в поле листают прежние запросы, как в оболочке командной строки:
    // вверх — к старым, вниз — обратно к новым и дальше к тому, что набирали
    // сами. Место в списке держим счётчиком, а не поиском текущего текста по
    // истории: иначе из повторяющегося запроса шаг вниз возвращал бы в ту же
    // строку, и казалось бы, что ходить можно только вверх.
    const bool inField = find_->hasFocus();
    if (inField && (event->key() == Qt::Key_Up || event->key() == Qt::Key_Down)) {
        stepHistory(event->key() == Qt::Key_Up ? -1 : 1);
        return;
    }
    QWidget::keyPressEvent(event);
}

}  // namespace zametti
