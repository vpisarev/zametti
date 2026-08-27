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
#include "zlogs.h"
#include "note_id.h"
#include "secret_store.h"
#include "times.h"
#include "webdav_remote.h"

#include <QDateTime>
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

// --- ярлыки хранилищ для диагностик -----------------------------------------
//
// «Чужое облако» человеку показывается не голым id: рядом читаемое имя и
// дата создания (решение владельца, 24.08.2026). Дата — из манифеста, в UTC;
// имя — заголовок КОРНЕВОЙ заметки: своё читается с диска (localStoreName),
// облачное — best-effort вскрытием доступным ключом (cloudStoreName): опечатка
// между СВОИМИ хранилищами на одном сервере вскроется; по-настоящему чужое
// честно остаётся без имени — оно зашифровано, и это не недостаток, а само
// шифрование.

QString utcOf(const QString& iso) {
    const QDateTime t = QDateTime::fromString(iso, Qt::ISODateWithMs);
    if (!t.isValid()) return iso;
    // Секундной точности достаточно: это дата для глаз, не ключ сравнения.
    return t.toUTC().toString(Qt::ISODate);
}

// Заголовок заметки из её байтов: первая строка '# …' после шапки.
QString titleOfBody(const QByteArray& body) {
    for (const QByteArray& raw : body.split('\n')) {
        const QByteArray line = raw.trimmed();
        if (line.startsWith("# ")) return QString::fromUtf8(line.mid(2)).trimmed();
    }
    return {};
}

QString storeTag(const QString& id, const QString& name, const QString& createdIso) {
    QString out = id;
    if (!name.isEmpty()) out += QStringLiteral(" \"%1\"").arg(name);
    if (!createdIso.isEmpty()) out += QStringLiteral(" (created %1)").arg(utcOf(createdIso));
    return out;
}

constexpr char kAddressHint[] = "; check the cloud address (set-remote --url/--to)";

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

// Один читатель на оба дома записи (remote.json и строка списка в state.json):
// чего в объекте нет, то пусто. Прежние ключи url/dir/user читаются запасным
// путём — старые remote.json продолжают работать и мигрируют при следующей
// записи.
void ZStorage::Config::parse(const QJsonObject& o) {
    const auto text = [&o](const char* fresh, const char* legacy) {
        const QJsonValue v = o.value(QLatin1String(fresh));
        if (v.isString()) return v.toString();
        return legacy != nullptr ? o.value(QLatin1String(legacy)).toString() : QString();
    };
    root = text("root", nullptr);
    name = text("name", nullptr);
    remoteUrl = text("remoteUrl", "url");
    remoteDir = text("remoteDir", "dir");
    remoteUser = text("remoteUser", "user");
    allowInsecureHttp = o.value(QStringLiteral("allowInsecureHttp")).toBool(false);
    timeoutMs = o.value(QStringLiteral("timeoutMs")).toInt(30000);
}

bool ZStorage::Config::parse(const QByteArray& bytes, QString* error) {
    QJsonParseError bad;
    const QJsonDocument doc = QJsonDocument::fromJson(bytes, &bad);
    if (bad.error != QJsonParseError::NoError || !doc.isObject()) {
        if (error) *error = QStringLiteral("remote.json is not a JSON object");
        return false;
    }
    parse(doc.object());
    return true;
}

// В remote.json уходит ТОЛЬКО облачная сторона: файл лежит В корне копии, и
// путь, вписанный внутрь, протух бы при cp -r.
QByteArray ZStorage::Config::remoteBytes() const {
    QJsonObject o;
    if (!remoteUrl.isEmpty()) o.insert(QStringLiteral("remoteUrl"), remoteUrl);
    if (!remoteDir.isEmpty()) o.insert(QStringLiteral("remoteDir"), remoteDir);
    if (!remoteUser.isEmpty()) o.insert(QStringLiteral("remoteUser"), remoteUser);
    if (allowInsecureHttp) o.insert(QStringLiteral("allowInsecureHttp"), true);
    o.insert(QStringLiteral("timeoutMs"), timeoutMs);
    return QJsonDocument(o).toJson(QJsonDocument::Indented);
}

QJsonObject ZStorage::Config::entryJson() const {
    QJsonObject o;
    if (!root.isEmpty()) o.insert(QStringLiteral("root"), root);
    if (!name.isEmpty()) o.insert(QStringLiteral("name"), name);
    if (!remoteUrl.isEmpty()) o.insert(QStringLiteral("remoteUrl"), remoteUrl);
    if (!remoteDir.isEmpty()) o.insert(QStringLiteral("remoteDir"), remoteDir);
    if (!remoteUser.isEmpty()) o.insert(QStringLiteral("remoteUser"), remoteUser);
    if (allowInsecureHttp) o.insert(QStringLiteral("allowInsecureHttp"), true);
    o.insert(QStringLiteral("timeoutMs"), timeoutMs);
    return o;
}

ZStorage::Config ZStorage::remoteConfig() const {
    Config cfg;
    QFile f(root_ + QStringLiteral("/.zametti/remote.json"));
    if (f.open(QIODevice::ReadOnly)) {
        QString why;
        if (!cfg.parse(f.readAll(), &why)) {
            // Битый конфиг ничего не подключает — как битый config.json ничего
            // не перезагружает. Конфиг без облака = «не настроен», и это видно.
            fprintf(stderr, "zametti: %s: %s\n", qPrintable(f.fileName()), qPrintable(why));
            cfg = Config();
        }
    }
    // root — корень ЭТОЙ копии, ПОСЛЕ разбора (читатель пишет все поля, и
    // root из него всегда пуст: в remote.json путь не пишется — переехал бы
    // вместе с каталогом и врал). name не заполняется нарочно: заголовок
    // корня стоит чтения файла, а сюда ходят на каждый пересчёт тулбара.
    cfg.root = root_;
    return cfg;
}

bool ZStorage::writeRemoteConfig(const Config& cfg, QString* error) {
    QDir().mkpath(root_ + QStringLiteral("/.zametti"));
    QSaveFile save(root_ + QStringLiteral("/.zametti/remote.json"));
    if (!save.open(QIODevice::WriteOnly)) {
        if (error) *error = QStringLiteral("cannot write remote.json: %1").arg(save.errorString());
        return false;
    }
    save.write(cfg.remoteBytes());
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

std::shared_ptr<RemoteStore> ZStorage::makeRemote(const Config& cfg,
                                                  const QString& serverPassword,
                                                  QString* error) {
    if (!cfg.hasCloud()) {
        if (error) *error = QStringLiteral("sync is not configured");
        return nullptr;
    }
    if (!cfg.remoteUrl.isEmpty()) {
        WebDavRemote::Config web;
        web.base = QUrl(cfg.remoteUrl);
        web.user = cfg.remoteUser;
        web.password = serverPassword;
        web.allowInsecureHttp = cfg.allowInsecureHttp;
        web.timeoutMs = cfg.timeoutMs;
        // Адрес проверяется ДО первой операции: пароль не уедет открытым
        // текстом даже один раз.
        if (!WebDavRemote::checkUrl(web, error)) return nullptr;
        return std::make_shared<WebDavRemote>(web);
    }
    return std::make_shared<FolderRemote>(cfg.remoteDir);
}

bool ZStorage::cloudHasKeyfile(const Config& cfg, const QString& serverPassword,
                               QString* error) {
    auto remote = makeRemote(cfg, serverPassword, error);
    if (!remote) return false;
    QByteArray envelope;
    return remote->get(QLatin1String(Keyfile::kRemoteName), &envelope, nullptr, nullptr);
}

bool ZStorage::attachRemote(const AttachOptions& how, SecretStore& secrets,
                            AttachOutcome* outcome, QString* error) {
    if (outcome) *outcome = AttachOutcome{};
    // «Адрес назван» — это про облако: ключи командной строки сильнее
    // remote.json, а root у обоих кандидатов и так этот.
    const Config cfg = how.cfg.hasCloud() ? how.cfg : remoteConfig();
    if (!cfg.hasCloud()) {
        if (error) *error = QStringLiteral("sync is not configured for this store");
        return false;
    }
    const Identity mine = identity(error);
    if (mine.isEmpty()) {
        if (error && error->isEmpty()) *error = QStringLiteral("the store has no identity");
        return false;
    }
    QString why;
    QString password = how.serverPassword;
    if (password.isEmpty() && !cfg.remoteUrl.isEmpty())
        password = secrets.serverPassword(mine.storeId(), &why);
    auto remote = makeRemote(cfg, password, error);
    if (!remote) return false;

    Keyfile keyfile;
    if (!secrets.loadKey(mine.storeId(), &keyfile, &why)) {
        // Ключа под рукой нет. Дальше — только с паролем: без него ни конверт
        // не развернуть, ни новый ключ не отчеканить.
        QByteArray envelope;
        const bool haveEnvelope =
            !how.encryptionPassword.isEmpty() &&
            remote->get(QLatin1String(Keyfile::kRemoteName), &envelope, nullptr, nullptr);
        if (haveEnvelope) {
            if (!keyfile.parse(envelope, error) ||
                !keyfile.unwrap(how.encryptionPassword, error))
                return false;
        } else if (how.mintIfCloudEmpty && !how.encryptionPassword.isEmpty()) {
            // ПЕРВАЯ ЗАЛИВКА В ПУСТОЕ ОБЛАКО. Только по явному разрешению: без
            // него «конверта нет» значит «не туда смотрим», а не «пора чеканить»
            // — молча отчеканенный второй ключ развёл бы копии навсегда.
            if (!Keyfile::create(mine.storeId(), how.encryptionPassword, how.mintParams,
                                 &keyfile, error))
                return false;
            if (!remote->mkdirOnce(error) ||
                !remote->put(QLatin1String(Keyfile::kRemoteName), keyfile.toBytes(), nullptr,
                             error))
                return false;
            if (outcome) outcome->mintedKeyfile = true;
        } else {
            // Не беда, а «настройся заново»: утраченный keyring лечится одним
            // вводом пароля (set-remote), и это названный брифом случай.
            if (error)
                *error = QStringLiteral("the key is not in the keyring (%1) — run set-remote "
                                        "once, or set ZAMETTI_SYNC_KEY / ZAMETTI_SYNC_PASSWORD")
                             .arg(why);
            return false;
        }
    }
    return setRemote(remote, keyfile, error);
}

std::shared_ptr<ZStorage> ZStorage::initFromRemote(
    const QString& root, const Config& cfg, const QString& encryptionPassword,
    const QString& serverPassword, SecretStore& secrets, const Keyfile::KdfParams& mintParams,
    ConnectOutcome* outcome, QString* error) {
    auto storage = std::make_shared<ZStorage>(root);
    if (!storage->connectRemote(cfg, encryptionPassword, serverPassword, secrets, mintParams,
                                outcome, error))
        return nullptr;
    return storage;
}

bool ZStorage::connectRemote(const Config& cfg, const QString& encryptionPassword,
                             const QString& serverPassword, SecretStore& secrets,
                             const Keyfile::KdfParams& mintParams, ConnectOutcome* outcome,
                             QString* error) {
    ConnectOutcome done;
    const auto finish = [&](bool ok) {
        if (outcome != nullptr) *outcome = done;
        return ok;
    };
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

    if (!store_) {
        // ПУСТОЙ ИЛИ НЕСУЩЕСТВУЮЩИЙ каталог — законный вход нового устройства,
        // но ТОЛЬКО при непустом облаке. «Пусто с обеих сторон» в жизни почти
        // не бывает — так выглядит опечатка в адресе облака или в локальном
        // пути (решение владельца), и молча родить новую пару хранилище+облако
        // значило бы её спрятать. Новое хранилище начинается с init.
        if (!haveManifest) {
            if (error)
                *error = QStringLiteral(
                    "the cloud is empty and '%1' is not a store — this looks like a "
                    "mistyped cloud address or local path; to really start a fresh "
                    "store here, run 'zametti store init' first")
                             .arg(root_);
            return finish(false);
        }
        // Каркас — БЕЗ чеканки идентичности: id наследуется из манифеста ниже.
        // Непустой каталог без метки хранилища makeSkeleton отвергает сам:
        // случайную папку с файлами в хранилище не превращаем.
        if (!makeSkeleton(error)) return finish(false);
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
                             "this store has notes but no identity, and the cloud belongs "
                             "to store %1 — bootstrap into an empty folder instead")
                             .arg(storeTag(theirs.storeId(), QString(), theirs.created()));
            return finish(false);
        } else {
            mine = ensureIdentity(error);
            if (mine.isEmpty()) return finish(false);
        }
    }
    if (haveManifest && theirs.storeId() != mine.storeId()) {
        // Честная остановка ДО единой записи — включая keyfile. Имя чужого —
        // best-effort: ИХ конверт пробуем развернуть ДАННЫМ паролем (свои
        // хранилища на одном сервере обычно делят пароль — опечатка в папке
        // тут же видна по имени); другой пароль — имени честно нет.
        if (error) {
            QString cloudName;
            QByteArray envelope;
            Keyfile theirKeyfile;
            if (remote->get(QLatin1String(Keyfile::kRemoteName), &envelope, nullptr, &why) &&
                theirKeyfile.parse(envelope, nullptr) && !theirKeyfile.tooNew() &&
                theirKeyfile.unwrap(encryptionPassword, nullptr)) {
                if (auto probe = XChaChaCipher::make(theirKeyfile, nullptr))
                    cloudName = cloudStoreName(*remote, *probe, theirs);
            }
            *error = QStringLiteral(
                         "this cloud folder belongs to another store — %1 — and this "
                         "store is %2%3")
                         .arg(storeTag(theirs.storeId(), cloudName, theirs.created()),
                              storeTag(mine.storeId(), localStoreName(), mine.created()),
                              QLatin1String(kAddressHint));
        }
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
                *error = QStringLiteral("the cloud keyfile belongs to store %1, and this "
                                        "store is %2%3")
                             .arg(storeTag(keyfile.storeId(), QString(), keyfile.created()),
                                  storeTag(mine.storeId(), localStoreName(), mine.created()),
                                  QLatin1String(kAddressHint));
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

    // ЧТО ЛЕЖИТ В ОБЛАКЕ — человеку при настройке: один листинг, ноль
    // расшифровок. Имена открыты, поэтому «сколько заметок и картинок»
    // видно до всякого пароля; объём — по шифротексту.
    {
        QVector<RemoteStore::Entry> listing;
        QString why;
        if (remote->list(&listing, &why)) {
            for (const RemoteStore::Entry& e : listing) {
                done.cloudBytes += e.size;
                if (e.name.endsWith(QStringLiteral(".log")))
                    ++done.cloudNotes;
                else if (e.name != QLatin1String(Identity::kFile) &&
                         e.name != QLatin1String(Keyfile::kRemoteName))
                    ++done.cloudAttachments;
            }
        }
    }

    // БУТСТРАП ГОТОВИТ ХРАНИЛИЩЕ К ЖИЗНИ СРАЗУ: корневая заметка скачивается
    // и материализуется здесь же — дерево нового устройства показывает имя
    // хранилища, не дожидаясь первого полного sync. Неудача — не провал
    // подключения: sync всё равно привезёт всё, поэтому только stderr.
    if (done.inheritedIdentity && !theirs.rootNote().isEmpty()) {
        QString why;
        if (fetchAndMaterialize(theirs.rootNote(), &why))
            done.rootMaterialized = true;
        else
            fprintf(stderr, "zametti: cannot fetch the root note now (%s) — 'sync' will\n",
                    qPrintable(why));
    }

    // ЗАПОМНИТЬ. Отказ keyring подключение не валит: программа работает,
    // просто следующий старт снова спросит пароль — и скажет об этом.
    QString keep;
    if (!secrets.storeKey(keyfile, &keep))
        fprintf(stderr, "zametti: the keyring refused the key: %s\n", qPrintable(keep));
    if (!cfg.remoteUrl.isEmpty() && !serverPassword.isEmpty() &&
        !secrets.setServerPassword(mine.storeId(), serverPassword, &keep))
        fprintf(stderr, "zametti: the keyring refused the server password: %s\n",
                qPrintable(keep));
    if (!writeRemoteConfig(cfg, error)) return finish(false);
    return finish(true);
}

// ЧТО В ОБЛАКЕ ПО ЭТОМУ АДРЕСУ — разведка диалога первичной настройки, ДО
// выбора местного каталога. Листинг первым: он же проверка адреса и пароля
// сервера, и по открытым именам видно и манифест, и конверт, и объём — без
// единой расшифровки. Конверт, если он есть, разворачивается данным паролем:
// «пароль подошёл» диалог обязан знать до того, как человек выберет папку.
bool ZStorage::probeCloud(const Config& cfg, const QString& serverPassword,
                          const QString& encryptionPassword, CloudProbe* out,
                          QString* error) {
    CloudProbe probe;
    const auto finish = [&](bool ok) {
        if (out != nullptr) *out = probe;
        return ok;
    };
    auto remote = makeRemote(cfg, serverPassword, error);
    if (!remote) return finish(false);

    QVector<RemoteStore::Entry> listing;
    if (!remote->list(&listing, error)) return finish(false);
    for (const RemoteStore::Entry& e : listing) {
        probe.bytes += e.size;
        if (e.name == QLatin1String(Identity::kFile))
            probe.hasManifest = true;
        else if (e.name == QLatin1String(Keyfile::kRemoteName))
            probe.hasKeyfile = true;
        else if (e.name.endsWith(QStringLiteral(".log")))
            ++probe.notes;
        else
            ++probe.attachments;
    }

    QString why;
    if (probe.hasManifest) {
        QByteArray manifestBytes;
        if (!remote->get(QLatin1String(Identity::kFile), &manifestBytes, nullptr, &why)) {
            if (error) *error = QStringLiteral("cannot read the cloud manifest: %1").arg(why);
            return finish(false);
        }
        if (!probe.identity.parse(manifestBytes, &why)) {
            if (error) *error = QStringLiteral("the cloud manifest is unreadable: %1").arg(why);
            return finish(false);
        }
        if (probe.identity.tooNew()) {
            if (error)
                *error = QStringLiteral(
                    "the cloud was written by a newer version of the program — update this one");
            return finish(false);
        }
    }

    if (probe.hasKeyfile) {
        QByteArray envelope;
        Keyfile keyfile;
        if (!remote->get(QLatin1String(Keyfile::kRemoteName), &envelope, nullptr, &why)) {
            if (error) *error = QStringLiteral("cannot read the cloud keyfile: %1").arg(why);
            return finish(false);
        }
        if (!keyfile.parse(envelope, error)) return finish(false);
        if (keyfile.tooNew()) {
            if (error)
                *error = QStringLiteral(
                    "the keyfile was written by a newer version of the program — update this one");
            return finish(false);
        }
        if (!keyfile.unwrap(encryptionPassword, &why)) {
            // По построению AEAD неверный пароль и порча неразличимы.
            if (error)
                *error = QStringLiteral("wrong password, or the keyfile is corrupted: %1").arg(why);
            return finish(false);
        }
        probe.keyOpened = true;
        if (probe.hasManifest) {
            if (auto cipher = XChaChaCipher::make(keyfile, nullptr))
                probe.name = cloudStoreName(*remote, *cipher, probe.identity);
        }
    }
    return finish(true);
}

// СБРОС ПАРОЛЯ ШИФРОВАНИЯ — замена облачной копии. Единственный лечащий ход
// при забытом пароле: конверт без пароля не развернуть по построению.
// Стирается ТОЛЬКО своё облако (или безымянное), и стирание идёт конвертом и
// манифестом вперёд: оборванная чистка не должна выглядеть ни целым
// хранилищем, ни действующим конвертом. Облако без манифеста — «первый синк»,
// так что обрыв в любой точке долечивается следующим прогоном.
bool ZStorage::resetCloudEncryption(const Config& cfg, const QString& newPassword,
                                    const QString& serverPassword, SecretStore& secrets,
                                    const Keyfile::KdfParams& mintParams, ResetOutcome* outcome,
                                    QString* error) {
    ResetOutcome done;
    const auto finish = [&](bool ok) {
        if (outcome != nullptr) *outcome = done;
        return ok;
    };
    if (!store_) {
        if (error) *error = QStringLiteral("not a store: %1").arg(root_);
        return finish(false);
    }
    if (newPassword.isEmpty()) {
        if (error) *error = QStringLiteral("the encryption password must not be empty");
        return finish(false);
    }
    const Identity mine = ensureIdentity(error);
    if (mine.isEmpty()) return finish(false);
    auto remote = makeRemote(cfg, serverPassword, error);
    if (!remote) return finish(false);
    // Каталог в облаке заводится ДО листинга: сброс при пустом (или ещё не
    // существующем) облаке — законный случай «облачной копии нет, будет».
    // Заодно это первая проверка адреса и пароля сервера.
    if (!remote->mkdirOnce(error)) return finish(false);

    // ЧУЖОЕ ОБЛАКО НЕ СТИРАЕТСЯ. Нечитаемый манифест — тоже отказ: непонятно
    // чьё стирать нельзя, и это отличает сброс от простого подключения.
    QByteArray manifestBytes;
    QString why;
    if (remote->get(QLatin1String(Identity::kFile), &manifestBytes, nullptr, &why)) {
        Identity theirs;
        if (!theirs.parse(manifestBytes, &why)) {
            if (error)
                *error = QStringLiteral(
                             "the cloud manifest is unreadable (%1) — refusing to wipe a "
                             "cloud that cannot be identified")
                             .arg(why);
            return finish(false);
        }
        if (theirs.storeId() != mine.storeId()) {
            if (error)
                *error = QStringLiteral(
                             "this cloud folder belongs to another store — %1 — and this "
                             "store is %2; refusing to wipe it%3")
                             .arg(storeTag(theirs.storeId(), QString(), theirs.created()),
                                  storeTag(mine.storeId(), localStoreName(), mine.created()),
                                  QLatin1String(kAddressHint));
            return finish(false);
        }
    }

    // Новый ключ — до первой стирающей операции: не отчеканился — облако цело.
    Keyfile keyfile;
    if (!Keyfile::create(mine.storeId(), newPassword, mintParams, &keyfile, error))
        return finish(false);

    QVector<RemoteStore::Entry> listing;
    if (!remote->list(&listing, error)) return finish(false);
    QStringList names;
    for (const RemoteStore::Entry& e : listing) names.append(e.name);
    // Конверт и манифест — первыми (см. шапку), остальное — как перечислилось.
    for (const QLatin1String first :
         {QLatin1String(Keyfile::kRemoteName), QLatin1String(Identity::kFile)})
        if (names.removeAll(first) > 0) names.prepend(first);
    for (const QString& name : names) {
        if (!remote->del(name, &why)) {
            if (error) *error = QStringLiteral("cannot remove %1 from the cloud: %2").arg(name, why);
            return finish(false);
        }
        ++done.wiped;
    }

    if (!remote->put(QLatin1String(Keyfile::kRemoteName), keyfile.toBytes(), nullptr, error))
        return finish(false);
    if (!setRemote(remote, keyfile, error)) return finish(false);
    if (!pushAll(&done.push, error)) return finish(false);

    // Бухгалтерия синка — про блобы, которых больше нет: пусть следующий
    // прогон построит её заново, это кэш, а не истина.
    QFile::remove(SyncLedger::pathFor(mine.storeId(), root_));

    // Запомнить, как в connectRemote: отказ keyring сброс не валит — облако
    // уже заменено, просто следующий старт снова спросит пароль.
    QString keep;
    if (!secrets.storeKey(keyfile, &keep))
        fprintf(stderr, "zametti: the keyring refused the key: %s\n", qPrintable(keep));
    if (!cfg.remoteUrl.isEmpty() && !serverPassword.isEmpty() &&
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



// Имя СВОЕГО хранилища — заголовок корневой заметки, читается файлом (не
// каталогом notes_: ярлык нужен и потоку синка).
QString ZStorage::localStoreName() const {
    const QString root = identity().rootNote();
    if (root.isEmpty()) return {};
    std::string raw;
    if (!readFileBytes(pathOf(root), raw)) return {};
    return titleOfBody(QByteArray::fromStdString(raw));
}

// Имя ОБЛАЧНОГО хранилища — best-effort: журнал их корня вскрывается данным
// шифром. Не вскрылся (чужой ключ, нет журнала, нет сети) — пусто, без жалоб.
QString ZStorage::cloudStoreName(RemoteStore& remote, BlobCipher& cipher,
                                 const Identity& theirs) {
    if (theirs.rootNote().isEmpty()) return {};
    const QString name = theirs.rootNote() + QStringLiteral(".log");
    QByteArray blob;
    QString why;
    if (!remote.get(name, &blob, nullptr, &why)) return {};
    QByteArray plain;
    const BlobAad aad{BlobKind::Journal, theirs.storeId(), name};
    if (!cipher.open(blob, aad, &plain, &why)) return {};
    ZJournal j;
    if (!j.parse(plain, ZJournal::Want::All, 0, &why)) return {};
    const int head = j.headIndex();
    if (head < 0 || !j.at(head).hasSnapshot()) return {};
    QByteArray body;
    if (!j.rebuildAt(head, &body, &why)) return {};
    return titleOfBody(body);
}

bool ZStorage::fetchAndMaterialize(const QString& id, QString* error) {
    if (!hasRemote()) {
        if (error) *error = QStringLiteral("no cloud is connected");
        return false;
    }
    const Identity mine = identity();
    const QString name = id + QStringLiteral(".log");
    QByteArray blob;
    if (!remote_->get(name, &blob, nullptr, error)) return false;
    QByteArray plain;
    const BlobAad aad{BlobKind::Journal, mine.storeId(), name};
    if (!cipher_->open(blob, aad, &plain, error)) return false;

    // Тот же путь, что у синка: чужие байты принимаются, только доказав себя;
    // ожидаемое состояние — хеш нынешних байтов (обычно журнала ещё нет).
    QByteArray current;
    if (!readJournalBytes(id, &current, error)) return false;
    // Конвенция стража та же, что у движка: «журнала нет» — пустой отпечаток.
    const Digest seen = current.isEmpty() ? Digest() : hashBytes(current);
    if (!adoptJournalBytes(id, plain, seen, error)) return false;

    ZJournal frames;
    if (!readJournal(id, &frames, error)) return false;
    const int head = frames.headIndex();
    if (head < 0 || !frames.at(head).hasSnapshot()) return true;  // нечего показывать
    QByteArray snap;
    if (!journalSnapshot(id, head, &snap, error)) return false;
    if (hashBytes(snap) != frames.at(head).digest()) {
        if (error) *error = QStringLiteral("head snapshot of %1 does not match its digest").arg(id);
        return false;
    }
    return writeFileBytes(pathOf(id), std::string(snap.constData(), size_t(snap.size())), error);
}

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
    const auto note = [&](const QString& what) {
        if (options.logs != nullptr) options.logs->sync(what);
    };
    const auto complain = [&](const QString& what) {
        ++done.integrityFailures;
        fprintf(stderr, "zametti sync: %s\n", qPrintable(what));
        if (options.logs != nullptr) {
            options.logs->sync(QStringLiteral("ERROR: ") + what);
            options.logs->err(QStringLiteral("sync: ") + what);
        }
    };
    note(QStringLiteral("start %1, store %2")
             .arg(options.mode == SyncOptions::PushOnly ? QStringLiteral("push-only")
                                                        : QStringLiteral("full"),
                  root_));

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
                note(QStringLiteral("align: baselined %1").arg(id));
                continue;
            }
            const int head = frames.headIndex();
            if (head >= 0 && frames.at(head).hasSnapshot() &&
                frames.at(head).digest() == fileHash)
                continue;  // выровнено
            if (head >= 0 && !frames.at(head).hasSnapshot()) {
                // Голова — надгробие, а файл ещё лежит. Если он равен
                // последнему слепку, это НЕ правка, а не материализованное
                // пока удаление (например, задержанное предохранителем) —
                // дописать external значило бы воскресить заметку без
                // человека. Правкой считается только файл, ушедший от
                // последнего слепка.
                const int last = frames.lastSnapshotIndex();
                if (last >= 0 && frames.at(last).digest() == fileHash) continue;
            }
            // Файл разошёлся с головой (правка снаружи, оборванное сохранение,
            // файл поверх надгробия) — дописать external: правка побеждает.
            if (!journalFor(id, options.journalRules)
                     ->record(ZJournal::Kind::External, bytes, &why)) {
                complain(QStringLiteral("cannot record external edit of %1: %2").arg(id, why));
                continue;
            }
            ++done.externalRecorded;
            note(QStringLiteral("align: external edit recorded for %1").arg(id));
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
                        if (error != nullptr) {
                            const QString cloudName =
                                cipher_ ? cloudStoreName(*remote_, *cipher_, theirs)
                                        : QString();
                            *error =
                                QStringLiteral(
                                    "this cloud folder now belongs to another store — %1 — "
                                    "refusing%2")
                                    .arg(storeTag(theirs.storeId(), cloudName,
                                                  theirs.created()),
                                         QLatin1String(kAddressHint));
                        }
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
    // Keyfile-чек — ранний сигнал ротации. «Изменился» значит «не совпал с
    // записанным», ВКЛЮЧАЯ пустую запись: бухгалтерия — кэш (инвариант D), и
    // её потеря не вправе превратить ротацию в «порчу», которую лечили бы
    // перезаливкой старым ключом. Цена — одна проба на первом контакте.
    bool keyfileChanged = false;
    {
        const QString name = QLatin1String(Keyfile::kRemoteName);
        SyncLedger::Blob led = ledger.blob(name);
        const QString etag = remoteEtag.value(name);
        keyfileChanged = !etag.isEmpty() && etag != led.etag;
        if (!etag.isEmpty() && led.etag != etag) {
            led.etag = etag;
            ledger.setBlob(name, led);
        }
    }

    // РОТАЦИЯ ЛОВИТСЯ ДО ЕДИНОЙ ЗАЛИВКИ. Сменившийся keyfile — ранний
    // сигнал, но верить ему на слово нельзя (смена ПАРОЛЯ перезаписывает
    // keyfile, не меняя ключа), а ждать первого AEAD-отказа — поздно: новая
    // заметка, блоба которой в облаке нет, успела бы уехать СТАРЫМ ключом и
    // застрять нечитаемой навсегда (её собственный ярус 1 дальше пропускал бы
    // блоб как «залитый»). Потому проба: один чужой журнал вскрывается ЗДЕСЬ,
    // до цикла. Не вскрылся — честная остановка без единой записи.
    if (keyfileChanged) {
        QString probeName;
        for (const RemoteStore::Entry& e : listing)
            if (e.name.endsWith(QStringLiteral(".log"))) {
                probeName = e.name;
                break;
            }
        if (!probeName.isEmpty()) {
            QByteArray blob;
            QByteArray plain;
            QString why;
            const BlobAad aad{BlobKind::Journal, storeId, probeName};
            if (remote_->get(probeName, &blob, nullptr, &why) &&
                !cipher_->open(blob, aad, &plain, &why)) {
                if (error != nullptr)
                    *error = QStringLiteral(
                        "the cloud key was rotated on another device — enter the password "
                        "again (set-remote)");
                complain(QStringLiteral(
                    "key probe failed on %1 — the cloud key was rotated, stopping before "
                    "any upload").arg(probeName));
                --done.integrityFailures;  // остановка названа в error, не провал блоба
                takeTraffic();
                return finish(false);
            }
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
        return pushOnly && wholeRun.elapsed() >= qint64(options.exitPushBudgetSec) * 1000;
    };

    QStringList processed;           // с кого снять dirty-пометку
    QSet<QString> touched;           // у кого голова могла смениться (материализация)
    QHash<QString, Digest> oldHead;  // digest головы ДО обмена — двойное условие
    int aeadFailures = 0;

    QStringList orderedJournals(journalIds.begin(), journalIds.end());
    orderedJournals.sort();
    if (options.progressTotal != nullptr)
        options.progressTotal->store(int(orderedJournals.size()));
    const auto bumpProgress = [&] {
        if (options.progressDone != nullptr) options.progressDone->fetch_add(1);
    };
    for (const QString& id : orderedJournals) {
        if (cancelled()) {
            done.cancelled = true;
            break;
        }
        bumpProgress();
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
                note(QStringLiteral("local journal of %1 fails validation — treated as "
                                    "absence, uploading nothing")
                         .arg(id));
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
                if (!led.etag.isEmpty()) {
                    ++done.healedRemote;
                    note(QStringLiteral("healing: %1 vanished from the cloud, re-uploading")
                             .arg(name));
                }
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
            // Перевыдача метки — короткий путь только для ЖИВОГО локального:
            // при absence содержимое всё равно нужно, чтобы вылечиться.
            if (localValid && !led.sealedHash.empty() && sealed == led.sealedHash) {
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
                        note(QStringLiteral("taken whole: %1").arg(name));
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
                            note(QStringLiteral("taken whole (superset): %1").arg(name));
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
                            note(QStringLiteral("merged %1: +%2 theirs, %3 common, %4 voided")
                                     .arg(name)
                                     .arg(st.fromOther)
                                     .arg(st.common)
                                     .arg(st.voidedDropped));
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
        if (options.progressTotal != nullptr)
            options.progressTotal->fetch_add(int(orderedAttachments.size()));
        for (const QString& name : orderedAttachments) {
            if (cancelled()) {
                done.cancelled = true;
                break;
            }
            bumpProgress();
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
            done.materializedIds.append(id);
            const QFileInfo info(path);
            ledger.setFileStat(id, {info.lastModified().toMSecsSinceEpoch(), info.size()});
        }

        // ПРЕДОХРАНИТЕЛЬ МАССОВОГО УДАЛЕНИЯ (порог владельца). Сверх порога —
        // ни одного удаления; список уходит в отчёт, решает человек:
        // подтвердил → повторный прогон с allowMassDelete; отказал →
        // declareAlive, и облако лечится записями поверх надгробий.
        if (int(deletes.size()) > options.deleteGuard && !options.allowMassDelete) {
            done.pendingDeletes = deletes;
            // Пометить, чтобы следующий прогон — с подтверждением или после
            // declareAlive — вернулся к этим заметкам, даже когда обмен для
            // них снова бесплатен.
            for (const QString& id : deletes) markDirty(id);
        } else {
            for (const QString& id : deletes) {
                const QString path = pathOf(id);
                if (!QFile::moveToTrash(path) && !QFile::remove(path)) {
                    complain(QStringLiteral("cannot remove %1 for its tombstone").arg(id));
                    continue;
                }
                ++done.deletesApplied;
                done.deletedIds.append(id);
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
    // Задержанные предохранителем — ИСКЛЮЧЕНИЕ: их пометка и есть память
    // «вопрос не решён», следующий прогон обязан к ним вернуться.
    if (!done.cancelled) {
        QStringList settled = processed;
        for (const QString& id : done.pendingDeletes) settled.removeAll(id);
        clearDirty(settled);
    }
    ledger.setCleanShutdown(true);
    QString ledgerWhy;
    if (!ledger.save(&ledgerWhy))
        fprintf(stderr, "zametti sync: cannot save the ledger: %s\n", qPrintable(ledgerWhy));
    takeTraffic();

    note(QStringLiteral(
             "done: listed %1, skipped %2, taken %3, pushed %4, merged %5, deferred %6, "
             "healed %7, materialized %8, deletes %9/%10 pending, attachments %11 up %12 "
             "down, failures %13; traffic %14 req, %15 B up, %16 B down")
             .arg(done.listed)
             .arg(done.skipped)
             .arg(done.takenWhole)
             .arg(done.pushedWhole)
             .arg(done.mergedJournals)
             .arg(done.deferred)
             .arg(done.healedRemote)
             .arg(done.materialized)
             .arg(done.deletesApplied)
             .arg(done.pendingDeletes.size())
             .arg(done.attachmentsUp)
             .arg(done.attachmentsDown)
             .arg(done.integrityFailures)
             .arg(done.traffic.requests)
             .arg(done.traffic.bytesUp)
             .arg(done.traffic.bytesDown));
    if (done.cancelled) note(QStringLiteral("cancelled by the user"));
    if (!done.pendingDeletes.isEmpty())
        note(QStringLiteral("mass-delete guard held %1 notes: %2")
                 .arg(done.pendingDeletes.size())
                 .arg(done.pendingDeletes.join(QLatin1Char(' '))));

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
            if (error != nullptr) {
                auto probe = XChaChaCipher::make(keyfile, nullptr);
                const QString cloudName =
                    probe ? cloudStoreName(*remote, *probe, theirs) : QString();
                *error = QStringLiteral(
                             "this cloud folder belongs to another store — %1 — and "
                             "this store is %2%3")
                             .arg(storeTag(theirs.storeId(), cloudName, theirs.created()),
                                  storeTag(mine.storeId(), localStoreName(), mine.created()),
                                  QLatin1String(kAddressHint));
            }
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
