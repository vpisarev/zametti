#include "editor_widget.h"

#include "document_builder.h"
#include "document_reader.h"
#include "document_saver.h"
#include "editor_ops.h"
#include "parser.h"
#include "settings.h"

#include <QKeyEvent>
#include <QKeySequence>
#include <QMessageBox>
#include <QScrollBar>
#include <QTextCursor>
#include <QTextDocument>

#include <cstdio>
#include <fstream>
#include <sstream>

namespace zametti {
namespace {

bool readFile(const QString& path, std::string& out) {
    std::ifstream in(path.toStdString(), std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

}  // namespace

NoteEditor::NoteEditor(QWidget* parent)
    : NoteView(parent), history_(appearance().undoLimit) {
    setReadOnly(false);
    setUndoRedoEnabled(false);   // историю ведём сами, см. edit_history.h

    // Хоткей разбираем один раз: на каждое нажатие клавиши это было бы разбором
    // строки впустую.
    toggleTaskKey_ = QKeySequence(appearance().toggleTaskKey, QKeySequence::PortableText);

    autosave_.setSingleShot(true);
    connect(&autosave_, &QTimer::timeout, this, [this] { save(true); });
    connect(document(), &QTextDocument::contentsChanged, this,
            &NoteEditor::onContentsChanged);
}

bool NoteEditor::openFile(const QString& path) {
    save(true);

    std::string text;
    if (!readFile(path, text)) {
        std::fprintf(stderr, "не читается: %s\n", path.toUtf8().constData());
        return false;
    }

    path_ = path;
    lastComplaint_.clear();
    // Серию набора обрываем: иначе первая правка в новой заметке подмешалась бы
    // к её исходному состоянию и отменить её было бы нечем.
    sinceLastEdit_.invalidate();

    Document doc = parse(text);
    history_.reset(doc, 0);
    rebuild(doc, 0, 0.0);
    emit fileChanged(path_);
    return true;
}

void NoteEditor::applyZoom(qreal value) {
    if (value == zoom()) return;
    NoteView::setZoom(value);
    refreshAppearance();
}

void NoteEditor::refreshAppearance() {
    // Облик меняется — содержимое нет. Берём его из истории и собираем заново;
    // ни нового шага, ни сдвига по истории при этом не происходит.
    rebuild(history_.current().doc, textCursor().position(), scrollRatio());
}

void NoteEditor::undo() {
    const HistoryStep* step = history_.undo();
    if (step == nullptr) return;
    rebuild(step->doc, step->cursor, scrollRatio());
    document()->setModified(true);
    autosave_.start(appearance().autosaveDelayMs);
}

void NoteEditor::redo() {
    const HistoryStep* step = history_.redo();
    if (step == nullptr) return;
    rebuild(step->doc, step->cursor, scrollRatio());
    document()->setModified(true);
    autosave_.start(appearance().autosaveDelayMs);
}

void NoteEditor::rebuild(const Document& doc, int cursor, double ratio) {
    const bool wasSuspended = recordingSuspended_;
    recordingSuspended_ = true;
    buildDocument(doc, *document(), zoom());
    applyContentWidth();

    QTextCursor place(document());
    place.setPosition(qBound(0, cursor, document()->characterCount() - 1));
    setTextCursor(place);
    setScrollRatio(ratio);

    // Сборка — не правка человека. Без этого открытая неканоническая заметка
    // считалась бы изменённой и переписывалась бы на диске при выходе, хотя мы
    // её всего лишь показали. Кто пересобрал ради отмены — поднимет флаг сам.
    document()->setModified(false);
    recordingSuspended_ = wasSuspended;
}

void NoteEditor::keyPressEvent(QKeyEvent* event) {
    const bool plainEnter = (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) &&
                            (event->modifiers() & ~Qt::KeypadModifier) == Qt::NoModifier;
    if (plainEnter && runOperation(splitBlockAtCursor)) return;
    if (event->key() == Qt::Key_Backspace && event->modifiers() == Qt::NoModifier &&
        runOperation(unwrapListItemAtCursor))
        return;

    // Tab и Shift+Tab внутри списка двигают пункт по уровням; вне списка
    // операция отказывается, и Tab остаётся обычным знаком табуляции.
    if (event->key() == Qt::Key_Tab && event->modifiers() == Qt::NoModifier &&
        runOperation(indentListItems))
        return;
    if (event->key() == Qt::Key_Backtab ||
        (event->key() == Qt::Key_Tab && event->modifiers() == Qt::ShiftModifier)) {
        if (runOperation(outdentListItems)) return;
        return;   // наружу Shift+Tab не отдаём: он увёл бы фокус из окна
    }

    if (!toggleTaskKey_.isEmpty() &&
        QKeySequence(event->keyCombination()).matches(toggleTaskKey_) ==
            QKeySequence::ExactMatch &&
        runOperation(toggleTaskAtCursor))
        return;

    NoteView::keyPressEvent(event);
}

bool NoteEditor::runOperation(bool (*op)(QTextDocument&, QTextCursor&)) {
    QTextCursor cursor = textCursor();
    // Шаг истории у операции свой; правки, которые она делает по дороге, в
    // историю попадать не должны — иначе одно нажатие даст два шага.
    recordingSuspended_ = true;
    const bool handled = op(*document(), cursor);
    recordingSuspended_ = false;
    if (!handled) return false;

    // Операция трогает содержимое, род и уровень; всё оформление, которое из
    // них следует, пересчитывает сборщик — так ни одно свойство не отстанет.
    // Разбивка на блоки после нормализации уже каноническая, поэтому место
    // курсора переживает пересборку.
    const int position = cursor.position();
    Document ir = readDocument(*document());
    history_.push(ir, position);
    // Операция — отдельный шаг: следующая набранная буква к ней не приклеится.
    sinceLastEdit_.invalidate();

    rebuild(ir, position, scrollRatio());
    document()->setModified(true);
    ensureCursorVisible();
    autosave_.start(appearance().autosaveDelayMs);
    return true;
}

void NoteEditor::onContentsChanged() {
    // Пересборка и перекладка полей под ширину окна — это облик. Документу они
    // неотличимы от правки текста, и без этих двух признаков ширина окна
    // заводила бы шаг истории.
    if (recordingSuspended_ || changingLayout()) return;
    recordEdit();
    autosave_.start(appearance().autosaveDelayMs);
}

void NoteEditor::recordEdit() {
    Document doc = readDocument(*document());
    const int cursor = textCursor().position();

    // Набор подряд — один шаг: иначе Ctrl+Z возвращал бы по одной букве. Паузу
    // меряем от предыдущей правки, а не от начала шага, — тогда длинная фраза
    // без пауз остаётся одним шагом, как и ожидается.
    const bool sameRun = sinceLastEdit_.isValid() &&
                         sinceLastEdit_.elapsed() < appearance().undoCoalesceMs;
    if (sameRun) history_.amend(std::move(doc), cursor);
    else history_.push(std::move(doc), cursor);
    sinceLastEdit_.restart();
}

void NoteEditor::save(bool interactive) {
    if (path_.isEmpty() || !document()->isModified()) return;

    const SaveOutcome outcome = saveDocument(*document(), path_, rescueTimestamp());
    if (outcome.result == SaveResult::Written || outcome.result == SaveResult::Unchanged) {
        document()->setModified(false);
        lastComplaint_.clear();
        return;
    }

    std::fprintf(stderr, "%s\n", outcome.message.toUtf8().constData());
    // Одну и ту же беду показываем один раз: автосохранение повторяется по
    // таймеру, и окно с ошибкой раз в полторы секунды — это пытка.
    if (!interactive || outcome.message == lastComplaint_) return;
    lastComplaint_ = outcome.message;
    QMessageBox::warning(this, QStringLiteral("zametti"), outcome.message);
}

double NoteEditor::scrollRatio() const {
    const QScrollBar* bar = verticalScrollBar();
    return bar->maximum() > 0 ? double(bar->value()) / bar->maximum() : 0.0;
}

void NoteEditor::setScrollRatio(double ratio) {
    QScrollBar* bar = verticalScrollBar();
    bar->setValue(int(ratio * bar->maximum()));
}

}  // namespace zametti
