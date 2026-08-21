#include "lang_editor.h"

#include "settings.h"

#include <QTextBlock>

#include <QFontMetricsF>
#include <QFocusEvent>
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
    : LineField(parent), candidates_(candidates) {
    setText(current);
    selectAll();
    // ФОН ЗАКРАШИВАЕМ САМИ, в paintEvent, и это не перестраховка.
    //
    // Прозрачное поле не стирает нарисованное под ним — а под ним лежит
    // прежнее имя языка, и при правке буквы наезжали одна на другую. Первая
    // попытка чинила это палитрой (Base) и autoFillBackground, и стало ХУЖЕ:
    // у поля снят фрейм, а фон роли Base рисует именно стиль вокруг фрейма —
    // делает это он не в каждом стиле, зато autoFillBackground красит ролью
    // Window, то есть чужим цветом. Владелец увидел усиление артефактов.
    //
    // Своя заливка от стиля не зависит вовсе. Цвет — тот же, каким выглядит
    // плашка: полупрозрачную подложку кода складываем с фоном страницы.
    setBackdrop(blend(settings().style().codeBackground(), settings().style().pageBackground()));
    QPalette colours = palette();
    colours.setColor(QPalette::Base, backdrop());
    colours.setColor(QPalette::Text, settings().style().codeLangColor());
    setPalette(colours);
    setAttribute(Qt::WA_MacShowFocusRect, false);
    connect(this, &LineField::edited, this, [this] { updateCompletion(); });
    updateCompletion();
}

int LanguageEditor::caretPosition() const {
    const QTextCursor at = textCursor();
    return at.position() - at.block().position();
}

QString LanguageEditor::language() const { return text() + completion_; }

void LanguageEditor::updateCompletion() {
    completion_.clear();
    const QString typed = text();
    // Дополняем только когда каретка в конце: посреди слова дописанный хвост
    // означал бы не то, что человек правит.
    if (typed.isEmpty() || caretPosition() != typed.size()) return;
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
            // С дополнением: человек видит хвост и жмёт Enter, соглашаясь с ним.
            emit accepted(language());
            event->accept();
            return;
        case Qt::Key_Tab:
            // Tab принимает дополнение, а не уводит фокус: поле живёт поверх
            // текста, и уходить ему некуда.
            takeCompletion();
            event->accept();
            return;
        case Qt::Key_Right:
            if (caretPosition() == text().size() && takeCompletion()) return;
            break;
        default:
            break;
    }
    LineField::keyPressEvent(event);
    // Забой и стрелки правкой текста не считаются, а дополнение от них меняется.
    updateCompletion();
    viewport()->update();
}

void LanguageEditor::paintEvent(QPaintEvent* event) {
    // Заливка, текст и своя каретка — у LineField; здесь дописывается серый
    // хвост дополнения.
    LineField::paintEvent(event);
    if (completion_.isEmpty()) return;

    QPainter painter(viewport());
    painter.setFont(font());
    QColor grey = settings().style().codeLangColor();
    grey.setAlpha(120);
    painter.setPen(grey);
    // Хвост начинается ровно там, где стоит каретка. У QPlainTextEdit
    // cursorRect() отдаёт НАСТОЯЩЕЕ её место; у QLineEdit, на котором поле
    // жило раньше, это была полоса шириной десять пикселей, посаженная
    // серединой на позицию набора, и хвост наезжал на последнюю букву — «c» +
    // «pp» рисовалось как «(cp)p». Здесь этой ловушки больше нет.
    const qreal x = caretRect().left();
    painter.drawText(QRectF(viewport()->rect()).adjusted(x, 0, 0, 0),
                     Qt::AlignVCenter | Qt::AlignLeft, completion_);
}

}  // namespace zametti
