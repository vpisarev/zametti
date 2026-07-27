#include "settings.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QStandardPaths>

namespace zametti {
namespace {

Appearance g_appearance;

QString colorToString(const QColor& c) {
    return c.alpha() == 255 ? c.name(QColor::HexRgb) : c.name(QColor::HexArgb);
}

// Значение из объекта, если оно там есть и нужного вида. Чужие и битые ключи
// молча пропускаем: конфиг правят руками, и опечатка в одном параметре не
// должна ронять остальные.
void readReal(const QJsonObject& o, const char* key, qreal& out) {
    const QJsonValue v = o.value(QLatin1String(key));
    if (v.isDouble()) out = v.toDouble();
}

void readString(const QJsonObject& o, const char* key, QString& out) {
    const QJsonValue v = o.value(QLatin1String(key));
    if (v.isString()) out = v.toString();
}

void readColor(const QJsonObject& o, const char* key, QColor& out) {
    const QJsonValue v = o.value(QLatin1String(key));
    if (!v.isString()) return;
    const QColor parsed = QColor::fromString(v.toString());
    if (parsed.isValid()) out = parsed;
}

QString styleToString(CheckboxStyle s) {
    switch (s) {
        case CheckboxStyle::Glyph: return QStringLiteral("glyph");
        case CheckboxStyle::Ascii: return QStringLiteral("ascii");
        case CheckboxStyle::Drawn: return QStringLiteral("drawn");
    }
    return QStringLiteral("drawn");
}

void readStyle(const QJsonObject& o, const char* key, CheckboxStyle& out) {
    const QJsonValue v = o.value(QLatin1String(key));
    if (!v.isString()) return;
    const QString s = v.toString();
    if (s == QLatin1String("glyph")) out = CheckboxStyle::Glyph;
    else if (s == QLatin1String("ascii")) out = CheckboxStyle::Ascii;
    else if (s == QLatin1String("drawn")) out = CheckboxStyle::Drawn;
}

QJsonObject appearanceToJson(const Appearance& a) {
    QJsonArray headings;
    for (qreal v : a.headingScale) headings.append(v);

    QJsonObject font{
        {QStringLiteral("family"), a.fontFamily},
        {QStringLiteral("pointSize"), a.baseFontPoint},
        {QStringLiteral("symbolFamily"), a.symbolFamily},
        {QStringLiteral("codeFamily"), a.codeFamily},
        {QStringLiteral("codePointSize"), a.codePointSize},
        {QStringLiteral("headingScale"), headings},
        {QStringLiteral("fallbackScale"), a.fallbackScale},
    };

    QJsonObject layout{
        {QStringLiteral("lineHeightFactor"), a.lineHeightFactor},
        {QStringLiteral("listLineHeightFactor"), a.listLineHeightFactor},
        {QStringLiteral("blockSpacing"), a.blockSpacing},
        {QStringLiteral("listSpacingBefore"), a.listSpacingBefore},
        {QStringLiteral("listSpacingAfter"), a.listSpacingAfter},
        {QStringLiteral("headingSpacingFactor"), a.headingSpacingFactor},
        {QStringLiteral("listIndent"), a.listIndent},
        {QStringLiteral("codeIndent"), a.codeIndent},
        {QStringLiteral("quoteIndent"), a.quoteIndent},
        {QStringLiteral("sideMargin"), a.sideMargin},
        {QStringLiteral("verticalMargin"), a.verticalMargin},
        {QStringLiteral("maxContentWidth"), a.maxContentWidth},
    };

    QJsonObject colors{
        {QStringLiteral("pageBackground"), colorToString(a.pageBackground)},
        {QStringLiteral("selectionBackground"), colorToString(a.selectionBackground)},
        {QStringLiteral("link"), colorToString(a.linkColor)},
        {QStringLiteral("quote"), colorToString(a.quoteColor)},
        {QStringLiteral("rawSource"), colorToString(a.rawColor)},
        {QStringLiteral("codeBackground"), colorToString(a.codeBackground)},
    };

    QJsonObject list{
        {QStringLiteral("bulletColor"), colorToString(a.bulletColor)},
        {QStringLiteral("orderedColor"), colorToString(a.orderedColor)},
        {QStringLiteral("bulletStyle"),
         a.bulletStyle == BulletStyle::Glyph ? QStringLiteral("glyph")
                                             : QStringLiteral("drawn")},
        {QStringLiteral("bulletDiameter"), a.bulletDiameter},
        {QStringLiteral("bulletRise"), a.bulletRise},
        {QStringLiteral("orderedRise"), a.orderedRise},
        {QStringLiteral("bullet"), a.bulletGlyph},
        {QStringLiteral("bulletScale"), a.bulletScale},
        {QStringLiteral("bulletTextGap"), a.bulletTextGap},
        {QStringLiteral("orderedTextGap"), a.orderedTextGap},
    };

    QJsonObject checkbox{
        {QStringLiteral("style"), styleToString(a.checkboxStyle)},
        {QStringLiteral("checkedColor"), colorToString(a.checkboxCheckedColor)},
        {QStringLiteral("uncheckedColor"), colorToString(a.checkboxUncheckedColor)},
        {QStringLiteral("tickColor"), colorToString(a.checkboxTickColor)},
        {QStringLiteral("penWidth"), a.checkboxPenWidth},
        {QStringLiteral("cornerRadius"), a.checkboxCornerRadius},
        {QStringLiteral("opticalRise"), a.checkboxOpticalRise},
        {QStringLiteral("glyphScale"), a.checkboxGlyphScale},
        {QStringLiteral("textGap"), a.checkboxTextGap},
    };

    QJsonObject notes{
        {QStringLiteral("root"), a.notesRoot},
    };

    QJsonObject sidebar{
        {QStringLiteral("fontFamily"), a.sidebarFontFamily},
        {QStringLiteral("fontSize"), a.sidebarFontPoint},
        {QStringLiteral("lineHeightFactor"), a.sidebarLineHeightFactor},
        {QStringLiteral("width"), a.sidebarWidth},
        {QStringLiteral("folderColor"), colorToString(a.sidebarFolderColor)},
        {QStringLiteral("folderFamily"), a.sidebarFolderFamily},
        {QStringLiteral("folderClosed"), a.sidebarFolderClosed},
        {QStringLiteral("folderOpen"), a.sidebarFolderOpen},
        {QStringLiteral("folderScale"), a.sidebarFolderScale},
    };

    QJsonObject editor{
        {QStringLiteral("autosaveDelayMs"), a.autosaveDelayMs},
        {QStringLiteral("undoCoalesceMs"), a.undoCoalesceMs},
        {QStringLiteral("undoLimit"), a.undoLimit},
        {QStringLiteral("toggleTaskKey"), a.toggleTaskKey},
    };

    QJsonObject zoom{
        {QStringLiteral("step"), a.zoomStep},
        {QStringLiteral("min"), a.zoomMin},
        {QStringLiteral("max"), a.zoomMax},
    };

    return QJsonObject{
        {QStringLiteral("font"), font},
        {QStringLiteral("layout"), layout},
        {QStringLiteral("colors"), colors},
        {QStringLiteral("list"), list},
        {QStringLiteral("checkbox"), checkbox},
        {QStringLiteral("notes"), notes},
        {QStringLiteral("sidebar"), sidebar},
        {QStringLiteral("editor"), editor},
        {QStringLiteral("zoom"), zoom},
    };
}

void appearanceFromJson(const QJsonObject& root, Appearance& a) {
    const QJsonObject font = root.value(QStringLiteral("font")).toObject();
    readString(font, "family", a.fontFamily);
    readReal(font, "pointSize", a.baseFontPoint);
    readString(font, "symbolFamily", a.symbolFamily);
    readString(font, "codeFamily", a.codeFamily);
    readReal(font, "codePointSize", a.codePointSize);
    readReal(font, "fallbackScale", a.fallbackScale);
    const QJsonArray headings = font.value(QStringLiteral("headingScale")).toArray();
    for (int i = 0; i < headings.size() && i < int(a.headingScale.size()); ++i)
        if (headings.at(i).isDouble()) a.headingScale[size_t(i)] = headings.at(i).toDouble();

    const QJsonObject layout = root.value(QStringLiteral("layout")).toObject();
    readReal(layout, "lineHeightFactor", a.lineHeightFactor);
    readReal(layout, "listLineHeightFactor", a.listLineHeightFactor);
    readReal(layout, "blockSpacing", a.blockSpacing);
    readReal(layout, "listSpacingBefore", a.listSpacingBefore);
    readReal(layout, "listSpacingAfter", a.listSpacingAfter);
    readReal(layout, "headingSpacingFactor", a.headingSpacingFactor);
    readReal(layout, "listIndent", a.listIndent);
    readReal(layout, "codeIndent", a.codeIndent);
    readReal(layout, "quoteIndent", a.quoteIndent);
    readReal(layout, "sideMargin", a.sideMargin);
    readReal(layout, "verticalMargin", a.verticalMargin);
    readReal(layout, "maxContentWidth", a.maxContentWidth);

    const QJsonObject colors = root.value(QStringLiteral("colors")).toObject();
    readColor(colors, "pageBackground", a.pageBackground);
    readColor(colors, "selectionBackground", a.selectionBackground);
    readColor(colors, "link", a.linkColor);
    readColor(colors, "quote", a.quoteColor);
    readColor(colors, "rawSource", a.rawColor);
    readColor(colors, "codeBackground", a.codeBackground);

    const QJsonObject list = root.value(QStringLiteral("list")).toObject();
    readColor(list, "bulletColor", a.bulletColor);
    readColor(list, "orderedColor", a.orderedColor);
    const QJsonValue bulletStyle = list.value(QStringLiteral("bulletStyle"));
    if (bulletStyle.isString()) {
        a.bulletStyle = bulletStyle.toString() == QLatin1String("glyph") ? BulletStyle::Glyph
                                                                        : BulletStyle::Drawn;
    }
    readReal(list, "bulletDiameter", a.bulletDiameter);
    readReal(list, "bulletRise", a.bulletRise);
    readReal(list, "orderedRise", a.orderedRise);
    readString(list, "bullet", a.bulletGlyph);
    readReal(list, "bulletScale", a.bulletScale);
    readReal(list, "bulletTextGap", a.bulletTextGap);
    readReal(list, "orderedTextGap", a.orderedTextGap);

    const QJsonObject checkbox = root.value(QStringLiteral("checkbox")).toObject();
    readStyle(checkbox, "style", a.checkboxStyle);
    readColor(checkbox, "checkedColor", a.checkboxCheckedColor);
    readColor(checkbox, "uncheckedColor", a.checkboxUncheckedColor);
    readColor(checkbox, "tickColor", a.checkboxTickColor);
    readReal(checkbox, "penWidth", a.checkboxPenWidth);
    readReal(checkbox, "cornerRadius", a.checkboxCornerRadius);
    readReal(checkbox, "opticalRise", a.checkboxOpticalRise);
    readReal(checkbox, "glyphScale", a.checkboxGlyphScale);
    readReal(checkbox, "textGap", a.checkboxTextGap);

    const QJsonObject notes = root.value(QStringLiteral("notes")).toObject();
    readString(notes, "root", a.notesRoot);

    const QJsonObject sidebar = root.value(QStringLiteral("sidebar")).toObject();
    readString(sidebar, "fontFamily", a.sidebarFontFamily);
    readReal(sidebar, "fontSize", a.sidebarFontPoint);
    readReal(sidebar, "lineHeightFactor", a.sidebarLineHeightFactor);
    readColor(sidebar, "folderColor", a.sidebarFolderColor);
    readString(sidebar, "folderFamily", a.sidebarFolderFamily);
    readString(sidebar, "folderClosed", a.sidebarFolderClosed);
    readString(sidebar, "folderOpen", a.sidebarFolderOpen);
    readReal(sidebar, "folderScale", a.sidebarFolderScale);
    const QJsonValue width = sidebar.value(QStringLiteral("width"));
    if (width.isDouble()) a.sidebarWidth = width.toInt();

    const QJsonObject editor = root.value(QStringLiteral("editor")).toObject();
    const QJsonValue delay = editor.value(QStringLiteral("autosaveDelayMs"));
    if (delay.isDouble()) a.autosaveDelayMs = delay.toInt();
    const QJsonValue coalesce = editor.value(QStringLiteral("undoCoalesceMs"));
    if (coalesce.isDouble()) a.undoCoalesceMs = coalesce.toInt();
    const QJsonValue limit = editor.value(QStringLiteral("undoLimit"));
    if (limit.isDouble()) a.undoLimit = limit.toInt();
    readString(editor, "toggleTaskKey", a.toggleTaskKey);

    const QJsonObject zoom = root.value(QStringLiteral("zoom")).toObject();
    readReal(zoom, "step", a.zoomStep);
    readReal(zoom, "min", a.zoomMin);
    readReal(zoom, "max", a.zoomMax);
}

bool writeJson(const QString& path, const QJsonObject& root) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    return true;
}

}  // namespace

Appearance& appearance() { return g_appearance; }

QByteArray defaultAppearanceJson() {
    return QJsonDocument(appearanceToJson(Appearance{})).toJson(QJsonDocument::Indented);
}

QString configPath() {
    return QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) +
           QStringLiteral("/config.json");
}

QString statePath() {
    return QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) +
           QStringLiteral("/state.json");
}

bool loadAppearance(QString* error) {
    const QString path = configPath();
    QFile file(path);

    if (file.exists()) {
        if (!file.open(QIODevice::ReadOnly)) {
            if (error != nullptr) *error = QStringLiteral("не читается: ") + path;
            return false;
        }
        QJsonParseError parseError{};
        const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
        file.close();
        if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
            if (error != nullptr)
                *error = path + QStringLiteral(": ") + parseError.errorString();
            return false;
        }
        appearanceFromJson(doc.object(), g_appearance);
    }
    return true;
}

Session loadSession() {
    Session session;
    QFile file(statePath());
    if (!file.open(QIODevice::ReadOnly)) return session;

    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isObject()) return session;
    const QJsonObject root = doc.object();

    session.lastFile = root.value(QStringLiteral("lastFile")).toString();
    session.scrollRatio = root.value(QStringLiteral("scrollRatio")).toDouble(0.0);
    session.zoom = root.value(QStringLiteral("zoom")).toDouble(1.0);
    session.windowGeometry = QByteArray::fromBase64(
        root.value(QStringLiteral("windowGeometry")).toString().toLatin1());
    session.splitterState = QByteArray::fromBase64(
        root.value(QStringLiteral("splitterState")).toString().toLatin1());
    for (const QJsonValue& v : root.value(QStringLiteral("expandedDirs")).toArray())
        if (v.isString()) session.expandedDirs.append(v.toString());
    return session;
}

void saveSession(const Session& session) {
    QJsonArray expanded;
    for (const QString& dir : session.expandedDirs) expanded.append(dir);

    writeJson(statePath(),
              QJsonObject{
                  {QStringLiteral("lastFile"), session.lastFile},
                  {QStringLiteral("scrollRatio"), session.scrollRatio},
                  {QStringLiteral("zoom"), session.zoom},
                  {QStringLiteral("windowGeometry"),
                   QString::fromLatin1(session.windowGeometry.toBase64())},
                  {QStringLiteral("splitterState"),
                   QString::fromLatin1(session.splitterState.toBase64())},
                  {QStringLiteral("expandedDirs"), expanded},
              });
}

}  // namespace zametti
