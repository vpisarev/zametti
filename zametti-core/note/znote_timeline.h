// ZNoteTimeline — заметка, прочитанная ИЗ ИСТОРИИ: read-only аналог ZNote.
//
// ЗАЧЕМ. Режим истории жил семнадцатью полями внутри NoteEditor: рамки
// журнала, номер записи, разобранный слепок, база, два сравнения, четыре
// готовых документа, признаки вида — и живая заметка на это время уезжала из
// редактора в дочернее поле, а редактор становился только чтением. Ровно на
// этом пути однажды утекла в живую заметку наша служебная строка. Теперь всё,
// что режим показывает, живёт здесь одним объектом, а редактор про историю не
// знает вовсе: живой буфер из него не уезжает.
//
// ЧТО ВНУТРИ:
//   * журнал заметки — ТОТ ЖЕ объект, что у ZNote (shared_ptr): разжатый хвост
//     и признак «чищен» у заметки и у истории общие;
//   * рамки записей (таймлайн) и номер показанной;
//   * показанный слепок — байты, как в журнале, и его строки сравнения;
//   * база сравнения — предыдущая запись ЛИБО свежая версия из буфера (два
//     сравнения, а не четыре: решение владельца, инверсии по Tab больше нет);
//   * документ разности на каждую базу — ZDocument, собирается лениво и живёт,
//     пока показан этот слепок (переключение базы — подмена указателя);
//   * найденное в документе разности (NoteSearch — как у ZNote).
//
// ЧЕГО ЗДЕСЬ НЕТ. Ни одного глагола правки: слепок неприкосновенен по
// построению, а показывается документ разности — отдельный артефакт, который на
// диск не уходит ни по какому пути. Восстановление берёт snapshotBody() —
// каноническое тело слепка, а не то, что нарисовано.
//
// Настройки классу не даются: облик документа разности приходит стилем в
// конструктор (копия стиля редактора), правила отбора живут в журнале.

#ifndef ZAMETTI_ZNOTE_TIMELINE_H
#define ZAMETTI_ZNOTE_TIMELINE_H

#include "diff.h"
#include "document.h"
#include "journal.h"
#include "note_search.h"
#include "znote_history.h"

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVector>

#include <array>
#include <memory>
#include <optional>
#include <string>

namespace zametti {

class ZNoteTimeline {
public:
    enum class Base { Previous, Fresh };

    // history — журнал заметки (тот же объект, что у ZNote); fresh — байты файла
    // живой заметки СЕЙЧАС (ZNote::fileBytes()); style — облик документа
    // разности (nullptr — из настроек).
    ZNoteTimeline(std::shared_ptr<ZNoteHistory> history, QByteArray fresh,
                  std::shared_ptr<const ZDocStyle> style);

    ZNoteTimeline(const ZNoteTimeline&) = delete;
    ZNoteTimeline& operator=(const ZNoteTimeline&) = delete;

    // Прочитать рамки (внутри — ленивая чистка журнала, второй триггер) и
    // встать на запись index; -1 — последняя со слепком. false — истории нет
    // (журнала нет, он не читается или в нём ни одного слепка).
    bool open(int index = -1, QString* error = nullptr);
    bool isOpen() const { return index_ >= 0; }

    // --- список записей ---------------------------------------------------
    const journal::ZJournal& journal() const { return journal_; }
    const QVector<journal::Entry>& entries() const { return journal_.entries(); }
    int count() const { return journal_.size(); }
    int index() const { return index_; }
    // Последняя запись со слепком; -1 — таких нет (надгробия пропущены).
    int lastSnapshotIndex() const;
    // Показать запись index. false — вне диапазона или без слепка (надгробие),
    // либо слепок не собрать: тогда показанное не меняется.
    bool select(int index, QString* error = nullptr);
    // Шаг к более старому слепку; false — дальше в прошлое некуда.
    bool stepBack();
    // Шаг к более новому; false — дальше последнего слепка только живая версия
    // (выход из режима решает вызывающий).
    bool stepForward();

    // --- база сравнения ---------------------------------------------------
    Base base() const { return base_; }
    void setBase(Base base);
    bool baseIsFresh() const { return base_ == Base::Fresh; }
    // Время записи-базы; 0 — база свежая версия или базы нет (первая запись).
    qint64 baseTime() const;

    // --- разность текущей пары (index × base) ------------------------------
    // Сравнение база → слепок: зелёное — есть в слепке и нет в базе.
    const diff::Result& result();
    int changedLines();
    // Документ разности — ZDocument; для той же пары отдаётся тот же документ.
    ZDocument& document();
    // Соответствие блок документа ↔ строка сравнения. У изменённой строки два
    // блока с одним номером.
    int rowOfBlock(int block);
    // Строка СЛЕПКА (сторона after) у блока; у убранной строки — строка
    // ближайшей соседней снизу; -1 — нет.
    int afterLineOfBlock(int block);
    // Ближайший сверху блок, чья строка слепка не позже line; -1 — нет.
    int blockOfAfterLine(int line);
    diff::Mark markOfBlock(int block);

    // --- исходный слепок (истина; ни одной дорисовки) -----------------------
    // Каноническое тело слепка (без шапки) — восстановлению.
    std::string snapshotBody() const;
    qint64 snapshotTime() const;
    journal::Kind snapshotKind() const;

    // --- поиск ------------------------------------------------------------
    NoteSearch& search() { return search_; }
    std::shared_ptr<ZNoteHistory> history() const { return history_; }

    // --- облик ------------------------------------------------------------
    // Облик сменился: документы разности выбрасываются и соберутся заново.
    void setStyle(std::shared_ptr<const ZDocStyle> style);
    void dropDocuments();

protected:
    std::shared_ptr<ZNoteHistory> history_;
    QByteArray fresh_;
    std::shared_ptr<const ZDocStyle> style_;
    journal::ZJournal journal_;
    int index_ = -1;
    QByteArray snapshotBytes_;
    QStringList snapshotLines_;
    Base base_ = Base::Previous;
    // На каждую базу: строки базы и её время, сравнение, документ и карта
    // блоков. Слот живёт, пока показан этот слепок; смена слепка сбрасывает оба.
    struct Slot {
        bool ready = false;
        qint64 time = 0;
        QStringList lines;
        diff::Result result;
        std::optional<ZDocument> doc;
        QVector<int> rowOfBlock;
    };
    std::array<Slot, 2> slots_;
    NoteSearch search_;

    Slot& slot();
    void computeSlot(Slot& slot, Base base);
    void ensureDocument(Slot& slot);
    void resetSlots();
    int previousSnapshotIndex(int from) const;
};

}  // namespace zametti

#endif  // ZAMETTI_ZNOTE_TIMELINE_H
