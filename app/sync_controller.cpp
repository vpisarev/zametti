#include "sync_controller.h"

#include "keyring_secrets.h"
#include "settings.h"

#include <QMetaObject>

namespace zametti {

SyncController::SyncController(std::shared_ptr<ZStorage> storage,
                               std::shared_ptr<SecretStore> secrets, QObject* parent)
    : QObject(parent),
      storage_(std::move(storage)),
      secrets_(secrets ? std::move(secrets) : std::make_shared<KeyringSecrets>()) {}

SyncController::~SyncController() {
    // Выходим — просим движок остановиться и ЖДЁМ: рабочий поток держит
    // указатель на хранилище, пережить его он не имеет права.
    if (cancel_) cancel_->store(true);
    joinWorker();
}

void SyncController::joinWorker() {
    if (worker_.joinable()) worker_.join();
}

bool SyncController::configured() const {
    return storage_ != nullptr && storage_->isStore() && !storage_->remoteConfig().isEmpty();
}

bool SyncController::ensureConnected() {
    if (storage_ == nullptr) return false;
    if (connected_ && storage_->hasRemote()) return true;
    QString why;
    if (!storage_->useLastRemote(*secrets_, &why)) {
        lastError_ = why;
        return false;
    }
    connected_ = true;
    return true;
}

void SyncController::toggle() {
    if (running_)
        cancel();
    else
        startFull(false);
}

void SyncController::cancel() {
    if (running_ && cancel_) cancel_->store(true);
}

void SyncController::startFull(bool allowMassDelete) {
    if (running_ || !configured()) return;
    if (!ensureConnected()) {
        emit stateChanged();
        emit finished(false);
        return;
    }
    joinWorker();
    ZStorage::SyncOptions options;
    options.mode = ZStorage::SyncOptions::Full;
    options.allowMassDelete = allowMassDelete;
    cancel_ = std::make_shared<std::atomic<bool>>(false);
    options.cancel = cancel_;
    running_ = true;
    lastError_.clear();
    emit stateChanged();
    run(options);
}

void SyncController::run(ZStorage::SyncOptions options) {
    worker_ = std::thread([this, options] {
        ZStorage::SyncReport report;
        QString error;
        const bool ok = storage_->sync(options, &report, &error);
        // Итог — в главный поток; сам контроллер живёт дольше потока
        // (деструктор ждёт join), так что this здесь надёжен.
        QMetaObject::invokeMethod(
            this,
            [this, ok, report, error] {
                lastReport_ = report;
                finishRun(ok, error);
            },
            Qt::QueuedConnection);
    });
}

void SyncController::finishRun(bool ok, const QString& error) {
    running_ = false;
    lastError_ = ok ? QString() : error;
    // Каталог — из главного потока и по именам: сторож хранилища сверяет
    // состав файлов, а материализация меняет СОДЕРЖИМОЕ закрытых заметок,
    // и без этого строки списка показывали бы старые заголовки.
    if (storage_ != nullptr) {
        for (const QString& id : lastReport_.materializedIds) storage_->refreshNote(id);
        for (const QString& id : lastReport_.deletedIds) storage_->refreshNote(id);
    }
    emit stateChanged();
    // Вопрос предохранителя — окну; прогон при этом УДАЧЕН: всё прочее
    // сделано, задержаны только удаления.
    if (ok && !lastReport_.pendingDeletes.isEmpty()) emit pendingDeletes(lastReport_.pendingDeletes);
    emit finished(ok);
}

void SyncController::declareAliveAndFinish(const QStringList& ids) {
    if (storage_ == nullptr || running_) return;
    QString why;
    if (!storage_->declareAlive(ids, &why)) {
        lastError_ = why;
        emit stateChanged();
        return;
    }
    startFull(false);
}

void SyncController::pushOnExit() {
    if (running_) {
        // Фоновый прогон ещё идёт — он и есть свежая заливка; просим его
        // закончить и ждём, второй запускать поверх нельзя.
        joinWorker();
        return;
    }
    if (!configured() || storage_->dirtyIds().isEmpty()) return;
    if (!ensureConnected()) return;  // офлайн — тихий пропуск, dirty переживает
    ZStorage::SyncOptions options;
    options.mode = ZStorage::SyncOptions::PushOnly;
    ZStorage::SyncReport report;
    QString why;
    if (!storage_->sync(options, &report, &why))
        fprintf(stderr, "zametti: exit push failed: %s\n", qPrintable(why));
    lastReport_ = report;
}

QString SyncController::statusText() const {
    if (!configured())
        return QStringLiteral("Sync is not set up — run zametti-store set-remote for this store");
    if (running_) return QStringLiteral("Syncing… click to cancel");
    if (!lastError_.isEmpty()) return QStringLiteral("Sync failed: %1").arg(lastError_);
    if (lastReport_.listed > 0 || lastReport_.materialized > 0)
        return QStringLiteral("Synced: %1 in, %2 out, %3 merged")
            .arg(lastReport_.takenWhole + lastReport_.materialized)
            .arg(lastReport_.pushedWhole)
            .arg(lastReport_.mergedJournals);
    return QStringLiteral("Sync now");
}

}  // namespace zametti
