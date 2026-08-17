// ZStorage — хранилище как объект.
//
// С КАТАЛОГОМ ХРАНИЛИЩА РАЗГОВАРИВАЕТ ТОЛЬКО ОН (решение владельца). Раньше
// роль хранилища делили трое (аудит refactor2, §1.4): дерево читало диск и
// держало единственный кэш заголовков, лямбды main() писали файлы (в том числе
// мимо штатного пути записи — rewriteNote), редактор заводил журнал по корню.
// Здесь это одно место:
//
//   * КАТАЛОГ — что лежит в хранилище: по id заметки её шапка (родитель,
//     заголовок, сниппет, времена, метка сортировки, архив, папка, находки) и
//     путь файла. Читается сканом (reload) и по одной заметке (refreshNote);
//     дерево и список — проекции каталога, диск сами не читают;
//   * ПУТИ И ЖУРНАЛЫ — по id: pathOf(id), historyOf(id, rules);
//   * ПРАВКА ШАПКИ ЗАКРЫТОЙ ЗАМЕТКИ (rename/move/sort у заметки, которой нет в
//     редакторе) — ШТАТНЫМ путём записи: разбор, правка глаголами ZDocument,
//     saveTo с самопроверкой и атомарной записью, шаг журнала. Обходной записи
//     на диск больше нет.
//
// Что дальше сюда переедет: замок хранилища, создание/архив/удаление,
// сигналы каталога (noteChanged/added/removed) для дерева и списка, чтение
// вложений для кэша картинок.

#ifndef ZAMETTI_ZSTORAGE_H
#define ZAMETTI_ZSTORAGE_H

#include "document.h"
#include "history_rules.h"
#include "sort_order.h"
#include "znote_history.h"

#include <QHash>
#include <QLockFile>
#include <QString>
#include <QStringList>

#include <functional>
#include <memory>
#include <optional>

namespace zametti {

class ZStorage {
public:
    // Запись каталога — шапка заметки, прочитанная с диска. Только чтение:
    // наполняет её хранилище.
    class NoteInfo {
    public:
        const QString& id() const { return id_; }
        const QString& parent() const { return parent_; }
        const QString& title() const { return title_; }
        const QString& snippet() const { return snippet_; }
        // Времена — в СРАВНИМОЙ форме (UTC, ISO): по ним сортируют строками.
        const QString& modified() const { return modified_; }
        const QString& created() const { return created_; }
        const QString& path() const { return path_; }
        std::optional<SortOrder> sortMark() const { return sortMark_; }
        bool archived() const { return archived_; }
        bool folder() const { return folder_; }
        bool lostFound() const { return lostFound_; }
        bool valid() const { return !id_.isEmpty(); }

    protected:
        friend class ZStorage;
        QString id_;
        QString parent_;
        QString title_;
        QString snippet_;
        QString modified_;
        QString created_;
        QString path_;
        std::optional<SortOrder> sortMark_;
        bool archived_ = false;
        bool folder_ = false;
        bool lostFound_ = false;
    };

    // Плоское ли это хранилище (метка — каталог .zametti).
    static bool isStoreRoot(const QString& dir);

    // Корень приводится к чистому виду ОДИН РАЗ, у двери: дальше он расходится
    // по всей программе, и «vpnotes//<id>.md» не равен по строке «vpnotes/<id>.md».
    explicit ZStorage(const QString& root);

    const QString& root() const { return root_; }
    bool isStore() const { return store_; }

    // --- замок --------------------------------------------------------------
    // Одно хранилище — одна программа: второй экземпляр на том же хранилище
    // писал бы в те же файлы и журналы, ничего не зная о первом. Замок
    // файловый (процессы разные), лежит внутри хранилища, стоит 4.4 мс один
    // раз за запуск. Забытый замок мёртвого процесса нашей машины снимается
    // сам (QLockFile ждал бы полминуты, а перезапуск сразу после падения —
    // самый частый случай); живой pid не трогается никогда.
    struct LockReport {
        bool locked = false;
        qint64 holderPid = 0;      // кто держит, если не вышло
        QString holderHost;
        QString note;              // что сделали по пути (сняли труп…), для лога
    };
    LockReport lock();
    // --unlock: снять чужой замок руками — когда QLockFile судить не берётся
    // (тот же pid достался чужому процессу, сетевая шара). Отвечает, был ли он.
    LockReport forceUnlock();
    bool isLocked() const;
    QString lockPath() const;

    // --- каталог ---------------------------------------------------------
    // Перечитать всё хранилище (скан «<id>.md»). Не хранилище — каталог пуст.
    void reload();
    // Перечитать одну заметку. Ложь — не читается (запись остаётся прежней)
    // или файла нет (запись снимается).
    bool refreshNote(const QString& id);
    const NoteInfo* info(const QString& id) const;
    bool has(const QString& id) const { return notes_.contains(id); }
    QStringList ids() const { return notes_.keys(); }
    int count() const { return int(notes_.size()); }

    // --- пути и журнал -----------------------------------------------------
    QString pathOf(const QString& id) const;
    static QString idOfPath(const QString& path);
    ZNoteHistory historyOf(const QString& id, const history::Rules& rules) const;

    // --- вопросы к каталогу ------------------------------------------------
    // Всё — по каталогу в памяти, диск не трогается.
    bool isFolder(const QString& id) const;
    // Помечена архивной сама или кто-то выше по цепочке родителей.
    bool inArchive(const QString& id) const;
    QStringList childrenOf(const QString& id) const;   // прямые дети, любой порядок
    // Потомки, дети РАНЬШЕ родителей: удаление подряд не наткнётся на папку, в
    // которой ещё что-то лежит.
    QStringList descendantsOf(const QString& id) const;
    // Заметка без содержательного текста (папка — без детей). Закрытую читаем
    // с диска; открытую спрашивайте у редактора — набранное могло не дойти.
    bool isEmptyNote(const QString& id) const;
    QString titleOf(const QString& id) const;

    // --- при открытии --------------------------------------------------------
    // Ленивые миграции хранилища, идемпотентные: корзина прошлых версий →
    // архив; заметки с оборванным parent — в бюро находок (единственное место,
    // где открытие ПИШЕТ; правило владельца). Что сделано — строками в лог
    // вызывающему; каталог перечитан, если что-то поменялось.
    QStringList migrate();
    // Импорт чужого .md: копия под свежим id в каноническом виде, источник не
    // трогается. Возвращает id; пусто — ошибка в error.
    QString importNote(const QString& parentId, const QString& sourcePath, QString* error);

    // --- операции ------------------------------------------------------------
    // Каждая — одна точка правды: пишет штатно, отмечает журнал, обновляет
    // каталог. Открытую в редакторе заметку вызывающий сохраняет ДО и
    // перечитывает ПОСЛЕ: человек убирает то, что видит, а на месте убранной
    // теперь стаб.
    //
    // Новая заметка или папка (папка — та же заметка с role: folder и
    // заголовком «Новая папка»). Родитель, которого нет или который в архиве,
    // — корень. Возвращает id; пусто — ошибка в error.
    QString createNote(const QString& parentId, bool folder, QString* error);
    // В АРХИВ: тело в журнал, файл — стаб с пометкой, parent не трогается;
    // папка — со всем поддеревом (пометку получает каждая заметка: файлы при
    // синхронизации приезжают поодиночке). Кто не убрался — в failed
    // («заголовок: почему»); истина — пусто ли failed.
    bool archive(const QString& id, const history::Rules& rules, QStringList* failed);
    // ИЗ АРХИВА: пометка снимается, тело возвращается из головы журнала,
    // заметка оказывается там, откуда её убрали (parent цел); папка — с
    // поддеревом.
    bool restore(const QString& id, QStringList* failed);
    // НАСОВСЕМ: архивная — вместе с журналом (тело живёт в нём и больше
    // нигде), прочая — файл в мусорку ОС, журнал остаётся (надгробие); картинки,
    // на которые больше никто не ссылается, — следом в ту же мусорку.
    bool remove(const QString& id, QString* error);

    // Именованные правки шапки закрытой заметки — те же rewriteNote, но
    // вызывающему не надо знать, какими глаголами это делается.
    bool rename(const QString& id, const QString& title, const history::Rules& rules, QString* error);
    bool move(const QString& id, const QString& parentId, const history::Rules& rules, QString* error);
    bool setSortMark(const QString& id, std::optional<SortOrder> order, const history::Rules& rules,
                     QString* error);

    // --- правка шапки закрытой заметки -----------------------------------
    // Разобрать файл, применить change к заметке, записать штатным путём
    // (самопроверка, атомарно, шаг журнала Save), обновить каталог. Ложь —
    // объяснение в error. Открытую в редакторе заметку так править нельзя:
    // сторож файла примет запись за чужую; для неё — глаголы редактора.
    bool rewriteNote(const QString& id, const std::function<void(ZDocument&)>& change,
                     const history::Rules& rules, QString* error);

protected:
    QString root_;
    bool store_ = false;
    QHash<QString, NoteInfo> notes_;
    std::shared_ptr<QLockFile> lock_;   // заведён при первом lock()

    bool readInfo(const QString& path, NoteInfo& out) const;
};

}  // namespace zametti

#endif  // ZAMETTI_ZSTORAGE_H
