#include "editor_widget.h"

#include "document_builder.h"
#include "document_reader.h"
#include "document_saver.h"
#include "doc_model.h"
#include "editor_ops.h"
#include "parser.h"
#include "settings.h"

#include <QKeyEvent>
#include <QKeySequence>
#include <QMimeData>
#include <QClipboard>
#include <QGuiApplication>
#include <QMessageBox>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextDocumentFragment>

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

    // Хоткеи разбираем один раз: на каждое нажатие клавиши это было бы разбором
    // строки впустую.
    moveUpKey_ = QKeySequence(appearance().moveUpKey, QKeySequence::PortableText);
    moveDownKey_ = QKeySequence(appearance().moveDownKey, QKeySequence::PortableText);
    const auto bind = [this](const QString& keys, bool (*op)(QTextDocument&, QTextCursor&)) {
        const QKeySequence sequence(keys, QKeySequence::PortableText);
        if (!sequence.isEmpty()) bindings_.push_back({sequence, op});
    };
    const auto bindInline = [this](QKeySequence::StandardKey standard, int bits,
                                   bool (*op)(QTextDocument&, QTextCursor&)) {
        for (const QKeySequence& keys : QKeySequence::keyBindings(standard))
            inlineBindings_.push_back({keys, {bits, op}});
    };
    bindInline(QKeySequence::Bold, SpanBold, toggleBold);
    bindInline(QKeySequence::Italic, SpanItalic, toggleItalic);
    // Зачёркивание своего стандартного сочетания не имеет; Ctrl+K взят из брифа.
    inlineBindings_.push_back(
        {QKeySequence(QStringLiteral("Ctrl+K")), {SpanStrike, toggleStrike}});

    bind(appearance().toggleTaskKey, toggleTaskAtCursor);
    bind(appearance().makeBulletKey, makeBullet);
    bind(appearance().makeOrderedKey, makeOrdered);
    bind(appearance().makeTaskKey, makeTask);
    bind(appearance().makeParagraphKey, makeParagraph);

    autosave_.setSingleShot(true);
    connect(&autosave_, &QTimer::timeout, this, [this] { save(true); });
    connect(document(), &QTextDocument::contentsChanged, this,
            &NoteEditor::onContentsChanged);
    connect(&watcher_, &QFileSystemWatcher::fileChanged, this, &NoteEditor::onFileChanged);
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
    externalPending_ = false;
    externalText_.clear();
    knownContent_ = QByteArray(text.data(), qsizetype(text.size()));
    watchFile();
    // Серию набора обрываем: иначе первая правка в новой заметке подмешалась бы
    // к её исходному состоянию и отменить её было бы нечем.
    sinceLastEdit_.invalidate();

    Document doc = parse(text);
    history_.reset(doc, 0);
    rebuild(doc, 0, 0.0);
    emit fileChanged(path_);
    return true;
}

void NoteEditor::watchFile() {
    if (!watcher_.files().isEmpty()) watcher_.removePaths(watcher_.files());
    if (!path_.isEmpty()) watcher_.addPath(path_);
}

void NoteEditor::onFileChanged(const QString& path) {
    // Замена файла через переименование снимает слежение — возвращаем его.
    // Наш собственный QSaveFile делает ровно это.
    if (!watcher_.files().contains(path)) watcher_.addPath(path);

    std::string text;
    if (!readFile(path, text)) return;   // файл унесли: ждём, пока вернётся
    const QByteArray content(text.data(), qsizetype(text.size()));
    if (content == knownContent_) return;   // это мы сами и записали
    knownContent_ = content;

    // Без несохранённых правок внешнее содержимое — просто ещё один шаг
    // истории: undo вернёт то, что было до него.
    if (!document()->isModified()) {
        adoptExternal(text);
        return;
    }

    // С правками не затираем молча ничего: спрашиваем и ждём ответа.
    externalPending_ = true;
    externalText_ = text;
    emit externalChangeDetected();
}

void NoteEditor::resolveExternalConflict(bool takeExternal) {
    if (!externalPending_) return;
    externalPending_ = false;
    const std::string text = std::move(externalText_);
    externalText_.clear();
    // «Оставить моё» ничего не делает: наша версия перезапишет файл при
    // ближайшем сохранении, и это ровно то, о чём человека спросили.
    if (takeExternal) adoptExternal(text);
}

void NoteEditor::adoptExternal(const std::string& text) {
    Document ir = parse(text);
    history_.push(ir, textCursor().position());
    sinceLastEdit_.invalidate();
    rebuild(ir, textCursor().position(), scrollRatio());
    document()->setModified(false);
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

    const auto pressed = [event](const QKeySequence& keys) {
        return !keys.isEmpty() && QKeySequence(event->keyCombination()).matches(keys) ==
                                      QKeySequence::ExactMatch;
    };
    if (pressed(moveUpKey_) && moveItem(-1)) return;
    if (pressed(moveDownKey_) && moveItem(1)) return;

    // Начертание без выделения — не правка документа, а формат для следующей
    // буквы. Отдельный путь: шага истории здесь нет и быть не должно.
    for (const auto& [keys, style] : inlineBindings_) {
        if (!pressed(keys)) continue;
        if (runOperation(style.op)) return;
        if (!textCursor().hasSelection())
            mergeCurrentCharFormat(inlineStyleForTyping(currentCharFormat(), style.bits));
        return;
    }

    for (const auto& [keys, op] : bindings_)
        if (pressed(keys) && runOperation(op)) return;

    NoteView::keyPressEvent(event);

    // Ctrl+Shift+V — вставка без разбора: иногда markdown в буфере нужен именно
    // как текст.
    // Проверяем сочетание напрямую: matches(Paste) на Ctrl+Shift+V не
    // срабатывает — для Qt это уже другое сочетание.
    if (event->key() == Qt::Key_V &&
        event->modifiers() == (Qt::ControlModifier | Qt::ShiftModifier)) {
        const QClipboard* clipboard = QGuiApplication::clipboard();
        if (clipboard != nullptr) pasteMarkdown(clipboard->text(), true);
        return;
    }

    // Автозамена срабатывает по пробелу и уже после того, как он набран: правило
    // смотрит на то, что человек написал. Отдельным шагом истории — первый
    // Ctrl+Z обязан вернуть набранные знаки, а не отменить предыдущую правку.
    if (event->text() == QStringLiteral(" ")) runOperation(applyInputRuleAtCursor);
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

QMimeData* NoteEditor::createMimeDataFromSelection() const {
    QMimeData* data = new QMimeData;
    data->setText(selectionToMarkdown(textCursor()));
    return data;
}

bool NoteEditor::canInsertFromMimeData(const QMimeData* source) const {
    return source != nullptr && source->hasText();
}

void NoteEditor::insertFromMimeData(const QMimeData* source) {
    if (source == nullptr || !source->hasText()) return;
    // В литеральный блок markdown не вставляется: там текст буквальный, и разбор
    // превратил бы вставленное в разметку, которой в коде взяться неоткуда.
    const QTextBlock block = textCursor().block();
    pasteMarkdown(source->text(), isRawBlock(block) || kindOf(block) == Kind::Code);
}

void NoteEditor::pasteMarkdown(const QString& text, bool literal) {
    if (text.isEmpty()) return;
    const QByteArray utf8 = text.toUtf8();
    const std::string source(utf8.constData(), size_t(utf8.size()));

    Document fragment;
    if (literal) {
        // Один абзац с текстом как есть: переводы строк внутри блока сборщик
        // разметит сам, и они вернутся переводами, а не разметкой.
        Block block;
        block.text = source;
        while (!block.text.empty() && block.text.back() == '\n') block.text.pop_back();
        fragment.push_back(std::move(block));
    } else {
        // Полным парсером ядра, а не вторым упрощённым разбором: их
        // идемпотентность и гарантирует, что скопированное вставится без потерь.
        fragment = parse(source);
    }
    if (fragment.empty()) return;

    QTextDocument staging;
    buildDocument(fragment, staging, zoom());

    recordingSuspended_ = true;
    QTextCursor cursor = textCursor();
    cursor.beginEditBlock();
    if (cursor.hasSelection()) cursor.removeSelectedText();
    cursor.insertFragment(QTextDocumentFragment(&staging));
    const int landed = cursor.position();
    // Вставленное могло приехать из другого места дерева: шов приводим в
    // порядок целиком, документ для этого достаточно мал.
    syncLiteralBlocks(*document(), {0, document()->blockCount() - 1});
    syncLists(*document(), {0, document()->blockCount() - 1});
    cursor.endEditBlock();
    recordingSuspended_ = false;

    Document ir = readDocument(*document());
    history_.push(ir, landed);
    sinceLastEdit_.invalidate();
    rebuild(ir, landed, scrollRatio());
    document()->setModified(true);
    ensureCursorVisible();
    autosave_.start(appearance().autosaveDelayMs);
}

bool NoteEditor::moveItem(int direction) {
    MoveResult moved = moveListItem(*document(), textCursor(), direction);
    if (!moved.done) return false;

    history_.push(moved.doc, textCursor().position());
    sinceLastEdit_.invalidate();
    rebuild(moved.doc, 0, scrollRatio());

    // Курсор ставим по месту в IR: после перестановки прежняя позиция в тексте
    // указывала бы на чужой пункт.
    const QTextBlock landed = blockForIrIndex(*document(), moved.irBlock);
    if (landed.isValid()) {
        QTextCursor place(document());
        place.setPosition(landed.position() +
                          qMin(moved.offsetInBlock, landed.length() - 1));
        setTextCursor(place);
    }
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
        // Запоминаем, что теперь в файле: иначе слежение примет нашу же запись
        // за чужую правку.
        std::string written;
        if (readFile(path_, written))
            knownContent_ = QByteArray(written.data(), qsizetype(written.size()));
        watchFile();
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
