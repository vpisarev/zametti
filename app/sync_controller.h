// Контроллер синхронизации: кнопка облака, синк на старте и на выходе.
//
// Тонкая прослойка между окном и ZStorage::sync: движок гоняется в РАБОЧЕМ
// ПОТОКЕ (он трогает только файлы и журналы — под общим замком либо атомарно;
// каталог заметок обновляют сторожа хранилища и external-путь редактора), а
// итоги приезжают сигналами в главный.
//
// ПОТОКИ РАЗДЕЛЕНЫ ПО ПРИРОДЕ ВЕЩЕЙ: секреты достаются В ГЛАВНОМ (keyring —
// QDBus, шина живёт при главном цикле), а АДАПТЕР ОБЛАКА рождается В ПОТОКЕ
// ПРОГОНА — QNetworkAccessManager однопоточен, и созданный в главном он из
// рабочего не работает (первый же живой GUI-прогон по WebDAV это показал).
// Потому подключение к облаку повторяется каждым прогоном заново: цена —
// один GET манифеста.
//
// Правил здесь нет: что и когда синкать, решают настройки и человек кнопкой;
// вопрос предохранителя показывает окно, контроллер лишь доносит список.

#ifndef ZAMETTI_SYNC_CONTROLLER_H
#define ZAMETTI_SYNC_CONTROLLER_H

#include "keyfile.h"
#include "zstorage.h"

#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <atomic>
#include <memory>
#include <thread>

namespace zametti {

class SecretStore;

class SyncController : public QObject {
    Q_OBJECT

public:
    // secrets — чей keyring спрашивать; по умолчанию системный
    // (KeyringSecrets), наборы подают свой.
    // logs — err.log/sync.log приложения (не владеем; nullptr = не писать).
    explicit SyncController(std::shared_ptr<ZStorage> storage,
                            std::shared_ptr<SecretStore> secrets = nullptr,
                            ZLogs* logs = nullptr, QObject* parent = nullptr);
    ~SyncController() override;

    // ПЕРЕЕХАТЬ НА ДРУГОЕ ХРАНИЛИЩЕ (пусто — окно без хранилища). Прогон, если
    // он идёт, отменяется и ДОЖИДАЕТСЯ: рабочий поток держит свою копию
    // указателя, и прежнее хранилище не умрёт, пока он жив.
    void setStorage(std::shared_ptr<ZStorage> storage);

    // Настроен ли синк у этой копии хранилища (.zametti/remote.json).
    bool configured() const;
    bool running() const { return running_; }

    // Кнопка: не идёт — полный прогон, идёт — отмена. Прерывание безопасно
    // в любой точке (инвариант E), поэтому отмена не спрашивает ничего.
    void toggle();
    void startFull(bool allowMassDelete = false);
    void cancel();

    // Push-only на выходе: СИНХРОННО, бюджет времени внутри движка. Тихий —
    // офлайн и «не настроен» не жалуются, dirty-set переживает до следующего
    // прогона.
    void pushOnExit();

    // Отказ от массового удаления: заметки объявляются живыми (записи поверх
    // надгробий) и прогон доделывается — облако лечится.
    void declareAliveAndFinish(const QStringList& ids);

    // Словами — для тултипа кнопки. Одна строка, без истории.
    QString statusText() const;
    const ZStorage::SyncReport& lastReport() const { return lastReport_; }

    // --- индикатор в статус-баре (решение владельца) ------------------------
    //
    //   cloud sync: [......*.............  3/20]
    //
    // «*» ездит по точкам вперёд-назад; счётчик дополняется пробелами СЛЕВА
    // до ширины итога, чтобы «]» не дёргался, пока число растёт. Обе части —
    // чистыми функциями: их проверяет набор, а таймер только зовёт.
    static constexpr int kBounceWidth = 20;
    // Позиция «*» на такте tick: 0,1,…,width-1,width-2,…,1,0,1,…
    static int bounceAt(int tick, int width = kBounceWidth);
    // Готовая строка индикатора; total <= 0 — итог ещё неизвестен, «0/?».
    static QString progressLine(int tick, int done, int total);

signals:
    void stateChanged();
    void finished(bool ok);
    // Строка для статус-бара; пустая — прогон кончился, индикатор убрать.
    void progress(const QString& line);
    // Предохранитель массового удаления: прогон задержал удаления и ждёт
    // решения человека — единственный вопрос синка (решение владельца).
    void pendingDeletes(const QStringList& ids);

protected:
    // Достать секреты — В ГЛАВНОМ потоке (keyring по QDBus): значения кладутся
    // в поля, поток прогона берёт их копиями.
    bool fetchSecrets();
    void run(ZStorage::SyncOptions options);
    void finishRun(bool ok, const QString& error);
    void joinWorker();

    std::shared_ptr<ZStorage> storage_;
    std::shared_ptr<SecretStore> secrets_;
    ZLogs* logs_ = nullptr;
    // Добытое fetchSecrets — значения для потока прогона.
    ZStorage::RemoteConfig cfg_;
    Keyfile keyfile_;
    QString serverPassword_;
    std::thread worker_;
    std::shared_ptr<std::atomic<bool>> cancel_;
    std::shared_ptr<std::atomic<int>> progressDone_;
    std::shared_ptr<std::atomic<int>> progressTotal_;
    QTimer ticker_;
    int tick_ = 0;
    bool running_ = false;
    QString lastError_;
    ZStorage::SyncReport lastReport_;
};

}  // namespace zametti

#endif  // ZAMETTI_SYNC_CONTROLLER_H
