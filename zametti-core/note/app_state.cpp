#include "app_state.h"
#include "settings.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace zametti {

QString ZAppState::path() {
    return configDir() + QStringLiteral("/state.json");
}

void ZAppState::rememberCaret(const QString& noteId, const CaretSpot& spot) {
    if (noteId.isEmpty()) return;
    // Без дублей: запись по id одна; свежая — в голову, чтобы при обрезке по
    // числу уходили самые давние.
    for (qsizetype i = carets_.size(); i-- > 0;)
        if (carets_[i].noteId == noteId) carets_.removeAt(i);
    carets_.prepend({noteId, spot.cursor, spot.anchor, spot.scroll});
    while (carets_.size() > kCaretLimit) carets_.removeLast();
}

CaretSpot ZAppState::caretOf(const QString& noteId) const {
    for (const CaretEntry& e : carets_)
        if (e.noteId == noteId) return CaretSpot{e.cursor, e.anchor, e.scroll};
    return CaretSpot{};
}

bool ZAppState::knowsCaret(const QString& noteId) const {
    for (const CaretEntry& e : carets_)
        if (e.noteId == noteId) return true;
    return false;
}

namespace {
// Ключ строки списка — канонический вид пути: один и тот же каталог, названный
// с хвостовым слэшем или через «..», не должен плодить две строки.
QString canonicalRoot(const QString& root) {
    if (root.isEmpty()) return {};
    return QDir::cleanPath(QFileInfo(root).absoluteFilePath());
}
}  // namespace

void ZAppState::rememberStore(const ZStorage::Config& entry) {
    if (entry.root.isEmpty()) return;
    ZStorage::Config kept = entry;
    kept.root = canonicalRoot(entry.root);
    for (ZStorage::Config& e : stores_) {
        if (e.root == kept.root) {
            // Имя могло не приехать (звали без чтения корня) — прежнее
            // дороже пустого.
            if (kept.name.isEmpty()) kept.name = e.name;
            e = kept;
            return;
        }
    }
    stores_.append(kept);
}

void ZAppState::forgetStore(const QString& root) {
    const QString key = canonicalRoot(root);
    for (qsizetype i = stores_.size(); i-- > 0;)
        if (stores_[i].root == key) stores_.removeAt(i);
}

ZStorage::Config ZAppState::storeFor(const QString& root) const {
    const QString key = canonicalRoot(root);
    for (const ZStorage::Config& e : stores_)
        if (e.root == key) return e;
    return {};
}

ZAppState ZAppState::load() {
    ZAppState session;
    QFile file(path());
    if (!file.open(QIODevice::ReadOnly)) return session;

    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isObject()) return session;
    const QJsonObject root = doc.object();

    session.setLastFile(root.value(QStringLiteral("lastFile")).toString());
    session.setStoreRoot(root.value(QStringLiteral("storeRoot")).toString());
    session.setTreeSort(root.value(QStringLiteral("treeSort")).toString());
    session.setCaret(root.value(QStringLiteral("caret")).toInt(0));
    session.setAnchor(root.value(QStringLiteral("anchor")).toInt(session.caret()));
    session.setZoom(root.value(QStringLiteral("zoom")).toDouble(1.0));
    session.setWindowGeometry(QByteArray::fromBase64(
        root.value(QStringLiteral("windowGeometry")).toString().toLatin1()));
    session.setSplitterState(QByteArray::fromBase64(
        root.value(QStringLiteral("splitterState")).toString().toLatin1()));
    session.setPanelsHidden(root.value(QStringLiteral("panelsHidden")).toBool(false));
    session.setMarkdownMode(root.value(QStringLiteral("markdownMode")).toBool(false));
    // Ленивая миграция: прежде масштаб исходника звался markdownZoom, а у
    // правки настроек был свой. Новый ключ один; нет его — берём старый.
    session.setPlainZoom(root.value(QStringLiteral("plainZoom"))
                             .toDouble(root.value(QStringLiteral("markdownZoom")).toDouble(1.0)));
    // Масштаб истории: нет ключа — берём масштаб заметки, чтобы режим открылся
    // тем же кеглем, каким человек читает саму заметку.
    session.setHistoryZoom(root.value(QStringLiteral("historyZoom")).toDouble(session.zoom()));
    session.setHistoryListWidth(root.value(QStringLiteral("historyListWidth")).toInt(0));
    session.setExportDir(root.value(QStringLiteral("exportDir")).toString());
    session.setExportKeepMeta(root.value(QStringLiteral("exportKeepMeta")).toBool(false));
    for (const QJsonValue& v : root.value(QStringLiteral("carets")).toArray()) {
        const QJsonObject o = v.toObject();
        CaretEntry e;
        e.noteId = o.value(QStringLiteral("id")).toString();
        e.cursor = o.value(QStringLiteral("cursor")).toInt(0);
        e.anchor = o.value(QStringLiteral("anchor")).toInt(e.cursor);
        e.scroll = o.value(QStringLiteral("scroll")).toInt(0);
        if (!e.noteId.isEmpty() && session.carets_.size() < kCaretLimit)
            session.carets_.append(e);
    }
    QStringList expanded;
    for (const QJsonValue& v : root.value(QStringLiteral("expandedDirs")).toArray())
        if (v.isString()) expanded.append(v.toString());
    session.setExpandedDirs(expanded);
    QStringList searches;
    for (const QJsonValue& v : root.value(QStringLiteral("searchHistory")).toArray())
        if (v.isString()) searches.append(v.toString());
    session.setSearchHistory(searches);
    session.setSearchRegex(root.value(QStringLiteral("searchRegex")).toBool(false));
    for (const QJsonValue& v : root.value(QStringLiteral("stores")).toArray()) {
        ZStorage::Config entry;
        entry.parse(v.toObject());
        // Через rememberStore, а не напрямую: канонизация и дедупликация —
        // одни на запись и на чтение.
        session.rememberStore(entry);
    }
    return session;
}

void ZAppState::save() const {
    const ZAppState& session = *this;
    QJsonArray expanded;
    for (const QString& dir : session.expandedDirs()) expanded.append(dir);
    QJsonArray searches;
    for (const QString& query : session.searchHistory()) searches.append(query);
    QJsonArray carets;
    for (const CaretEntry& e : session.carets_)
        carets.append(QJsonObject{{QStringLiteral("id"), e.noteId},
                                  {QStringLiteral("cursor"), e.cursor},
                                  {QStringLiteral("anchor"), e.anchor},
                                  {QStringLiteral("scroll"), e.scroll}});
    QJsonArray stores;
    for (const ZStorage::Config& e : session.stores_) stores.append(e.entryJson());

    const QJsonObject root{
                  {QStringLiteral("lastFile"), session.lastFile()},
                  {QStringLiteral("storeRoot"), session.storeRoot()},
                  {QStringLiteral("treeSort"), session.treeSort()},
                  {QStringLiteral("caret"), session.caret()},
                  {QStringLiteral("anchor"), session.anchor()},
                  {QStringLiteral("zoom"), session.zoom()},
                  {QStringLiteral("windowGeometry"),
                   QString::fromLatin1(session.windowGeometry().toBase64())},
                  {QStringLiteral("splitterState"),
                   QString::fromLatin1(session.splitterState().toBase64())},
                  {QStringLiteral("panelsHidden"), session.panelsHidden()},
                  {QStringLiteral("markdownMode"), session.markdownMode()},
                  {QStringLiteral("plainZoom"), session.plainZoom()},
                  {QStringLiteral("historyZoom"), session.historyZoom()},
                  {QStringLiteral("historyListWidth"), session.historyListWidth()},
                  {QStringLiteral("exportDir"), session.exportDir()},
                  {QStringLiteral("exportKeepMeta"), session.exportKeepMeta()},
                  {QStringLiteral("expandedDirs"), expanded},
                  {QStringLiteral("searchHistory"), searches},
                  {QStringLiteral("searchRegex"), session.searchRegex()},
                  {QStringLiteral("carets"), carets},
                  {QStringLiteral("stores"), stores},
    };
    QDir().mkpath(QFileInfo(path()).absolutePath());
    QFile file(path());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return;
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));

}

}  // namespace zametti
