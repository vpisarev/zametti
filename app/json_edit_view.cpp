#include "json_edit_view.h"

#include "json_highlighter.h"
#include "key_binding.h"
#include "settings.h"

#include <QKeyEvent>
#include <QTextBlock>
#include <QTextCursor>

namespace zametti {

JsonEditView::JsonEditView(QWidget* parent) : PlainEditView(parent) { refreshAppearance(); }

JsonEditView::~JsonEditView() {
    // Порядок разрушения, как у вида исходника: соединения снимает база, а
    // подсветчик отпускаем сами, пока документ жив.
    disconnect(this, nullptr, this, nullptr);
    disconnect(document(), nullptr, this, nullptr);
    highlighter_.reset();
}

void JsonEditView::refreshAppearance() {
    highlighter_ = std::make_shared<ZSyntaxHighlighterJSON>(document(), settings().jsonEditing());
    highlighter_->rehighlight();
    commentKeys_ = keySequencesOf(settings().jsonEditing().commentKey());
    PlainEditView::refreshAppearance();
}

int JsonEditView::tabStop() const { return qMax(1, settings().jsonEditing().tabIndent()); }

QColor JsonEditView::wrapMarkColor() const { return settings().jsonEditing().comment(); }

void JsonEditView::keyPressEvent(QKeyEvent* event) {
    if (keyEventMatchesAny(*event, commentKeys_)) {
        toggleComment();
        event->accept();
        return;
    }
    PlainEditView::keyPressEvent(event);
}

void JsonEditView::toggleComment() {
    QTextCursor at = textCursor();
    const LineSpan span = spanOf(at);
    const bool manyLines = span.last > span.first;
    const bool forward = at.anchor() <= at.position();
    const int caretColumn = at.position() - at.block().position();
    const int caretBlock = at.blockNumber();

    // Что делаем — решают все непустые строки разом: если каждая уже
    // закомментирована, снимаем; иначе ставим. Иначе смешанное выделение
    // мигало бы туда-сюда по строкам.
    bool anyLine = false;
    bool allCommented = true;
    int minIndent = -1;
    for (int number = span.first; number <= span.last; ++number) {
        const QTextBlock line = document()->findBlockByNumber(number);
        if (!line.isValid()) continue;
        const QString text = line.text();
        const QString trimmed = text.trimmed();
        if (trimmed.isEmpty()) continue;
        anyLine = true;
        if (!trimmed.startsWith(QLatin1String("//"))) allCommented = false;
        const int indent = int(indentStringOf(line).size());
        if (minIndent < 0 || indent < minIndent) minIndent = indent;
    }
    if (!anyLine) return;

    at.beginEditBlock();
    for (int number = span.first; number <= span.last; ++number) {
        const QTextBlock line = document()->findBlockByNumber(number);
        if (!line.isValid()) continue;
        const QString text = line.text();
        if (text.trimmed().isEmpty()) continue;
        QTextCursor edit(line);
        if (allCommented) {
            // Снять «//» и один пробел за ними, если он есть.
            const int slashes = int(indentStringOf(line).size());
            int drop = 2;
            if (slashes + 2 < text.size() && text.at(slashes + 2) == QLatin1Char(' ')) drop = 3;
            edit.setPosition(line.position() + slashes);
            edit.setPosition(line.position() + slashes + drop, QTextCursor::KeepAnchor);
            edit.removeSelectedText();
            continue;
        }
        edit.setPosition(line.position() + qMax(0, minIndent));
        edit.insertText(QStringLiteral("// "));
    }
    at.endEditBlock();

    if (manyLines) {
        selectLines(span.first, span.last, forward);
        return;
    }
    // Одна строка: каретка остаётся на том же знаке текста.
    const QTextBlock line = document()->findBlockByNumber(caretBlock);
    const int shift = allCommented ? -3 : 3;
    QTextCursor moved(document());
    moved.setPosition(line.position() +
                      qBound(0, caretColumn + shift, qMax(0, line.length() - 1)));
    setTextCursor(moved);
}

}  // namespace zametti
