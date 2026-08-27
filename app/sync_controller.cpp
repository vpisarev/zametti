#include "sync_controller.h"

#include "keyring_secrets.h"
#include "settings.h"
#include "zlogs.h"

#include <QMetaObject>

namespace zametti {

SyncController::SyncController(std::shared_ptr<ZStorage> storage,
                               std::shared_ptr<SecretStore> secrets, ZLogs* logs,
                               QObject* parent)
    : QObject(parent),
      storage_(std::move(storage)),
      secrets_(secrets ? std::move(secrets) : std::make_shared<KeyringSecrets>()),
      logs_(logs) {
    // Повтор при пропавшей сети: одноразовый таймер, взводится итогом
    // прогона с сетевыми бедами и гасится любым ручным вмешательством.
    retry_.setSingleShot(true);
    retry_.setInterval(kRetrySec * 1000);
    connect(&retry_, &QTimer::timeout, this, [this] {
        if (!running_ && configured()) startFull(false);
    });
    // Такт индикатора. Живёт в главном потоке; движок только пишет атомики.
    ticker_.setInterval(120);
    connect(&ticker_, &QTimer::timeout, this, [this] {
        ++tick_;
        emit progress(progressLine(tick_, progressDone_ ? progressDone_->load() : 0,
                                   progressTotal_ ? progressTotal_->load() : 0,
                                   progressPhase_ != nullptr && progressPhase_->load() != 0));
    });
}

void SyncController::setStorage(std::shared_ptr<ZStorage> storage) {
    // ЖДЁМ ПОТОК ПРОГОНА, А НЕ ПРОСТО ПРОСИМ ЕГО ОСТАНОВИТЬСЯ. Он держит СВОЮ
    // копию shared_ptr на хранилище; пока он жив, прежнее хранилище не умрёт,
    // а значит не отпустит замок — и новое открытие того же каталога упрётся в
    // «уже открыто другой копией zametti», указывающее на нас самих. Ровно та
    // же причина, по которой ждёт деструктор.
    if (cancel_) cancel_->store(true);
    joinWorker();
    running_ = false;
    ticker_.stop();
    retry_.stop();
    storage_ = std::move(storage);
    // Секреты и адрес добыты для ПРЕЖНЕГО хранилища: у нового и storeId другой,
    // и облако может быть другое. Забываем всё — fetchSecrets достанет заново.
    cfg_ = ZStorage::Config{};
    keyfile_ = Keyfile{};
    serverPassword_.clear();
    lastReport_ = ZStorage::SyncReport{};
    lastError_.clear();
    emit stateChanged();
}

int SyncController::bounceAt(int tick, int width) {
    // Путь туда-обратно без задержки на краях: период 2*(width-1).
    const int period = 2 * (width - 1);
    const int at = tick % period;
    return at < width ? at : period - at;
}

QString SyncController::progressLine(int tick, int done, int total, bool materializing) {
    QString bar(kBounceWidth, QLatin1Char('.'));
    bar[bounceAt(tick)] = QLatin1Char('*');
    // Счётчик дополняется слева до ширины итога: «]» стоит на месте, пока
    // число растёт. Итог неизвестен — честный «0/?».
    const QString right = total > 0
        ? QStringLiteral("%1/%2").arg(done, QString::number(total).size()).arg(total)
        : QStringLiteral("0/?");
    // Слово фазы: после обмена идёт материализация, и человек должен видеть,
    // ЧЕМ программа занята, а не гадать по замершему счётчику.
    return QStringLiteral("%1: [%2  %3]")
        .arg(materializing ? QStringLiteral("materializing") : QStringLiteral("cloud sync"),
             bar, right);
}

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
    return storage_ != nullptr && storage_->isStore() && storage_->remoteConfig().hasCloud();
}

bool SyncController::fetchSecrets() {
    if (storage_ == nullptr) return false;
    cfg_ = storage_->remoteConfig();
    if (!cfg_.hasCloud()) {
        lastError_ = QStringLiteral("sync is not configured for this store");
        return false;
    }
    const ZStorage::Identity mine = storage_->identity();
    if (mine.isEmpty()) {
        lastError_ = QStringLiteral("the store has no identity");
        return false;
    }
    QString why;
    if (!cfg_.remoteUrl.isEmpty()) serverPassword_ = secrets_->serverPassword(mine.storeId(), &why);
    if (!secrets_->loadKey(mine.storeId(), &keyfile_, &why)) {
        lastError_ =
            QStringLiteral("the key is not in the keyring (%1) — run set-remote once").arg(why);
        return false;
    }
    return true;
}

void SyncController::toggle() {
    if (running_)
        cancel();
    else
        startFull(false);
}

void SyncController::cancel() {
    // Остановить — значит остановить: и прогон, и запланированный повтор.
    retry_.stop();
    if (running_ && cancel_) cancel_->store(true);
}

void SyncController::startFull(bool allowMassDelete) {
    if (running_ || !configured()) return;
    retry_.stop();   // идём сейчас — ждать больше нечего
    if (!fetchSecrets()) {
        emit stateChanged();
        emit finished(false);
        return;
    }
    joinWorker();
    ZStorage::SyncOptions options;
    options.mode = ZStorage::SyncOptions::Full;
    options.allowMassDelete = allowMassDelete;
    options.logs = logs_;
    cancel_ = std::make_shared<std::atomic<bool>>(false);
    options.cancel = cancel_;
    progressDone_ = std::make_shared<std::atomic<int>>(0);
    progressTotal_ = std::make_shared<std::atomic<int>>(0);
    progressPhase_ = std::make_shared<std::atomic<int>>(0);
    options.progressDone = progressDone_;
    options.progressTotal = progressTotal_;
    options.progressPhase = progressPhase_;
    running_ = true;
    lastError_.clear();
    tick_ = 0;
    ticker_.start();
    emit stateChanged();
    run(options);
}

void SyncController::run(ZStorage::SyncOptions options) {
    worker_ = std::thread([this, options] {
        ZStorage::SyncReport report;
        QString error;
        // Адаптер рождается ЗДЕСЬ, в потоке прогона: QNetworkAccessManager
        // однопоточен. Подключение повторяется каждым прогоном — прежний
        // адаптер привязан к уже умершему потоку.
        bool ok = false;
        auto remote = ZStorage::makeRemote(cfg_, serverPassword_, &error);
        if (remote != nullptr && storage_->setRemote(remote, keyfile_, &error))
            ok = storage_->sync(options, &report, &error);
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
    ticker_.stop();
    emit progress(QString());  // индикатор убрать
    lastError_ = ok ? QString() : error;
    // Сетевые беды повторяются сами, пока окно живо: отложенные блобы или
    // недостучавшееся подключение (у него в отчёте нули). Отменённый рукой
    // прогон и провалы целостности повторов не заводят.
    const bool networkTrouble =
        !lastReport_.cancelled &&
        ((ok && lastReport_.deferred > 0) ||
         (!ok && lastReport_.integrityFailures == 0));
    if (networkTrouble)
        retry_.start();
    else
        retry_.stop();
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
    // Выход — синхронный и в главном потоке: адаптер рождается здесь же.
    if (!fetchSecrets()) return;  // keyring пуст — тихий пропуск, dirty переживает
    QString why;
    auto remote = ZStorage::makeRemote(cfg_, serverPassword_, &why);
    if (remote == nullptr || !storage_->setRemote(remote, keyfile_, &why)) {
        fprintf(stderr, "zametti: exit push cannot connect: %s\n", qPrintable(why));
        return;  // офлайн — тихий пропуск, dirty переживает
    }
    ZStorage::SyncOptions options;
    options.mode = ZStorage::SyncOptions::PushOnly;
    options.logs = logs_;
    ZStorage::SyncReport report;
    if (!storage_->sync(options, &report, &why))
        fprintf(stderr, "zametti: exit push failed: %s\n", qPrintable(why));
    lastReport_ = report;
}

QString SyncController::statusText() const {
    if (!configured())
        return QStringLiteral(
            "Sync is not set up — open the storage dialog (the database button)");
    if (running_) return QStringLiteral("Syncing… click to cancel");
    // Слово о запланированном повторе — человек должен знать, что программа
    // не сдалась, а ждёт сеть.
    const QString retryNote =
        retry_.isActive() ? QStringLiteral(" — will retry in a couple of minutes") : QString();
    if (!lastError_.isEmpty())
        return QStringLiteral("Sync failed: %1%2").arg(lastError_, retryNote);
    if (lastReport_.deferred > 0)
        return QStringLiteral("Synced with %1 skipped%2")
            .arg(lastReport_.deferred)
            .arg(retryNote);
    if (lastReport_.listed > 0 || lastReport_.materialized > 0)
        return QStringLiteral("Synced: %1 in, %2 out, %3 merged")
            .arg(lastReport_.takenWhole + lastReport_.materialized)
            .arg(lastReport_.pushedWhole)
            .arg(lastReport_.mergedJournals);
    return QStringLiteral("Sync now");
}

}  // namespace zametti
