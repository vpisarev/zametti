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
    historyButton_->setText(settings().look.findHistoryGlyph);
    historyButton_->setToolTip(QStringLiteral("Прежние запросы"));
    connect(historyButton_, &QToolButton::clicked, this, &FindBar::showHistory);

    find_ = new QLineEdit(this);
    find_->setPlaceholderText(QStringLiteral("Найти"));
    find_->setClearButtonEnabled(true);
    status_ = new QLabel(this);
    status_->setMinimumWidth(70);

    auto* previous = new QToolButton(this);
    previous->setText(settings().look.findPreviousGlyph);
    previous->setToolTip(QStringLiteral("Предыдущее (Shift+F3)"));
    auto* next = new QToolButton(this);
    next->setText(settings().look.findNextGlyph);
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

    // Кегль чуть крупнее панельного: в поле поиска печатают, а не смотрят на
    // него, и мелкий шрифт здесь читается хуже. Прибавка — в конфиге.
    QFont panelFont(settings().look.sidebarFontFamily.isEmpty()
                        ? settings().look.fontFamily
                        : settings().look.sidebarFontFamily);
    panelFont.setPointSizeF(settings().look.sidebarFontPoint + settings().look.findFontDelta);
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
    find_->setPlaceholderText(mode == Mode::Global ? QStringLiteral("Найти во всех заметках")
                              : mode == Mode::History
                                  ? QStringLiteral("Найти в слепке и в истории заметки")
                                  : QStringLiteral("Найти в заметке"));
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
        if (history_.size() >= settings().look.findHistoryLimit) break;
    }
}

void FindBar::rememberQuery() {
    const QString text = find_->text().trimmed();
    // Тот же порог, что у поиска: однобуквенные запросы не исполняются, и
    // помнить их незачем.
    if (text.size() < 2) return;
    history_.removeAll(text);
    history_.prepend(text);   // свежий сверху
    while (history_.size() > settings().look.findHistoryLimit) history_.removeLast();
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
        menu.addAction(QStringLiteral("пока пусто"))->setEnabled(false);
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
