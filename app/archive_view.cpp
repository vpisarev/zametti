#include "archive_view.h"

#include "znote.h"

#include <QFile>
#include <QFileInfo>
#include <QScrollBar>

namespace zametti {

ArchiveView::ArchiveView(QWidget* parent) : NoteView(parent) {
    setReadOnly(true);
    // Фокус берём: по виду листают клавишами, ищут в нём и копируют из него.
    // Каретки при этом нет — её рисует только редактор.
    setFocusPolicy(Qt::StrongFocus);
    applyPalette(*this, /*history=*/true);
    setDocument(shown_.getDocument());
}

bool ArchiveView::showFile(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return false;
    const QByteArray bytes = file.readAll();
    file.close();

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
    setDocument(shown_.getDocument());
    applyContentWidth();
    verticalScrollBar()->setValue(0);
    return true;
}

void ArchiveView::clear() {
    if (path_.isEmpty()) return;
    path_.clear();
    retired_ = std::move(shown_);
    shown_ = ZDocument{};
    setDocument(shown_.getDocument());
}

void ArchiveView::refreshAppearance() {
    applyPalette(*this, /*history=*/true);
    if (!path_.isEmpty()) {
        const QString path = path_;
        path_.clear();
        showFile(path);
    }
}

}  // namespace zametti
