#include "markdown_edit_view.h"

#include "key_binding.h"
#include "list_line.h"
#include "settings.h"
#include "syntax_highlighter.h"

#include <QContextMenuEvent>
#include <QKeyEvent>
#include <QMenu>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextLayout>

#include <utility>
#include <vector>

namespace zametti {

MarkdownEditView::MarkdownEditView(QWidget* parent) : PlainEditView(parent) {
    refreshAppearance();
}

MarkdownEditView::~MarkdownEditView() {
    // Соединения снимает база (ещё при живых членах — порядок разрушения, см.
    // ~PlainEditView); подсветчик отпускаем сами, пока документ жив.
    disconnect(this, nullptr, this, nullptr);
    disconnect(document(), nullptr, this, nullptr);
    highlighter_.reset();
}

void MarkdownEditView::refreshAppearance() {
    // ПОДСВЕТКА — тот же класс ядра, что расцвечивает строки разности
    // (ZSyntaxHighlighterMD). Он и писался под этот режим. Прикрепляется к
    // СВОЕМУ документу: заметка тут ни при чём.
    highlighter_ = std::make_shared<ZSyntaxHighlighterMD>(
        document(), settings().markdownHighlighting(), 0);
    highlighter_->rehighlight();

    // Сочетание переключения задачи — из настроек, списком, тем же разборщиком,
    // что у обычного вида (key_binding.h); разбирается здесь, а не на каждое
    // нажатие.
    toggleTaskKeys_ = keySequencesOf(settings().editor().toggleTaskKey());

    PlainEditView::refreshAppearance();   // палитра, шрифт, стоп, поля
}

void MarkdownEditView::showSource(const QString& markdown, SourcePos caret) {
    setText(markdown, caret.line, caret.column);
}

SourcePos MarkdownEditView::caretPos() const {
    SourcePos pos;
    pos.line = caretLine();
    pos.column = caretColumn();
    return pos;
}

// --- клавиши ---------------------------------------------------------------
//
// ПРАВИЛА — НАД ТЕКСТОМ, и только над ним: заметка в режиме правку не
// принимает, истина живёт в тексте вида. Что такое «строка пункта», решает
// parseListLine (list_line.h) — то же правило, которым подсветчик красит
// маркер. Всё, что надо знать о строках, узнаётся ДО скобки отмены: состояния
// блоков (в заборе ли) подсветчик перечитывает только на внешнем endEditBlock,
// и внутри скобки они устарели. Каждое нажатие — одна скобка, один шаг отмены.

namespace {

// Строка внутри забора кода — и сам забор, открывающий и закрывающий. Тот же
// признак, которым кладётся плашка.
bool isCodeLine(const QTextBlock& block) {
    return ZSyntaxHighlighterMD::inFence(block.userState()) ||
           (block.previous().isValid() && ZSyntaxHighlighterMD::inFence(block.previous().userState()));
}

// Пункт ли эта строка. Внутри кода пунктов не бывает, как бы строка ни
// выглядела: «- a» в заборе — это код.
ListLine itemOf(const QTextBlock& block, int stop) {
    if (!block.isValid() || isCodeLine(block)) return {};
    return parseListLine(block.text(), stop);
}

int indentColumnOf(const QTextBlock& block, int stop) {
    const QString text = block.text();
    return columnOf(text, leadingWhitespace(text), stop);
}

// Отступ строки знаками — дословно, табы вместе с пробелами.
QString indentStringOf(const QTextBlock& block) {
    const QString text = block.text();
    return text.left(leadingWhitespace(text));
}

// Отступ, которым пункт встаёт ПОД этот пункт: его отступ дословно плюс пробелы
// до его колонки содержимого. Дословно — чтобы чужие табы остались табами.
QString childIndentOf(const QTextBlock& parent, const ListLine& item) {
    return indentStringOf(parent) + QString(qMax(0, item.contentColumn - item.indent), QLatin1Char(' '));
}

// Предыдущий пункт ТОГО ЖЕ отступа — сосед, под которого пункт уходит по Tab.
// Пустые строки и более глубокие пункты пропускаются; более мелкий пункт
// (мы — первый ребёнок) или чужой текст не глубже нашего (список кончился) —
// соседа нет.
QTextBlock siblingAbove(const QTextBlock& block, const ListLine& mine, int stop) {
    for (QTextBlock b = block.previous(); b.isValid(); b = b.previous()) {
        if (isBlankLine(b.text())) continue;
        const ListLine other = itemOf(b, stop);
        if (other.item) {
            if (other.indent == mine.indent) return b;
            if (other.indent < mine.indent) return {};
            continue;
        }
        if (indentColumnOf(b, stop) <= mine.indent) return {};
    }
    return {};
}

// Ближайший пункт выше с МЕНЬШИМ отступом — родитель; на его отступ пункт
// выходит по Shift+Tab. Нет — выходит на нулевой.
QTextBlock parentAbove(const QTextBlock& block, const ListLine& mine, int stop) {
    for (QTextBlock b = block.previous(); b.isValid(); b = b.previous()) {
        if (isBlankLine(b.text())) continue;
        const ListLine other = itemOf(b, stop);
        if (other.item && other.indent < mine.indent) return b;
    }
    return {};
}

// Пункт, который продолжает Shift+Enter: ближайший пункт на строке каретки или
// выше, не глубже неё. Чужой текст мельче строки по дороге — список кончился,
// продолжать нечего. Пустая строка каретки смотрит на предыдущую непустую.
struct Continuation {
    bool found = false;
    QString indent;   // отступ новой строки — до колонки содержимого пункта
};

Continuation continuationOf(const QTextBlock& block, int stop) {
    QTextBlock ref = block;
    while (ref.isValid() && isBlankLine(ref.text())) ref = ref.previous();
    if (!ref.isValid()) return {};
    const int refIndent = indentColumnOf(ref, stop);
    for (QTextBlock b = ref; b.isValid(); b = b.previous()) {
        if (isBlankLine(b.text())) continue;
        const ListLine other = itemOf(b, stop);
        if (other.item) {
            if (other.indent > refIndent) continue;
            // Продолжать можно сам пункт или то, что лежит в его содержимом.
            if (b != ref && refIndent < other.contentColumn) return {};
            return {true, childIndentOf(b, other)};
        }
        if (indentColumnOf(b, stop) < refIndent) return {};
    }
    return {};
}

}  // namespace

void MarkdownEditView::keyPressEvent(QKeyEvent* event) {
    if (keyEventMatchesAny(*event, toggleTaskKeys_)) {
        toggleTasks();
        event->accept();
        return;
    }
    PlainEditView::keyPressEvent(event);
}

// ENTER: пункт продолжается пунктом (пустой пункт — выходит из списка), всё
// остальное — переносом с отступом строки. SHIFT+ENTER: продолжение пункта —
// новая строка под первым знаком содержимого; вне списка — как Enter.
void MarkdownEditView::pressEnter(bool shift) {
    const int stop = tabStop();
    QTextCursor at = textCursor();
    const int from = qMin(at.anchor(), at.position());
    const int to = qMax(at.anchor(), at.position());
    const QTextBlock block = document()->findBlock(from);
    const QTextBlock endBlock = document()->findBlock(to);
    const int column = from - block.position();
    // Строка, какой она станет после удаления выделения: голова до каретки и
    // хвост за концом выделения. Правила смотрят на неё, а не на нынешнюю.
    const QString line = block.text().left(column) + endBlock.text().mid(to - endBlock.position());
    const bool code = isCodeLine(block);
    const ListLine item = code ? ListLine{} : parseListLine(line, stop);

    QString insert;
    bool clearLine = false;
    if (!shift && item.item && column >= item.contentStart) {
        if (item.emptyBody)
            clearLine = true;   // пустой пункт + Enter — из списка вон
        else
            insert = QLatin1Char('\n') + line.left(item.indentChars) + nextMarker(item);
    } else {
        Continuation cont;
        // Shift+Enter продолжает пункт, если каретка стоит в его содержимом (не
        // в отступе и не в маркере) и строка не код.
        if (shift && !code && column >= leadingWhitespace(line) &&
            (!item.item || column >= item.contentStart))
            cont = continuationOf(block, stop);
        if (cont.found) {
            insert = QLatin1Char('\n') + cont.indent;
        } else {
            // Автоотступ — как у базы: отступ строки, обрезанный по каретке.
            PlainEditView::pressEnter(shift);
            return;
        }
    }

    at.beginEditBlock();
    if (at.hasSelection()) at.removeSelectedText();
    if (clearLine) {
        at.movePosition(QTextCursor::StartOfBlock);
        at.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
        at.removeSelectedText();
    } else {
        at.insertText(insert);
    }
    at.endEditBlock();
    setTextCursor(at);
    ensureCursorVisible();
}

// TAB / SHIFT+TAB. На строке пункта — сам пункт: под предыдущего соседа того же
// отступа / на отступ родителя; остальные строки выделения едут на ту же
// дельту (как indentListItems решает по первому блоку), пустые не трогаются.
// Не пункт (в коде, вне списков) — как у базы: пробелы до стопа / снять до
// стопа, выделение в несколько строк — единым сдвигом на стоп.
void MarkdownEditView::pressTab(bool back) {
    const int stop = tabStop();
    QTextCursor at = textCursor();
    const LineSpan span = spanOf(at);
    const bool manyLines = span.last > span.first;
    const QTextBlock first = document()->findBlockByNumber(span.first);
    const ListLine item = itemOf(first, stop);
    if (!item.item) {
        PlainEditView::pressTab(back);
        return;
    }

    // Новый отступ первой строки-пункта по правилу списка; отказ — ничего
    // («первый пункт отступать некуда», выступать из нуля некуда).
    QString newIndent;
    if (!back) {
        const QTextBlock sibling = siblingAbove(first, item, stop);
        if (!sibling.isValid()) return;
        newIndent = childIndentOf(sibling, itemOf(sibling, stop));
    } else {
        if (item.indent == 0) return;
        const QTextBlock parent = parentAbove(first, item, stop);
        newIndent = parent.isValid() ? indentStringOf(parent) : QString();
    }

    // Дельта в знаках для остальных строк выделения.
    const int delta = int(newIndent.size()) - item.indentChars;
    const bool forward = at.anchor() <= at.position();
    const int caretColumn = at.position() - at.block().position();
    const int caretBlock = at.blockNumber();

    at.beginEditBlock();
    {
        // Первая строка-пункт: её отступ заменяется целиком на новый.
        QTextCursor edit(first);
        edit.setPosition(first.position() + item.indentChars, QTextCursor::KeepAnchor);
        edit.insertText(newIndent);
    }
    if (manyLines) indentLines(span.first + 1, span.last, delta);
    at.endEditBlock();

    if (!manyLines) {
        // Каретка остаётся на своём знаке строки; если стояла в отступе — на
        // его конце.
        const QTextBlock line = document()->findBlockByNumber(caretBlock);
        const int shifted = qMax(int(newIndent.size()), caretColumn + delta);
        QTextCursor moved(document());
        moved.setPosition(line.position() + qMin(shifted, qMax(0, line.length() - 1)));
        setTextCursor(moved);
        return;
    }
    selectLines(span.first, span.last, forward);
}

// ПЕРЕКЛЮЧЕНИЕ ЗАДАЧИ (toggleTaskKey): строки выделения или строка каретки;
// первая задача задаёт направление, остальные идут за ней; задач нет — ничего
// (как toggleTask в обычном виде). Длина строк не меняется — выделение цело.
void MarkdownEditView::toggleTasks() {
    const int stop = tabStop();
    QTextCursor at = textCursor();
    const LineSpan span = spanOf(at);

    bool found = false;
    bool target = true;
    std::vector<std::pair<int, bool>> tasks;   // позиция знака в скобках, нынешнее
    for (int number = span.first; number <= span.last; ++number) {
        const QTextBlock line = document()->findBlockByNumber(number);
        const ListLine item = itemOf(line, stop);
        if (!item.item || item.marker != Marker::Task) continue;
        if (!found) {
            found = true;
            target = !item.checked;
        }
        tasks.push_back({line.position() + item.markerEnd - 2, item.checked});
    }
    if (!found) return;

    const int anchor = at.anchor();
    const int position = at.position();
    QTextCursor edit(document());
    edit.beginEditBlock();
    for (const auto& [pos, checked] : tasks) {
        if (checked == target) continue;
        edit.setPosition(pos);
        edit.setPosition(pos + 1, QTextCursor::KeepAnchor);
        edit.insertText(target ? QStringLiteral("x") : QStringLiteral(" "));
    }
    edit.endEditBlock();
    // Замена знака на знак длину не меняет — концы выделения возвращаем как были.
    QTextCursor same(document());
    same.setPosition(anchor);
    same.setPosition(position, QTextCursor::KeepAnchor);
    setTextCursor(same);
}

// --- плашки под блоками кода --------------------------------------------------

// МЕНЮ ПРАВОЙ КНОПКИ В РЕЖИМЕ ИСХОДНИКА (жалоба владельца: «контекстное меню
// сильно усохло, оттуда пропала большая часть опций»).
//
// Усохло оно не сегодня: меню заметки живёт у NoteEditor, а исходник правит
// другой вид, и ему доставалось штатное меню Qt. Операций разметки здесь и не
// может быть — человек пишет её руками, — но то, что вид УМЕЕТ САМ, в меню
// быть обязано: сдвиг пункта и переключение задачи. Клавиши те же, что и в
// заметке, и подписаны рядом.
QMenu* MarkdownEditView::buildContextMenu(const QPoint& at) {
    QMenu* menu = createStandardContextMenu(at);
    if (menu == nullptr) return nullptr;
    menu->setAttribute(Qt::WA_DeleteOnClose);
    menu->addSeparator();

    QAction* indent = menu->addAction(QStringLiteral("Indent"), this, [this] { pressTab(false); });
    indent->setShortcut(QKeySequence(Qt::Key_Tab));
    QAction* outdent =
        menu->addAction(QStringLiteral("Outdent"), this, [this] { pressTab(true); });
    outdent->setShortcut(QKeySequence(Qt::SHIFT | Qt::Key_Backtab));

    QAction* task = menu->addAction(QStringLiteral("Toggle task"), this, [this] { toggleTasks(); });
    if (!toggleTaskKeys_.isEmpty()) task->setShortcut(toggleTaskKeys_.first());
    return menu;
}

void MarkdownEditView::contextMenuEvent(QContextMenuEvent* event) {
    QMenu* menu = buildContextMenu(event->pos());
    if (menu == nullptr) return;
    menu->popup(event->globalPos());
    event->accept();
}

void MarkdownEditView::extraOverlays(QList<QTextEdit::ExtraSelection>& shown) {
    // ПЛАШКА ПОД БЛОКАМИ КОДА (просьба владельца: код видно и в исходнике).
    //
    // Выделением во всю ширину, а не своей отрисовкой: QPlainTextEdit заливает
    // вьюпорт фоном САМ, перед текстом, и нарисованное до него стёрлось бы, а
    // нарисованное после — легло бы поверх букв.
    //
    // Строка внутри забора несёт состояние «в заборе» (его ставит подсветчик);
    // у закрывающего забора состояние уже Plain, но его предшественница — в
    // заборе. Тот же приём, что у вида разности.
    const QColor plate = settings().markdownHighlighting().codeBackground();
    const int height = viewport()->height();
    for (QTextBlock block = firstVisibleBlock(); block.isValid(); block = block.next()) {
        if (blockBoundingGeometry(block).translated(contentOffset()).top() > height) break;
        const bool inFence =
            ZSyntaxHighlighterMD::inFence(block.userState()) ||
            (block.previous().isValid() &&
             ZSyntaxHighlighterMD::inFence(block.previous().userState()));
        if (!inFence) continue;
        // ПОЛОСА НА КАЖДУЮ ВИЗУАЛЬНУЮ СТРОКУ, а не на блок: выделение без
        // диапазона с FullWidthSelection Qt красит ровно ту визуальную строку,
        // где стоит позиция курсора. Длинная строка кода в узком окне
        // переносится — и её хвосты шли на подложке обычного текста (нашёл
        // владелец). Видимые блоки у QPlainTextEdit свёрстаны всегда.
        const QTextLayout* layout = block.layout();
        const int lines = layout != nullptr ? qMax(1, layout->lineCount()) : 1;
        for (int i = 0; i < lines; ++i) {
            QTextEdit::ExtraSelection band;
            band.cursor = QTextCursor(block);
            if (layout != nullptr && i < layout->lineCount())
                band.cursor.setPosition(block.position() + layout->lineAt(i).textStart());
            band.format.setBackground(plate);
            band.format.setProperty(QTextFormat::FullWidthSelection, true);
            shown.push_back(band);
        }
    }
}

}  // namespace zametti
