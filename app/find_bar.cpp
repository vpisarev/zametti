#include "find_bar.h"

#include "settings.h"

#include <QHBoxLayout>
#include <QKeyEvent>

namespace zametti {

FindBar::FindBar(QWidget* parent) : QWidget(parent) {
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(6, 4, 6, 4);
    layout->setSpacing(6);

    find_ = new QLineEdit(this);
    find_->setPlaceholderText(QStringLiteral("Найти"));
    find_->setClearButtonEnabled(true);
    status_ = new QLabel(this);
    status_->setMinimumWidth(70);

    auto* previous = new QToolButton(this);
    previous->setText(QStringLiteral("▲"));
    previous->setToolTip(QStringLiteral("Предыдущее (Shift+F3)"));
    auto* next = new QToolButton(this);
    next->setText(QStringLiteral("▼"));
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
    connect(previous, &QToolButton::clicked, this, &FindBar::findPrevious);
    connect(next, &QToolButton::clicked, this, &FindBar::findNext);
    connect(replaceButton_, &QToolButton::clicked, this, &FindBar::replaceOne);
    connect(replaceAllButton_, &QToolButton::clicked, this, &FindBar::replaceAll);
    connect(close, &QToolButton::clicked, this, [this] {
        hide();
        emit closed();
    });

    QFont panelFont(appearance().sidebarFontFamily.isEmpty()
                        ? appearance().fontFamily
                        : appearance().sidebarFontFamily);
    panelFont.setPointSizeF(appearance().sidebarFontPoint);
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

void FindBar::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape) {
        hide();
        emit closed();
        return;
    }
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        if ((event->modifiers() & Qt::ShiftModifier) != 0) emit findPrevious();
        else emit findNext();
        return;
    }
    QWidget::keyPressEvent(event);
}

}  // namespace zametti
