#include "zbookmarks.h"

#include "times.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <algorithm>
#include <map>

namespace zametti {
namespace {
// Keys, named once.
constexpr char kVersionKey[] = "version";
constexpr char kListKey[] = "bookmarks";
constexpr char kIdKey[] = "id";
constexpr char kNoteKey[] = "note";
constexpr char kSnippetKey[] = "snippet";
constexpr char kHeadingKey[] = "heading";
constexpr char kLineKey[] = "line";
constexpr char kNameKey[] = "name";
constexpr char kCreatedKey[] = "created";
constexpr char kUpdatedKey[] = "updated";
constexpr char kDeletedKey[] = "deleted";

QJsonObject toJson(const ZBookmarks::Entry& e) {
    QJsonObject o = e.extra;
    o.insert(QLatin1String(kIdKey), e.id);
    o.insert(QLatin1String(kNoteKey), e.note);
    o.insert(QLatin1String(kSnippetKey), e.snippet);
    if (!e.heading.isEmpty()) o.insert(QLatin1String(kHeadingKey), e.heading);
    o.insert(QLatin1String(kLineKey), e.line);
    if (!e.name.isEmpty()) o.insert(QLatin1String(kNameKey), e.name);
    if (!e.created.isEmpty()) o.insert(QLatin1String(kCreatedKey), e.created);
    if (!e.updated.isEmpty()) o.insert(QLatin1String(kUpdatedKey), e.updated);
    if (e.deleted) o.insert(QLatin1String(kDeletedKey), true);
    return o;
}

bool fromJson(QJsonObject o, ZBookmarks::Entry* out) {
    ZBookmarks::Entry e;
    e.id = o.value(QLatin1String(kIdKey)).toString();
    if (e.id.isEmpty()) return false;
    e.note = o.value(QLatin1String(kNoteKey)).toString();
    e.snippet = o.value(QLatin1String(kSnippetKey)).toString();
    e.heading = o.value(QLatin1String(kHeadingKey)).toString();
    e.line = o.value(QLatin1String(kLineKey)).toInt(0);
    e.name = o.value(QLatin1String(kNameKey)).toString();
    e.created = o.value(QLatin1String(kCreatedKey)).toString();
    e.updated = o.value(QLatin1String(kUpdatedKey)).toString();
    e.deleted = o.value(QLatin1String(kDeletedKey)).toBool(false);
    for (const char* key : {kIdKey, kNoteKey, kSnippetKey, kHeadingKey, kLineKey, kNameKey,
                            kCreatedKey, kUpdatedKey, kDeletedKey})
        o.remove(QLatin1String(key));
    e.extra = o;
    *out = std::move(e);
    return true;
}

// Which of two records of the same id survives a merge.
bool laterThan(const ZBookmarks::Entry& a, const ZBookmarks::Entry& b) {
    const QString ta = store::comparableTime(a.updated);
    const QString tb = store::comparableTime(b.updated);
    if (ta != tb) return ta > tb;
    // The same stamp: a tombstone wins (a deletion is the later intent),
    // then the deterministic tie-break by content.
    if (a.deleted != b.deleted) return a.deleted;
    return QJsonDocument(toJson(a)).toJson() > QJsonDocument(toJson(b)).toJson();
}
}  // namespace

bool ZBookmarks::parse(const QByteArray& bytes, QString* error) {
    QJsonParseError problem{};
    const QJsonDocument doc = QJsonDocument::fromJson(bytes, &problem);
    if (problem.error != QJsonParseError::NoError) {
        if (error != nullptr)
            *error = QStringLiteral("%1 is not JSON: %2").arg(QLatin1String(kFile), problem.errorString());
        return false;
    }
    if (!doc.isObject()) {
        if (error != nullptr) *error = QStringLiteral("%1 is not an object").arg(QLatin1String(kFile));
        return false;
    }
    QJsonObject object = doc.object();
    const QJsonValue version = object.value(QLatin1String(kVersionKey));
    if (version.isDouble() && version.toInt(kFormatVersion) > kFormatVersion) {
        if (error != nullptr)
            *error = QStringLiteral("%1: version %2 is newer than this build knows (%3)")
                         .arg(QLatin1String(kFile)).arg(version.toInt()).arg(kFormatVersion);
        return false;
    }
    std::vector<Entry> entries;
    for (const QJsonValue& v : object.value(QLatin1String(kListKey)).toArray()) {
        Entry e;
        // A record without an id is nobody's: skipped, not fatal — a hand
        // edit must not take the whole file down.
        if (v.isObject() && fromJson(v.toObject(), &e)) entries.push_back(std::move(e));
    }
    object.remove(QLatin1String(kVersionKey));
    object.remove(QLatin1String(kListKey));
    extra_ = object;
    entries_ = std::move(entries);
    return true;
}

QByteArray ZBookmarks::toBytes() const {
    QJsonObject object = extra_;
    object.insert(QLatin1String(kVersionKey), kFormatVersion);
    QJsonArray list;
    for (const Entry& e : entries_) list.append(toJson(e));
    object.insert(QLatin1String(kListKey), list);
    return QJsonDocument(object).toJson(QJsonDocument::Indented);
}

std::vector<ZBookmarks::Entry> ZBookmarks::forNote(const QString& noteId) const {
    std::vector<Entry> out;
    for (const Entry& e : entries_)
        if (!e.deleted && e.note == noteId) out.push_back(e);
    return out;
}

const ZBookmarks::Entry* ZBookmarks::find(const QString& id) const {
    for (const Entry& e : entries_)
        if (e.id == id) return &e;
    return nullptr;
}

void ZBookmarks::set(const Entry& entry) {
    for (Entry& e : entries_) {
        if (e.id == entry.id) {
            e = entry;
            return;
        }
    }
    entries_.push_back(entry);
}

bool ZBookmarks::remove(const QString& id, const QString& updatedIso) {
    for (Entry& e : entries_) {
        if (e.id != id) continue;
        if (e.deleted) return false;
        e.deleted = true;
        e.updated = updatedIso;
        return true;
    }
    return false;
}

ZBookmarks ZBookmarks::mergedWith(const ZBookmarks& other) const {
    // Ordered by id: the result does not depend on which side is "this".
    std::map<QString, Entry> byId;
    const auto take = [&byId](const Entry& e) {
        auto it = byId.find(e.id);
        if (it == byId.end()) {
            byId.emplace(e.id, e);
            return;
        }
        if (laterThan(e, it->second)) it->second = e;
    };
    for (const Entry& e : entries_) take(e);
    for (const Entry& e : other.entries_) take(e);
    ZBookmarks out;
    // The extra keys of both files: ours, then theirs where ours has none.
    out.extra_ = other.extra_;
    for (auto it = extra_.begin(); it != extra_.end(); ++it) out.extra_.insert(it.key(), it.value());
    for (auto& [id, e] : byId) out.entries_.push_back(std::move(e));
    return out;
}

}  // namespace zametti
