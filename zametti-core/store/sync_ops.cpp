// Глаголы облака: подключение и заливка (m17, сессия 3).
//
// ЕДИНСТВЕННОЕ МЕСТО, где файлы хранилища встречаются с именами блобов и с
// AAD. Адаптер (CloudStore) возит байты и о заметках не знает; шифр
// (BlobCipher) знает только про AAD; хранилище знает файлы. Здесь они
// сходятся, и больше нигде — иначе «как зовётся блоб этой картинки» имело бы
// два ответа.
//
// Раскладка в облаке плоская и открытая (id непрозрачны):
//
//   zametti.json    манифест, ОТКРЫТЫМ ТЕКСТОМ: по нему сверяется storeId
//                   ДО ввода пароля и до расшифровки хоть чего-нибудь;
//   keyfile         конверт мастер-ключа (тоже открыто, см. справочник);
//   <id>.zm         журнал заметки, зашифрован;
//   <id>_<ext>.pic  вложение, зашифровано; настоящее расширение — в имени.
//
// Расширения СВОИ (решение владельца 28.08.2026): шифроблоб под настоящим
// расширением картинки сервисы принимали за битую картинку. Наследные имена
// (<id>.log, <id>.<ext>) читаются, пока существуют, и удаляются после
// заливки новых; AAD включает имя, так что на сервере блоб не переименовать
// — только пере-запечатать своими байтами.

#include "zstorage.h"

#include "folder_cloud.h"
#include "keyfile.h"
#include "sync_ledger.h"
#include "zlogs.h"
#include "note_id.h"
#include "secret_store.h"
#include "times.h"
#include "webdav_cloud.h"

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
    return name.endsWith(QStringLiteral(".zm")) || name.endsWith(QStringLiteral(".log"))
               ? BlobKind::Journal
               : BlobKind::Attachment;
}

// --- облачные имена ----------------------------------------------------------
//
// Пара «локальное имя ↔ облачное» живёт ЗДЕСЬ и только здесь: заливка, синк,
// лечение и сброс ходят через эти функции, иначе наследные имена возвращались
// бы то одним путём, то другим.
QString cloudJournalName(const QString& id) { return id + QStringLiteral(".zm"); }

QString cloudAttachmentName(const QString& localName) {
    const qsizetype dot = localName.lastIndexOf(QLatin1Char('.'));
    return localName.left(dot) + QLatin1Char('_') + localName.mid(dot + 1) +
           QStringLiteral(".pic");
}

// Локальное имя из облачного `<id>_<ext>.pic`; пусто — имя не наше.
QString localAttachmentName(const QString& cloudName) {
    if (!cloudName.endsWith(QStringLiteral(".pic"))) return {};
    const QString stem = cloudName.left(cloudName.size() - 4);
    const qsizetype under = stem.lastIndexOf(QLatin1Char('_'));
    if (under <= 0) return {};
    const QString id = stem.left(under);
    const QString ext = stem.mid(under + 1);
    if (ext.isEmpty() || !isValidNoteId(id.toStdString())) return {};
    return id + QLatin1Char('.') + ext;
}

// Основа-id облачного журнального имени (`<id>.zm`, наследное `<id>.log`);
// пусто — не журнал.
QString journalStemOf(const QString& cloudName) {
    QString stem;
    if (cloudName.endsWith(QStringLiteral(".zm")))
        stem = cloudName.left(cloudName.size() - 3);
    else if (cloudName.endsWith(QStringLiteral(".log")))
        stem = cloudName.left(cloudName.size() - 4);
    else
        return {};
    return isValidNoteId(stem.toStdString()) ? stem : QString();
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

constexpr char kAddressHint[] = "; check the cloud address (set-cloud --url/--to)";

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

// Один читатель на оба дома записи (cloud.json и строка списка в state.json):
// чего в объекте нет, то пусто. Прежние ключи remote*/url/dir/user читаются
// запасным путём — старые cloud.json/remote.json продолжают работать и
// мигрируют при следующей записи.
void ZStorage::Config::parse(const QJsonObject& o) {
    const auto text = [&o](std::initializer_list<const char*> keys) {
        for (const char* key : keys) {
            const QJsonValue v = o.value(QLatin1String(key));
            if (v.isString()) return v.toString();
        }
        return QString();
    };
    root = text({"root"});
    name = text({"name"});
    cloudUrl = text({"cloudUrl", "remoteUrl", "url"});
    cloudDir = text({"cloudDir", "remoteDir", "dir"});
    cloudUser = text({"cloudUser", "remoteUser", "user"});
    timeoutMs = o.value(QStringLiteral("timeoutMs")).toInt(30000);
}

bool ZStorage::Config::parse(const QByteArray& bytes, QString* error) {
    QJsonParseError bad;
    const QJsonDocument doc = QJsonDocument::fromJson(bytes, &bad);
    if (bad.error != QJsonParseError::NoError || !doc.isObject()) {
        if (error) *error = QStringLiteral("cloud.json is not a JSON object");
        return false;
    }
    parse(doc.object());
    return true;
}

// В cloud.json уходит ТОЛЬКО облачная сторона: файл лежит В корне копии, и
// путь, вписанный внутрь, протух бы при cp -r.
QByteArray ZStorage::Config::cloudConfigBytes() const {
    QJsonObject o;
    if (!cloudUrl.isEmpty()) o.insert(QStringLiteral("cloudUrl"), cloudUrl);
    if (!cloudDir.isEmpty()) o.insert(QStringLiteral("cloudDir"), cloudDir);
    if (!cloudUser.isEmpty()) o.insert(QStringLiteral("cloudUser"), cloudUser);
    o.insert(QStringLiteral("timeoutMs"), timeoutMs);
    return QJsonDocument(o).toJson(QJsonDocument::Indented);
}

QJsonObject ZStorage::Config::entryJson() const {
    QJsonObject o;
    if (!root.isEmpty()) o.insert(QStringLiteral("root"), root);
    if (!name.isEmpty()) o.insert(QStringLiteral("name"), name);
    if (!cloudUrl.isEmpty()) o.insert(QStringLiteral("cloudUrl"), cloudUrl);
    if (!cloudDir.isEmpty()) o.insert(QStringLiteral("cloudDir"), cloudDir);
    if (!cloudUser.isEmpty()) o.insert(QStringLiteral("cloudUser"), cloudUser);
    o.insert(QStringLiteral("timeoutMs"), timeoutMs);
    return o;
}

ZStorage::Config ZStorage::cloudConfig() const {
    Config cfg;
    // Сперва новое имя, при его отсутствии — наследное remote.json: копии
    // владельца обновляются не в один день, и старая копия обязана читаться.
    // Мигрирует файл при следующей записи (writeCloudConfig).
    QFile f(root_ + QStringLiteral("/.zametti/cloud.json"));
    if (!f.exists()) f.setFileName(root_ + QStringLiteral("/.zametti/remote.json"));
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
    // root из него всегда пуст: в cloud.json путь не пишется — переехал бы
    // вместе с каталогом и врал). name не заполняется нарочно: заголовок
    // корня стоит чтения файла, а сюда ходят на каждый пересчёт тулбара.
    cfg.root = root_;
    return cfg;
}

bool ZStorage::writeCloudConfig(const Config& cfg, QString* error) {
    QDir().mkpath(root_ + QStringLiteral("/.zametti"));
    QSaveFile save(root_ + QStringLiteral("/.zametti/cloud.json"));
    if (!save.open(QIODevice::WriteOnly)) {
        if (error) *error = QStringLiteral("cannot write cloud.json: %1").arg(save.errorString());
        return false;
    }
    save.write(cfg.cloudConfigBytes());
    if (!save.commit()) {
        if (error) *error = QStringLiteral("cannot write cloud.json: %1").arg(save.errorString());
        return false;
    }
    // Наследный remote.json уходит ТЕМ ЖЕ шагом, каким записан новый: две
    // версии файла с одним смыслом — это два ответа на один вопрос.
    const QString legacy = root_ + QStringLiteral("/.zametti/remote.json");
    if (QFile::exists(legacy)) files().removeForever(legacy);
    return true;
}

bool ZStorage::clearCloudConfig(QString* error) {
    QString why;
    bool ok = true;
    for (const char* name : {"/.zametti/cloud.json", "/.zametti/remote.json"}) {
        const QString path = root_ + QLatin1String(name);
        if (!QFile::exists(path)) continue;
        if (!files().removeForever(path, &why)) ok = false;
    }
    if (!ok && error) *error = QStringLiteral("cannot remove cloud.json: %1").arg(why);
    return ok;
}

std::shared_ptr<CloudStore> ZStorage::makeCloud(const Config& cfg,
                                                  const QString& serverPassword,
                                                  QString* error) {
    if (!cfg.hasCloudAddress()) {
        if (error) *error = QStringLiteral("sync is not configured");
        return nullptr;
    }
    if (!cfg.cloudUrl.isEmpty()) {
        WebDavCloud::Config web;
        web.base = QUrl(cfg.cloudUrl);
        web.user = cfg.cloudUser;
        web.password = serverPassword;
        web.timeoutMs = cfg.timeoutMs;
        // Адрес проверяется ДО первой операции: пароль не уедет открытым
        // текстом даже один раз.
        if (!WebDavCloud::checkUrl(web, error)) return nullptr;
        return std::make_shared<WebDavCloud>(web);
    }
    return std::make_shared<FolderCloud>(cfg.cloudDir);
}

bool ZStorage::cloudHasKeyfile(const Config& cfg, const QString& serverPassword,
                               QString* error) {
    auto cloud = makeCloud(cfg, serverPassword, error);
    if (!cloud) return false;
    QByteArray envelope;
    return cloud->get(QLatin1String(Keyfile::kCloudName), &envelope, nullptr, nullptr);
}

bool ZStorage::attachCloud(const AttachOptions& how, SecretStore& secrets,
                            AttachOutcome* outcome, QString* error) {
    if (outcome) *outcome = AttachOutcome{};
    // «Адрес назван» — это про облако: ключи командной строки сильнее
    // cloud.json, а root у обоих кандидатов и так этот.
    const Config cfg = how.cfg.hasCloudAddress() ? how.cfg : cloudConfig();
    if (!cfg.hasCloudAddress()) {
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
    if (password.isEmpty() && !cfg.cloudUrl.isEmpty())
        password = secrets.serverPassword(mine.storeId(), &why);
    auto cloud = makeCloud(cfg, password, error);
    if (!cloud) return false;

    Keyfile keyfile;
    if (!secrets.loadKey(mine.storeId(), &keyfile, &why)) {
        // Ключа под рукой нет. Дальше — только с паролем: без него ни конверт
        // не развернуть, ни новый ключ не отчеканить.
        QByteArray envelope;
        const bool haveEnvelope =
            !how.encryptionPassword.isEmpty() &&
            cloud->get(QLatin1String(Keyfile::kCloudName), &envelope, nullptr, nullptr);
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
            if (!cloud->mkdirOnce(error) ||
                !cloud->put(QLatin1String(Keyfile::kCloudName), keyfile.toBytes(), nullptr,
                             error))
                return false;
            if (outcome) outcome->mintedKeyfile = true;
        } else {
            // Не беда, а «настройся заново»: утраченный keyring лечится одним
            // вводом пароля (set-cloud), и это названный брифом случай.
            if (error)
                *error = QStringLiteral("the key is not in the keyring (%1) — run set-cloud "
                                        "once, or set ZAMETTI_SYNC_KEY / ZAMETTI_SYNC_PASSWORD")
                             .arg(why);
            return false;
        }
        // Пароль подошёл (конверт развернулся или отчеканен) — запомнить и
        // его: он для глаз человека (решение владельца 28.08.2026).
        QString keep;
        if (!secrets.setEncryptionPassword(mine.storeId(), how.encryptionPassword, &keep))
            fprintf(stderr, "zametti: the keyring refused the encryption password: %s\n",
                    qPrintable(keep));
    }
    return setCloud(cloud, keyfile, error);
}

std::shared_ptr<ZStorage> ZStorage::initFromCloud(
    const QString& root, const Config& cfg, const QString& encryptionPassword,
    const QString& serverPassword, SecretStore& secrets, const Keyfile::KdfParams& mintParams,
    ConnectOutcome* outcome, QString* error) {
    auto storage = std::make_shared<ZStorage>(root);
    if (!storage->connectCloud(cfg, encryptionPassword, serverPassword, secrets, mintParams,
                                outcome, error))
        return nullptr;
    return storage;
}

bool ZStorage::connectCloud(const Config& cfg, const QString& encryptionPassword,
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
    auto cloud = makeCloud(cfg, serverPassword, error);
    if (!cloud) return finish(false);

    // МАНИФЕСТ — ДО идентичности: бутстрап наследует id из облака, а не
    // чеканит свой (иначе два id на одно хранилище и молчаливый развод).
    QByteArray manifestBytes;
    QString why;
    Identity theirs;
    const bool haveManifest =
        cloud->get(QLatin1String(Identity::kFile), &manifestBytes, nullptr, &why);
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
            if (cloud->get(QLatin1String(Keyfile::kCloudName), &envelope, nullptr, &why) &&
                theirKeyfile.parse(envelope, nullptr) && !theirKeyfile.tooNew() &&
                theirKeyfile.unwrap(encryptionPassword, nullptr)) {
                if (auto probe = XChaChaCipher::make(theirKeyfile, nullptr))
                    cloudName = cloudStoreName(*cloud, *probe, theirs);
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
    if (cloud->get(QLatin1String(Keyfile::kCloudName), &keyfileBytes, nullptr, &why)) {
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
        if (!cloud->mkdirOnce(error)) return finish(false);
        if (!cloud->put(QLatin1String(Keyfile::kCloudName), keyfile.toBytes(), nullptr, error))
            return finish(false);
        done.mintedKeyfile = true;
    }

    if (!setCloud(cloud, keyfile, error)) return finish(false);

    // ЧТО ЛЕЖИТ В ОБЛАКЕ — человеку при настройке: один листинг, ноль
    // расшифровок. Имена открыты, поэтому «сколько заметок и картинок»
    // видно до всякого пароля; объём — по шифротексту.
    {
        QVector<CloudStore::Entry> listing;
        QString why;
        if (cloud->list(&listing, &why)) {
            // Счёт — УНИКАЛЬНЫМИ id: в окне миграции имя может лежать и
            // новым, и наследным, а двоить заметку человеку нельзя.
            QSet<QString> noteIds;
            QSet<QString> attachmentIds;
            for (const CloudStore::Entry& e : listing) {
                done.cloudBytes += e.size;
                const QString journal = journalStemOf(e.name);
                if (!journal.isEmpty()) {
                    noteIds.insert(journal);
                    continue;
                }
                if (e.name == QLatin1String(Identity::kFile) ||
                    e.name == QLatin1String(Keyfile::kCloudName))
                    continue;
                const QString viaNew = localAttachmentName(e.name);
                const QString local = !viaNew.isEmpty() ? viaNew : e.name;
                const qsizetype dot = local.lastIndexOf(QLatin1Char('.'));
                if (dot > 0 && isValidNoteId(local.left(dot).toStdString()))
                    attachmentIds.insert(local.left(dot));
            }
            done.cloudNotes = noteIds.size();
            done.cloudAttachments = attachmentIds.size();
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
    if (!cfg.cloudUrl.isEmpty() && !serverPassword.isEmpty() &&
        !secrets.setServerPassword(mine.storeId(), serverPassword, &keep))
        fprintf(stderr, "zametti: the keyring refused the server password: %s\n",
                qPrintable(keep));
    // Пароль шифрования — тоже в keyring (решение владельца 28.08.2026): он
    // для глаз человека, синк работает ключом. Кладётся ТОЛЬКО после удачи —
    // разворот или чеканка выше уже состоялись.
    if (!secrets.setEncryptionPassword(mine.storeId(), encryptionPassword, &keep))
        fprintf(stderr, "zametti: the keyring refused the encryption password: %s\n",
                qPrintable(keep));
    if (!writeCloudConfig(cfg, error)) return finish(false);
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
    auto cloud = makeCloud(cfg, serverPassword, error);
    if (!cloud) return finish(false);

    QVector<CloudStore::Entry> listing;
    if (!cloud->list(&listing, error)) return finish(false);
    {
        // Счёт уникальными id — как в connectCloud: окно миграции держит имя
        // и новым, и наследным.
        QSet<QString> noteIds;
        QSet<QString> attachmentIds;
        for (const CloudStore::Entry& e : listing) {
            probe.bytes += e.size;
            // Самая свежая метка листинга — «когда облако правили» для сводки.
            if (e.lastModified.isValid() &&
                (!probe.lastModified.isValid() || e.lastModified > probe.lastModified))
                probe.lastModified = e.lastModified;
            if (e.name == QLatin1String(Identity::kFile)) {
                probe.hasManifest = true;
                continue;
            }
            if (e.name == QLatin1String(Keyfile::kCloudName)) {
                probe.hasKeyfile = true;
                continue;
            }
            const QString journal = journalStemOf(e.name);
            if (!journal.isEmpty()) {
                noteIds.insert(journal);
                continue;
            }
            const QString viaNew = localAttachmentName(e.name);
            const QString local = !viaNew.isEmpty() ? viaNew : e.name;
            const qsizetype dot = local.lastIndexOf(QLatin1Char('.'));
            if (dot > 0 && isValidNoteId(local.left(dot).toStdString()))
                attachmentIds.insert(local.left(dot));
        }
        probe.notes = noteIds.size();
        probe.attachments = attachmentIds.size();
    }

    QString why;
    if (probe.hasManifest) {
        QByteArray manifestBytes;
        if (!cloud->get(QLatin1String(Identity::kFile), &manifestBytes, nullptr, &why)) {
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

    // ПУСТОЙ ПАРОЛЬ — НЕ ПОПЫТКА, А ОТКАЗ ОТ НЕЁ (п.9 брифа 30.08.2026):
    // сводка облака доступна без пароля шифрования — имена открыты, манифест
    // открыт, — и Check обязан работать без него. Конверт тогда просто не
    // разворачивается: keyOpened ложь, имени хранилища честно нет.
    if (probe.hasKeyfile && !encryptionPassword.isEmpty()) {
        QByteArray envelope;
        Keyfile keyfile;
        if (!cloud->get(QLatin1String(Keyfile::kCloudName), &envelope, nullptr, &why)) {
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
                probe.name = cloudStoreName(*cloud, *cipher, probe.identity);
        }
    }
    return finish(true);
}

// --- ТРИ ДВЕРИ ОБЛАКА (§2.6): стереть, засеять, сменить пароль --------------

namespace {

// Имя нашей формы: голова (манифест, конверт), журнал или вложение. По ним
// узнаётся оборванное прежнее стирание — его можно дострать; всё прочее —
// постороннее, и его не трогают.
bool looksLikeOurBlob(const QString& name) {
    if (name == QLatin1String(ZStorage::Identity::kFile) ||
        name == QLatin1String(Keyfile::kCloudName))
        return true;
    if (!journalStemOf(name).isEmpty()) return true;
    if (!localAttachmentName(name).isEmpty()) return true;
    // Наследное имя вложения: <id>.<ext>.
    const qsizetype dot = name.lastIndexOf(QLatin1Char('.'));
    return dot > 0 && isValidNoteId(name.left(dot).toStdString());
}

}  // namespace

bool ZStorage::eraseCloudStorage(const Config& cfg, const QString& serverPassword,
                                 bool keepFolder, EraseOutcome* outcome,
                                 QString* error) {
    EraseOutcome done;
    const auto finish = [&](bool ok) {
        if (outcome != nullptr) *outcome = done;
        return ok;
    };
    if (!store_) {
        if (error) *error = QStringLiteral("not a store: %1").arg(root_);
        return finish(false);
    }
    // Идентичность — ЧТЕНИЕМ, без чеканки: стирать вправе только то, что
    // доказанно наше, а хранилищу без zametti.json доказывать нечем.
    QString why;
    const Identity mine = identity(&why);
    if (mine.isEmpty()) {
        if (error)
            *error = QStringLiteral(
                "this store has no identity — refusing to erase any cloud for it");
        return finish(false);
    }
    // КАТАЛОГ-ОБЛАКО: только абсолютный путь (урок 30.08.2026 — «../..»,
    // резолвленное по cwd, стоило владельцу каталога) и не предок корня
    // хранилища, дома или cwd: даже совпавший манифест не даёт права стирать
    // файлы там, где живёт всё остальное.
    if (!cfg.cloudDir.isEmpty()) {
        if (QDir::isRelativePath(cfg.cloudDir)) {
            if (error)
                *error = QStringLiteral(
                             "the cloud folder must be an absolute path, got \"%1\"")
                             .arg(cfg.cloudDir);
            return finish(false);
        }
        const QString dir = QDir::cleanPath(cfg.cloudDir) + QLatin1Char('/');
        for (const QString& fort :
             {QDir(root_).absolutePath(), QDir::homePath(), QDir::currentPath()}) {
            if ((QDir::cleanPath(fort) + QLatin1Char('/')).startsWith(dir)) {
                if (error)
                    *error = QStringLiteral(
                                 "the cloud folder \"%1\" contains \"%2\" — refusing to "
                                 "erase it")
                                 .arg(QDir::cleanPath(cfg.cloudDir), fort);
                return finish(false);
            }
        }
    }
    auto cloud = makeCloud(cfg, serverPassword, error);
    if (!cloud) return finish(false);
    // Каталог заводится ДО листинга: стирание при пустом (или ещё не
    // существующем) облаке — законная часть сброса, и заодно первая проверка
    // адреса и пароля сервера.
    if (!cloud->mkdirOnce(error)) return finish(false);

    // ЧУЖОЕ НЕ СТИРАЕТСЯ. Манифест есть — он обязан быть НАШИМ (нечитаемый —
    // отказ: непонятно чьё стирать нельзя). Манифеста нет — «безымянное» НЕ
    // значит «наше» (та дыра и снесла /Users/…/work): стирание разрешено,
    // только когда КАЖДОЕ имя в листинге — нашей формы, как выглядит
    // оборванное прежнее стирание. Один посторонний файл — отказ.
    QVector<CloudStore::Entry> listing;
    if (!cloud->list(&listing, error)) return finish(false);
    QByteArray manifestBytes;
    if (cloud->get(QLatin1String(Identity::kFile), &manifestBytes, nullptr, &why)) {
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
    } else {
        for (const CloudStore::Entry& e : listing) {
            if (looksLikeOurBlob(e.name)) continue;
            if (error)
                *error = QStringLiteral(
                             "\"%1\" holds \"%2\", which is not a zametti blob — "
                             "refusing to erase a folder that is not provably ours")
                             .arg(cfg.cloudUrl.isEmpty() ? cfg.cloudDir : cfg.cloudUrl,
                                  e.name);
            return finish(false);
        }
    }
    done.wiped = int(listing.size());

    // Само стирание — одним жестом (WebDAV: один DELETE по коллекции).
    if (!cloud->removeTree(error)) return finish(false);
    if (keepFolder && !cloud->mkdirOnce(error)) return finish(false);

    // Бухгалтерия синка — про блобы, которых больше нет: пусть следующий
    // прогон построит её заново, это кэш, а не истина. Лежит она в каталоге
    // ХОЗЯЙСТВА, а не в корне хранилища, — и область у неё поэтому своя.
    const QString ledger = SyncLedger::pathFor(mine.storeId(), root_);
    ZSystem(ZSystem::Area::Config, QFileInfo(ledger).absolutePath()).removeForever(ledger);
    // Стёртое облако больше не подключено: адаптер держал бы мёртвый адрес.
    dropCloud();
    return finish(true);
}

bool ZStorage::initCloudStorage(const Config& cfg, const QString& password,
                                const QString& serverPassword, SecretStore& secrets,
                                const Keyfile::KdfParams& mintParams, QString* error) {
    if (!store_) {
        if (error) *error = QStringLiteral("not a store: %1").arg(root_);
        return false;
    }
    if (password.isEmpty()) {
        if (error) *error = QStringLiteral("the encryption password must not be empty");
        return false;
    }
    const Identity mine = ensureIdentity(error);
    if (mine.isEmpty()) return false;
    auto cloud = makeCloud(cfg, serverPassword, error);
    if (!cloud) return false;
    if (!cloud->mkdirOnce(error)) return false;

    // Живое облако не засеивают: существующий конверт — чьи-то данные, и
    // молча заслонить его новым значило бы отрезать их навсегда.
    QByteArray envelope;
    QString why;
    if (cloud->get(QLatin1String(Keyfile::kCloudName), &envelope, nullptr, &why)) {
        if (error)
            *error = QStringLiteral(
                "the cloud already holds a keyfile — connect to it, or erase it first");
        return false;
    }
    // Чужой манифест поверх пустого конверта — та же честная остановка.
    QByteArray manifestBytes;
    if (cloud->get(QLatin1String(Identity::kFile), &manifestBytes, nullptr, &why)) {
        Identity theirs;
        if (theirs.parse(manifestBytes, &why) && theirs.storeId() != mine.storeId()) {
            if (error)
                *error = QStringLiteral(
                             "this cloud folder belongs to another store — %1 — and this "
                             "store is %2%3")
                             .arg(storeTag(theirs.storeId(), QString(), theirs.created()),
                                  storeTag(mine.storeId(), localStoreName(), mine.created()),
                                  QLatin1String(kAddressHint));
            return false;
        }
    }

    Keyfile keyfile;
    if (!Keyfile::create(mine.storeId(), password, mintParams, &keyfile, error))
        return false;
    // ОДИН PUT: конверт. Манифест НЕ заливается — его несёт заливка данных
    // (pushAll/sync, манифест последним): сказать «здесь лежит хранилище»
    // раньше, чем оно там лежит, значило бы соврать при обрыве.
    if (!cloud->put(QLatin1String(Keyfile::kCloudName), keyfile.toBytes(), nullptr, error))
        return false;
    if (!setCloud(cloud, keyfile, error)) return false;

    // Бухгалтерия обнуляется: прежние метки — про блобы прежнего облака.
    const QString ledger = SyncLedger::pathFor(mine.storeId(), root_);
    ZSystem(ZSystem::Area::Config, QFileInfo(ledger).absolutePath()).removeForever(ledger);

    // Запомнить, как в connectCloud: отказ keyring засев не валит — облако
    // уже живое, просто следующий старт снова спросит пароль.
    QString keep;
    if (!secrets.storeKey(keyfile, &keep))
        fprintf(stderr, "zametti: the keyring refused the key: %s\n", qPrintable(keep));
    if (!cfg.cloudUrl.isEmpty() && !serverPassword.isEmpty() &&
        !secrets.setServerPassword(mine.storeId(), serverPassword, &keep))
        fprintf(stderr, "zametti: the keyring refused the server password: %s\n",
                qPrintable(keep));
    if (!secrets.setEncryptionPassword(mine.storeId(), password, &keep))
        fprintf(stderr, "zametti: the keyring refused the encryption password: %s\n",
                qPrintable(keep));
    return writeCloudConfig(cfg, error);
}

bool ZStorage::changeEncryptionPassword(const Config& cfg, const QString& newPassword,
                                        const QString& serverPassword,
                                        SecretStore& secrets,
                                        const Keyfile::KdfParams& mintParams,
                                        QString* error) {
    if (!store_) {
        if (error) *error = QStringLiteral("not a store: %1").arg(root_);
        return false;
    }
    if (newPassword.isEmpty()) {
        if (error) *error = QStringLiteral("the encryption password must not be empty");
        return false;
    }
    QString why;
    const Identity mine = identity(&why);
    if (mine.isEmpty()) {
        if (error) *error = QStringLiteral("the store has no identity");
        return false;
    }
    // Живой ключ — из связки: он и есть то, чем облако открывается, и потому
    // смена пароля ничего не стирает. Ключа нет — этой дороги нет тоже.
    Keyfile keyfile;
    if (!secrets.loadKey(mine.storeId(), &keyfile, &why)) {
        if (error)
            *error = QStringLiteral(
                         "the key is not in the keyring (%1) — erase the cloud and set "
                         "a new password instead")
                         .arg(why);
        return false;
    }
    auto cloud = makeCloud(cfg, serverPassword, error);
    if (!cloud) return false;
    // Чужой конверт не перезаписывается: опечатка в адресе не должна стоить
    // кому-то его облака.
    QByteArray envelope;
    if (cloud->get(QLatin1String(Keyfile::kCloudName), &envelope, nullptr, &why)) {
        Keyfile theirs;
        if (theirs.parse(envelope, nullptr) && theirs.storeId() != mine.storeId()) {
            if (error)
                *error = QStringLiteral("the cloud keyfile belongs to store %1, and this "
                                        "store is %2%3")
                             .arg(storeTag(theirs.storeId(), QString(), theirs.created()),
                                  storeTag(mine.storeId(), localStoreName(), mine.created()),
                                  QLatin1String(kAddressHint));
            return false;
        }
    }
    // Тот же ключ — новый конверт: rewrap будит живой ключ без старого пароля.
    if (!keyfile.rewrap(QString(), newPassword, mintParams, error)) return false;
    if (!cloud->mkdirOnce(error)) return false;
    if (!cloud->put(QLatin1String(Keyfile::kCloudName), keyfile.toBytes(), nullptr, error))
        return false;

    QString keep;
    if (!secrets.setEncryptionPassword(mine.storeId(), newPassword, &keep))
        fprintf(stderr, "zametti: the keyring refused the encryption password: %s\n",
                qPrintable(keep));
    if (!cfg.cloudUrl.isEmpty() && !serverPassword.isEmpty() &&
        !secrets.setServerPassword(mine.storeId(), serverPassword, &keep))
        fprintf(stderr, "zametti: the keyring refused the server password: %s\n",
                qPrintable(keep));
    return writeCloudConfig(cfg, error);
}

// СБРОС ПАРОЛЯ ШИФРОВАНИЯ — композиция трёх ступеней (решение владельца:
// метод нужен, живёт для CLI). Прерывание безопасно: облако без манифеста —
// «первый синк», следующий прогон дольёт всё.
bool ZStorage::resetCloudEncryption(const Config& cfg, const QString& newPassword,
                                    const QString& serverPassword, SecretStore& secrets,
                                    const Keyfile::KdfParams& mintParams, ResetOutcome* outcome,
                                    QString* error) {
    ResetOutcome done;
    const auto finish = [&](bool ok) {
        if (outcome != nullptr) *outcome = done;
        return ok;
    };
    if (newPassword.isEmpty()) {
        if (error) *error = QStringLiteral("the encryption password must not be empty");
        return finish(false);
    }
    // Идентичность чеканится здесь (единственная из трёх ступеней): сброс на
    // хранилище без zametti.json — законный первый контакт с облаком.
    if (!store_ || ensureIdentity(error).isEmpty()) {
        if (error && error->isEmpty())
            *error = QStringLiteral("not a store: %1").arg(root_);
        return finish(false);
    }
    EraseOutcome erased;
    if (!eraseCloudStorage(cfg, serverPassword, /*keepFolder=*/true, &erased, error))
        return finish(false);
    done.wiped = erased.wiped;
    if (!initCloudStorage(cfg, newPassword, serverPassword, secrets, mintParams, error))
        return finish(false);
    if (!pushAll(&done.push, error)) return finish(false);
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
QString ZStorage::cloudStoreName(CloudStore& cloud, BlobCipher& cipher,
                                 const Identity& theirs) {
    if (theirs.rootNote().isEmpty()) return {};
    // Сперва новое имя, потом наследное: облако могло не мигрировать.
    QString name = cloudJournalName(theirs.rootNote());
    QByteArray blob;
    QString why;
    if (!cloud.get(name, &blob, nullptr, &why)) {
        name = theirs.rootNote() + QStringLiteral(".log");
        if (!cloud.get(name, &blob, nullptr, &why)) return {};
    }
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
    if (!isConnected()) {
        if (error) *error = QStringLiteral("no cloud is connected");
        return false;
    }
    const Identity mine = identity();
    // Сперва новое имя, потом наследное: облако могло не мигрировать.
    QString name = cloudJournalName(id);
    QByteArray blob;
    if (!cloud_->get(name, &blob, nullptr, error)) {
        name = id + QStringLiteral(".log");
        if (!cloud_->get(name, &blob, nullptr, error)) return false;
    }
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
    if (!isConnected()) {
        if (error != nullptr)
            *error = QStringLiteral("no cloud is connected — call setCloud first");
        return finish(false);
    }
    const Identity mine = ensureIdentity(error);
    if (mine.isEmpty()) return finish(false);
    const QString storeId = mine.storeId();

    const CloudStore::Traffic t0 = cloud_->traffic();
    const auto takeTraffic = [&] {
        const CloudStore::Traffic t1 = cloud_->traffic();
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
    // СЕТЬ МОГЛА ПРОСТО ПРОПАСТЬ — и это не провал (решение владельца:
    // телефонный интернет то есть, то нет). Обрыв передачи ОДНОГО блоба —
    // «отложено, вернёмся»: пометки живы, бухгалтерия не тронута, прогон
    // доделывает остальное и остаётся удачным, а окно повторит его само.
    // Провалом остаются целостность и локальные беды — их повтором не лечат.
    const auto skipTransfer = [&](const QString& what) {
        ++done.deferred;
        fprintf(stderr, "zametti sync: skipped (will retry): %s\n", qPrintable(what));
        note(QStringLiteral("skipped (will retry): ") + what);
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
        // Новые имена и наследные: CLI открывает хранилище без миграций.
        for (const QString& name :
             QDir(root_ + QStringLiteral("/history"))
                 .entryList({QStringLiteral("*.zm"), QStringLiteral("*.log")}, QDir::Files)) {
            const QString stem = journalStemOf(name);
            if (!stem.isEmpty()) out.append(stem);
        }
        out.sort();
        out.removeDuplicates();
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
    if (!cloud_->mkdirOnce(error)) return finish(false);
    QVector<CloudStore::Entry> listing;
    if (!cloud_->list(&listing, error)) {
        takeTraffic();
        return finish(false);
    }
    QHash<QString, QString> cloudEtag;
    for (const CloudStore::Entry& e : listing) cloudEtag.insert(e.name, e.etag);
    done.listed = int(listing.size());

    // Манифест — по etag ИЗ ТОГО ЖЕ листинга; GET только при расхождении.
    // Страж от переадресации МЕЖДУ прогонами: cloudDir мог смениться.
    {
        const QString name = QLatin1String(Identity::kFile);
        const QString etag = cloudEtag.value(name);
        SyncLedger::Blob led = ledger.blob(name);
        if (!etag.isEmpty() && etag != led.etag) {
            QByteArray bytes;
            QString why;
            if (cloud_->get(name, &bytes, nullptr, &why)) {
                Identity theirs;
                if (theirs.parse(bytes, &why)) {
                    if (theirs.storeId() != storeId) {
                        if (error != nullptr) {
                            const QString cloudName =
                                cipher_ ? cloudStoreName(*cloud_, *cipher_, theirs)
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
        const QString name = QLatin1String(Keyfile::kCloudName);
        SyncLedger::Blob led = ledger.blob(name);
        const QString etag = cloudEtag.value(name);
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
        for (const CloudStore::Entry& e : listing)
            if (!journalStemOf(e.name).isEmpty()) {
                probeName = e.name;
                break;
            }
        if (!probeName.isEmpty()) {
            QByteArray blob;
            QByteArray plain;
            QString why;
            const BlobAad aad{BlobKind::Journal, storeId, probeName};
            if (cloud_->get(probeName, &blob, nullptr, &why) &&
                !cipher_->open(blob, aad, &plain, &why)) {
                if (error != nullptr)
                    *error = QStringLiteral(
                        "the cloud key was rotated on another device — enter the password "
                        "again (set-cloud)");
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
    for (auto it = cloudEtag.constBegin(); it != cloudEtag.constEnd(); ++it) {
        const QString stem = journalStemOf(it.key());
        if (!stem.isEmpty()) journalIds.insert(stem);
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

    // Вложения считаются ЗДЕСЬ ЖЕ, до цикла журналов: у обоих множеств одни
    // входы (листинг облака и локальные имена), а итог индикатора обязан
    // стоять с первой секунды — владелец видел «285», по ходу превращающиеся
    // в «297», и растущий итог читается как враньё счётчика.
    QSet<QString> attachmentSet;
    if (!pushOnly) {
        for (const QString& name : attachmentNames()) attachmentSet.insert(name);
        for (auto it = cloudEtag.constBegin(); it != cloudEtag.constEnd(); ++it) {
            const QString& name = it.key();
            if (name == QLatin1String(Identity::kFile) ||
                name == QLatin1String(Keyfile::kCloudName) ||
                !journalStemOf(name).isEmpty())
                continue;
            // Ключ множества — ЛОКАЛЬНОЕ имя: новое облачное приводится к
            // нему, наследное с ним совпадает.
            const QString viaNew = localAttachmentName(name);
            if (!viaNew.isEmpty()) {
                attachmentSet.insert(viaNew);
                continue;
            }
            const qsizetype dot = name.lastIndexOf(QLatin1Char('.'));
            if (dot <= 0 || !isValidNoteId(name.left(dot).toStdString())) continue;
            attachmentSet.insert(name);
        }
    }
    QStringList orderedAttachments(attachmentSet.begin(), attachmentSet.end());
    orderedAttachments.sort();

    QStringList orderedJournals(journalIds.begin(), journalIds.end());
    orderedJournals.sort();
    if (options.progressTotal != nullptr)
        options.progressTotal->store(int(orderedJournals.size()) +
                                     int(orderedAttachments.size()));
    const auto bumpProgress = [&] {
        if (options.progressDone != nullptr) options.progressDone->fetch_add(1);
    };

    // ПРЕДЗАГРУЗКА ЖУРНАЛОВ ПАКЕТАМИ. Кандидат на GET виден заранее: облачная
    // метка разошлась с бухгалтерией (или бухгалтерии нет вовсе — первая
    // загрузка). Судьбу блоба по-прежнему решает цикл ниже теми же правилами;
    // здесь только байты — чтобы канал не простаивал по кругу «запрос —
    // ответ» (~0.6 с на запрос к живому серверу, замерено 28.08.2026). Окно
    // в kPrefetchBatch имён потребляется по ходу цикла (порядок совпадает),
    // взятое стирается — память не копит всё облако. Вложения пакетом не
    // ходят: их мало, они большие, и им хватает сторожа на каждом.
    QStringList prefetchQueue;
    if (!pushOnly) {
        for (const QString& id : orderedJournals) {
            const QString legacy = id + QStringLiteral(".log");
            const QString name =
                cloudEtag.contains(legacy) ? legacy : cloudJournalName(id);
            if (!cloudEtag.contains(name)) continue;
            const SyncLedger::Blob led = ledger.blob(name);
            if (led.etag.isEmpty() || led.etag != cloudEtag.value(name))
                prefetchQueue.append(name);
        }
    }
    int prefetchNext = 0;
    QHash<QString, CloudStore::Fetched> prefetched;
    constexpr int kPrefetchBatch = 32;
    const auto fetchBlob = [&](const QString& name, QByteArray* blob, QString* why) {
        if (!prefetched.contains(name) && prefetchNext < int(prefetchQueue.size()) &&
            !cancelled()) {
            const int at = int(prefetchQueue.indexOf(name, prefetchNext));
            if (at >= 0) {
                // Догрузить окно, имя — включительно.
                const int upto = qMin(int(prefetchQueue.size()),
                                      qMax(at + 1, prefetchNext + kPrefetchBatch));
                QStringList batch;
                for (int i = prefetchNext; i < upto; ++i)
                    batch.append(prefetchQueue.at(i));
                prefetchNext = upto;
                cloud_->getMany(batch, &prefetched);
            }
        }
        const auto it = prefetched.find(name);
        if (it != prefetched.end()) {
            const bool ok = it->ok;
            *blob = it->bytes;
            if (!ok && why != nullptr) *why = it->error;
            prefetched.erase(it);  // взятое стирается: память не копит облако
            return ok;
        }
        return cloud_->get(name, blob, nullptr, why);
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
        // Читаем НАСЛЕДНОЕ имя, пока оно есть (в нём может быть то, чего в
        // новом ещё нет), пишем ВСЕГДА новое; наследное удаляется после
        // удачной заливки. Обрыв между «залил» и «удалил» безопасен: union
        // записей при следующем слиянии ничего не теряет, доудалит следующий
        // прогон.
        const QString pushName = cloudJournalName(id);
        const QString legacyName = id + QStringLiteral(".log");
        const bool haveLegacy = cloudEtag.contains(legacyName);
        const QString name = haveLegacy ? legacyName : pushName;
        QByteArray localBytes;
        if (!readJournalBytes(id, &localBytes, error)) {
            takeTraffic();
            return finish(false);
        }
        SyncLedger::Blob led = ledger.blob(name);
        const bool haveCloud = cloudEtag.contains(name);
        const QString etag = cloudEtag.value(name);
        Digest localHash = localBytes.isEmpty() ? Digest() : hashBytes(localBytes);

        // ЯРУС 1: обе стороны там же, где были, — пропуск, ноль трафика.
        if (haveCloud && !led.etag.isEmpty() && led.etag == etag && !localBytes.isEmpty() &&
            localHash == led.plainHash) {
            if (!haveLegacy) {
                ++done.skipped;
                processed.append(id);
                continue;
            }
            // Стороны совпадают, но блоб под наследным именем: пропуску не
            // бывать — ниже сработает миграция (то же содержимое, новое имя).
        }
        if (!haveCloud && localBytes.isEmpty()) continue;

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
        const bool theyChanged = haveCloud && led.etag != etag;

        bool needPush = false;
        QByteArray pushBytes;
        // Ожидаемая метка — у ИМЕНИ ЗАЛИВКИ: при наследном чтении это другое
        // имя, и у него может быть свой блоб (обрыв прежней миграции).
        QString pushExpectedEtag = cloudEtag.value(pushName);

        if (!haveCloud) {
            // На сервере блоба нет. При записанном etag это повреждение
            // сервера — перезаливка-лечение; в push-only откладывается.
            if (localValid) {
                if (!led.etag.isEmpty()) {
                    ++done.healedCloud;
                    note(QStringLiteral("healing: %1 vanished from the cloud, re-uploading")
                             .arg(name));
                }
                needPush = true;
                pushBytes = localBytes;
                pushExpectedEtag.clear();
                if (led.etag.isEmpty() && localBytes.size() > 0) ++done.pushedWhole;
            }
        }

        if (haveCloud && (theyChanged || led.etag.isEmpty() || !localValid)) {
            if (pushOnly) {
                // Выход не скачивает и не сливает: отложено полному прогону.
                ++done.deferred;
                continue;
            }
            // ЯРУС 2: GET; хеш шифротекста прежний — это перевыдача etag.
            QByteArray blob;
            QString why;
            if (!fetchBlob(name, &blob, &why)) {
                skipTransfer(QStringLiteral("cannot download %1: %2").arg(name, why));
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
                QByteArray cloudPlain;
                const BlobAad aad{BlobKind::Journal, storeId, name};
                if (!cipher_->open(blob, aad, &cloudPlain, &why)) {
                    ++aeadFailures;
                    if (keyfileChanged) {
                        // Ротация ключа с другого устройства — не порча.
                        if (error != nullptr)
                            *error = QStringLiteral(
                                "the cloud key was rotated on another device — enter the "
                                "password again (set-cloud)");
                        takeTraffic();
                        return finish(false);
                    }
                    if (localValid) {
                        // Порча блоба: лечим здоровым своим.
                        ++done.healedCloud;
                        needPush = true;
                        pushBytes = localBytes;
                    } else {
                        complain(QStringLiteral("blob %1 fails AEAD and the local journal "
                                                "is invalid too")
                                     .arg(name));
                        continue;
                    }
                } else {
                    ZJournal cloudJ;
                    const bool cloudValid = cloudJ.parse(cloudPlain, ZJournal::Want::All, 0,
                                                           &why) &&
                                             cloudJ.damagedCount() == 0 &&
                                             !cloudJ.tailTrimmed();
                    if (!cloudValid) {
                        if (localValid) {
                            ++done.healedCloud;
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
                        if (!adoptJournalBytes(id, cloudPlain, localHash, &why)) {
                            complain(QStringLiteral("cannot adopt %1: %2").arg(name, why));
                            continue;
                        }
                        ++done.takenWhole;
                        touched.insert(id);
                        note(QStringLiteral("taken whole: %1").arg(name));
                        led.etag = etag;
                        led.sealedHash = sealed;
                        led.plainHash = hashBytes(cloudPlain);
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
                                for (int k = 0; k < cloudJ.size() && !found; ++k) {
                                    const ZJournal::Record& e = cloudJ.at(k);
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
                            if (!adoptJournalBytes(id, cloudPlain, localHash, &why)) {
                                ++done.deferred;  // журнал дописался под рукой
                                continue;
                            }
                            ++done.takenWhole;
                            touched.insert(id);
                            note(QStringLiteral("taken whole (superset): %1").arg(name));
                            led.etag = etag;
                            led.sealedHash = sealed;
                            led.plainHash = hashBytes(cloudPlain);
                            ledger.setBlob(name, led);
                            processed.append(id);
                            continue;
                        }
                        ZJournal merged;
                        ZJournal::MergeStats st;
                        if (!localJ.mergedWith(cloudJ, &merged, &st, &why)) {
                            complain(QStringLiteral("merge of %1 refused: %2").arg(name, why));
                            continue;
                        }
                        done.merge.fromThis += st.fromThis;
                        done.merge.fromOther += st.fromOther;
                        done.merge.common += st.common;
                        done.merge.voidedDropped += st.voidedDropped;
                        done.merge.frameConflicts += st.frameConflicts;
                        const Digest mergedSet = merged.contentDigest();
                        if (mergedSet == cloudJ.contentDigest()) {
                            if (mergedSet != localJ.contentDigest()) {
                                if (!adoptJournalBytes(id, cloudPlain, localHash, &why)) {
                                    ++done.deferred;
                                    continue;
                                }
                                ++done.takenWhole;
                                touched.insert(id);
                                led.plainHash = hashBytes(cloudPlain);
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
        } else if (haveCloud && localValid && weChanged && !theyChanged) {
            // Менялись только мы: fast-forward push целиком.
            needPush = true;
            pushBytes = localBytes;
            ++done.pushedWhole;
        }

        // МИГРАЦИЯ ИМЕНИ: стороны совпадают, а блоб живёт под наследным
        // именем — то же содержимое запечатывается новым именем (AAD включает
        // имя, на сервере не переименовать), наследное удалится после заливки.
        // Ветки выше, кончившиеся continue (принято целиком, слито), оставили
        // бухгалтерию наследному имени — их миграция догонит следующим
        // прогоном тем же местом.
        if (!needPush && haveLegacy && localValid && !pushOnly && !weChanged && !theyChanged) {
            needPush = true;
            pushBytes = localBytes;
        }

        if (needPush) {
            QByteArray blob;
            const BlobAad aad{BlobKind::Journal, storeId, pushName};
            if (!cipher_->seal(pushBytes, aad, &blob, error)) {
                takeTraffic();
                return finish(false);
            }
            QString newEtag;
            bool clash = false;
            QString why;
            const bool ok =
                pushExpectedEtag.isEmpty()
                    ? cloud_->put(pushName, blob, &newEtag, &why)
                    : cloud_->putIfMatch(pushName, blob, pushExpectedEtag, &newEtag, &clash,
                                          &why);
            if (!ok) {
                skipTransfer(QStringLiteral("cannot upload %1: %2").arg(pushName, why));
                continue;
            }
            if (clash) {
                // Кто-то успел раньше: не трогаем бухгалтерию, следующий
                // полный прогон скачает и сольёт.
                ++done.deferred;
                continue;
            }
            SyncLedger::Blob pushed;
            pushed.etag = newEtag;
            pushed.sealedHash = hashBytes(blob);
            pushed.plainHash = hashBytes(pushBytes);
            ledger.setBlob(pushName, pushed);
            if (haveLegacy) {
                // Наследный блоб уже слит в нас чтением выше, его содержимое
                // — подмножество только что залитого; с забором версий других
                // писателей нет. Не удалилось — доудалит следующий прогон.
                QString dropWhy;
                if (cloud_->del(legacyName, &dropWhy)) {
                    ledger.dropBlob(legacyName);
                    ++done.migratedLegacy;
                } else {
                    skipTransfer(QStringLiteral("cannot drop legacy %1: %2")
                                     .arg(legacyName, dropWhy));
                }
            }
            processed.append(id);
        }
    }

    // Ранний сигнал без единого расшифрованного блоба: ключ сменился, а
    // читать нечего не пробовали — не гадаем, скажет следующая расшифровка.

    // ВЛОЖЕНИЯ — presence-синк (решение владельца: XMP rev — отдельной
    // сессией). Нет в облаке — залить; нет локально — скачать; обе стороны
    // разные и обе новые — НЕ ТРОГАТЬ и назвать в отчёте.
    if (!pushOnly) {
        // Множество собрано до цикла журналов (итог индикатора — один раз);
        // имя в нём — ЛОКАЛЬНОЕ. Облачное: пишем всегда новым (<id>_<ext>.pic),
        // читаем наследное (== локальному), пока оно существует.
        for (const QString& name : orderedAttachments) {
            if (cancelled()) {
                done.cancelled = true;
                break;
            }
            bumpProgress();
            const bool haveLocal = QFile::exists(attachmentPath(name));
            const QString pushName = cloudAttachmentName(name);
            const bool haveLegacy = cloudEtag.contains(name);
            const QString readName = haveLegacy ? name : pushName;
            const bool haveCloud = haveLegacy || cloudEtag.contains(pushName);
            SyncLedger::Blob led = ledger.blob(readName);
            const QString etag = cloudEtag.value(readName);
            const BlobAad aadRead{BlobKind::Attachment, storeId, readName};
            const BlobAad aadPush{BlobKind::Attachment, storeId, pushName};
            QString why;
            // Заливка локальных байтов НОВЫМ именем; удачная — убирает
            // наследный блоб (миграция, лечение и первая заливка ходят одним
            // путём, иначе наследные имена возвращались бы то тут, то там).
            const auto pushLocal = [&](const QByteArray& bytes) -> bool {
                QByteArray sealedBlob;
                QString newEtag;
                if (!cipher_->seal(bytes, aadPush, &sealedBlob, &why)) {
                    complain(QStringLiteral("cannot seal attachment %1: %2").arg(name, why));
                    return false;
                }
                if (!cloud_->put(pushName, sealedBlob, &newEtag, &why)) {
                    skipTransfer(
                        QStringLiteral("cannot upload attachment %1: %2").arg(pushName, why));
                    return false;
                }
                ledger.setBlob(pushName,
                               {newEtag, hashBytes(sealedBlob), hashBytes(bytes)});
                if (haveLegacy) {
                    QString dropWhy;
                    if (cloud_->del(name, &dropWhy)) {
                        ledger.dropBlob(name);
                        ++done.migratedLegacy;
                    } else {
                        skipTransfer(QStringLiteral("cannot drop legacy %1: %2")
                                         .arg(name, dropWhy));
                    }
                }
                return true;
            };
            if (haveLocal && haveCloud && !led.etag.isEmpty() && led.etag == etag) {
                if (!haveLegacy) continue;
                // Стороны совпадают, но блоб — под наследным именем: миграция.
                // Свои байты сверяются с бухгалтерией ПРЕЖДЕ удаления
                // наследного: разъехались — пусть решает обычный путь
                // следующего прогона, стирать вслепую нельзя.
                QByteArray bytes;
                if (!readAttachmentBytes(name, &bytes, &why)) continue;
                if (hashBytes(bytes) != led.plainHash) continue;
                pushLocal(bytes);
                continue;
            }
            if (haveLocal && !haveCloud) {
                QByteArray bytes;
                if (!readAttachmentBytes(name, &bytes, &why)) {
                    complain(QStringLiteral("cannot read attachment %1: %2").arg(name, why));
                    continue;
                }
                if (pushLocal(bytes)) ++done.attachmentsUp;
                continue;
            }
            // Дальше нужен GET: либо файла нет локально, либо etag разошёлся.
            QByteArray blob;
            if (!cloud_->get(readName, &blob, nullptr, &why)) {
                skipTransfer(
                    QStringLiteral("cannot download attachment %1: %2").arg(readName, why));
                continue;
            }
            const Digest sealed = hashBytes(blob);
            if (haveLocal && !led.sealedHash.empty() && sealed == led.sealedHash) {
                ++done.etagReissued;
                led.etag = etag;
                ledger.setBlob(readName, led);
                continue;
            }
            QByteArray plain;
            if (!cipher_->open(blob, aadRead, &plain, &why)) {
                ++aeadFailures;
                if (keyfileChanged) {
                    if (error != nullptr)
                        *error = QStringLiteral(
                            "the cloud key was rotated on another device — enter the password "
                            "again (set-cloud)");
                    takeTraffic();
                    return finish(false);
                }
                if (haveLocal) {
                    // Порча блоба: здоровая сторона побеждает — перезаливка,
                    // уже новым именем (наследный битый уходит там же).
                    QByteArray bytes;
                    if (readAttachmentBytes(name, &bytes, &why) && pushLocal(bytes)) {
                        ++done.healedCloud;
                    } else {
                        complain(QStringLiteral("cannot heal attachment %1: %2").arg(name, why));
                    }
                } else {
                    complain(QStringLiteral("attachment %1 fails AEAD and there is no local "
                                            "copy")
                                 .arg(readName));
                }
                continue;
            }
            if (!haveLocal) {
                // Скачивание: атомарная запись; dirty-пометки не нужны — это
                // вложение, его хеш ложится в бухгалтерию сразу. Наследное имя
                // при скачивании НЕ мигрируется (это перезаливка всего блоба);
                // мигрирует та машина, у которой байты уже локально (ярус 1).
                if (!writeFileBytes(attachmentPath(name),
                                    std::string(plain.constData(), size_t(plain.size())),
                                    &why)) {
                    complain(QStringLiteral("cannot write attachment %1: %2").arg(name, why));
                    continue;
                }
                ++done.attachmentsDown;
                ledger.setBlob(readName, {etag, sealed, hashBytes(plain)});
                continue;
            }
            // Обе стороны есть и байты разные.
            QByteArray localAtt;
            if (!readAttachmentBytes(name, &localAtt, &why)) continue;
            const Digest localHashAtt = hashBytes(localAtt);
            if (localHashAtt == hashBytes(plain)) {
                ledger.setBlob(readName, {etag, sealed, localHashAtt});
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
                ledger.setBlob(readName, {etag, sealed, hashBytes(plain)});
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
        // Материализация — тоже в счётчик и со СВОИМ словом фазы: на первой
        // загрузке она самая долгая, и индикатор, замерший на N/N, читался
        // как зависание (владелец, первый живой прогон).
        if (options.progressPhase != nullptr) options.progressPhase->store(1);
        if (options.progressTotal != nullptr)
            options.progressTotal->fetch_add(int(orderedCandidates.size()));
        for (const QString& id : orderedCandidates) {
            // Отмена опрашивается и здесь: на первой загрузке материализация —
            // самая долгая фаза, и без опроса кнопка «остановить» дожидалась
            // бы её конца. Каждая запись атомарна, пометки dirty живы —
            // прерывание безопасно в любой точке (инвариант E), повторный
            // прогон достроит.
            if (cancelled()) {
                done.cancelled = true;
                break;
            }
            bumpProgress();
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
        // Отменённый прогон удалений не применяет вовсе: «остановить» значит
        // остановить, а надгробия никуда не денутся — доделает следующий.
        if (done.cancelled) {
            deletes.clear();
        }
        if (int(deletes.size()) > options.deleteGuard && !options.allowMassDelete) {
            done.pendingDeletes = deletes;
            // Пометить, чтобы следующий прогон — с подтверждением или после
            // declareAlive — вернулся к этим заметкам, даже когда обмен для
            // них снова бесплатен.
            for (const QString& id : deletes) markDirty(id);
        } else {
            for (const QString& id : deletes) {
                const QString path = pathOf(id);
                QString whyDelete;
                if (!files().remove(path, &whyDelete)) {
                    complain(QStringLiteral("cannot remove %1 for its tombstone: %2")
                                 .arg(id, whyDelete));
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
    // Перекладывается и после миграции имён: свежая версия формата в нём —
    // забор для прежних сборок (tooNew — честный стоп), иначе старая машина
    // возвращала бы наследные имена в облако.
    if (!pushOnly && !done.cancelled &&
        (!cloudEtag.contains(QLatin1String(Identity::kFile)) || done.migratedLegacy > 0)) {
        QString newEtag;
        QString why;
        if (cloud_->put(QLatin1String(Identity::kFile), mine.atCurrentFormat().toBytes(),
                         &newEtag, &why)) {
            SyncLedger::Blob led;
            led.etag = newEtag;
            led.sealedHash = hashBytes(mine.atCurrentFormat().toBytes());
            ledger.setBlob(QLatin1String(Identity::kFile), led);
        } else {
            skipTransfer(QStringLiteral("cannot upload the manifest: %1").arg(why));
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
             "healed %7, migrated %8, materialized %9, deletes %10/%11 pending, "
             "attachments %12 up %13 down, failures %14; traffic %15 req, %16 B up, "
             "%17 B down")
             .arg(done.listed)
             .arg(done.skipped)
             .arg(done.takenWhole)
             .arg(done.pushedWhole)
             .arg(done.mergedJournals)
             .arg(done.deferred)
             .arg(done.healedCloud)
             .arg(done.migratedLegacy)
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

bool ZStorage::setCloud(const std::shared_ptr<CloudStore>& cloud,
                         const Keyfile& keyfile, QString* error) {
    if (!store_) {
        if (error != nullptr) *error = QStringLiteral("not a store: %1").arg(root_);
        return false;
    }
    if (!cloud) {
        if (error != nullptr) *error = QStringLiteral("no cloud given");
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
    // опечатка в cloudDir иначе молча слила бы две несвязанные базы.
    QByteArray manifest;
    QString why;
    if (cloud->get(QLatin1String(Identity::kFile), &manifest, nullptr, &why)) {
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
                    probe ? cloudStoreName(*cloud, *probe, theirs) : QString();
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
    cloud_ = cloud;
    cipher_ = cipher;
    return true;
}

void ZStorage::dropCloud() {
    cloud_.reset();
    cipher_.reset();
}

bool ZStorage::pushAll(PushReport* report, QString* error) {
    PushReport done;
    const auto finish = [&](bool ok) {
        if (report != nullptr) *report = done;
        return ok;
    };
    if (!isConnected()) {
        if (error != nullptr)
            *error = QStringLiteral("no cloud is connected — call setCloud first");
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

    if (!cloud_->mkdirOnce(error)) return finish(false);

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
        const bool ok = cloud_->put(name, blob, nullptr, error);
        done.usPut += timer.nsecsElapsed() / 1000;
        return ok;
    };

    for (const QString& id : noteIds) {
        QByteArray journal;
        if (!readJournalBytes(id, &journal, error)) return finish(false);
        if (journal.isEmpty()) continue;
        if (!sealAndPut(cloudJournalName(id), journal)) return finish(false);
        ++done.journals;
    }

    for (const QString& name : attachmentNames()) {
        QByteArray bytes;
        if (!readAttachmentBytes(name, &bytes, error)) return finish(false);
        if (!sealAndPut(cloudAttachmentName(name), bytes)) return finish(false);
        ++done.attachments;
    }

    // 3. МАНИФЕСТ — последним и ОТКРЫТЫМ ТЕКСТОМ. Последним потому, что он
    // означает «здесь лежит хранилище такое-то»: сказать это раньше, чем
    // хоть что-то залито, значило бы соврать при обрыве.
    timer.restart();
    if (!cloud_->put(QLatin1String(Identity::kFile), mine.atCurrentFormat().toBytes(),
                      nullptr, error))
        return finish(false);
    done.usPut += timer.nsecsElapsed() / 1000;
    return finish(true);
}

}  // namespace zametti
