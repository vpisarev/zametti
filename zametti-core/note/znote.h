// ZNote — заметка, которая открыта сейчас или была открыта недавно и к
// которой мы хотим быстро вернуться.
//
// Это ОБЪЕКТ, а не набор полей в структуре виджета (решение владельца,
// refactor2): всё, что переживает уход из заметки и возврат к ней, живёт
// здесь и уезжает в кэш открытых заметок целиком — забыть перенести поле
// нельзя, потому что переносится сам объект. Внутри:
//
//   * ZDocument — живая модель текста; правится только своими глаголами;
//   * ZJournal — журнал этой заметки ОДНИМ ПОЛЕМ, а не россыпью;
//   * NoteSearch — найденное в ней (кэш поиска: вернулись — оно при заметке);
//   * шапка (мета), отпечаток и последняя записанная копия — «изменилось ли»;
//   * производное от текста — счёт слов и строк, блоки сборки для заплатки
//     (Derived<T>, derived.h: значение + ревизия документа; новая кэшируемая
//     величина заводится тем же шаблоном, а не своим флагом);
//   * место человека в заметке: каретка, второй конец выделения, прокрутка.
//
// Что НЕ переживает ухода (серия набора, курсоры уборки) — здесь не живёт;
// это состояние текущего вида (см. NoteEditor). Режим истории — отдельный
// объект поверх того же журнала (ZNoteTimeline): заметка про него не знает.
//
// Держится через std::shared_ptr и не копируется: ZDocument внутри — ручка,
// и копия объекта разделяла бы документ, но заводила бы второй журнал и
// вторую каретку. Живой документ наружу отдаётся ровно одним вызовом —
// view->setDocument(note.doc().getDocument()) — и стережёт это сборка.

#ifndef ZAMETTI_ZNOTE_H
#define ZAMETTI_ZNOTE_H

#include "document.h"
#include "document_pieces.h"
#include "document_saver.h"
#include "hash.h"
#include "note_header.h"
#include "sort_order.h"
#include "text_stats.h"
#include "caret_spot.h"
#include "derived.h"
#include "note_search.h"
#include "journal.h"

#include <QByteArray>
#include <QString>

#include <memory>
#include <optional>
#include <string_view>

#include <vector>

namespace zametti {


class ZNote {
public:
    // Пустая заметка без пути: так выглядит редактор, пока ничего не открыто.
    ZNote() = default;
    // Заметка, только что прочитанная с диска: путь, байты файла (они же
    // последняя записанная копия), их отпечаток и её журнал. Документ пуст —
    // его собирает вид из разобранных блоков; шапка ставится setHeader.
    ZNote(QString path, QByteArray fileBytes, Digest digest, std::shared_ptr<ZJournal> journal);

    ZNote(const ZNote&) = delete;
    ZNote& operator=(const ZNote&) = delete;
    // Перенос — не копия: заметка уезжает целиком, второго журнала не заводится.
    // Нужен, чтобы вернуть заметку из функции значением (наборы, fromFile).
    ZNote(ZNote&&) = default;
    ZNote& operator=(ZNote&&) = default;

    // --- кто --------------------------------------------------------------
    const QString& path() const { return path_; }
    bool hasPath() const { return !path_.isEmpty(); }
    // Id заметки — имя файла без расширения (см. docs/info/zametti-storage.md §2).
    QString id() const;

    // --- текст ------------------------------------------------------------
    ZDocument& doc() { return doc_; }
    const ZDocument& doc() const { return doc_; }
    // Подменить живой документ (свежая сборка при открытии). Прежний
    // возвращается: вид держит его живым до возврата в цикл событий.
    ZDocument replaceDoc(ZDocument fresh);

    // --- круг файла: шапка + тело ------------------------------------------
    //
    // ФАЙЛ ЗАМЕТКИ = ШАПКА + ТЕЛО, И СКЛЕИВАЕТ ИХ ЗАМЕТКА (решение владельца:
    // ZDocument — чистая часть .md, без метаданных; документы заводятся и для
    // служебных целей — разность, слепок, бумага, — и шапки у них нет). Шапка
    // (NoteHeader) — конверт файла: чужие ключи и порядок строк — байт в байт.
    // Круг файл→заметка→файл проверяет тоже заметка (save с самопроверкой).
    NoteHeader& header() { return header_; }
    const NoteHeader& header() const { return header_; }
    void setHeader(NoteHeader header) { header_ = std::move(header); }
    // Разобрать байты файла: шапка → сюда, тело → документ. Пустая заметка без
    // шапки — тоже заметка (файл вне хранилища).
    bool load(std::string_view bytes);
    // Байты файла целиком: конверт + каноническое тело.
    std::string toMarkdown() const;
    // Совпадают ли канонические байты с исходными. Так проверяется ДРЕЙФ.
    bool isCanonical(std::string_view original) const;
    // Дамп строения в JSON — шапка и блоки (золотые наборы, отладка).
    std::string toJson() const { return doc_.toJson(header_); }
    // Байты, какими они лягут в файл, — без записи (сравнить с прошлой копией).
    QByteArray fileBytes(std::vector<Piece>* fileBlocks = nullptr, bool* enriched = nullptr,
                         bool* reshaped = nullptr) const;
    // Записать в файл штатным путём (self-check, атомарно) в своём конверте.
    SaveOutcome save(const QString& path, const QString& timestamp, const Digest& known = {},
                     const std::vector<Piece>* prebuiltBlocks = nullptr,
                     const QByteArray* prebuiltText = nullptr);
    // Стаб архива: шапка с пометкой и первый заголовок; тело живёт в журнале.
    // Привести файл заметки к канону прямо на диске. Право на это у программы
    // есть: хранилище наше, и лишние пробелы в конце строк или недостающий
    // перевод строки в конце файла — не содержимое, а сор. Ни одного значения
    // в шапке не меняется, в том числе modified: заметку всего лишь открыли.
    // Записывается ровно то, что дал бы круг файл→заметка→файл, — та же
    // канонизация, что и при записи, только без штампа. Трогаем только НАШИ
    // заметки — с блоком метаданных: чужой .md программе не принадлежит.
    // Истина — файл переписан (text и digest обновлены); любая неудача — тихая
    // ложь: не смогли причесать, показываем как есть.
    static bool canonicaliseFile(const QString& path, std::string& text, Digest& digest);

    // --- метаданные ---------------------------------------------------------
    //
    // ОДНО СТРУКТУРИРОВАННОЕ МЕСТО (решение владельца) — то, что о заметке знают
    // дерево, список, поиск, каталог хранилища: id, путь, родитель, заголовок,
    // сниппет, времена, метка сортировки, архив, папка, находки и ДОСТУП.
    // Собирается из шапки и тела; правится глаголами заметки ниже, которые
    // пишут в шапку. Позже сюда же — теги.
    class Metadata {
    public:
        const QString& id() const { return id_; }
        const QString& path() const { return path_; }
        const QString& parent() const { return parent_; }
        const QString& title() const { return title_; }       // «Без названия», если тела нет
        const QString& snippet() const { return snippet_; }
        // Времена — в СРАВНИМОЙ форме (UTC, ISO): по ним сортируют строками.
        const QString& modified() const { return modified_; }
        const QString& created() const { return created_; }
        std::optional<SortOrder> sortMark() const { return sortMark_; }
        bool archived() const { return archived_; }
        // Своя пометка `access: read-only`. НАСЛЕДОВАНИЕ ОТ ПАПКИ здесь не
        // учитывается: у одной заметки нет ни родителей, ни каталога — подъём
        // по цепочке делает ZStorage::isReadOnly.
        bool readOnly() const { return readOnly_; }
        // The soft lock (`lock: yes`, NoteHeader::kLockKey): the note's own
        // mark, nothing inherited — a lock belongs to one note.
        bool locked() const { return locked_; }
        // A BOOK (`role: book`, brief 18): the list shows author and year
        // instead of a snippet, the window opens it in reading mode.
        bool book() const { return book_; }
        const QString& author() const { return author_; }
        const QString& year() const { return year_; }
        bool folder() const { return folder_; }
        bool lostFound() const { return lostFound_; }
        // Корневая заметка хранилища: она же его имя и его настройки показа.
        bool root() const { return root_; }
        bool valid() const { return !id_.isEmpty(); }

        // Прочитать с диска: шапка и первый блок. Ложь valid() — не читается.
        static Metadata fromFile(const QString& path);

    protected:
        friend class ZNote;
        QString id_;
        QString path_;
        QString parent_;
        QString title_;
        QString snippet_;
        QString modified_;
        QString created_;
        std::optional<SortOrder> sortMark_;
        bool archived_ = false;
        bool readOnly_ = false;
        bool locked_ = false;
        bool book_ = false;
        QString author_;
        QString year_;
        bool folder_ = false;
        bool lostFound_ = false;
        bool root_ = false;
    };
    Metadata metadata() const;

    // Глаголы метаданных — пишут в шапку. Ключи шапки названы здесь и в
    // znote.cpp один раз.
    QString parentId() const;
    void setParentId(const QString& id);   // пусто — заметка в корне
    // Папка: role folder ИЛИ root — корень тоже папка, см. znote.cpp.
    bool isFolder() const;
    bool isLost() const;
    // КОРНЕВАЯ ЗАМЕТКА ХРАНИЛИЩА (role: root). Её заголовок — имя хранилища,
    // её метка sort — порядок «всех заметок». parent у корневых заметок
    // остаётся ПУСТЫМ: 282 переписанные шапки — это 282 записи в журналы и
    // первый синк ценой всего хранилища, а бюро находок держится ровно на
    // различии «пустой parent» против «неразрешимый parent».
    bool isRoot() const;
    QString role() const;
    void setRole(const QString& role);     // "folder", "lost", "root", пусто — заметка
    bool isArchived() const;
    void setArchived(bool archived);
    // Своя пометка доступа (см. NoteHeader::kAccessKey). Read-only у ПАПКИ
    // запирает и всё, что внутри, — но об этом знает хранилище, а не заметка.
    bool isReadOnly() const;
    void setReadOnly(bool readOnly);
    // The soft lock (NoteHeader::kLockKey): typed input is refused, the file
    // is still written — annotations, bookmarks, the lock's own removal.
    bool isLocked() const;
    void setLocked(bool locked);
    // A book (`role: book`). The book keys of the header — author, translator,
    // year, isbn, publisher, series, lang, genre, cover, source — are read
    // through headerValue(); the two the list shows have their own verbs.
    bool isBook() const;
    QString bookAuthor() const;
    QString bookYear() const;
    QString created() const;
    QString modified() const;
    void stampModified();
    std::optional<SortOrder> sortMark() const;
    void setSortMark(std::optional<SortOrder> order);
    QString headerValue(const QString& key) const;
    void setHeaderValue(const QString& key, const QString& value);
    bool hasHeader() const { return header_.present(); }
    void setHasHeader(bool present) { header_.setPresent(present); }
    // Заголовок и сниппет — у тела (ZDocument::title/snippet); здесь для
    // симметрии с каталогом: заголовок с запасным «Без названия».
    QString title() const;

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
    // Журнал — ОДИН объект на заметку, и держится он умным указателем: режим
    // истории (ZNoteTimeline) читает тот же журнал, что заметка пишет, — с тем
    // же разжатым хвостом и тем же признаком «чищен», а не своей копией, у
    // которой чистка и хвост разошлись бы с заметкой. Ссылка не бывает пустой:
    // без хранилища это пустой ZJournal, у которого все файловые глаголы «нет».
    ZJournal& journal() { return *journal_; }
    const ZJournal& journal() const { return *journal_; }
    // Тот же объект разделяет режим истории: разжатый хвост и «чищено за этот
    // заход» у заметки и у её истории общие.
    std::shared_ptr<ZJournal> journalPtr() const { return journal_; }

    // --- найденное (кэш поиска) --------------------------------------------
    // Запрос и вхождения в документе этой заметки; переживают уход и возврат.
    // Свежесть — по ревизии документа (NoteSearch::isFreshFor).
    NoteSearch& search() { return search_; }
    const NoteSearch& search() const { return search_; }

    // --- слова и строки (производное, см. derived.h) ----------------------
    // Числа отвечают тому, что в документе сейчас, только пока свежи — по
    // ревизии документа: ложь тут дороже молчания, окно показывает «?».
    const NoteStats& stats() const { return stats_.value(); }
    // The outline of the document as it is now (recounted when the document
    // changed since the last ask).
    const std::vector<ZDocument::OutlineEntry>& outline();
    bool statsFresh() const { return stats_.freshFor(doc_.revision()); }
    // Считались и не сброшены вслух — даже если с тех пор правили. Нужно
    // тому, кто сообщает «числа устарели» ровно один раз, а не на каждую букву.
    bool statsCounted() const { return stats_.valid(); }
    void setStats(const NoteStats& stats) { stats_.set(stats, doc_.revision()); }
    void invalidateStats() { stats_.invalidate(); }

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
    // (zametti-method-not-copy). Пока заплатка patchDocument сравнивает блоки,
    // они живут здесь; nullptr — заплатке не за что зацепиться. Свежесть у
    // них НЕ по ревизии: заплатка как раз сравнивает собранное с правленым.
    const std::vector<Piece>* builtBlocks() const {
        return built_.valid() ? &built_.value() : nullptr;
    }
    void setBuiltBlocks(std::vector<Piece> blocks) { built_.set(std::move(blocks), doc_.revision()); }
    void invalidateBuilt() { built_.invalidate(); }

protected:
    QString path_;
    ZDocument doc_;
    NoteHeader header_;
    NoteHeader lostMeta_;
    Digest digest_;
    QByteArray lastSaved_;
    std::shared_ptr<ZJournal> journal_ = std::make_shared<ZJournal>();
    NoteSearch search_;
    // The table of contents, by revision (brief 18): asked by the ToC list
    // and by the reading mode's status line; one walk when stale.
    Derived<std::vector<ZDocument::OutlineEntry>> outline_;
    Derived<NoteStats> stats_;
    bool selfCheckFailed_ = false;
    CaretSpot caret_;
    bool wasModified_ = false;
    qint64 cachedBytes_ = 0;
    Derived<std::vector<Piece>> built_;
};

}  // namespace zametti

#endif  // ZAMETTI_ZNOTE_H
