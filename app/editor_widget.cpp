#include "editor_widget.h"

#include "document_builder.h"
#include "document_reader.h"
#include "document_saver.h"
#include "doc_model.h"
#include "editor_ops.h"
#include "image_insert.h"
#include "marker.h"
#include "parser.h"
#include "serializer.h"
#include "settings.h"

#include <QDateTime>
#include <QFileDialog>
#include <QImageReader>
#include <QFileInfo>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QKeySequence>
#include <QMimeData>
#include <QAction>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QMenu>
#include <QDesktopServices>
#include <QUrl>
#include <QGuiApplication>
#include <QMessageBox>
#include <QProgressDialog>
#include <QCheckBox>
#include <QAbstractTextDocumentLayout>
#include <QFontMetricsF>
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

NoteEditor::NoteEditor(QWidget* parent) : NoteView(parent) {
    setReadOnly(false);
    setUndoRedoEnabled(false);   // историю ведём сами, см. edit_history.h

    // Хоткеи разбираем один раз: на каждое нажатие клавиши это было бы разбором
    // строки впустую.
    moveUpKey_ = QKeySequence(appearance().moveUpKey, QKeySequence::PortableText);
    moveDownKey_ = QKeySequence(appearance().moveDownKey, QKeySequence::PortableText);
    // Сочетаний на команду может быть несколько: через точку с запятой.
    const auto bind = [this](const QString& keys, bool (*op)(QTextDocument&, QTextCursor&)) {
        for (const QKeySequence& sequence :
             QKeySequence::listFromString(keys, QKeySequence::PortableText))
            if (!sequence.isEmpty()) bindings_.push_back({sequence, op});
    };
    const auto bindInline = [this](QKeySequence::StandardKey standard, int bits,
                                   bool (*op)(QTextDocument&, QTextCursor&)) {
        for (const QKeySequence& keys : QKeySequence::keyBindings(standard))
            inlineBindings_.push_back({keys, {bits, op}});
    };
    bindInline(QKeySequence::Bold, SpanBold, toggleBold);
    bindInline(QKeySequence::Italic, SpanItalic, toggleItalic);
    // Встроенный код: Ctrl+E — так его помечают всюду, где вообще помечают.
    inlineBindings_.push_back(
        {QKeySequence(QStringLiteral("Ctrl+E")), {SpanCode, toggleCode}});
    // Зачёркивание своего стандартного сочетания не имеет; Ctrl+K взят из брифа.
    inlineBindings_.push_back(
        {QKeySequence(QStringLiteral("Ctrl+K")), {SpanStrike, toggleStrike}});

    // Автозамены из конфига: сочетание и знак, который оно вставляет.
    // Сочетаний на одну замену может быть несколько, через точку с запятой —
    // как и у любой другой команды.
    for (const auto& [keys, text] : appearance().specialKeys) {
        if (text.isEmpty()) continue;
        for (const QKeySequence& sequence :
             QKeySequence::listFromString(keys, QKeySequence::PortableText))
            if (!sequence.isEmpty()) specialKeys_.push_back({sequence, text});
    }

    // Уровень заголовка — ДЕЙСТВИЯМИ, а не разбором события. С зажатым Shift
    // event->key() приходит знаком верхнего регистра ("@" вместо "2" на
    // латинской раскладке, кавычка на русской), и сравнение с Qt::Key_2 не
    // срабатывает никогда — владелец нажал Ctrl+Shift+2 и не получил ничего.
    // Сопоставление сочетаний Qt делает сама и с учётом раскладки.
    for (int level = 0; level <= 6; ++level) {
        auto* action = new QAction(this);
        action->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+%1").arg(level)));
        action->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        connect(action, &QAction::triggered, this, [this, level] {
            runOperation([level](QTextDocument& doc, QTextCursor& at) {
                return setHeadingLevel(doc, at, level);
            });
        });
        addAction(action);
    }

    bind(appearance().toggleTaskKey, toggleTaskAtCursor);
    bind(appearance().makeBulletKey, makeBullet);
    bind(appearance().makeOrderedKey, makeOrdered);
    bind(appearance().makeTaskKey, makeTask);
    bind(appearance().makeParagraphKey, makeParagraph);
    bind(appearance().makeCommentKey, toggleCommentAtCursor);

    autosave_.setSingleShot(true);
    connect(&autosave_, &QTimer::timeout, this, [this] { save(true); });
    snapshot_.setSingleShot(true);
    // Тишина дольше склейки — серия набора кончилась: записываем шаг и
    // закрываем серию, следующая буква заведёт новый шаг.
    connect(&snapshot_, &QTimer::timeout, this, [this] {
        flushPendingEdit();
        note_.typingRun = false;
        note_.runChars = 0;
    });
    connectDocument();
    connect(&watcher_, &QFileSystemWatcher::fileChanged, this, &NoteEditor::onFileChanged);
    externalSettle_.setSingleShot(true);
    connect(&externalSettle_, &QTimer::timeout, this, &NoteEditor::onExternalSettled);
    connect(this, &QTextEdit::cursorPositionChanged, this, &NoteEditor::onCaretMoved);
    connect(this, &QTextEdit::cursorPositionChanged, this,
            &NoteEditor::snapCaretOffImage);
}

// Внутри хитро-отрисованной строки-фотографии каретке делать нечего: любой
// заход внутрь сводится к началу строки (и фотография показывается выбранной),
// шаг вправо с её начала перепрыгивает строку целиком. Направление входа
// различается по прошлой позиции. Действует только на показанные фото: строка
// без файла — обычный текст.
void NoteEditor::snapCaretOffImage() {
    if (snappingCaret_) return;
    const QTextCursor cursor = textCursor();
    const int cameFrom = note_.lastCaretPosition;
    note_.lastCaretPosition = cursor.position();
    if (cursor.hasSelection()) return;
    const QTextBlock block = cursor.block();
    if (cursor.positionInBlock() == 0) return;
    if (imageRectInViewport(block).isEmpty()) return;

    QTextCursor moved = cursor;
    if (cameFrom == block.position() && cursor.position() == cameFrom + 1 &&
        block.next().isValid()) {
        moved.setPosition(block.next().position());
    } else {
        moved.setPosition(block.position());
    }
    snappingCaret_ = true;
    setTextCursor(moved);
    snappingCaret_ = false;
    note_.lastCaretPosition = moved.position();
}

// Строка из одних пробелов неотличима глазом от пустой, а ведёт себя как
// текст — на этом ловилась «склейка при двух пустых». Правило владельца:
// хвостовые пробелы умирают, как только каретка уходит со строки; опустевшая
// строка становится настоящей пустой. Пока каретка на строке — свобода:
// человек имеет право начать с «   слово». Кода это не касается: там
// хвостовые пробелы — содержимое.
void NoteEditor::onCaretMoved() {
    if (tidying_ || recordingSuspended_ || changingLayout()) {
        note_.lastLine = textCursor();
        return;
    }
    const QTextCursor now = textCursor();
    if (!note_.lastLine.isNull() && note_.lastLine.document() == document()) {
        const QString text = note_.lastLine.block().text();
        int line = 0;
        for (int i = 0; i < note_.lastLine.positionInBlock() && i < text.size(); ++i)
            if (text.at(i) == QChar::LineSeparator) ++line;
        const QString nowText = now.block().text();
        int nowLine = 0;
        for (int i = 0; i < now.positionInBlock() && i < nowText.size(); ++i)
            if (nowText.at(i) == QChar::LineSeparator) ++nowLine;
        if (note_.lastLine.blockNumber() != now.blockNumber() ||
            line != nowLine)
            tidyLeftLine(note_.lastLine);
    }
    note_.lastLine = textCursor();
}

// Границы того, что менялось с прошлой уборки. Курсор, а не пара чисел:
// позиции плывут от каждой следующей правки, а курсор Qt двигает сам —
// починка после набора и слияние блоков область не сбивают.
//
// Свои же вырезы копить незачем: уборка только удаляет пробелы и новых не
// заводит.
void NoteEditor::onContentsChange(int position, int charsRemoved, int charsAdded) {
    Q_UNUSED(charsRemoved);
    // Границы шага отмены считаются по ЭТИМ числам, а не по курсору редактора:
    // курсор к моменту разбора может ещё стоять на старом месте (правка пришла
    // не с клавиатуры, а из вставки или из системы ввода).
    if (!tidying_ && !recordingSuspended_) {
        note_.changeStart = position;
        note_.changeEnd = position + charsAdded;
        note_.changeSeparator = false;
        for (int i = position; i < position + charsAdded; ++i) {
            const QChar ch = document()->characterAt(i);
            if (ch.isSpace() || ch.isPunct() || ch == QChar::ParagraphSeparator) {
                note_.changeSeparator = true;
                break;
            }
        }
    }
    // Границы нужны не только уборке: место под фотографии тоже перемеряется
    // после каждой правки, и по всему документу это 458 мкс на большой
    // заметке — почти всё, что мы добавляем сверх Qt.
    markImageRegion(position, charsAdded);
    if (tidying_) return;
    const int last = qMax(0, document()->characterCount() - 1);
    const int from = qBound(0, position, last);
    const int to = qBound(from, position + charsAdded, last);
    if (note_.dirty.isNull() || note_.dirty.document() != document()) {
        note_.dirty = QTextCursor(document());
        note_.dirty.setPosition(from);
        note_.dirty.setPosition(to, QTextCursor::KeepAnchor);
        return;
    }
    const int lo = qMin(note_.dirty.selectionStart(), from);
    const int hi = qMax(note_.dirty.selectionEnd(), to);
    note_.dirty.setPosition(lo);
    note_.dirty.setPosition(hi, QTextCursor::KeepAnchor);
}

// Хвостовые пробелы — везде, кроме строки каретки, кода и дословных кусков.
//
// Проход не по всему документу, а по накопленной области правки: хвостовым
// пробелам неоткуда взяться там, куда правка не дотянулась. Строку, с которой
// ушла каретка, чистит tidyLeftLine — она в область может и не попасть.
// Соседний блок с каждой стороны берём про запас: правка на границе блоков
// сливает и делит их, и то, что стало «соседом», час назад было серединой.
//
// Полный проход остаётся там, где документ и так собирается целиком: после
// пересборки область забывается, и подметать в ней нечего.
void NoteEditor::tidySweep(const QTextCursor& caret) {
    if (tidying_ || note_.dirty.isNull()) return;
    const int first = qMax(0, document()->findBlock(note_.dirty.selectionStart()).blockNumber() - 1);
    const int afterLast =
        qMin(document()->blockCount() - 1,
             document()->findBlock(note_.dirty.selectionEnd()).blockNumber() + 1);
    note_.dirty = QTextCursor();

    const int caretBlock = caret.blockNumber();
    int caretLine = 0;
    {
        const QString text = caret.block().text();
        for (int i = 0; i < caret.positionInBlock() && i < text.size(); ++i)
            if (text.at(i) == QChar::LineSeparator) ++caretLine;
    }

    // Сначала собрать, потом резать с конца: позиции не плывут.
    std::vector<std::pair<int, int>> cuts;
    QTextBlock block = document()->findBlockByNumber(first);
    for (int number = first; number <= afterLast && block.isValid();
         ++number, block = block.next()) {
        if (isRawBlock(block)) continue;
        const Kind kind = kindOf(block);
        if (kind == Kind::Code) continue;
        const QString text = block.text();
        int line = 0;
        int lineStart = 0;
        for (int i = 0; i <= text.size(); ++i) {
            if (i != text.size() && text.at(i) != QChar::LineSeparator) continue;
            if (!(block.blockNumber() == caretBlock && line == caretLine)) {
                int cut = i;
                while (cut > lineStart && (text.at(cut - 1) == QLatin1Char(' ') ||
                                           text.at(cut - 1) == QLatin1Char('\t')))
                    --cut;
                if (cut < i) cuts.push_back({block.position() + cut, block.position() + i});
            }
            lineStart = i + 1;
            ++line;
        }
    }
    if (cuts.empty()) return;

    tidying_ = true;
    QTextCursor edit(document());
    edit.beginEditBlock();
    for (auto it = cuts.rbegin(); it != cuts.rend(); ++it) {
        edit.setPosition(it->first);
        edit.setPosition(it->second, QTextCursor::KeepAnchor);
        edit.removeSelectedText();
    }
    edit.endEditBlock();
    tidying_ = false;
}

void NoteEditor::tidyLeftLine(const QTextCursor& left) {
    const QTextBlock block = left.block();
    if (!block.isValid() || isRawBlock(block)) return;
    const Kind kind = kindOf(block);
    // Пустую строку и черту чистим тоже: на них могли пожить пробелы, пока
    // каретка там стояла. Не трогаем только код: там хвостовые пробелы —
    // содержимое.
    if (kind == Kind::Code) return;

    const QString text = block.text();
    // Границы строки, на которой стояла каретка.
    int from = 0;
    for (int i = left.positionInBlock() - 1; i >= 0; --i)
        if (text.at(i) == QChar::LineSeparator) { from = i + 1; break; }
    int to = text.size();
    for (int i = left.positionInBlock(); i < text.size(); ++i)
        if (text.at(i) == QChar::LineSeparator) { to = i; break; }
    int cut = to;
    while (cut > from && (text.at(cut - 1) == QLatin1Char(' ') ||
                          text.at(cut - 1) == QLatin1Char('\t')))
        --cut;
    const bool emptied = cut == from;
    // Пустая ХВОСТОВАЯ строка многострочного блока — мусор от удаления: при
    // сохранении она затвердела бы в неразрывный пробел. Отрезаем её в
    // настоящую пустую строку. Серединные пустые не трогаем: ими человек
    // намеренно отбивает куски внутри блока.
    const bool tailOfBlock = to == text.size();
    if (cut == to && !(emptied && tailOfBlock && from > 0)) return;

    tidying_ = true;
    QTextCursor edit(document());
    edit.beginEditBlock();
    if (cut < to) {
        edit.setPosition(block.position() + cut);
        edit.setPosition(block.position() + to, QTextCursor::KeepAnchor);
        edit.removeSelectedText();
    }
    const QTextBlock after = edit.block();
    if (emptied && tailOfBlock && from > 0) {
        // Снять перенос перед опустевшей строкой и завести настоящую пустую
        // строку после блока.
        edit.setPosition(block.position() + from - 1);
        edit.setPosition(block.position() + from, QTextCursor::KeepAnchor);
        edit.removeSelectedText();
        edit.movePosition(QTextCursor::EndOfBlock);
        edit.insertBlock(vspaceBlockFormat(*document(), false, false));
        const BlockRange range{qMax(0, block.blockNumber() - 1), block.blockNumber() + 2};
        syncGaps(*document(), range);
        syncLists(*document(), range);
        applyListGeometry(*document(), range);
    } else if (after.text().isEmpty() && kind == Kind::Paragraph) {
        // Строка (и весь блок) опустела: это настоящая пустая строка.
        edit.setBlockFormat(vspaceBlockFormat(*document(),
                                              after.previous().isValid() &&
                                                  isVSpaceBlock(after.previous()),
                                              after.blockNumber() == 0));
        const BlockRange range{qMax(0, after.blockNumber() - 1), after.blockNumber() + 1};
        syncGaps(*document(), range);
        syncLists(*document(), range);
        applyListGeometry(*document(), range);
    }
    edit.endEditBlock();
    tidying_ = false;
}


// Сигналы документа подключаются заново на каждой подмене: документ у нас не
// один на всю жизнь виджета, а свой у каждой заметки.
//
// Оба сигнала, и порядок важен: contentsChange приходит первым и приносит
// границы правки, contentsChanged — следом, и по нему уже подметаем.
void NoteEditor::connectDocument() {
    connect(document(), &QTextDocument::contentsChange, this, &NoteEditor::onContentsChange);
    connect(document(), &QTextDocument::contentsChanged, this, &NoteEditor::onContentsChanged);
}

void NoteEditor::installSession(NoteSession session) {
    // Подмена заметки — ОДНА операция, а не «присвоить объект, потом поставить
    // документ». Порознь между ними существует миг, когда виджет смотрит на
    // уже разрушенный документ: присваивание объекта убивает старый вместе с
    // ним. Так и падало, пока не свёл в одно место.
    std::unique_ptr<QTextDocument> previous = std::move(note_.document);
    if (document() != nullptr) disconnect(document(), nullptr, this, nullptr);
    note_ = std::move(session);
    setDocument(note_.document.get());
    connectDocument();
    applyContentWidth();

    QTextCursor place(document());
    place.setPosition(qBound(0, note_.cursor, document()->characterCount() - 1));
    setTextCursor(place);
    verticalScrollBar()->setValue(note_.scroll);
    document()->setModified(note_.modified);
    // Курсоры, державшиеся за прежний документ, теперь ни на что не указывают.
    note_.lastLine = QTextCursor();
    note_.dirty = QTextCursor();

    // Мимолётное состояние жестов принадлежит не заметке, а прикосновению к
    // ней, и через подмену не переносится: номер блока, за угол которого тянут,
    // в новом документе значит совсем другое.
    imageResizeBlock_ = -1;
    imageHoverCorner_ = false;
    pressedAnchor_.clear();

    // Таймеры перенастраиваются под новую заметку. Отложенный снимок — её
    // свойство и приехал вместе с ней; висящий от прошлой заметки таймер
    // отменяем, иначе он записал бы шаг в чужую цепочку.
    snapshot_.stop();
    autosave_.stop();
    if (note_.pendingEdit) snapshot_.start(appearance().undoCoalesceMs);
    watchFile();
}

void NoteEditor::installDocument(std::unique_ptr<QTextDocument> doc) {
    // Прежний держим живым до самой подмены: Qt удаляет старый документ только
    // если сам его и заводил, а наши — наши, и умирают здесь, строкой ниже.
    std::unique_ptr<QTextDocument> previous = std::move(note_.document);
    if (document() != nullptr) disconnect(document(), nullptr, this, nullptr);
    note_.document = std::move(doc);
    setDocument(note_.document.get());
    connectDocument();
    // Курсоры, державшиеся за прежний документ, теперь ни на что не указывают.
    note_.lastLine = QTextCursor();
    note_.dirty = QTextCursor();
}

qint64 NoteEditor::estimateDocumentBytes(const QTextDocument& doc) {
    // Знаки UTF-16 плюс множитель на фрагменты, форматы и undo-стек Qt; плюс
    // надбавка на сам документ, чтобы у крошечной заметки вес не выходил
    // нулевым. Обоснование множителя — замером, см. documentCacheSizeMb.
    return qint64(doc.characterCount()) * 2 * 9 / 2 + 4096;
}

qint64 NoteEditor::cachedNoteBytes() const {
    qint64 total = 0;
    for (const NoteSession& note : noteCache_) total += note.bytes;
    return total;
}

void NoteEditor::clearNoteCache() {
    noteCache_.clear();
}

void NoteEditor::trimNoteCache() {
    const qint64 budget = qint64(qMax(1, appearance().documentCacheSizeMb)) * 1024 * 1024;
    // С хвоста, пока не уложились: самое давнее уходит первым. От вытесненной
    // заметки остаётся только место каретки — вот единственное место, где оно
    // попадает в общую карту. Каретка принадлежит заметке и живёт в её объекте;
    // карта — это ОСТАТОК объекта, а не второй источник правды о нём.
    while (!noteCache_.empty() && cachedNoteBytes() > budget) {
        const NoteSession& going = noteCache_.back();
        if (!going.path.isEmpty()) caretMemory_[going.path] = going.cursor;
        noteCache_.pop_back();
    }
}

void NoteEditor::stashCurrentNote() {
    // Место каретки — свойство заметки, и живёт оно в её объекте. Но объект
    // тяжёлый и в кэше остаётся не всегда, а место каретки весит четыре байта
    // и терять его незачем. Поэтому здесь, в единственной точке ухода заметки
    // из открытых, от неё остаётся этот лёгкий след. Второй записи в карту в
    // программе нет: иначе появился бы второй источник правды о каретке.
    if (!note_.path.isEmpty()) caretMemory_[note_.path] = textCursor().position();

    // Откладываем только ЧИСТОЕ и только то, чей отпечаток мы знаем: иначе при
    // возврате не с чем было бы сверять файл. Несохранённое не откладываем
    // вовсе — потерять правки страшнее, чем пересобрать документ.
    if (note_.path.isEmpty() || note_.document == nullptr) return;
    if (document()->isModified() || note_.digest.empty()) return;
    flushPendingEdit();

    // Инвариант кэша: в нём лежат только документы, чья сериализация БАЙТ В
    // БАЙТ равна файлу. Документ имеет право отличаться от файла: хвост пустых
    // строк в конце набирается случайно, в файл не идёт и при перечитывании
    // исчезает. Отдав такой документ из кэша, мы вернули бы человеку то, чего в
    // файле нет.
    //
    // Сериализуется документ КАК ЕСТЬ, без приведения к тому, что умеет
    // файл: именно приведение и срезает хвост, и со сверкой через него
    // отпечатки сходились бы всегда. Нам нужен другой вопрос — «этот документ
    // и есть файл?», а не «запишется ли он в тот же файл».
    Document read = readDocument(*document());
    read.meta = note_.meta;
    if (hashOf(serialize(read)) != note_.digest) return;

    const qint64 bytes = estimateDocumentBytes(*document());
    const qint64 budget = qint64(qMax(1, appearance().documentCacheSizeMb)) * 1024 * 1024;
    // Заметка тяжелее всего бюджета в кэш не идёт: она вытеснила бы всё
    // остальное и всё равно осталась бы одна.
    if (bytes > budget) return;

    // Прежняя запись про этот же файл больше не нужна.
    std::erase_if(noteCache_,
                  [this](const NoteSession& note) { return note.path == note_.path; });

    // Уезжает ВЕСЬ объект заметки, а не выбранные поля. Забыть перенести
    // что-то нельзя: переносится всё, потому что переносится он сам.
    note_.cursor = textCursor().position();
    note_.scroll = verticalScrollBar()->value();
    note_.modified = false;
    note_.bytes = bytes;
    noteCache_.insert(noteCache_.begin(), std::move(note_));
    note_ = NoteSession{};
    trimNoteCache();
}

bool NoteEditor::restoreCachedNote(const QString& path, const Digest& digest) {
    const auto at = std::find_if(noteCache_.begin(), noteCache_.end(),
                                 [&path](const NoteSession& note) { return note.path == path; });
    if (at == noteCache_.end()) return false;
    // Отпечаток не сошёлся — файл правили снаружи. Отложенное выбрасываем и
    // собираем с диска: терять нечего, в кэш попало только записанное.
    if (at->digest != digest) {
        noteCache_.erase(at);
        return false;
    }

    NoteSession note = std::move(*at);
    noteCache_.erase(at);
    note.modified = false;   // в кэш попадает только записанное
    installSession(std::move(note));
    return true;
}

bool NoteEditor::openFile(const QString& path) {
    // Открытие другой заметки выводит из режима истории. Без этого редактор
    // остался бы показывать слепок ПРЕЖНЕЙ заметки, имея путь новой, — и
    // первая же правка записала бы чужое прошлое в новый файл. Найдено
    // пробником: после ухода и возврата режим оставался включён.
    leaveHistory();
    save(true, true);   // уходим из заметки: пробуем записать, не спрашивая признак

    std::string text;
    if (!readFile(path, text)) {
        std::fprintf(stderr, "не читается: %s\n", path.toUtf8().constData());
        return false;
    }

    // Место каретки запоминается в stashCurrentNote — единственной точке, где
    // заметка перестаёт быть открытой. Память живёт до выхода из приложения:
    // между запусками место помнит только последняя заметка (state.json), и
    // заводить ради этого файл на каждую заметку незачем.

    // Уходя из заметки, откладываем её целиком — если есть что откладывать.
    if (note_.path != path) stashCurrentNote();

    Digest digest = hashOf(text);
    // Хранилище наше, и держать в нём сор незачем: лишние пробелы в конце строк
    // и недостающий перевод строки в конце файла причёсываются прямо на диске,
    // не трогая ни одного значения в шапке. Заметку всего лишь открыли —
    // всплывать наверх списка недавних ей не с чего.
    canonicaliseNoteFile(path, text, digest);
    note_.path = path;
    setImageBase(QFileInfo(path).absolutePath());
    note_.lastComplaint.clear();
    note_.externalPending = false;
    note_.externalText.clear();
    externalSettle_.stop();
    note_.externalEmptyRetried = false;
    note_.lastLine = QTextCursor();
    note_.digest = digest;
    // Копия файла в памяти: с ней сравнивается всё, что мы соберёмся писать.
    note_.lastSaved = QByteArray(text.data(), qsizetype(text.size()));
    note_.journalTailKnown = false;   // журнал этой заметки ещё не смотрели
    watchFile();
    // Серию набора обрываем: иначе первая правка в новой заметке подмешалась бы
    // к её исходному состоянию и отменить её было бы нечем.
    forgetPendingEdit();

    // Опорная запись. Заметки старше журнала: хранилище жило годами, а история
    // заведена только сейчас. Если журнала у заметки ещё нет, кладём в него то,
    // с чем её открыли, — иначе первой записью стало бы первое сохранение, и
    // всё, чем заметка была до него, не попало бы в историю никогда. Владелец
    // на это и наткнулся: он опустошил заметку (Ctrl+A, Delete), автосохранение
    // записало пустоту, и она оказалась самой первой записью её истории.
    //
    // Время берём у файла, а не «сейчас»: содержимое ровно такой давности, и
    // таймлайн не должен утверждать, будто заметка написана в эту минуту.
    recordBaseline(QByteArray(text.data(), qsizetype(text.size())));

    // Отложенная заметка: файл не разбираем и документ не собираем вовсе —
    // история, каретка и прокрутка возвращаются такими, какими были.
    if (restoreCachedNote(path, digest)) {
        emit fileChanged(note_.path);
        return true;
    }

    Document doc = parse(text);
    note_.meta = doc.meta;
    const int caret = caretMemory_.value(note_.path, 0);
    note_.undoChain.reset(doc, caret);
    // Открывается другой файл: с прежним документом у нового ничего общего,
    // заплатке не за что зацепиться.
    note_.builtValid = false;
    installDocument(std::make_unique<QTextDocument>());
    rebuild(doc, caret, {});
    // Показать место каретки, а не начало документа: иначе «вернуться туда,
    // где читал» означало бы прокрутить заново.
    if (caret > 0) ensureCursorVisible();
    emit fileChanged(note_.path);
    return true;
}

void NoteEditor::watchFile() {
    if (!watcher_.files().isEmpty()) watcher_.removePaths(watcher_.files());
    if (!note_.path.isEmpty()) watcher_.addPath(note_.path);
}

void NoteEditor::onFileChanged(const QString& path) {
    // Замена файла через переименование снимает слежение — возвращаем его.
    // Наш собственный QSaveFile делает ровно это.
    if (!watcher_.files().contains(path)) watcher_.addPath(path);

    // Не читаем сразу: внешние редакторы пишут «обрезать → записать», и первый
    // сигнал часто застаёт файл пустым. Ждём паузу тишины; каждый новый сигнал
    // отодвигает срок.
    onExternalSettled();   // ПРОБА: без отстойника
}

void NoteEditor::onExternalSettled() {
    std::string text;
    if (!readFile(note_.path, text)) return;   // файл унесли: ждём, пока вернётся
    const Digest digest = hashOf(text);
    if (digest == note_.digest) return;   // это мы сами и записали

    // Файл опустел, а был непустым: похоже, мы всё же попали в середину чужой
    // записи. Одна повторная попытка, прежде чем поверить в пустоту. «Был
    // непустым» — это «прежний отпечаток не равен отпечатку пустоты»: у пустого
    // входа отпечаток свой, и с «не считали» он не путается.
    if (text.empty() && note_.digest != hashOf(std::string_view()) &&
        !note_.externalEmptyRetried) {
        note_.externalEmptyRetried = true;
        externalSettle_.start(300);
        return;
    }
    note_.externalEmptyRetried = false;
    note_.digest = digest;
    note_.lastSaved = QByteArray(text.data(), qsizetype(text.size()));

    // Шаг истории пишется здесь, а не после ответа человека: файл на диске уже
    // изменился, и это случилось независимо от того, примем мы чужую версию
    // или перезапишем своей. «Оставить моё» тогда ляжет следующей записью.
    recordHistory(journal::Kind::External, QByteArray(text.data(), qsizetype(text.size())));

    // Без несохранённых правок внешнее содержимое — просто ещё один шаг
    // истории: undo вернёт то, что было до него.
    if (!document()->isModified()) {
        adoptExternal(text);
        return;
    }

    // С правками не затираем молча ничего: спрашиваем и ждём ответа.
    note_.externalPending = true;
    note_.externalText = text;
    emit externalChangeDetected();
}

void NoteEditor::resolveExternalConflict(bool takeExternal) {
    if (!note_.externalPending) return;
    note_.externalPending = false;
    const std::string text = std::move(note_.externalText);
    note_.externalText.clear();
    // «Оставить моё» ничего не делает: наша версия перезапишет файл при
    // ближайшем сохранении, и это ровно то, о чём человека спросили.
    if (takeExternal) adoptExternal(text);
}

void NoteEditor::adoptExternal(const std::string& text) {
    flushPendingEdit();
    Document ir = parse(text);
    // Чужой редактор мог снести или испортить блок метаданных. Тихо принять
    // это нельзя: заметка потеряла бы родителя и дату создания, то есть уехала
    // бы в корень и «постарела». Прежние значения у нас в памяти — предлагаем
    // вернуть их одним действием, а решает человек.
    const NoteMeta previous = note_.meta;
    const bool lost = previous.present && !ir.meta.present;
    // Ключи, которые были и пропали. parent сюда не входит: его правка руками
    // — законный перенос заметки, а не потеря (решение брифа этапа 4).
    QStringList dropped;
    if (!lost && previous.present && ir.meta.present) {
        for (const char* key : {"created", "id"}) {
            if (!previous.get(key).empty() && ir.meta.get(key).empty())
                dropped.append(QString::fromLatin1(key));
        }
    }

    // role — ключ хранилища, а не текста: им задаётся, папка это или заметка,
    // а заметка папкой никогда не становится и наоборот. Что бы ни вписал
    // снаружи чужой редактор, ставим обратно своё значение; не было своего —
    // просто снимаем ключ. Спрашивать тут нечего: подмена рода не «правка».
    if (ir.meta.present && ir.meta.get("role") != previous.get("role"))
        ir.meta.set("role", previous.get("role"));

    note_.meta = ir.meta;
    note_.undoChain.push(ir, textCursor().position());
    note_.typingRun = false;
    rebuild(ir, textCursor().position(), viewAnchor());
    document()->setModified(false);

    emit externalAdopted(note_.path);
    if (lost || !dropped.isEmpty()) {
        note_.lostMeta = previous;
        emit metaDamaged(note_.path, lost ? QStringList{QStringLiteral("весь блок")} : dropped);
    }
}

void NoteEditor::restoreDamagedMeta() {
    if (!note_.lostMeta.present) return;
    NoteMeta restored = note_.lostMeta;
    note_.lostMeta = NoteMeta();
    // Правки тела сохраняются: меняется только шапка. Обычная запись — значит
    // и обычная отмена: вернуть всё как было можно тем же Ctrl+Z.
    editMeta([&restored](NoteMeta& meta) {
        // Ключи, которые чужой редактор оставил, важнее прежних: он мог
        // осмысленно поправить parent, и затирать это нельзя.
        for (const std::string& line : restored.lines) {
            const size_t colon = line.find(':');
            if (colon == std::string::npos) continue;
            const std::string key = line.substr(0, colon);
            if (!meta.get(key).empty()) continue;
            meta.set(key, line.substr(colon + 1).find_first_not_of(' ') == std::string::npos
                              ? std::string()
                              : line.substr(line.find_first_not_of(' ', colon + 1)));
        }
        meta.present = true;
    });
}

void NoteEditor::applyZoom(qreal value) {
    if (value == zoom()) return;
    NoteView::setZoom(value);
    refreshAppearance();
}

void NoteEditor::refreshAppearance() {
    flushPendingEdit();
    // Облик запечён в документах при сборке: всё отложенное протухло разом.
    clearNoteCache();
    // Облик меняется — содержимое нет. Берём его из истории и собираем заново;
    // ни нового шага, ни сдвига по истории при этом не происходит.
    //
    // Именно собираем: от облика зависит каждый блок, в том числе и те, что не
    // менялись, и заплатка их не тронула бы.
    note_.builtValid = false;
    rebuild(note_.undoChain.current().doc, textCursor().position(), viewAnchor());
}

void NoteEditor::keepCaretOffEdge() {
    const QRect at = cursorRect();
    const int height = viewport()->height();
    if (height <= 0) return;

    // Зазор — то же поле страницы, в высотах строки. Больше половины окна не
    // берём: в узком окне зазор сверху и снизу иначе перекрылись бы.
    const qreal lineUnit = QFontMetricsF(baseFont()).height();
    const int gap = qBound(0, qRound(appearance().verticalMargin * lineUnit), height / 3);

    QScrollBar* bar = verticalScrollBar();
    if (at.top() < gap) bar->setValue(bar->value() - (gap - at.top()));
    else if (at.bottom() > height - gap) bar->setValue(bar->value() + at.bottom() - height + gap);
}

void NoteEditor::showEditPlace(int scrollBefore) {
    const int height = viewport()->height();
    // Место правки в координатах документа. Спрашивать «видно ли сейчас» нельзя:
    // пересборка ставит курсор через setTextCursor, а он подкручивает вид сам —
    // к моменту нашего вопроса место уже видно, причём ровно у кромки.
    const int where = verticalScrollBar()->value() + cursorRect().center().y();

    // Было ли оно видно до правки. Если было — возвращаем вид как стоял: человек
    // и так смотрит на это место, дёргать картинку незачем.
    if (where >= scrollBefore && where <= scrollBefore + height) {
        verticalScrollBar()->setValue(scrollBefore);
        return;
    }
    verticalScrollBar()->setValue(where - height / 2);
}

int NoteEditor::findMatches(const QString& text, bool caseSensitive) {
    note_.matchText = text;
    note_.matchCaseSensitive = caseSensitive;
    note_.matches.clear();
    note_.currentMatch = -1;
    if (!text.isEmpty()) {
        QTextDocument::FindFlags flags;
        if (caseSensitive) flags |= QTextDocument::FindCaseSensitively;
        QTextCursor at(document());
        while (true) {
            at = document()->find(text, at, flags);
            if (at.isNull()) break;
            note_.matches.push_back(at);
            // Со следующего знака после НАЧАЛА совпадения: перекрывающиеся
            // вхождения тоже вхождения, и счётчик обязан считать их так же,
            // как их обойдёт F3.
            QTextCursor next(document());
            next.setPosition(at.selectionStart() + 1);
            if (next.position() >= document()->characterCount() - 1) break;
            at = next;
        }
    }
    showMatchHighlights();
    return int(note_.matches.size());
}

void NoteEditor::showMatchHighlights() {
    QList<QTextEdit::ExtraSelection> selections;
    selections.reserve(int(note_.matches.size()));
    const QColor base = appearance().selectionBackground;
    // Текущее совпадение — контрастнее прочих. Не другим цветом: цвет в
    // оформлении один, а разной должна быть заметность.
    QColor pale = base;
    pale.setAlpha(110);
    for (size_t i = 0; i < note_.matches.size(); ++i) {
        QTextEdit::ExtraSelection selection;
        selection.cursor = note_.matches[i];
        selection.format.setBackground(int(i) == note_.currentMatch ? base : pale);
        selections.append(selection);
    }
    setExtraSelections(selections);
}

void NoteEditor::goToMatch(int index) {
    if (note_.matches.empty()) return;
    const int count = int(note_.matches.size());
    note_.currentMatch = ((index % count) + count) % count;
    const int scrollBefore = verticalScrollBar()->value();
    setTextCursor(note_.matches[size_t(note_.currentMatch)]);
    showMatchHighlights();
    // Тем же правилом, что и правки: пока совпадение в пределах видимости —
    // картинку не дёргаем, ушло за край — показываем по центру.
    showEditPlace(scrollBefore);
}

void NoteEditor::stepMatch(int direction) {
    if (note_.matches.empty()) return;
    if (note_.currentMatch >= 0) {
        goToMatch(note_.currentMatch + direction);
        return;
    }
    // Первый шаг — от каретки, а не с начала заметки: человек только что на
    // что-то смотрел, и прыжок в начало документа был бы неожиданным.
    const int at = textCursor().position();
    if (direction > 0) {
        for (size_t i = 0; i < note_.matches.size(); ++i)
            if (note_.matches[i].selectionStart() >= at) {
                goToMatch(int(i));
                return;
            }
        goToMatch(0);
        return;
    }
    for (size_t i = note_.matches.size(); i-- > 0;)
        if (note_.matches[i].selectionEnd() <= at) {
            goToMatch(int(i));
            return;
        }
    goToMatch(int(note_.matches.size()) - 1);
}

void NoteEditor::clearMatches() {
    note_.matches.clear();
    note_.currentMatch = -1;
    note_.matchText.clear();
    setExtraSelections({});
}

bool NoteEditor::replaceCurrentMatch(const QString& with) {
    if (note_.currentMatch < 0 || size_t(note_.currentMatch) >= note_.matches.size()) return false;
    const QTextCursor target = note_.matches[size_t(note_.currentMatch)];
    const bool done = runOperation([&](QTextDocument&, QTextCursor& cursor) {
        cursor.setPosition(target.selectionStart());
        cursor.setPosition(target.selectionEnd(), QTextCursor::KeepAnchor);
        cursor.insertText(with);
        return true;
    });
    if (!done) return false;
    // Документ пересобран — прежние курсоры недействительны, ищем заново и
    // встаём на следующее вхождение.
    const int at = note_.currentMatch;
    findMatches(note_.matchText, note_.matchCaseSensitive);
    if (!note_.matches.empty()) goToMatch(at < int(note_.matches.size()) ? at : 0);
    return true;
}

int NoteEditor::replaceAllMatches(const QString& text, bool caseSensitive,
                                  const QString& with) {
    if (text.isEmpty()) return 0;
    int replaced = 0;
    const bool done = runOperation([&](QTextDocument& doc, QTextCursor& cursor) {
        QTextDocument::FindFlags flags;
        if (caseSensitive) flags |= QTextDocument::FindCaseSensitively;
        // Одна транзакция на всю замену: иначе Ctrl+Z откатывал бы её по
        // одному вхождению.
        cursor.beginEditBlock();
        QTextCursor at(&doc);
        while (true) {
            at = doc.find(text, at, flags);
            if (at.isNull()) break;
            at.insertText(with);
            ++replaced;
        }
        cursor.endEditBlock();
        return replaced > 0;
    });
    if (!done) return 0;
    findMatches(text, caseSensitive);
    return replaced;
}

void NoteEditor::undo() {
    // В режиме истории отмена шагает по слепкам. Клавиша это и так знает, но
    // отмена приходит и из меню, и из тестов, а поведение обязано быть одно.
    if (inHistory()) {
        historyStepBack();
        return;
    }
    flushPendingEdit();
    // Граница серии отмены: первое Ctrl+Z после правок сначала сохраняет.
    // Иначе вершина цепочки — то, что человек только что набрал, — не попала
    // бы в историю вовсе: он отменяет её, уходит из заметки, и сохранять уже
    // нечего. Сохранение само пишет свой шаг истории, отдельной записи тут нет.
    //
    // Только первый шаг: дальше человек идёт по уже записанному прошлому, и
    // складывать в историю промежуточные состояния отката значило бы забивать
    // её ровно тем, от чего он уходит.
    if (document()->isModified() && !note_.undoRun) save(false);
    const int scrollBefore = verticalScrollBar()->value();
    // Курсор ставим туда, где была отменяемая правка, — но её позиция записана
    // в координатах ОТМЕНЯЕМОГО документа, а вернём мы другой. Отображаем
    // через общий префикс и суффикс плоских текстов: до префикса позиции
    // совпадают, после суффикса сдвинуты на разницу длин, а внутри изменённой
    // зоны каретка идёт к началу расхождения. Голое записанное смещение
    // промахивалось: правка добавила знаки выше каретки — и в более коротком
    // возвращённом документе каретка прыгала на пару строк вниз.
    const int recorded = note_.undoChain.current().cursor;
    const QString undonePlain = document()->toPlainText();
    const HistoryStep* step = note_.undoChain.undo();
    if (step == nullptr) {
        // Дно внутридокументной цепочки — дальше шагаем в слепки. Режим
        // объявляет себя сам (баннер, заголовок окна), и это же служит защитой
        // от случайного глубокого отката.
        enterHistory();
        return;
    }
    note_.undoRun = true;
    rebuild(step->doc, 0, viewAnchor());
    const QString restoredPlain = document()->toPlainText();

    const int shared = int(qMin(undonePlain.size(), restoredPlain.size()));
    int prefix = 0;
    while (prefix < shared && undonePlain.at(prefix) == restoredPlain.at(prefix)) ++prefix;
    int suffix = 0;
    while (suffix < shared - prefix &&
           undonePlain.at(undonePlain.size() - 1 - suffix) ==
               restoredPlain.at(restoredPlain.size() - 1 - suffix))
        ++suffix;

    int where = prefix;
    if (recorded <= prefix) where = recorded;
    else if (recorded >= int(undonePlain.size()) - suffix) {
        // Хвостовая зона, но не ниже конца изменённого участка: отмена
        // возвращает к месту правки, а не туда, куда каретка уехала после неё
        // (правило черты, например, уводит её на блок ниже).
        const int mapped = recorded + int(restoredPlain.size()) - int(undonePlain.size());
        where = qMin(mapped, int(restoredPlain.size()) - suffix);
    }
    QTextCursor cursor(document());
    cursor.setPosition(qBound(0, where, int(document()->characterCount()) - 1));
    setTextCursor(cursor);

    // Вид держится сам, но отменённая правка может оказаться за окном — тогда
    // её надо показать: человек нажал отмену, чтобы увидеть результат.
    showEditPlace(scrollBefore);
    document()->setModified(true);
    autosave_.start(appearance().autosaveDelayMs);
}

void NoteEditor::redo() {
    if (inHistory()) {
        historyStepForward();
        return;
    }
    flushPendingEdit();
    const int scrollBefore = verticalScrollBar()->value();
    const HistoryStep* step = note_.undoChain.redo();
    if (step == nullptr) return;
    note_.undoRun = true;
    rebuild(step->doc, step->cursor, viewAnchor());
    showEditPlace(scrollBefore);
    document()->setModified(true);
    autosave_.start(appearance().autosaveDelayMs);
}

NoteEditor::ViewAnchor NoteEditor::viewAnchor() const {
    const QAbstractTextDocumentLayout* layout = document()->documentLayout();
    const int top = verticalScrollBar()->value();
    const QTextBlock at = document()->findBlock(layout->hitTest(QPointF(0, top), Qt::FuzzyHit));
    if (!at.isValid()) return {};

    // Держимся только за блок НАД правкой: если правка выше кромки, номера
    // блоков IR за ней съедут, и якорь показал бы на чужой блок.
    const int anchorIndex = irIndexOfBlock(at);
    if (anchorIndex > irIndexOfBlock(textCursor().block())) return {};
    return {anchorIndex, top - int(layout->blockBoundingRect(at).top())};
}

void NoteEditor::rebuild(const Document& doc, int cursor, const ViewAnchor& anchor,
                         const Document* current) {
    const bool wasSuspended = recordingSuspended_;
    recordingSuspended_ = true;
    // Заплатка вместо сборки: на большой заметке сборка стоит 151 мс, а
    // меняется при обычной правке один блок. Облик и масштаб задают каждый
    // блок, а не только изменившиеся, — при их смене заплатка не годится.
    bool patched = false;
    if (note_.builtValid && note_.builtZoom == zoom()) {
        Document read;
        if (current == nullptr) {
            read = readDocument(*document());
            current = &read;
        }
        patched = patchDocument(note_.built, *current, doc, *document(), zoom());
    }
    if (!patched) buildDocument(doc, *document(), zoom());
    note_.built = doc;
    note_.builtValid = true;
    note_.builtZoom = zoom();

    // Слова и строки — здесь и только здесь (плюс запись на диск). Полная
    // сборка на заметке в 239 КБ стоит 19 мс, счёт по IR — 1.2 мс: в таком
    // соседстве он незаметен. За заплаткой не считаем вовсе: она стоит 64 мкс,
    // и счёт был бы в двадцать раз дороже самой правки.
    if (!patched) refreshStats(doc);
    applyContentWidth();
    // Сборка — не правка: подметать за ней нечего, а область от неё вышла бы
    // во весь документ и утащила бы следующую уборку на полный проход.
    note_.dirty = QTextCursor();

    QTextCursor place(document());
    place.setPosition(qBound(0, cursor, document()->characterCount() - 1));
    setTextCursor(place);

    // Возвращаем блок-якорь на прежнее место относительно кромки. Прокрутка при
    // пересборке сбрасывается в ноль, и без этого документ прыгал бы к началу.
    const QTextBlock landed = blockForIrIndex(*document(), anchor.irIndex);
    const bool held = anchor.irIndex >= 0 && landed.isValid();
    if (held) {
        const QRectF rect = document()->documentLayout()->blockBoundingRect(landed);
        verticalScrollBar()->setValue(int(rect.top()) + anchor.above);
    }
    // Показывать курсор — только если вид ни за что не держится. Иначе мы сами
    // же сбивали бы наведённый вид: правки над IR ставят курсор уже после
    // пересборки, и здесь он ещё стоит в начале документа. Прокрутка уезжала
    // к началу, а потом обратно вниз — и переставленный пункт оказывался у
    // самой нижней кромки окна.

    // Сборка — не правка человека. Без этого открытая неканоническая заметка
    // считалась бы изменённой и переписывалась бы на диске при выходе, хотя мы
    // её всего лишь показали. Кто пересобрал ради отмены — поднимет флаг сам.
    document()->setModified(false);
    recordingSuspended_ = wasSuspended;
}

QTextBlock NoteEditor::checkboxUnder(const QMouseEvent& event) const {
    if (event.button() != Qt::LeftButton) return QTextBlock();
    // Точка в координатах документа: вьюпорт прокручен, а раскладка — нет.
    const QPointF point(event.position().x() + horizontalScrollBar()->value(),
                        event.position().y() + verticalScrollBar()->value());
    return blockAtCheckbox(*document(), point, baseFont());
}

// Зона угла: квадрат вокруг каждого из четырёх углов фотографии, наполовину
// внутри, наполовину снаружи — в пиксель попадать не приходится.
QTextBlock NoteEditor::imageCornerUnder(const QPoint& pos, bool* onRight,
                                        bool* onBottom) {
    const int hit = document()->documentLayout()->hitTest(
        QPointF(pos) + QPointF(horizontalScrollBar()->value(),
                               verticalScrollBar()->value()),
        Qt::FuzzyHit);
    if (hit < 0) return {};
    // Кандидаты — блок под точкой и его сосед сверху: фотография живёт в
    // нижнем поле своего блока, и точка над углом может числиться за соседом.
    QTextBlock candidate = document()->findBlock(hit);
    for (int step = 0; step < 2 && candidate.isValid(); ++step) {
        const QRectF photo = imageRectInViewport(candidate);
        if (!photo.isEmpty()) {
            const qreal grip = qMax(12.0, 10.0 * zoom());
            for (int corner = 0; corner < 4; ++corner) {
                const bool right = (corner & 1) != 0;
                const bool bottom = (corner & 2) != 0;
                const QPointF at(right ? photo.right() : photo.left(),
                                 bottom ? photo.bottom() : photo.top());
                const QRectF zone(at.x() - grip / 2 - 2, at.y() - grip / 2 - 2,
                                  grip + 4, grip + 4);
                if (!zone.contains(QPointF(pos))) continue;
                if (onRight != nullptr) *onRight = right;
                if (onBottom != nullptr) *onBottom = bottom;
                return candidate;
            }
        }
        candidate = step == 0 ? candidate.previous() : QTextBlock();
    }
    return {};
}

void NoteEditor::mouseMoveEvent(QMouseEvent* event) {
    if (imageResizeBlock_ >= 0) {
        // Живой примерочный размер; запись — на отпускании.
        const qreal delta =
            imageResizeSign_ * (event->position().x() - imageResizePressX_) / zoom();
        setImageDragWidth(imageResizeBlock_, qMax(24.0, imageResizeStart_ + delta));
        event->accept();
        return;
    }
    bool right = false;
    bool bottom = false;
    const bool hover =
        imageCornerUnder(event->position().toPoint(), &right, &bottom).isValid();
    if (hover != imageHoverCorner_ || hover) {
        imageHoverCorner_ = hover;
        // Диагональ курсора — по углу: ↘ у главной диагонали, ↗ у побочной.
        viewport()->setCursor(!hover ? Qt::IBeamCursor
                              : right == bottom ? Qt::SizeFDiagCursor
                                                : Qt::SizeBDiagCursor);
    }
    if (hover) {
        event->accept();
        return;
    }
    // Ладонь над ссылкой при зажатом Ctrl: знак, что клик её откроет.
    if (!imageHoverCorner_) {
        const bool overLink = (event->modifiers() & Qt::ControlModifier) != 0 &&
                              !anchorAt(event->position().toPoint()).isEmpty();
        viewport()->setCursor(overLink ? Qt::PointingHandCursor : Qt::IBeamCursor);
    }
    NoteView::mouseMoveEvent(event);
}

void NoteEditor::mouseReleaseEvent(QMouseEvent* event) {
    if (imageResizeBlock_ < 0) {
        // Ctrl+клик по ссылке — открыть адрес. Qt в редактируемом виджете
        // ссылок сам не активирует (замерено пробником: anchorClicked молчит
        // и в чистом QTextBrowser), поэтому сверка нажатия и отпускания своя.
        if (!pressedAnchor_.isEmpty() &&
            (event->modifiers() & Qt::ControlModifier) != 0 &&
            anchorAt(event->position().toPoint()) == pressedAnchor_ &&
            !textCursor().hasSelection()) {
            const QString anchor = pressedAnchor_;
            pressedAnchor_.clear();
            QDesktopServices::openUrl(QUrl::fromUserInput(anchor));
            event->accept();
            return;
        }
        pressedAnchor_.clear();
        NoteView::mouseReleaseEvent(event);
        return;
    }
    const QTextBlock block = document()->findBlockByNumber(imageResizeBlock_);
    // Записывается то, что показано: ширина после предела колонки, а не голая
    // позиция мыши за её краем.
    const qreal shown = block.isValid() ? imageRectInViewport(block).width() / zoom() : 0.0;
    imageResizeBlock_ = -1;
    setImageDragWidth(-1, 0.0);

    if (block.isValid() && shown > 0.0 && std::fabs(shown - imageResizeStart_) >= 1.0) {
        QTextCursor cursor(block);
        setTextCursor(cursor);
        const int width = qRound(shown);
        runOperation([width](QTextDocument& doc, QTextCursor& at) {
            return setImageWidthAtCursor(doc, at, width);
        });
    }
    event->accept();
}

void NoteEditor::mousePressEvent(QMouseEvent* event) {
    // Нажатие на ссылке запоминается: отпускание с Ctrl на том же адресе
    // откроет его (см. mouseReleaseEvent).
    pressedAnchor_ = event->button() == Qt::LeftButton
                         ? anchorAt(event->position().toPoint())
                         : QString();

    bool onRight = false;
    bool onBottom = false;
    const QTextBlock corner =
        event->button() == Qt::LeftButton
            ? imageCornerUnder(event->position().toPoint(), &onRight, &onBottom)
            : QTextBlock();
    if (corner.isValid()) {
        const QRectF photo = imageRectInViewport(corner);
        imageResizeBlock_ = corner.blockNumber();
        imageResizePressX_ = event->position().x();
        imageResizeSign_ = onRight ? 1.0 : -1.0;
        imageResizeStart_ = photo.width() / zoom();
        event->accept();
        return;   // каретка остаётся где была: человек взялся за угол, не за текст
    }

    // Щелчок по фотографии выбирает её: каретка в начало строки, а не в
    // случайное место скрытого текста.
    if (event->button() == Qt::LeftButton &&
        (event->modifiers() & Qt::ShiftModifier) == 0) {
        const int hitAt = document()->documentLayout()->hitTest(
            QPointF(event->position()) + QPointF(horizontalScrollBar()->value(),
                                                 verticalScrollBar()->value()),
            Qt::FuzzyHit);
        QTextBlock under = hitAt >= 0 ? document()->findBlock(hitAt) : QTextBlock();
        for (int step = 0; step < 2 && under.isValid(); ++step) {
            const QRectF photo = imageRectInViewport(under);
            if (!photo.isEmpty() && photo.contains(event->position())) {
                setTextCursor(QTextCursor(under));
                event->accept();
                return;
            }
            under = step == 0 ? under.previous() : QTextBlock();
        }
    }

    const QTextBlock hit = checkboxUnder(*event);
    if (!hit.isValid()) {
        NoteView::mousePressEvent(event);
        return;
    }

    // Щелчок внутри выделения переключает всё выделенное разом и выделение
    // сохраняет — ровно как Ctrl+Space. Иначе выделить десяток задач и отметить
    // их одним движением было бы нельзя.
    const QTextCursor cursor = textCursor();
    const int from = qMin(cursor.anchor(), cursor.position());
    const int to = qMax(cursor.anchor(), cursor.position());
    const bool insideSelection = cursor.hasSelection() && hit.position() < to &&
                                 hit.position() + hit.length() > from;
    if (!insideSelection) {
        QTextCursor place = cursor;
        place.setPosition(hit.position());
        setTextCursor(place);
    }
    runOperation(toggleTaskAtCursor);
}

void NoteEditor::mouseDoubleClickEvent(QMouseEvent* event) {
    // По рамке — молча: первый щелчок уже переключил задачу, а выделять слово
    // под рамкой человек не собирался.
    if (checkboxUnder(*event).isValid()) return;
    NoteView::mouseDoubleClickEvent(event);
}

void NoteEditor::dropLinkAtRightEdge() {
    const QTextCursor cursor = textCursor();
    if (cursor.hasSelection() || !currentCharFormat().isAnchor()) return;

    // Внутри ссылки набор её продолжает — так и надо. Речь только о правом крае:
    // там, где следующий знак ссылке уже не принадлежит.
    const QString href = currentCharFormat().anchorHref();
    QTextCursor next = cursor;
    if (next.movePosition(QTextCursor::Right)) {
        const QTextCharFormat ahead = next.charFormat();
        if (ahead.isAnchor() && ahead.anchorHref() == href) return;
    }

    QTextCharFormat plain = currentCharFormat();
    plain.setAnchor(false);
    plain.clearProperty(QTextFormat::AnchorHref);
    plain.setFontUnderline(cursor.block().charFormat().fontUnderline());
    plain.setForeground(cursor.block().charFormat().foreground());
    setCurrentCharFormat(plain);
}

void NoteEditor::keepColumnAcrossMargins(QKeyEvent* event) {
    const bool vertical = event->key() == Qt::Key_Down || event->key() == Qt::Key_Up;
    if (!vertical) return;

    const QTextBlock before = textCursor().block();
    const qreal marginBefore = before.blockFormat().leftMargin();
    const qreal x = cursorRect().x() + horizontalScrollBar()->value();

    NoteView::keyPressEvent(event);

    const QTextBlock after = textCursor().block();
    const qreal marginAfter = after.blockFormat().leftMargin();
    if (after == before || qFuzzyCompare(marginAfter + 1.0, marginBefore + 1.0)) return;

    // Текст нового блока начинается на столько же правее или левее — значит и
    // курсор должен переехать вместе с ним.
    const QPointF wanted(x + marginAfter - marginBefore,
                         cursorRect().center().y() + verticalScrollBar()->value());
    const int at = document()->documentLayout()->hitTest(wanted, Qt::FuzzyHit);
    if (at < 0 || document()->findBlock(at) != after) return;

    QTextCursor moved = textCursor();
    moved.setPosition(at, event->modifiers().testFlag(Qt::ShiftModifier)
                              ? QTextCursor::KeepAnchor
                              : QTextCursor::MoveAnchor);
    setTextCursor(moved);
}

void NoteEditor::keyPressEvent(QKeyEvent* event) {
    // Голое нажатие модификатора ничего не редактирует и каретку не двигает, а
    // хвостовой keepCaretOffEdge прокручивал бы вид к ней: нажал Ctrl перед
    // Ctrl+кликом — и текст упрыгал к каретке. Мимо всей обработки.
    switch (event->key()) {
        case Qt::Key_Control:
        case Qt::Key_Shift:
        case Qt::Key_Alt:
        case Qt::Key_AltGr:
        case Qt::Key_Meta:
        case Qt::Key_CapsLock:
            NoteView::keyPressEvent(event);
            return;
        default:
            break;
    }

    // В слепке клавиши работают иначе: править нечего, зато ходить по истории
    // и копировать из неё — можно, ради этого режим и заведён.
    if (inHistory()) {
        if (event->matches(QKeySequence::Undo)) {   // шаг в более старое
            historyStepBack();
            event->accept();
            return;
        }
        if (event->matches(QKeySequence::Redo)) {   // шаг в более новое и в живое
            historyStepForward();
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_Escape) {
            leaveHistory();
            event->accept();
            return;
        }
        // Печатающая клавиша не восстанавливает ничего (решение владельца:
        // случайное нажатие при попытке выделить не должно менять режим).
        // Окно на этот сигнал подсвечивает кнопку «Восстановить эту».
        if (!event->text().isEmpty() && event->text().at(0).isPrint() &&
            (event->modifiers() & ~Qt::ShiftModifier) == Qt::NoModifier) {
            emit historyEditRefused();
            event->accept();
            return;
        }
        // Всё остальное — базовому виджету: перемещение каретки, выделение,
        // Ctrl+C. Правки он и сам не пропустит, поле только для чтения.
        NoteView::keyPressEvent(event);
        return;
    }

    // Esc посреди перетаскивания угла фотографии — отмена жеста: примерочная
    // ширина снимается, ничего не записывается. Отпускание кнопки после этого
    // идёт обычным путём и тоже ничего не пишет — ему нечего писать, признак
    // перетаскивания уже снят. Без этого начатый жест приходилось доводить до
    // конца и отменять записанное через Ctrl+Z.
    if (event->key() == Qt::Key_Escape && imageResizeBlock_ >= 0) {
        imageResizeBlock_ = -1;
        setImageDragWidth(-1, 0.0);
        event->accept();
        return;
    }

    // Отмену обрабатываем здесь, а не ярлыком окна: QTextEdit объявляет Ctrl+Z
    // своим и глотает его — ярлык не срабатывает ни разу. Собственная история у
    // нас всё равно своя, так что и клавиша должна быть нашей.
    if (event->matches(QKeySequence::Undo)) {
        undo();
        return;
    }
    if (event->matches(QKeySequence::Redo)) {
        redo();
        return;
    }

    // Ctrl+C/Ctrl+X на строке-фотографии без выделения: каретка на картинке —
    // это выбранная картинка. В клипборд идёт текстовое представление строки
    // (вики-вложение или канон image-спана) — Ctrl+V вставит его обратно через
    // полный парсер ядра, тем же путём, что и любой markdown.
    if (!textCursor().hasSelection() &&
        (event->matches(QKeySequence::Copy) || event->matches(QKeySequence::Cut)) &&
        blockImageRef(textCursor().block()).valid) {
        QTextCursor line = textCursor();
        line.setPosition(line.block().position());
        line.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
        QGuiApplication::clipboard()->setText(selectionToMarkdown(line));
        if (event->matches(QKeySequence::Cut)) runOperation(cutImageLineAtCursor);
        return;
    }

    const bool plainEnter = (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) &&
                            (event->modifiers() & ~Qt::KeypadModifier) == Qt::NoModifier;
    // "---" и Enter — тоже тематическая черта, как и "---" с пробелом:
    // правило раньше разреза, иначе Enter развёл бы дефисы и новый блок.
    // На фотографии Enter не делит блок, а заводит пустую строку ЗА ней:
    // каретка на картинке считается стоящей сразу за ней (правило владельца).
    if (plainEnter && runOperation(newLineAfterImage)) return;
    if (plainEnter && runOperation(applyDividerRuleAtCursor)) return;
    if (plainEnter && runOperation(splitBlockAtCursor)) return;

    // Shift+Enter — «другое»: в абзаце разрезает, в списке переносит строку
    // внутри пункта.
    const bool shiftEnter = (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) &&
                            (event->modifiers() & ~Qt::KeypadModifier) == Qt::ShiftModifier;
    if (shiftEnter && runOperation(splitBlockOtherwiseAtCursor)) return;

    // Блок кода из выделенного и обратно. Ctrl+Shift+E рядом с Ctrl+E: тот
    // делает код в строке, этот — блоком.
    if (event->modifiers() == (Qt::ControlModifier | Qt::ShiftModifier) &&
        event->key() == Qt::Key_E) {
        applyIrEdit(toggleCodeBlock(*document(), textCursor()));
        return;
    }
    // Фотография — атом, как черта: Backspace и Delete не грызут её скрытый
    // текст по буквам, а убирают строку целиком. Правило действует, только
    // когда фото показано (файл читается) — иначе строка это видимый текст и
    // правится как текст.
    if ((event->key() == Qt::Key_Backspace || event->key() == Qt::Key_Delete) &&
        event->modifiers() == Qt::NoModifier && !textCursor().hasSelection()) {
        const QTextBlock own = textCursor().block();
        if (!imageRectInViewport(own).isEmpty()) {
            runOperation(cutImageLineAtCursor);
            return;
        }
        if (event->key() == Qt::Key_Backspace && textCursor().atBlockStart() &&
            own.previous().isValid() &&
            !imageRectInViewport(own.previous()).isEmpty() &&
            runOperation(deleteImageLineBackward))
            return;
        if (event->key() == Qt::Key_Delete && own.next().isValid() &&
            !imageRectInViewport(own.next()).isEmpty() &&
            runOperation(deleteImageLineForward))
            return;

        // Пустая строка, отделяющая фотографию от непустого текста, неудаляема:
        // её гибель склеила бы текст со скрытой подписью, и фото рассыпалось бы
        // в огрызок разметки. Отказ и шаг, как у черты: каретка встаёт на
        // фотографию (выбирает её), следующее нажатие убирает её целиком.
        if (event->key() == Qt::Key_Backspace && textCursor().atBlockStart() &&
            own.previous().isValid() && isVSpaceBlock(own.previous())) {
            const QTextBlock photo = own.previous().previous();
            if (photo.isValid() && !imageRectInViewport(photo).isEmpty() &&
                blocksWouldMerge(photo, own)) {
                setTextCursor(QTextCursor(photo));
                return;
            }
        }
        if (event->key() == Qt::Key_Delete &&
            textCursor().position() == own.position() + own.length() - 1 &&
            own.next().isValid() && isVSpaceBlock(own.next())) {
            const QTextBlock photo = own.next().next();
            if (photo.isValid() && !imageRectInViewport(photo).isEmpty() &&
                blocksWouldMerge(own, photo)) {
                setTextCursor(QTextCursor(photo));
                return;
            }
        }

        // ТО ЖЕ САМОЕ, но когда каретка стоит НА САМОЙ пустой строке. Прежнее
        // правило смотрело только с текстовой строки, и три подхода из четырёх
        // проходили мимо него: Delete сверху, Backspace и Delete снизу склеивали
        // пустую строку с фотографией и рассыпали её в огрызок разметки.
        // Владелец нашёл один из них, набор — остальные.
        if (isVSpaceBlock(own)) {
            const bool back = event->key() == Qt::Key_Backspace;
            const bool forward = event->key() == Qt::Key_Delete;
            const QTextBlock photo = back ? own.previous() : own.next();
            // Спрашиваем МОДЕЛЬ, а не вид: imageRectInViewport отвечает только
            // про то, что уже разложено на экране, и в наборе без окна молчал.
            // Правило же не про показ, а про то, чем блок является.
            if ((back || forward) && photo.isValid() && blockImageRef(photo).valid) {
                // Отказ и шаг, как у черты: каретка встаёт на фотографию —
                // выбирает её, — и следующее нажатие убирает её целиком.
                setTextCursor(QTextCursor(photo));
                return;
            }
            // Пустая строка ПОД фотографией: убрать её нельзя и в другую
            // сторону — текст снизу поднялся бы к фотографии вплотную и слился
            // бы с ней при первом же чтении файла.
            const QTextBlock above = own.previous();
            if (forward && above.isValid() && blockImageRef(above).valid &&
                own.next().isValid() && blocksWouldMerge(above, own.next())) {
                setTextCursor(QTextCursor(above));
                return;
            }
        }
    }

    if (event->key() == Qt::Key_Backspace && event->modifiers() == Qt::NoModifier) {
        // Жест снятия комментарности: как у первого пункта списка — сначала
        // блок становится абзацем, и только следующее нажатие сливает.
        if (runOperation(uncommentAtBlockStart)) return;
        if (runOperation(unwrapListItemAtCursor)) return;
        // Черта прямо над кареткой удаляется — это удаление назад: гибнет то,
        // что НАД кареткой, а своя строка остаётся под ней. Поэтому раньше
        // обработки пустых строк: с пустой строки под чертой Backspace убирает
        // черту, а не пустую.
        if (runOperation(deleteDividerAbove)) return;
        if (runOperation(joinAcrossVSpaceBackward)) return;
        // Каретка на самой черте, выше непустой текст: по плоской модели слева
        // от каретки стоит перевод строки, но удалить его нельзя — черта не
        // живёт в строке текста. Отказ, каретка шагает в конец строки выше.
        // Сама черта под кареткой — дело Delete, Backspace удаляет слева.
        const QTextBlock atBlock = textCursor().block();
        if (!textCursor().hasSelection() && !isRawBlock(atBlock) &&
            kindOf(atBlock) == Kind::Divider) {
            const QTextBlock prev = atBlock.previous();
            if (prev.isValid()) {
                QTextCursor up = textCursor();
                up.setPosition(prev.position() + prev.length() - 1);
                setTextCursor(up);
            }
            return;
        }
    }
    // Delete у пустой строки — то же самое с другой стороны: строка исчезает, а
    // соседи, которым markdown не даёт стоять раздельно, сливаются.
    if (event->key() == Qt::Key_Delete && event->modifiers() == Qt::NoModifier &&
        runOperation(joinAcrossVSpaceForward))
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

    // Автозамены. Это не операция над блоками, а тот же набор, только знаком,
    // которого нет на клавиатуре: идёт обычной вставкой, слипается в один шаг
    // истории с соседними буквами и работает в дословных кусках наравне с
    // остальным текстом.
    for (const auto& [keys, text] : specialKeys_) {
        if (!pressed(keys)) continue;
        textCursor().insertText(text);
        keepCaretOffEdge();
        return;
    }

    // Начертание без выделения — не правка документа, а формат для следующей
    // буквы. Отдельный путь: шага истории здесь нет и быть не должно.
    for (const auto& [keys, style] : inlineBindings_) {
        if (!pressed(keys)) continue;
        if (runOperation(style.op)) return;
        // В блоке кода и в дословном куске текст буквальный — начертанию там
        // взяться неоткуда, как и при выделении.
        const QTextBlock block = textCursor().block();
        if (!textCursor().hasSelection() && !isRawBlock(block) && kindOf(block) != Kind::Code)
            setCurrentCharFormat(
                inlineStyleForTyping(*document(), block, currentCharFormat(), style.bits));
        return;
    }

    for (const auto& [keys, op] : bindings_)
        if (pressed(keys) && runOperation(op)) return;

    // Tab в списке молчит, даже когда отступать некуда: первый пункт отступать
    // не к чему, но и табуляцию в его текст ставить незачем. Без этого отказ
    // операции проваливался в QTextEdit, и в пункте появлялся знак табуляции.
    if ((event->key() == Qt::Key_Tab || event->key() == Qt::Key_Backtab) &&
        (isListBlock(textCursor().block()) || levelOf(textCursor().block()) >= 0))
        return;

    // Шаг вверх-вниз между блоками с разными полями: курсор должен остаться на
    // той же колонке, а не уехать на ширину маркера.
    if (event->key() == Qt::Key_Down || event->key() == Qt::Key_Up) {
        keepColumnAcrossMargins(event);
        keepCaretOffEdge();
        return;
    }

    // "---" и пробел — черта. Правило срабатывает ДО вставки пробела: тогда
    // пробел не попадает в шаг истории, и Ctrl+Z возвращает голые дефисы с
    // кареткой сразу за ними, без хвоста. Enter-путь устроен так же — выше.
    if (event->text() == QStringLiteral(" ") && runOperation(applyDividerRuleAtCursor))
        return;

    // Перед самим набором: у правого края ссылки набранное не должно уезжать
    // внутрь неё.
    if (!event->text().isEmpty() && event->text().at(0).isPrint()) dropLinkAtRightEdge();

    NoteView::keyPressEvent(event);
    // Курсор мог уехать к самой кромке — и набор, и перемещение по странице:
    // держим зазор.
    keepCaretOffEdge();

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

    // Ctrl+Shift+I — добавить изображения. Из свободных сочетаний это
    // единственное с говорящей буквой: Ctrl+I занят курсивом.
    if (event->key() == Qt::Key_I &&
        event->modifiers() == (Qt::ControlModifier | Qt::ShiftModifier)) {
        chooseAndInsertImages();
        return;
    }

    // Автозамена срабатывает по пробелу и уже после того, как он набран: правило
    // смотрит на то, что человек написал. Отдельным шагом истории — первый
    // Ctrl+Z обязан вернуть набранные знаки, а не отменить предыдущую правку.
    if (event->text() == QStringLiteral(" ")) runOperation(applyInputRuleAtCursor);
    // Закрывающая кавычка превращает набранное в ней во встроенный код. После
    // этого курсор стоит в конце размеченного куска, и без сброса формата набор
    // продолжался бы кодом — вышло бы `код и всё, что дальше`.
    if (event->text() == QStringLiteral("`") && runOperation(applyCodeSpanRuleAtCursor))
        setCurrentCharFormat(textCursor().block().charFormat());
}

bool NoteEditor::runOperation(bool (*op)(QTextDocument&, QTextCursor&)) {
    return runOperation(std::function<bool(QTextDocument&, QTextCursor&)>(op));
}

bool NoteEditor::runOperation(const std::function<bool(QTextDocument&, QTextCursor&)>& op) {
    // Набранное до операции обязано остаться отдельным шагом: Ctrl+Z после
    // правила черты возвращает голые дефисы, а не съедает их вместе с чертой.
    // Снимок только один на серию, так что платим за него не чаще раза.
    flushPendingEdit();
    QTextCursor cursor = textCursor();
    const int scrollBefore = verticalScrollBar()->value();
    // Шаг истории у операции свой; правки, которые она делает по дороге, в
    // историю попадать не должны — иначе одно нажатие даст два шага.
    recordingSuspended_ = true;
    const bool handled = op(*document(), cursor);
    if (!handled) {
        recordingSuspended_ = false;
        return false;
    }
    // Подметание — внутри транзакции операции, до снимка истории: слияния
    // внутри операций тоже оставляют хвостовые пробелы, а через
    // contentsChanged они не проходят.
    tidySweep(cursor);
    recordingSuspended_ = false;

    // Операция трогает содержимое, род и уровень; всё оформление, которое из
    // них следует, пересчитывает сборщик — так ни одно свойство не отстанет.
    // Разбивка на блоки после нормализации уже каноническая, поэтому место
    // курсора переживает пересборку.
    const int anchor = cursor.anchor();
    const int position = cursor.position();
    Document ir = readDocument(*document());
    note_.undoChain.push(ir, position);
    // Операция — отдельный шаг: следующая набранная буква к ней не приклеится.
    note_.typingRun = false;

    rebuild(ir, position, viewAnchor(), &ir);
    // Выделение возвращаем: операция могла тронуть десяток пунктов сразу, и
    // терять его после этого — значит заставлять выделять заново. Текст от
    // смены рода не меняется, поэтому обе границы остаются на своих местах.
    if (anchor != position) {
        const int last = document()->characterCount() - 1;
        QTextCursor restored(document());
        restored.setPosition(qBound(0, anchor, last));
        restored.setPosition(qBound(0, position, last), QTextCursor::KeepAnchor);
        setTextCursor(restored);
    }
    document()->setModified(true);
    showEditPlace(scrollBefore);
    autosave_.start(appearance().autosaveDelayMs);
    return true;
}

void NoteEditor::contextMenuEvent(QContextMenuEvent* event) {
    // Щелчок правой кнопкой вне выделения переносит курсор туда: иначе команда
    // применилась бы не к тому месту, на которое человек показал.
    if (!textCursor().hasSelection()) setTextCursor(cursorForPosition(event->pos()));

    QMenu* menu = createStandardContextMenu(event->pos());
    if (menu == nullptr) return;
    menu->setAttribute(Qt::WA_DeleteOnClose);

    const auto add = [this, menu](const QString& title, const QString& keys,
                                  bool (*op)(QTextDocument&, QTextCursor&)) {
        QAction* action = menu->addAction(title, this, [this, op] { runOperation(op); });
        const QList<QKeySequence> all =
            QKeySequence::listFromString(keys, QKeySequence::PortableText);
        if (!all.isEmpty()) action->setShortcut(all.first());
    };

    menu->addSeparator();
    {
        QAction* action = menu->addAction(QStringLiteral("Добавить изображения…"), this,
                                          &NoteEditor::chooseAndInsertImages);
        action->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+I")));
    }

    menu->addSeparator();
    add(QStringLiteral("Жирный"), QStringLiteral("Ctrl+B"), toggleBold);
    add(QStringLiteral("Курсив"), QStringLiteral("Ctrl+I"), toggleItalic);
    add(QStringLiteral("Зачёркнутый"), QStringLiteral("Ctrl+K"), toggleStrike);
    add(QStringLiteral("Код в строке"), QStringLiteral("Ctrl+E"), toggleCode);
    {
        QAction* action = menu->addAction(QStringLiteral("Блок кода"));
        action->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+E")));
        connect(action, &QAction::triggered, this,
                [this] { applyIrEdit(toggleCodeBlock(*document(), textCursor())); });
    }

    // Уровень заголовка. Подменю, а не семь пунктов вперемешку с прочим:
    // строка «Заголовок» в меню — это одна мысль, а какого он ранга — уточнение.
    {
        menu->addSeparator();
        QMenu* heading = menu->addMenu(QStringLiteral("Заголовок"));
        const int now = kindOf(textCursor().block()) == Kind::Heading
                            ? textCursor().blockFormat().headingLevel()
                            : 0;
        const auto addLevel = [this, heading, now](const QString& title, int level) {
            QAction* action = heading->addAction(title, this, [this, level] {
                runOperation([level](QTextDocument& doc, QTextCursor& at) {
                    return setHeadingLevel(doc, at, level);
                });
            });
            // Отметка показывает, что стоит сейчас: без неё непонятно, какой
            // ранг у строки, на которой стоишь.
            action->setCheckable(true);
            action->setChecked(now == level);
            action->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+%1").arg(level)));
        };
        addLevel(QStringLiteral("Обычный текст"), 0);
        for (int level = 1; level <= 3; ++level)
            addLevel(QStringLiteral("Уровень %1").arg(level), level);
        heading->addSeparator();
        for (int level = 4; level <= 6; ++level)
            addLevel(QStringLiteral("Уровень %1").arg(level), level);
    }

    // Выравнивание — только на строке с фотографией: где картинки нет, пункт
    // ничего не значит и в меню ему делать нечего.
    if (blockImageRef(textCursor().block()).valid) {
        menu->addSeparator();
        QMenu* align = menu->addMenu(QStringLiteral("Выровнять фотографию"));
        const BlockImageRef ref = blockImageRef(textCursor().block());
        const auto addAlign = [this, align, ref](const QString& title, ImageAlign to) {
            QAction* action = align->addAction(title, this, [this, to] {
                runOperation([to](QTextDocument& doc, QTextCursor& at) {
                    return setImageAlignAtCursor(doc, at, to);
                });
            });
            // Отметка показывает, что стоит сейчас; по центру — и когда в файле
            // не написано ничего.
            action->setCheckable(true);
            action->setChecked(ref.align == to);
        };
        addAlign(QStringLiteral("Слева"), ImageAlign::Left);
        addAlign(QStringLiteral("По центру"), ImageAlign::Center);
        addAlign(QStringLiteral("Справа"), ImageAlign::Right);
    }

    menu->addSeparator();
    add(QStringLiteral("Переключить задачу"), appearance().toggleTaskKey,
        toggleTaskAtCursor);

    QMenu* kinds = menu->addMenu(QStringLiteral("Сделать"));
    const auto addKind = [this, kinds](const QString& title, const QString& keys,
                                       bool (*op)(QTextDocument&, QTextCursor&)) {
        QAction* action = kinds->addAction(title, this, [this, op] { runOperation(op); });
        const QList<QKeySequence> all =
            QKeySequence::listFromString(keys, QKeySequence::PortableText);
        if (!all.isEmpty()) action->setShortcut(all.first());
    };
    addKind(QStringLiteral("Маркированным списком"), appearance().makeBulletKey, makeBullet);
    addKind(QStringLiteral("Нумерованным списком"), appearance().makeOrderedKey, makeOrdered);
    addKind(QStringLiteral("Списком задач"), appearance().makeTaskKey, makeTask);
    addKind(QStringLiteral("Комментарием"), appearance().makeCommentKey,
            toggleCommentAtCursor);
    addKind(QStringLiteral("Обычным текстом"), appearance().makeParagraphKey, makeParagraph);

    menu->addSeparator();
    add(QStringLiteral("Сдвинуть вправо"), QStringLiteral("Tab"), indentListItems);
    add(QStringLiteral("Сдвинуть влево"), QStringLiteral("Shift+Tab"), outdentListItems);

    QAction* up = menu->addAction(QStringLiteral("Переставить вверх"), this,
                                  [this] { moveItem(-1); });
    up->setShortcut(QKeySequence(appearance().moveUpKey, QKeySequence::PortableText));
    QAction* down = menu->addAction(QStringLiteral("Переставить вниз"), this,
                                    [this] { moveItem(1); });
    down->setShortcut(QKeySequence(appearance().moveDownKey, QKeySequence::PortableText));

    // Внешний редактор — команда окна, а не редактора: запускать процессы
    // виджету текста не по чину. Пункт здесь, потому что искать его человек
    // будет там, где смотрит на заметку.
    menu->addSeparator();
    menu->addAction(QStringLiteral("Открыть во внешнем редакторе"), this,
                    [this] { emit externalEditorRequested(note_.path); });
    // Вывоз — тоже команда окна: диалог сохранения и запись PDF виджету текста
    // не по чину, да и заметку перед вывозом надо сперва записать.
    menu->addAction(QStringLiteral("Экспортировать…"), this,
                    [this] { emit exportRequested(note_.path); });

    menu->popup(event->globalPos());
}

QMimeData* NoteEditor::createMimeDataFromSelection() const {
    QMimeData* data = new QMimeData;
    data->setText(selectionToMarkdown(textCursor()));
    return data;
}

bool NoteEditor::canInsertFromMimeData(const QMimeData* source) const {
    if (source == nullptr) return false;
    // Битмап и файлы принимаем наравне с текстом — на этом же ответе стоит и
    // drag&drop: Qt спрашивает разрешения именно здесь, а бросив «нет»,
    // перетаскивание молча не сработало бы.
    return source->hasText() || source->hasImage() || source->hasUrls();
}

void NoteEditor::insertFromMimeData(const QMimeData* source) {
    if (source == nullptr) return;

    // ПОРЯДОК ВАЖЕН. Файловые менеджеры кладут в буфер и путь текстом, и список
    // ссылок; браузеры — и картинку, и её адрес. Спрашиваем сначала о том, что
    // человек скорее всего имел в виду, а текст оставляем на потом.
    if (source->hasUrls()) {
        QStringList files;
        for (const QUrl& url : source->urls())
            if (url.isLocalFile()) files << url.toLocalFile();
        // Из принесённого берём только то, что вообще читается как картинка:
        // перетащенный pdf должен вставиться ссылкой, а не притвориться фото.
        QStringList images;
        for (const QString& f : files)
            if (!QImageReader(f).format().isEmpty()) images << f;
        if (!images.isEmpty()) {
            insertImageFiles(images);
            return;
        }
    }
    if (source->hasImage()) {
        const QImage image = qvariant_cast<QImage>(source->imageData());
        if (!image.isNull() && insertImagePixels(image)) return;
    }
    if (!source->hasText()) return;
    // В литеральный блок markdown не вставляется: там текст буквальный, и разбор
    // превратил бы вставленное в разметку, которой в коде взяться неоткуда.
    const QTextBlock block = textCursor().block();
    pasteMarkdown(source->text(), isRawBlock(block) || kindOf(block) == Kind::Code);
}

void NoteEditor::pasteMarkdown(const QString& text, bool literal) {
    // Вставка — свой шаг истории, и набранное до неё обязано остаться своим.
    flushPendingEdit();
    const int scrollBefore = verticalScrollBar()->value();
    if (text.isEmpty()) return;
    const QByteArray utf8 = text.toUtf8();
    const std::string source(utf8.constData(), size_t(utf8.size()));

    Document fragment;
    std::vector<Block>& pieces = fragment.blocks;
    if (literal) {
        // Один абзац с текстом как есть: переводы строк внутри блока сборщик
        // разметит сам, и они вернутся переводами, а не разметкой.
        std::string body = source;
        while (!body.empty() && body.back() == '\n') body.pop_back();
        pieces.push_back(fragment.newBlock(Kind::Paragraph, body));
    } else {
        // Полным парсером ядра, а не вторым упрощённым разбором: их
        // идемпотентность и гарантирует, что скопированное вставится без потерь.
        fragment = parse(source);
    }
    if (pieces.empty()) return;

    QTextDocument staging;
    buildDocument(fragment, staging, zoom());

    // Кусок из одного обычного абзаца вставляется в строку: скопированные слова
    // должны войти в тот блок, куда их кладут. Всё прочее — заголовок, пункт,
    // код, цитата, да и просто несколько блоков — вставляется своими блоками:
    // род блока это его свойство, и терять его при переносе нельзя.
    const Block& head = pieces.front();
    // Фотография — тоже блочная вещь: абзац из одного image-спана целиком и
    // вики-вложение "![[...]]" встают своей строкой, а не вклеиваются в текст
    // (в середине текста фотография не показывается — вклейка её потеряла бы).
    const std::string_view headText = fragment.text(head);
    const std::span<const Inline> headSpans = fragment.inlines(head);
    const bool wholeImage =
        !head.raw && head.kind == Kind::Paragraph &&
        ((headSpans.size() == 1 && headSpans[0].image() && headSpans[0].text.start == 0 &&
          size_t(headSpans[0].text.end) == headText.size()) ||
         (headText.rfind("![[", 0) == 0 && headText.size() > 5 &&
          headText.compare(headText.size() - 2, 2, "]]") == 0));
    const bool blockLevel = pieces.size() > 1 || head.raw ||
                            head.kind != Kind::Paragraph || wholeImage;

    recordingSuspended_ = true;
    QTextCursor cursor = textCursor();
    cursor.beginEditBlock();
    if (cursor.hasSelection()) cursor.removeSelectedText();

    // Qt вливает первый блок куска в текущий блок, и формат берётся у текущего:
    // вставленный заголовок становился обычным текстом, а вставка в начало
    // абзаца, наоборот, делала заголовком сам абзац. Поэтому под блочный кусок
    // заводим пустой блок и потом ставим ему формат первого блока куска.
    if (blockLevel && !cursor.block().text().isEmpty()) {
        QTextBlockFormat plain;
        plain.setLineHeight(cursor.blockFormat().lineHeight(),
                            cursor.blockFormat().lineHeightType());
        if (cursor.atBlockStart()) {
            // Пустой блок заводим НАД текущим и встаём в него: текст блока
            // уезжает вниз целиком и остаётся собой.
            QTextCursor tail(document());
            tail.setPosition(cursor.position());
            cursor.insertBlock(cursor.blockFormat());
            cursor.setPosition(tail.block().previous().position());
        } else {
            const bool wasAtEnd = cursor.atBlockEnd();
            cursor.insertBlock(plain);
            if (!wasAtEnd) {
                // Резали посередине: хвост уехал вниз, а вставлять надо между
                // половинками — заводим ещё один пустой блок и встаём в него.
                QTextCursor tail(document());
                tail.setPosition(cursor.position());
                cursor.insertBlock(plain);
                cursor.setPosition(tail.block().previous().position());
            }
        }
    }

    const QTextBlockFormat headFormat = staging.firstBlock().blockFormat();
    const int start = cursor.blockNumber();
    cursor.insertFragment(QTextDocumentFragment(&staging));
    const int landed = cursor.position();
    // Формат первого блока куска Qt не донёс — ставим сами.
    if (blockLevel) {
        QTextCursor fix(document());
        fix.setPosition(document()->findBlockByNumber(start).position());
        fix.setBlockFormat(headFormat);
    }
    // Вставленное могло приехать из другого места дерева: шов приводим в
    // порядок целиком, документ для этого достаточно мал.
    syncLiteralBlocks(*document(), {0, document()->blockCount() - 1});
    syncLists(*document(), {0, document()->blockCount() - 1});
    cursor.endEditBlock();
    recordingSuspended_ = false;

    Document ir = readDocument(*document());
    note_.undoChain.push(ir, landed);
    note_.typingRun = false;
    rebuild(ir, landed, viewAnchor(), &ir);
    document()->setModified(true);
    showEditPlace(scrollBefore);
    autosave_.start(appearance().autosaveDelayMs);
}

QString NoteEditor::attachmentDir() const {
    if (note_.path.isEmpty()) return {};
    return QFileInfo(note_.path).absolutePath();
}

int NoteEditor::insertImageFiles(const QStringList& paths) {
    if (paths.isEmpty()) return 0;
    const QString dir = attachmentDir();
    if (dir.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("zametti"),
                             QStringLiteral("Заметка ещё не сохранена — вложению некуда лечь."));
        return 0;
    }

    if (!beginImport(int(paths.size()))) return 0;
    importer_->importFiles(paths, dir);
    return int(paths.size());
}

// Общая подготовка пачки: работник, окно прогресса и замок правки. Одна на оба
// входа (файлы и буфер обмена) — разъехаться им негде.
bool NoteEditor::beginImport(int count) {
    if (importer_ != nullptr && importer_->busy()) {
        // Второй пачки разом не бывает: поток один, и очередь в нём — не то,
        // чего человек ждёт от «вставить ещё раз». Говорим прямо.
        QMessageBox::information(this, QStringLiteral("zametti"),
                                 QStringLiteral("Предыдущие картинки ещё везутся."));
        return false;
    }
    if (importer_ == nullptr) {
        importer_ = new ImageImporter(this);
        connect(importer_, &ImageImporter::imported, this,
                [this](const ImportedImage& one) { importedBatch_.push_back(one); });
        connect(importer_, &ImageImporter::finished, this, &NoteEditor::onImportFinished);
        connect(importer_, &ImageImporter::progress, this,
                [this](int done, int total, const QString& name) {
                    if (importProgress_ != nullptr) {
                        importProgress_->setMaximum(total);
                        importProgress_->setValue(done);
                        if (!name.isEmpty()) importProgress_->setLabelText(name);
                    }
                    // И в полосе сведений — там человек и ищет ответ на «почему
                    // не печатается». Числа рядом: без них «везу» не говорит,
                    // сколько ещё ждать.
                    if (!name.isEmpty())
                        emit importStatus(QStringLiteral("режим чтения: импортируем %1 (%2 из %3)")
                                              .arg(name)
                                              .arg(done + 1)
                                              .arg(total));
                });
    }

    importedBatch_.clear();
    // Окно прогресса неблокирующее: цикл событий крутится, окно перерисовывается,
    // и «программа не отвечает» больше неоткуда взяться.
    delete importProgress_;
    importProgress_ = new QProgressDialog(QStringLiteral("Ввоз картинок…"),
                                          QStringLiteral("Отмена"), 0, count, this);
    importProgress_->setWindowModality(Qt::NonModal);
    importProgress_->setAutoClose(false);
    importProgress_->setAutoReset(false);
    // Не выскакивать на мелочи: пачка из одного маленького png успевает
    // кончиться раньше, чем человек заметит окно.
    importProgress_->setMinimumDuration(400);
    connect(importProgress_, &QProgressDialog::canceled, importer_, &ImageImporter::cancel);

    // Замок: пока везём, править нельзя ничего. Редактор становится читалкой
    // (это перекрывает и набор, и вставку, и операции клавишами), а дерево со
    // списком гасит окно. Отдельного флага нет — editingAllowed спрашивает
    // сам импортёр.
    wasEditableBeforeImport_ = !isReadOnly();
    setReadOnly(true);

    return true;
}

void NoteEditor::onImportFinished(int done, int total, bool cancelled) {
    Q_UNUSED(done);
    Q_UNUSED(total);
    // Замок снимается сам: работник уже опустил busy, и editingAllowed это
    // видит. Здесь только возвращаем редактору правимость.
    if (wasEditableBeforeImport_) setReadOnly(false);
    emit importStatus(QString());
    if (importProgress_ != nullptr) {
        importProgress_->reset();
        importProgress_->deleteLater();
        importProgress_ = nullptr;
    }

    // Порядок вставки — порядок ВЫБОРА файлов, а не готовности.
    std::sort(importedBatch_.begin(), importedBatch_.end(),
              [](const ImportedImage& a, const ImportedImage& b) { return a.index < b.index; });
    QStringList pieces;
    QStringList failures;
    for (const ImportedImage& one : importedBatch_) {
        if (one.stored.ok()) pieces << imageMarkdown(one.stored);
        else
            failures << QStringLiteral("%1: %2").arg(QFileInfo(one.source).fileName(),
                                                     one.stored.error);
    }
    importedBatch_.clear();
    Q_UNUSED(cancelled);

    // ОДНОЙ ВСТАВКОЙ, а не в цикле: тогда это один шаг истории, и Ctrl+Z
    // убирает всё разом (инвариант C брифа).
    //
    // РАЗДЕЛИТЕЛЬ — ПУСТАЯ СТРОКА, и это требование владельца, а не оформление:
    // без неё, чтобы написать что-то между двумя картинками, пришлось бы
    // вставать вплотную к ним, и одно неверное движение стёрло бы картинку.
    // Проверено разбором: "![a](1)\n\n![b](2)" даёт три блока — картинка,
    // VSpace, картинка, — то есть пустая строка выживает как настоящий блок, в
    // который можно встать. Собирать её из "\n" было бы ошибкой: подряд идущие
    // строки markdown слил бы в один абзац, и вторая картинка пропала бы.
    if (!pieces.isEmpty()) {
        // Пустая строка ПОСЛЕ последней картинки — не украшение: в саму
        // картинку каретка не пускается, и без соседа за ней в заметку
        // нечего было бы дописать вовсе. Владелец на это и наткнулся.
        pasteMarkdown(pieces.join(QStringLiteral("\n\n")) + QStringLiteral("\n\n"), false);
    }

    // О неудачах говорим ПОСЛЕ вставки удачных: молчаливый пропуск — худшее из
    // возможного, а прерывать всю пачку из-за одного битого файла незачем.
    if (!failures.isEmpty())
        QMessageBox::warning(this, QStringLiteral("zametti"),
                             QStringLiteral("Не вставилось:\n") +
                                 failures.join(QLatin1Char('\n')));
}

bool NoteEditor::insertImagePixels(const QImage& image) {
    if (image.isNull()) return false;
    const QString dir = attachmentDir();
    if (dir.isEmpty()) return false;

    // Тем же путём, что и файлы: снимок экрана из буфера бывает и на
    // двадцать мегапикселей, и замирало на нём точно так же.
    if (!beginImport(1)) return false;
    importer_->importPixels(image, dir);
    return true;
}

void NoteEditor::chooseAndInsertImages() {
    // Заметку на диск ДО ввоза (решение владельца). Ввоз идёт фоном и надолго
    // запирает правку; если программа умрёт в это время, набранное до сих пор
    // должно уже лежать в файле, а не в памяти.
    save(false);
    if (attachmentDir().isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("zametti"),
                             QStringLiteral("Заметка ещё не сохранена — вложению некуда лечь."));
        return;
    }
    // Фильтр строим из того, что читатели УМЕЮТ на этой машине, а не из
    // списка в коде: без libheif heic не прочтётся, и предлагать его было бы
    // обманом.
    QStringList patterns;
    for (const QByteArray& fmt : QImageReader::supportedImageFormats())
        patterns << QStringLiteral("*.") + QString::fromLatin1(fmt);
    patterns.sort();
    const QString filter = QStringLiteral("Картинки (%1);;Все файлы (*)")
                               .arg(patterns.join(QLatin1Char(' ')));

    const QStringList chosen = QFileDialog::getOpenFileNames(
        this, QStringLiteral("Добавить изображения"), lastImageDir_, filter);
    if (chosen.isEmpty()) return;
    // Следующий раз открываемся там же: складывать картинки обычно приходится
    // из одного каталога, и заставлять человека ходить туда заново невежливо.
    lastImageDir_ = QFileInfo(chosen.first()).absolutePath();
    insertImageFiles(chosen);
}

bool NoteEditor::moveItem(int direction) {
    return applyIrEdit(moveListItem(*document(), textCursor(), direction));
}

bool NoteEditor::applyIrEdit(const MoveResult& moved) {
    flushPendingEdit();
    if (!moved.done) return false;
    const int scrollBefore = verticalScrollBar()->value();

    note_.undoChain.push(moved.doc, textCursor().position());
    note_.typingRun = false;
    rebuild(moved.doc, 0, viewAnchor());

    // Курсор ставим по месту в IR: после перестановки или слияния блоков прежняя
    // позиция в тексте указывала бы на чужое место. Вид при этом уже наведён
    // пересборкой, и трогаем его только если курсор из него выпал.
    const QTextBlock landed = blockForIrIndex(*document(), moved.irBlock);
    if (landed.isValid()) {
        QTextCursor place(document());
        place.setPosition(landed.position() +
                          qMin(moved.offsetInBlock, landed.length() - 1));
        setTextCursor(place);
    }
    document()->setModified(true);
    showEditPlace(scrollBefore);
    autosave_.start(appearance().autosaveDelayMs);
    return true;
}

// Пересчёт слов и строк. Зовётся из двух мест — полной сборки и записи на
// диск, — и оба они и без него стоят миллисекунды.
void NoteEditor::refreshStats(const Document& ir) { refreshStats(irStats(ir)); }

void NoteEditor::refreshStats(const NoteStats& stats) {
    note_.stats = stats;
    note_.statsFresh = true;
    emit statsChanged();
}

void NoteEditor::onContentsChanged() {
    // Пересборка и перекладка полей под ширину окна — это облик. Документу они
    // неотличимы от правки текста, и без этих двух признаков ширина окна
    // заводила бы шаг истории.
    if (recordingSuspended_ || changingLayout()) return;

    // Набор — единственная правка мимо операций, и он умеет ломать инвариант
    // пустых строк: текстом на пустой строке и выделением, съевшим границу
    // блоков. Чиним здесь, а не в обработчике клавиш: текст приходит и мимо
    // него — из системы ввода, из вставки, из подстановки.
    //
    // Починка меняет документ и вызывает этот обработчик заново, но признак
    // операции уже поднят, и второй заход сразу возвращается.
    QTextCursor cursor = textCursor();
    recordingSuspended_ = true;
    const bool repaired = repairAfterTyping(*document(), cursor);
    recordingSuspended_ = false;
    if (repaired) setTextCursor(cursor);

    // Правка любого вида могла оставить хвостовые пробелы на строках, где
    // каретки нет, — выделение с удалением, вставка, слияние. Инвариант
    // владельца: таких строк не существует. Чистим после каждой правки.
    tidySweep(textCursor());

    note_.undoRun = false;   // настоящая правка — серия отмены кончилась
    // Числа отстали от документа. Сам пересчёт будет на ближайшем
    // автосохранении: на нажатие клавиши статистику не считаем.
    if (note_.statsFresh) {
        note_.statsFresh = false;
        emit statsChanged();
    }
    recordEdit();
    autosave_.start(appearance().autosaveDelayMs);
}

// Правка есть — снимка пока нет. Читать документ целиком на каждую букву
// незачем: набор подряд всё равно склеивается в один шаг, и все промежуточные
// снимки этого шага выбрасываются. Ждём конца серии.
void NoteEditor::recordEdit() {
    const int at = textCursor().position();

    // СЕРИЯ РВЁТСЯ НЕ ТОЛЬКО ТИШИНОЙ. Пока человек печатает ровно, паузы в
    // 700 мс не случается вовсе, и весь набор ложился одним шагом: два Ctrl+Z —
    // и редактор уходил в историю, к чужому слепку. Владелец сказал про это
    // «буфера на 200 шагов как будто нет», и замер подтвердил: после 150
    // правок глубина цепочки была единица.
    //
    // Шаг — СЛОВО, как в Apple Notes. Границы три: набранный разделитель
    // (он уходит в тот же шаг, следующая буква начинает новый), уход каретки в
    // другое место и потолок в undoRunChars знаков — на случай слова, которое
    // никак не кончается.
    const int start = note_.changeStart >= 0 ? note_.changeStart : at;
    const bool near = note_.runCursor < 0 || qAbs(start - note_.runCursor) <= 1;
    if (!near || note_.runChars >= qMax(1, appearance().undoRunChars)) {
        // Признак серии гасим ВСЕГДА, а не только когда есть что сбрасывать.
        // flushPendingEdit оставляет его поднятым — и после сохранения, которое
        // сбросило накопленное, следующая правка снова дописывалась в тот же
        // шаг. Так вся сессия и оказывалась одним шагом.
        if (note_.pendingEdit) flushPendingEdit();
        note_.typingRun = false;
        note_.runChars = 0;
    }

    ++note_.runChars;
    note_.runCursor = note_.changeEnd >= 0 ? note_.changeEnd : at;
    note_.pendingCursor = at;
    note_.pendingEdit = true;

    // Разделитель закрывает шаг ПРЯМО СЕЙЧАС, а не при следующей правке.
    // Иначе снимок брался бы уже после первой буквы нового слова, и отмена
    // возвращала бы «один два три ч» вместо «один два три ».
    if (note_.changeSeparator) {
        flushPendingEdit();
        note_.typingRun = false;
        note_.runChars = 0;
        return;
    }
    snapshot_.start(appearance().undoCoalesceMs);
}

void NoteEditor::flushPendingEdit() {
    if (!note_.pendingEdit) return;
    note_.pendingEdit = false;
    snapshot_.stop();
    // Набор подряд — один шаг: иначе Ctrl+Z возвращал бы по одной букве. Серия
    // кончается тишиной (сработал таймер) или чем угодно, что заводит
    // собственный шаг, — те сами гасят признак.
    Document doc = readDocument(*document());
    if (note_.typingRun) note_.undoChain.amend(std::move(doc), note_.pendingCursor);
    else note_.undoChain.push(std::move(doc), note_.pendingCursor);
    note_.typingRun = true;
}

void NoteEditor::forgetPendingEdit() {
    note_.undoRun = false;   // другой файл — своя серия
    note_.pendingEdit = false;
    snapshot_.stop();
    note_.typingRun = false;
    note_.runChars = 0;
    note_.runCursor = -1;
    note_.changeStart = -1;
    note_.changeEnd = -1;
    note_.changeSeparator = false;
}

void NoteEditor::editMeta(const std::function<void(NoteMeta&)>& change) {
    note_.meta.present = true;
    change(note_.meta);
    // Правка одной меты не трогает modified: перенос, корзина и
    // восстановление — не редактирование содержимого, и всплывать наверх
    // списка заметка от них не должна (правило владельца). Несохранённые
    // правки текста, подобранные этой же записью, штамп заслужили.
    stampModifiedOnSave_ = document()->isModified();
    document()->setModified(true);
    save(false);
    stampModifiedOnSave_ = true;
}

void NoteEditor::setMetaParent(const QString& parentId) {
    editMeta([&parentId](NoteMeta& meta) {
        if (parentId.isEmpty()) meta.unset("parent");
        else meta.set("parent", parentId.toStdString());
    });
}

// --- режим истории ----------------------------------------------------------

bool NoteEditor::enterHistory(int index) {
    // Уже в режиме — значит просят другой слепок (щёлкнули в таймлайне).
    // Отдельного метода на это не заводим: снаружи это одно и то же желание —
    // «покажи вот эту запись».
    if (inHistory()) return index >= 0 && showSnapshot(index);
    if (storeRoot_.isEmpty() || note_.path.isEmpty()) return false;

    // Незаписанные правки — в файл, а значит и в журнал: человек пошёл смотреть
    // прошлое, и вершина цепочки обязана в этом прошлом оказаться. Иначе он
    // вернётся к живой версии, уйдёт из заметки — и то, что набрал, пропадёт
    // из истории навсегда.
    flushPendingEdit();
    if (document()->isModified()) save(false);

    journal::History history(storeRoot_);
    QString error;
    // Читаем в местную переменную: объект заметки сейчас уедет целиком, и
    // положенное в него до этого уехало бы вместе с ним.
    journal::Journal timeline;
    if (!history.read(QFileInfo(note_.path).completeBaseName(), &timeline, &error)) {
        std::fprintf(stderr, "история не читается: %s\n", error.toUtf8().constData());
        return false;
    }
    // Записи без слепка (надгробие) показывать нечего.
    int last = timeline.entries.size() - 1;
    while (last >= 0 && !timeline.entries[last].hasSnapshot()) --last;
    if (last < 0) return false;

    // Незаписанный снимок принадлежит живой заметке; он уедет вместе с ней, а
    // не в режим истории.
    flushPendingEdit();

    // Живая заметка уезжает целиком в дочерний объект, а на её месте
    // заводится объект слепка — с тем же путём и метой, но со своей цепочкой
    // отмены и своим документом. Возврат — обратная подмена, и потерять при
    // ней нечего: переносится объект, а не набор полей.
    auto live = std::make_unique<NoteSession>(std::move(note_));
    live->cursor = textCursor().position();
    live->scroll = verticalScrollBar()->value();
    live->modified = live->document && live->document->isModified();

    note_ = NoteSession{};
    note_.path = live->path;
    note_.meta = live->meta;
    note_.digest = live->digest;
    note_.timeline = std::move(timeline);
    note_.live = std::move(live);

    setReadOnly(true);
    // О начале режима сообщаем ДО показа слепка: слушатель на этом сигнале
    // заполняет таймлайн, а сигнал о номере записи приходит из showSnapshot.
    // Обратный порядок означал бы, что номер приезжает в пустой список и
    // выделение теряется, — так и было, пока не поменял.
    emit historyModeChanged(true);
    if (!showSnapshot(index < 0 ? last : index)) {
        leaveHistory();
        return false;
    }
    return true;
}

bool NoteEditor::showSnapshot(int index) {
    if (index < 0 || index >= note_.timeline.entries.size()) return false;
    if (!note_.timeline.entries[index].hasSnapshot()) return false;

    // Показ слепка — не правка человека. Без этого подмена документа считалась
    // бы правкой и лезла в цепочку отмены, которой сейчас нет вовсе: она
    // отложена вместе с живой заметкой. Первый же отложенный снимок ронял
    // программу на пустой очереди — поймано набором.
    const bool wasSuspended = recordingSuspended_;
    recordingSuspended_ = true;
    struct Restore {
        NoteEditor* self;
        bool was;
        ~Restore() { self->recordingSuspended_ = was; }
    } restore{this, wasSuspended};

    journal::History history(storeRoot_);
    QByteArray bytes;
    QString error;
    if (!history.snapshotAt(QFileInfo(note_.path).completeBaseName(), index, &bytes, &error)) {
        std::fprintf(stderr, "слепок не собрать: %s\n", error.toUtf8().constData());
        return false;
    }

    // Слепок — байты файла целиком, вместе с шапкой. Разбираем тем же ядром,
    // что и обычное открытие: второго способа прочитать заметку нет.
    const Document doc = parse(std::string(bytes.constData(), size_t(bytes.size())));
    installDocument(std::make_unique<QTextDocument>());
    note_.builtValid = false;
    rebuild(doc, 0, ViewAnchor{});
    document()->setModified(false);
    note_.historyIndex = index;
    emit historyIndexChanged(index);
    return true;
}

void NoteEditor::leaveHistory() {
    if (!inHistory()) return;
    // Всё, что накопилось на слепке, к живой заметке отношения не имеет.
    flushPendingEdit();

    installSession(std::move(*note_.live));
    setReadOnly(false);
    emit historyModeChanged(false);
}

bool NoteEditor::historyStepBack() {
    if (!inHistory() && !enterHistory()) return false;
    int at = note_.historyIndex - 1;
    while (at >= 0 && !note_.timeline.entries[at].hasSnapshot()) --at;
    if (at < 0) return false;   // дальше в прошлое некуда: остаёмся где были
    return showSnapshot(at);
}

bool NoteEditor::historyStepForward() {
    if (!inHistory()) return false;
    int at = note_.historyIndex + 1;
    while (at < note_.timeline.entries.size() && !note_.timeline.entries[at].hasSnapshot()) ++at;
    if (at >= note_.timeline.entries.size()) {
        // Дальше последнего слепка — живая версия. Это и есть выход из режима
        // хронологическим шагом вперёд.
        leaveHistory();
        return true;
    }
    return showSnapshot(at);
}

qint64 NoteEditor::restoreShownSnapshot(bool* alreadyCurrent) {
    if (alreadyCurrent != nullptr) *alreadyCurrent = false;
    if (!inHistory() || note_.historyIndex < 0) return 0;
    const qint64 source = note_.timeline.entries[note_.historyIndex].time;

    // Тело слепка забираем ДО выхода из режима: сейчас оно в поле редактора.
    Document body = readDocument(*document());

    leaveHistory();

    // Слепок и есть нынешняя версия — восстанавливать нечего. Сравниваем тела,
    // без меты: восстановление её и не трогает, а штамп modified у слепка свой
    // и разошёлся бы всегда.
    Document liveBody = readDocument(*document());
    liveBody.meta = {};
    Document sameBody = body;
    sameBody.meta = {};
    if (serialize(sameBody) == serialize(liveBody)) {
        if (alreadyCurrent != nullptr) *alreadyCurrent = true;
        return 0;
    }

    // Метаданные живой заметки побеждают: история возвращает содержимое, а не
    // местоположение. parent, теги и created остаются нынешними; modified
    // поднимется при записи, как при любой правке.
    //
    // Одной правкой, а не пересозданием документа: восстановление обязано
    // отменяться обычным Ctrl+Z, а для этого оно должно быть шагом нашей
    // цепочки, как всякая другая правка.
    flushPendingEdit();
    rebuild(body, textCursor().position(), viewAnchor());
    document()->setModified(true);
    note_.typingRun = false;
    note_.undoRun = false;
    recordEdit();
    flushPendingEdit();

    // Ближайшее сохранение станет записью restore со ссылкой на источник.
    note_.restoreSource = source;
    save(false);
    return source;
}

void NoteEditor::recordBaseline(const QByteArray& contents) {
    if (storeRoot_.isEmpty() || note_.path.isEmpty()) return;
    journal::History history(storeRoot_);
    const QString noteId = QFileInfo(note_.path).completeBaseName();
    journal::Journal journal;
    QString error;
    if (!history.read(noteId, &journal, &error)) {
        std::fprintf(stderr, "история не читается: %s\n", error.toUtf8().constData());
        return;
    }
    if (!journal.entries.isEmpty()) return;   // история уже начата

    const QDateTime when = QFileInfo(note_.path).lastModified();
    if (!history.append(noteId, journal::Kind::Save,
                        when.isValid() ? when.toMSecsSinceEpoch()
                                       : QDateTime::currentMSecsSinceEpoch(),
                        contents, 0, &error))
        std::fprintf(stderr, "опорная запись не записана: %s\n", error.toUtf8().constData());
}

// Насколько две версии разошлись — в знаках, считая и дописанное, и стёртое.
//
// Меряется куском, который изменился: общее начало и общий хвост отбрасываются,
// остаётся то, что человек тронул. Разницы длин мало — два текста одной длины
// бывают разными целиком.
static int changedChars(const QByteArray& a, const QByteArray& b) {
    const qsizetype shared = qMin(a.size(), b.size());
    qsizetype prefix = 0;
    while (prefix < shared && a[prefix] == b[prefix]) ++prefix;
    qsizetype suffix = 0;
    while (suffix < shared - prefix && a[a.size() - 1 - suffix] == b[b.size() - 1 - suffix])
        ++suffix;
    return int(qMax(a.size(), b.size()) - prefix - suffix);
}

void NoteEditor::recordHistory(journal::Kind kind, const QByteArray& snapshot, qint64 source) {
    if (storeRoot_.isEmpty() || note_.path.isEmpty()) return;
    journal::History history(storeRoot_);
    const QString noteId = QFileInfo(note_.path).completeBaseName();
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QString error;

    // Хвост журнала разжимается ОДИН РАЗ на заход в заметку. Дальше он у нас в
    // руках: после каждой записи последней становится ровно то, что мы только
    // что записали.
    if (!note_.journalTailKnown) {
        note_.journalTailKnown = true;
        note_.journalTail.clear();
        note_.journalTailTime = 0;
        journal::Journal read;
        if (history.read(noteId, &read, &error) && !read.entries.isEmpty()) {
            const int last = int(read.entries.size()) - 1;
            if (read.entries[last].hasSnapshot() &&
                history.snapshotAt(noteId, last, &note_.journalTail, &error))
                note_.journalTailTime = read.entries[last].time;
        }
    }

    // ЭКВИВАЛЕНТНЫХ ЗАПИСЕЙ В ХВОСТЕ НЕ БЫВАЕТ — правило владельца, и оно
    // рекурсивное. Одного сравнения с последней записью мало: набрал человек
    // текст (M0'), отменил его (M0" = M0) — и в журнале появлялась пара
    // одинаковых записей ЧЕРЕЗ ОДНУ. Так и вышло у него в «Пробуем Obsidian»:
    //
    //    9  1166      11  1166 == #9        ← набрали и отменили
    //   10  1224      12  1224 == #10
    //
    // Поэтому решение принимается так: сперва считаем, сколько записей с конца
    // уходит (мелкая свежая правка заменяет прошлую), а потом смотрим, не
    // совпал ли новый слепок с тем, что после этого оказалось последним. Совпал
    // — новую запись не пишем вовсе, остаётся САМАЯ СТАРАЯ из одинаковых:
    //
    //   …, M0        →  …, M0, M0'   →  …, M0
    //
    // Сравнение — «не считая строки modified»: она меняется на каждой записи и
    // изменением заметки не является.
    journal::Journal read;
    if (!history.read(noteId, &read, &error)) {
        std::fprintf(stderr, "история не читается: %s\n", error.toUtf8().constData());
        note_.journalTailKnown = false;
        return;
    }

    // ЗАМЕНА ВМЕСТО ДОБАВЛЕНИЯ: два условия, и оба обязаны сойтись — прошлая
    // запись свежая (её ещё не поздно переписать) и разошлись версии на мелочь.
    // Свежесть и близость считаются по слепку, который лежит у нас в памяти:
    // ради этого решения журнал не разжимается.
    const Appearance& look = appearance();
    const qint64 age = now - note_.journalTailTime;
    const bool fresh = note_.journalTailTime > 0 && age >= 0 &&
                       age <= qint64(qMax(1, look.historyMergeHours)) * 3600 * 1000;
    const bool close = !note_.journalTail.isEmpty() &&
                       changedChars(note_.journalTail, snapshot) <= qMax(0, look.historyMergeChars);
    // Заменяем ТОЛЬКО обычное сохранение обычным: возврат из истории и приход
    // правки снаружи — вешки, которые человек ставил не набором, и стирать их
    // нельзя ни в каком виде.
    const bool merge = kind == journal::Kind::Save && fresh && close &&
                       !read.entries.isEmpty() && read.entries.back().kind == journal::Kind::Save;

    int keep = int(read.entries.size());
    bool writeNew = true;

    // ВОЗВРАТ К УЖЕ ЗАПИСАННОМУ СОСТОЯНИЮ. Ищем среди СВЕЖИХ записей хвоста
    // самую старую, равную новому слепку. Нашли — всё, что после неё, было
    // работой, которую человек сам же и отменил: она уходит, а новая запись не
    // пишется вовсе.
    //
    // Почему только среди свежих: вернуться к состоянию месячной давности —
    // законное дело, и стирать за это месяц истории было бы разбоем. Окно то
    // же, что у замены (historyMergeHours): в пределах сегодняшней работы
    // отмена не оставляет следов, дальше — история неприкосновенна.
    // Схлопывание — ТОЛЬКО для обычного сохранения. Возврат из истории и приход
    // правки снаружи — вешки, которые человек ставил намеренно; восстановив
    // старый слепок, он ждёт увидеть в истории и его, и всё, от чего ушёл.
    // (Без этой оговорки восстановление стирало всю историю новее себя —
    // поймано набором.)
    const qint64 window = qint64(qMax(1, look.historyMergeHours)) * 3600 * 1000;
    int sameAs = -1;   // самая старая свежая запись, равная новому слепку
    for (int i = kind == journal::Kind::Save ? int(read.entries.size()) - 1 : -1; i >= 0; --i) {
        const journal::Entry& entry = read.entries[i];
        if (now - entry.time > window) break;   // дальше история старая, её не трогаем
        if (!entry.hasSnapshot()) break;        // надгробие: за него не заглядываем
        QByteArray older;
        if (!history.snapshotAt(noteId, i, &older, &error)) break;
        if (sameApartFromModified(older, snapshot)) {
            sameAs = i;     // нашли; но, может, ещё старее лежит такая же
            continue;
        }
        if (sameAs >= 0) break;                          // старее — уже другое состояние
        if (entry.kind != journal::Kind::Save) break;     // чужую вешку не перепрыгиваем
    }
    if (sameAs >= 0) {
        keep = sameAs + 1;   // эта запись остаётся последней
        writeNew = false;    // такое состояние в журнале уже есть
    }

    // Если возврата не было — работает правило замены: мелкая свежая правка
    // становится на место прошлой, а не рядом с ней.
    if (writeNew && merge && keep >= 2) --keep;

    bool ok = true;
    if (keep < int(read.entries.size())) ok = history.truncate(noteId, keep, &error);
    if (ok && writeNew) ok = history.append(noteId, kind, now, snapshot, source, &error);
    if (!ok) {
        std::fprintf(stderr, "история не записана: %s\n", error.toUtf8().constData());
        note_.journalTailKnown = false;   // что там теперь — неизвестно
        return;
    }
    note_.journalTail = snapshot;
    note_.journalTailTime = now;
}

void NoteEditor::save(bool interactive, bool force) {
    if (note_.path.isEmpty()) return;
    if (!force && !document()->isModified()) return;
    // До записи: сохранение умеет пересобрать документ из перечитанного файла,
    // и шаг серии остался бы без содержимого.
    flushPendingEdit();

    // СРАВНИТЬ, НЕ СЧИТАЯ ШТАМПА. Порядок тут — не вкусовщина.
    //
    // Раньше свежий modified вставал в шапку ДО сериализации, и байты выходили
    // другими всегда: набрал человек «abcd», стёр четыре раза — заметка та же,
    // а на диск уходила копия и в историю запись, отличающаяся одной цифрой в
    // дате. Владелец увидел это в таймлайне как соседей одинакового размера.
    //
    // Теперь собираем байты с ТЕМИ метаданными, что есть, и сравниваем с тем,
    // что лежит в файле (отпечаток известен, файл не читаем). Совпало —
    // сохранять нечего: ни файла, ни записи, ни штампа. Документ при этом
    // перестаёт считаться изменённым: он и правда равен файлу.
    // Штамп ставим сразу, а сравниваем потом и не считая его: так документ
    // разбирается и сериализуется РОВНО ОДИН РАЗ. Не сошлось — эти же байты и
    // уходят в файл; сошлось — откатываем штамп, чтобы шапка в памяти не
    // разъехалась с той, что лежит на диске.
    const NoteMeta metaBefore = note_.meta;
    if (note_.meta.present && stampModifiedOnSave_)
        note_.meta.set("modified",
                  QDateTime::currentDateTimeUtc()
                      .toString(QStringLiteral("yyyy-MM-ddTHH:mm:ss'Z'"))
                      .toStdString());

    Document fileIr;
    const QByteArray candidate = noteBytes(*document(), note_.meta, nullptr, &fileIr);
    if (!note_.lastSaved.isEmpty() && sameApartFromModified(candidate, note_.lastSaved)) {
        note_.meta = metaBefore;
        document()->setModified(false);
        return;
    }

    // Отпечаток того, что в файле, мы знаем — значит «не изменилось ли»
    // решается без чтения файла.
    const SaveOutcome outcome =
        saveDocument(*document(), note_.path, rescueTimestamp(), nullptr, note_.meta, note_.digest,
                     &fileIr, &candidate);
    if (outcome.result == SaveResult::Written || outcome.result == SaveResult::Unchanged) {
        document()->setModified(false);
        note_.lastComplaint.clear();
        // Что теперь в файле, известно из самой записи: отпечаток посчитан по
        // тому буферу, который туда и ушёл. Раньше файл ради этого читался
        // заново — на каждое автосохранение.
        note_.digest = outcome.digest;
        note_.lastSaved = outcome.written;
        watchFile();

        // Файл может прочитаться богаче документа: голую ссылку человек набирает
        // текстом, а разбор делает из неё ссылку. Догоняем — иначе то, что на
        // диске, и то, что на экране, расходились бы до перезагрузки. Шага
        // истории здесь нет: содержимое то же самое, изменилась только разметка
        // внутри строки.
        if (outcome.differsFromDocument) {
            const int cursor = textCursor().position();
            const ViewAnchor anchor = viewAnchor();
            rebuild(outcome.reread, cursor, anchor);
        }
        // Шаг истории — по факту записи, а не по факту нажатия: Unchanged
        // означает, что на диске уже ровно это, и второй одинаковый слепок
        // подряд в журнале не нужен (дедупликация тут бесплатна, потому что
        // сравнение отпечатков уже сделано выше).
        if (outcome.result == SaveResult::Written) {
            const bool restore = note_.restoreSource != 0;
            recordHistory(restore ? journal::Kind::Restore : journal::Kind::Save,
                          outcome.written, note_.restoreSource);
        }
        // Признак гасим при любом исходе записи: он относится к одному
        // ближайшему сохранению, а не «пока не сработает».
        note_.restoreSource = 0;

        // Самопроверка. Окна с вопросом нет и не будет: оно повторялось на
        // каждом автосохранении, и человек его выключал — а вместе с ним
        // терял и сам сигнал. Теперь признак живёт у заметки, полоса рисует
        // по нему красную звёздочку, и гаснет она сама, как только очередная
        // запись сойдётся.
        const bool failed = !outcome.rescuePath.isEmpty();
        if (failed != note_.selfCheckFailed) {
            note_.selfCheckFailed = failed;
            emit statsChanged();   // полосе пора перерисоваться
        }
        if (failed)
            std::fprintf(stderr, "%s\n", outcome.message.toUtf8().constData());

        // Слова и строки: IR записанного уже построен самой записью, и счёт по
        // нему стоит долей от неё. Дальше документ не менялся — значит числа
        // отвечают тому, что на экране.
        //
        // При Unchanged IR не отдают вовсе (записи не было), и там считаем
        // обходом документа. Он втрое дороже, но случай редкий — правка,
        // которая в файле ничего не поменяла, — а обнулить числа пустым IR было
        // бы куда хуже цены.
        if (outcome.result == SaveResult::Written) refreshStats(outcome.reread);
        else refreshStats(documentStats(*document()));

        // Файл на диске стал другим: средней колонке пора перечитать заголовок,
        // начало текста и дату. Сигнал, а не прямой вызов: редактор про список
        // ничего не знает и знать не должен.
        emit fileSaved(note_.path);
        return;
    }

    std::fprintf(stderr, "%s\n", outcome.message.toUtf8().constData());
    // Одну и ту же беду показываем один раз: автосохранение повторяется по
    // таймеру, и окно с ошибкой раз в полторы секунды — это пытка.
    if (!interactive || outcome.message == note_.lastComplaint) return;
    // Файл, про который человек попросил не напоминать, — молчим до конца
    // сессии: беда известна, он правит её руками.
    if (mutedComplaints_.contains(note_.path)) return;
    note_.lastComplaint = outcome.message;

    QMessageBox box(QMessageBox::Warning, QStringLiteral("zametti"), outcome.message,
                    QMessageBox::Ok, this);
    auto* mute = new QCheckBox(
        QStringLiteral("больше не предупреждать про этот файл в этой сессии"), &box);
    box.setCheckBox(mute);
    box.exec();
    if (mute->isChecked()) mutedComplaints_.insert(note_.path);
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
