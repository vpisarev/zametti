// ZStorage::Identity — разбор и сборка zametti.json. Файлов здесь нет:
// чтение и запись — глаголы хранилища (zstorage.cpp).

#include "zstorage.h"

#include "note_id.h"

#include <QJsonDocument>
#include <QJsonValue>

namespace zametti {
namespace {
// Ключи названы один раз.
constexpr char kStoreIdKey[] = "storeId";
constexpr char kFormatVersionKey[] = "formatVersion";
constexpr char kCreatedKey[] = "created";
constexpr char kRootNoteKey[] = "rootNote";
}  // namespace

bool ZStorage::Identity::parse(const QByteArray& bytes, QString* error) {
    QJsonParseError problem{};
    const QJsonDocument doc = QJsonDocument::fromJson(bytes, &problem);
    if (problem.error != QJsonParseError::NoError) {
        if (error != nullptr)
            *error = QStringLiteral("%1 is not JSON: %2")
                         .arg(QLatin1String(kFile), problem.errorString());
        return false;
    }
    if (!doc.isObject()) {
        if (error != nullptr)
            *error = QStringLiteral("%1 is not an object").arg(QLatin1String(kFile));
        return false;
    }
    QJsonObject object = doc.object();

    const QString id = object.value(QLatin1String(kStoreIdKey)).toString();
    if (!isValidNoteId(id.toUtf8().toStdString())) {
        if (error != nullptr)
            *error = QStringLiteral("%1: storeId is not a valid id: %2")
                         .arg(QLatin1String(kFile), id);
        return false;
    }
    storeId_ = id;
    // Версии нет — считаем первой: так выглядит файл, написанный сборкой, у
    // которой версии ещё не было. Нечисло — тоже первая, а не ноль: «странное
    // значение» не повод объявлять хранилище будущим.
    const QJsonValue version = object.value(QLatin1String(kFormatVersionKey));
    formatVersion_ = version.isDouble() ? version.toInt(kFormatVersion) : kFormatVersion;
    created_ = object.value(QLatin1String(kCreatedKey)).toString();
    rootNote_ = object.value(QLatin1String(kRootNoteKey)).toString();

    // Остальное — чужое знание: держим его при себе и вернём в файл.
    object.remove(QLatin1String(kStoreIdKey));
    object.remove(QLatin1String(kFormatVersionKey));
    object.remove(QLatin1String(kCreatedKey));
    object.remove(QLatin1String(kRootNoteKey));
    extra_ = object;
    return true;
}

QByteArray ZStorage::Identity::toBytes() const {
    QJsonObject object = extra_;
    object.insert(QLatin1String(kStoreIdKey), storeId_);
    object.insert(QLatin1String(kFormatVersionKey), formatVersion_);
    if (!created_.isEmpty()) object.insert(QLatin1String(kCreatedKey), created_);
    if (!rootNote_.isEmpty()) object.insert(QLatin1String(kRootNoteKey), rootNote_);
    // Человекочитаемо: файл лежит на виду, и заглянуть в него должно быть
    // можно любым редактором.
    return QJsonDocument(object).toJson(QJsonDocument::Indented);
}

ZStorage::Identity ZStorage::Identity::mint(const QString& storeId, const QString& createdIso) {
    Identity out;
    out.storeId_ = storeId;
    out.formatVersion_ = kFormatVersion;
    out.created_ = createdIso;
    return out;
}

}  // namespace zametti
