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
    backdrop_ = blend(settings().style().codeBackground(), settings().style().pageBackground());
    QPalette colours = palette();
    colours.setColor(QPalette::Base, backdrop_);
    colours.setColor(QPalette::Text, settings().style().codeLangColor());
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
    {
        // Сперва своя заливка — ею и закрывается всё, что нарисовано под полем.
        QPainter under(this);
        under.fillRect(rect(), backdrop_);
    }
    QLineEdit::paintEvent(event);
    if (completion_.isEmpty()) return;

    QPainter painter(this);
    painter.setFont(font());
    QColor grey = settings().style().codeLangColor();
    grey.setAlpha(120);
    painter.setPen(grey);
    // Начало хвоста — ровно там, где стоит каретка. Считать его шириной
    // набранного нельзя: у QLineEdit своё внутреннее поле слева, а при длинном
    // имени текст ещё и уезжает вбок. Но и cursorRect().left() не годится —
    // Qt отдаёт под каретку прямоугольник ШИРИНОЙ ДЕСЯТЬ ПИКСЕЛЕЙ, посаженный
    // серединой на позицию каретки (rectForPos: cix - 5, ширина 10). Его левый
    // край — это пять пикселей ВЛЕВО от места набора, и хвост наезжал на
    // последнюю набранную букву: «c» + «pp» рисовалось как «(cp)p». Берём
    // середину — она и есть каретка.
    const qreal x = cursorRect().center().x();
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
