#include "app_state.h"
#include "settings.h"
#include "zstorage_manager.h"

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

namespace {
// The entry of the note, taken out of the list (to the head after the edit);
// a fresh one when there is none.
ZAppState::CaretEntry takeEntry(QList<ZAppState::CaretEntry>& carets, const QString& noteId) {
    // Без дублей: запись по id одна; свежая — в голову, чтобы при обрезке по
    // числу уходили самые давние.
    ZAppState::CaretEntry entry;
    entry.noteId = noteId;
    for (qsizetype i = carets.size(); i-- > 0;) {
        if (carets[i].noteId != noteId) continue;
        entry = carets[i];
        carets.removeAt(i);
    }
    return entry;
}
}  // namespace

void ZAppState::rememberCaret(const QString& noteId, const CaretSpot& spot) {
    if (noteId.isEmpty()) return;
    CaretEntry entry = takeEntry(carets_, noteId);
    entry.cursor = spot.cursor;
    entry.anchor = spot.anchor;
    entry.scroll = spot.scroll;
    carets_.prepend(entry);
    while (carets_.size() > kCaretLimit) carets_.removeLast();
}

void ZAppState::rememberReading(const QString& noteId, int block, int line) {
    if (noteId.isEmpty()) return;
    CaretEntry entry = takeEntry(carets_, noteId);
    entry.readingBlock = block;
    entry.readingLine = line;
    carets_.prepend(entry);
    while (carets_.size() > kCaretLimit) carets_.removeLast();
}

void ZAppState::rememberMode(const QString& noteId, int mode) {
    if (noteId.isEmpty()) return;
    CaretEntry entry = takeEntry(carets_, noteId);
    entry.mode = mode;
    carets_.prepend(entry);
    while (carets_.size() > kCaretLimit) carets_.removeLast();
}

CaretSpot ZAppState::caretOf(const QString& noteId) const {
    for (const CaretEntry& e : carets_)
        if (e.noteId == noteId)
            return CaretSpot{e.cursor, e.anchor, e.scroll, e.readingBlock, e.readingLine, e.mode};
    return CaretSpot{};
}

bool ZAppState::knowsCaret(const QString& noteId) const {
    for (const CaretEntry& e : carets_)
        if (e.noteId == noteId) return true;
    return false;
}

// ТРИ СТУПЕНИ МАСШТАБА — СВОЕЙ СЕКЦИЕЙ, и это же отличает их от старого
// файла. Прежде масштабы лежали в корне дробными числами (zoom: 1.1,
// plainZoom, historyZoom, ещё раньше markdownZoom); теперь в корне лежит
// ОБЪЕКТ zoom с целыми ступенями. Разобрать одно от другого можно ровно по
// виду значения — число это или объект, — и никак иначе: в JSON «2» одинаково
// годится и на двойной множитель, и на две ступени.
void ZAppState::readZoom(const QJsonObject& root) {
    const QJsonValue zoom = root.value(QStringLiteral("zoom"));
    if (zoom.isObject()) {
        const QJsonObject steps = zoom.toObject();
        setNoteZoom(steps.value(QStringLiteral("note")).toInt(0));
        // Ступень «history» слита в source (решение владельца, 02.09.2026):
        // разность и исходник — плоские виды одного кегля. Файл, писанный до
        // слияния, несёт оба ключа — побеждает source (режим исходника
        // переживает перезапуск, им пользуются чаще); файл без source, но с
        // history отдаёт объединённой ступени свою.
        setSourceZoom(steps.value(QStringLiteral("source"))
                          .toInt(steps.value(QStringLiteral("history")).toInt(0)));
        setInterfaceZoom(steps.value(QStringLiteral("interface")).toInt(0));
        setBookZoom(steps.value(QStringLiteral("book")).toInt(0));
        return;
    }

    // Старый файл: множители переводим в ступени. Числа при этом слегка
    // округляются (1.1 — это не ровно ступень, а 1.98 от неё), и это верно:
    // шкала теперь одна, и промежуточных множителей на ней не бывает.
    // Дробный historyZoom не читается вовсе: его ступень слита в source.
    setNoteZoom(zoomStepsFor(zoom.toDouble(1.0)));
    const QJsonValue source = root.contains(QStringLiteral("plainZoom"))
                                  ? root.value(QStringLiteral("plainZoom"))
                                  : root.value(QStringLiteral("markdownZoom"));
    setSourceZoom(zoomStepsFor(source.toDouble(1.0)));
    // Масштаба оболочки в старых файлах не было вовсе — ступень нулевая.
    setInterfaceZoom(0);
}

ZAppState ZAppState::load(ZStorageManager* stores) {
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
    session.readZoom(root);
    session.setWindowGeometry(QByteArray::fromBase64(
        root.value(QStringLiteral("windowGeometry")).toString().toLatin1()));
    session.setSplitterState(QByteArray::fromBase64(
        root.value(QStringLiteral("splitterState")).toString().toLatin1()));
    session.setPanelsHidden(root.value(QStringLiteral("panelsHidden")).toBool(false));
    session.setMarkdownMode(root.value(QStringLiteral("markdownMode")).toBool(false));
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
        e.readingBlock = o.value(QStringLiteral("readingBlock")).toInt(0);
        e.readingLine = o.value(QStringLiteral("readingLine")).toInt(0);
        e.mode = o.value(QStringLiteral("mode")).toInt(0);
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
    // Секция хранилищ тут же отдаётся менеджеру и НЕ хранится: приложение
    // работает со списком только через ZStorageManager, а массив живёт один
    // миг сериализации (решение владельца, 30.08.2026).
    if (stores != nullptr)
        stores->storesFromJson(root.value(QStringLiteral("stores")).toArray());
    return session;
}

void ZAppState::save(const ZStorageManager& stores) const {
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
                                  {QStringLiteral("scroll"), e.scroll},
                                            {QStringLiteral("readingBlock"), e.readingBlock},
                                            {QStringLiteral("readingLine"), e.readingLine},
                                            {QStringLiteral("mode"), e.mode}});
    const QJsonObject root{
                  {QStringLiteral("lastFile"), session.lastFile()},
                  {QStringLiteral("storeRoot"), session.storeRoot()},
                  {QStringLiteral("treeSort"), session.treeSort()},
                  {QStringLiteral("caret"), session.caret()},
                  {QStringLiteral("anchor"), session.anchor()},
                  {QStringLiteral("zoom"),
                   QJsonObject{{QStringLiteral("note"), session.noteZoom()},
                               {QStringLiteral("source"), session.sourceZoom()},
                               {QStringLiteral("interface"), session.interfaceZoom()},
                               {QStringLiteral("book"), session.bookZoom()}}},
                  {QStringLiteral("windowGeometry"),
                   QString::fromLatin1(session.windowGeometry().toBase64())},
                  {QStringLiteral("splitterState"),
                   QString::fromLatin1(session.splitterState().toBase64())},
                  {QStringLiteral("panelsHidden"), session.panelsHidden()},
                  {QStringLiteral("markdownMode"), session.markdownMode()},
                  {QStringLiteral("historyListWidth"), session.historyListWidth()},
                  {QStringLiteral("exportDir"), session.exportDir()},
                  {QStringLiteral("exportKeepMeta"), session.exportKeepMeta()},
                  {QStringLiteral("expandedDirs"), expanded},
                  {QStringLiteral("searchHistory"), searches},
                  {QStringLiteral("searchRegex"), session.searchRegex()},
                  {QStringLiteral("carets"), carets},
                  {QStringLiteral("stores"), stores.storesToJson()},
    };
    QDir().mkpath(QFileInfo(path()).absolutePath());
    QFile file(path());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return;
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));

}

}  // namespace zametti
