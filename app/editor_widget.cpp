#include "editor_widget.h"

#include "document_builder.h"
#include "document_reader.h"
#include "document_saver.h"
#include "doc_model.h"
#include "editor_ops.h"
#include "marker.h"
#include "parser.h"
#include "settings.h"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QKeySequence>
#include <QMimeData>
#include <QAction>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QMenu>
#include <QGuiApplication>
#include <QMessageBox>
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

NoteEditor::NoteEditor(QWidget* parent)
    : NoteView(parent), history_(appearance().undoLimit) {
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
    meta_ = doc.meta;
    history_.reset(doc, 0);
    rebuild(doc, 0, {});
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
    meta_ = ir.meta;
    history_.push(ir, textCursor().position());
    sinceLastEdit_.invalidate();
    rebuild(ir, textCursor().position(), viewAnchor());
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
    rebuild(history_.current().doc, textCursor().position(), viewAnchor());
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

void NoteEditor::undo() {
    const int scrollBefore = verticalScrollBar()->value();
    // Курсор ставим туда, где была отменяемая правка, — но её позиция записана
    // в координатах ОТМЕНЯЕМОГО документа, а вернём мы другой. Отображаем
    // через общий префикс и суффикс плоских текстов: до префикса позиции
    // совпадают, после суффикса сдвинуты на разницу длин, а внутри изменённой
    // зоны каретка идёт к началу расхождения. Голое записанное смещение
    // промахивалось: правка добавила знаки выше каретки — и в более коротком
    // возвращённом документе каретка прыгала на пару строк вниз.
    const int recorded = history_.current().cursor;
    const QString undonePlain = document()->toPlainText();
    const HistoryStep* step = history_.undo();
    if (step == nullptr) return;
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
    else if (recorded >= int(undonePlain.size()) - suffix)
        where = recorded + int(restoredPlain.size()) - int(undonePlain.size());
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
    const int scrollBefore = verticalScrollBar()->value();
    const HistoryStep* step = history_.redo();
    if (step == nullptr) return;
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

void NoteEditor::rebuild(const Document& doc, int cursor, const ViewAnchor& anchor) {
    const bool wasSuspended = recordingSuspended_;
    recordingSuspended_ = true;
    buildDocument(doc, *document(), zoom());
    applyContentWidth();

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

void NoteEditor::mousePressEvent(QMouseEvent* event) {
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

    const bool plainEnter = (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) &&
                            (event->modifiers() & ~Qt::KeypadModifier) == Qt::NoModifier;
    // "---" и Enter — тоже тематическая черта, как и "---" с пробелом:
    // правило раньше разреза, иначе Enter развёл бы дефисы и новый блок.
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
    if (event->key() == Qt::Key_Backspace && event->modifiers() == Qt::NoModifier) {
        if (runOperation(unwrapListItemAtCursor)) return;
        // Черта прямо над кареткой удаляется — это удаление назад: гибнет то,
        // что НАД кареткой, а своя строка остаётся под ней. Поэтому раньше
        // обработки пустых строк: с пустой строки под чертой Backspace убирает
        // черту, а не пустую.
        if (runOperation(deleteDividerAbove)) return;
        if (runOperation(joinAcrossVSpaceBackward)) return;
        // Каретка на самой черте, выше текст: черта — строка без содержимого,
        // слить её вверх и значит удалить. Без этого лесенку черт нельзя было
        // снести Backspace-ом до конца: последняя, стоящая под текстом,
        // оставалась, а каретка перешагивала её и ела текст.
        if (runOperation(deleteDividerAtCursor)) return;
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

    // Автозамена срабатывает по пробелу и уже после того, как он набран: правило
    // смотрит на то, что человек написал. Отдельным шагом истории — первый
    // Ctrl+Z обязан вернуть набранные знаки, а не отменить предыдущую правку.
    if (event->text() == QStringLiteral(" ")) {
        // "---" и пробел — тематическая черта, как и остальные автозамены.
        if (!runOperation(applyDividerRuleAtCursor)) runOperation(applyInputRuleAtCursor);
    }
    // Закрывающая кавычка превращает набранное в ней во встроенный код. После
    // этого курсор стоит в конце размеченного куска, и без сброса формата набор
    // продолжался бы кодом — вышло бы `код и всё, что дальше`.
    if (event->text() == QStringLiteral("`") && runOperation(applyCodeSpanRuleAtCursor))
        setCurrentCharFormat(textCursor().block().charFormat());
}

bool NoteEditor::runOperation(bool (*op)(QTextDocument&, QTextCursor&)) {
    QTextCursor cursor = textCursor();
    const int scrollBefore = verticalScrollBar()->value();
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
    const int anchor = cursor.anchor();
    const int position = cursor.position();
    Document ir = readDocument(*document());
    history_.push(ir, position);
    // Операция — отдельный шаг: следующая набранная буква к ней не приклеится.
    sinceLastEdit_.invalidate();

    rebuild(ir, position, viewAnchor());
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

    menu->popup(event->globalPos());
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
    const int scrollBefore = verticalScrollBar()->value();
    if (text.isEmpty()) return;
    const QByteArray utf8 = text.toUtf8();
    const std::string source(utf8.constData(), size_t(utf8.size()));

    Document fragment;
    std::vector<Block>& pieces = fragment.blocks;
    if (literal) {
        // Один абзац с текстом как есть: переводы строк внутри блока сборщик
        // разметит сам, и они вернутся переводами, а не разметкой.
        Block block;
        block.text = source;
        while (!block.text.empty() && block.text.back() == '\n') block.text.pop_back();
        pieces.push_back(std::move(block));
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
    const bool blockLevel =
        pieces.size() > 1 || !head.rawSource.empty() || head.kind != Kind::Paragraph;

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
    history_.push(ir, landed);
    sinceLastEdit_.invalidate();
    rebuild(ir, landed, viewAnchor());
    document()->setModified(true);
    showEditPlace(scrollBefore);
    autosave_.start(appearance().autosaveDelayMs);
}

bool NoteEditor::moveItem(int direction) {
    return applyIrEdit(moveListItem(*document(), textCursor(), direction));
}

bool NoteEditor::applyIrEdit(const MoveResult& moved) {
    if (!moved.done) return false;
    const int scrollBefore = verticalScrollBar()->value();

    history_.push(moved.doc, textCursor().position());
    sinceLastEdit_.invalidate();
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

    const SaveOutcome outcome =
        saveDocument(*document(), path_, rescueTimestamp(), nullptr, meta_);
    if (outcome.result == SaveResult::Written || outcome.result == SaveResult::Unchanged) {
        document()->setModified(false);
        lastComplaint_.clear();
        // Запоминаем, что теперь в файле: иначе слежение примет нашу же запись
        // за чужую правку.
        std::string written;
        if (readFile(path_, written))
            knownContent_ = QByteArray(written.data(), qsizetype(written.size()));
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
