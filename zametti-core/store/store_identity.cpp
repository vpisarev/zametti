#include "store_identity.h"

#include "note_id.h"
#include "times.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

namespace zametti::store {

QString StoreIdentity::pathFor(const QString& root) {
    return QDir(root).filePath(QStringLiteral("zametti.json"));
}

StoreIdentity StoreIdentity::read(const QString& root, QString* error) {
    StoreIdentity out;
    QFile file(pathFor(root));
    if (!file.exists()) return out;
    if (!file.open(QIODevice::ReadOnly)) {
        if (error != nullptr) *error = QStringLiteral("cannot read zametti.json");
        return out;
    }
    const QByteArray bytes = file.readAll();
    file.close();

    QJsonParseError parsed{};
    const QJsonDocument doc = QJsonDocument::fromJson(bytes, &parsed);
    if (parsed.error != QJsonParseError::NoError || !doc.isObject()) {
        if (error != nullptr)
            *error = QStringLiteral("zametti.json is not valid JSON: %1").arg(parsed.errorString());
        return out;
    }
    const QJsonObject object = doc.object();
    out.storeId_ = object.value(QStringLiteral("storeId")).toString();
    out.formatVersion_ =
        object.value(QStringLiteral("formatVersion")).toInt(kStoreFormatVersion);
    out.created_ = object.value(QStringLiteral("created")).toString();
    out.rootNote_ = object.value(QStringLiteral("rootNote")).toString();
    return out;
}

bool StoreIdentity::write(const QString& root, QString* error) const {
    QJsonObject object;
    object.insert(QStringLiteral("storeId"), storeId_);
    object.insert(QStringLiteral("formatVersion"), formatVersion_);
    object.insert(QStringLiteral("created"), created_);
    // Пустой ключ не пишем: «корня ещё нет» и «корень назван пустым» — разные
    // вещи, и различать их по значению проще, чем по наличию.
    if (!rootNote_.isEmpty()) object.insert(QStringLiteral("rootNote"), rootNote_);

    QSaveFile file(pathFor(root));
    if (!file.open(QIODevice::WriteOnly)) {
        if (error != nullptr) *error = QStringLiteral("cannot write zametti.json");
        return false;
    }
    file.write(QJsonDocument(object).toJson(QJsonDocument::Indented));
    if (!file.commit()) {
        if (error != nullptr) *error = QStringLiteral("cannot write zametti.json");
        return false;
    }
    return true;
}

StoreIdentity StoreIdentity::ensure(const QString& root, QString* error) {
    StoreIdentity found = read(root, error);
    if (!found.isEmpty()) return found;

    StoreIdentity fresh;
    fresh.storeId_ = QString::fromStdString(newNoteId());
    fresh.formatVersion_ = kStoreFormatVersion;
    fresh.created_ = nowStamp();
    if (!fresh.write(root, error)) return StoreIdentity{};
    return fresh;
}

bool StoreIdentity::setRootNote(const QString& root, const QString& noteId, QString* error) {
    rootNote_ = noteId;
    return write(root, error);
}

}  // namespace zametti::store
