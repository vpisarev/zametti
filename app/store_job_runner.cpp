// StoreJobRunner: работы окна хранилищ. Подробности — в заголовке.

#include "store_job_runner.h"

#include "secret_store.h"

#include <QFile>

namespace zametti {

namespace {

using Outcome = StoreManagerModel::Outcome;
using CloudSeen = StoreManagerModel::CloudSeen;
using Job = StoreManagerModel::Job;

// «Коллекции ещё нет» — это ПУСТОЕ облако, а не беда: человек назвал папку,
// которой на сервере пока не существует, и её заведёт первая заливка
// (mkdirOnce). Сорт узнаётся по слову адаптера: WebDAV отвечает листингу
// HTTP 404, каталог-облако — «does not exist» (найдено живой пробой
// владельца 30.08: Check по свежему имени папки показывал голое «404»).
bool cloudAbsent(const QString& error) {
    return error.contains(QLatin1String("HTTP 404")) ||
           error.contains(QLatin1String("does not exist"));
}

// Увиденное — из разведки: сводка заполнена и при неудаче (имена открыты).
CloudSeen seenFrom(const ZStorage::CloudProbe& probe, const QString& address,
                   CloudSeen::State state) {
    CloudSeen seen;
    seen.state = state;
    seen.address = address;
    seen.notes = probe.notes;
    seen.attachments = probe.attachments;
    seen.bytes = probe.bytes;
    seen.lastModified = probe.lastModified;
    return seen;
}

}  // namespace

Outcome StoreJobRunner::run(const Job& job, SecretStore& secrets) {
    switch (job.kind) {
        case Job::Kind::Check: return check(job, secrets);
        case Job::Kind::Create: return create(job, secrets);
        case Job::Kind::ChangePassword: return changePassword(job, secrets);
        case Job::Kind::EraseAndReseed: return eraseAndReseed(job, secrets);
        case Job::Kind::EraseAndDisconnect: return eraseAndDisconnect(job, secrets);
        case Job::Kind::None: break;
    }
    return {};
}

// ПРОВЕРИТЬ — И ПОДКЛЮЧИТЬ, ЕСЛИ ЕСТЬ ЧЕМ (таблица B матрицы). Разведка —
// один листинг + чтение головы, пароль шифрования не обязателен: сводка и
// манифест открыты. Но Check не только смотрит: у ХРАНИЛИЩА против своего
// облака он разворачивает конверт и записывает адрес (connectCloud), против
// пустого — запечатывает его конвертом (initCloudStorage; беда M: подключение
// к пустому серверу обязано залить keyfile) — НО только с пропуском модели
// (sealEmpty): первый Check по неизвестному адресу лишь смотрит, чтобы
// опечатка в пароле не запечатала облако навсегда.
Outcome StoreJobRunner::check(const Job& job, SecretStore& secrets) {
    Outcome out;
    const QString address = job.cfg.cloudAddressText();
    ZStorage::CloudProbe probe;
    QString err;
    const bool ok =
        ZStorage::probeCloud(job.cfg, job.serverPassword, job.encryptionPassword,
                             &probe, &err);
    if (!ok && !cloudAbsent(err)) {
        // Сорт беды — по слову ядра: логин, пароль, связь. Строка фактов при
        // этом говорит про ОБЛАКО (сводка есть и при неверном пароле), а
        // сообщение — про событие.
        CloudSeen::State state = CloudSeen::State::NoAnswer;
        QString message = err;
        if (err.contains(QStringLiteral("refused the login"))) {
            state = CloudSeen::State::LoginRefused;
        } else if (err.contains(QStringLiteral("wrong password"))) {
            state = CloudSeen::State::WrongPassword;
            message = QStringLiteral("Wrong password.");
        }
        out.seen = seenFrom(probe, address, state);
        out.message = message;
        out.alarm = true;
        return out;
    }
    if (!ok) probe = ZStorage::CloudProbe();   // папки нет = пустое облако

    CloudSeen::State state = CloudSeen::State::Empty;
    if (probe.hasManifest && !probe.hasKeyfile) {
        state = CloudSeen::State::Incomplete;
    } else if (probe.hasManifest || probe.hasKeyfile) {
        state = CloudSeen::State::Ours;
        // Чужое узнаётся сверкой ОТКРЫТЫХ манифестов — пароль не нужен.
        // Сверять не с чем (папка пуста или без идентичности) — облако
        // считается пригодным: так выглядит бутстрап нового устройства.
        if (probe.hasManifest &&
            ZStorage::inspect(job.root) == ZStorage::DirKind::Store) {
            const QString mineId = ZStorage(job.root).identity().storeId();
            if (!mineId.isEmpty() && mineId != probe.identity.storeId())
                state = CloudSeen::State::Foreign;
        }
    }
    out.seen = seenFrom(probe, address, state);
    out.ok = true;
    switch (state) {
        case CloudSeen::State::Foreign:
            out.message = QStringLiteral("This cloud belongs to another store.");
            out.alarm = true;
            return out;
        case CloudSeen::State::Incomplete:
            out.message = QStringLiteral("The cloud has no keyfile.");
            out.alarm = true;
            return out;
        default:
            break;
    }
    out.message = QStringLiteral("Connected.");

    // --- подключение (только для хранилища; пустой папке подключать нечего) --
    if (ZStorage::inspect(job.root) != ZStorage::DirKind::Store) return out;
    ZStorage storage(job.root);
    QString why;
    if (state == CloudSeen::State::Ours && !job.encryptionPassword.isEmpty()) {
        // Конверт есть и пароль дан: развернуть, адрес — в cloud.json,
        // секреты — в копилку. Скачиваний здесь нет — их ведёт прогон.
        ZStorage::ConnectOutcome connected;
        if (!storage.connectCloud(job.cfg, job.encryptionPassword, job.serverPassword,
                                  secrets, mintParams_, &connected, &why)) {
            out.ok = false;
            out.alarm = true;
            out.message = why.contains(QStringLiteral("wrong password"))
                              ? QStringLiteral("Wrong password.")
                              : why;
            if (why.contains(QStringLiteral("wrong password")))
                out.seen.state = CloudSeen::State::WrongPassword;
            return out;
        }
    } else if (state == CloudSeen::State::Ours) {
        // Пароля в поле нет — но ключ мог лежать в связке (окно подсадило его
        // в копилку): подключение без вопросов, как на старте программы.
        ZStorage::AttachOptions how;
        how.cfg = job.cfg;
        how.serverPassword = job.serverPassword;
        if (storage.attachCloud(how, secrets, nullptr, &why))
            storage.writeCloudConfig(job.cfg, nullptr);
        // Не вышло — не беда: разведка честно удалась, подключит пароль.
    } else if (state == CloudSeen::State::Empty && job.sealEmpty &&
               !job.encryptionPassword.isEmpty()) {
        // ЗАПЕЧАТАТЬ ПУСТОЕ ОБЛАКО — по пропуску модели (повтор пройден).
        if (!storage.initCloudStorage(job.cfg, job.encryptionPassword,
                                      job.serverPassword, secrets, mintParams_, &why)) {
            out.ok = false;
            out.alarm = true;
            out.message = why;
            return out;
        }
        // Конверт уехал: облако больше не пустое, и заливку данных поведёт
        // фоновый прогон после Open.
        out.seen.state = CloudSeen::State::Ours;
        out.message = QStringLiteral("Connected. Uploading in the background.");
    }
    return out;
}

// СОЗДАНИЕ в пустой (или несуществующей) папке. Облако с манифестом — бутстрап
// ГОЛОВЫ (идентичность + корень), скачивание ведёт фоновый прогон после Open;
// пустое или неназванное облако — свежее хранилище (+ засев конверта).
Outcome StoreJobRunner::create(const Job& job, SecretStore& secrets) {
    Outcome out;
    QString err;
    const QString address = job.cfg.cloudAddressText();

    if (!job.cfg.hasCloudAddress()) {
        if (!ZStorage(job.root).init(&err)) {
            out.message = err;
            out.alarm = true;
            return out;
        }
        out.ok = true;
        out.message = QStringLiteral("Created.");
        return out;
    }

    if (job.encryptionPassword.isEmpty()) {
        // Обе дороги ниже открывают или чеканят конверт — без пароля нечем.
        out.message = QStringLiteral("Enter the encryption password.");
        return out;
    }
    // Что там, решает разведка: манифест — бутстрап, пусто — первое
    // устройство. Пароль отдаётся ЕЙ ЖЕ: неверный обязан быть отвергнут ДО
    // первой записи — отказ не оставляет ни каркаса, ни огрызков (цена —
    // второй разворот конверта внутри initFromCloud, доли секунды).
    ZStorage::CloudProbe probe;
    if (!ZStorage::probeCloud(job.cfg, job.serverPassword, job.encryptionPassword,
                              &probe, &err)) {
        if (!cloudAbsent(err)) {
            out.message = err.contains(QStringLiteral("wrong password"))
                              ? QStringLiteral("Wrong password.")
                              : err;
            out.alarm = true;
            return out;
        }
        probe = ZStorage::CloudProbe();   // папки нет = пустое облако, заведём
    }
    if (probe.hasManifest || probe.hasKeyfile) {
        // БУТСТРАП, ГОЛОВОЙ ВПЕРЁД: манифест, конверт, корневая заметка. Ни
        // одного блоба содержимого здесь не качается (П6) — привезёт прогон.
        ZStorage::ConnectOutcome boot;
        auto storage = ZStorage::initFromCloud(job.root, job.cfg, job.encryptionPassword,
                                               job.serverPassword, secrets, mintParams_,
                                               &boot, &err);
        if (storage == nullptr) {
            out.message = err;
            out.alarm = true;
            return out;
        }
        out.ok = true;
        out.downloadedNew = true;
        out.seen = seenFrom(probe, address, CloudSeen::State::Ours);
        out.message = QStringLiteral("Connected.");
        return out;
    }
    // Первое устройство: свежее хранилище + конверт в пустое облако. Манифест
    // не заливается — его несёт заливка данных (фоновый прогон, последним).
    if (!ZStorage(job.root).init(&err)) {
        out.message = err;
        out.alarm = true;
        return out;
    }
    ZStorage storage(job.root);
    if (!storage.initCloudStorage(job.cfg, job.encryptionPassword, job.serverPassword,
                                  secrets, mintParams_, &err)) {
        out.message = err;
        out.alarm = true;
        return out;
    }
    out.ok = true;
    out.seen = seenFrom(probe, address, CloudSeen::State::Empty);
    out.message = QStringLiteral("Created. Uploading in the background.");
    return out;
}

// СМЕНА ПАРОЛЯ при живом ключе: один конверт, ноль стираний. Ключ окно
// подсадило в копилку до запуска — из рабочего потока настоящую связку не
// спросить.
Outcome StoreJobRunner::changePassword(const Job& job, SecretStore& secrets) {
    Outcome out;
    QString err;
    ZStorage storage(job.root);
    if (!storage.changeEncryptionPassword(job.cfg, job.encryptionPassword,
                                          job.serverPassword, secrets, mintParams_,
                                          &err)) {
        out.message = err;
        out.alarm = true;
        return out;
    }
    out.ok = true;
    out.message = QStringLiteral("Password changed.");
    return out;
}

// СТЕРЕТЬ И ЗАПЕЧАТАТЬ ЗАНОВО: пара запросов на стирание (DELETE+MKCOL), один
// PUT конверта; перезаливку содержимого ведёт фоновый прогон после закрытия.
Outcome StoreJobRunner::eraseAndReseed(const Job& job, SecretStore& secrets) {
    Outcome out;
    QString err;
    ZStorage storage(job.root);
    ZStorage::EraseOutcome erased;
    if (!storage.eraseCloudStorage(job.cfg, job.serverPassword, /*keepFolder=*/true,
                                   &erased, &err)) {
        out.message = err;
        out.alarm = true;
        return out;
    }
    if (!storage.initCloudStorage(job.cfg, job.encryptionPassword, job.serverPassword,
                                  secrets, mintParams_, &err)) {
        out.message = err;
        out.alarm = true;
        return out;
    }
    out.ok = true;
    CloudSeen seen;
    seen.state = CloudSeen::State::Ours;
    seen.address = job.cfg.cloudAddressText();
    out.seen = seen;
    out.message = QStringLiteral("Erased. Uploading in the background.");
    return out;
}

// СТЕРЕТЬ И ОТВЯЗАТЬСЯ: облако снесено целиком, адрес и три секрета забыты.
Outcome StoreJobRunner::eraseAndDisconnect(const Job& job, SecretStore& secrets) {
    Outcome out;
    QString err;
    ZStorage storage(job.root);
    const QString storeId = storage.identity().storeId();
    if (!storage.eraseCloudStorage(job.cfg, job.serverPassword, /*keepFolder=*/false,
                                   nullptr, &err)) {
        out.message = err;
        out.alarm = true;
        return out;
    }
    if (!storage.clearCloudConfig(&err)) {
        out.message = err;
        out.alarm = true;
        return out;
    }
    // Секреты отвязанного облака забываются все три: копилка окна перенесёт
    // просьбу в настоящую связку.
    if (!storeId.isEmpty()) {
        secrets.clearKey(storeId, nullptr);
        secrets.clearServerPassword(storeId, nullptr);
        secrets.clearEncryptionPassword(storeId, nullptr);
    }
    out.ok = true;
    out.message = QStringLiteral("Disconnected.");
    return out;
}

}  // namespace zametti
