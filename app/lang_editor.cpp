#include "lang_editor.h"

#include "settings.h"

#include <QFontMetricsF>
#include <QKeyEvent>
#include <QPainter>

namespace zametti {

LanguageEditor::LanguageEditor(const QStringList& candidates, const QString& current,
                               QWidget* parent)
    : QLineEdit(parent), candidates_(candidates) {
    setFrame(false);
    setText(current);
    selectAll();
    // Фон свой: поле стоит поверх полоски, и системный белый прямоугольник
    // выглядел бы заплаткой на странице.
    QPalette colours = palette();
    colours.setColor(QPalette::Base, Qt::transparent);
    colours.setColor(QPalette::Text, appearance().codeLangColor);
    setPalette(colours);
    setAttribute(Qt::WA_MacShowFocusRect, false);
    connect(this, &QLineEdit::textEdited, this, [this] { updateCompletion(); });
    updateCompletion();
}

QString LanguageEditor::language() const { return text() + completion_; }

void LanguageEditor::updateCompletion() {
    completion_.clear();
    const QString typed = text();
    // Дополняем только когда каретка в конце: посреди слова дописанный хвост
    // означал бы не то, что человек правит.
    if (typed.isEmpty() || cursorPosition() != typed.size()) return;
    for (const QString& candidate : candidates_) {
        if (candidate.size() <= typed.size()) continue;
        if (!candidate.startsWith(typed, Qt::CaseInsensitive)) continue;
        completion_ = candidate.mid(typed.size());
        return;
    }
}

bool LanguageEditor::takeCompletion() {
    if (completion_.isEmpty()) return false;
    const QString whole = language();
    setText(whole);
    setCursorPosition(whole.size());
    completion_.clear();
    update();
    return true;
}

void LanguageEditor::keyPressEvent(QKeyEvent* event) {
    switch (event->key()) {
        case Qt::Key_Return:
        case Qt::Key_Enter:
            emit accepted(language());
            return;
        case Qt::Key_Escape:
            emit cancelled();
            return;
        case Qt::Key_Tab:
            // Tab принимает дополнение, а не уводит фокус: поле живёт поверх
            // текста, и уходить ему некуда.
            takeCompletion();
            return;
        case Qt::Key_Right:
            if (cursorPosition() == text().size() && takeCompletion()) return;
            break;
        default:
            break;
    }
    QLineEdit::keyPressEvent(event);
    // Забой и стрелки textEdited не шлют, а дополнение от них меняется.
    updateCompletion();
    update();
}

void LanguageEditor::paintEvent(QPaintEvent* event) {
    QLineEdit::paintEvent(event);
    if (completion_.isEmpty()) return;

    QPainter painter(this);
    painter.setFont(font());
    QColor grey = appearance().codeLangColor;
    grey.setAlpha(120);
    painter.setPen(grey);
    const QRectF box = rect();
    const qreal x = QFontMetricsF(font()).horizontalAdvance(text());
    painter.drawText(box.adjusted(x, 0, 0, 0), Qt::AlignVCenter | Qt::AlignLeft, completion_);
}

void LanguageEditor::focusOutEvent(QFocusEvent* event) {
    QLineEdit::focusOutEvent(event);
    // Ушли мимо — как Esc: молча применять то, чего человек не подтвердил,
    // нельзя, а оставлять поле висеть поверх текста — тем более.
    emit cancelled();
}

}  // namespace zametti
