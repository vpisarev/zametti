#include "history_controller.h"

#include "settings.h"

#include <QShortcut>

#include <cstdio>

namespace zametti {

HistoryController::HistoryController(NoteEditor& editor, HistoryView& view, QObject* parent)
    : QObject(parent), editor_(editor), view_(view), list_(view.list()) {
    // Редактор просит в историю: дно цепочки отмены и архивная заметка.
    connect(&editor_, &NoteEditor::historyRequested, this, [this] { enter(); });
    connect(&editor_, &NoteEditor::archivedNoteOpened, this, [this] {
        if (!enter()) {
            // Журнала нет вовсе — показать нечего, но и молчать нельзя: человек
            // видит одну строку вместо заметки и вправе знать, почему.
            std::fprintf(stderr, "архивная заметка без истории: %s\n",
                         editor_.filePath().toUtf8().constData());
        }
    });
    // Открытие другой заметки выводит из режима. Без этого вид остался бы
    // показывать слепок ПРЕЖНЕЙ заметки при открытой новой (найдено пробником
    // этапа 10: после ухода и возврата режим оставался включён).
    connect(&editor_, &NoteEditor::fileChanged, this, [this] { leave(); });

    connect(&view_, &HistoryView::leaveRequested, this, &HistoryController::leave);
    connect(&view_, &HistoryView::restoreRequested, this, [this] { restore(); });
    connect(&view_, &HistoryView::baseChanged, this, &HistoryController::setBaseFresh);
    connect(&view_, &HistoryView::stepBackRequested, this, [this] { stepBack(); });
    connect(&view_, &HistoryView::stepForwardRequested, this, [this] { stepForward(); });
    connect(&view_, &HistoryView::editRefused, this, &HistoryController::editRefused);
    connect(&list_, &HistoryTimeline::entryChosen, this, [this](int index) { select(index); });
    connect(&list_, &HistoryTimeline::closeRequested, this, &HistoryController::leave);
}

bool HistoryController::enter(int index) {
    if (active()) return index >= 0 && select(index);
    if (!editor_.hasStorage()) return false;

    // Незаписанные правки — в файл, а значит и в журнал (см. заголовок).
    editor_.save(false, /*force=*/true);

    // Таймлайн — тот же журнал, что у заметки (один объект: чистка и хвост
    // общие), свежая версия — байты живой заметки СЕЙЧАС, облик — стиль
    // документа редактора.
    auto timeline = std::make_shared<ZNoteTimeline>(editor_.noteHistory(), editor_.noteFileBytes(),
                                                    editor_.note().stylePtr());
    QString error;
    if (!timeline->open(index, &error)) {
        if (!error.isEmpty())
            std::fprintf(stderr, "история не читается: %s\n", error.toUtf8().constData());
        return false;
    }
    timeline_ = std::move(timeline);
    lastIndex_ = timeline_->index();
    // О начале режима сообщаем ДО показа: слушатель на этом сигнале показывает
    // виды, а список заполняется здесь же — номер приезжает в готовый список.
    list_.setEntries(timeline_->entries());
    view_.attach(timeline_);
    for (QShortcut* shortcut : shortcuts_) shortcut->setEnabled(true);
    emit modeChanged(true);
    showState();
    return true;
}

void HistoryController::leave() {
    if (!active()) return;
    lastIndex_ = timeline_->index();
    view_.detach();
    timeline_.reset();
    for (QShortcut* shortcut : shortcuts_) shortcut->setEnabled(false);
    emit modeChanged(false);
}

void HistoryController::showState() {
    if (!active()) return;
    lastIndex_ = timeline_->index();
    list_.setCurrent(timeline_->index());
    emit indexChanged(timeline_->index());
}

bool HistoryController::select(int index) {
    if (!active()) return false;
    if (!timeline_->select(index)) return false;
    view_.refresh();
    showState();
    return true;
}

bool HistoryController::stepBack() {
    if (!active()) return enter();
    if (!timeline_->stepBack()) return false;   // дальше в прошлое некуда
    view_.refresh();
    showState();
    return true;
}

bool HistoryController::stepForward() {
    if (!active()) return false;
    if (!timeline_->stepForward()) {
        // Дальше последнего слепка — живая версия. Это и есть выход из режима
        // хронологическим шагом вперёд.
        leave();
        return true;
    }
    view_.refresh();
    showState();
    return true;
}

void HistoryController::setBaseFresh(bool fresh) {
    if (!active()) return;
    const ZNoteTimeline::Base base = fresh ? ZNoteTimeline::Base::Fresh : ZNoteTimeline::Base::Previous;
    if (timeline_->base() == base) return;
    timeline_->setBase(base);
    view_.refresh();
}

qint64 HistoryController::restore(bool* alreadyCurrent) {
    if (alreadyCurrent != nullptr) *alreadyCurrent = false;
    if (!active()) return 0;
    // Тело — из самого слепка (каноническое), а не из документа разности: тот
    // показывает и убранные строки, и на диск не уходит ни по какому пути.
    const std::string body = timeline_->snapshotBody();
    const qint64 source = timeline_->snapshotTime();
    // СПЕРВА ВОССТАНОВИТЬ, ПОТОМ ВЫЙТИ. По выходу из режима (modeChanged(false))
    // возобновляется отложенный markdown-режим и заливает вид исходником
    // заметки — он обязан быть уже восстановленным. restoreBody от режима
    // истории не зависит: пишет заметку и журнал штатным save.
    bool current = false;
    const qint64 done = editor_.restoreBody(body, source, &current);
    leave();
    if (alreadyCurrent != nullptr) *alreadyCurrent = current;
    if (current) emit restoreWasCurrent();
    else if (done > 0) emit restored(done);
    return done;
}

HistorySearchReport HistoryController::searchHistory(const QString& text) {
    HistorySearchReport report;
    const std::shared_ptr<ZNoteHistory> history = editor_.noteHistory();
    if (history == nullptr || !history->available()) return report;
    // Обращение к истории — значит и чистка: искать надо по уже вычищенному
    // журналу, иначе один и тот же текст найдётся в трёх дубликатах.
    history->compressOnce();
    return searchNoteHistory(history->file(), history->noteId(), makeQuery(text));
}

void HistoryController::installShortcuts(QWidget* window) {
    const auto bind = [&](const QString& keys, auto&& action) {
        for (const QString& part : keys.split(QLatin1Char(';'), Qt::SkipEmptyParts)) {
            const QKeySequence sequence(part.trimmed());
            if (sequence.isEmpty()) continue;
            auto* shortcut = new QShortcut(sequence, window);
            shortcut->setEnabled(active());
            connect(shortcut, &QShortcut::activated, this, action);
            shortcuts_.push_back(shortcut);
        }
    };
    bind(settings().editor().diffNextKey(), [this] { view_.textView().stepChange(true); });
    bind(settings().editor().diffPreviousKey(), [this] { view_.textView().stepChange(false); });
}

void HistoryController::refreshAppearance() {
    applyPalette(view_.textView(), /*history=*/true);
    if (!active()) return;
    timeline_->setStyle(editor_.note().stylePtr());
    view_.refresh();
}

}  // namespace zametti
