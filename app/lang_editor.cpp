#include "lang_editor.h"

#include "settings.h"

#include <QFontMetricsF>
#include <QKeyEvent>
#include <QPainter>

namespace zametti {
namespace {

// Полупрозрачный цвет поверх плотного — плотным же. Qt складывать цвета не
// умеет, а нам нужен ровно тот вид, который даёт отрисовка полоски.
QColor blend(const QColor& over, const QColor& under) {
    const qreal a = over.alphaF();
    return QColor::fromRgbF(over.redF() * a + under.redF() * (1 - a),
                            over.greenF() * a + under.greenF() * (1 - a),
                            over.blueF() * a + under.blueF() * (1 - a));
}

}  // namespace

LanguageEditor::LanguageEditor(const QStringList& candidates, const QString& current,
                               QWidget* parent)
    : QLineEdit(parent), candidates_(candidates) {
    setFrame(false);
    setText(current);
    selectAll();
    // ФОН НЕПРОЗРАЧНЫЙ, и это не про красоту. Прозрачное поле не стирает то,
    // что нарисовано под ним, — а под ним нарисовано прежнее имя языка, и при
    // правке буквы наезжали одна на другую (владелец увидел это как «фон не
    // чистится»). Цвет берём тот же, каким выглядит полоска: полупрозрачную
    // полоску складываем с фоном страницы и получаем ровно её вид, но плотный.
    QPalette colours = palette();
    colours.setColor(QPalette::Base, blend(appearance().codeStripBackground,
                                           appearance().pageBackground));
    colours.setColor(QPalette::Text, appearance().codeLangColor);
    setPalette(colours);
    setAutoFillBackground(true);
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
    // Начало хвоста берём У САМОЙ КАРЕТКИ, а не считаем шириной набранного:
    // у QLineEdit есть своё внутреннее поле слева, и посчитанное от края
    // виджета место оказывалось на пару пикселей левее — владелец увидел это
    // как смещение первой буквы относительно дописанного.
    const qreal x = cursorRect().left();
    painter.drawText(QRectF(rect()).adjusted(x, 0, 0, 0),
                     Qt::AlignVCenter | Qt::AlignLeft, completion_);
}

void LanguageEditor::focusOutEvent(QFocusEvent* event) {
    QLineEdit::focusOutEvent(event);
    // Ушли мимо — как Esc: молча применять то, чего человек не подтвердил,
    // нельзя, а оставлять поле висеть поверх текста — тем более.
    emit cancelled();
}

}  // namespace zametti
