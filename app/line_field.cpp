#include "line_field.h"

#include "note_view.h"   // caretShouldBeDrawn — правило показа каретки одно на всех
#include "settings.h"

#include <QFocusEvent>
#include <QKeyEvent>
#include <QPainter>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextBlockFormat>
#include <QTextCursor>
#include <QTextLayout>

namespace zametti {

LineField::LineField(QWidget* parent) : QPlainTextEdit(parent) {
    setFrameStyle(QFrame::NoFrame);
    // Однострочность: переносов нет, полос нет, лишних полей нет.
    setLineWrapMode(QPlainTextEdit::NoWrap);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setTabChangesFocus(true);
    document()->setDocumentMargin(0);
    setContextMenuPolicy(Qt::NoContextMenu);

    // ШТАТНАЯ КАРЕТКА ПОГАШЕНА НАСМЕРТЬ — ради этого поле и сделано на
    // QPlainTextEdit (см. заголовок). Своя рисуется в paintEvent.
    setCursorWidth(0);
    connect(&caret_, &CaretBlink::phaseChanged, this, [this] { viewport()->update(); });
    connect(this, &QPlainTextEdit::cursorPositionChanged, this, &LineField::showCaret);
    connect(document(), &QTextDocument::contentsChanged, this, [this] {
        showCaret();
        emit edited();
    });
    backdrop_ = settings().style().pageBackground();
}

QString LineField::text() const { return document()->firstBlock().text(); }

void LineField::setText(const QString& text) {
    // Одна строка: перевод строки, приехавший из буфера, — не текст подписи.
    QString flat = text;
    flat.replace(QLatin1Char('\n'), QLatin1Char(' '));
    flat.replace(QChar(QChar::LineSeparator), QLatin1Char(' '));
    setPlainText(flat);
    document()->clearUndoRedoStacks();
    setCursorPosition(-1);
}

void LineField::setCursorPosition(int position) {
    QTextCursor at(document());
    const int last = qMax(0, document()->firstBlock().length() - 1);
    at.setPosition(position < 0 ? last : qBound(0, position, last));
    setTextCursor(at);
}

void LineField::selectAll() { QPlainTextEdit::selectAll(); }

void LineField::setAlignment(Qt::Alignment how) {
    // У QPlainTextEdit выключка — свойство блока, а не виджета.
    QTextCursor at(document());
    QTextBlockFormat format = at.blockFormat();
    format.setAlignment(how);
    at.setBlockFormat(format);
}

void LineField::setBackdrop(const QColor& colour) {
    backdrop_ = colour;
    viewport()->update();
}

QRect LineField::caretRect() const { return cursorRect(); }

void LineField::showCaret() {
    caret_.wake(hasFocus() && !isReadOnly());
    viewport()->update();
}

void LineField::focusInEvent(QFocusEvent* event) {
    QPlainTextEdit::focusInEvent(event);
    showCaret();
}

void LineField::focusOutEvent(QFocusEvent* event) {
    QPlainTextEdit::focusOutEvent(event);
    caret_.sleep();
    viewport()->update();
    // Ушли мимо — как Esc: молча применять то, чего человек не подтвердил,
    // нельзя, а оставлять поле висеть поверх текста — тем более.
    emit cancelled();
}

void LineField::keyPressEvent(QKeyEvent* event) {
    switch (event->key()) {
        case Qt::Key_Return:
        case Qt::Key_Enter:
            emit accepted(text());
            event->accept();
            return;
        case Qt::Key_Escape:
            emit cancelled();
            event->accept();
            return;
        default:
            break;
    }
    QPlainTextEdit::keyPressEvent(event);
}

// Колонка каретки, перерисованная без штатного курсора. `setCursorWidth(0)`
// гасит его не совсем: на ДРОБНОМ масштабе экрана нулевая ширина округляется
// вверх и становится физическим пикселем (qt-caret-facts; та же беда лечится
// тем же приёмом в NoteView и PlainEditView). Замер на масштабе 1.25 показывал
// один тёмный пиксель рядом с нашей кареткой — вот он.
void LineField::repaintOverNativeCaret(QPainter& painter) {
    const QTextBlock block = document()->firstBlock();
    if (!block.isValid() || block.layout() == nullptr) return;
    const QRect cursor = cursorRect();
    const QRect column(cursor.left() - 2, cursor.top() - 1, 6, cursor.height() + 2);

    painter.save();
    painter.setClipRect(column);
    painter.fillRect(column, backdrop_);
    QList<QTextLayout::FormatRange> ranges;
    if (textCursor().hasSelection()) {
        const int a = qMin(textCursor().anchor(), textCursor().position());
        const int b = qMax(textCursor().anchor(), textCursor().position());
        QTextLayout::FormatRange range;
        range.start = a;
        range.length = b - a;
        range.format.setBackground(palette().brush(QPalette::Highlight));
        range.format.setForeground(palette().brush(QPalette::HighlightedText));
        ranges.push_back(range);
    }
    painter.setPen(palette().color(QPalette::Text));
    block.layout()->draw(&painter,
                         blockBoundingGeometry(block).translated(contentOffset()).topLeft(),
                         ranges, column);
    painter.restore();
}

void LineField::paintEvent(QPaintEvent* event) {
    {
        // Сперва своя заливка — ею закрывается всё, что нарисовано под полем
        // (прежнее имя языка, прежняя подпись). Фон роли Base у поля без
        // фрейма рисует стиль, и делает это не всякий; своя заливка от стиля
        // не зависит вовсе.
        QPainter under(viewport());
        under.fillRect(viewport()->rect(), backdrop_);
    }
    QPlainTextEdit::paintEvent(event);

    QPainter painter(viewport());
    repaintOverNativeCaret(painter);
    if (!caret_.on() ||
        !caretShouldBeDrawn(hasFocus(), isReadOnly(), textCursor().hasSelection(), false))
        return;
    QRect at = cursorRect();
    at.setWidth(caretPixelWidth(settings().style().caretWidth(), 1.0));
    painter.fillRect(at, settings().style().caretColor());
}

}  // namespace zametti
