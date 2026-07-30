#include "find_bar.h"

#include "settings.h"

#include <QHBoxLayout>
#include <QMenu>
#include <QKeyEvent>

namespace zametti {

FindBar::FindBar(QWidget* parent) : QWidget(parent) {
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(6, 4, 6, 4);
    layout->setSpacing(6);

    // История — слева от поля: по сути выпадающий список, только раскрывается
    // вверх (панель у нижней кромки окна, вниз списку некуда).
    historyButton_ = new QToolButton(this);
    historyButton_->setText(appearance().findHistoryGlyph);
    historyButton_->setToolTip(QStringLiteral("Прежние запросы"));
    connect(historyButton_, &QToolButton::clicked, this, &FindBar::showHistory);

    find_ = new QLineEdit(this);
    find_->setPlaceholderText(QStringLiteral("Найти"));
    find_->setClearButtonEnabled(true);
    status_ = new QLabel(this);
    status_->setMinimumWidth(70);

    auto* previous = new QToolButton(this);
    previous->setText(appearance().findPreviousGlyph);
    previous->setToolTip(QStringLiteral("Предыдущее (Shift+F3)"));
    auto* next = new QToolButton(this);
    next->setText(appearance().findNextGlyph);
    next->setToolTip(QStringLiteral("Следующее (F3)"));

    replaceLabel_ = new QLabel(QStringLiteral("на"), this);
    replace_ = new QLineEdit(this);
    replace_->setPlaceholderText(QStringLiteral("Заменить на"));
    replaceButton_ = new QToolButton(this);
    replaceButton_->setText(QStringLiteral("Заменить"));
    replaceAllButton_ = new QToolButton(this);
    replaceAllButton_->setText(QStringLiteral("Все"));

    auto* close = new QToolButton(this);
    close->setText(QStringLiteral("✕"));
    close->setToolTip(QStringLiteral("Закрыть (Esc)"));

    layout->addWidget(historyButton_);
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

    // Кегль чуть крупнее панельного: в поле поиска печатают, а не смотрят на
    // него, и мелкий шрифт здесь читается хуже. Прибавка — в конфиге.
    QFont panelFont(appearance().sidebarFontFamily.isEmpty()
                        ? appearance().fontFamily
                        : appearance().sidebarFontFamily);
    panelFont.setPointSizeF(appearance().sidebarFontPoint + appearance().findFontDelta);
    setFont(panelFont);
    hide();
}

void FindBar::open(Mode mode, const QString& preset) {
    mode_ = mode;
    const bool replacing = mode == Mode::Replace;
    replaceLabel_->setVisible(replacing);
    replace_->setVisible(replacing);
    replaceButton_->setVisible(replacing);
    replaceAllButton_->setVisible(replacing);
    find_->setPlaceholderText(mode == Mode::Global
                                  ? QStringLiteral("Найти во всех заметках")
                                  : QStringLiteral("Найти в заметке"));
    if (!preset.isEmpty()) find_->setText(preset);
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
        if (history_.size() >= appearance().findHistoryLimit) break;
    }
}

void FindBar::rememberQuery() {
    const QString text = find_->text().trimmed();
    // Тот же порог, что у поиска: однобуквенные запросы не исполняются, и
    // помнить их незачем.
    if (text.size() < 2) return;
    history_.removeAll(text);
    history_.prepend(text);   // свежий сверху
    while (history_.size() > appearance().findHistoryLimit) history_.removeLast();
}

void FindBar::showHistory() {
    QMenu menu(this);
    if (history_.isEmpty()) {
        menu.addAction(QStringLiteral("пока пусто"))->setEnabled(false);
    } else {
        for (const QString& item : history_) {
            const QString text = item;
            menu.addAction(text, this, [this, text] {
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
    // Стрелка вверх в поле — самый быстрый способ достать прошлый запрос, как
    // в оболочке командной строки.
    if (event->key() == Qt::Key_Up && !history_.isEmpty() && find_->hasFocus()) {
        const int at = history_.indexOf(find_->text().trimmed());
        const int next = at < 0 ? 0 : qMin(at + 1, int(history_.size()) - 1);
        find_->setText(history_.at(next));
        find_->selectAll();
        return;
    }
    QWidget::keyPressEvent(event);
}

}  // namespace zametti
