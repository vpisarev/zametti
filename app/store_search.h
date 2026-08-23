// Поиск по всем заметкам хранилища — вне UI-потока.
//
// Требование архитектурное, а не про сегодняшний корпус: замер этапа 4 даёт
// 11 мс на полный проход по 271 заметке и 113 мс по 2710 — второе уже видно
// глазом как рывок при наборе. Поэтому проход живёт в своём потоке, а новый
// запрос отменяет предыдущий; отмена проверяется между файлами, чтобы не
// бросать заметку разобранной наполовину.

#ifndef ZAMETTI_STORE_SEARCH_H
#define ZAMETTI_STORE_SEARCH_H

#include "hash.h"

#include <QObject>
#include <QString>
#include <QThread>
#include <QVector>

#include <atomic>
#include <memory>

namespace zametti {

// Одно совпадение в списке результатов. Заголовок здесь же: список рисуется
// в UI-потоке, ходить за ним в модель дерева ему незачем.
struct SearchResult {
    QString noteId;
    QString path;
    QString title;
    QString line;        // строка с совпадением, обрезанная по краям
    int lineOffset = 0;  // где в ней совпадение — для подсветки
    int lineLength = 0;
    int ordinal = 0;     // какое это совпадение по счёту внутри заметки

    // Находка в СЛЕПКЕ ИСТОРИИ, а не в живой заметке (поиск по истории
    // заметки). Адресуется запись парой (время, отпечаток), а не номером:
    // номер протухает от чистки журнала — она выкидывает дубликаты, и всё,
    // что после, съезжает. Ноль во времени — находка в живой заметке.
    qint64 snapshotTime = 0;
    Digest snapshotDigest;

    // Находка в АРХИВНОЙ заметке. Раньше таких не бывало вовсе: у архивной в
    // файле лежал стаб, и текста для поиска в ней не было. Теперь тело на
    // месте, и убранное находится — это к лучшему, но человек обязан видеть,
    // что нашёл убранное, и находки эти идут ПОСЛЕ всех живых (решение
    // владельца): ищут обычно среди того, чем пользуются.
    bool archived = false;
};

class StoreSearch : public QObject {
    Q_OBJECT

public:
    explicit StoreSearch(QObject* parent = nullptr);
    ~StoreSearch() override;

    // Запустить поиск. Предыдущий запрос отменяется: его результаты, если он
    // ещё бежит, будут отброшены по номеру поколения.
    void search(const QString& root, const QString& text);
    // Бросить текущий поиск и ничего не искать.
    void cancel();

signals:
    // Результаты последнего актуального запроса. truncated — упёрлись в
    // потолок и показали не всё: молча обрезать список нельзя.
    void found(const QString& text, const QVector<zametti::SearchResult>& results,
               bool truncated, qint64 elapsedMs);

private:
    class Worker;

    QThread thread_;
    Worker* worker_ = nullptr;
    std::shared_ptr<std::atomic<quint64>> latest_;
    quint64 generation_ = 0;
};

}  // namespace zametti

Q_DECLARE_METATYPE(zametti::SearchResult)

#endif  // ZAMETTI_STORE_SEARCH_H
