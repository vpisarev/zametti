#include "editor_widget.h"

#include "block_object.h"
#include "caption_editor.h"
#include "lang_editor.h"
#include "diff_view.h"
#include "history_rules.h"

#include "document_builder.h"
#include "document_saver.h"
#include "doc_model.h"
#include "editor_ops.h"
#include "image_insert.h"
#include "marker.h"
#include "serializer.h"
#include "settings.h"
#include "archive.h"
#include "times.h"

#include <QDateTime>
#include <QFileDialog>
#include <QImageReader>
#include <QFileInfo>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QKeySequence>
#include <QShortcut>
#include <QMimeData>
#include <QPainter>
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

#include <cmath>
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
    // Человек взялся за полосу прокрутки — каретку в виду больше не держим.
    // Именно ДЕЙСТВИЕ человека (actionTriggered), а не всякая смена значения:
    // значение меняет и сама Qt — при довёрстке документа, при перекладке окна,
    // — и, отпуская удержание на этом, мы теряли каретку из виду при запуске
    // (владелец: «то слишком высоко, то в самом низу»).
    connect(verticalScrollBar(), &QScrollBar::actionTriggered, this, [this] { releaseCaret(); });
    // Подсветка поиска лежит только на видимом — при прокрутке перекладывается.
    connect(verticalScrollBar(), &QScrollBar::valueChanged, this, [this] {
        if (!current_.search.empty()) showMatchHighlights();
    });
    // Щелчок по месту языка в полоске заводит поле ввода. Виджет просмотра
    // сам язык не правит: правки документа живут здесь.
    connect(this, &NoteView::codeStripClicked, this, [this](int block, const QRect& strip) {
        editCodeLanguage(block, strip);
    });
    // СТЕК ОТМЕНЫ — ШТАТНЫЙ, QTextDocument'а. Своих снимков больше нет: Qt
    // хранит правки на два порядка компактнее (у нас шаг был полной копией
    // содержимого — 361 КБ на заметке в 239 КБ), сам склеивает подряд идущий
    // набор в один шаг и сам возвращает каретку на место правки.
    //
    // Включить его удалось ровно после того, как вид перестал писать в документ
    // резерв места под объекты: пока писал, отменённая запись возвращалась
    // сама, и до текста человека отмена не доходила никогда (замер:
    // zametti-bench undo).

    // Хоткеи разбираем один раз: на каждое нажатие клавиши это было бы разбором
    // строки впустую.
    moveUpKey_ = QKeySequence(settings().editor.moveUpKey, QKeySequence::PortableText);
    moveDownKey_ = QKeySequence(settings().editor.moveDownKey, QKeySequence::PortableText);
    // Сочетаний на команду может быть несколько: через точку с запятой.
    const auto bind = [this](const QString& keys, const NoteOp& op) {
        for (const QKeySequence& sequence :
             QKeySequence::listFromString(keys, QKeySequence::PortableText))
            if (!sequence.isEmpty()) bindings_.push_back({sequence, op});
    };
    const auto bindInline = [this](QKeySequence::StandardKey standard, ZDocument::Style style) {
        for (const QKeySequence& keys : QKeySequence::keyBindings(standard))
            inlineBindings_.push_back({keys, style});
    };
    bindInline(QKeySequence::Bold, ZDocument::Style::Bold);
    bindInline(QKeySequence::Italic, ZDocument::Style::Italic);
    // Встроенный код: Ctrl+E — так его помечают всюду, где вообще помечают.
    inlineBindings_.push_back(
        {QKeySequence(QStringLiteral("Ctrl+E")), ZDocument::Style::Code});
    // Зачёркивание своего стандартного сочетания не имеет; Ctrl+K взят из брифа.
    inlineBindings_.push_back(
        {QKeySequence(QStringLiteral("Ctrl+K")), ZDocument::Style::Strike});

    // Формулы с клавиатуры: строчная и выключная. Сочетания предложены
    // владельцем и стоят рядом с прочими пометками начертания.
    bind(QStringLiteral("Ctrl+M"),
         [](ZDocument& note, QTextCursor& at) { return note.toggleInlineMath(at); });
    bind(QStringLiteral("Ctrl+Shift+M"),
         [](ZDocument& note, QTextCursor& at) { return note.toggleDisplayMath(at); });

    // Автозамены из конфига: сочетание и знак, который оно вставляет.
    // Сочетаний на одну замену может быть несколько, через точку с запятой —
    // как и у любой другой команды.
    for (const auto& [keys, text] : settings().editor.specialKeys) {
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
            runNoteEdit([level](ZDocument& note, QTextCursor& at) {
                return note.setHeadingLevel(at, level);
            });
        });
        addAction(action);
    }

    bind(settings().editor.toggleTaskKey,
         [](ZDocument& note, QTextCursor& at) { return note.toggleTask(at); });
    bind(settings().editor.makeBulletKey,
         [](ZDocument& note, QTextCursor& at) { return note.makeBullet(at); });
    bind(settings().editor.makeOrderedKey,
         [](ZDocument& note, QTextCursor& at) { return note.makeOrdered(at); });
    bind(settings().editor.makeTaskKey,
         [](ZDocument& note, QTextCursor& at) { return note.makeTask(at); });
    bind(settings().editor.makeParagraphKey,
         [](ZDocument& note, QTextCursor& at) { return note.makeParagraph(at); });
    bind(settings().editor.makeCommentKey,
         [](ZDocument& note, QTextCursor& at) { return note.toggleComment(at); });

    autosave_.setSingleShot(true);
    connect(&autosave_, &QTimer::timeout, this, [this] { save(true); });
    // ТИШИНА ТОЖЕ КОНЧАЕТ СЕРИЮ НАБОРА — четвёртое правило границы, и оно
    // единственное, которое не выводится из самого текста. Отмерив паузу,
    // просто помечаем серию оборванной: следующая буква откроет новый шаг.
    typingPause_.setSingleShot(true);
    connect(&typingPause_, &QTimer::timeout, this, [this] { current_.runBroken = true; });
    connectDocument();
    connect(&watcher_, &QFileSystemWatcher::fileChanged, this, &NoteEditor::onFileChanged);
    externalSettle_.setSingleShot(true);
    connect(&externalSettle_, &QTimer::timeout, this, &NoteEditor::onExternalSettled);
    connect(this, &QTextEdit::cursorPositionChanged, this, &NoteEditor::onCaretMoved);
    connect(this, &QTextEdit::cursorPositionChanged, this,
            &NoteEditor::snapCaretOffImage);
}

// ПЛАВАЮЩИЕ ПОЛЯ ЗАКРЫВАЮТСЯ МОЛЧА И ПЕРВЫМИ. Поле языка и поле подписи живут
// поверх вьюпорта, и у обоих потеря фокуса значит «отмена». Разбор виджета
// прячет окно, прятание уводит фокус, уход фокуса зовёт отмену — а отмена лезет
// в документ и в вид, которых к тому моменту уже нет (падение: ImageEdge
// закрывал редактор с открытым полем подписи; стек — hide_sys → setFocusWidget →
// cancelled → setEditedImageCaption → findBlockByNumber на мёртвом документе).
// Поэтому поля отвязываются и удаляются здесь, пока всё ещё живо.
NoteEditor::~NoteEditor() {
    for (QWidget* field : {static_cast<QWidget*>(captionEditor_),
                           static_cast<QWidget*>(languageEditor_)}) {
        if (field == nullptr) continue;
        field->disconnect();
        delete field;
    }
    captionEditor_ = nullptr;
    languageEditor_ = nullptr;
}

// Внутри хитро-отрисованной строки-фотографии каретке делать нечего: любой
// заход внутрь сводится к началу строки (и фотография показывается выбранной),
// шаг вправо с её начала перепрыгивает строку целиком. Направление входа
// различается по прошлой позиции. Действует только на показанные фото: строка
// без файла — обычный текст.
void NoteEditor::snapCaretOffImage() {
    if (snappingCaret_) return;
    const QTextCursor cursor = textCursor();
    const int cameFrom = current_.lastCaretPosition;
    current_.lastCaretPosition = cursor.position();
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
    current_.lastCaretPosition = moved.position();
}

// Строка из одних пробелов неотличима глазом от пустой, а ведёт себя как
// текст — на этом ловилась «склейка при двух пустых». Правило владельца:
// хвостовые пробелы умирают, как только каретка уходит со строки; опустевшая
// строка становится настоящей пустой. Пока каретка на строке — свобода:
// человек имеет право начать с «   слово». Кода это не касается: там
// хвостовые пробелы — содержимое.
void NoteEditor::onCaretMoved() {
    // Каретка забрела в спрятанную строку таблицы — подтягиваем её на видимую.
    // Qt по невидимым блокам её водит охотно (пробник), и без этого каретка
    // пропадала бы из виду посреди сетки. Одно правило на все пути входа:
    // стрелки, щелчок, Home/End, поиск.
    if (!inHistory() && snapCaretOutOfHiddenTable()) return;

    if (tidying_ || recordingSuspended_ || changingLayout()) {
        current_.lastLine = textCursor();
        return;
    }

    // ПРАВКА КОНЧАЕТСЯ ТОЛЬКО ТОГДА, КОГДА ЧЕЛОВЕК УВЁЛ КАРЕТКУ САМ.
    //
    // Проверять состояние таблицы посреди правки нельзя вовсе (правило
    // владельца): Enter внутри исходника заводит пустую строку, а пустая
    // строка таблицу кончает — то есть между двумя нажатиями кусок законно
    // перестаёт быть таблицей. Раньше эта проверка стояла ВЫШЕ защиты «каретку
    // двигает машина, а не человек», и правка обрывалась на промежуточной
    // позиции внутри самой операции: владелец увидел это как «выход по
    // уезжанию работает ненадёжно».
    // ПРАВКА ФОРМУЛЫ КОНЧАЕТСЯ УХОДОМ КАРЕТКИ ИЗ ЕЁ БЛОКА. Проще, чем у
    // таблицы: у формулы блок ОДИН, и «рядом» тут значит «в нём же». Какой
    // блок был раскрыт, помнить не нужно — свернуть надо ТОТ, ИЗ КОТОРОГО
    // ушли, и его номер мы только что запомнили сами (lastLine).
    if (!current_.lastLine.isNull() && current_.lastLine.document() == document() &&
        current_.lastLine.blockNumber() != textCursor().blockNumber()) {
        QTextCursor left = current_.lastLine;
        runNoteEdit([&left](ZDocument& note, QTextCursor&) { return note.closeFormula(left); });
    }

    if (editedTable() >= 0) {
        const int near = tableNearCaret();
        if (near < 0) leaveTableEdit();
        else setEditedTable(near);
    }
    const QTextCursor now = textCursor();
    if (!current_.lastLine.isNull() && current_.lastLine.document() == document()) {
        const QString text = current_.lastLine.block().text();
        int line = 0;
        for (int i = 0; i < current_.lastLine.positionInBlock() && i < text.size(); ++i)
            if (text.at(i) == QChar::LineSeparator) ++line;
        const QString nowText = now.block().text();
        int nowLine = 0;
        for (int i = 0; i < now.positionInBlock() && i < nowText.size(); ++i)
            if (nowText.at(i) == QChar::LineSeparator) ++nowLine;
        if (current_.lastLine.blockNumber() != now.blockNumber() ||
            line != nowLine)
            tidyLeftLine(current_.lastLine);
    }
    current_.lastLine = textCursor();
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
    // Границы шага отмены здесь больше не считаются: подряд идущий набор
    // склеивает в один шаг сам Qt (замер: десять знаков — один шаг). Вместе с
    // этим ушли и три правила границы серии, которые мы держали руками.
    // Границы нужны не только уборке: место под фотографии тоже перемеряется
    // после каждой правки, и по всему документу это 458 мкс на большой
    // заметке — почти всё, что мы добавляем сверх Qt.
    markImageRegion(position, charsAdded);
    if (tidying_) return;
    const int last = qMax(0, document()->characterCount() - 1);
    const int from = qBound(0, position, last);
    const int to = qBound(from, position + charsAdded, last);
    if (current_.dirty.isNull() || current_.dirty.document() != document()) {
        current_.dirty = QTextCursor(document());
        current_.dirty.setPosition(from);
        current_.dirty.setPosition(to, QTextCursor::KeepAnchor);
        return;
    }
    const int lo = qMin(current_.dirty.selectionStart(), from);
    const int hi = qMax(current_.dirty.selectionEnd(), to);
    current_.dirty.setPosition(lo);
    current_.dirty.setPosition(hi, QTextCursor::KeepAnchor);
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
    if (tidying_ || current_.dirty.isNull()) return;
    const int first = document()->findBlock(current_.dirty.selectionStart()).blockNumber() - 1;
    const int last = document()->findBlock(current_.dirty.selectionEnd()).blockNumber() + 1;
    current_.dirty = QTextCursor();

    tidying_ = true;
    note_->doc().tidyRange(first, last, caret);
    tidying_ = false;
}

void NoteEditor::tidyLeftLine(const QTextCursor& left) {
    if (tidying_) return;
    tidying_ = true;
    note_->doc().tidyLine(left);
    tidying_ = false;
}

// Сигналы документа подключаются заново на каждой подмене: документ у нас не
// один на всю жизнь виджета, а свой у каждой заметки.
//
// Оба сигнала, и порядок важен: contentsChange приходит первым и приносит
// границы правки, contentsChanged — следом, и по нему уже подметаем.
namespace {

// Логические блоки живого документа — местная ступень редактора.
//
// Локальная нарочно: наружу из этого файла она не выходит, а всё, что заметка
// умеет делать сама, она делает своими методами. Здесь же документ ещё не
// принадлежит ZDocument, и снять с него блоки больше нечем.
std::vector<Piece> piecesOf(const QTextDocument& doc) {
    std::vector<Piece> out;
    walkPieces(doc, [&](const Piece& piece) {
        out.push_back(piece);
        return true;
    });
    return out;
}

}  // namespace

void NoteEditor::connectDocument() {
    // ЖИВАЯ ЗАМЕТКА — СО СТЕКОМ. Сборщик гасит его у всякого документа, который
    // собирает, и правильно делает: у только что собранного отменять нечего, а
    // временные документы (staging вставки, стороны разности, слепок журнала)
    // так и остаются без стека. Живой заметке его возвращаем здесь — в одном
    // месте, через которое проходит каждая подмена документа.
    document()->setUndoRedoEnabled(true);
    connect(document(), &QTextDocument::contentsChange, this, &NoteEditor::onContentsChange);
    connect(document(), &QTextDocument::contentsChanged, this, &NoteEditor::onContentsChanged);
    // Высота документа поехала — вернуть каретку в вид, если мы её ещё держим.
    // Здесь же, а не у открытия: подмена документа проходит через это одно
    // место, и связь заводится ровно на тот документ, который сейчас показан.
    connect(document()->documentLayout(), &QAbstractTextDocumentLayout::documentSizeChanged, this,
            [this] { keepCaretInView(); });
}

void NoteEditor::retireDocument(std::shared_ptr<QTextDocument> previous) {
    if (!previous) return;
    // СТАРЫЙ ДОКУМЕНТ НЕ УМИРАЕТ ЗДЕСЬ И СЕЙЧАС — он уезжает в deleteLater.
    //
    // Подмена документа случается из обработчиков событий: Tab меняет сторону
    // сравнения, потеря фокуса снимала подглядывание. Qt в этот момент ещё
    // разбирается с тем же событием и с тем же документом — гасит каретку,
    // закрывает ввод из системы, — и вырванная у неё из-под рук память
    // роняет программу. Владелец получил падение по Alt: оконный менеджер
    // забирал фокус, а мы на focusOut сносили документ.
    //
    // Отложенное удаление стоит ноль и снимает целый класс бед: документ
    // доживает до возврата в цикл событий.
    //
    // Держим его СПИСКОМ ЖИВЫХ, а не deleteLater: у shared_ptr нет release, да и
    // отпустить владение ради ручного удаления значило бы вернуться к тому, от
    // чего умный указатель и заводится. Список пустеет на ближайшем возврате в
    // цикл событий — ровно тогда, когда Qt уже отпустила прежний документ.
    retiring_.push_back(std::move(previous));
    QTimer::singleShot(0, this, [this] {
        retiring_.clear();
        retiringNotes_.clear();
    });
}

// То же для ЗАМЕТКИ. Копия ZDocument разделяет ту же внутренность и стоит
// ничего, так что «подержать живой» здесь буквально: положили копию — она и
// держит, пока не вернёмся в цикл событий.
void NoteEditor::retireNote(ZDocument previous) {
    retiringNotes_.push_back(std::move(previous));
    QTimer::singleShot(0, this, [this] {
        retiring_.clear();
        retiringNotes_.clear();
    });
}

void NoteEditor::rememberCaretInto(ZNote& note) const {
    // ПАРОЙ, а не двумя присваиваниями по месту. Забыть один конец — и заметка
    // возвращается с выделением от начала файла до каретки: ровно это и вышло
    // при входе в историю, где записывался только cursor. Прокрутка здесь же —
    // «где я был» это все три числа сразу.
    note.rememberCaret(textCursor().position(), textCursor().anchor(),
                       verticalScrollBar()->value());
}

void NoteEditor::installNote(std::shared_ptr<ZNote> note) {
    // Подмена заметки — ОДНА операция, а не «присвоить объект, потом поставить
    // документ». Порознь между ними существует миг, когда виджет смотрит на
    // уже разрушенный документ: присваивание объекта убивает старый вместе с
    // ним. Так и падало, пока не свёл в одно место.
    ZDocument previousNote = note_->doc();
    if (document() != nullptr) disconnect(document(), nullptr, this, nullptr);
    note_ = std::move(note);
    if (note_ == nullptr) note_ = std::make_shared<ZNote>();
    // ВСЁ, ЧТО НЕ ПЕРЕЖИВАЕТ УХОДА, — выбрасывается здесь, целиком и без
    // разбора. Пока состояние было размазано по объекту заметки, забыть сбросить
    // поле было делом времени: признак «мы в режиме истории» однажды пережил
    // переход к другой заметке, и редактор показывал слепок ПРЕЖНЕЙ.
    current_ = CurrentNoteState{};
    setDocument(note_->doc().getDocument());
    connectDocument();
    retireNote(std::move(previousNote));
    restoreScale();   // документ подменён — масштаб приехал не с ним
    applyContentWidth();

    // Каретка и ВЫДЕЛЕНИЕ: сперва свободный конец, потом каретка с
    // удержанием — так восстанавливается и то, и другое разом. Без выделения
    // концы совпадают, и получается обычная каретка.
    const CaretSpot spot = note_->caret();
    const int last = document()->characterCount() - 1;
    QTextCursor place(document());
    place.setPosition(qBound(0, spot.anchor, last));
    place.setPosition(qBound(0, spot.cursor, last), QTextCursor::KeepAnchor);
    setTextCursor(place);
    verticalScrollBar()->setValue(spot.scroll);
    document()->setModified(note_->wasModified());
    // Курсоры, державшиеся за прежний документ, теперь ни на что не указывают.
    current_.lastLine = QTextCursor();
    current_.dirty = QTextCursor();

    // Мимолётное состояние жестов принадлежит не заметке, а прикосновению к
    // ней, и через подмену не переносится: номер блока, за угол которого тянут,
    // в новом документе значит совсем другое.
    imageResizeBlock_ = -1;
    imageHoverCorner_ = false;
    pressedAnchor_.clear();

    // Таймеры перенастраиваются под новую заметку. Отложенный снимок — её
    // свойство и приехал вместе с ней; висящий от прошлой заметки таймер
    // отменяем, иначе он записал бы шаг в чужую цепочку.
    autosave_.stop();
    watchFile();
}

// Документ разности из своего слота. Слот им и владеет: вынимать его оттуда
// ради показа значило бы завести второе место, где живёт «что сейчас в поле».
void NoteEditor::showDiffSlot(int slot) {
    releaseCaret();   // в истории показан слепок, каретки заметки в нём нет
    if (document() != nullptr) disconnect(document(), nullptr, this, nullptr);
    setDocument(current_.diffDocs[size_t(slot)].get());
    connectDocument();
    restoreScale();
    current_.lastLine = QTextCursor();
    current_.dirty = QTextCursor();
}

qint64 NoteEditor::estimateDocumentBytes(const QTextDocument& doc) {
    // Знаки UTF-16 плюс множитель на фрагменты, форматы и undo-стек Qt; плюс
    // надбавка на сам документ, чтобы у крошечной заметки вес не выходил
    // нулевым. Обоснование множителя — замером, см. documentCacheSizeMb.
    return qint64(doc.characterCount()) * 2 * 9 / 2 + 4096;
}

qint64 NoteEditor::cachedNoteBytes() const {
    qint64 total = 0;
    for (const std::shared_ptr<ZNote>& note : noteCache_) total += note->cachedBytes();
    return total;
}

void NoteEditor::clearNoteCache() {
    noteCache_.clear();
}

void NoteEditor::trimNoteCache() {
    const qint64 budget = qint64(qMax(1, settings().cache.documentCacheSizeMb)) * 1024 * 1024;
    // С хвоста, пока не уложились: самое давнее уходит первым. От вытесненной
    // заметки остаётся только место каретки — вот единственное место, где оно
    // попадает в общую карту. Каретка принадлежит заметке и живёт в её объекте;
    // карта — это ОСТАТОК объекта, а не второй источник правды о нём.
    while (!noteCache_.empty() && cachedNoteBytes() > budget) {
        const ZNote& going = *noteCache_.back();
        if (going.hasPath()) caretMemory_[going.path()] = going.caret();
        noteCache_.pop_back();
    }
}

void NoteEditor::stashCurrentNote() {
    // Место каретки — свойство заметки, и живёт оно в её объекте. Но объект
    // тяжёлый и в кэше остаётся не всегда, а место каретки весит четыре байта
    // и терять его незачем. Поэтому здесь, в единственной точке ухода заметки
    // из открытых, от неё остаётся этот лёгкий след. Второй записи в карту в
    // программе нет: иначе появился бы второй источник правды о каретке.
    if (note_->hasPath())
        caretMemory_[note_->path()] = {textCursor().position(), textCursor().anchor(),
                                       verticalScrollBar()->value()};

    // Откладываем только ЧИСТОЕ и только то, чей отпечаток мы знаем: иначе при
    // возврате не с чем было бы сверять файл. Несохранённое не откладываем
    // вовсе — потерять правки страшнее, чем пересобрать документ.
    if (!note_->hasPath()) return;
    if (document()->isModified() || note_->digest().empty()) return;

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
    if (hashOf(writePieces(piecesOf(*document()), note_->meta()).toUtf8()) != note_->digest()) return;

    const qint64 bytes = estimateDocumentBytes(*document());
    const qint64 budget = qint64(qMax(1, settings().cache.documentCacheSizeMb)) * 1024 * 1024;
    // Заметка тяжелее всего бюджета в кэш не идёт: она вытеснила бы всё
    // остальное и всё равно осталась бы одна.
    if (bytes > budget) return;

    // Прежняя запись про этот же файл больше не нужна.
    std::erase_if(noteCache_, [this](const std::shared_ptr<ZNote>& note) {
        return note->path() == note_->path();
    });

    // Уезжает ВЕСЬ объект заметки, а не выбранные поля. Забыть перенести
    // что-то нельзя: переносится всё, потому что переносится он сам.
    rememberCaretInto(*note_);
    note_->setWasModified(false);
    note_->setCachedBytes(bytes);
    noteCache_.insert(noteCache_.begin(), std::move(note_));
    note_ = std::make_shared<ZNote>();
    trimNoteCache();
}

bool NoteEditor::cachedNoteMatches(const QString& path, const Digest& digest) const {
    return std::any_of(noteCache_.begin(), noteCache_.end(),
                       [&](const std::shared_ptr<ZNote>& note) {
                           return note->path() == path && note->digest() == digest;
                       });
}

bool NoteEditor::restoreCachedNote(const QString& path, const Digest& digest) {
    const auto at = std::find_if(
        noteCache_.begin(), noteCache_.end(),
        [&path](const std::shared_ptr<ZNote>& note) { return note->path() == path; });
    if (at == noteCache_.end()) return false;
    // Отпечаток не сошёлся — файл правили снаружи. Отложенное выбрасываем и
    // собираем с диска: терять нечего, в кэш попало только записанное.
    if ((*at)->digest() != digest) {
        noteCache_.erase(at);
        return false;
    }

    std::shared_ptr<ZNote> note = std::move(*at);
    noteCache_.erase(at);
    note->setWasModified(false);   // в кэш попадает только записанное
    installNote(std::move(note));
    return true;
}

void NoteEditor::activateNote(bool takeFocus) {
    // Каретка и выделение — из объекта заметки, где бы он ни взялся: приехал
    // из кэша или собран только что. Оба пути сходятся здесь, поэтому забыть
    // про один из них нельзя.
    const CaretSpot spot = note_->caret();
    const int last = document()->characterCount() - 1;
    QTextCursor place(document());
    place.setPosition(qBound(0, spot.anchor, last));
    place.setPosition(qBound(0, spot.cursor, last), QTextCursor::KeepAnchor);
    setTextCursor(place);
    // Каретка — в золотом сечении окна, тем же правилом, что и всякое другое
    // место (просьба владельца: при восстановлении места каретки после запуска
    // страница листается так, чтобы каретка стояла около середины). Заметке из
    // кэша вернули её прокрутку, и каретка там уже на виду — правило её и не
    // тронет.
    revealInGolden(caretRectInDocument());

    // И ДЕРЖИМ ЕЁ НА ТОЙ ЖЕ ДОЛЕ ВЫСОТЫ ОКНА, пока вёрстка не устаканится и
    // окно не примет свой размер: высота на этот миг ещё не окончательная —
    // картинки декодируются в другом потоке, формулы считаются по первой
    // отрисовке, при запуске заметка открывается ДО show() и окно ещё меняет
    // размер, — и всё, что выше каретки, подрастает уже после нас, унося текст
    // у человека из-под глаз. Доля, а не пиксель: окно вырастет — каретка
    // останется на своём месте в нём, а не уедет к верхней кромке. Отпускаем
    // при первом же прикосновении человека.
    holdingCaret_ = true;
    heldRatio_ = heldRatioNow();

    // Фокус. Каретку Qt рисует ТОЛЬКО в виджете с фокусом ввода, и без этой
    // строки человек видел открытую заметку без каретки: место восстановлено,
    // а печатать некуда — пока не ткнёшь в текст мышью.
    if (takeFocus) setFocus(Qt::OtherFocusReason);
}

bool NoteEditor::openFile(const QString& path, bool takeFocus) {
    // Открытие другой заметки выводит из режима истории. Без этого редактор
    // остался бы показывать слепок ПРЕЖНЕЙ заметки, имея путь новой, — и
    // первая же правка записала бы чужое прошлое в новый файл. Найдено
    // пробником: после ухода и возврата режим оставался включён.
    leaveHistory();
    // Каретку прежней заметки в виду больше не держим: заметка сменяется, и
    // всё, что дальше делается с документом — сборка, подмена, перекладка
    // полей, — не должно на каждое изменение высоты гонять каретку в вид.
    // Замер (zametti-bench big, «Карамазовы»): с поднятым признаком сборка
    // документа стоила 1940 мс против 396 — каждый вставленный блок менял
    // высоту, и на каждую высоту ensureCursorVisible заново верстал документ.
    releaseCaret();
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
    if (note_->path() != path) stashCurrentNote();

    Digest digest = hashOf(text);
    // Хранилище наше, и держать в нём сор незачем: лишние пробелы в конце строк
    // и недостающий перевод строки в конце файла причёсываются прямо на диске,
    // не трогая ни одного значения в шапке. Заметку всего лишь открыли —
    // всплывать наверх списка недавних ей не с чего.
    //
    // НО НЕ ТО, ЧТО МЫ САМИ ТОЛЬКО ЧТО ЗАПИСАЛИ. В кэше отложенных лежат только
    // заметки, чья сериализация байт в байт равна файлу; если байты файла дают
    // тот же отпечаток, файл и есть канон, и разбирать его целиком ради ответа
    // «причёсывать нечего» незачем. На «Карамазовых» это 50–100 мс на каждый
    // возврат к заметке (замер zametti-bench big).
    if (!cachedNoteMatches(path, digest)) canonicaliseNoteFile(path, text, digest);
    setImageBase(QFileInfo(path).absolutePath());
    current_.lastComplaint.clear();
    current_.externalPending = false;
    current_.externalText.clear();
    externalSettle_.stop();
    current_.externalEmptyRetried = false;
    current_.lastLine = QTextCursor();
    const QByteArray fileBytes(text.data(), qsizetype(text.size()));

    // Журнал этой заметки — её объект; правила отбора — числами из настроек.
    // Опорная запись. Заметки старше журнала: хранилище жило годами, а история
    // заведена только сейчас. Если журнала у заметки ещё нет, кладём в него то,
    // с чем её открыли, — иначе первой записью стало бы первое сохранение, и
    // всё, чем заметка была до него, не попало бы в историю никогда. Владелец
    // на это и наткнулся: он опустошил заметку (Ctrl+A, Delete), автосохранение
    // записало пустоту, и она оказалась самой первой записью её истории.
    //
    // Время берём у файла, а не «сейчас»: содержимое ровно такой давности, и
    // таймлайн не должен утверждать, будто заметка написана в эту минуту.
    ZNoteHistory history = storeRoot_.isEmpty()
                               ? ZNoteHistory()
                               : ZNoteHistory(storeRoot_, QFileInfo(path).completeBaseName(),
                                              historyRules());
    {
        const QDateTime when = QFileInfo(path).lastModified();
        history.ensureBaseline(fileBytes, when.isValid() ? when.toMSecsSinceEpoch() : 0);
    }

    // Отложенная заметка: файл не разбираем и документ не собираем вовсе —
    // история, каретка и прокрутка возвращаются такими, какими были (и журнал
    // с уже разжатым хвостом — тоже её).
    if (restoreCachedNote(path, digest)) {
        watchFile();
        activateNote(takeFocus);
        emit fileChanged(note_->path());
        return true;
    }

    // СВЕЖАЯ ЗАМЕТКА: новый объект с копией файла в памяти (с ней сравнивается
    // всё, что мы соберёмся писать) и своим журналом. Прежнюю заметку держим
    // живой до возврата в цикл событий: виджет на её документ ещё смотрит, а
    // собрать новый мы успеем и так. Ставится ОДНОЙ операцией вместе с
    // документом — installNote; ту же дорогу проходит и отложенная.
    auto fresh = std::make_shared<ZNote>(path, fileBytes, digest, std::move(history));
    std::vector<Piece> doc;
    // Граница файла: байты → текст, один раз.
    NoteHeader meta;
    parsePieces(QString::fromUtf8(text.data(), qsizetype(text.size())), doc, meta);
    fresh->setMeta(std::move(meta));
    fresh->rememberCaret(caretMemory_.value(path));
    installNote(std::move(fresh));
    watchFile();
    rebuild(doc, note_->caret().cursor, {});
    // Каретка, выделение, показ места и фокус — общей дорогой с отложенной
    // заметкой: два пути открытия, одно правило.
    activateNote(takeFocus);
    emit fileChanged(note_->path());
    enterHistoryIfArchived();
    return true;
}

// АРХИВНАЯ ЗАМЕТКА ОТКРЫВАЕТСЯ ИСТОРИЕЙ. В файле у неё стаб — шапка и строка
// заголовка, — а тело живёт в журнале, и показывать человеку одну строку было
// бы враньём: заметка цела, просто лежит в другом месте. Поэтому вместо
// правки открывается голова журнала: тот же режим истории, что и всегда, со
// своим баннером, таймлайном и запретом правки.
//
// Здесь же и объяснение, почему это в openFile, а не в окне: открыть заметку
// можно из дерева, из списка, из поиска и восстановлением — правило одно на
// все двери.
void NoteEditor::enterHistoryIfArchived() {
    if (storeRoot_.isEmpty() || note_->path().isEmpty()) return;
    if (!zametti::store::isArchivedMeta(note_->meta())) return;
    if (inHistory()) return;
    if (!enterHistory()) {
        // Журнала нет вовсе — показать нечего, но и молчать нельзя: человек
        // видит одну строку вместо заметки и вправе знать, почему.
        std::fprintf(stderr, "архивная заметка без истории: %s\n",
                     note_->path().toUtf8().constData());
    }
}

void NoteEditor::watchFile() {
    if (!watcher_.files().isEmpty()) watcher_.removePaths(watcher_.files());
    if (!note_->path().isEmpty()) watcher_.addPath(note_->path());
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
    if (!readFile(note_->path(), text)) return;   // файл унесли: ждём, пока вернётся
    const Digest digest = hashOf(text);
    if (digest == note_->digest()) return;   // это мы сами и записали

    // Файл опустел, а был непустым: похоже, мы всё же попали в середину чужой
    // записи. Одна повторная попытка, прежде чем поверить в пустоту. «Был
    // непустым» — это «прежний отпечаток не равен отпечатку пустоты»: у пустого
    // входа отпечаток свой, и с «не считали» он не путается.
    if (text.empty() && note_->digest() != hashOf(std::string_view()) &&
        !current_.externalEmptyRetried) {
        current_.externalEmptyRetried = true;
        externalSettle_.start(300);
        return;
    }
    current_.externalEmptyRetried = false;
    note_->markWritten(digest, QByteArray(text.data(), qsizetype(text.size())));

    // Шаг истории пишется здесь, а не после ответа человека: файл на диске уже
    // изменился, и это случилось независимо от того, примем мы чужую версию
    // или перезапишем своей. «Оставить моё» тогда ляжет следующей записью.
    note_->history().record(journal::Kind::External, note_->lastSaved());

    // Без несохранённых правок внешнее содержимое — просто ещё один шаг
    // истории: undo вернёт то, что было до него.
    if (!document()->isModified()) {
        adoptExternal(text);
        return;
    }

    // С правками не затираем молча ничего: спрашиваем и ждём ответа.
    current_.externalPending = true;
    current_.externalText = text;
    emit externalChangeDetected();
}

void NoteEditor::resolveExternalConflict(bool takeExternal) {
    if (!current_.externalPending) return;
    current_.externalPending = false;
    const std::string text = std::move(current_.externalText);
    current_.externalText.clear();
    // «Оставить моё» ничего не делает: наша версия перезапишет файл при
    // ближайшем сохранении, и это ровно то, о чём человека спросили.
    if (takeExternal) adoptExternal(text);
}

void NoteEditor::adoptExternal(const std::string& text) {
    std::vector<Piece> ir;
    NoteHeader fresh;
    parsePieces(QString::fromUtf8(text.data(), qsizetype(text.size())), ir, fresh);
    // Чужой редактор мог снести или испортить блок метаданных. Тихо принять
    // это нельзя: заметка потеряла бы родителя и дату создания, то есть уехала
    // бы в корень и «постарела». Прежние значения у нас в памяти — предлагаем
    // вернуть их одним действием, а решает человек.
    const NoteHeader previous = note_->meta();
    const bool lost = previous.present() && !fresh.present();
    // Ключи, которые были и пропали. parent сюда не входит: его правка руками
    // — законный перенос заметки, а не потеря (решение брифа этапа 4).
    QStringList dropped;
    if (!lost && previous.present() && fresh.present()) {
        for (const char* key : {"created", "id"}) {
            if (!previous.get(key).empty() && fresh.get(key).empty())
                dropped.append(QString::fromLatin1(key));
        }
    }

    // role — ключ хранилища, а не текста: им задаётся, папка это или заметка,
    // а заметка папкой никогда не становится и наоборот. Что бы ни вписал
    // снаружи чужой редактор, ставим обратно своё значение; не было своего —
    // просто снимаем ключ. Спрашивать тут нечего: подмена рода не «правка».
    if (fresh.present() && fresh.get("role") != previous.get("role"))
        fresh.set("role", previous.get("role"));

    note_->setMeta(fresh);
    // ВНЕШНЕЕ СОДЕРЖИМОЕ ПРИНИМАЕТСЯ КАК ОБЫЧНАЯ ПРАВКА, и Ctrl+Z возвращает
    // то, что было до него (README обещает это прямо). Значит и пересборка
    // здесь — правка: asEdit, внутри скобки, одним шагом отмены.
    {
        QTextCursor group(document());
        group.beginEditBlock();
        rebuild(ir, textCursor().position(), viewAnchor(), nullptr, /*asEdit=*/true);
        group.endEditBlock();
    }
    document()->setModified(false);

    emit externalAdopted(note_->path());
    if (lost || !dropped.isEmpty()) {
        note_->rememberLostMeta(previous);
        emit metaDamaged(note_->path(), lost ? QStringList{QStringLiteral("весь блок")} : dropped);
    }
}

void NoteEditor::restoreDamagedMeta() {
    if (!note_->hasLostMeta()) return;
    NoteHeader restored = note_->takeLostMeta();
    // Правки тела сохраняются: меняется только шапка. Обычная запись — значит
    // и обычная отмена: вернуть всё как было можно тем же Ctrl+Z.
    editMeta([&restored](NoteHeader& meta) {
        // Ключи, которые чужой редактор оставил, важнее прежних: он мог
        // осмысленно поправить parent, и затирать это нельзя.
        for (const std::string& line : restored.lines()) {
            const size_t colon = line.find(':');
            if (colon == std::string::npos) continue;
            const std::string key = line.substr(0, colon);
            if (!meta.get(key).empty()) continue;
            meta.set(key, line.substr(colon + 1).find_first_not_of(' ') == std::string::npos
                              ? std::string()
                              : line.substr(line.find_first_not_of(' ', colon + 1)));
        }
        meta.setPresent(true);
    });
}

void NoteEditor::applyZoom(qreal value) {
    if (value == zoom()) return;
    // Масштаб держит вид за свой якорь (блок у середины окна) — удержание
    // каретки с ним спорить не должно.
    releaseCaret();
    // МАСШТАБ — ЭТО ОДИН setDefaultFont, а не пересборка.
    //
    // Раньше здесь стоял refreshAppearance(), то есть полная сборка документа
    // заново — и она же всё ломала: сборщик ставит документу БАЗОВЫЙ кегль,
    // ничего не зная о масштабе, так что применить его было некому. Текст
    // стоял, а маркеры и фотографии, которые вид рисует сам, ехали от zoom_ —
    // отсюда и «зум не работает вообще».
    //
    // Пересборка вдобавок стоит 151 мс против 34.7 мс на смену шрифта и чистит
    // стек отмены (замеры — zametti-bench zoom). Ничего из этого масштабу не
    // нужно: абсолютных кеглей в документе нет, размеры знаков заданы ступенями
    // от его шрифта.
    // ЗА ЧТО ДЕРЖИМСЯ ГЛАЗАМИ. Высота документа от смены кегля меняется, а
    // прокрутка задана пикселями — и текст уезжает тем сильнее, чем ниже по
    // заметке человек стоял. Держимся за блок у СЕРЕДИНЫ окна и за его высоту
    // относительно неё: строка, бывшая в середине, там и остаётся.
    //
    // Не за каретку: её на экране может не быть вовсе (человек прокрутил и
    // смотрит другое место), и тогда вид прыгнул бы к ней.
    const QAbstractTextDocumentLayout* layout = document()->documentLayout();
    const int middle = verticalScrollBar()->value() + viewport()->height() / 2;
    const QTextBlock held = document()->findBlock(layout->hitTest(QPointF(0, middle),
                                                                 Qt::FuzzyHit));
    const qreal above = held.isValid()
                            ? layout->blockBoundingRect(held).top() -
                                  verticalScrollBar()->value()
                            : 0.0;

    NoteView::setZoom(value);
    applyContentWidth();
    syncTables();
    syncFormulas();

    if (held.isValid()) {
        // Вёрстку заставляем пересчитаться: без этого прямоугольник блока
        // отдаётся по старому шрифту, и держаться было бы не за что.
        (void)layout->documentSize();
        verticalScrollBar()->setValue(int(layout->blockBoundingRect(held).top() - above));
    }
}

void NoteEditor::refreshAppearance() {
    // В РЕЖИМЕ ИСТОРИИ СОБИРАТЬ НАДО СЛЕПОК, а не живую заметку. Цепочка отмены
    // здесь своя и пустая — по ней документ вышел бы чистым листом. Облик
    // запечён в собранных документах разности, поэтому все слоты выбрасываем и
    // строим заново тот, что показан.
    if (inHistory()) {
        const DiffSpot keep = diffSpotAtCaret();
        dropDiffDocuments();
        // Тот, что в поле, в слотах не лежит — его забрали при показе. Он тоже
        // устарел, и уходит он тем же путём: отложенным удалением, а не здесь и
        // сейчас (виджет на него ещё смотрит).
        current_.diffSlot = -1;
        renderDiff(keep);
        return;
    }
    // Облик запечён в документах при сборке: всё отложенное протухло разом.
    clearNoteCache();
    // Облик меняется — содержимое нет. Берём его из истории и собираем заново;
    // ни нового шага, ни сдвига по истории при этом не происходит.
    //
    // Именно собираем: от облика зависит каждый блок, в том числе и те, что не
    // менялись, и заплатка их не тронула бы.
    note_->invalidateBuilt();
    // СМЕНА ОБЛИКА СБРАСЫВАЕТ ОТМЕНУ, и это единственное место, где мы на это
    // идём сознательно (кроме открытия другой заметки). Облик задаёт КАЖДЫЙ
    // блок, заплатка тут не годится, а полная сборка стек не переживает.
    // Случается редко — правка config.json, — но цена названа: набранное до
    // смены облика отменить будет нечем.
    //
    // ИЗ ЖИВОГО ДОКУМЕНТА, а не из снимка. Прежде содержимое бралось из вершины
    // своей цепочки отмены, и потому смене облика приходилось сперва сбрасывать
    // отложенный снимок — иначе набранное и не попавшее в цепочку пропадало бы.
    // Снимков нет, вопрос снят: живой документ и есть единственное содержимое.
    rebuild(piecesOf(*document()), textCursor().position(), viewAnchor());
}

void NoteEditor::keepCaretOffEdge() {
    const QRect at = cursorRect();
    const int height = viewport()->height();
    if (height <= 0) return;

    // Зазор — то же поле страницы, в высотах строки. Больше половины окна не
    // берём: в узком окне зазор сверху и снизу иначе перекрылись бы.
    const qreal lineUnit = QFontMetricsF(baseFont()).height();
    const int gap = qBound(0, qRound(settings().look.verticalMargin * lineUnit), height / 3);

    QScrollBar* bar = verticalScrollBar();
    if (at.top() < gap) bar->setValue(bar->value() - (gap - at.top()));
    else if (at.bottom() > height - gap) bar->setValue(bar->value() + at.bottom() - height + gap);
}

void NoteEditor::showEditPlace(int scrollBefore, bool jump) {
    // Спрашивать «видно ли сейчас» нельзя: setTextCursor подкручивает вид сам —
    // к моменту нашего вопроса место уже видно, причём ровно у кромки. Поэтому
    // сперва вид возвращается туда, где он стоял ДО правки, и только потом
    // судит правило показа.
    movingView_ = true;
    verticalScrollBar()->setValue(scrollBefore);
    movingView_ = false;
    if (jump) {
        // ПЕРЕХОД (F3, Ctrl+Z не у каретки, вставка издалека): место вне
        // окна или у самой кромки — в золотое сечение.
        revealInGolden(caretRectInDocument());
        return;
    }
    // ПРАВКА У КАРЕТКИ (набор, Enter, вставка): пока каретка видна, вид не
    // трогаем вовсе — иначе набор у нижней кромки дёргал бы окно на каждой
    // строке; ушла за край — тем же правилом показа, что и переход.
    const int height = viewport()->height();
    const int where = scrollBefore + cursorRect().center().y();
    if (where >= scrollBefore && where <= scrollBefore + height) return;
    revealInGolden(caretRectInDocument());
}

QRectF NoteEditor::caretRectInDocument() const {
    return QRectF(cursorRect()).translated(horizontalScrollBar()->value(),
                                          verticalScrollBar()->value());
}

void NoteEditor::revealInGolden(const QRectF& place) {
    const int height = viewport()->height();
    // Окна ещё нет (заметка открывается до show()): показать место сейчас
    // нельзя — его поставит удержание каретки на первой же настоящей раскладке
    // (keepCaretInView по золотому сечению).
    if (!isVisible() || height <= 0 || place.isNull()) return;
    const int scroll = verticalScrollBar()->value();
    const qreal top = place.top() - scroll;

    // Уже на виду и не у самой кромки — вид не трогаем: дёргать картинку под
    // человеком, когда он и так смотрит на нужное место, хуже, чем не двигать.
    const qreal edge = height * 0.15;
    if (top >= edge && place.bottom() - scroll <= height - edge) return;

    // Иначе ставим место в ЗОЛОТОЕ СЕЧЕНИЕ окна (просьба владельца: «в середине
    // или чуть выше»). ensureCursorVisible здесь не годится — он прокручивает
    // МИНИМАЛЬНО, то есть кладёт место у самой кромки, где его толком не видно.
    movingView_ = true;
    verticalScrollBar()->setValue(int(place.top() - height * qBound(0.0, settings().look.focusRatio, 0.9)));
    movingView_ = false;
}

int NoteEditor::findMatches(const QString& text, bool caseSensitive) {
    const int found = current_.search.find(*document(), text, caseSensitive);
    showMatchHighlights();
    return found;
}

void NoteEditor::showMatchHighlights() {
    // ПОДСВЕЧИВАЕТСЯ ТОЛЬКО ВИДИМОЕ. Совпадений в большой заметке тысячи, а Qt
    // на каждую подсветку считает прямоугольник (setExtraSelections →
    // selectionRect → вёрстка строки): «the» в «Карамазовых» стоило 207 мс на
    // каждое нажатие в поле поиска и столько же на снятие. Цена подсветки
    // обязана зависеть от объёма ПОКАЗАННОГО (правило проекта), поэтому
    // берётся окно с запасом по экрану сверху и снизу, а при прокрутке
    // подсветка перекладывается заново — это O(видимого).
    QList<QTextEdit::ExtraSelection> selections;
    const NoteSearch& search = current_.search;
    if (search.empty()) {
        setExtraSelections(selections);
        return;
    }
    const int height = viewport()->height();
    const int from = cursorForPosition(QPoint(0, -height)).position();
    const int to = cursorForPosition(QPoint(viewport()->width(), 2 * height)).position();
    // Совпадения идут по возрастанию позиции: границы окна — двоичным поиском.
    const auto [first, last] = search.range(from, to);
    selections.reserve(last - first + 1);
    const QColor base = settings().look.searchHighlight;
    // Текущее совпадение — контрастнее прочих. Не другим цветом: цвет в
    // оформлении один, а разной должна быть заметность.
    QColor pale = base;
    pale.setAlpha(110);
    for (int i = first; i < last; ++i) {
        QTextEdit::ExtraSelection selection;
        selection.cursor = search.hit(i);
        selection.format.setBackground(i == search.current() ? base : pale);
        selections.append(selection);
    }
    setExtraSelections(selections);
}

void NoteEditor::goToMatch(int index) {
    if (current_.search.empty()) return;
    current_.search.setCurrent(index);
    const int scrollBefore = verticalScrollBar()->value();
    setTextCursor(current_.search.hit(current_.search.current()));
    showMatchHighlights();
    // Переход: совпадение вне окна или у самой кромки — в золотое сечение.
    showEditPlace(scrollBefore, /*jump=*/true);
}

void NoteEditor::stepMatch(int direction) {
    const NoteSearch& search = current_.search;
    if (search.empty()) return;
    if (search.hasCurrent()) {
        goToMatch(search.current() + direction);
        return;
    }
    // Первый шаг — от каретки, а не с начала заметки: человек только что на
    // что-то смотрел, и прыжок в начало документа был бы неожиданным. Дальше
    // каретки ничего нет — по кругу: с начала (или с конца).
    const int at = textCursor().position();
    const int nearest = direction > 0 ? search.nearestForward(at) : search.nearestBackward(at);
    goToMatch(nearest >= 0 ? nearest : (direction > 0 ? 0 : search.count() - 1));
}

void NoteEditor::clearMatches() {
    current_.search.clear();
    setExtraSelections({});
}

bool NoteEditor::replaceCurrentMatch(const QString& with) {
    if (!current_.search.hasCurrent()) return false;
    const QTextCursor target = current_.search.hit(current_.search.current());
    // Замена одного вхождения — это НАБОР ПОВЕРХ ВЫДЕЛЕНИЯ, ровно тот же
    // глагол, что у клавиатуры: заводить ради неё второй путь незачем.
    const QTextCharFormat format = currentCharFormat();
    const bool done = runNoteEdit([&](ZDocument& note, QTextCursor& cursor) {
        cursor.setPosition(target.selectionStart());
        cursor.setPosition(target.selectionEnd(), QTextCursor::KeepAnchor);
        return note.insertText(cursor, with, format);
    });
    if (!done) return false;
    // Прежние курсоры недействительны, ищем заново и встаём на следующее
    // вхождение.
    const int at = current_.search.current();
    findMatches(current_.search.text(), current_.search.caseSensitive());
    if (!current_.search.empty()) goToMatch(at < current_.search.count() ? at : 0);
    return true;
}

int NoteEditor::replaceAllMatches(const QString& text, bool caseSensitive,
                                  const QString& with) {
    if (text.isEmpty()) return 0;
    int replaced = 0;
    const bool done = runNoteEdit([&](ZDocument& note, QTextCursor&) {
        replaced = note.replaceAll(text, caseSensitive, with);
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
    // Дно стека — дальше шагаем в слепки журнала. Режим объявляет себя сам
    // (баннер, заголовок окна), и это же служит защитой от случайного глубокого
    // отката.
    if (!document()->isUndoAvailable()) {
        enterHistory();
        return;
    }
    // Граница серии отмены: первое Ctrl+Z после правок сначала сохраняет.
    // Иначе вершина — то, что человек только что набрал, — не попала бы в
    // историю вовсе: он отменяет её, уходит из заметки, и сохранять уже нечего.
    // Сохранение само пишет свой шаг истории, отдельной записи тут нет.
    //
    // Только первый шаг: дальше человек идёт по уже записанному прошлому, и
    // складывать в историю промежуточные состояния отката значило бы забивать
    // её ровно тем, от чего он уходит.
    if (document()->isModified() && !current_.undoRun) save(false);
    const int scrollBefore = verticalScrollBar()->value();
    current_.undoRun = true;
    // КАРЕТКУ СТАВИТ САМ Qt: команда отмены помнит, где была правка, и ставит
    // каретку туда. Прежде её приходилось выводить сопоставлением плоских
    // текстов до и после — снимок помнил смещение в координатах ОТМЕНЯЕМОГО
    // документа, а возвращался другой, и голое смещение промахивалось на пару
    // строк. Тридцать строк арифметики ушли вместе со снимками.
    // ПОСЛЕ ОТМЕНЫ УБИРАТЬ НЕЧЕГО, и это довод, а не оптимизация.
    //
    // Отмена возвращает состояние, которое мы сами и построили — каноническим,
    // со всеми инвариантами. Значит уборка на нём либо не найдёт ничего (пустая
    // работа на каждом Ctrl+Z), либо что-то изменит — а это означало бы, что
    // отмена вернула не то, что было, и уборка эту беду ЗАМАЗЫВАЕТ вместо того,
    // чтобы дать ей проявиться.
    //
    // Симптом, по которому это нашлось: уборка — новая правка, а всякая новая
    // правка отбрасывает у Qt ветку повтора. Владелец увидел так: «несколько
    // Ctrl+Z — всё хорошо, а redo возвращает пару слов и встаёт».
    recordingSuspended_ = true;
    QTextEdit::undo();
    recordingSuspended_ = false;

    // Вид держится сам, но отменённая правка может оказаться за окном — тогда
    // её надо показать: человек нажал отмену, чтобы увидеть результат.
    showEditPlace(scrollBefore, /*jump=*/true);   // отмена может быть далеко от каретки
    document()->setModified(true);
    autosave_.start(settings().editor.autosaveDelayMs);
}

void NoteEditor::redo() {
    if (inHistory()) {
        historyStepForward();
        return;
    }
    if (!document()->isRedoAvailable()) return;
    const int scrollBefore = verticalScrollBar()->value();
    current_.undoRun = true;
    // По тому же доводу, что и у отмены: возвращённое состояние каноническое,
    // убирать в нём нечего, а всякая правка обрубила бы следующий повтор.
    recordingSuspended_ = true;
    QTextEdit::redo();
    recordingSuspended_ = false;
    showEditPlace(scrollBefore, /*jump=*/true);   // отмена может быть далеко от каретки
    document()->setModified(true);
    autosave_.start(settings().editor.autosaveDelayMs);
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

void NoteEditor::rebuild(const std::vector<Piece>& doc, int cursor, const ViewAnchor& anchor,
                         const std::vector<Piece>* current, bool asEdit) {
    const bool wasSuspended = recordingSuspended_;
    recordingSuspended_ = true;
    // Заплатка вместо сборки: на большой заметке сборка стоит 151 мс, а
    // меняется при обычной правке один блок. Облик и масштаб задают каждый
    // блок, а не только изменившиеся, — при их смене заплатка не годится.
    bool patched = false;
    // Масштаба в этом условии больше нет: он не запечён в документе вовсе —
    // сборщик ставит базовый кегль, а масштаб кладётся поверх шрифтом. Пока
    // условие помнило про масштаб, первая правка после Ctrl+= отказывалась от
    // заплатки и шла полной сборкой в 151 мс.
    if (const std::vector<Piece>* built = note_->builtBlocks(); built != nullptr) {
        std::vector<Piece> read;
        if (current == nullptr) {
            read = piecesOf(*document());
            current = &read;
        }
        patched = patchDocument(*built, *current, doc, *document());
    }
    if (!patched && !asEdit) {
        // Вне правки — прямая сборка. Она гасит стек отмены, и это честно:
        // содержимое заменено целиком (заметку открыли, сменили облик, показали
        // слепок), отменять в нём нечего.
        // Пока документ собирается, вёрстку выключаем: иначе Qt верстает по
        // ходу — на каждый вставленный блок понемногу, а всякий, кто в этот
        // миг спросит высоту, вынудит доверстать до места. Собранный целиком
        // документ верстается один раз и лениво, когда его спросят.
        document()->setLayoutEnabled(false);
        buildDocument(doc, *document());
        document()->setLayoutEnabled(true);
        document()->setUndoRedoEnabled(true);
    } else if (!patched) {
        // ВНУТРИ ПРАВКИ ЧЕЛОВЕКА пересобирать документ на месте нельзя: сборка
        // начинается с clear(), а он в стек отмены не ложится — один Ctrl+Z
        // оставлял от заметки пустой лист. Собираем во временный документ и
        // ЗАМЕНЯЕМ содержимое курсором: замена отменяема и входит в тот же шаг,
        // что и сама правка.
        //
        // ДОЛГ, НАЗВАННЫЙ ВСЛУХ: это O(N) на правку и нарушение главного
        // правила проекта — стоимость нажатия клавиши не должна зависеть от
        // размера заметки. Мало того, что переписывается весь текст: в стек
        // отмены на каждую такую правку ложится копия всего документа.
        //
        // Как ВРЕМЕННОЕ решение владелец его принял; как постоянное оно не
        // годится. Снимается базисом: replaceRange правит РОВНО тот диапазон,
        // которого касается правка, и тогда сюда попадать перестанут вовсе —
        // заплатке не придётся отказываться, потому что операции перестанут
        // менять документ целиком.
        //
        // Пока же платим редко: заплатка отказывается только там, где поменялось
        // всё, — на настоящих заметках это край, на крошечных (наборы) норма.
        QTextDocument staging;
        buildDocument(doc, staging);
        QTextCursor whole(document());
        whole.select(QTextCursor::Document);
        whole.insertFragment(QTextDocumentFragment(&staging));
        // Формат первого блока Qt через фрагмент не доносит — ставим сами.
        QTextCursor head(document());
        head.setPosition(document()->firstBlock().position());
        head.setBlockFormat(staging.firstBlock().blockFormat());
        head.setBlockCharFormat(staging.firstBlock().charFormat());
    }
    // Сборщик поставил документу базовый кегль — масштаб ему возвращаем мы.
    // Заплатка шрифта не трогает, но звать здесь всё равно дешевле, чем помнить
    // о двух путях: setZoom сравнивает шрифт и на совпадении ничего не делает.
    restoreScale();
    note_->setBuiltBlocks(doc);

    // Слова и строки — здесь и только здесь (плюс запись на диск). Считаем
    // ОБХОДОМ ЖИВОГО ДОКУМЕНТА: он только что собран, и брать числа больше
    // неоткуда — доставать ради счёта вторую копию содержимого было бы работой
    // на пустом месте. Обход стоит 3.2 мс на заметке в 239 КБ против 19 мс
    // самой сборки, в таком соседстве он незаметен. За заплаткой не считаем
    // вовсе: она стоит 64 мкс, и счёт был бы в полсотни раз дороже правки.
    if (!patched) refreshStats(documentStats(*document()));
    applyContentWidth();
    // Сборка — не правка: подметать за ней нечего, а область от неё вышла бы
    // во весь документ и утащила бы следующую уборку на полный проход.
    current_.dirty = QTextCursor();

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
        runNoteEdit([width](ZDocument& note, QTextCursor& at) {
            return note.setImageWidth(at, width);
        });
    }
    event->accept();
}

void NoteEditor::mousePressEvent(QMouseEvent* event) {
    releaseCaret();   // человек тронул — каретку в виду больше не держим
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
    runNoteEdit([](ZDocument& note, QTextCursor& at) { return note.toggleTask(at); });
}

void NoteEditor::mouseDoubleClickEvent(QMouseEvent* event) {
    // По рамке — молча: первый щелчок уже переключил задачу, а выделять слово
    // под рамкой человек не собирался.
    if (checkboxUnder(*event).isValid()) return;

    // ДВОЙНОЙ ЩЕЛЧОК ПО ФОРМУЛЕ — это правка её исходника, а не выделение слова
    // в пустоте: текста под вёрсткой нет вовсе, и выделять там нечего. То же
    // правило, что у Enter на ней, — и то же, что у таблицы.
    // ПО КАРТИНКЕ — правка подписи, ровно как Enter на ней.
    if (!isReadOnly() && !inHistory()) {
        QTextCursor at = cursorForPosition(event->pos());
        const BlockObject object = objectOf(at.block());
        if (object.kind == ObjectKind::Formula) {
            setTextCursor(at);
            runNoteEdit([](ZDocument& note, QTextCursor& caret) {
                return note.openFormula(caret);
            });
            event->accept();
            return;
        }
        if (object.kind == ObjectKind::Image) {
            setTextCursor(at);
            editImageCaption(object.first);
            event->accept();
            return;
        }
    }
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
    releaseCaret();   // человек тронул — каретку в виду больше не держим
    // Голое нажатие модификатора ничего не редактирует и каретку не двигает, а
    // хвостовой keepCaretOffEdge прокручивал бы вид к ней: нажал Ctrl перед
    // Ctrl+кликом — и текст упрыгал к каретке. Мимо всей обработки.
    switch (event->key()) {
        case Qt::Key_Alt:
        case Qt::Key_Control:
        case Qt::Key_Shift:
        case Qt::Key_Meta:
        case Qt::Key_AltGr:
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
        // TAB (сторона сравнения) и F4 (ходьба по изменениям) ловятся ЯРЛЫКАМИ
        // ОКНА, а не здесь: фокус в режиме истории запросто оказывается в
        // списке слепков, и обработчик редактора до них не доходил. Одна дверь
        // на клавишу — в main.cpp.
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
        QGuiApplication::clipboard()->setText(shownAsMarkdown(line));
        if (event->matches(QKeySequence::Cut))
            runNoteEdit([](ZDocument& note, QTextCursor& at) { return note.cutImageLine(at); });
        return;
    }

    // ОБЪЕКТЫ — одним местом, и РАНЬШЕ всех прочих правил Enter.
    //
    // Картинка, таблица и будущая формула ведут себя одинаково, и правило для
    // них лежит в block_object.h. Первая редакция ставила этот вызов ниже, за
    // разрезом строки, — и Enter на таблице успевал разрезать её исходник
    // прежде, чем слой о нём узнавал. Поймала проверка флипа; глазами это
    // выглядело бы как «Enter ничего не делает», а на деле резало таблицу.
    // ESC СВОРАЧИВАЕТ РАСКРЫТУЮ ФОРМУЛУ обратно в объект — так правка и
    // кончается (решение владельца: Enter провалиться, Esc выйти, Ctrl+Z —
    // если не понравилось). Стоит ДО слоя объектов: пока формула раскрыта,
    // объекта в этом блоке нет, и слой о ней ничего не знает.
    if (event->key() == Qt::Key_Escape && !isReadOnly() &&
        runNoteEdit([](ZDocument& note, QTextCursor& at) { return note.closeFormula(at); })) {
        event->accept();
        return;
    }

    if (handleObjectKey(event)) return;

    const bool plainEnter = (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) &&
                            (event->modifiers() & ~Qt::KeypadModifier) == Qt::NoModifier;
    // "---" и Enter — тоже тематическая черта, как и "---" с пробелом:
    // правило раньше разреза, иначе Enter развёл бы дефисы и новый блок.
    // На фотографии Enter не делит блок, а заводит пустую строку ЗА ней:
    // каретка на картинке считается стоящей сразу за ней (правило владельца).
    // ЧЕРЕЗ ГЛАГОЛ ЗАМЕТКИ: порядок правил (объект, черта, разрез) живёт теперь
    // внутри breakBlock — он про содержимое, а не про клавиши.
    if (plainEnter && runNoteEdit([](ZDocument& note, QTextCursor& at) {
            return note.breakBlock(at, ZDocument::BreakKind::Plain);
        }))
        return;

    // Ctrl+Enter — выход из блока кода вниз. Раньше разреза и раньше правил
    // черты: в коде оба они означали бы другое, а тут нажатие однозначно.
    const bool ctrlEnter = (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) &&
                           (event->modifiers() & ~Qt::KeypadModifier) == Qt::ControlModifier;
    if (ctrlEnter && runNoteEdit([](ZDocument& note, QTextCursor& at) {
            return note.breakBlock(at, ZDocument::BreakKind::LeaveCode);
        }))
        return;

    // Shift+Enter — «другое»: в абзаце разрезает, в списке переносит строку
    // внутри пункта.
    const bool shiftEnter = (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) &&
                            (event->modifiers() & ~Qt::KeypadModifier) == Qt::ShiftModifier;
    if (shiftEnter && runNoteEdit([](ZDocument& note, QTextCursor& at) {
            return note.breakBlock(at, ZDocument::BreakKind::Otherwise);
        }))
        return;

    // Блок кода из выделенного и обратно. Ctrl+Shift+E рядом с Ctrl+E: тот
    // делает код в строке, этот — блоком.
    if (event->modifiers() == (Qt::ControlModifier | Qt::ShiftModifier) &&
        event->key() == Qt::Key_E) {
        runNoteEdit([](ZDocument& note, QTextCursor& at) { return note.toggleCodeBlock(at); });
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
            runNoteEdit([](ZDocument& note, QTextCursor& at) { return note.cutImageLine(at); });
            return;
        }
        if (event->key() == Qt::Key_Backspace && textCursor().atBlockStart() &&
            own.previous().isValid() &&
            !imageRectInViewport(own.previous()).isEmpty() &&
            runNoteEdit([](ZDocument& note, QTextCursor& at) {
                return note.deleteImageAbove(at);
            }))
            return;
        if (event->key() == Qt::Key_Delete && own.next().isValid() &&
            !imageRectInViewport(own.next()).isEmpty() &&
            runNoteEdit([](ZDocument& note, QTextCursor& at) {
                return note.deleteImageBelow(at);
            }))
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
        // ЧЕРЕЗ ГЛАГОЛ ЗАМЕТКИ. Все жесты со своим правилом — снять
        // комментарность, снять пункт, убрать черту над кареткой, склеить через
        // пустую строку — живут теперь внутри deleteBack: они про содержимое, а
        // не про клавишу. Туда же ушло и обычное удаление знака: тогда починка
        // шва попадает в тот же шаг отмены, что и само удаление.
        if (runNoteEdit([](ZDocument& note, QTextCursor& at) { return note.deleteBack(at); }))
            return;
        // Заметка отказалась — значит каретка на самой черте: удалять слева
        // нечего, и она просто шагает в конец строки выше.
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
    // Delete — то же самое с другой стороны.
    if (event->key() == Qt::Key_Delete && event->modifiers() == Qt::NoModifier &&
        runNoteEdit([](ZDocument& note, QTextCursor& at) { return note.deleteForward(at); }))
        return;

    // Tab и Shift+Tab внутри списка двигают пункт по уровням; вне списка
    // операция отказывается, и Tab остаётся обычным знаком табуляции.
    //
    // ЧТО ИМЕННО ОТСТУПАЕТ, решает заметка: блок кода бывает и внутри пункта
    // списка, и там Tab должен отступать код, а не углублять пункт. Порядок
    // разбора живёт у неё — снаружи про блоки кода знать не должны.
    if (event->key() == Qt::Key_Tab && event->modifiers() == Qt::NoModifier &&
        runNoteEdit([](ZDocument& note, QTextCursor& at) { return note.indent(at); }))
        return;
    if (event->key() == Qt::Key_Backtab ||
        (event->key() == Qt::Key_Tab && event->modifiers() == Qt::ShiftModifier)) {
        runNoteEdit([](ZDocument& note, QTextCursor& at) { return note.outdent(at); });
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
        const ZDocument::Style want = style;
        if (runNoteEdit([want](ZDocument& note, QTextCursor& at) {
                return note.toggleStyle(at, want);
            }))
            return;
        // В блоке кода и в дословном куске текст буквальный — начертанию там
        // взяться неоткуда, как и при выделении. Про это знает сама заметка:
        // без выделения она отдаёт формат для следующей буквы, а в коде
        // возвращает нынешний нетронутым.
        if (!textCursor().hasSelection())
            setCurrentCharFormat(
                note_->doc().styleForTyping(textCursor(), currentCharFormat(), want));
        return;
    }

    for (const auto& [keys, op] : bindings_)
        if (pressed(keys) && runNoteEdit(op)) return;

    // Дальше Tab не идёт НИКОГДА, и это правило шире прежнего.
    //
    // Раньше Tab глотался только в списке, а в обычном абзаце проваливался в
    // QTextEdit и вставлял знак табуляции. Матрица краёв этапа 11 показала,
    // чем это кончается: "\tafter" — это в markdown блок кода с отступом, и
    // абзац, в начале которого нажали Tab, при следующем открытии заметки
    // становился кодом. В конце строки не лучше: хвостовой таб при чтении
    // отбрасывается, и файл перестаёт читаться сам в себя.
    //
    // Внутри кода отступ ставит indentCodeAtCursor выше — пробелами.
    if (event->key() == Qt::Key_Tab || event->key() == Qt::Key_Backtab) return;

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
    if (event->text() == QStringLiteral(" ") &&
        runNoteEdit([](ZDocument& note, QTextCursor& at) { return note.applyDividerRule(at); }))
        return;

    // Перед самим набором: у правого края ссылки набранное не должно уезжать
    // внутрь неё.
    if (!event->text().isEmpty() && event->text().at(0).isPrint()) dropLinkAtRightEdge();

    // Печатающий знак вставляем САМИ — ради границы шага отмены. Всё прочее
    // (Enter, Backspace, стрелки, сочетания) ведёт Qt, как и раньше.
    if (!insertTyped(event->text(), event->modifiers())) NoteView::keyPressEvent(event);
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
    if (event->text() == QStringLiteral(" "))
        runNoteEdit([](ZDocument& note, QTextCursor& at) { return note.applyInputRule(at); });
    // Закрывающая кавычка превращает набранное в ней во встроенный код. После
    // этого курсор стоит в конце размеченного куска, и без сброса формата набор
    // продолжался бы кодом — вышло бы `код и всё, что дальше`.
    if (event->text() == QStringLiteral("`") &&
        runNoteEdit([](ZDocument& note, QTextCursor& at) { return note.applyCodeSpanRule(at); }))
        setCurrentCharFormat(textCursor().block().charFormat());
}


// Исполнение правил слоя объекта. Само правило — чистая функция actionFor,
// здесь только «что делать» и никакого «когда».
bool NoteEditor::handleObjectKey(QKeyEvent* event) {
    if (inHistory() || isReadOnly()) return false;

    const QTextCursor caret = textCursor();
    const QTextBlock block = caret.block();
    const BlockObject own = objectOf(block);

    ObjectContext where;
    where.hasSelection = caret.hasSelection();
    where.atBlockStart = caret.positionInBlock() == 0;
    where.atBlockEnd = caret.positionInBlock() == block.length() - 1;
    where.onGap = isVSpaceBlock(block);
    // Каретка «на объекте» — это каретка на любой его строке. У картинки строка
    // одна, у таблицы их столько, сколько в исходнике. Объект, который СЕЙЧАС
    // ПРАВЯТ исходником, объектом для клавиш не считается — там обычный текст,
    // и буквы обязаны попадать в него как всюду. Список правимых один на все
    // виды: заведи третий вид со своей проверкой — и он забудет либо про
    // запрет, либо про правку (мы уже забывали и то, и другое).
    where.onObject = own.valid() && own.first != editedTable();
    // Сочетание переключения (Ctrl+Space, toggleTaskKey) — настраиваемое, и
    // слой узнаёт его признаком, а не кодом клавиши. Сравнение то же, что у
    // прочих сочетаний в keyPressEvent: Qt сопоставляет с учётом раскладки.
    for (const QKeySequence& keys :
         QKeySequence::listFromString(settings().editor.toggleTaskKey, QKeySequence::PortableText))
        if (!keys.isEmpty() &&
            QKeySequence(event->keyCombination()).matches(keys) == QKeySequence::ExactMatch)
            where.toggleKey = true;

    const QTextBlock above = block.previous();
    const QTextBlock below = block.next();
    const BlockObject objectAbove = objectOf(above);
    const BlockObject objectBelow = objectOf(below);
    where.objectAbove = objectAbove.valid() && objectAbove.first != editedTable();
    where.objectBelow = objectBelow.valid() && objectBelow.first != editedTable();
    if (isVSpaceBlock(above)) {
        const BlockObject overGap = objectOf(above.previous());
        where.objectAboveGap = overGap.valid() && blocksWouldMerge(above.previous(), block);
    }
    if (isVSpaceBlock(below)) {
        const BlockObject overGap = objectOf(below.next());
        where.objectBelowGap = overGap.valid() && blocksWouldMerge(block, below.next());
    }

    const ObjectAction action = actionFor(event->key(), event->modifiers(), where);
    if (action == ObjectAction::None) {
        // ОБЪЕКТ АТОМАРЕН И ДЛЯ БУКВ. Всё, чего слой объекта не назвал своим
        // действием, на объекте просто не делается: буква, набранная на
        // выбранной формуле, не имеет права попасть внутрь её исходника —
        // формула правится только через Enter, как таблица и подпись картинки.
        //
        // Одним местом на все объекты, и это принципиально: владелец трижды
        // ловил нас на том, что у формулы заводится своя копия правил и своя
        // же дыра в них. Раньше буква проваливалась в обычную правку и
        // вписывалась прямо в LaTeX.
        const bool prints = !event->text().isEmpty() && event->text().at(0).isPrint() &&
                            (event->modifiers() & ~Qt::ShiftModifier) == Qt::NoModifier;
        if (where.onObject && prints) {
            switch (own.kind) {
                case ObjectKind::Image:
                    emit importStatus(
                        QStringLiteral("Подпись картинки правится по Enter; %1 прячет её под снимком")
                            .arg(settings().editor.toggleTaskKey));
                    break;
                case ObjectKind::Table:
                    emit importStatus(QStringLiteral("Таблица правится по Enter"));
                    break;
                default:
                    emit importStatus(
                        QStringLiteral("Формула правится по Enter — так же, как таблица"));
                    break;
            }
            return true;
        }
        return false;
    }

    switch (action) {
        case ObjectAction::Edit: {
            // Править таблицу — значит показать её исходник и встать в него.
            if (own.kind == ObjectKind::Table) {
                setEditedTable(own.first);
                QTextCursor at(document()->findBlockByNumber(own.first));
                setTextCursor(at);
                return true;
            }
            // ПРАВИТЬ ФОРМУЛУ — ЗНАЧИТ РАСКРЫТЬ ЕЁ В ИСХОДНИК. Объект
            // заменяется настоящим текстом абзаца, и каретка встаёт в него как
            // во всякий другой; Esc (или уход каретки) сворачивает обратно.
            // Замена идёт глаголом заметки — вид в документ не пишет.
            if (own.kind == ObjectKind::Formula) {
                runNoteEdit([](ZDocument& note, QTextCursor& at) {
                    return note.openFormula(at);
                });
                return true;
            }
            // ПРАВИТЬ КАРТИНКУ — ЗНАЧИТ ПРАВИТЬ ЕЁ ПОДПИСЬ: поле ввода встаёт
            // под снимок, на место подписи. Сама заметка при этом не трогается,
            // пока человек не нажмёт Enter в поле, — тогда идёт глагол
            // setImageCaption. У вики-вложения подписи нет — сказать вслух.
            if (own.kind == ObjectKind::Image) {
                if (editImageCaption(own.first) == nullptr)
                    emit importStatus(
                        QStringLiteral("У вики-вложения «![[…]]» подписи не бывает"));
                return true;
            }
            return false;
        }
        case ObjectAction::ToggleCaption: {
            // Ctrl+Space (toggleTaskKey) на картинке: `~` становится первым
            // знаком подписи, и под снимком её больше не видно
            // (isNonameCaption); то же сочетание снимает знак обратно. У
            // таблицы и формулы подписи нет.
            if (own.kind != ObjectKind::Image) {
                emit importStatus(QStringLiteral("Подпись бывает только у картинки"));
                return true;
            }
            if (!runNoteEdit([](ZDocument& note, QTextCursor& at) {
                    return note.toggleImageCaption(at, QLatin1Char('~'));
                }))
                emit importStatus(QStringLiteral("Подписи у картинки нет — прятать нечего"));
            return true;
        }
        case ObjectAction::LineAfter: {
            const int last = own.last;
            runNoteEdit([last](ZDocument& note, QTextCursor& at) {
                return note.insertLineAfter(at, last);
            });
            return true;
        }
        case ObjectAction::Remove: {
            const BlockObject target =
                where.onObject ? own : (where.objectAbove ? objectAbove : objectBelow);
            if (!target.valid()) return false;
            const int first = target.first;
            const int last = target.last;
            runNoteEdit([first, last](ZDocument& note, QTextCursor& at) {
                return note.removeBlocks(at, first, last);
            });
            setEditedTable(-1);
            return true;
        }
        case ObjectAction::StepOver: {
            // ПЕРЕШАГНУТЬ ОБЪЕКТ ЦЕЛИКОМ. Внутри вёрстки каретке негде быть:
            // она там невидима, и человек жмёт стрелку, пока не проедет весь
            // исходник. Вниз и вправо — за последний блок объекта, вверх и
            // влево — перед первым.
            const bool forward =
                event->key() == Qt::Key_Down || event->key() == Qt::Key_Right;
            const QTextBlock target = forward
                                          ? document()->findBlockByNumber(own.last).next()
                                          : document()->findBlockByNumber(own.first).previous();
            if (!target.isValid()) return true;   // край документа: стоим на месте
            QTextCursor at(target);
            if (!forward) at.movePosition(QTextCursor::EndOfBlock);
            setTextCursor(at);
            return true;
        }
        case ObjectAction::Select: {
            const BlockObject target = where.objectAbove || where.objectAboveGap
                                           ? (where.objectAbove ? objectAbove
                                                                : objectOf(above.previous()))
                                           : (where.objectBelow ? objectBelow
                                                                : objectOf(below.next()));
            if (!target.valid()) return false;
            setTextCursor(QTextCursor(document()->findBlockByNumber(target.last)));
            return true;
        }
        case ObjectAction::None:
            break;
    }
    return false;
}

LanguageEditor* NoteEditor::editCodeLanguage(int firstBlockNumber, const QRect& strip) {
    if (inHistory() || isReadOnly()) return nullptr;
    const QTextBlock block = document()->findBlockByNumber(firstBlockNumber);
    if (!block.isValid() || isRawBlock(block) || kindOf(block) != Kind::Code) return nullptr;
    if (languageEditor_ != nullptr) closeCodeLanguageEditor();

    languageBlock_ = firstBlockNumber;
    languageEditor_ = new LanguageEditor(note_->doc().codeLanguagesNear(firstBlockNumber),
                                         block.blockFormat().stringProperty(InfoProperty),
                                         viewport());
    languageEditor_->setFont(codeLangFont());
    // Пока правят — своя надпись не рисуется, чтобы под полем ничего не было.
    setEditedCodeLanguage(firstBlockNumber);
    // Поле ввода прижато ВПРАВО, к кнопке копирования. Место под имя языка —
    // вся полоска до кнопки (по ней ловится щелчок, и у блока без языка целить
    // больше некуда), а сама надпись нарисована у правого края: поле во всю
    // полоску начиналось бы далеко левее неё, и буквы прыгали бы при входе в
    // правку. Внутри поля текст остаётся прижатым влево — серое дополнение
    // дописывается справа от каретки, и правому выравниванию ему расти некуда.
    QRect box = strip;
    const int want =
        qMax(1, int(std::round(
                   QFontMetricsF(codeLangFont()).horizontalAdvance(QLatin1Char('A')) * 12)));
    if (box.width() > want) box.setLeft(box.right() - want);
    languageEditor_->setGeometry(box);
    languageEditor_->show();
    languageEditor_->setFocus(Qt::MouseFocusReason);

    connect(languageEditor_, &LanguageEditor::accepted, this, [this](const QString& language) {
        const int block = languageBlock_;
        closeCodeLanguageEditor();
        // Правка идёт ШТАТНЫМ путём: тот же глагол заметки, что и у всех прочих
        // операций, — значит и шаг отмены, и запись в журнал, и сериализация
        // в ```lang получаются сами собой.
        runNoteEdit([block, language](ZDocument& note, QTextCursor& at) {
            QTextCursor line = note.caretAtBlock(block);
            if (line.blockNumber() != block) return false;
            const bool done = note.setCodeLanguage(line, language);
            if (done) at = line;
            return done;
        });
    });
    connect(languageEditor_, &LanguageEditor::cancelled, this,
            [this] { closeCodeLanguageEditor(); });
    return languageEditor_;
}

CaptionEditor* NoteEditor::editImageCaption(int blockNumber) {
    if (inHistory() || isReadOnly()) return nullptr;
    const QTextBlock block = document()->findBlockByNumber(blockNumber);
    const BlockImageRef ref = blockImageRef(block);
    if (!ref.valid || ref.wiki) return nullptr;
    if (captionEditor_ != nullptr) closeImageCaptionEditor();

    captionBlock_ = blockNumber;
    // В поле — подпись КАК ОНА ЕСТЬ, со знаком «~», если спрятана: так видно,
    // почему её нет под снимком, и как вернуть.
    captionEditor_ = new CaptionEditor(ref.alt, viewport());
    captionEditor_->setFont(captionFont());
    captionEditor_->setAlignment(ref.align == ImageAlign::Right ? Qt::AlignRight
                                                                 : Qt::AlignLeft);
    // Пока правят — место под подпись отведено, а своя надпись не рисуется.
    setEditedImageCaption(blockNumber);
    placeCaptionEditor();
    captionEditor_->show();
    captionEditor_->setFocus(Qt::OtherFocusReason);

    connect(captionEditor_, &CaptionEditor::accepted, this, [this](const QString& caption) {
        const int block = captionBlock_;
        closeImageCaptionEditor();
        // Штатным путём: глагол заметки — значит шаг отмены, журнал и запись
        // «![подпись](путь)» получаются сами собой.
        runNoteEdit([block, caption](ZDocument& note, QTextCursor& at) {
            QTextCursor line = note.caretAtBlock(block);
            if (line.blockNumber() != block) return false;
            const bool done = note.setImageCaption(line, caption);
            if (done) at = line;
            return done;
        });
    });
    connect(captionEditor_, &CaptionEditor::cancelled, this,
            [this] { closeImageCaptionEditor(); });
    // Прокрутка уводит снимок — поле едет за ним.
    connect(verticalScrollBar(), &QAbstractSlider::valueChanged, captionEditor_,
            [this] { placeCaptionEditor(); });
    return captionEditor_;
}

void NoteEditor::placeCaptionEditor() {
    if (captionEditor_ == nullptr) return;
    const QTextBlock block = document()->findBlockByNumber(captionBlock_);
    const QRectF place = imageCaptionRectInViewport(block);
    if (place.isEmpty()) return;
    // Поле чуть выше строки подписи: у QLineEdit своё внутреннее поле, и
    // впритык буквы режутся снизу.
    const int height = qMax(int(std::ceil(place.height())),
                            captionEditor_->fontMetrics().height() + 4);
    captionEditor_->setGeometry(QRect(int(std::floor(place.left())),
                                      int(std::round(place.center().y())) - height / 2,
                                      int(std::ceil(place.width())), height));
}

void NoteEditor::closeImageCaptionEditor() {
    if (captionEditor_ == nullptr) return;
    CaptionEditor* going = captionEditor_;
    captionEditor_ = nullptr;
    captionBlock_ = -1;
    setEditedImageCaption(-1);
    going->hide();
    // deleteLater, а не delete: закрытие приходит из обработчика самого поля
    // (Esc, потеря фокуса), и убивать виджет под его же стеком нельзя.
    going->deleteLater();
    setFocus(Qt::OtherFocusReason);
}

void NoteEditor::closeCodeLanguageEditor() {
    if (languageEditor_ == nullptr) return;
    LanguageEditor* going = languageEditor_;
    languageEditor_ = nullptr;
    languageBlock_ = -1;
    setEditedCodeLanguage(-1);
    going->hide();
    // deleteLater, а не delete: закрытие приходит из обработчика самого поля
    // (Esc, потеря фокуса), и убивать виджет под его же стеком нельзя.
    going->deleteLater();
    setFocus(Qt::OtherFocusReason);
}

// Набор — единственная правка, которая идёт мимо операций, и здесь она обретает
// ГРАНИЦУ ШАГА ОТМЕНЫ. Qt сам склеивает подряд идущие вставки в один шаг и рвать
// эту склейку не умеет — пустая скобка правки её не разрывает (замерено). Значит
// границу ставим мы: продолжается серия — дописываемся в прошлый шаг, кончилась —
// открываем новый.
bool NoteEditor::insertTyped(const QString& text, Qt::KeyboardModifiers modifiers) {
    if (text.isEmpty()) return false;
    const QChar first = text.at(0);
    // Печатающий знак и только он. Enter приходит как "\r", Backspace пустым —
    // их ведёт Qt, у них свои правила и свои операции.
    if (!first.isPrint()) return false;

    // МОДИФИКАТОРЫ ОТСЕКАЕМ, и это не мелочь: Ctrl+S приносит с собой text()
    // "s", и без этой проверки сохранение печатало букву (поймал набор
    // редактора: в файл уходило «два twos»). Правило то же, что у самого Qt:
    // набор — это ввод без модификаторов, кроме Shift и цифрового блока; пара
    // Ctrl+Alt разрешена особо — так приходит AltGr.
    const Qt::KeyboardModifiers meaningful =
        modifiers & ~(Qt::ShiftModifier | Qt::KeypadModifier);
    const bool altGr = meaningful == (Qt::ControlModifier | Qt::AltModifier);
    if (meaningful != Qt::NoModifier && !altGr) return false;

    QTextCursor cursor = textCursor();
    const bool tooLong = current_.runChars >= qMax(1, settings().editor.undoRunChars);
    const bool moved = current_.runCursor < 0 || cursor.position() != current_.runCursor;
    const bool startNew = current_.runBroken || moved || tooLong || cursor.hasSelection();

    if (startNew) {
        cursor.beginEditBlock();
        current_.runChars = 0;
    } else {
        cursor.joinPreviousEditBlock();
    }
    // ВСТАВКУ ДЕЛАЕТ ЗАМЕТКА. Формат берём ТОТ, КОТОРЫМ ПЕЧАТАЮТ, а не тот, что
    // у курсора: Ctrl+B без выделения задаёт начертание для следующей буквы, и
    // оно живёт здесь, у вида.
    //
    // Заметка заодно приводит к канону ШОВ — набранное на пустой строке, съеденную
    // выделением границу блоков. Прежде это чинилось после набора обходом
    // накопленной области; теперь чинить снаружи нечего.
    recordingSuspended_ = true;
    note_->doc().insertText(cursor, text, currentCharFormat());
    recordingSuspended_ = false;
    cursor.endEditBlock();
    setTextCursor(cursor);

    current_.runChars += int(text.size());
    current_.runCursor = cursor.position();
    // Разделитель остаётся в ЭТОМ шаге, а следующая буква начинает новый: так
    // отмена возвращает «один два три », а не «один два три ч».
    current_.runBroken = false;
    for (const QChar c : text)
        if (c.isSpace() || c.isPunct() || c == QChar::ParagraphSeparator) current_.runBroken = true;
    typingPause_.start(settings().editor.undoCoalesceMs);
    return true;
}

// ПРАВКА ЧЕРЕЗ ГЛАГОЛ ЗАМЕТКИ. От runOperation отличается ровно одним: после
// неё НЕТ ни обхода документа, ни пересборки — заметка привела шов к канону
// сама, и что вышло ровно то же, что собрал бы сборщик, стережёт ассерт
// отладочной сборки.
//
// Ради этого базис и затевался: прежде каждая операция платила обходом всего
// документа (замер: Enter на заметке в 490 КБ стоил 12.7 мс против 3.5 мс у
// набора, и вся разница была здесь).
bool NoteEditor::runNoteEdit(const std::function<bool(ZDocument&, QTextCursor&)>& op) {
    QTextCursor cursor = textCursor();
    const int scrollBefore = verticalScrollBar()->value();
    recordingSuspended_ = true;

    // Скобку открывает курсор человека: Qt возвращает каретку туда, где скобку
    // открыли (см. довод у runOperation).
    QTextCursor group = textCursor();
    group.beginEditBlock();
    const bool handled = op(note_->doc(), cursor);
    if (!handled) {
        group.endEditBlock();
        recordingSuspended_ = false;
        return false;
    }
    setTextCursor(cursor);
    group.endEditBlock();
    recordingSuspended_ = false;

    current_.runBroken = true;   // структурная правка кончает серию набора
    if (note_->statsFresh()) {
        note_->invalidateStats();
        emit statsChanged();
    }
    document()->setModified(true);
    showEditPlace(scrollBefore);
    autosave_.start(settings().editor.autosaveDelayMs);
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
                                  const NoteOp& op) {
        QAction* action = menu->addAction(title, this, [this, op] { runNoteEdit(op); });
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
    const auto style = [](ZDocument::Style want) {
        return NoteOp([want](ZDocument& note, QTextCursor& at) {
            return note.toggleStyle(at, want);
        });
    };
    add(QStringLiteral("Жирный"), QStringLiteral("Ctrl+B"), style(ZDocument::Style::Bold));
    add(QStringLiteral("Курсив"), QStringLiteral("Ctrl+I"), style(ZDocument::Style::Italic));
    add(QStringLiteral("Зачёркнутый"), QStringLiteral("Ctrl+K"),
        style(ZDocument::Style::Strike));
    add(QStringLiteral("Код в строке"), QStringLiteral("Ctrl+E"), style(ZDocument::Style::Code));
    {
        QAction* action = menu->addAction(QStringLiteral("Блок кода"));
        action->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+E")));
        connect(action, &QAction::triggered, this,
                [this] {
                    runNoteEdit([](ZDocument& note, QTextCursor& at) {
                        return note.toggleCodeBlock(at);
                    });
                });
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
                runNoteEdit([level](ZDocument& note, QTextCursor& at) {
                    return note.setHeadingLevel(at, level);
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
                runNoteEdit([to](ZDocument& note, QTextCursor& at) {
                    return note.setImageAlign(at, to);
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

        // Подпись: править (Enter на снимке) и спрятать/вернуть (сочетание
        // переключения). У вики-вложения подписи нет — и пунктов нет.
        if (!ref.wiki) {
            const int block = textCursor().blockNumber();
            QAction* edit = menu->addAction(QStringLiteral("Подпись…"), this,
                                            [this, block] { editImageCaption(block); });
            edit->setShortcut(QKeySequence(Qt::Key_Return));
            // Спрятанная ЗНАКОМ подпись возвращается тем же сочетанием; имя от
            // камеры («IMG_1234») знаком не вернуть — оно безымянное само по
            // себе, и пункт тогда ни к чему.
            const bool marked = !ref.alt.isEmpty() && (ref.alt.at(0) == QLatin1Char('~') ||
                                                       ref.alt.at(0) == QLatin1Char('-'));
            QAction* toggle = menu->addAction(
                marked ? QStringLiteral("Показать подпись под снимком")
                       : QStringLiteral("Спрятать подпись под снимком"),
                this, [this] {
                    runNoteEdit([](ZDocument& note, QTextCursor& at) {
                        return note.toggleImageCaption(at, QLatin1Char('~'));
                    });
                });
            const QList<QKeySequence> all = QKeySequence::listFromString(
                settings().editor.toggleTaskKey, QKeySequence::PortableText);
            if (!all.isEmpty()) toggle->setShortcut(all.first());
            toggle->setEnabled(!ref.alt.trimmed().isEmpty() &&
                               (marked || !isNonameCaption(ref.alt)));
        }
    }

    menu->addSeparator();
    add(QStringLiteral("Переключить задачу"), settings().editor.toggleTaskKey,
        [](ZDocument& note, QTextCursor& at) { return note.toggleTask(at); });

    QMenu* kinds = menu->addMenu(QStringLiteral("Сделать"));
    const auto addKind = [this, kinds](const QString& title, const QString& keys,
                                       const NoteOp& op) {
        QAction* action = kinds->addAction(title, this, [this, op] { runNoteEdit(op); });
        const QList<QKeySequence> all =
            QKeySequence::listFromString(keys, QKeySequence::PortableText);
        if (!all.isEmpty()) action->setShortcut(all.first());
    };
    addKind(QStringLiteral("Маркированным списком"), settings().editor.makeBulletKey,
            [](ZDocument& note, QTextCursor& at) { return note.makeBullet(at); });
    addKind(QStringLiteral("Нумерованным списком"), settings().editor.makeOrderedKey,
            [](ZDocument& note, QTextCursor& at) { return note.makeOrdered(at); });
    addKind(QStringLiteral("Списком задач"), settings().editor.makeTaskKey,
            [](ZDocument& note, QTextCursor& at) { return note.makeTask(at); });
    addKind(QStringLiteral("Комментарием"), settings().editor.makeCommentKey,
            [](ZDocument& note, QTextCursor& at) { return note.toggleComment(at); });
    addKind(QStringLiteral("Обычным текстом"), settings().editor.makeParagraphKey,
            [](ZDocument& note, QTextCursor& at) { return note.makeParagraph(at); });

    menu->addSeparator();
    add(QStringLiteral("Сдвинуть вправо"), QStringLiteral("Tab"),
        [](ZDocument& note, QTextCursor& at) { return note.indent(at); });
    add(QStringLiteral("Сдвинуть влево"), QStringLiteral("Shift+Tab"),
        [](ZDocument& note, QTextCursor& at) { return note.outdent(at); });

    QAction* up = menu->addAction(QStringLiteral("Переставить вверх"), this,
                                  [this] { moveItem(-1); });
    up->setShortcut(QKeySequence(settings().editor.moveUpKey, QKeySequence::PortableText));
    QAction* down = menu->addAction(QStringLiteral("Переставить вниз"), this,
                                    [this] { moveItem(1); });
    down->setShortcut(QKeySequence(settings().editor.moveDownKey, QKeySequence::PortableText));

    // Внешний редактор — команда окна, а не редактора: запускать процессы
    // виджету текста не по чину. Пункт здесь, потому что искать его человек
    // будет там, где смотрит на заметку.
    menu->addSeparator();
    menu->addAction(QStringLiteral("Открыть во внешнем редакторе"), this,
                    [this] { emit externalEditorRequested(note_->path()); });
    // Вывоз — тоже команда окна: диалог сохранения и запись PDF виджету текста
    // не по чину, да и заметку перед вывозом надо сперва записать.
    menu->addAction(QStringLiteral("Экспортировать…"), this,
                    [this] { emit exportRequested(note_->path()); });

    menu->popup(event->globalPos());
}

QMimeData* NoteEditor::createMimeDataFromSelection() const {
    QMimeData* data = new QMimeData;
    data->setText(shownAsMarkdown(textCursor()));
    return data;
}

// Кусок ПОКАЗАННОГО как markdown.
//
// Обычно показана живая заметка, и спрашиваем её саму — глаголом. В режиме
// истории в поле лежит ЧУЖОЙ документ: иллюстрированная копия слепка, не наша
// заметка. Копировать из неё заметка не может и не должна — это не она, и
// второй путь здесь не дубль, а признание факта.
//
// Путь исчезнет, когда слепок сам станет заметкой (отделённый ZDocument — см.
// zametti-editing-basis.md §1); пока он один на всю программу и назван.
QString NoteEditor::shownAsMarkdown(const QTextCursor& range) const {
    if (inHistory()) return selectionToMarkdown(range);
    return note_->doc().markdownOf(range);
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
    const int scrollBefore = verticalScrollBar()->value();

    // ВСЯ РАБОТА С СОДЕРЖИМЫМ — У ЗАМЕТКИ. Здесь остаётся только то, что и
    // должно быть у виджета: один шаг отмены, признак «изменено», показ места
    // правки и таймер записи.
    recordingSuspended_ = true;
    QTextCursor cursor = textCursor();
    // СКОБКУ ОТКРЫВАЕМ КУРСОРОМ ЧЕЛОВЕКА, а не свежим.
    //
    // QTextCursor::beginEditBlock запоминает позицию ТОГО курсора, которым
    // скобку открыли, и именно туда отмена возвращает каретку. Свежий курсор
    // стоит в начале документа — и после Enter с Ctrl+Z каретка уезжала в самое
    // начало заметки (жалоба владельца).
    QTextCursor group = textCursor();
    group.beginEditBlock();
    const bool done = note_->doc().replaceRange(
        cursor, text,
        literal ? ZDocument::PasteMode::Literal : ZDocument::PasteMode::Markdown);
    if (!done) {
        group.endEditBlock();
        recordingSuspended_ = false;
        return;
    }
    setTextCursor(cursor);
    group.endEditBlock();
    recordingSuspended_ = false;

    // ПЕРЕСБОРКИ ЗДЕСЬ НЕТ, и в этом весь смысл базиса.
    //
    // Прежде вставка кончалась обходом всего документа (piecesOf — 1.86 мс на
    // заметке в 228 КБ) и заплаткой поверх: стоимость правки зависела от
    // размера заметки. Теперь заметка приводит к канону только ШОВ, а что
    // получилось ровно то же, что собрал бы сборщик, проверяет ассерт в
    // отладочной сборке — на каждой вставке всех наборов.
    //
    // built в ZNote от этого устаревает, и это законно: заплатка читает нынешнее
    // состояние документа сама, а устаревший «из чего собрано» лишь расширяет
    // ей область работы, не портя результат.
    current_.runBroken = true;   // вставка кончает серию набора
    if (note_->statsFresh()) {
        note_->invalidateStats();
        emit statsChanged();
    }

    document()->setModified(true);
    showEditPlace(scrollBefore);
    autosave_.start(settings().editor.autosaveDelayMs);
}

QString NoteEditor::attachmentDir() const {
    if (note_->path().isEmpty()) return {};
    return QFileInfo(note_->path()).absolutePath();
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
        importer_ = new ImageImporter(importLimitsFrom(settings().images), this);
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
    return runNoteEdit([direction](ZDocument& note, QTextCursor& at) {
        return note.moveListItem(at, direction);
    });
}

// Пересчёт слов и строк. Зовётся из двух мест — полной сборки и записи на
// диск, — и оба они и без него стоят миллисекунды.
void NoteEditor::refreshStats(const NoteStats& stats) {
    note_->setStats(stats);
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
    // ПОЧИНКА ПРИКЛЕИВАЕТСЯ К ТОЙ ПРАВКЕ, КОТОРУЮ ЧИНИТ.
    //
    // joinPreviousEditBlock дописывает наши правки в ТУ ЖЕ команду отмены, что
    // и набранный знак. Без этого одно нажатие клавиши давало два-три шага:
    // сам знак, починка инварианта пустых строк, снятые хвостовые пробелы, — и
    // Ctrl+Z снимал не букву, а невидимую уборку.
    //
    // Это ровно тот случай, для которого joinPreviousEditBlock в Qt и заведён:
    // правка вызвана чужой правкой и своим шагом быть не должна.
    QTextCursor cursor = textCursor();
    recordingSuspended_ = true;
    QTextCursor join(document());
    join.joinPreviousEditBlock();
    const bool repaired = note_->doc().repairAfterEdit(cursor);
    // Правка любого вида могла оставить хвостовые пробелы на строках, где
    // каретки нет, — выделение с удалением, вставка, слияние. Инвариант
    // владельца: таких строк не существует. Чистим после каждой правки.
    tidySweep(repaired ? cursor : textCursor());
    join.endEditBlock();
    recordingSuspended_ = false;
    if (repaired) setTextCursor(cursor);

    current_.undoRun = false;   // настоящая правка — серия отмены кончилась
    // Числа отстали от документа. Сам пересчёт будет на ближайшем
    // автосохранении: на нажатие клавиши статистику не считаем.
    if (note_->statsFresh()) {
        note_->invalidateStats();
        emit statsChanged();
    }
    autosave_.start(settings().editor.autosaveDelayMs);
}

// Правка есть — снимка пока нет. Читать документ целиком на каждую букву
// незачем: набор подряд всё равно склеивается в один шаг, и все промежуточные
// снимки этого шага выбрасываются. Ждём конца серии.

void NoteEditor::editMeta(const std::function<void(NoteHeader&)>& change) {
    note_->meta().setPresent(true);
    change(note_->meta());
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
    editMeta([&parentId](NoteHeader& meta) {
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
    if (storeRoot_.isEmpty() || note_->path().isEmpty()) return false;

    // Незаписанные правки — в файл, а значит и в журнал: человек пошёл смотреть
    // прошлое, и вершина цепочки обязана в этом прошлом оказаться. Иначе он
    // вернётся к живой версии, уйдёт из заметки — и то, что набрал, пропадёт
    // из истории навсегда.
    // ВХОД В ИСТОРИЮ — ЕЩЁ ОДНА ТОЧКА СОХРАНЕНИЯ, в один ряд с уходом из
    // заметки и выходом из программы. Признак «изменён» не спрашивается вовсе
    // (force): он может и не встать из-за нашей же ошибки в редком пути правки,
    // а человек как раз пошёл смотреть прошлое, и вершина его работы обязана в
    // этом прошлом оказаться. Лишней записи от этого не бывает — все правила
    // отбора внутри save работают: не изменилось, значит записи нет, и вершина
    // таймлайна и так равна живому буферу.
    save(false, true);

    // ВТОРОЙ ТРИГГЕР ЛЕНИВОЙ МИГРАЦИИ: первое чтение журнала ради истории этой
    // заметки. Просмотр заметки журнал не трогает вовсе — а вот пошёл человек
    // в прошлое, и прошлое обязано быть уже чистым: показывать дубликаты,
    // которые всё равно уйдут при первой правке, незачем.
    QString error;
    // Читаем в местную переменную: объект заметки сейчас уедет целиком, и
    // положенное в него до этого уехало бы вместе с ним. Чтение чистит журнал
    // само (второй триггер ленивой миграции).
    journal::Journal timeline;
    if (!note_->history().read(&timeline, &error)) {
        std::fprintf(stderr, "история не читается: %s\n", error.toUtf8().constData());
        return false;
    }
    // Записи без слепка (надгробие) показывать нечего.
    int last = timeline.entries.size() - 1;
    while (last >= 0 && !timeline.entries[last].hasSnapshot()) --last;
    if (last < 0) return false;

    // Незаписанный снимок принадлежит живой заметке; он уедет вместе с ней, а
    // не в режим истории.

    // Живая заметка уезжает целиком в дочерний объект, а на её месте
    // заводится объект слепка — с тем же путём и метой, но со своей цепочкой
    // отмены и своим документом. Возврат — обратная подмена, и потерять при
    // ней нечего: переносится объект, а не набор полей.
    std::shared_ptr<ZNote> live = std::move(note_);
    rememberCaretInto(*live);
    live->setWasModified(document() != nullptr && document()->isModified());

    // Объект слепка: тот же путь, мета и отпечаток, свой документ и своя
    // (пустая) история — в режиме истории журнал не пишется.
    note_ = std::make_shared<ZNote>(live->path(), live->lastSaved(), live->digest(),
                                    ZNoteHistory());
    note_->setMeta(live->meta());
    current_.timeline = std::move(timeline);
    current_.live = std::move(live);

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
    if (index < 0 || index >= current_.timeline.entries.size()) return false;
    if (!current_.timeline.entries[index].hasSnapshot()) return false;

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

    QByteArray bytes;
    QString error;
    if (!current_.live->history().snapshotAt(index, &bytes, &error)) {
        std::fprintf(stderr, "слепок не собрать: %s\n", error.toUtf8().constData());
        return false;
    }

    // Слепок — байты файла целиком, вместе с шапкой. Разбираем тем же ядром,
    // что и обычное открытие: второго способа прочитать заметку нет.
    // ГДЕ ЧЕЛОВЕК СТОЯЛ. У другого слепка другой текст, и точного места в нём
    // нет; но номер строки от слепка к слепку меняется мало, и вернуться
    // ПРИМЕРНО туда же куда полезнее, чем начинать каждый слепок с начала
    // (просьба владельца). Снимаем ДО подмены — потом этого документа не будет.
    const int wasLine = diffLineAtCaret();
    const int wasOnScreen = caretOnScreen();

    current_.snapshot.clear();
    NoteHeader snapshotHeader;
    parsePieces(QString::fromUtf8(bytes), current_.snapshot, snapshotHeader);
    current_.historyIndex = index;
    computeDiff(index);
    renderDiff({});
    goToDiffLine(wasLine, wasOnScreen);
    emit historyIndexChanged(index);
    return true;
}

// --- разность ---------------------------------------------------------------

void NoteEditor::computeDiff(int index) {
    dropDiffDocuments();   // собранное относилось к другому слепку или другой базе
    current_.diffReady = false;
    current_.base.clear();
    current_.snapshotText = diff::textOf(current_.snapshot);

    current_.baseTime = 0;
    current_.baseIsLive = diffFromFresh_;
    QByteArray baseBytes;
    if (diffFromFresh_) {
        // Со свежей версией. Она у нас в руках — это последняя записанная
        // копия отложенной живой заметки; читать файл заново незачем.
        if (current_.live != nullptr) baseBytes = current_.live->lastSaved();
        if (baseBytes.isEmpty()) {
            QFile file(note_->path());
            if (file.open(QIODevice::ReadOnly)) baseBytes = file.readAll();
        }
    } else {
        // С предыдущей записью. Надгробия пропускаем: слепка у них нет.
        int at = index - 1;
        while (at >= 0 && !current_.timeline.entries[at].hasSnapshot()) --at;
        QString error;
        if (at >= 0) {
            current_.baseTime = current_.timeline.entries[at].time;
            if (!current_.live->history().snapshotAt(at, &baseBytes, &error))
                std::fprintf(stderr, "слепок для сравнения не собрать: %s\n",
                             error.toUtf8().constData());
        }
    }

    // Базы нет вовсе (самая первая запись) — сравниваем с пустотой: вся
    // заметка окажется добавленной, и это правда.
    current_.base.clear();
    NoteHeader baseHeader;
    parsePieces(QString::fromUtf8(baseBytes), current_.base, baseHeader);
    current_.baseText = diff::textOf(current_.base);
    // ДВА ПРОГОНА, по одному на сторону: показанная сторона всегда «after»
    // своего сравнения, и тогда зелёное с красным не приходится выворачивать
    // наизнанку при переключении — они просто меняются местами сами.
    current_.diffResult = diff::compare(current_.baseText.lines, current_.snapshotText.lines);
    current_.diffReverse = diff::compare(current_.snapshotText.lines, current_.baseText.lines);
    current_.diffReady = true;
}

NoteEditor::DiffSpot NoteEditor::diffSpotAtCaret() const {
    DiffSpot spot;
    if (!current_.diffReady) return spot;
    spot.base = diffPeek_;

    // ДЕРЖИМСЯ ЗА ТО, ЧТО НА ЭКРАНЕ, а не за каретку. Читая историю, человек
    // крутит колесо, а каретка так и стоит там, где её оставили, — чаще всего в
    // самом начале. Держась за неё, мы возвращали вид к началу документа:
    // владелец увидел это как «Tab убегает, иногда вообще на начало».
    //
    // Тот же приём, что у пересборки документа (см. ViewAnchor): блок у верхней
    // кромки окна и его высота относительно неё.
    const QAbstractTextDocumentLayout* layout = document()->documentLayout();
    const int scroll = verticalScrollBar()->value();
    const int at = layout->hitTest(QPointF(0, scroll), Qt::FuzzyHit);
    QTextBlock top = document()->findBlock(at);
    if (!top.isValid()) top = document()->begin();
    if (!top.isValid()) return spot;

    const int number = top.blockNumber();
    const int source = number >= 0 && number < diffSource_.size() ? diffSource_[number] : -1;
    if (diffPlainView_) {
        spot.line = number >= 0 && number < shownResult().rows.size()
                        ? shownResult().rows[number].after
                        : -1;
    } else {
        const diff::Text& text = shownText();
        spot.line = source >= 0 && source < text.blocks.size() ? text.blocks[source].first : -1;
    }
    if (spot.line < 0) return spot;
    spot.onScreen = int(layout->blockBoundingRect(top).top()) - scroll;
    return spot;
}

int NoteEditor::lineOnOtherSide(int line, bool fromBase) const {
    // Пары строк живут в ПРЯМОМ сравнении: у общей и у изменённой строки есть
    // обе стороны. Точной пары может не быть (строка есть только здесь) —
    // тогда берём ближайшую, у которой пара есть: лучше рядом, чем в начале.
    int best = -1;
    int bestDistance = -1;
    for (const diff::Row& row : current_.diffResult.rows) {
        const int here = fromBase ? row.before : row.after;
        const int there = fromBase ? row.after : row.before;
        if (here < 0 || there < 0) continue;
        const int distance = qAbs(here - line);
        if (bestDistance < 0 || distance < bestDistance) {
            bestDistance = distance;
            best = there;
        }
        if (distance == 0) break;
    }
    return best;
}

void NoteEditor::diffGoToSpot(const DiffSpot& spot) {
    if (spot.line < 0) return;
    // Сторона могла смениться (Tab): переводим строку в её пространство.
    const int line = spot.base == diffPeek_ ? spot.line
                                            : lineOnOtherSide(spot.line, spot.base);
    goToDiffLine(line, spot.onScreen);
}

int NoteEditor::caretOnScreen() const {
    const QRectF rect = document()->documentLayout()->blockBoundingRect(textCursor().block());
    return int(rect.top()) - verticalScrollBar()->value();
}

int NoteEditor::diffLineAtCaret() const {
    if (!inHistory() || !current_.diffReady) return -1;
    if (diffPlainView_) {
        const int row = textCursor().blockNumber();
        if (row < 0 || row >= shownResult().rows.size()) return -1;
        return shownResult().rows[row].after;
    }
    // Номер блока КОПИИ переводим в блок слепка: у копии свои номера, и всё,
    // что ниже дорисованной строки, съехало бы.
    const int number = textCursor().blockNumber();
    const int source = number >= 0 && number < diffSource_.size() ? diffSource_[number] : -1;
    const diff::Text& text = shownText();
    return source >= 0 && source < text.blocks.size() ? text.blocks[source].first : -1;
}

void NoteEditor::goToDiffLine(int line, int onScreen) {
    if (line < 0 || !inHistory() || !current_.diffReady) return;
    if (diffPlainView_) {
        // В виде «как под капотом» ищем строку сравнения с таким номером на
        // показанной стороне; точной может не быть — берём ближайшую сверху.
        int row = -1;
        for (int i = 0; i < shownResult().rows.size(); ++i) {
            const int at = shownResult().rows[i].after;
            if (at >= 0 && at <= line) row = i;
        }
        if (row < 0) return;
        const QTextBlock target = document()->findBlockByNumber(row);
        if (!target.isValid()) return;
        // КАРЕТКУ НЕ ТРОГАЕМ: в режиме истории она человеку не нужна, а вид —
        // нужен. Двигаем только прокрутку.
        const QRectF rect = document()->documentLayout()->blockBoundingRect(target);
        verticalScrollBar()->setValue(int(rect.top()) - onScreen);
        return;
    }
    const diff::Text& text = shownText();
    int block = -1;
    for (int i = 0; i < text.blocks.size(); ++i)
        if (text.blocks[i].first <= line) block = i;
    if (block < 0) return;
    // Обратно: ищем блок ПОКАЗАННОГО документа, взятый из этого блока слепка.
    QTextBlock target;
    int number = 0;
    for (QTextBlock b = document()->begin(); b.isValid(); b = b.next(), ++number)
        if (number < diffSource_.size() && diffSource_[number] == block) {
            target = b;
            break;
        }
    if (!target.isValid()) return;
    const QRectF rect = document()->documentLayout()->blockBoundingRect(target);
    verticalScrollBar()->setValue(int(rect.top()) - onScreen);
}

void NoteEditor::dropDiffDocuments() {
    for (std::shared_ptr<QTextDocument>& doc : current_.diffDocs) retireDocument(std::move(doc));
    for (QVector<diff::Mark>& marks : current_.diffDocMarks) marks.clear();
    for (QVector<int>& source : current_.diffDocSource) source.clear();
    current_.diffSlot = -1;
}

std::shared_ptr<QTextDocument> NoteEditor::buildDiffDocument(int slot,
                                                             QVector<diff::Mark>* marks,
                                                             QVector<int>* source) {
    const bool base = (slot & 1) != 0;
    const bool plain = (slot & 2) != 0;
    // Сравнение выбирается СТОРОНОЙ: показанная сторона всегда «after».
    const diff::Result& result = base ? current_.diffReverse : current_.diffResult;
    auto doc = std::make_shared<QTextDocument>();
    source->clear();
    if (plain) {
        diff::buildPlainDocument(result, *doc, marks);
        return doc;   // у этого вида блок и есть строка сравнения
    }
    // ПОКАЗЫВАЕМ ИЛЛЮСТРИРОВАННУЮ КОПИЮ, а не сам слепок. Слепок (snapshot,
    // base) — истина, он лежит рядом неизменным, и по нему работает
    // восстановление; копия существует только ради показа, и всё дорисованное
    // живёт в ней.
    const diff::Text& front = base ? current_.baseText : current_.snapshotText;
    const diff::BlockMarks blocks =
        current_.diffReady ? diff::blockMarks(result, front.blocks) : diff::BlockMarks{};
    const diff::Illustrated shown =
        diff::illustrate(base ? current_.base : current_.snapshot, blocks);
    buildDocument(shown.blocks, *doc);
    // Метка блока документа — из метки блока копии; соответствие «логический
    // блок → блок документа» не один к одному (литеральные лежат построчно).
    marks->clear();
    for (int ir : irIndexOfEveryBlock(*doc)) {
        marks->append(ir >= 0 && ir < shown.blockMark.size() ? shown.blockMark[ir]
                                                            : diff::Mark::Same);
        source->append(ir >= 0 && ir < shown.sourceBlock.size() ? shown.sourceBlock[ir] : -1);
    }

    // Дорисованные строки — цветом разности: они не текст заметки, и читаться
    // как текст заметки не должны. Цвет ставим ПОСЛЕ сборки, прямо в документе:
    // в IR цвета нет вовсе (там курсив), а документ этот и есть копия — портить
    // в нём нечего.
    {
        QTextCursor paint(doc.get());
        int number = 0;
        for (QTextBlock block = doc->begin(); block.isValid(); block = block.next(), ++number) {
            if (number >= source->size() || source->at(number) >= 0) continue;
            if (block.text().isEmpty()) continue;
            QTextCharFormat colour;
            colour.setForeground(settings().look.diffRemoved);
            paint.setPosition(block.position());
            paint.setPosition(block.position() + block.length() - 1, QTextCursor::KeepAnchor);
            paint.mergeCharFormat(colour);
        }
    }
    return doc;
}

void NoteEditor::renderDiff(const DiffSpot& keep) {
    if (!inHistory()) return;
    const int slot = diffSlotNow();
    if (current_.diffSlot == slot) return;   // этот уже в поле

    // Подсветка находок держится курсорами в документе, а документ сейчас
    // сменится: запоминаем запрос и ставим подсветку заново на новом. Без
    // этого смена стороны гасила поиск молча.
    const QString query = current_.search.text();
    const bool caseSensitive = current_.search.caseSensitive();
    clearMatches();

    // Показ слепка — не правка человека: цепочки отмены здесь нет вовсе, она
    // отложена вместе с живой заметкой.
    const bool wasSuspended = recordingSuspended_;
    recordingSuspended_ = true;
    struct Restore {
        NoteEditor* self;
        bool was;
        ~Restore() { self->recordingSuspended_ = was; }
    } restore{this, wasSuspended};

    // ПРЕЖНИЙ ДОКУМЕНТ ОСТАЁТСЯ В СВОЁМ СЛОТЕ, а не уничтожается: на него мы
    // ещё вернёмся следующим же Tab. Отсюда и вся быстрота переключения —
    // строится каждая сторона по одному разу.
    if (!current_.diffDocs[size_t(slot)])
        current_.diffDocs[size_t(slot)] =
            buildDiffDocument(slot, &current_.diffDocMarks[size_t(slot)],
                              &current_.diffDocSource[size_t(slot)]);

    showDiffSlot(slot);
    current_.diffSlot = slot;
    diffMarks_ = current_.diffDocMarks[size_t(slot)];
    diffSource_ = current_.diffDocSource[size_t(slot)];
    // Документ собран не через rebuild, значит заплатке опереться не на что.
    note_->invalidateBuilt();
    applyContentWidth();
    document()->setModified(false);

    diffGoToSpot(keep);
    if (!query.isEmpty()) findMatches(query, caseSensitive);
    viewport()->update();
}

void NoteEditor::setDiffPlainView(bool on) {
    if (diffPlainView_ == on) return;
    // МЕСТО СНИМАЕТСЯ ДО СМЕНЫ ПРИЗНАКА. Иначе оно считается по новым правилам
    // на старом документе: у вида «как под капотом» строка — это номер блока, у
    // вида с полосками — блок IR, и перепутанные местами они дают то соседнюю
    // строку, то начало документа. Владелец это и увидел.
    const DiffSpot keep = diffSpotAtCaret();
    diffPlainView_ = on;
    renderDiff(keep);
    emit diffViewChanged(on);
}

void NoteEditor::setDiffPeek(bool on) {
    if (diffPeek_ == on) return;
    const DiffSpot keep = diffSpotAtCaret();   // та же причина, что и у вида
    diffPeek_ = on;
    renderDiff(keep);
    emit diffSideChanged(on);
}

void NoteEditor::setDiffFromFresh(bool on) {
    if (diffFromFresh_ == on) return;
    diffFromFresh_ = on;
    if (!inHistory() || current_.historyIndex < 0) return;
    // База сменилась, слепок тот же — место держим.
    const DiffSpot keep = diffSpotAtCaret();
    computeDiff(current_.historyIndex);
    renderDiff(keep);
}

HistorySearchReport NoteEditor::searchNoteHistory(const QString& text) {
    HistorySearchReport report;
    ZNoteHistory& history = inHistory() && current_.live != nullptr ? current_.live->history()
                                                                    : note_->history();
    if (!history.available()) return report;
    // Обращение к истории — значит и чистка: искать надо по уже вычищенному
    // журналу, иначе один и тот же текст найдётся в трёх дубликатах.
    history.compressOnce();
    return zametti::searchNoteHistory(history.file(), history.noteId(), makeQuery(text));
}

void NoteEditor::showBlockInGolden(const QTextBlock& block) {
    if (!block.isValid()) return;
    QTextCursor place(block);
    setTextCursor(place);
    revealInGolden(document()->documentLayout()->blockBoundingRect(block));
}

void NoteEditor::paintEvent(QPaintEvent* event) {
    NoteView::paintEvent(event);
    if (!inHistory() || diffMarks_.isEmpty()) return;

    // Полоска на поле, у самого края окна: она отвечает на вопрос «сюда
    // смотреть?», и место ей там, где взгляд ведёт строку, а не внутри текста.
    // Координаты вьюпорта, без сдвига по горизонтали: полоса стоит у кромки
    // окна и при прокрутке вбок не уезжает.
    QPainter painter(viewport());
    const QAbstractTextDocumentLayout* layout = document()->documentLayout();
    const int scroll = verticalScrollBar()->value();
    const qreal width = qMax(1.0, settings().look.diffBarWidth * zoom());
    for (QTextBlock block = document()->begin(); block.isValid(); block = block.next()) {
        const int number = block.blockNumber();
        if (number >= diffMarks_.size()) break;
        const diff::Mark mark = diffMarks_[number];
        if (mark == diff::Mark::Same) continue;
        const QRectF rect = layout->blockBoundingRect(block);
        const qreal top = rect.top() - scroll;
        if (top > viewport()->height()) break;
        if (top + rect.height() < 0) continue;
        QColor colour = mark == diff::Mark::Added      ? settings().look.diffAdded
                        : mark == diff::Mark::Removed  ? settings().look.diffRemoved
                                                       : settings().look.diffChanged;
        painter.fillRect(QRectF(width, top, width, rect.height()), colour);
    }
}

bool NoteEditor::focusNextPrevChild(bool next) {
    if (inHistory()) return false;   // Tab тут меняет вид, а не фокус
    return NoteView::focusNextPrevChild(next);
}

bool NoteEditor::diffStep(bool forward) {
    if (!inHistory() || diffMarks_.isEmpty()) return false;
    const int blocks = document()->blockCount();
    const int from = textCursor().blockNumber();
    // По кругу: дошли до края — начинаем сначала. Ходьба по изменениям без
    // круга каждый раз упирается в конец и молчит.
    for (int step = 1; step <= blocks; ++step) {
        const int at = ((forward ? from + step : from - step) % blocks + blocks) % blocks;
        if (at >= diffMarks_.size() || diffMarks_[at] == diff::Mark::Same) continue;
        // Соседние блоки одного изменения — одно место: встаём на его начало,
        // иначе F4 шагал бы по строкам внутри одного правленого куска.
        int start = at;
        if (forward)
            while (start > 0 && start - 1 < diffMarks_.size() &&
                   diffMarks_[start - 1] != diff::Mark::Same && start - 1 != from)
                --start;
        showBlockInGolden(document()->findBlockByNumber(start));
        return true;
    }
    return false;
}

void NoteEditor::leaveHistory() {
    if (!inHistory()) return;
    // Всё, что накопилось на слепке, к живой заметке отношения не имеет.
    // Подглядывание — состояние на время удержания клавиши, и переживать
    // режим оно не должно ни в каком виде.
    diffPeek_ = false;
    diffMarks_.clear();
    dropDiffDocuments();

    installNote(std::move(current_.live));
    setReadOnly(false);
    emit historyModeChanged(false);
}

bool NoteEditor::historyStepBack() {
    if (!inHistory() && !enterHistory()) return false;
    int at = current_.historyIndex - 1;
    while (at >= 0 && !current_.timeline.entries[at].hasSnapshot()) --at;
    if (at < 0) return false;   // дальше в прошлое некуда: остаёмся где были
    return showSnapshot(at);
}

bool NoteEditor::historyStepForward() {
    if (!inHistory()) return false;
    int at = current_.historyIndex + 1;
    while (at < current_.timeline.entries.size() && !current_.timeline.entries[at].hasSnapshot()) ++at;
    if (at >= current_.timeline.entries.size()) {
        // Дальше последнего слепка — живая версия. Это и есть выход из режима
        // хронологическим шагом вперёд.
        leaveHistory();
        return true;
    }
    return showSnapshot(at);
}

qint64 NoteEditor::restoreShownSnapshot(bool* alreadyCurrent) {
    if (alreadyCurrent != nullptr) *alreadyCurrent = false;
    if (!inHistory() || current_.historyIndex < 0) return 0;
    const qint64 source = current_.timeline.entries[current_.historyIndex].time;

    // Тело слепка берём ИЗ РАЗОБРАННОГО СЛЕПКА, а не из поля редактора. В поле
    // сейчас может лежать что угодно из того, что показывает режим: документ с
    // заглушками «удалено: N строк», вид «как под капотом» построчно или вовсе
    // вторая сторона сравнения по зажатому Alt. Восстанавливать надо ту запись,
    // которую человек выбрал в таймлайне, а не то, чем она сейчас нарисована.
    std::vector<Piece> body = current_.snapshot;

    leaveHistory();

    // Слепок и есть нынешняя версия — восстанавливать нечего. Сравниваем тела,
    // без меты: восстановление её и не трогает, а штамп modified у слепка свой
    // и разошёлся бы всегда.
    if (writePieces(body) == writePieces(piecesOf(*document()))) {
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
    // ВОССТАНОВЛЕНИЕ — ОБЫЧНАЯ ПРАВКА, и отменяется обычным Ctrl+Z (об этом
    // сказано и выше по тексту). Значит и пересборка здесь идёт правкой: внутри
    // скобки, одним шагом. Полная сборка на её месте сбрасывала бы стек — то
    // есть человек, вернувшийся из истории, терял бы возможность передумать.
    {
        QTextCursor group(document());
        group.beginEditBlock();
        rebuild(body, textCursor().position(), viewAnchor(), nullptr, /*asEdit=*/true);
        group.endEditBlock();
    }
    document()->setModified(true);
    current_.undoRun = false;

    // Ближайшее сохранение станет записью restore со ссылкой на источник.
    note_->history().markNextSaveAsRestore(source);
    save(false);
    return source;
}

// Правила отбора записей взяты из общего свода (store/history_rules.h): тем же
// кодом чистится и старая история. Здесь — только числа из настроек.
zametti::history::Rules NoteEditor::historyRules() {
    const ZSettings::History& history = settings().history;
    zametti::history::Rules rules;
    rules.mergeChars = qMax(0, history.historyMergeChars);
    rules.mergeHours = qMax(1, history.historyMergeHours);
    rules.ignoreAge = false;   // живая запись смотрит только на свежие записи
    return rules;
}

void NoteEditor::save(bool interactive, bool force) {
    if (!note_->hasPath()) return;
    // В РЕЖИМЕ ИСТОРИИ НЕ ПИШЕМ НИЧЕГО. В поле лежит слепок, а не заметка:
    // записать его — значит откатить заметку к прошлому молча, а с этапа 10 ещё
    // и унести на диск заглушки «удалено: N строк». Живая заметка отложена
    // целиком и уже сохранена — вход в режим её и записывает.
    //
    // Дверь эта не выдуманная: и выход из программы, и уход из заметки зовут
    // save с force, не спрашивая признак «изменён».
    if (inHistory()) return;
    if (!force && !document()->isModified()) return;
    // До записи: сохранение умеет пересобрать документ из перечитанного файла,
    // и шаг серии остался бы без содержимого.

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
    const NoteHeader metaBefore = note_->meta();
    if (note_->meta().present() && stampModifiedOnSave_)
        note_->meta().set("modified", store::isoNow().toStdString());

    std::vector<Piece> fileIr;
    QByteArray candidate = noteBytes(*document(), note_->meta(), nullptr, &fileIr);
    if (!note_->lastSaved().isEmpty() && sameApartFromModified(candidate, note_->lastSaved())) {
        note_->setMeta(metaBefore);
        document()->setModified(false);
        return;
    }

    // ЛЕНИВАЯ МИГРАЦИЯ ВРЕМЁН (этап 15) — и она стоит ЗДЕСЬ, ниже решения
    // «сохранять нечего», а не выше. Метки переписываются в новом виде
    // (ISO-8601 с офсетом) на всяком сохранении, но САМА миграция сохранения
    // не вызывает: иначе открытие старой заметки, где человек ничего не
    // тронул, переписывало бы файл и заводило запись в истории — ровно то, чего
    // не должно случаться никогда. Я на этом и попался: набор поймал семь
    // проверок в трёх местах.
    //
    // Момент при этом не меняется, меняется представление. Специального
    // прохода по хранилищу нет: заметки переезжают по мере того, как их правят.
    //
    // Метку, у которой офсет УЖЕ ЕСТЬ, не трогаем вовсе: в ней записан
    // локальный контекст того, кто её ставил («у него было 21:40»), и перевод
    // в свою зону этот контекст стёр бы — ровно ради него формат и менялся.
    if (note_->meta().present()) {
        bool moved = false;
        for (const char* key : {"created", "modified"}) {
            const std::string had = note_->meta().get(key);
            if (had.empty()) continue;
            const QDateTime moment = store::parseNoteTime(had);
            if (!moment.isValid()) continue;   // чужая строка — не наша забота
            if (moment.timeSpec() == Qt::OffsetFromUTC || moment.timeSpec() == Qt::TimeZone)
                continue;
            const QString fresh = store::isoWithOffset(moment.toLocalTime());
            if (fresh.isEmpty() || fresh.toStdString() == had) continue;
            note_->meta().set(key, fresh.toStdString());
            moved = true;
        }
        // Пересобираем байты только если что-то и правда переехало: лишняя
        // сериализация большой заметки — это миллисекунды на каждое
        // автосохранение.
        if (moved) candidate = noteBytes(*document(), note_->meta(), nullptr, &fileIr);
    }

    // Отпечаток того, что в файле, мы знаем — значит «не изменилось ли»
    // решается без чтения файла.
    const SaveOutcome outcome =
        saveDocument(*document(), note_->path(), rescueTimestamp(), nullptr, note_->meta(),
                     note_->digest(), &fileIr, &candidate);
    if (outcome.result == SaveResult::Written || outcome.result == SaveResult::Unchanged) {
        document()->setModified(false);
        current_.lastComplaint.clear();
        // Что теперь в файле, известно из самой записи: отпечаток посчитан по
        // тому буферу, который туда и ушёл. Раньше файл ради этого читался
        // заново — на каждое автосохранение.
        note_->markWritten(outcome.digest, outcome.written);
        watchFile();

        // Файл может прочитаться богаче документа: голую ссылку человек набирает
        // текстом, а разбор делает из неё ссылку. Догоняем — иначе то, что на
        // диске, и то, что на экране, расходились бы до перезагрузки. Шага
        // истории здесь нет: содержимое то же самое, изменилась только разметка
        // внутри строки.
        if (outcome.differsFromDocument) {
            const int cursor = textCursor().position();
            const ViewAnchor anchor = viewAnchor();
            // ПРИКЛЕИВАЕТСЯ К ТОЙ ПРАВКЕ, КОТОРУЮ ДОГОНЯЕТ, а не заводит свой
            // шаг и не сбрасывает стек. Прежде здесь стояла полная пересборка:
            // она чистила отмену, и человек, записавший заметку, терял всю
            // историю правок — тем вернее, чем чаще срабатывало автосохранение.
            QTextCursor join(document());
            join.joinPreviousEditBlock();
            rebuild(outcome.reread, cursor, anchor, nullptr, /*asEdit=*/true);
            join.endEditBlock();
        }
        // Шаг истории — по факту записи, а не по факту нажатия: Unchanged
        // означает, что на диске уже ровно это, и второй одинаковый слепок
        // подряд в журнале не нужен (дедупликация тут бесплатна, потому что
        // сравнение отпечатков уже сделано выше).
        if (outcome.result == SaveResult::Written)
            note_->history().record(journal::Kind::Save, outcome.written);
        // Признак «восстановление» гасим при любом исходе записи: он относится
        // к одному ближайшему сохранению, а не «пока не сработает».
        note_->history().clearPendingRestore();

        // Самопроверка. Окна с вопросом нет и не будет: оно повторялось на
        // каждом автосохранении, и человек его выключал — а вместе с ним
        // терял и сам сигнал. Теперь признак живёт у заметки, полоса рисует
        // по нему красную звёздочку, и гаснет она сама, как только очередная
        // запись сойдётся.
        const bool failed = !outcome.rescuePath.isEmpty();
        if (note_->setSelfCheckFailed(failed))
            emit statsChanged();   // полосе пора перерисоваться
        if (failed)
            std::fprintf(stderr, "%s\n", outcome.message.toUtf8().constData());

        // Слова и строки после записи. Документ с момента записи не менялся,
        // значит обход отвечает ровно тому, что на экране и что в файле.
        refreshStats(documentStats(*document()));

        // Файл на диске стал другим: средней колонке пора перечитать заголовок,
        // начало текста и дату. Сигнал, а не прямой вызов: редактор про список
        // ничего не знает и знать не должен.
        emit fileSaved(note_->path());
        return;
    }

    std::fprintf(stderr, "%s\n", outcome.message.toUtf8().constData());
    // Одну и ту же беду показываем один раз: автосохранение повторяется по
    // таймеру, и окно с ошибкой раз в полторы секунды — это пытка.
    if (!interactive || outcome.message == current_.lastComplaint) return;
    // Файл, про который человек попросил не напоминать, — молчим до конца
    // сессии: беда известна, он правит её руками.
    if (mutedComplaints_.contains(note_->path())) return;
    current_.lastComplaint = outcome.message;

    QMessageBox box(QMessageBox::Warning, QStringLiteral("zametti"), outcome.message,
                    QMessageBox::Ok, this);
    auto* mute = new QCheckBox(
        QStringLiteral("больше не предупреждать про этот файл в этой сессии"), &box);
    box.setCheckBox(mute);
    box.exec();
    if (mute->isChecked()) mutedComplaints_.insert(note_->path());
}

void NoteEditor::wheelEvent(QWheelEvent* event) {
    releaseCaret();
    NoteView::wheelEvent(event);
}

// Доля высоты окна, на которой сейчас стоит каретка; окна ещё нет (заметка
// открыта до show()) или каретка вне окна — золотое сечение: там ей и место при
// открытии.
qreal NoteEditor::heldRatioNow() const {
    const int height = viewport()->height();
    const qreal golden = qBound(0.0, settings().look.focusRatio, 0.9);
    if (!isVisible() || height <= 0) return golden;
    const int top = cursorRect().top();
    if (top < 0 || top >= height) return golden;
    return qreal(top) / height;
}

void NoteEditor::keepCaretInView() {
    if (!holdingCaret_) return;
    const int height = viewport()->height();
    if (height <= 0) return;
    // Высота может быть и от окна-заготовки (до show()): ставим по ней, а
    // показ и перекладка окна поставят заново — по настоящей.
    const qreal wantTop = heldRatio_ * height;
    const int dy = int(cursorRect().top() - wantTop);
    if (dy == 0) return;
    movingView_ = true;
    verticalScrollBar()->setValue(verticalScrollBar()->value() + dy);
    movingView_ = false;
}

void NoteEditor::resizeEvent(QResizeEvent* event) {
    NoteView::resizeEvent(event);
    // Окно поменяло размер (в том числе впервые появилось): каретка — на той же
    // доле новой высоты.
    keepCaretInView();
}

void NoteEditor::showEvent(QShowEvent* event) {
    // QTextEdit при первом показе сам зовёт ensureCursorVisible — минимальную
    // прокрутку, которая ставит каретку впритык к нижней кромке. Возвращаем её
    // на удерживаемую долю окна сразу же.
    QTextBrowser::showEvent(event);
    keepCaretInView();
}

void NoteEditor::rememberCaretFor(const QString& path, int cursor, int anchor) {
    if (path.isEmpty()) return;
    caretMemory_[path] = {cursor, anchor};
}

EscapeAction escapeActionFor(bool languageEditorOpen, bool editingTable, bool findBarVisible) {
    if (languageEditorOpen) return EscapeAction::CloseLanguageEditor;
    // Правка таблицы закрывается раньше панели поиска по той же причине, по
    // которой раньше неё закрывается поле языка: сперва уходит то, что открыто
    // ПОВЕРХ текста и держит каретку.
    if (editingTable) return EscapeAction::LeaveTableEdit;
    if (findBarVisible) return EscapeAction::CloseFindBar;
    return EscapeAction::Nothing;
}

// Каретка ушла из раскрытой формулы — сворачиваем её обратно в объект.
//
// Своего признака «сейчас правят» у формулы больше нет и не нужно: раскрытая
// формула — это обычный абзац с исходником, и узнать её можно у самого
// документа. Прежний editedFormula_ был именно таким признаком, и жил он
// потому, что исходник лежал в блоке всегда.
void NoteEditor::leaveFormulaEdit() {
    runNoteEdit([](ZDocument& note, QTextCursor& at) { return note.closeFormula(at); });
}

void NoteEditor::leaveTableEdit() {
    if (editedTable() < 0) return;
    setEditedTable(-1);
    reparseAfterTableEdit();
}

// ПЕРЕЧИТАТЬ ЗАМЕТКУ ПОСЛЕ ПРАВКИ ТАБЛИЦЫ — и только после неё.
//
// Правило владельца: состояние таблицы проверяется на ВЫХОДЕ из правки,
// промежуточные состояния законно бывают не таблицами. Но есть и вторая
// причина, техническая: набор внутри дословного куска склеивает его строки в
// один абзац с мягкими переносами (та же склейка, что нашлась на этапе 11 в
// блоках кода). В файле от этого ничего не меняется — мягкие переносы
// сериализуются переводами строк, — а вот в живом документе таблицы больше
// нет, и сетка не возвращается до перечитывания заметки.
//
// Поэтому на выходе из правки заметка перечитывается ровно так, как при
// открытии файла: текст → parse → сборка. Шага истории это не заводит: текст
// не изменился ни на байт, изменилось только его разбиение на блоки.
void NoteEditor::reparseAfterTableEdit() {
    const int at = textCursor().position();
    std::vector<Piece> fresh;
    NoteHeader ignored;
    parsePieces(writePieces(piecesOf(*document())), fresh, ignored);

    recordingSuspended_ = true;
    // Перечитывание после правки таблицы — тоже правка: одним шагом отмены и
    // без сброса стека.
    {
        QTextCursor group(document());
        group.beginEditBlock();
        rebuild(fresh, at, viewAnchor(), &fresh, /*asEdit=*/true);
        group.endEditBlock();
    }
    recordingSuspended_ = false;

    // Каретка встаёт НА таблицу, если она снова таблица: выйти из правки —
    // значит вернуться к выбранной таблице, а не улететь в текст.
    const BlockObject object = objectOf(textCursor().block());
    if (object.kind == ObjectKind::Table)
        setTextCursor(QTextCursor(document()->findBlockByNumber(object.last)));
}

void installHistoryShortcuts(QWidget* window, NoteEditor& editor) {
    const auto add = [window](const QKeySequence& keys, auto&& slot) {
        auto* key = new QShortcut(keys, window);
        QObject::connect(key, &QShortcut::activated, window, slot);
        return key;
    };
    // Ходьба по изменённым местам. Сочетания — из конфига: на маке F4 занята
    // системой, и владелец выберет свой аккорд правкой конфига, а не кода.
    // Через точку с запятой их можно перечислить несколько.
    const auto walk = [&](const QString& spec, bool forward) {
        for (const QString& part : spec.split(QLatin1Char(';'))) {
            const QKeySequence keys(part.trimmed());
            if (keys.isEmpty()) continue;
            add(keys, [&editor, forward] { editor.diffStep(forward); });
        }
    };
    walk(settings().editor.diffPreviousKey, false);
    walk(settings().editor.diffNextKey, true);

    // Tab меняет сторону сравнения. Ярлык включается ТОЛЬКО в режиме истории:
    // в обычной работе Tab принадлежит переходу фокуса, и отбирать его у всего
    // окна нельзя.
    QShortcut* side = add(QKeySequence(Qt::Key_Tab),
                          [&editor] { editor.setDiffPeek(!editor.diffPeek()); });
    side->setEnabled(editor.inHistory());
    QObject::connect(&editor, &NoteEditor::historyModeChanged, side,
                     [side](bool on) { side->setEnabled(on); });
}

}  // namespace zametti
