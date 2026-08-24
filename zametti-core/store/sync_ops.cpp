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
