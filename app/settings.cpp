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
        {QStringLiteral("headingScale"), headings},
        {QStringLiteral("fallbackScale"), a.fallbackScale},
    };

    QJsonObject layout{
        {QStringLiteral("lineHeightFactor"), a.lineHeightFactor},
        {QStringLiteral("listLineHeightFactor"), a.listLineHeightFactor},
        {QStringLiteral("blockSpacing"), a.blockSpacing},
        {QStringLiteral("sideMargin"), a.sideMargin},
        {QStringLiteral("verticalMargin"), a.verticalMargin},
    };

    QJsonObject colors{
        {QStringLiteral("pageBackground"), colorToString(a.pageBackground)},
        {QStringLiteral("selectionBackground"), colorToString(a.selectionBackground)},
        {QStringLiteral("link"), colorToString(a.linkColor)},
        {QStringLiteral("listMarker"), colorToString(a.markerColor)},
        {QStringLiteral("quote"), colorToString(a.quoteColor)},
        {QStringLiteral("rawSource"), colorToString(a.rawColor)},
        {QStringLiteral("codeBackground"), colorToString(a.codeBackground)},
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

    QJsonObject zoom{
        {QStringLiteral("step"), a.zoomStep},
        {QStringLiteral("min"), a.zoomMin},
        {QStringLiteral("max"), a.zoomMax},
    };

    return QJsonObject{
        {QStringLiteral("font"), font},
        {QStringLiteral("layout"), layout},
        {QStringLiteral("colors"), colors},
        {QStringLiteral("checkbox"), checkbox},
        {QStringLiteral("zoom"), zoom},
    };
}

void appearanceFromJson(const QJsonObject& root, Appearance& a) {
    const QJsonObject font = root.value(QStringLiteral("font")).toObject();
    readString(font, "family", a.fontFamily);
    readReal(font, "pointSize", a.baseFontPoint);
    readString(font, "symbolFamily", a.symbolFamily);
    readReal(font, "fallbackScale", a.fallbackScale);
    const QJsonArray headings = font.value(QStringLiteral("headingScale")).toArray();
    for (int i = 0; i < headings.size() && i < int(a.headingScale.size()); ++i)
        if (headings.at(i).isDouble()) a.headingScale[size_t(i)] = headings.at(i).toDouble();

    const QJsonObject layout = root.value(QStringLiteral("layout")).toObject();
    readReal(layout, "lineHeightFactor", a.lineHeightFactor);
    readReal(layout, "listLineHeightFactor", a.listLineHeightFactor);
    readReal(layout, "blockSpacing", a.blockSpacing);
    readReal(layout, "sideMargin", a.sideMargin);
    readReal(layout, "verticalMargin", a.verticalMargin);

    const QJsonObject colors = root.value(QStringLiteral("colors")).toObject();
    readColor(colors, "pageBackground", a.pageBackground);
    readColor(colors, "selectionBackground", a.selectionBackground);
    readColor(colors, "link", a.linkColor);
    readColor(colors, "listMarker", a.markerColor);
    readColor(colors, "quote", a.quoteColor);
    readColor(colors, "rawSource", a.rawColor);
    readColor(colors, "codeBackground", a.codeBackground);

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

    // Файл переписывается всегда: так в нём появляются параметры, добавленные в
    // новой версии, а править его дальше можно с полным списком перед глазами.
    writeJson(path, appearanceToJson(g_appearance));
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
    return session;
}

void saveSession(const Session& session) {
    writeJson(statePath(),
              QJsonObject{
                  {QStringLiteral("lastFile"), session.lastFile},
                  {QStringLiteral("scrollRatio"), session.scrollRatio},
                  {QStringLiteral("zoom"), session.zoom},
                  {QStringLiteral("windowGeometry"),
                   QString::fromLatin1(session.windowGeometry.toBase64())},
              });
}

}  // namespace zametti
