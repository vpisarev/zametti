// ZNote — заметка, которая открыта сейчас или была открыта недавно и к
// которой мы хотим быстро вернуться.
//
// Это ОБЪЕКТ, а не набор полей в структуре виджета (решение владельца,
// refactor2): всё, что переживает уход из заметки и возврат к ней, живёт
// здесь и уезжает в кэш открытых заметок целиком — забыть перенести поле
// нельзя, потому что переносится сам объект. Внутри:
//
//   * ZDocument — живая модель текста; правится только своими глаголами;
//   * ZNoteHistory — журнал этой заметки ОДНИМ ПОЛЕМ, а не россыпью;
//   * шапка (мета), отпечаток и последняя записанная копия — «изменилось ли»;
//   * счёт слов и строк, признак самопроверки;
//   * место человека в заметке: каретка, второй конец выделения, прокрутка.
//
// Что НЕ переживает ухода (режим истории, найденное, серия набора, курсоры
// уборки) — здесь не живёт; это состояние текущего вида (см. NoteEditor).
//
// Держится через std::shared_ptr и не копируется: ZDocument внутри — ручка,
// и копия объекта разделяла бы документ, но заводила бы второй журнал и
// вторую каретку. Живой документ наружу отдаётся ровно одним вызовом —
// view->setDocument(note.doc().getDocument()) — и стережёт это сборка.

#ifndef ZAMETTI_ZNOTE_H
#define ZAMETTI_ZNOTE_H

#include "document.h"
#include "document_pieces.h"
#include "hash.h"
#include "note_header.h"
#include "text_stats.h"
#include "znote_history.h"

#include <QByteArray>
#include <QString>

#include <vector>

namespace zametti {

// Место человека в заметке — все три числа сразу: «где каретка» и «что
// выделено» — разные вопросы, и заметка обязана помнить оба (просьба
// владельца), а прокрутка — где он был на экране.
struct CaretSpot {
    int cursor = 0;
    int anchor = 0;
    int scroll = 0;
};

class ZNote {
public:
    // Пустая заметка без пути: так выглядит редактор, пока ничего не открыто.
    ZNote() = default;
    // Заметка, только что прочитанная с диска: путь, байты файла (они же
    // последняя записанная копия), их отпечаток и её журнал. Документ пуст —
    // его собирает вид из разобранных блоков; шапка ставится setMeta.
    ZNote(QString path, QByteArray fileBytes, Digest digest, ZNoteHistory history);

    ZNote(const ZNote&) = delete;
    ZNote& operator=(const ZNote&) = delete;

    // --- кто --------------------------------------------------------------
    const QString& path() const { return path_; }
    bool hasPath() const { return !path_.isEmpty(); }
    // Id заметки — имя файла без расширения (см. zametti-storage.md §2).
    QString id() const;

    // --- текст ------------------------------------------------------------
    ZDocument& doc() { return doc_; }
    const ZDocument& doc() const { return doc_; }
    // Подменить живой документ (свежая сборка при открытии). Прежний
    // возвращается: вид держит его живым до возврата в цикл событий.
    ZDocument replaceDoc(ZDocument fresh);

    // --- шапка ------------------------------------------------------------
    NoteHeader& meta() { return meta_; }
    const NoteHeader& meta() const { return meta_; }
    void setMeta(NoteHeader meta) { meta_ = std::move(meta); }
    // Мета, потерянная внешней правкой: показать человеку, что пропало, и
    // дать вернуть одним движением.
    void rememberLostMeta(NoteHeader lost) { lostMeta_ = std::move(lost); }
    bool hasLostMeta() const { return lostMeta_.present(); }
    NoteHeader takeLostMeta();

    // --- файл: «изменилось ли» -------------------------------------------
    // Каким файл был, когда мы его последний раз видели, и его копия целиком:
    // сравнение идёт побайтово, не считая строки modified в шапке.
    const Digest& digest() const { return digest_; }
    const QByteArray& lastSaved() const { return lastSaved_; }
    // Файл записан (нами) или перечитан (внешняя правка): что теперь на диске.
    void markWritten(const Digest& digest, QByteArray written);
    // Самопроверка при последней записи не сошлась. Признак заметки, а не
    // окна: при переключении едет вместе с ней. Возвращает, изменился ли.
    bool selfCheckFailed() const { return selfCheckFailed_; }
    bool setSelfCheckFailed(bool failed);

    // --- журнал -----------------------------------------------------------
    ZNoteHistory& history() { return history_; }
    const ZNoteHistory& history() const { return history_; }

    // --- слова и строки ---------------------------------------------------
    // Числа отвечают тому, что в документе сейчас, только пока fresh: ложь тут
    // дороже молчания — окно показывает «?» вместо числа слов.
    const NoteStats& stats() const { return stats_; }
    bool statsFresh() const { return statsFresh_; }
    void setStats(const NoteStats& stats);
    void invalidateStats() { statsFresh_ = false; }

    // --- место человека ---------------------------------------------------
    CaretSpot caret() const { return caret_; }
    void rememberCaret(const CaretSpot& spot) { caret_ = spot; }
    void rememberCaret(int cursor, int anchor, int scroll) { caret_ = {cursor, anchor, scroll}; }

    // --- отложенная заметка (кэш открытых) --------------------------------
    // Признак «изменена» осмыслен у отложенной: у открытой спрашивают документ.
    bool wasModified() const { return wasModified_; }
    void setWasModified(bool modified) { wasModified_ = modified; }
    // Оценка веса в кэше (см. documentCacheSizeMb).
    qint64 cachedBytes() const { return cachedBytes_; }
    void setCachedBytes(qint64 bytes) { cachedBytes_ = bytes; }

    // --- из чего собран документ: нужно заплатке -------------------------
    // ДОЛГ, названный вслух: копия содержимого, от которой мы уходим
    // (zametti-method-not-copy). Пока заплатка pathDocument сравнивает блоки,
    // они живут здесь; nullptr — заплатке не за что зацепиться.
    const std::vector<Piece>* builtBlocks() const { return builtValid_ ? &built_ : nullptr; }
    void setBuiltBlocks(std::vector<Piece> blocks);
    void invalidateBuilt() { builtValid_ = false; }

protected:
    QString path_;
    ZDocument doc_;
    NoteHeader meta_;
    NoteHeader lostMeta_;
    Digest digest_;
    QByteArray lastSaved_;
    ZNoteHistory history_;
    NoteStats stats_;
    bool statsFresh_ = false;
    bool selfCheckFailed_ = false;
    CaretSpot caret_;
    bool wasModified_ = false;
    qint64 cachedBytes_ = 0;
    std::vector<Piece> built_;
    bool builtValid_ = false;
};

}  // namespace zametti

#endif  // ZAMETTI_ZNOTE_H
