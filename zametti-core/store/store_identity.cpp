#include "store_identity.h"

#include "note_id.h"

#include <QJsonDocument>
#include <QJsonValue>

namespace zametti::store {
namespace {
// Ключи названы один раз.
constexpr char kStoreId[] = "storeId";
constexpr char kFormatVersion[] = "formatVersion";
constexpr char kCreated[] = "created";
constexpr char kRootNote[] = "rootNote";
}  // namespace

bool StoreIdentity::parse(const QByteArray& bytes, QString* error) {
    QJsonParseError problem{};
    const QJsonDocument doc = QJsonDocument::fromJson(bytes, &problem);
    if (problem.error != QJsonParseError::NoError) {
        if (error != nullptr)
            *error = QStringLiteral("%1 is not JSON: %2")
                         .arg(QLatin1String(kIdentityFile), problem.errorString());
        return false;
    }
    if (!doc.isObject()) {
        if (error != nullptr)
            *error = QStringLiteral("%1 is not an object").arg(QLatin1String(kIdentityFile));
        return false;
    }
    QJsonObject object = doc.object();

    const QString id = object.value(QLatin1String(kStoreId)).toString();
    if (!isValidNoteId(id.toUtf8().toStdString())) {
        if (error != nullptr)
            *error = QStringLiteral("%1: storeId is not a valid id: %2")
                         .arg(QLatin1String(kIdentityFile), id);
        return false;
    }
    storeId_ = id;
    // Версии нет — считаем первой: так выглядит файл, написанный сборкой, у
    // которой версии ещё не было. Нечисло — тоже первая, а не ноль: «странное
    // значение» не повод объявлять хранилище будущим.
    const QJsonValue version = object.value(QLatin1String(kFormatVersion));
    formatVersion_ = version.isDouble() ? version.toInt(kStoreFormatVersion) : kStoreFormatVersion;
    created_ = object.value(QLatin1String(kCreated)).toString();
    rootNote_ = object.value(QLatin1String(kRootNote)).toString();

    // Остальное — чужое знание: держим его при себе и вернём в файл.
    object.remove(QLatin1String(kStoreId));
    object.remove(QLatin1String(kFormatVersion));
    object.remove(QLatin1String(kCreated));
    object.remove(QLatin1String(kRootNote));
    extra_ = object;
    return true;
}

QByteArray StoreIdentity::toBytes() const {
    QJsonObject object = extra_;
    object.insert(QLatin1String(kStoreId), storeId_);
    object.insert(QLatin1String(kFormatVersion), formatVersion_);
    if (!created_.isEmpty()) object.insert(QLatin1String(kCreated), created_);
    if (!rootNote_.isEmpty()) object.insert(QLatin1String(kRootNote), rootNote_);
    // Человекочитаемо: файл лежит на виду, и заглянуть в него должно быть
    // можно любым редактором.
    return QJsonDocument(object).toJson(QJsonDocument::Indented);
}

StoreIdentity StoreIdentity::mint(const QString& storeId, const QString& createdIso) {
    StoreIdentity out;
    out.storeId_ = storeId;
    out.formatVersion_ = kStoreFormatVersion;
    out.created_ = createdIso;
    return out;
}

}  // namespace zametti::store
