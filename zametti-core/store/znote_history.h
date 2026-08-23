// Журнал ОДНОЙ заметки как объект: механика файла (journal::History) плюс
// правила отбора (history_rules.h) плюс то состояние, что раньше лежало
// россыпью в NoteEditor::NoteSession — разжатый хвост, признак «чищен», признак
// «ближайшая запись — восстановление».
//
// ЗАЧЕМ. Пять полей и шесть мест создания `journal::History` в редакторе
// (аудит refactor2, §1.2): у журнала заметки не было объекта, а был набор
// полей в чужой структуре и функции виджета, которые их трогали. Здесь это
// один класс с одним публичным видом операций; редактор зовёт `record`, а как
// именно решается «заменить хвост или лечь рядом» — дело этого класса.
//
// Правила отбора (`Rules`) ПРИХОДЯТ СНАРУЖИ, в конструктор: числа из настроек
// — свойство настроек, а не журнала, и класс не лезет за ними в глобальное
// состояние. Тому, кто заводит заметку, эти числа известны.
//
// Ошибки — не падение и не окно: журнал это удобство, а не условие работы
// (решение владельца, этап 7). Метод возвращает false и объяснение, а
// вызывающий решает, жаловаться ли; сам класс пишет в stderr — как писал
// редактор до него, чтобы поведение не менялось молча.

#ifndef ZAMETTI_ZNOTE_HISTORY_H
#define ZAMETTI_ZNOTE_HISTORY_H

#include "history_rules.h"
#include "journal.h"

#include <QByteArray>
#include <QString>

namespace zametti {

class ZNoteHistory {
public:
    // Без хранилища: всё «нет». Так живёт заметка, открытая файлом вне
    // хранилища, — журнала у неё нет, и все методы молчат.
    ZNoteHistory() = default;
    ZNoteHistory(QString storeRoot, QString noteId, history::Rules rules);

    bool available() const { return !root_.isEmpty() && !id_.isEmpty(); }
    const QString& noteId() const { return id_; }

    // ОПОРНАЯ ЗАПИСЬ. Заметки старше журнала: если журнала ещё нет, в него
    // кладётся то, с чем заметку открыли, — временем файла, а не «сейчас».
    // Иначе первой записью стало бы первое сохранение, и всё, чем заметка
    // была до него, не попало бы в историю никогда (владелец опустошил заметку
    // Ctrl+A, Delete — и пустота оказалась первой записью).
    void ensureBaseline(const QByteArray& contents, qint64 fileTimeMs);

    // ЗАПИСАТЬ СЛЕПОК ПО ПРАВИЛАМ. Хвост журнала разжимается один раз на заход
    // в заметку и дальше держится в памяти (после каждой записи последней
    // становится то, что мы записали); перед первой записью журнал чистится
    // (ленивая миграция); решает общий свод (decideStep): заменить хвост или
    // лечь рядом. Род записи — kind (Save, External…); Save становится Restore,
    // если перед этим было markNextSaveAsRestore. Ложь — журнал не записан;
    // хвост тогда считается неизвестным и разожмётся заново.
    bool record(journal::Kind kind, const QByteArray& snapshot, QString* error = nullptr);

    // Ближайшая запись — восстановление из прошлого: время источника едет в
    // запись. Признак гасится любой записью, даже неудавшейся, — он относится
    // к одному ближайшему сохранению, а не «пока не сработает».
    void markNextSaveAsRestore(qint64 sourceTimeMs) { restoreSource_ = sourceTimeMs; }
    qint64 pendingRestoreSource() const { return restoreSource_; }
    void clearPendingRestore() { restoreSource_ = 0; }

    // ЛЕНИВАЯ ЧИСТКА — раз за заход в заметку. Два триггера: первая запись и
    // первое чтение истории (просмотр заметки журнал не трогает вовсе).
    void compressOnce();

    // Чтение рамок — с чисткой перед первым чтением (второй триггер): показывать
    // дубликаты, которые всё равно уйдут при первой правке, незачем. Слепок по
    // номеру чистки НЕ зовёт: номера относятся к прочитанным рамкам, и сдвигать
    // их между read и snapshotAt нельзя.
    bool read(journal::ZJournal* out, QString* error);
    bool snapshotAt(int index, QByteArray* out, QString* error) const;

    // Механика файла — тем, кому нужен сам журнал (поиск по истории). Чистку
    // перед своим чтением такой вызывающий зовёт сам (compressOnce).
    journal::History file() const { return journal::History(root_); }

protected:
    QString root_;
    QString id_;
    history::Rules rules_;
    // Последняя запись журнала, разжатая, и когда она сделана.
    QByteArray tail_;
    qint64 tailTime_ = 0;
    bool tailKnown_ = false;
    bool compressed_ = false;
    qint64 restoreSource_ = 0;

    void loadTail(journal::History& history);
};

}  // namespace zametti

#endif  // ZAMETTI_ZNOTE_HISTORY_H
