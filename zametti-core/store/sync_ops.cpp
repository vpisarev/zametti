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

#include "keyfile.h"
#include "note_id.h"
#include "times.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>

namespace zametti {
namespace {

// Журналы и вложения различаются по имени, и это различие подписано тегом
// (AAD): подсунуть вложение под именем журнала не выйдет.
BlobKind kindOfBlob(const QString& name) {
    return name.endsWith(QStringLiteral(".log")) ? BlobKind::Journal
                                                 : BlobKind::Attachment;
}

}  // namespace

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
    done.msBaseline = timer.elapsed();

    if (!remote_->mkdirOnce(error)) return finish(false);

    // 2. ЗАЛИВКА. Один путь для журналов и вложений: имя блоба, байты, AAD.
    const auto sealAndPut = [&](const QString& name, const QByteArray& plain) {
        QByteArray blob;
        timer.restart();
        const BlobAad aad{kindOfBlob(name), mine.storeId(), name};
        if (!cipher_->seal(plain, aad, &blob, error)) return false;
        done.msSeal += timer.elapsed();
        done.plainBytes += plain.size();
        done.sealedBytes += blob.size();

        timer.restart();
        const bool ok = remote_->put(name, blob, nullptr, error);
        done.msPut += timer.elapsed();
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
    done.msPut += timer.elapsed();
    return finish(true);
}

}  // namespace zametti
