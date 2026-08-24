#include "sync_ledger.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>

#include <algorithm>

namespace zametti {
namespace {

// Отпечатки в файле — шестнадцатеричные: бухгалтерию читают глазами при
// разборе бед, и byte-массивы в JSON этому не помощники.
QString hexOf(const Digest& d) {
    return d.empty() ? QString() : QString::fromStdString(d.hex());
}

Digest digestFromHex(const QString& hex) {
    Digest out;
    if (hex.size() != int(out.bytes.size()) * 2) return {};
    const QByteArray bytes = QByteArray::fromHex(hex.toLatin1());
    if (bytes.size() != int(out.bytes.size())) return {};
    std::copy(bytes.begin(), bytes.end(), reinterpret_cast<char*>(out.bytes.data()));
    return out;
}

}  // namespace

QString SyncLedger::pathFor(const QString& storeId, const QString& storeRoot) {
    // Хеш КАНОНИЧЕСКОГО пути: две копии хранилища на одной машине не делят
    // бухгалтерию, а один и тот же каталог, названный по-разному
    // («vpnotes/» и «vpnotes»), — делит.
    const QString canonical = QDir(storeRoot).absolutePath();
    const std::string utf8 = canonical.toStdString();
    const QString tag = QString::fromStdString(hashOf(utf8).hex()).left(8);
    return QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) +
           QStringLiteral("/sync-state-%1-%2.json").arg(storeId, tag);
}

SyncLedger SyncLedger::load(const QString& path) {
    SyncLedger out;
    out.path_ = path;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return out;
    QJsonParseError bad;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &bad);
    if (bad.error != QJsonParseError::NoError || !doc.isObject()) return out;
    const QJsonObject root = doc.object();
    // Версия новее нашей — пустой старт, не отказ: кэш пересчитается сам.
    if (root.value(QStringLiteral("version")).toInt(0) != kFormatVersion) return out;
    out.cleanShutdown_ = root.value(QStringLiteral("cleanShutdown")).toBool(true);

    const QJsonObject blobs = root.value(QStringLiteral("blobs")).toObject();
    for (auto it = blobs.begin(); it != blobs.end(); ++it) {
        const QJsonObject o = it.value().toObject();
        Blob b;
        b.etag = o.value(QStringLiteral("etag")).toString();
        b.sealedHash = digestFromHex(o.value(QStringLiteral("sealed")).toString());
        b.plainHash = digestFromHex(o.value(QStringLiteral("plain")).toString());
        if (!it.key().isEmpty() && !b.isEmpty()) out.blobs_.insert(it.key(), b);
    }
    const QJsonObject files = root.value(QStringLiteral("files")).toObject();
    for (auto it = files.begin(); it != files.end(); ++it) {
        const QJsonObject o = it.value().toObject();
        Stat s;
        s.mtimeMs = qint64(o.value(QStringLiteral("mtime")).toDouble(0));
        s.size = qint64(o.value(QStringLiteral("size")).toDouble(-1));
        if (!it.key().isEmpty() && !s.isEmpty()) out.files_.insert(it.key(), s);
    }
    return out;
}

bool SyncLedger::save(QString* error) const {
    if (path_.isEmpty()) {
        if (error) *error = QStringLiteral("the ledger has no path to save to");
        return false;
    }
    QJsonObject blobs;
    for (auto it = blobs_.begin(); it != blobs_.end(); ++it) {
        QJsonObject o;
        if (!it.value().etag.isEmpty()) o.insert(QStringLiteral("etag"), it.value().etag);
        if (!it.value().sealedHash.empty())
            o.insert(QStringLiteral("sealed"), hexOf(it.value().sealedHash));
        if (!it.value().plainHash.empty())
            o.insert(QStringLiteral("plain"), hexOf(it.value().plainHash));
        blobs.insert(it.key(), o);
    }
    QJsonObject files;
    for (auto it = files_.begin(); it != files_.end(); ++it) {
        QJsonObject o;
        o.insert(QStringLiteral("mtime"), double(it.value().mtimeMs));
        o.insert(QStringLiteral("size"), double(it.value().size));
        files.insert(it.key(), o);
    }
    QJsonObject root;
    root.insert(QStringLiteral("version"), kFormatVersion);
    root.insert(QStringLiteral("cleanShutdown"), cleanShutdown_);
    root.insert(QStringLiteral("blobs"), blobs);
    root.insert(QStringLiteral("files"), files);

    QDir().mkpath(QFileInfo(path_).absolutePath());
    QSaveFile save(path_);
    if (!save.open(QIODevice::WriteOnly)) {
        if (error) *error = QStringLiteral("cannot write %1: %2").arg(path_, save.errorString());
        return false;
    }
    save.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    if (!save.commit()) {
        if (error) *error = QStringLiteral("cannot write %1: %2").arg(path_, save.errorString());
        return false;
    }
    return true;
}

QStringList SyncLedger::blobNames() const {
    QStringList out(blobs_.keyBegin(), blobs_.keyEnd());
    std::sort(out.begin(), out.end());
    return out;
}

QStringList SyncLedger::knownFiles() const {
    QStringList out(files_.keyBegin(), files_.keyEnd());
    std::sort(out.begin(), out.end());
    return out;
}

}  // namespace zametti
