// Глаголы облака: подключение и заливка (m17, сессия 3).
//
// ЕДИНСТВЕННОЕ МЕСТО, где файлы хранилища встречаются с именами блобов и с
// AAD. Адаптер (RemoteStore) возит байты и о заметках не знает; шифр
// (BlobCipher) знает только про AAD; хранилище знает файлы. Здесь они
// сходятся, и больше нигде — иначе «как зовётся блоб этой картинки» имело бы
// два ответа.
//
// Раскладка в облаке плоская и открытая (id непрозрачны):
//
//   zametti.json    манифест, ОТКРЫТЫМ ТЕКСТОМ: по нему сверяется storeId
//                   ДО ввода пароля и до расшифровки хоть чего-нибудь;
//   keyfile         конверт мастер-ключа (тоже открыто, см. справочник);
//   <id>.log        журнал заметки, зашифрован;
//   <id>.<ext>      вложение, зашифровано.

#include "zstorage.h"

#include "folder_remote.h"
#include "keyfile.h"
#include "sync_ledger.h"
#include "note_id.h"
#include "secret_store.h"
#include "times.h"
#include "webdav_remote.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutexLocker>
#include <QSaveFile>
#include <QSet>
#include <QUrl>

#include <algorithm>

namespace zametti {
namespace {

// Журналы и вложения различаются по имени, и это различие подписано тегом
// (AAD): подсунуть вложение под именем журнала не выйдет.
BlobKind kindOfBlob(const QString& name) {
    return name.endsWith(QStringLiteral(".log")) ? BlobKind::Journal
                                                 : BlobKind::Attachment;
}

}  // namespace

// --- DIRTY-SET -------------------------------------------------------------
//
// Файл `.zametti/dirty` — по имени в строке, дозаписью, БЕЗ fsync (как
// журнал: дробить пометку на коммиты файловой системы — порча носителя ради
// кэша). Пометка — подсказка, не истина: лишняя стоит одной проверки хеша,
// потерянную находит stat-скан после нештатного завершения.

QString ZStorage::dirtyPath() const {
    return root_ + QStringLiteral("/.zametti/dirty");
}

void ZStorage::loadDirtyLocked() const {
    if (dirtyLoaded_) return;
    dirtyLoaded_ = true;
    QFile f(dirtyPath());
    if (!f.open(QIODevice::ReadOnly)) return;
    for (const QByteArray& line : f.readAll().split('\n')) {
        const QString name = QString::fromUtf8(line).trimmed();
        if (!name.isEmpty()) dirty_.insert(name);
    }
}

void ZStorage::markDirty(const QString& noteId) {
    if (noteId.isEmpty()) return;
    const QMutexLocker locked(&dirtyGate_);
    loadDirtyLocked();
    if (dirty_.contains(noteId)) return;
    dirty_.insert(noteId);
    QDir().mkpath(root_ + QStringLiteral("/.zametti"));
    QFile f(dirtyPath());
    if (f.open(QIODevice::WriteOnly | QIODevice::Append))
        f.write(noteId.toUtf8() + '\n');
    else
        fprintf(stderr, "zametti: cannot mark %s dirty: %s\n", qPrintable(noteId),
                qPrintable(f.errorString()));
}

QStringList ZStorage::dirtyIds() const {
    const QMutexLocker locked(&dirtyGate_);
    loadDirtyLocked();
    QStringList out(dirty_.begin(), dirty_.end());
    std::sort(out.begin(), out.end());
    return out;
}

void ZStorage::clearDirty(const QStringList& synced) {
    const QMutexLocker locked(&dirtyGate_);
    loadDirtyLocked();
    bool changed = false;
    for (const QString& name : synced) changed = dirty_.remove(name) || changed;
    if (!changed) return;
    QStringList rest(dirty_.begin(), dirty_.end());
    std::sort(rest.begin(), rest.end());
    // Пересборка файла — атомарно: обрезанный на половине список пометок
    // выглядел бы как «всё чисто» для имён из отрезанной части.
    QSaveFile save(dirtyPath());
    if (!save.open(QIODevice::WriteOnly)) return;
    for (const QString& name : rest) save.write(name.toUtf8() + '\n');
    save.commit();
}

// --- АДРЕС ОБЛАКА И ПОДКЛЮЧЕНИЕ --------------------------------------------

bool ZStorage::RemoteConfig::parse(const QByteArray& bytes, QString* error) {
    QJsonParseError bad;
    const QJsonDocument doc = QJsonDocument::fromJson(bytes, &bad);
    if (bad.error != QJsonParseError::NoError || !doc.isObject()) {
        if (error) *error = QStringLiteral("remote.json is not a JSON object");
        return false;
    }
    const QJsonObject o = doc.object();
    url = o.value(QStringLiteral("url")).toString();
    dir = o.value(QStringLiteral("dir")).toString();
    user = o.value(QStringLiteral("user")).toString();
    allowInsecureHttp = o.value(QStringLiteral("allowInsecureHttp")).toBool(false);
    timeoutMs = o.value(QStringLiteral("timeoutMs")).toInt(30000);
    return true;
}

QByteArray ZStorage::RemoteConfig::toBytes() const {
    QJsonObject o;
    if (!url.isEmpty()) o.insert(QStringLiteral("url"), url);
    if (!dir.isEmpty()) o.insert(QStringLiteral("dir"), dir);
    if (!user.isEmpty()) o.insert(QStringLiteral("user"), user);
    if (allowInsecureHttp) o.insert(QStringLiteral("allowInsecureHttp"), true);
    o.insert(QStringLiteral("timeoutMs"), timeoutMs);
    return QJsonDocument(o).toJson(QJsonDocument::Indented);
}

ZStorage::RemoteConfig ZStorage::remoteConfig() const {
    RemoteConfig cfg;
    QFile f(root_ + QStringLiteral("/.zametti/remote.json"));
    if (!f.open(QIODevice::ReadOnly)) return cfg;
    QString why;
    if (!cfg.parse(f.readAll(), &why)) {
        // Битый конфиг ничего не подключает — как битый config.json ничего
        // не перезагружает. Пустой конфиг = «не настроен», и это видно.
        fprintf(stderr, "zametti: %s: %s\n", qPrintable(f.fileName()), qPrintable(why));
        return RemoteConfig();
    }
    return cfg;
}

bool ZStorage::writeRemoteConfig(const RemoteConfig& cfg, QString* error) {
    QDir().mkpath(root_ + QStringLiteral("/.zametti"));
    QSaveFile save(root_ + QStringLiteral("/.zametti/remote.json"));
    if (!save.open(QIODevice::WriteOnly)) {
        if (error) *error = QStringLiteral("cannot write remote.json: %1").arg(save.errorString());
        return false;
    }
    save.write(cfg.toBytes());
    if (!save.commit()) {
        if (error) *error = QStringLiteral("cannot write remote.json: %1").arg(save.errorString());
        return false;
    }
    return true;
}

bool ZStorage::clearRemoteConfig(QString* error) {
    QFile f(root_ + QStringLiteral("/.zametti/remote.json"));
    if (!f.exists()) return true;
    if (f.remove()) return true;
    if (error) *error = QStringLiteral("cannot remove remote.json: %1").arg(f.errorString());
    return false;
}

std::shared_ptr<RemoteStore> ZStorage::makeRemote(const RemoteConfig& cfg,
                                                  const QString& serverPassword,
                                                  QString* error) const {
    if (cfg.isEmpty()) {
        if (error) *error = QStringLiteral("sync is not configured");
        return nullptr;
    }
    if (!cfg.url.isEmpty()) {
        WebDavRemote::Config web;
        web.base = QUrl(cfg.url);
        web.user = cfg.user;
        web.password = serverPassword;
        web.allowInsecureHttp = cfg.allowInsecureHttp;
        web.timeoutMs = cfg.timeoutMs;
        // Адрес проверяется ДО первой операции: пароль не уедет открытым
        // текстом даже один раз.
        if (!WebDavRemote::checkUrl(web, error)) return nullptr;
        return std::make_shared<WebDavRemote>(web);
    }
    return std::make_shared<FolderRemote>(cfg.dir);
}

bool ZStorage::useLastRemote(SecretStore& secrets, QString* error) {
    const RemoteConfig cfg = remoteConfig();
    if (cfg.isEmpty()) {
        if (error) *error = QStringLiteral("sync is not configured for this store");
        return false;
    }
    const Identity mine = identity(error);
    if (mine.isEmpty()) {
        if (error && error->isEmpty()) *error = QStringLiteral("the store has no identity");
        return false;
    }
    QString why;
    QString password;
    if (!cfg.url.isEmpty()) password = secrets.serverPassword(mine.storeId(), &why);
    auto remote = makeRemote(cfg, password, error);
    if (!remote) return false;
    Keyfile keyfile;
    if (!secrets.loadKey(mine.storeId(), &keyfile, &why)) {
        // Не беда, а «настройся заново»: утраченный keyring лечится одним
        // вводом пароля (set-remote), и это названный брифом случай.
        if (error)
            *error = QStringLiteral("the key is not in the keyring (%1) — run set-remote once")
                         .arg(why);
        return false;
    }
    return setRemote(remote, keyfile, error);
}

bool ZStorage::connectRemote(const RemoteConfig& cfg, const QString& encryptionPassword,
                             const QString& serverPassword, SecretStore& secrets,
                             const Keyfile::KdfParams& mintParams, ConnectOutcome* outcome,
                             QString* error) {
    ConnectOutcome done;
    const auto finish = [&](bool ok) {
        if (outcome != nullptr) *outcome = done;
        return ok;
    };
    if (!store_) {
        if (error) *error = QStringLiteral("not a store: %1").arg(root_);
        return finish(false);
    }
    if (encryptionPassword.isEmpty()) {
        if (error) *error = QStringLiteral("the encryption password must not be empty");
        return finish(false);
    }
    auto remote = makeRemote(cfg, serverPassword, error);
    if (!remote) return finish(false);

    // МАНИФЕСТ — ДО идентичности: бутстрап наследует id из облака, а не
    // чеканит свой (иначе два id на одно хранилище и молчаливый развод).
    QByteArray manifestBytes;
    QString why;
    Identity theirs;
    const bool haveManifest =
        remote->get(QLatin1String(Identity::kFile), &manifestBytes, nullptr, &why);
    if (haveManifest) {
        if (!theirs.parse(manifestBytes, &why)) {
            if (error) *error = QStringLiteral("the cloud manifest is unreadable: %1").arg(why);
            return finish(false);
        }
        if (theirs.tooNew()) {
            if (error)
                *error = QStringLiteral(
                    "the cloud was written by a newer version of the program — update this one");
            return finish(false);
        }
    }

    if (!loaded_) reload();
    Identity mine = identity(&why);
    if (mine.isEmpty()) {
        if (haveManifest && ids().isEmpty()) {
            // БУТСТРАП: пустое хранилище наследует идентичность облака;
            // дальнейший sync скачает всё — отдельного кода восстановления
            // не существует.
            if (!writeIdentity(theirs, error)) return finish(false);
            done.inheritedIdentity = true;
            mine = theirs;
        } else if (haveManifest) {
            if (error)
                *error = QStringLiteral(
                    "this store has notes but no identity, and the cloud belongs to "
                    "store %1 — bootstrap into an empty folder instead")
                             .arg(theirs.storeId());
            return finish(false);
        } else {
            mine = ensureIdentity(error);
            if (mine.isEmpty()) return finish(false);
        }
    }
    if (haveManifest && theirs.storeId() != mine.storeId()) {
        // Честная остановка ДО единой записи — включая keyfile.
        if (error)
            *error = QStringLiteral(
                "this cloud folder belongs to another store (%1), and this store is %2")
                         .arg(theirs.storeId(), mine.storeId());
        return finish(false);
    }

    // KEYFILE: есть — развернуть паролем; нет — отчеканить и залить.
    Keyfile keyfile;
    QByteArray keyfileBytes;
    if (remote->get(QLatin1String(Keyfile::kRemoteName), &keyfileBytes, nullptr, &why)) {
        if (!keyfile.parse(keyfileBytes, error)) return finish(false);
        if (keyfile.tooNew()) {
            if (error)
                *error = QStringLiteral(
                    "the keyfile was written by a newer version of the program — update this one");
            return finish(false);
        }
        if (keyfile.storeId() != mine.storeId()) {
            if (error)
                *error = QStringLiteral("the cloud keyfile belongs to store %1, not %2")
                             .arg(keyfile.storeId(), mine.storeId());
            return finish(false);
        }
        if (!keyfile.unwrap(encryptionPassword, &why)) {
            // По построению AEAD «неверный пароль» и «испорченный keyfile»
            // неразличимы — так и говорим.
            if (error)
                *error = QStringLiteral("wrong password, or the keyfile is corrupted: %1").arg(why);
            return finish(false);
        }
    } else {
        if (!Keyfile::create(mine.storeId(), encryptionPassword, mintParams, &keyfile, error))
            return finish(false);
        if (!remote->mkdirOnce(error)) return finish(false);
        if (!remote->put(QLatin1String(Keyfile::kRemoteName), keyfile.toBytes(), nullptr, error))
            return finish(false);
        done.mintedKeyfile = true;
    }

    if (!setRemote(remote, keyfile, error)) return finish(false);

    // ЗАПОМНИТЬ. Отказ keyring подключение не валит: программа работает,
    // просто следующий старт снова спросит пароль — и скажет об этом.
    QString keep;
    if (!secrets.storeKey(keyfile, &keep))
        fprintf(stderr, "zametti: the keyring refused the key: %s\n", qPrintable(keep));
    if (!cfg.url.isEmpty() && !serverPassword.isEmpty() &&
        !secrets.setServerPassword(mine.storeId(), serverPassword, &keep))
        fprintf(stderr, "zametti: the keyring refused the server password: %s\n",
                qPrintable(keep));
    if (!writeRemoteConfig(cfg, error)) return finish(false);
    return finish(true);
}

// --- ДВИЖОК СИНХРОНИЗАЦИИ ---------------------------------------------------
//
// Порядок шагов — инвариант: выравнивание → обмен → объединение →
// материализация. Движок трогает только файлы и журналы; каталог заметок
// (notes_) не читается и не правится — списки берутся с диска, окно обновляют
// его сторожа. Так один и тот же код безопасен и в CLI, и в фоновом потоке
// приложения.

namespace {

Digest hashBytes(const QByteArray& bytes) {
    return hashOf(std::string_view(bytes.constData(), size_t(bytes.size())));
}

}  // namespace

bool ZStorage::declareAlive(const QStringList& noteIds, QString* error) {
    for (const QString& id : noteIds) {
        std::string raw;
        if (!readFileBytes(pathOf(id), raw)) {
            if (error) *error = QStringLiteral("note %1 has no file to declare alive").arg(id);
            return false;
        }
        // ПОВЕРХ надгробия, минуя отбор: planStep счёл бы возврат к уже
        // записанному состоянию дубликатом и не записал бы ничего — а здесь
        // «заметка жива» и есть новое событие, его ревизия бьёт удаление.
        // Схлопывание через надгробие не прыгает, так что запись не вычистится.
        if (!appendToJournal(id, ZJournal::NewRecord::save(QByteArray::fromStdString(raw)),
                             error))
            return false;
        markDirty(id);
    }
    return true;
}

bool ZStorage::sync(const SyncOptions& options, SyncReport* report, QString* error) {
    SyncReport done;
    const auto finish = [&](bool ok) {
        if (report != nullptr) *report = done;
        return ok;
    };
    if (!hasRemote()) {
        if (error != nullptr)
            *error = QStringLiteral("no cloud is connected — call setRemote first");
        return finish(false);
    }
    const Identity mine = ensureIdentity(error);
    if (mine.isEmpty()) return finish(false);
    const QString storeId = mine.storeId();

    const RemoteStore::Traffic t0 = remote_->traffic();
    const auto takeTraffic = [&] {
        const RemoteStore::Traffic t1 = remote_->traffic();
        done.traffic.requests = t1.requests - t0.requests;
        done.traffic.bytesUp = t1.bytesUp - t0.bytesUp;
        done.traffic.bytesDown = t1.bytesDown - t0.bytesDown;
    };
    const auto cancelled = [&] {
        return options.cancel != nullptr && options.cancel->load();
    };
    const auto complain = [&](const QString& what) {
        ++done.integrityFailures;
        fprintf(stderr, "zametti sync: %s\n", qPrintable(what));
    };

    SyncLedger ledger = SyncLedger::load(SyncLedger::pathFor(storeId, root_));
    const bool afterCrash = !ledger.cleanShutdown();
    // Прогон начался: до чистого конца бухгалтерия числится «нечистой» — упали
    // посреди, и следующий прогон сделает stat-скан на любой платформе.
    ledger.setCleanShutdown(false);
    ledger.save(nullptr);

    // Списки — С ДИСКА, не из notes_: движку можно жить в фоновом потоке.
    const auto noteIdsOnDisk = [&] {
        QStringList out;
        for (const QString& name :
             QDir(root_).entryList({QStringLiteral("*.md")}, QDir::Files)) {
            const QString stem = name.left(name.size() - 3);
            if (isValidNoteId(stem.toStdString())) out.append(stem);
        }
        out.sort();
        return out;
    };
    const auto journalIdsOnDisk = [&] {
        QStringList out;
        for (const QString& name : QDir(root_ + QStringLiteral("/history"))
                                       .entryList({QStringLiteral("*.log")}, QDir::Files)) {
            const QString stem = name.left(name.size() - 4);
            if (isValidNoteId(stem.toStdString())) out.append(stem);
        }
        out.sort();
        return out;
    };

    QElapsedTimer phase;
    phase.start();

    // ===== ШАГ 1: ВЫРАВНИВАНИЕ — O(изменений) ==============================
    //
    // «Голова ≡ файлу» держится транзакционно единственным путём записи;
    // здесь проверяются только названные: dirty-set плюс stat-скан (десктоп
    // всегда; после нештатного завершения — на любой платформе: он закрывает
    // окно write-ahead-пометки без fsync).
    QSet<QString> toCheck;
    for (const QString& name : dirtyIds())
        if (!name.contains(QLatin1Char('.'))) toCheck.insert(name);
    const QStringList notesOnDisk = noteIdsOnDisk();
    if (options.statScan || afterCrash) {
        for (const QString& id : notesOnDisk) {
            const QFileInfo info(pathOf(id));
            const SyncLedger::Stat seen = ledger.fileStat(id);
            if (seen.isEmpty() || seen.mtimeMs != info.lastModified().toMSecsSinceEpoch() ||
                seen.size != info.size()) {
                if (!toCheck.contains(id)) ++done.statScanned;
                toCheck.insert(id);
            }
        }
        // Diff против бухгалтерии ловит УДАЛЕНИЯ: пропавший при живой голове
        // журнала файл вернётся материализацией.
        for (const QString& id : ledger.knownFiles())
            if (!QFile::exists(pathOf(id))) toCheck.insert(id);
    }

    // Какими байты файла видел шаг 1 — двойное условие материализации.
    QHash<QString, Digest> alignedFile;
    {
        QStringList ordered(toCheck.begin(), toCheck.end());
        ordered.sort();
        for (const QString& id : ordered) {
            if (cancelled()) break;
            ++done.dirtyChecked;
            std::string raw;
            if (!readFileBytes(pathOf(id), raw)) continue;  // файла нет — решит материализация
            const QByteArray bytes = QByteArray::fromStdString(raw);
            const Digest fileHash = hashBytes(bytes);
            alignedFile.insert(id, fileHash);

            ZJournal frames;
            QString why;
            if (!readJournal(id, &frames, &why)) {
                complain(QStringLiteral("journal of %1 unreadable at align: %2").arg(id, why));
                continue;
            }
            if (frames.isEmpty()) {
                // Журнала нет — родить с опорной записью ВРЕМЕНЕМ ФАЙЛА.
                journalFor(id, options.journalRules)
                    ->ensureBaseline(bytes, QFileInfo(pathOf(id)).lastModified().toMSecsSinceEpoch());
                ++done.baselined;
                continue;
            }
            const int head = frames.headIndex();
            if (head >= 0 && frames.at(head).hasSnapshot() &&
                frames.at(head).digest() == fileHash)
                continue;  // выровнено
            // Файл разошёлся с головой (правка снаружи, оборванное сохранение,
            // файл поверх надгробия) — дописать external: правка побеждает.
            if (!journalFor(id, options.journalRules)
                     ->record(ZJournal::Kind::External, bytes, &why)) {
                complain(QStringLiteral("cannot record external edit of %1: %2").arg(id, why));
                continue;
            }
            ++done.externalRecorded;
        }
    }
    done.usAlign = phase.nsecsElapsed() / 1000;
    phase.restart();

    // ===== ШАГ 2 И 3: ОБМЕН И ОБЪЕДИНЕНИЕ ==================================
    if (!remote_->mkdirOnce(error)) return finish(false);
    QVector<RemoteStore::Entry> listing;
    if (!remote_->list(&listing, error)) {
        takeTraffic();
        return finish(false);
    }
    QHash<QString, QString> remoteEtag;
    for (const RemoteStore::Entry& e : listing) remoteEtag.insert(e.name, e.etag);
    done.listed = int(listing.size());

    // Манифест — по etag ИЗ ТОГО ЖЕ листинга; GET только при расхождении.
    // Страж от переадресации МЕЖДУ прогонами: remoteDir мог смениться.
    {
        const QString name = QLatin1String(Identity::kFile);
        const QString etag = remoteEtag.value(name);
        SyncLedger::Blob led = ledger.blob(name);
        if (!etag.isEmpty() && etag != led.etag) {
            QByteArray bytes;
            QString why;
            if (remote_->get(name, &bytes, nullptr, &why)) {
                Identity theirs;
                if (theirs.parse(bytes, &why)) {
                    if (theirs.storeId() != storeId) {
                        if (error != nullptr)
                            *error = QStringLiteral(
                                "this cloud folder now belongs to another store (%1) — refusing")
                                         .arg(theirs.storeId());
                        takeTraffic();
                        return finish(false);
                    }
                    if (theirs.tooNew()) {
                        if (error != nullptr)
                            *error = QStringLiteral(
                                "the cloud was written by a newer version — update the program");
                        takeTraffic();
                        return finish(false);
                    }
                }
                led.etag = etag;
                led.sealedHash = hashBytes(bytes);
                ledger.setBlob(name, led);
            }
        }
    }
    // Keyfile-чек — дешёвый ранний сигнал ротации; сама ротация распознаётся
    // массовым AEAD-отказом входящих ниже.
    bool keyfileChanged = false;
    {
        const QString name = QLatin1String(Keyfile::kRemoteName);
        SyncLedger::Blob led = ledger.blob(name);
        const QString etag = remoteEtag.value(name);
        keyfileChanged = !etag.isEmpty() && !led.etag.isEmpty() && etag != led.etag;
        if (!etag.isEmpty() && led.etag != etag) {
            led.etag = etag;
            ledger.setBlob(name, led);
        }
    }

    // Полный набор журналов: локальные (включая журналы удалённых и архивных
    // заметок — файла .md у них нет) и облачные.
    QSet<QString> journalIds(toCheck);
    for (const QString& id : journalIdsOnDisk()) journalIds.insert(id);
    for (auto it = remoteEtag.constBegin(); it != remoteEtag.constEnd(); ++it)
        if (it.key().endsWith(QStringLiteral(".log"))) {
            const QString stem = it.key().left(it.key().size() - 4);
            if (isValidNoteId(stem.toStdString())) journalIds.insert(stem);
        }

    const QElapsedTimer wholeRun = [] { QElapsedTimer t; t.start(); return t; }();
    const bool pushOnly = options.mode == SyncOptions::PushOnly;
    const auto budgetSpent = [&] {
        return pushOnly && wholeRun.elapsed() > qint64(options.exitPushBudgetSec) * 1000;
    };

    QStringList processed;           // с кого снять dirty-пометку
    QSet<QString> touched;           // у кого голова могла смениться (материализация)
    QHash<QString, Digest> oldHead;  // digest головы ДО обмена — двойное условие
    int aeadFailures = 0;

    QStringList orderedJournals(journalIds.begin(), journalIds.end());
    orderedJournals.sort();
    for (const QString& id : orderedJournals) {
        if (cancelled()) {
            done.cancelled = true;
            break;
        }
        if (budgetSpent()) {
            ++done.deferred;
            continue;
        }
        const QString name = id + QStringLiteral(".log");
        QByteArray localBytes;
        if (!readJournalBytes(id, &localBytes, error)) {
            takeTraffic();
            return finish(false);
        }
        SyncLedger::Blob led = ledger.blob(name);
        const bool haveRemote = remoteEtag.contains(name);
        const QString etag = remoteEtag.value(name);
        Digest localHash = localBytes.isEmpty() ? Digest() : hashBytes(localBytes);

        // ЯРУС 1: обе стороны там же, где были, — пропуск, ноль трафика.
        if (haveRemote && !led.etag.isEmpty() && led.etag == etag && !localBytes.isEmpty() &&
            localHash == led.plainHash) {
            ++done.skipped;
            processed.append(id);
            continue;
        }
        if (!haveRemote && localBytes.isEmpty()) continue;

        // ЦЕЛОСТНОСТЬ — ПРЕЖДЕ КЛАССИФИКАЦИИ. Битый локальный журнал — это
        // absence: принять удалённый целиком, НОЛЬ заливок — локальный бит-рот
        // никогда не уезжает в облако. Рваный хвост после обрыва — валиден,
        // но в облако едет только целая часть.
        ZJournal localJ;
        bool localValid = false;
        if (!localBytes.isEmpty()) {
            QString why;
            localValid = localJ.parse(localBytes, ZJournal::Want::All, 0, &why) &&
                         localJ.damagedCount() == 0;
            if (localValid && localJ.tailTrimmed()) {
                localBytes = localBytes.left(int(localJ.goodBytes()));
                localHash = hashBytes(localBytes);
            }
            if (!localValid) {
                ++done.corruptLocalTreatedAsAbsence;
                fprintf(stderr,
                        "zametti sync: local journal of %s does not pass validation (%s) — "
                        "treating as absence, uploading nothing\n",
                        qPrintable(id), qPrintable(why));
            }
        }
        if (localValid) {
            const int h = localJ.headIndex();
            if (h >= 0) oldHead.insert(id, localJ.at(h).digest());
        }

        const bool weChanged = localValid && (led.plainHash.empty() || localHash != led.plainHash);
        const bool theyChanged = haveRemote && led.etag != etag;

        bool needPush = false;
        QByteArray pushBytes;
        QString pushExpectedEtag = etag;

        if (!haveRemote) {
            // На сервере блоба нет. При записанном etag это повреждение
            // сервера — перезаливка-лечение; в push-only откладывается.
            if (localValid) {
                if (!led.etag.isEmpty()) ++done.healedRemote;
                needPush = true;
                pushBytes = localBytes;
                pushExpectedEtag.clear();
                if (led.etag.isEmpty() && localBytes.size() > 0) ++done.pushedWhole;
            }
        }

        if (haveRemote && (theyChanged || led.etag.isEmpty() || !localValid)) {
            if (pushOnly) {
                // Выход не скачивает и не сливает: отложено полному прогону.
                ++done.deferred;
                continue;
            }
            // ЯРУС 2: GET; хеш шифротекста прежний — это перевыдача etag.
            QByteArray blob;
            QString why;
            if (!remote_->get(name, &blob, nullptr, &why)) {
                complain(QStringLiteral("cannot download %1: %2").arg(name, why));
                continue;
            }
            const Digest sealed = hashBytes(blob);
            if (!led.sealedHash.empty() && sealed == led.sealedHash) {
                ++done.etagReissued;
                led.etag = etag;
                ledger.setBlob(name, led);
                if (localValid && weChanged) {
                    needPush = true;
                    pushBytes = localBytes;
                    ++done.pushedWhole;
                } else {
                    processed.append(id);
                    continue;
                }
            } else {
                // Настоящее чужое изменение: вскрыть и провалидировать.
                QByteArray remotePlain;
                const BlobAad aad{BlobKind::Journal, storeId, name};
                if (!cipher_->open(blob, aad, &remotePlain, &why)) {
                    ++aeadFailures;
                    if (keyfileChanged) {
                        // Ротация ключа с другого устройства — не порча.
                        if (error != nullptr)
                            *error = QStringLiteral(
                                "the cloud key was rotated on another device — enter the "
                                "password again (set-remote)");
                        takeTraffic();
                        return finish(false);
                    }
                    if (localValid) {
                        // Порча блоба: лечим здоровым своим.
                        ++done.healedRemote;
                        needPush = true;
                        pushBytes = localBytes;
                    } else {
                        complain(QStringLiteral("blob %1 fails AEAD and the local journal "
                                                "is invalid too")
                                     .arg(name));
                        continue;
                    }
                } else {
                    ZJournal remoteJ;
                    const bool remoteValid = remoteJ.parse(remotePlain, ZJournal::Want::All, 0,
                                                           &why) &&
                                             remoteJ.damagedCount() == 0 &&
                                             !remoteJ.tailTrimmed();
                    if (!remoteValid) {
                        if (localValid) {
                            ++done.healedRemote;
                            needPush = true;
                            pushBytes = localBytes;
                        } else {
                            complain(QStringLiteral("blob %1 decrypts but does not parse, and "
                                                    "the local journal is invalid too")
                                         .arg(name));
                            continue;
                        }
                    } else if (!localValid || localBytes.isEmpty()) {
                        // Принять целиком ЧУЖИМИ БАЙТАМИ: копии сходятся
                        // побайтово, самопроверка внутри.
                        if (!adoptJournalBytes(id, remotePlain, localHash, &why)) {
                            complain(QStringLiteral("cannot adopt %1: %2").arg(name, why));
                            continue;
                        }
                        ++done.takenWhole;
                        touched.insert(id);
                        led.etag = etag;
                        led.sealedHash = sealed;
                        led.plainHash = hashBytes(remotePlain);
                        ledger.setBlob(name, led);
                        processed.append(id);
                        continue;
                    } else {
                        // Обе стороны живые. Мы не менялись и чужой покрывает
                        // нас (параноидальная проверка вместо веры в ярусы) —
                        // принять целиком; иначе полное слияние.
                        const auto covers = [&] {
                            for (int i = 0; i < localJ.size(); ++i) {
                                const ZJournal::Record& r = localJ.at(i);
                                bool found = false;
                                for (int k = 0; k < remoteJ.size() && !found; ++k) {
                                    const ZJournal::Record& e = remoteJ.at(k);
                                    if (e.time() == r.time() && e.digest() == r.digest())
                                        found = true;
                                    else if (e.voidsRecord(r))
                                        found = true;
                                }
                                if (!found) return false;
                            }
                            return true;
                        };
                        if (!weChanged && covers()) {
                            if (!adoptJournalBytes(id, remotePlain, localHash, &why)) {
                                ++done.deferred;  // журнал дописался под рукой
                                continue;
                            }
                            ++done.takenWhole;
                            touched.insert(id);
                            led.etag = etag;
                            led.sealedHash = sealed;
                            led.plainHash = hashBytes(remotePlain);
                            ledger.setBlob(name, led);
                            processed.append(id);
                            continue;
                        }
                        ZJournal merged;
                        ZJournal::MergeStats st;
                        if (!localJ.mergedWith(remoteJ, &merged, &st, &why)) {
                            complain(QStringLiteral("merge of %1 refused: %2").arg(name, why));
                            continue;
                        }
                        done.merge.fromThis += st.fromThis;
                        done.merge.fromOther += st.fromOther;
                        done.merge.common += st.common;
                        done.merge.voidedDropped += st.voidedDropped;
                        done.merge.frameConflicts += st.frameConflicts;
                        const Digest mergedSet = merged.contentDigest();
                        if (mergedSet == remoteJ.contentDigest()) {
                            if (mergedSet != localJ.contentDigest()) {
                                if (!adoptJournalBytes(id, remotePlain, localHash, &why)) {
                                    ++done.deferred;
                                    continue;
                                }
                                ++done.takenWhole;
                                touched.insert(id);
                                led.plainHash = hashBytes(remotePlain);
                            } else {
                                // Наборы равны при разных байтах — анти-ping-pong:
                                // не заливать и не переписывать.
                                ++done.skipped;
                                led.plainHash = localHash;
                            }
                            led.etag = etag;
                            led.sealedHash = sealed;
                            ledger.setBlob(name, led);
                            processed.append(id);
                            continue;
                        }
                        if (mergedSet == localJ.contentDigest()) {
                            // Чужой — подмножество: залить свои байты как есть.
                            needPush = true;
                            pushBytes = localBytes;
                            ++done.pushedWhole;
                        } else {
                            if (!adoptMergedJournal(id, merged, localHash, &why)) {
                                ++done.deferred;
                                continue;
                            }
                            if (!readJournalBytes(id, &pushBytes, error)) {
                                takeTraffic();
                                return finish(false);
                            }
                            needPush = true;
                            ++done.mergedJournals;
                            touched.insert(id);
                        }
                    }
                }
            }
        } else if (haveRemote && localValid && weChanged && !theyChanged) {
            // Менялись только мы: fast-forward push целиком.
            needPush = true;
            pushBytes = localBytes;
            ++done.pushedWhole;
        }

        if (needPush) {
            QByteArray blob;
            const BlobAad aad{BlobKind::Journal, storeId, name};
            if (!cipher_->seal(pushBytes, aad, &blob, error)) {
                takeTraffic();
                return finish(false);
            }
            QString newEtag;
            bool clash = false;
            QString why;
            const bool ok =
                pushExpectedEtag.isEmpty()
                    ? remote_->put(name, blob, &newEtag, &why)
                    : remote_->putIfMatch(name, blob, pushExpectedEtag, &newEtag, &clash, &why);
            if (!ok) {
                complain(QStringLiteral("cannot upload %1: %2").arg(name, why));
                continue;
            }
            if (clash) {
                // Кто-то успел раньше: не трогаем бухгалтерию, следующий
                // полный прогон скачает и сольёт.
                ++done.deferred;
                continue;
            }
            led.etag = newEtag;
            led.sealedHash = hashBytes(blob);
            led.plainHash = hashBytes(pushBytes);
            ledger.setBlob(name, led);
            processed.append(id);
        }
    }

    // Ранний сигнал без единого расшифрованного блоба: ключ сменился, а
    // читать нечего не пробовали — не гадаем, скажет следующая расшифровка.

    // ВЛОЖЕНИЯ — presence-синк (решение владельца: XMP rev — отдельной
    // сессией). Нет в облаке — залить; нет локально — скачать; обе стороны
    // разные и обе новые — НЕ ТРОГАТЬ и назвать в отчёте.
    if (!pushOnly) {
        QSet<QString> attachmentSet;
        for (const QString& name : attachmentNames()) attachmentSet.insert(name);
        for (auto it = remoteEtag.constBegin(); it != remoteEtag.constEnd(); ++it) {
            const QString& name = it.key();
            if (name.endsWith(QStringLiteral(".log")) || name == QLatin1String(Identity::kFile) ||
                name == QLatin1String(Keyfile::kRemoteName))
                continue;
            const qsizetype dot = name.lastIndexOf(QLatin1Char('.'));
            if (dot <= 0 || !isValidNoteId(name.left(dot).toStdString())) continue;
            attachmentSet.insert(name);
        }
        QStringList orderedAttachments(attachmentSet.begin(), attachmentSet.end());
        orderedAttachments.sort();
        for (const QString& name : orderedAttachments) {
            if (cancelled()) {
                done.cancelled = true;
                break;
            }
            const bool haveLocal = QFile::exists(attachmentPath(name));
            const bool haveRemote = remoteEtag.contains(name);
            SyncLedger::Blob led = ledger.blob(name);
            const QString etag = remoteEtag.value(name);
            const BlobAad aad{BlobKind::Attachment, storeId, name};
            QString why;
            if (haveLocal && haveRemote && !led.etag.isEmpty() && led.etag == etag) continue;
            if (haveLocal && !haveRemote) {
                QByteArray bytes;
                if (!readAttachmentBytes(name, &bytes, &why)) {
                    complain(QStringLiteral("cannot read attachment %1: %2").arg(name, why));
                    continue;
                }
                QByteArray blob;
                QString newEtag;
                if (!cipher_->seal(bytes, aad, &blob, &why) ||
                    !remote_->put(name, blob, &newEtag, &why)) {
                    complain(QStringLiteral("cannot upload attachment %1: %2").arg(name, why));
                    continue;
                }
                ++done.attachmentsUp;
                ledger.setBlob(name, {newEtag, hashBytes(blob), hashBytes(bytes)});
                continue;
            }
            // Дальше нужен GET: либо файла нет локально, либо etag разошёлся.
            QByteArray blob;
            if (!remote_->get(name, &blob, nullptr, &why)) {
                complain(QStringLiteral("cannot download attachment %1: %2").arg(name, why));
                continue;
            }
            const Digest sealed = hashBytes(blob);
            if (haveLocal && !led.sealedHash.empty() && sealed == led.sealedHash) {
                ++done.etagReissued;
                led.etag = etag;
                ledger.setBlob(name, led);
                continue;
            }
            QByteArray plain;
            if (!cipher_->open(blob, aad, &plain, &why)) {
                ++aeadFailures;
                if (keyfileChanged) {
                    if (error != nullptr)
                        *error = QStringLiteral(
                            "the cloud key was rotated on another device — enter the password "
                            "again (set-remote)");
                    takeTraffic();
                    return finish(false);
                }
                if (haveLocal) {
                    // Порча блоба: здоровая сторона побеждает — перезаливка.
                    QByteArray bytes;
                    QByteArray healed;
                    QString newEtag;
                    if (readAttachmentBytes(name, &bytes, &why) &&
                        cipher_->seal(bytes, aad, &healed, &why) &&
                        remote_->put(name, healed, &newEtag, &why)) {
                        ++done.healedRemote;
                        ledger.setBlob(name, {newEtag, hashBytes(healed), hashBytes(bytes)});
                    } else {
                        complain(QStringLiteral("cannot heal attachment %1: %2").arg(name, why));
                    }
                } else {
                    complain(QStringLiteral("attachment %1 fails AEAD and there is no local "
                                            "copy")
                                 .arg(name));
                }
                continue;
            }
            if (!haveLocal) {
                // Скачивание: атомарная запись; dirty-пометки не нужны — это
                // вложение, его хеш ложится в бухгалтерию сразу.
                if (!writeFileBytes(attachmentPath(name),
                                    std::string(plain.constData(), size_t(plain.size())),
                                    &why)) {
                    complain(QStringLiteral("cannot write attachment %1: %2").arg(name, why));
                    continue;
                }
                ++done.attachmentsDown;
                ledger.setBlob(name, {etag, sealed, hashBytes(plain)});
                continue;
            }
            // Обе стороны есть и байты разные.
            QByteArray localAtt;
            if (!readAttachmentBytes(name, &localAtt, &why)) continue;
            const Digest localHashAtt = hashBytes(localAtt);
            if (localHashAtt == hashBytes(plain)) {
                ledger.setBlob(name, {etag, sealed, localHashAtt});
                continue;
            }
            if (!led.plainHash.empty() && localHashAtt == led.plainHash) {
                // Мы не менялись — чужое новее: принять.
                if (!writeFileBytes(attachmentPath(name),
                                    std::string(plain.constData(), size_t(plain.size())),
                                    &why)) {
                    complain(QStringLiteral("cannot write attachment %1: %2").arg(name, why));
                    continue;
                }
                ++done.attachmentsDown;
                ledger.setBlob(name, {etag, sealed, hashBytes(plain)});
                continue;
            }
            // Истинно конкурентная замена под одним id: без rev судить нечем —
            // не перетирать ничьё, назвать в отчёте (XMP rev придёт следующей
            // сессией).
            done.attachmentConflicts.append(name);
        }
    }
    done.usExchange = phase.nsecsElapsed() / 1000;
    phase.restart();

    // ===== ШАГ 4: МАТЕРИАЛИЗАЦИЯ ===========================================
    //
    // Только полный прогон, и никогда раньше выравнивания (инвариант A).
    // Условие двойное: голова ≠ файлу И файл не менялся с шага 1 — иначе
    // повторное выравнивание, не перезапись. Правился во время синка — не
    // проигрываешь никогда.
    if (!pushOnly && !done.cancelled) {
        QSet<QString> candidates = touched;
        for (const QString& id : toCheck) candidates.insert(id);
        QStringList deletes;
        QStringList orderedCandidates(candidates.begin(), candidates.end());
        orderedCandidates.sort();
        for (const QString& id : orderedCandidates) {
            ZJournal frames;
            QString why;
            if (!readJournal(id, &frames, &why) || frames.isEmpty()) continue;
            const int head = frames.headIndex();
            if (head < 0) continue;
            const ZJournal::Record& h = frames.at(head);
            const QString path = pathOf(id);
            std::string raw;
            const bool haveFile = readFileBytes(path, raw);
            const QByteArray current = QByteArray::fromStdString(raw);
            const Digest currentHash = haveFile ? hashBytes(current) : Digest();
            if (!h.hasSnapshot()) {
                // Надгробие-голова: файла быть не должно.
                if (haveFile) deletes.append(id);
                continue;
            }
            if (haveFile && currentHash == h.digest()) {
                const QFileInfo info(path);
                ledger.setFileStat(id, {info.lastModified().toMSecsSinceEpoch(), info.size()});
                continue;
            }
            if (haveFile) {
                const Digest atAlign = alignedFile.value(id);
                const Digest before = oldHead.value(id);
                const bool unchangedSinceAlign =
                    (!atAlign.empty() && currentHash == atAlign) ||
                    (!before.empty() && currentHash == before);
                if (!unchangedSinceAlign) {
                    // Файл правился, пока шёл синк, — повторное выравнивание,
                    // и ни в коем случае не перезапись.
                    if (journalFor(id, options.journalRules)
                            ->record(ZJournal::Kind::External, current, &why))
                        ++done.externalRecorded;
                    ++done.deferred;
                    continue;
                }
            }
            QByteArray snap;
            if (!journalSnapshot(id, head, &snap, &why)) {
                complain(QStringLiteral("head snapshot of %1 does not rebuild: %2").arg(id, why));
                continue;
            }
            // Хеш из записи сверяет распакованные байты (пересборка уже
            // сверила; сверка здесь — та самая паранойя, что и в сохранении).
            if (hashBytes(snap) != h.digest()) {
                complain(QStringLiteral("head snapshot of %1 does not match its digest").arg(id));
                continue;
            }
            if (!writeFileBytes(path, std::string(snap.constData(), size_t(snap.size())), &why)) {
                complain(QStringLiteral("cannot materialize %1: %2").arg(id, why));
                continue;
            }
            // Прочитать обратно и свериться: материализация не вправе верить
            // даже атомарной записи на слово.
            std::string back;
            if (!readFileBytes(path, back) ||
                hashBytes(QByteArray::fromStdString(back)) != h.digest()) {
                complain(QStringLiteral("materialized %1 reads back differently").arg(id));
                continue;
            }
            ++done.materialized;
            const QFileInfo info(path);
            ledger.setFileStat(id, {info.lastModified().toMSecsSinceEpoch(), info.size()});
        }

        // ПРЕДОХРАНИТЕЛЬ МАССОВОГО УДАЛЕНИЯ (порог владельца). Сверх порога —
        // ни одного удаления; список уходит в отчёт, решает человек:
        // подтвердил → повторный прогон с allowMassDelete; отказал →
        // declareAlive, и облако лечится записями поверх надгробий.
        if (int(deletes.size()) > options.deleteGuard && !options.allowMassDelete) {
            done.pendingDeletes = deletes;
        } else {
            for (const QString& id : deletes) {
                const QString path = pathOf(id);
                if (!QFile::moveToTrash(path) && !QFile::remove(path)) {
                    complain(QStringLiteral("cannot remove %1 for its tombstone").arg(id));
                    continue;
                }
                ++done.deletesApplied;
                ledger.dropFile(id);
                processed.append(id);
            }
        }
    }
    done.usMaterialize = phase.nsecsElapsed() / 1000;

    // МАНИФЕСТ — последним и открытым текстом, как в pushAll: «здесь лежит
    // хранилище такое-то» говорится после того, как это стало правдой.
    if (!pushOnly && !done.cancelled && !remoteEtag.contains(QLatin1String(Identity::kFile))) {
        QString newEtag;
        QString why;
        if (remote_->put(QLatin1String(Identity::kFile), mine.toBytes(), &newEtag, &why)) {
            SyncLedger::Blob led;
            led.etag = newEtag;
            led.sealedHash = hashBytes(mine.toBytes());
            ledger.setBlob(QLatin1String(Identity::kFile), led);
        } else {
            complain(QStringLiteral("cannot upload the manifest: %1").arg(why));
        }
    }

    // Материализация метит файлы в dirty (штатный write-ahead писателя) — но
    // мы сами и привели их к голове, пометка снимается вместе с обработанными.
    if (!done.cancelled) clearDirty(processed);
    ledger.setCleanShutdown(true);
    QString ledgerWhy;
    if (!ledger.save(&ledgerWhy))
        fprintf(stderr, "zametti sync: cannot save the ledger: %s\n", qPrintable(ledgerWhy));
    takeTraffic();

    if (done.integrityFailures > 0) {
        if (error != nullptr)
            *error = QStringLiteral("%1 blob(s) failed integrity or transfer — see stderr; "
                                    "the rest of the run is complete")
                         .arg(done.integrityFailures);
        return finish(false);
    }
    return finish(true);
}

bool ZStorage::setRemote(const std::shared_ptr<RemoteStore>& remote,
                         const Keyfile& keyfile, QString* error) {
    if (!store_) {
        if (error != nullptr) *error = QStringLiteral("not a store: %1").arg(root_);
        return false;
    }
    if (!remote) {
        if (error != nullptr) *error = QStringLiteral("no remote given");
        return false;
    }
    if (!keyfile.hasKey()) {
        if (error != nullptr)
            *error = QStringLiteral("the keyfile carries no live key — unwrap it first");
        return false;
    }

    // Своя идентичность — до всякого разговора с сервером: без storeId
    // сверять нечего, а чеканить его при подключении к чужому облаку тем
    // более нельзя.
    const Identity mine = ensureIdentity(error);
    if (mine.isEmpty()) return false;
    if (mine.storeId() != keyfile.storeId()) {
        if (error != nullptr)
            *error = QStringLiteral(
                "this keyfile belongs to store %1, and this store is %2")
                         .arg(keyfile.storeId(), mine.storeId());
        return false;
    }

    // МАНИФЕСТ СВЕРЯЕМ ДО ЕДИНОЙ ЗАПИСИ. Он лежит открытым текстом ровно
    // затем, чтобы «то ли это облако» решалось без пароля. Расхождение
    // storeId — не конфликт, который надо сливать, а ошибка адресации:
    // опечатка в remoteDir иначе молча слила бы две несвязанные базы.
    QByteArray manifest;
    QString why;
    if (remote->get(QLatin1String(Identity::kFile), &manifest, nullptr, &why)) {
        Identity theirs;
        if (!theirs.parse(manifest, &why)) {
            if (error != nullptr)
                *error = QStringLiteral("the cloud manifest is unreadable: %1").arg(why);
            return false;
        }
        if (theirs.storeId() != mine.storeId()) {
            if (error != nullptr)
                *error = QStringLiteral(
                    "this cloud folder belongs to another store (%1), and this "
                    "store is %2 — check sync.remoteDir")
                             .arg(theirs.storeId(), mine.storeId());
            return false;
        }
        if (theirs.tooNew()) {
            if (error != nullptr)
                *error = QStringLiteral(
                    "the cloud was written by a newer version of the program "
                    "(format %1) — update this one")
                             .arg(theirs.formatVersion());
            return false;
        }
    }
    // Манифеста нет — так выглядит первый синк; это не беда.

    auto cipher = XChaChaCipher::make(keyfile, error);
    if (!cipher) return false;
    remote_ = remote;
    cipher_ = cipher;
    return true;
}

void ZStorage::dropRemote() {
    remote_.reset();
    cipher_.reset();
}

bool ZStorage::pushAll(PushReport* report, QString* error) {
    PushReport done;
    const auto finish = [&](bool ok) {
        if (report != nullptr) *report = done;
        return ok;
    };
    if (!hasRemote()) {
        if (error != nullptr)
            *error = QStringLiteral("no cloud is connected — call setRemote first");
        return finish(false);
    }
    const Identity mine = ensureIdentity(error);
    if (mine.isEmpty()) return finish(false);
    if (!loaded_) reload();

    QElapsedTimer timer;

    // 1. ДОЖУРНАЛИЗАЦИЯ. Заметка без журнала — это заметка, которой в облаке
    // нет вовсе: облако хранит журналы, а `.md` — их местная материализация.
    // Опорной записи отдают время ФАЙЛА (заметка 2017 года обязана и в
    // истории начинаться 2017 годом).
    timer.start();
    const QStringList noteIds = ids();
    for (const QString& id : noteIds) {
        std::string bytes;
        if (!readFileBytes(pathOf(id), bytes)) continue;
        ZJournal have;
        if (!readJournal(id, &have, error)) return finish(false);
        if (!have.isEmpty()) continue;
        const QFileInfo info(pathOf(id));
        journalFor(id, {})->ensureBaseline(QByteArray::fromStdString(bytes),
                                           info.lastModified().toMSecsSinceEpoch());
        ++done.baselined;
    }
    done.usBaseline = timer.nsecsElapsed() / 1000;

    if (!remote_->mkdirOnce(error)) return finish(false);

    // 2. ЗАЛИВКА. Один путь для журналов и вложений: имя блоба, байты, AAD.
    const auto sealAndPut = [&](const QString& name, const QByteArray& plain) {
        QByteArray blob;
        timer.restart();
        const BlobAad aad{kindOfBlob(name), mine.storeId(), name};
        if (!cipher_->seal(plain, aad, &blob, error)) return false;
        done.usSeal += timer.nsecsElapsed() / 1000;
        done.plainBytes += plain.size();
        done.sealedBytes += blob.size();

        timer.restart();
        const bool ok = remote_->put(name, blob, nullptr, error);
        done.usPut += timer.nsecsElapsed() / 1000;
        return ok;
    };

    for (const QString& id : noteIds) {
        QByteArray journal;
        if (!readJournalBytes(id, &journal, error)) return finish(false);
        if (journal.isEmpty()) continue;
        if (!sealAndPut(id + QStringLiteral(".log"), journal)) return finish(false);
        ++done.journals;
    }

    for (const QString& name : attachmentNames()) {
        QByteArray bytes;
        if (!readAttachmentBytes(name, &bytes, error)) return finish(false);
        if (!sealAndPut(name, bytes)) return finish(false);
        ++done.attachments;
    }

    // 3. МАНИФЕСТ — последним и ОТКРЫТЫМ ТЕКСТОМ. Последним потому, что он
    // означает «здесь лежит хранилище такое-то»: сказать это раньше, чем
    // хоть что-то залито, значило бы соврать при обрыве.
    timer.restart();
    if (!remote_->put(QLatin1String(Identity::kFile), mine.toBytes(), nullptr, error))
        return finish(false);
    done.usPut += timer.nsecsElapsed() / 1000;
    return finish(true);
}

}  // namespace zametti
