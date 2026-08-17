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
#include <QString>
#include <QStringList>

#include <functional>
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

    bool readInfo(const QString& path, NoteInfo& out) const;
};

}  // namespace zametti

#endif  // ZAMETTI_ZSTORAGE_H
