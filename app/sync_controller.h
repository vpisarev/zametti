// Контроллер синхронизации: кнопка облака, синк на старте и на выходе.
//
// Тонкая прослойка между окном и ZStorage::sync: движок гоняется в РАБОЧЕМ
// ПОТОКЕ (он трогает только файлы и журналы — под общим замком либо атомарно;
// каталог заметок обновляют сторожа хранилища и external-путь редактора), а
// итоги приезжают сигналами в главный. Подключение (useLastRemote — keyring
// по QDBus) выполняется в главном потоке, до рождения рабочего.
//
// Правил здесь нет: что и когда синкать, решают настройки и человек кнопкой;
// вопрос предохранителя показывает окно, контроллер лишь доносит список.

#ifndef ZAMETTI_SYNC_CONTROLLER_H
#define ZAMETTI_SYNC_CONTROLLER_H

#include "zstorage.h"

#include <QObject>
#include <QString>
#include <QStringList>

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
    explicit SyncController(std::shared_ptr<ZStorage> storage,
                            std::shared_ptr<SecretStore> secrets = nullptr,
                            QObject* parent = nullptr);
    ~SyncController() override;

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

signals:
    void stateChanged();
    void finished(bool ok);
    // Предохранитель массового удаления: прогон задержал удаления и ждёт
    // решения человека — единственный вопрос синка (решение владельца).
    void pendingDeletes(const QStringList& ids);

protected:
    // Подключение к облаку — лениво и в главном потоке (keyring по QDBus).
    bool ensureConnected();
    void run(ZStorage::SyncOptions options);
    void finishRun(bool ok, const QString& error);
    void joinWorker();

    std::shared_ptr<ZStorage> storage_;
    std::shared_ptr<SecretStore> secrets_;
    std::thread worker_;
    std::shared_ptr<std::atomic<bool>> cancel_;
    bool running_ = false;
    bool connected_ = false;
    QString lastError_;
    ZStorage::SyncReport lastReport_;
};

}  // namespace zametti

#endif  // ZAMETTI_SYNC_CONTROLLER_H
