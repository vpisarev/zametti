#include "editor_widget.h"

#include "image_read.h"

#include "key_binding.h"

#include "block_object.h"
#include "caption_editor.h"
#include "zapp.h"
#include "lang_editor.h"

#include "document_builder.h"
#include "document_saver.h"
#include "doc_model.h"
#include "editor_ops.h"
#include "image_insert.h"
#include "marker.h"
#include "serializer.h"
#include "settings.h"
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

namespace zametti {

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
        if (!note_->search().empty()) showMatchHighlights();
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
    // Разбор настройки-списка — общий для всех режимов (key_binding.h).
    moveUpKeys_ = keySequencesOf(settings().editor().moveUpKey());
    moveDownKeys_ = keySequencesOf(settings().editor().moveDownKey());
    // Сочетаний на команду может быть несколько: через точку с запятой.
    const auto bind = [this](const QString& keys, const NoteOp& op) {
        for (const QKeySequence& sequence : keySequencesOf(keys)) bindings_.push_back({sequence, op});
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
    // Зачёркивание своего стандартного сочетания не имеет; Ctrl+/ взят из брифа.
    inlineBindings_.push_back(
        {QKeySequence(QStringLiteral("Ctrl+/")), ZDocument::Style::Strike});

    // Формулы с клавиатуры: строчная и выключная. Сочетания предложены
    // владельцем и стоят рядом с прочими пометками начертания.
    bind(QStringLiteral("Ctrl+4"),
         [](ZDocument& note, QTextCursor& at) { return note.toggleInlineMath(at); });
    bind(QStringLiteral("Ctrl+Shift+4"),
         [](ZDocument& note, QTextCursor& at) { return note.toggleDisplayMath(at); });

    // Автозамены из конфига: сочетание и знак, который оно вставляет.
    // Сочетаний на одну замену может быть несколько, через точку с запятой —
    // как и у любой другой команды.
    for (const auto& [keys, text] : settings().editor().specialKeys()) {
        if (text.isEmpty()) continue;
        for (const QKeySequence& sequence : keySequencesOf(keys)) specialKeys_.push_back({sequence, text});
    }

    // СОЧЕТАНИЙ У УРОВНЕЙ ЗАГОЛОВКА БОЛЬШЕ НЕТ (решение владельца): заголовок
    // набирают автозаменой — «# » и пробел, — а не аккордом, и семь аккордов
    // Ctrl+Shift+0…6 занимали ряд цифр целиком без всякой нужды. В меню уровни
    // остались; ушли только клавиши.
    //
    // Ушли не просто так: ряд был ЗАНЯТ, и это уже стоило одной команды.
    // Выключная формула получила Ctrl+Shift+4 — и не работала ни разу, потому
    // что QAction с сочетанием срабатывает РАНЬШЕ keyPressEvent, и четвёрка
    // делала заголовок 4-го уровня. Набор снимков формул краснел именно этим.
    //
    // Прежний довод в пользу QAction остаётся верным и записан здесь, чтобы не
    // изобретать его заново: с зажатым Shift event->key() приходит знаком
    // верхнего регистра («@» вместо «2» на латинской раскладке, кавычка на
    // русской), и сравнение с Qt::Key_2 не срабатывает никогда. Кому понадобится
    // Ctrl+Shift+цифра — сопоставлять сочетание обязана Qt (QKeySequence), а не
    // разбор события руками. Ровно так это и делает bind() ниже.

    bind(settings().editor().toggleTaskKey(),
         [](ZDocument& note, QTextCursor& at) { return note.toggleTask(at); });
    bind(settings().editor().makeBulletKey(),
         [](ZDocument& note, QTextCursor& at) { return note.makeBullet(at); });
    bind(settings().editor().makeOrderedKey(),
         [](ZDocument& note, QTextCursor& at) { return note.makeOrdered(at); });
    bind(settings().editor().makeTaskKey(),
         [](ZDocument& note, QTextCursor& at) { return note.makeTask(at); });
    bind(settings().editor().makeParagraphKey(),
         [](ZDocument& note, QTextCursor& at) { return note.makeParagraph(at); });
    bind(settings().editor().makeCommentKey(),
         [](ZDocument& note, QTextCursor& at) { return note.toggleComment(at); });

    autosave_.setSingleShot(true);
    connect(&autosave_, &QTimer::timeout, this, [this] {
        // РАСКРЫТАЯ ТАБЛИЦА ПОД КАРЕТКОЙ — автосохранение ЖДЁТ выхода из правки:
        // промежуточные состояния законно не таблицы, а файл обязан быть
        // согласован сам с собой (см. save()).
        if (caretInOpenObject()) {
            autosave_.start(settings().editor().autosaveDelayMs());
            return;
        }
        save(true);
    });
    // ТИШИНА ТОЖЕ КОНЧАЕТ СЕРИЮ НАБОРА — четвёртое правило границы, и оно
    // единственное, которое не выводится из самого текста. Отмерив паузу,
    // просто помечаем серию оборванной: следующая буква откроет новый шаг.
    typingPause_.setSingleShot(true);
    connect(&typingPause_, &QTimer::timeout, this, [this] { current_.runBroken = true; });
    // ЧИСЛА ДОГОНЯЮТ ДОКУМЕНТ ЧЕРЕЗ ПАУЗУ, а не на ближайшей записи (просьба
    // владельца).
    //
    // Считаем ТОЛЬКО если с прошлого счёта и правда правили: у свежих чисел
    // обход отнял бы миллисекунды впустую.
    //
    // Гонок здесь нет и быть не может: и правки документа, и этот тик приходят
    // в поток GUI, а цикл событий один и не реентерабелен — пока считаем,
    // ничей обработчик правки не выполняется (вопрос владельца).
    statsRecount_.setSingleShot(true);
    connect(&statsRecount_, &QTimer::timeout, this, [this] {
        if (note_->statsFresh()) return;
        refreshStats(documentStats(*document()));
    });
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
    // ТО ЖЕ У ТАБЛИЦЫ: ушли из раскрытого дословного блока — судья файла решает,
    // что он теперь (closeTable). Оба хука — только в живой заметке: в режиме
    // истории и в читалке править нечего.
    // Спрашиваем заметку ДО правки: пустая скобка правки поднимает ревизию
    // документа (замер Qt), а по ревизии судят кэши — счёт слов, найденное,
    // собранные блоки. Пока вопроса не было, три холостых захода на каждый ход
    // каретки протухали их все.
    if (!isReadOnly() && !current_.lastLine.isNull() &&
        current_.lastLine.document() == document() &&
        current_.lastLine.blockNumber() != textCursor().blockNumber() &&
        note_->doc().mayHaveOpenObject(current_.lastLine)) {
        QTextCursor left = current_.lastLine;
        if (!runNoteEdit(
                [&left](ZDocument& note, QTextCursor&) { return note.closeInlineFormula(left); }))
            if (!runNoteEdit(
                    [&left](ZDocument& note, QTextCursor&) { return note.closeFormula(left); }))
                runNoteEdit([&left](ZDocument& note, QTextCursor&) { return note.closeTable(left); });
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
    // ГДЕ ОТМЕНИЛОСЬ. Пока идёт Ctrl+Z или Ctrl+Shift+Z, копим границы того,
    // что Qt вернул: туда и поедет каретка (см. undo/redo). Своей позиции у
    // команды отмены нет — Qt ставит каретку в КОНЕЦ отменённого куска, и на
    // переключении задачи это оказывалась строка НИЖЕ той, где человек щёлкал
    // (жалоба владельца: «курсор прыгает на строку вниз, хотя бы там и не
    // были»).
    if (undoTouch_.watching && (charsRemoved != 0 || charsAdded != 0)) {
        undoTouch_.from = undoTouch_.from < 0 ? position : qMin(undoTouch_.from, position);
        undoTouch_.to = qMax(undoTouch_.to, position + charsAdded);
    }
    // НАЙДЕННОЕ ПОСЛЕ ПРАВКИ ПРОТУХЛО — поиск повторится сам. То же правило,
    // что в режиме исходника (решение владельца: «смещения изменились и
    // количество изменилось»), и заводится тем же признаком: изменился ТЕКСТ,
    // а не формат.
    if (charsRemoved != 0 || charsAdded != 0) scheduleResearch();
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
    note_->doc().setUndoEnabled(true);
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
    // ПОДСВЕТКА НАХОДОК СНИМАЕТСЯ ДО ПОДМЕНЫ ДОКУМЕНТА. Она держится курсорами
    // в прежнем документе, и Qt при следующем setExtraSelections спрашивает у
    // старых курсоров их прямоугольники — а документа к тому времени уже нет
    // (падение: поиск в заметке, открытой после другой, где искали). Найденное
    // при заметке остаётся (кэш поиска), и у той, что пришла, подсветка
    // ставится заново ниже.
    setExtraSelections({});
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
    // Найденное приехало вместе с заметкой (кэш поиска) — показать его.
    if (!note_->search().empty()) showMatchHighlights();

    // Таймеры перенастраиваются под новую заметку. Отложенный снимок — её
    // свойство и приехал вместе с ней; висящий от прошлой заметки таймер
    // отменяем, иначе он записал бы шаг в чужую цепочку.
    autosave_.stop();
    watchFile();
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

void NoteEditor::closeFile() {
    // ЗАМЕТКИ БОЛЬШЕ НЕТ — не «другая», а никакой. Нужно ровно одному: смене
    // хранилища. Открытая заметка принадлежит уходящему хранилищу, а её журнал
    // смотрит на него СЫРЫМ указателем, и пережить хранилище не имеет права.
    //
    // Не через openFile(""): тот читает файл и честно жалуется, что такого
    // файла нет, — а здесь нечему не удаваться.
    releaseCaret();
    // Правки на диск, пока ещё есть куда: дальше писать будет некуда.
    save(true, true);
    // Каретку — в память приложения. НЕ откладываем саму заметку в кэш: она из
    // хранилища, которое сейчас умрёт, и в кэше ей делать нечего.
    rememberCurrentCaretInApp();
    // ПОРЯДОК: сперва вид переезжает на новый (пустой) документ, и ТОЛЬКО
    // ПОТОМ прежняя заметка отпускается. Наоборот — падение: QTextEdit
    // отцепляется от прежнего документа внутри setDocument, а тот уже умер
    // вместе с заметкой (поймано на первом же переключении хранилища).
    //
    // У openFile этой беды нет не потому, что там порядок другой, а потому что
    // уходящая заметка там откладывается в кэш и живёт дальше; здесь её класть
    // некуда — она из хранилища, которое сейчас умрёт.
    auto fresh = std::make_shared<ZNote>();
    setDocument(fresh->doc().getDocument());
    note_ = std::move(fresh);
    watchFile();
    emit fileChanged(QString());
}

void NoteEditor::trimNoteCache() {
    const qint64 budget = qint64(qMax(1, settings().cache().documentCacheSizeMb())) * 1024 * 1024;
    // С хвоста, пока не уложились: самое давнее уходит первым. От вытесненной
    // заметки остаётся только место каретки — вот единственное место, где оно
    // попадает в общую карту. Каретка принадлежит заметке и живёт в её объекте;
    // карта — это ОСТАТОК объекта, а не второй источник правды о нём.
    while (!noteCache_.empty() && cachedNoteBytes() > budget) {
        const ZNote& going = *noteCache_.back();
        if (going.hasPath()) ZApp::instance().state().rememberCaret(going.id(), going.caret());
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
        ZApp::instance().state().rememberCaret(
            note_->id(),
            {textCursor().position(), textCursor().anchor(), verticalScrollBar()->value()});

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
    if (hashOf(writePieces(piecesOf(*document()), note_->header()).toUtf8()) != note_->digest()) return;

    const qint64 bytes = estimateDocumentBytes(*document());
    const qint64 budget = qint64(qMax(1, settings().cache().documentCacheSizeMb())) * 1024 * 1024;
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

bool NoteEditor::isArchivedNote() const {
    return note_ != nullptr && !note_->path().isEmpty() && note_->isArchived();
}

bool NoteEditor::isReadOnlyNote() const {
    if (note_ == nullptr || note_->path().isEmpty()) return false;
    // Своя пометка `access: read-only` — вопрос к самой заметке; пометка ПАПКИ
    // над ней — вопрос к хранилищу, у которого есть каталог. Спрашиваем обоих:
    // заметку открывают и без хранилища (одиночный файл), и тогда у неё есть
    // только своя строка в шапке.
    if (note_->isReadOnly()) return true;
    return storage_ != nullptr && storage_->isReadOnly(note_->id());
}

// РЕЖИМ ЗАМЕТКИ — производное состояние показа, и восстанавливает его ОДНА
// функция, которую зовут ОБА пути открытия (свежая сборка и возврат из кэша).
// Классический баг проекта иначе воспроизводится дословно: свежее открытие
// одно, переключился-вернулся — другое.
//
// Архивную и запертую заметку показывает не редактор, а ВИД (ReaderView) —
// отдельная страница стека, как история и исходник. Здесь остаётся страховка на
// случай ошибки в выборе страницы: редактор такую заметку правку не примет.
void NoteEditor::applyNoteMode() {
    setReadOnly(isArchivedNote() || isReadOnlyNote());
}

void NoteEditor::activateNote(bool takeFocus) {
    // Режим — первым делом. Оба пути открытия сходятся здесь.
    applyNoteMode();

    // СТАТИСТИКА ДОКУМЕНТА (слова, строки, картинки) ПРИ ПЕРЕКЛЮЧЕНИИ ЗАМЕТКИ
    // СЧИТАЕТСЯ ВСЕГДА (решение владельца). Она привязана к ревизии документа,
    // а ревизия растёт и от того, что правкой текста не является; заметка при
    // этом остаётся чистой и уезжает в кэш с уже протухшей статистикой.
    // Возврат к ней показывал «?» до первой правки — счесть было некому: кэш
    // идёт мимо сборки, а сборка была единственным местом счёта.
    //
    // Здесь сходятся ОБА пути открытия — свежая сборка и возврат из кэша, —
    // поэтому счёт живёт здесь, а не в них по отдельности (правило проекта:
    // производное состояние восстанавливает одна функция, и её зовут оба
    // пути). Обход стоит 3.2 мс на заметке в 239 КБ и приходится на смену
    // заметки — не на нажатие клавиши.
    //
    refreshStats(documentStats(*document()));

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
    // Каретку прежней заметки в виду больше не держим: заметка сменяется, и
    // всё, что дальше делается с документом — сборка, подмена, перекладка
    // полей, — не должно на каждое изменение высоты гонять каретку в вид.
    // Замер (zametti-bench big, «Карамазовы»): с поднятым признаком сборка
    // документа стоила 1940 мс против 396 — каждый вставленный блок менял
    // высоту, и на каждую высоту ensureCursorVisible заново верстал документ.
    releaseCaret();
    // ЗАМЕТКА СЕЙЧАС СМЕНИТСЯ — и это последний миг, когда прежняя ещё открыта.
    // Тому, у кого правки живут СНАРУЖИ документа (режим правки исходника:
    // истина там в тексте виджета), их надо успеть наложить до подмены, иначе
    // работа человека уедет вместе с заметкой. Сигнал именно «сейчас сменится»,
    // а не «сменилась»: после подмены накладывать уже некуда.
    if (path != note_->path()) emit fileAboutToChange(path);
    save(true, true);   // уходим из заметки: пробуем записать, не спрашивая признак

    std::string text;
    if (!ZStorage::readFileBytes(path, text)) {
        std::fprintf(stderr, "unreadable: %s\n", path.toUtf8().constData());
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
    if (!cachedNoteMatches(path, digest)) ZNote::canonicaliseFile(path, text, digest);
    adoptNoteAt(path);   // одна функция на оба пути показа: см. NoteView
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
    std::shared_ptr<ZJournal> noteJournal =
        storage_ == nullptr ? std::make_shared<ZJournal>()
                            : storage_->journalFor(ZStorage::idOfPath(path), historyRules());
    {
        const QDateTime when = QFileInfo(path).lastModified();
        noteJournal->ensureBaseline(fileBytes, when.isValid() ? when.toMSecsSinceEpoch() : 0);
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
    auto fresh = std::make_shared<ZNote>(path, fileBytes, digest, std::move(noteJournal));
    // Байты → шапка + тело разбирает и собирает сама заметка; документ ещё не
    // показан, и вёрстки при сборке нет вовсе (её включает getDocument).
    fresh->load(text);
    fresh->rememberCaret(ZApp::instance().state().caretOf(fresh->id()));
    installNote(std::move(fresh));
    watchFile();
    // Масштаб и ширину колонки свежему документу уже вернул installNote — здесь
    // только каретка и якорь. Каждый лишний пересчёт вида на документе с
    // формулами — лишний вызов движка (набор снимков формул это считает).
    // Статистику считает activateNote — одним местом на оба пути открытия,
    // поэтому здесь сборка её не считает.
    landAfterBuild(note_->caret().cursor, {}, /*statsDone=*/true);
    // Каретка, выделение, показ места и фокус — общей дорогой с отложенной
    // заметкой: два пути открытия, одно правило.
    activateNote(takeFocus);
    emit fileChanged(note_->path());
    // Архивная заметка БОЛЬШЕ НЕ УВОДИТ В РЕЖИМ ИСТОРИИ. Прежде уводила по
    // необходимости: в файле у неё лежал стаб, а тело — в журнале. Теперь тело
    // в файле, и она открывается обычным видом — только для чтения и на сером
    // (applyNoteMode). Кнопка истории у неё работает как у всякой другой.
    return true;
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
    // файл унесли: ждём, пока вернётся
    if (!ZStorage::readFileBytes(note_->path(), text)) return;
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
    note_->journal().record(ZJournal::Kind::External, note_->lastSaved());

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
    // ВНЕШНЯЯ ПРАВКА ПРИНИМАЕТСЯ ТЕМ ЖЕ ПУТЁМ, ЧТО И ВОЗВРАТ ИЗ РЕЖИМА
    // ИСХОДНИКА (решение владельца, сессия 9): ZDocument::applySourceText —
    // разбор как у файла, наложение только тронутыми кусками, ОДИН шаг отмены,
    // и Ctrl+Z возвращает то, что было до внешнего изменения (README обещает
    // это прямо). Прежде здесь была своя ветка — полная пересборка документа
    // правкой, — и две ветки разошлись: одна берегла неразрывные отступы,
    // другая нет. Разница осталась одна: шапку здесь забираем мы.
    NoteHeader fresh;
    ZDocument& doc = note_->doc();
    // Пока идёт режим исходника, заметка правку не принимает; внешняя — не
    // правка человека в окне, а новая истина файла, её пропускаем.
    const bool sourceMode = doc.sourceEditing();
    doc.setSourceEditing(false);
    const int hunks = doc.applySourceText(
        QString::fromUtf8(text.data(), qsizetype(text.size())), nullptr, &fresh);
    doc.setSourceEditing(sourceMode);
    Q_UNUSED(hunks);   // < 0 здесь не бывает: шапка разрешена, запасной путь внутри
    // Чужой редактор мог снести или испортить блок метаданных. Тихо принять
    // это нельзя: заметка потеряла бы родителя и дату создания, то есть уехала
    // бы в корень и «постарела». Прежние значения у нас в памяти — предлагаем
    // вернуть их одним действием, а решает человек.
    const NoteHeader previous = note_->header();
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

    note_->setHeader(fresh);
    document()->setModified(false);

    emit externalAdopted(note_->path());
    if (lost || !dropped.isEmpty()) {
        note_->rememberLostMeta(previous);
        emit metaDamaged(note_->path(), lost ? QStringList{QStringLiteral("whole block")} : dropped);
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
    NoteView::applyZoom(value);
}

void NoteEditor::refreshAppearance() {
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
    const int gap = qBound(0, qRound(docStyle().verticalMargin() * lineUnit), height / 3);

    QScrollBar* bar = verticalScrollBar();
    if (at.top() < gap) bar->setValue(bar->value() - (gap - at.top()));
    else if (at.bottom() > height - gap) bar->setValue(bar->value() + at.bottom() - height + gap);
}

bool NoteEditor::replaceCurrentMatch(const QString& with) {
    if (!note_->search().hasCurrent()) return false;
    const SearchHit hit = note_->search().hitAt(note_->search().current());
    const QTextCursor target = hit.cursor;
    bool done = false;
    if (hit.inObject() && isInlineFormulaChar(*document(), target.selectionStart())) {
        // Вхождение внутри СТРОЧНОЙ формулы: переписывается исходник ровно
        // этого объекта; судья тот же, что у закрытия.
        const int position = target.selectionStart();
        QTextCursor probe(document());
        probe.setPosition(position + 1);
        QString source = probe.charFormat().property(ObjectSourceProperty).toString();
        source.replace(hit.innerOffset, hit.innerLength, with);
        done = runNoteEdit([&](ZDocument& note, QTextCursor& cursor) {
            return note.rewriteInlineFormula(cursor, position, source);
        });
    } else if (hit.inObject()) {
        // Вхождение внутри объекта: переписать исходник и рассудить блок заново
        // тем же судьёй, что и при сворачивании (таблица могла перестать быть
        // таблицей, формула — формулой; это законно, в файл уйдёт написанное).
        const QTextBlock block = target.block();
        QString source = searchableTextOf(block);
        source.replace(hit.innerOffset, hit.innerLength, with);
        const int number = block.blockNumber();
        done = runNoteEdit([&](ZDocument& note, QTextCursor& cursor) {
            return note.rewriteObjectSource(cursor, number, source);
        });
    } else {
        // Замена одного вхождения — это НАБОР ПОВЕРХ ВЫДЕЛЕНИЯ, ровно тот же
        // глагол, что у клавиатуры: заводить ради неё второй путь незачем.
        const QTextCharFormat format = currentCharFormat();
        done = runNoteEdit([&](ZDocument& note, QTextCursor& cursor) {
            cursor.setPosition(target.selectionStart());
            cursor.setPosition(target.selectionEnd(), QTextCursor::KeepAnchor);
            return note.insertText(cursor, with, format);
        });
    }
    if (!done) return false;
    // Прежние курсоры недействительны, ищем заново и встаём на следующее
    // вхождение.
    const int at = note_->search().current();
    findMatches(note_->search().text(), note_->search().caseSensitive());
    if (!note_->search().empty()) goToMatch(at < note_->search().count() ? at : 0);
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

NoteEditor::ShotList NoteEditor::noteShots() const {
    ShotList out;
    const int caretBlock = textCursor().blockNumber();
    for (QTextBlock b = document()->begin(); b.isValid(); b = b.next()) {
        const BlockImageRef ref = blockImageRef(b);
        if (!ref.valid) continue;
        const QString abs = absoluteImagePath(ref.path);
        if (abs.isEmpty()) continue;
        if (b.blockNumber() == caretBlock) out.atCaret = int(out.paths.size());
        out.paths << abs;
        // Подпись — ПОКАЗАННАЯ: безымянную («image 3», имя от камеры) человек
        // под снимком не видит, и в просмотре ей тоже делать нечего.
        out.captions << ref.shownCaption();
    }
    return out;
}

void NoteEditor::undo() {
    // Дно стека — дальше шагаем в слепки журнала. Режим истории заводит
    // контроллер по этому сигналу; режим объявляет себя сам (баннер, заголовок
    // окна), и это же служит защитой от случайного глубокого отката.
    if (!document()->isUndoAvailable()) {
        emit historyRequested();
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
    undoTouch_.watch();
    QTextEdit::undo();
    undoTouch_.stop();
    recordingSuspended_ = false;
    landCaretWhereUndone();

    // Вид держится сам, но отменённая правка может оказаться за окном — тогда
    // её надо показать: человек нажал отмену, чтобы увидеть результат.
    showEditPlace(scrollBefore, /*jump=*/true);   // отмена может быть далеко от каретки
    document()->setModified(true);
    autosave_.start(settings().editor().autosaveDelayMs());
}

void NoteEditor::redo() {
    if (!document()->isRedoAvailable()) return;
    const int scrollBefore = verticalScrollBar()->value();
    current_.undoRun = true;
    // По тому же доводу, что и у отмены: возвращённое состояние каноническое,
    // убирать в нём нечего, а всякая правка обрубила бы следующий повтор.
    recordingSuspended_ = true;
    undoTouch_.watch();
    QTextEdit::redo();
    undoTouch_.stop();
    recordingSuspended_ = false;
    landCaretWhereUndone();
    showEditPlace(scrollBefore, /*jump=*/true);   // отмена может быть далеко от каретки
    document()->setModified(true);
    autosave_.start(settings().editor().autosaveDelayMs());
}

// КАРЕТКА ПОСЛЕ ОТМЕНЫ И ПОВТОРА — В НАЧАЛЕ ТОГО, ЧТО ВЕРНУЛОСЬ.
//
// Своей позиции у команды отмены нет: Qt ставит каретку в конец отменённого
// куска. Для набора это почти всегда то же место, а для правки, которая
// кончается на границе блоков (переключение задачи, смена маркера), — строка
// НИЖЕ той, где человек работал. Владелец сказал прямо: вниз — нелогично,
// пусть остаётся на своей строке или уходит вверх.
//
// Начало правки этому правилу и отвечает: оно не ниже строки, которую
// отменили. Границы принёс contentsChange — тот же сигнал, по которому мы
// узнаём о любой правке; выдумывать позицию не приходится.
void NoteEditor::landCaretWhereUndone() {
    if (undoTouch_.from < 0) return;
    const int last = qMax(0, document()->characterCount() - 1);
    // Последняя строка, которую отмена и правда тронула. Берём to − 1: сам to
    // стоит уже ЗА возвращённым куском, и по нему строка вышла бы соседней.
    const QTextBlock touched = document()->findBlock(qBound(0, undoTouch_.to - 1, last));
    if (!touched.isValid()) return;
    // Каретка внутри тронутого — не трогаем её вовсе: там её оставил человек,
    // и это ровно то место, где он работал (операция над выделением, набор).
    if (textCursor().blockNumber() <= touched.blockNumber()) return;
    // А НИЖЕ ТРОНУТОГО ЕЙ ДЕЛАТЬ НЕЧЕГО. Замена целого блока (переключение
    // задачи, смена маркера) приходит от Qt как правка, кончающаяся на начале
    // следующего блока, и каретка по ней вставала строкой НИЖЕ той, где
    // человек щёлкал (жалоба владельца: «курсор прыгает на строку вниз, хотя
    // бы там и не были; вниз — очень нелогично»). Возвращаем её в начало
    // изменённой строки.
    QTextCursor place(document());
    place.setPosition(qBound(0, touched.position(), last));
    setTextCursor(place);
}

NoteEditor::ViewAnchor NoteEditor::viewAnchor() const {
    const QAbstractTextDocumentLayout* layout = document()->documentLayout();
    const int top = verticalScrollBar()->value();
    const QTextBlock at = document()->findBlock(layout->hitTest(QPointF(0, top), Qt::FuzzyHit));
    if (!at.isValid()) return {};

    // Держимся только за блок НАД правкой: если правка выше кромки, номера
    // блоков за ней съедут, и якорь показал бы на чужой блок.
    const int anchorIndex = at.blockNumber();
    if (anchorIndex > textCursor().blockNumber()) return {};
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
        note_->doc().setUndoEnabled(true);
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
    note_->setBuiltBlocks(doc);
    settleAfterBuild(cursor, anchor, patched);
    recordingSuspended_ = wasSuspended;
}

void NoteEditor::settleAfterBuild(int cursor, const ViewAnchor& anchor, bool patched) {
    // Сборщик поставил документу базовый кегль — масштаб ему возвращаем мы.
    // Заплатка шрифта не трогает, но звать здесь всё равно дешевле, чем помнить
    // о двух путях: setZoom сравнивает шрифт и на совпадении ничего не делает.
    restoreScale();
    applyContentWidth();
    landAfterBuild(cursor, anchor, /*statsDone=*/patched);
}

void NoteEditor::landAfterBuild(int cursor, const ViewAnchor& anchor, bool statsDone) {
    // Статистика документа после ПЕРЕСБОРКИ (при открытии её считает
    // activateNote, при записи на диск — save). Считаем
    // ОБХОДОМ ЖИВОГО ДОКУМЕНТА: он только что собран, и брать числа больше
    // неоткуда — доставать ради счёта вторую копию содержимого было бы работой
    // на пустом месте. Обход стоит 3.2 мс на заметке в 239 КБ против 19 мс
    // самой сборки, в таком соседстве он незаметен. За заплаткой не считаем
    // вовсе: она стоит 64 мкс, и счёт был бы в полсотни раз дороже правки.
    if (!statsDone) refreshStats(documentStats(*document()));
    // Сборка — не правка: подметать за ней нечего, а область от неё вышла бы
    // во весь документ и утащила бы следующую уборку на полный проход.
    current_.dirty = QTextCursor();

    QTextCursor place(document());
    place.setPosition(qBound(0, cursor, document()->characterCount() - 1));
    setTextCursor(place);

    // Возвращаем блок-якорь на прежнее место относительно кромки. Прокрутка при
    // пересборке сбрасывается в ноль, и без этого документ прыгал бы к началу.
    const QTextBlock landed = document()->findBlockByNumber(anchor.irIndex);
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
    // случайное место скрытого текста. А щелчок по ПОДПИСИ под ней открывает
    // поле правки подписи (просьба владельца): человек целится в текст, и
    // ждать от него ещё одного жеста — лишнее. Только по настоящей подписи:
    // у картинки без неё под снимком пусто, и открывать там нечего — пустую
    // заводят Enter'ом или из меню.
    if (event->button() == Qt::LeftButton &&
        (event->modifiers() & Qt::ShiftModifier) == 0) {
        const int hitAt = document()->documentLayout()->hitTest(
            QPointF(event->position()) + QPointF(horizontalScrollBar()->value(),
                                                 verticalScrollBar()->value()),
            Qt::FuzzyHit);
        QTextBlock under = hitAt >= 0 ? document()->findBlock(hitAt) : QTextBlock();
        for (int step = 0; step < 2 && under.isValid(); ++step) {
            const QRectF photo = imageRectInViewport(under);
            // Показанная подпись — та, что человек видит под снимком
            // (безымянная и вики-вложение её не показывают, целиться там не во
            // что).
            const bool hasCaption = !blockImageRef(under).shownCaption().isEmpty();
            const QRectF caption =
                hasCaption ? imageCaptionRectInViewport(under) : QRectF();
            if (!caption.isEmpty() && caption.contains(event->position())) {
                setTextCursor(QTextCursor(under));
                editImageCaption(under.blockNumber());
                event->accept();
                return;
            }
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
    // сохраняет — ровно как Ctrl+D. Иначе выделить десяток задач и отметить
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
    if (!isReadOnly()) {
        QTextCursor at = cursorForPosition(event->pos());
        // ПО СТРОЧНОЙ ФОРМУЛЕ — раскрыть её исходник на месте: слова под
        // вёрсткой нет, выделять нечего. Знак — под щелчком или прямо слева
        // (cursorForPosition отдаёт ближайшую щель между знаками).
        {
            int knob = -1;
            if (isInlineFormulaChar(*document(), at.position())) knob = at.position();
            else if (isInlineFormulaChar(*document(), at.position() - 1))
                knob = at.position() - 1;
            if (knob >= 0) {
                QTextCursor place(document());
                place.setPosition(knob);
                setTextCursor(place);
                runNoteEdit([](ZDocument& note, QTextCursor& caret) {
                    return note.openInlineFormula(caret);
                });
                event->accept();
                return;
            }
        }
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
        // ПО ТАБЛИЦЕ — раскрыть исходник, каретка в ячейку под щелчком: ряд по
        // вертикали, колонка по горизонтали (карта точка → ячейка → смещение —
        // фундамент будущей правки по ячейкам).
        if (object.kind == ObjectKind::Table) {
            const QPointF documentPoint = QPointF(event->position()) +
                                          QPointF(horizontalScrollBar()->value(),
                                                  verticalScrollBar()->value());
            int offset = -1;
            (void)tableCellAt(documentPoint, nullptr, nullptr, nullptr, &offset);
            setTextCursor(at);
            runNoteEdit([offset](ZDocument& note, QTextCursor& caret) {
                return note.openTable(caret, offset);
            });
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
        QGuiApplication::clipboard()->setText(note_->doc().markdownOf(line));
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
    // И РАСКРЫТУЮ ТАБЛИЦУ — тем же жестом.
    if (event->key() == Qt::Key_Escape && !isReadOnly() && closeOpenObject()) {
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

    const auto pressed = [event](const QKeySequence& keys) { return keyEventMatches(*event, keys); };
    if (keyEventMatchesAny(*event, moveUpKeys_) && moveItem(-1)) return;
    if (keyEventMatchesAny(*event, moveDownKeys_) && moveItem(1)) return;

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
    if (isReadOnly()) return false;

    // СТРОЧНАЯ ФОРМУЛА — АТОМ В СТРОКЕ, у неё своё маленькое правило
    // (inlineObjectActionFor): Enter на ВЫДЕЛЕННОМ знаке (двойной щелчок или
    // Shift+стрелка выделяют его) раскрывает исходник на месте. Каретка
    // ВПЛОТНУЮ к знаку правило не включает: Enter рядом с формулой — обычный
    // разрез абзаца, как рядом с буквой.
    {
        const QTextCursor caret = textCursor();
        int knob = -1;
        if (caret.hasSelection() && qAbs(caret.position() - caret.anchor()) == 1) {
            const int at = qMin(caret.position(), caret.anchor());
            if (isInlineFormulaChar(*document(), at)) knob = at;
        }
        if (inlineObjectActionFor(event->key(), event->modifiers(), knob >= 0) ==
            InlineObjectAction::Edit) {
            QTextCursor place(document());
            place.setPosition(knob);
            setTextCursor(place);
            if (runNoteEdit([](ZDocument& note, QTextCursor& at) {
                    return note.openInlineFormula(at);
                }))
                return true;
        }
    }

    const QTextCursor caret = textCursor();
    const QTextBlock block = caret.block();
    const BlockObject own = objectOf(block);

    ObjectContext where;
    where.hasSelection = caret.hasSelection();
    where.atBlockStart = caret.positionInBlock() == 0;
    where.atBlockEnd = caret.positionInBlock() == block.length() - 1;
    where.onGap = isVSpaceBlock(block);
    // Пустой пункт (после Ctrl+Enter) или пустой абзац под объектом: Backspace
    // в нём убирает его, а не объект.
    where.blockEmpty = !isVSpaceBlock(block) && block.text().isEmpty();
    // Каретка «на объекте» — это каретка на любой его строке. У картинки строка
    // одна, у таблицы их столько, сколько в исходнике. Объект, который СЕЙЧАС
    // ПРАВЯТ исходником, объектом для клавиш не считается — там обычный текст,
    // и буквы обязаны попадать в него как всюду. Список правимых один на все
    // виды: заведи третий вид со своей проверкой — и он забудет либо про
    // запрет, либо про правку (мы уже забывали и то, и другое).
    where.onObject = own.valid();
    // Сочетание переключения (Ctrl+D, toggleTaskKey) — настраиваемое, и
    // слой узнаёт его признаком, а не кодом клавиши. Сравнение то же, что у
    // прочих сочетаний в keyPressEvent: Qt сопоставляет с учётом раскладки.
    if (keyEventMatches(*event, settings().editor().toggleTaskKey())) where.toggleKey = true;

    const QTextBlock above = block.previous();
    const QTextBlock below = block.next();
    const BlockObject objectAbove = objectOf(above);
    const BlockObject objectBelow = objectOf(below);
    where.objectAbove = objectAbove.valid();
    where.objectBelow = objectBelow.valid();
    if (isVSpaceBlock(above)) {
        const BlockObject overGap = objectOf(above.previous());
        // «Через пустую» — когда без неё блок слипся бы с объектом; пустой блок
        // под объектом (пункт после Ctrl+Enter) — тоже «через пустую»: пустая
        // строка заведена вместе с ним, и Backspace убирает их вместе.
        where.objectAboveGap = overGap.valid() &&
                               (where.blockEmpty || blocksWouldMerge(above.previous(), block));
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
        // НА ОБЪЕКТЕ БУКВА НЕ ДЕЛАЕТ НИЧЕГО — и не говорит ничего тоже (решение
        // владельца). Проглотили нажатие, и на этом всё: ни правки исходника,
        // ни сообщений. Прежде здесь шёл совет через importStatus — тот самый
        // сигнал, которым ввоз запирает окно, — и одна буква на картинке гасила
        // весь интерфейс навсегда (см. editor_widget.h).
        if (where.onObject && prints) return true;
        return false;
    }

    switch (action) {
        case ObjectAction::Edit: {
            // ПРАВИТЬ ТАБЛИЦУ — ЗНАЧИТ РАСКРЫТЬ ЕЁ В ИСХОДНИК, как формулу:
            // объект заменяется дословным блоком, каретка — в начало (решение
            // владельца); Esc или уход каретки сворачивают судьёй файла.
            if (own.kind == ObjectKind::Table) {
                runNoteEdit([](ZDocument& note, QTextCursor& at) { return note.openTable(at); });
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
                    emit hint(QStringLiteral("A wiki embed «![[…]]» has no caption"));
                return true;
            }
            return false;
        }
        case ObjectAction::ToggleCaption: {
            // Ctrl+D (toggleTaskKey) на картинке: `~` становится первым
            // знаком подписи, и под снимком её больше не видно
            // (isNonameCaption); то же сочетание снимает знак обратно. У
            // таблицы и формулы подписи нет.
            if (own.kind != ObjectKind::Image) {
                emit hint(QStringLiteral("Only an image has a caption"));
                return true;
            }
            if (!runNoteEdit([](ZDocument& note, QTextCursor& at) {
                    return note.toggleImageCaption(at, QLatin1Char('~'));
                }))
                emit hint(QStringLiteral("The image has no caption — nothing to hide"));
            return true;
        }
        case ObjectAction::LineAfter: {
            const int last = own.last;
            runNoteEdit([last](ZDocument& note, QTextCursor& at) {
                return note.insertLineAfter(at, last);
            });
            return true;
        }
        case ObjectAction::ContinueAfter: {
            // Shift+Enter на объекте — продолжить пункт текстом под ним.
            const int last = own.last;
            runNoteEdit([last](ZDocument& note, QTextCursor& at) {
                return note.continueItemAfter(at, last);
            });
            return true;
        }
        case ObjectAction::DropEmpty: {
            // Пустой блок под объектом убираем целиком и встаём на объект —
            // тот, что прямо над ним или через пустую строку.
            const BlockObject target = where.objectAbove ? objectAbove : objectOf(above.previous());
            const int number = block.blockNumber();
            runNoteEdit([number](ZDocument& note, QTextCursor& at) {
                return note.dropEmptyBlockAfterObject(at, number);
            });
            if (target.valid()) {
                // Объект стоял выше убранного — его номер не изменился.
                const QTextBlock landing = document()->findBlockByNumber(target.last);
                if (landing.isValid()) setTextCursor(QTextCursor(landing));
            }
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
    if (isReadOnly()) return nullptr;
    const QTextBlock block = document()->findBlockByNumber(firstBlockNumber);
    if (!block.isValid() || isRawBlock(block) || kindOf(block) != Kind::Code) return nullptr;
    if (languageEditor_ != nullptr) closeCodeLanguageEditor();

    languageBlock_ = firstBlockNumber;
    languageEditor_ = new LanguageEditor(note_->doc().codeLanguagesNear(firstBlockNumber),
                                         block.blockFormat().stringProperty(InfoProperty),
                                         viewport());
    languageEditor_->setFont(codeLangFont(docStyle(), plateScale()));
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
                   QFontMetricsF(codeLangFont(docStyle(), plateScale())).horizontalAdvance(QLatin1Char('A')) * 12)));
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
    if (isReadOnly()) return nullptr;
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
    const bool tooLong = current_.runChars >= qMax(1, settings().editor().undoRunChars());
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
    typingPause_.start(settings().editor().undoCoalesceMs());
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
    if (note_->statsCounted()) {
        note_->invalidateStats();
        emit statsChanged();
    }
    document()->setModified(true);
    showEditPlace(scrollBefore);
    autosave_.start(settings().editor().autosaveDelayMs());
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
        QAction* action = menu->addAction(QStringLiteral("Add images…"), this,
                                          &NoteEditor::chooseAndInsertImages);
        action->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+I")));
    }

    menu->addSeparator();
    const auto style = [](ZDocument::Style want) {
        return NoteOp([want](ZDocument& note, QTextCursor& at) {
            return note.toggleStyle(at, want);
        });
    };
    add(QStringLiteral("Bold"), QStringLiteral("Ctrl+B"), style(ZDocument::Style::Bold));
    add(QStringLiteral("Italic"), QStringLiteral("Ctrl+I"), style(ZDocument::Style::Italic));
    add(QStringLiteral("Strikethrough"), QStringLiteral("Ctrl+/"),
        style(ZDocument::Style::Strike));
    add(QStringLiteral("Inline code"), QStringLiteral("Ctrl+E"), style(ZDocument::Style::Code));
    {
        QAction* action = menu->addAction(QStringLiteral("Code block"));
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
        QMenu* heading = menu->addMenu(QStringLiteral("Heading"));
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
            // Сочетание есть только у «обычного текста» (makeParagraphKey) —
            // у уровней заголовка клавиш нет вовсе, их набирают автозаменой.
            if (level == 0)
                action->setShortcut(QKeySequence(settings().editor().makeParagraphKey(),
                                                 QKeySequence::PortableText));
        };
        addLevel(QStringLiteral("Plain text"), 0);
        for (int level = 1; level <= 3; ++level)
            addLevel(QStringLiteral("Level %1").arg(level), level);
        heading->addSeparator();
        for (int level = 4; level <= 6; ++level)
            addLevel(QStringLiteral("Level %1").arg(level), level);
    }

    // Показать во весь экран — тоже только на строке с фотографией.
    if (blockImageRef(textCursor().block()).valid) {
        menu->addSeparator();
        menu->addAction(QStringLiteral("View full screen"), this,
                        [this] { emit fullscreenShotRequested(); });
    }

    // Выравнивание — только на строке с фотографией: где картинки нет, пункт
    // ничего не значит и в меню ему делать нечего.
    if (blockImageRef(textCursor().block()).valid) {
        menu->addSeparator();
        QMenu* align = menu->addMenu(QStringLiteral("Align photo"));
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
        addAlign(QStringLiteral("Left"), ImageAlign::Left);
        addAlign(QStringLiteral("Center"), ImageAlign::Center);
        addAlign(QStringLiteral("Right"), ImageAlign::Right);

        // Подпись: править (Enter на снимке) и спрятать/вернуть (сочетание
        // переключения). У вики-вложения подписи нет — и пунктов нет.
        if (!ref.wiki) {
            const int block = textCursor().blockNumber();
            QAction* edit = menu->addAction(QStringLiteral("Caption…"), this,
                                            [this, block] { editImageCaption(block); });
            edit->setShortcut(QKeySequence(Qt::Key_Return));
            // Спрятанная ЗНАКОМ подпись возвращается тем же сочетанием; имя от
            // камеры («IMG_1234») знаком не вернуть — оно безымянное само по
            // себе, и пункт тогда ни к чему.
            const bool marked = !ref.alt.isEmpty() && (ref.alt.at(0) == QLatin1Char('~') ||
                                                       ref.alt.at(0) == QLatin1Char('-'));
            QAction* toggle = menu->addAction(
                marked ? QStringLiteral("Show caption under the photo")
                       : QStringLiteral("Hide caption under the photo"),
                this, [this] {
                    runNoteEdit([](ZDocument& note, QTextCursor& at) {
                        return note.toggleImageCaption(at, QLatin1Char('~'));
                    });
                });
            const QList<QKeySequence> all = QKeySequence::listFromString(
                settings().editor().toggleTaskKey(), QKeySequence::PortableText);
            if (!all.isEmpty()) toggle->setShortcut(all.first());
            toggle->setEnabled(!ref.alt.trimmed().isEmpty() &&
                               (marked || !isNonameCaption(ref.alt)));
        }
    }

    menu->addSeparator();
    add(QStringLiteral("Toggle task"), settings().editor().toggleTaskKey(),
        [](ZDocument& note, QTextCursor& at) { return note.toggleTask(at); });

    QMenu* kinds = menu->addMenu(QStringLiteral("Make"));
    const auto addKind = [this, kinds](const QString& title, const QString& keys,
                                       const NoteOp& op) {
        QAction* action = kinds->addAction(title, this, [this, op] { runNoteEdit(op); });
        const QList<QKeySequence> all =
            QKeySequence::listFromString(keys, QKeySequence::PortableText);
        if (!all.isEmpty()) action->setShortcut(all.first());
    };
    addKind(QStringLiteral("Bullet list"), settings().editor().makeBulletKey(),
            [](ZDocument& note, QTextCursor& at) { return note.makeBullet(at); });
    addKind(QStringLiteral("Numbered list"), settings().editor().makeOrderedKey(),
            [](ZDocument& note, QTextCursor& at) { return note.makeOrdered(at); });
    addKind(QStringLiteral("Task list"), settings().editor().makeTaskKey(),
            [](ZDocument& note, QTextCursor& at) { return note.makeTask(at); });
    addKind(QStringLiteral("Comment"), settings().editor().makeCommentKey(),
            [](ZDocument& note, QTextCursor& at) { return note.toggleComment(at); });
    addKind(QStringLiteral("Plain text"), settings().editor().makeParagraphKey(),
            [](ZDocument& note, QTextCursor& at) { return note.makeParagraph(at); });

    menu->addSeparator();
    add(QStringLiteral("Indent"), QStringLiteral("Tab"),
        [](ZDocument& note, QTextCursor& at) { return note.indent(at); });
    add(QStringLiteral("Outdent"), QStringLiteral("Shift+Tab"),
        [](ZDocument& note, QTextCursor& at) { return note.outdent(at); });

    QAction* up = menu->addAction(QStringLiteral("Move up"), this,
                                  [this] { moveItem(-1); });
    up->setShortcut(QKeySequence(settings().editor().moveUpKey(), QKeySequence::PortableText));
    QAction* down = menu->addAction(QStringLiteral("Move down"), this,
                                    [this] { moveItem(1); });
    down->setShortcut(QKeySequence(settings().editor().moveDownKey(), QKeySequence::PortableText));

    // Внешний редактор — команда окна, а не редактора: запускать процессы
    // виджету текста не по чину. Пункт здесь, потому что искать его человек
    // будет там, где смотрит на заметку.
    menu->addSeparator();
    menu->addAction(QStringLiteral("Open in external editor"), this,
                    [this] { emit externalEditorRequested(note_->path()); });
    // Вывоз — тоже команда окна: диалог сохранения и запись PDF виджету текста
    // не по чину, да и заметку перед вывозом надо сперва записать.
    menu->addAction(QStringLiteral("Export…"), this,
                    [this] { emit exportRequested(note_->path()); });

    menu->popup(event->globalPos());
}

QMimeData* NoteEditor::createMimeDataFromSelection() const {
    QMimeData* data = new QMimeData;
    data->setText(note_->doc().markdownOf(textCursor()));
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
        //
        // СПРАШИВАЕМ ЯДРО, А НЕ QT. Наши читатели — не плагины Qt, их зовут
        // напрямую, и QImageReader про avif/heic не знает даже в сборке с
        // WITH_HEIF=ON: перетащенный avif молча вставлялся ссылкой (нашёл
        // владелец). probeImageFile читает только начало файла и опознаёт по
        // байтам, а не по расширению.
        QStringList images;
        for (const QString& f : files)
            if (probeImageFile(f).valid()) images << f;
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
    if (note_->statsCounted()) {
        note_->invalidateStats();
        emit statsChanged();
    }

    document()->setModified(true);
    showEditPlace(scrollBefore);
    autosave_.start(settings().editor().autosaveDelayMs());
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
                             QStringLiteral("The note is not saved yet — the attachment has nowhere to go."));
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
                                 QStringLiteral("The previous images are still being imported."));
        return false;
    }
    if (importer_ == nullptr) {
        importer_ = new ImageImporter(importLimitsFrom(settings().images()), this);
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
                        emit importStatus(QStringLiteral("read-only: importing %1 (%2 of %3)")
                                              .arg(name)
                                              .arg(done + 1)
                                              .arg(total));
                });
    }

    importedBatch_.clear();
    // Окно прогресса неблокирующее: цикл событий крутится, окно перерисовывается,
    // и «программа не отвечает» больше неоткуда взяться.
    delete importProgress_;
    importProgress_ = new QProgressDialog(QStringLiteral("Importing images…"),
                                          QStringLiteral("Cancel"), 0, count, this);
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
                             QStringLiteral("Not inserted:\n") +
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
                             QStringLiteral("The note is not saved yet — the attachment has nowhere to go."));
        return;
    }
    // Фильтр строим из того, что читатели УМЕЮТ на этой машине, а не из списка
    // в коде: без libheif heic не прочтётся, и предлагать его было бы обманом.
    // Спрашиваем ЯДРО (readableImageExtensions): у Qt свой список, и наших
    // читателей — jxl, avif, heic — в нём нет.
    QStringList patterns;
    for (const QString& ext : readableImageExtensions())
        patterns << QStringLiteral("*.") + ext;
    patterns.sort();
    const QString filter = QStringLiteral("Images (%1);;All files (*)")
                               .arg(patterns.join(QLatin1Char(' ')));

    const QStringList chosen = QFileDialog::getOpenFileNames(
        this, QStringLiteral("Add images"), lastImageDir_, filter);
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
    // СТАТИСТИКА ДОКУМЕНТА — ДО ВСЯКИХ РАННИХ ВЫХОДОВ (решение владельца). Это
    // единственное
    // место, куда приходит ЛЮБАЯ правка документа: и набор, и отмена с
    // повтором (они идут под recordingSuspended_ и до конца обработчика не
    // добираются), и наложение правленого исходника. Пока устаревание жило
    // ниже, после Ctrl+Z полоса сведений застревала на «?» до следующей
    // правки, и лечить это пришлось бы вызовом из каждой ветки — а забыть
    // одну вопрос времени.
    //
    // Смена облика сюда тоже приходит и заводит лишний пересчёт. Это принято
    // сознательно: облик меняют не раз в несколько секунд, а один обход
    // документа дешевле развилки «правка это или нет».
    if (note_->statsCounted()) {
        note_->invalidateStats();
        emit statsChanged();   // окну пора показать «?»
    }
    statsRecount_.start(settings().editor().statsDelayMs());

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
    autosave_.start(settings().editor().autosaveDelayMs());
}

// Правка есть — снимка пока нет. Читать документ целиком на каждую букву
// незачем: набор подряд всё равно склеивается в один шаг, и все промежуточные
// снимки этого шага выбрасываются. Ждём конца серии.

void NoteEditor::editMeta(const std::function<void(NoteHeader&)>& change) {
    note_->header().setPresent(true);
    change(note_->header());
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

// --- разность ---------------------------------------------------------------

qint64 NoteEditor::restoreBody(const std::string& body, qint64 sourceTime, bool* alreadyCurrent) {
    if (alreadyCurrent != nullptr) *alreadyCurrent = false;
    if (isReadOnly() || sourceTime <= 0) return 0;

    // Тело приходит КАНОНИЧЕСКИМ MARKDOWN СЛЕПКА (ZNoteTimeline::snapshotBody),
    // а не тем, что нарисовано в режиме истории: документ разности — отдельный
    // артефакт, и на диск он не уходит ни по какому пути.
    std::vector<Piece> blocks;
    NoteHeader ignored;
    parsePieces(normaliseSpaces(QString::fromUtf8(body.data(), qsizetype(body.size()))), blocks,
                ignored);

    // Слепок и есть нынешняя версия — восстанавливать нечего. Сравниваем тела,
    // без меты: восстановление её и не трогает, а штамп modified у слепка свой
    // и разошёлся бы всегда.
    if (writePieces(blocks) == writePieces(piecesOf(*document()))) {
        if (alreadyCurrent != nullptr) *alreadyCurrent = true;
        return 0;
    }

    // Метаданные живой заметки побеждают: история возвращает содержимое, а не
    // местоположение. parent, теги и created остаются нынешними; modified
    // поднимется при записи, как при любой правке.
    //
    // ВОССТАНОВЛЕНИЕ — ОБЫЧНАЯ ПРАВКА, и отменяется обычным Ctrl+Z. Значит и
    // пересборка здесь идёт правкой: внутри скобки, одним шагом. Полная сборка
    // на её месте сбрасывала бы стек — то есть человек, вернувшийся из истории,
    // терял бы возможность передумать.
    {
        QTextCursor group(document());
        group.beginEditBlock();
        rebuild(blocks, textCursor().position(), viewAnchor(), nullptr, /*asEdit=*/true);
        group.endEditBlock();
    }
    document()->setModified(true);
    current_.undoRun = false;

    // Ближайшее сохранение станет записью restore со ссылкой на источник.
    note_->journal().markNextSaveAsRestore(sourceTime);
    save(false);
    return sourceTime;
}

// Правила отбора записей — свод журнала (ZJournal::Rules): тем же
// кодом чистится и старая история. Здесь — только числа из настроек.
zametti::ZJournal::Rules NoteEditor::historyRules() {
    const ZSettings::History& history = settings().history();
    zametti::ZJournal::Rules rules;
    rules.mergeChars = qMax(0, history.historyMergeChars());
    rules.mergeHours = qMax(1, history.historyMergeHours());
    rules.ignoreAge = false;   // живая запись смотрит только на свежие записи
    return rules;
}

void NoteEditor::save(bool interactive, bool force) {
    if (!note_->hasPath()) return;
    if (!force && !document()->isModified()) return;
    // РАСКРЫТАЯ ТАБЛИЦА ПОД КАРЕТКОЙ. Промежуточные состояния правки законно не
    // таблицы (Enter завёл строку, набирается ряд), а файл обязан быть
    // согласован сам с собой — самопроверка круга иначе отвергнет запись,
    // положит .rescue и пересоберёт документ из перечитанного, выбив каретку из
    // правки. Автосохранение по таймеру сюда не доходит — ждёт выхода из
    // правки (см. таймер); сохранение по слову человека или при уходе (Ctrl+S,
    // смена заметки, выход) сперва сворачивает раскрытое судьёй файла:
    // состояние таблицы и так проверяется на выходе, а сохранение и есть выход.
    if (caretInOpenObject()) closeOpenObject();
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
    const NoteHeader metaBefore = note_->header();
    if (note_->hasHeader() && stampModifiedOnSave_) note_->stampModified();

    std::vector<Piece> fileIr;
    QByteArray candidate = note_->fileBytes(&fileIr);
    if (!note_->lastSaved().isEmpty() && NoteHeader::sameFileApartFromStamps(candidate, note_->lastSaved())) {
        note_->setHeader(metaBefore);
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
    if (note_->header().present()) {
        bool moved = false;
        // ВЕРСИЯ ФОРМАТА — ТОЖЕ ЛЕНИВО И ТОЖЕ ЗДЕСЬ (refactor3, решение владельца):
        // `version: 1` получает только заметка, которую мы и правда пишем.
        // Выше, в stampModified, ей не место: штамп ставится ДО решения
        // «сохранять нечего» и откатывается, а новая строка шапки ломала бы
        // сравнение «не считая modified» — и нетронутая заметка переписывалась
        // бы (так покраснел ArchiveEditor: стаб оброс версией).
        if (note_->header().get(NoteHeader::kVersionKey).empty()) {
            note_->header().ensureVersion();
            moved = true;
        }
        for (const char* key : {"created", "modified"}) {
            const std::string had = note_->header().get(key);
            if (had.empty()) continue;
            const QDateTime moment = store::parseNoteTime(had);
            if (!moment.isValid()) continue;   // чужая строка — не наша забота
            if (moment.timeSpec() == Qt::OffsetFromUTC || moment.timeSpec() == Qt::TimeZone)
                continue;
            const QString fresh = store::isoWithOffset(moment.toLocalTime());
            if (fresh.isEmpty() || fresh.toStdString() == had) continue;
            note_->header().set(key, fresh.toStdString());
            moved = true;
        }
        // Пересобираем байты только если что-то и правда переехало: лишняя
        // сериализация большой заметки — это миллисекунды на каждое
        // автосохранение.
        if (moved) candidate = note_->fileBytes(&fileIr);
    }

    // Отпечаток того, что в файле, мы знаем — значит «не изменилось ли»
    // решается без чтения файла.
    const SaveOutcome outcome =
        note_->save(note_->path(), rescueTimestamp(), note_->digest(), &fileIr, &candidate);
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
            note_->journal().record(ZJournal::Kind::Save, outcome.written);
        // Признак «восстановление» гасим при любом исходе записи: он относится
        // к одному ближайшему сохранению, а не «пока не сработает».
        note_->journal().clearPendingRestore();

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

        // Статистика документа после записи. Документ с момента записи не менялся,
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
        QStringLiteral("do not warn about this file again in this session"), &box);
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
    const qreal golden = qBound(0.0, settings().ui().focusRatio(), 0.9);
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
    verticalScrollBar()->setValue(verticalScrollBar()->value() + dy);
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

void NoteEditor::rememberCurrentCaretInApp() const {
    if (!note_->hasPath()) return;
    ZApp::instance().state().rememberCaret(
        note_->id(),
        {textCursor().position(), textCursor().anchor(), verticalScrollBar()->value()});
}

EscapeAction escapeActionFor(bool languageEditorOpen, bool caretInOpenObject, bool findBarVisible,
                             bool settingsModeActive) {
    if (languageEditorOpen) return EscapeAction::CloseLanguageEditor;
    // Раскрытый объект (формула, таблица) сворачивается раньше панели поиска по
    // той же причине, по которой раньше неё закрывается поле языка: сперва
    // уходит то, что открыто ПОВЕРХ текста и держит каретку.
    if (caretInOpenObject) return EscapeAction::CloseObject;
    if (findBarVisible) return EscapeAction::CloseFindBar;
    // Правка настроек — последней: закрывать её, пока над ней открыта панель
    // поиска, значило бы уносить из-под человека сразу два слоя. Режима
    // исходника здесь нет намеренно — см. заголовок.
    if (settingsModeActive) return EscapeAction::LeaveMode;
    return EscapeAction::Nothing;
}


// СВЕРНУТЬ РАСКРЫТЫЙ ОБЪЕКТ ПОД КАРЕТКОЙ — формулу или таблицу. Один вход для
// Esc из редактора и из ярлыка окна (EscapeAction::CloseObject): ярлык окна
// перехватывает Esc раньше виджета, и без общей двери Esc в редакторе не
// доходил до closeFormula вовсе.
bool NoteEditor::closeOpenObject() {
    if (isReadOnly()) return false;
    // Строчная — первой: её блок остаётся абзацем, и до closeFormula/closeTable
    // дело не дойдёт (те требуют целого блока-формулы или дословного куска).
    if (runNoteEdit([](ZDocument& note, QTextCursor& at) { return note.closeInlineFormula(at); }))
        return true;
    if (runNoteEdit([](ZDocument& note, QTextCursor& at) { return note.closeFormula(at); }))
        return true;
    return runNoteEdit([](ZDocument& note, QTextCursor& at) { return note.closeTable(at); });
}

// Каретка стоит в раскрытом объекте (формуле или таблице), который Esc свернёт.
bool NoteEditor::caretInOpenObject() const {
    if (isReadOnly()) return false;
    const QTextBlock block = textCursor().block();
    if (isRawBlock(block)) return !isTableObjectBlock(block) && looksLikeTable(sourceTextOf(block));
    // Раскрытая СТРОЧНАЯ формула в блоке каретки: Esc её свернёт, сохранение
    // свернёт перед записью — как таблицу и выключную.
    if (hasOpenInlineFormula(block)) return true;
    if (kindOf(block) != Kind::Paragraph) return false;
    const BlockFormulaRef ref = blockFormulaRef(block);
    return ref.valid && ref.display;
}

}  // namespace zametti
