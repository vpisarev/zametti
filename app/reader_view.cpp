#include "reader_view.h"

#include "zapp.h"
#include "znote.h"

#include <QFile>
#include <QFileInfo>
#include <QScrollBar>

namespace zametti {

ReaderView::ReaderView(QWidget* parent, Tint tint) : NoteView(parent), tint_(tint) {
    setReadOnly(true);
    // Фокус берём: по виду листают клавишами, ищут в нём и копируют из него.
    // Каретки при этом нет — её рисует только редактор.
    setFocusPolicy(Qt::StrongFocus);
    applyPalette(*this, /*history=*/tint_ == Tint::Aged);
    setDocument(shown_.getDocument());
}

bool ReaderView::showFile(const QString& path, const QString& noteId) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return false;
    const QByteArray bytes = file.readAll();
    file.close();
    // Уходим с прежнего документа — запомнили, где на нём стояли.
    rememberSpot();

    // ТОТ ЖЕ КОД, ЧТО У РЕДАКТОРА: заметка разбирает байты на шапку и тело и
    // строит документ. Второго пути «из markdown в документ» в программе нет и
    // заводить его нельзя — он разошёлся бы с первым на первой же тонкости.
    // ВСЁ, ЧТО ВИД ОБЯЗАН ЗНАТЬ О ЗАМЕТКЕ, — одной функцией, той же, что зовёт
    // редактор. Именно её отсутствия и стоила заметка из одних снимков: база
    // относительных ссылок была пуста, и картинки не рисовались вовсе.
    adoptNoteAt(path);

    ZNote note;
    note.load(std::string_view(bytes.constData(), size_t(bytes.size())));
    // Подмена — ОДНА операция: сперва забираем документ у заметки, потом
    // отправляем прежний на покой и только затем отдаём виду новый. Порознь
    // между этими шагами есть миг, когда вид смотрит на разрушенный документ.
    ZDocument fresh = note.replaceDoc(ZDocument{});
    retired_ = std::move(shown_);
    shown_ = std::move(fresh);
    path_ = path;
    // Имя, под которым помнится место чтения. Пусто — выводим из пути, как это
    // делает сама заметка: id — имя файла без расширения.
    noteId_ = noteId.isEmpty() ? QFileInfo(path).completeBaseName() : noteId;
    title_ = shown_.title();
    setDocument(shown_.getDocument());
    applyContentWidth();
    restoreSpot();
    // ПОСЛЕ того, как проставлены путь и заголовок: слушатель спросит и то, и
    // другое прямо в обработчике.
    emit shownChanged(path_);
    return true;
}

void ReaderView::clear() {
    if (path_.isEmpty()) return;
    rememberSpot();
    path_.clear();
    noteId_.clear();
    title_.clear();
    retired_ = std::move(shown_);
    shown_ = ZDocument{};
    setDocument(shown_.getDocument());
    emit shownChanged(QString());
}

// МЕСТО ЧТЕНИЯ — ТАМ ЖЕ, ГДЕ КАРЕТКИ ПРАВИМЫХ ЗАМЕТОК (ZAppState, по id).
// Второго хранилища мест не заводим: тогда «где я читал» и «где стояла
// каретка» разошлись бы у одной и той же заметки, вернувшейся из архива.
void ReaderView::rememberSpot() {
    if (noteId_.isEmpty()) return;
    CaretSpot spot = ZApp::instance().state().caretOf(noteId_);
    spot.scroll = verticalScrollBar()->value();
    ZApp::instance().state().rememberCaret(noteId_, spot);
}

void ReaderView::restoreSpot() {
    // Ставится ПОСЛЕ вёрстки: до неё полоса прокрутки не знает своего
    // размаха и обрежет значение до нуля.
    const int scroll = ZApp::instance().state().caretOf(noteId_).scroll;
    verticalScrollBar()->setValue(scroll);
}

void ReaderView::refreshAppearance() {
    applyPalette(*this, /*history=*/tint_ == Tint::Aged);
    if (!path_.isEmpty()) {
        const QString path = path_;
        const QString id = noteId_;
        path_.clear();
        showFile(path, id);
    }
}

}  // namespace zametti
