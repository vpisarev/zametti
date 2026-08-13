#include "settings.h"

#include <QDir>
#include <QFile>
#include <QFont>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QImageReader>
#include <QScreen>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QStandardPaths>

#include <algorithm>
#include <cctype>
#include <cmath>

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

void readInt(const QJsonObject& o, const char* key, int& out) {
    const QJsonValue v = o.value(QLatin1String(key));
    if (v.isDouble()) out = v.toInt();
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
    QJsonArray shapes;
    for (BulletShape shape : a.bulletShapes)
        shapes.append(shape == BulletShape::Circle   ? QStringLiteral("circle")
                      : shape == BulletShape::Square ? QStringLiteral("square")
                                                     : QStringLiteral("disc"));

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

    QJsonObject tables{
        {QStringLiteral("cellPadding"), a.tables.cellPadding},
        {QStringLiteral("cellPaddingY"), a.tables.cellPaddingY},
        {QStringLiteral("borderColor"), colorToString(a.tables.borderColor)},
        {QStringLiteral("horizontalBorder"), a.tables.horizontalBorder},
        {QStringLiteral("verticalBorder"), a.tables.verticalBorder},
        {QStringLiteral("headerSeparator"), a.tables.headerSeparator},
        {QStringLiteral("rowSeparator"), a.tables.rowSeparator},
        {QStringLiteral("columnSeparator"), a.tables.columnSeparator},
        {QStringLiteral("headerColor"), colorToString(a.tables.headerColor)},
        {QStringLiteral("tableColor"), colorToString(a.tables.tableColor)},
        {QStringLiteral("altTableColor"), colorToString(a.tables.altTableColor)},
    };

    QJsonArray paperHeadings;
    for (qreal v : a.pdf.headingScale) paperHeadings.append(v);
    QJsonObject pdf{
        {QStringLiteral("fontFamily"), a.pdf.fontFamily},
        {QStringLiteral("pointSize"), a.pdf.pointSize},
        {QStringLiteral("codeFamily"), a.pdf.codeFamily},
        {QStringLiteral("codePointSize"), a.pdf.codePointSize},
        {QStringLiteral("headingScale"), paperHeadings},
        {QStringLiteral("marginMm"), a.pdf.marginMm},
        {QStringLiteral("imageDpi"), a.pdf.imageDpi},
        {QStringLiteral("maxExportedImageSize"), a.pdf.maxExportedImageSize},
        {QStringLiteral("codeStripHeight"), a.pdf.codeStripHeight},
    };

    QJsonObject layout{
        {QStringLiteral("lineHeightFactor"), a.lineHeightFactor},
        {QStringLiteral("listLineHeightFactor"), a.listLineHeightFactor},
        {QStringLiteral("blockSpacing"), a.blockSpacing},
        {QStringLiteral("listIndent"), a.listIndent},
        {QStringLiteral("codeIndent"), a.codeIndent},
        {QStringLiteral("codePadLeft"), a.codePadLeft},
        {QStringLiteral("codeStripHeight"), a.codeStripHeight},
        {QStringLiteral("codePadTop"), a.codePadTop},
        {QStringLiteral("codeCornerRadius"), a.codeCornerRadius},
        {QStringLiteral("codeLangPointSize"), a.codeLangPointSize},
        {QStringLiteral("codeStripPadding"), a.codeStripPadding},
        {QStringLiteral("codeLangGap"), a.codeLangGap},
        {QStringLiteral("quoteIndent"), a.quoteIndent},
        {QStringLiteral("sideMargin"), a.sideMargin},
        {QStringLiteral("verticalMargin"), a.verticalMargin},
        {QStringLiteral("maxContentWidth"), a.maxContentWidth},
        {QStringLiteral("caretWidth"), a.caretWidth},
        {QStringLiteral("dividerWidth"), a.dividerWidth},
    };

    QJsonObject colors{
        {QStringLiteral("pageBackground"), colorToString(a.pageBackground)},
        {QStringLiteral("historyBackground"), colorToString(a.historyBackground)},
        {QStringLiteral("selectionBackground"), colorToString(a.selectionBackground)},
        {QStringLiteral("searchHighlight"), colorToString(a.searchHighlight)},
        {QStringLiteral("diffAdded"), colorToString(a.diffAdded)},
        {QStringLiteral("diffRemoved"), colorToString(a.diffRemoved)},
        {QStringLiteral("diffChanged"), colorToString(a.diffChanged)},
        {QStringLiteral("link"), colorToString(a.linkColor)},
        {QStringLiteral("quote"), colorToString(a.quoteColor)},
        {QStringLiteral("rawSource"), colorToString(a.rawColor)},
        {QStringLiteral("divider"), colorToString(a.dividerColor)},
        {QStringLiteral("codeBackground"), colorToString(a.codeBackground)},
        {QStringLiteral("codeLang"), colorToString(a.codeLangColor)},
        {QStringLiteral("caret"), colorToString(a.caretColor)},
    };

    QJsonObject list{
        {QStringLiteral("bulletColor"), colorToString(a.bulletColor)},
        {QStringLiteral("orderedColor"), colorToString(a.orderedColor)},
        {QStringLiteral("bulletStyle"),
         a.bulletStyle == BulletStyle::Glyph ? QStringLiteral("glyph")
                                             : QStringLiteral("drawn")},
        {QStringLiteral("bulletDiameter"), a.bulletDiameter},
        {QStringLiteral("bulletStrokeWidth"), a.bulletStrokeWidth},
        {QStringLiteral("bulletSquareSide"), a.bulletSquareSide},
        {QStringLiteral("bulletShapes"), shapes},
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
        {QStringLiteral("title"), a.storeTitle},
        {QStringLiteral("watchFolder"), a.watchStore},
    };

    QJsonObject noteList{
        {QStringLiteral("width"), a.noteListWidth},
        {QStringLiteral("snippetLines"), a.noteListSnippetLines},
        {QStringLiteral("snippetColor"), colorToString(a.noteListSnippetColor)},
        {QStringLiteral("dateColor"), colorToString(a.noteListDateColor)},
    };

    QJsonObject sidebar{
        {QStringLiteral("fontFamily"), a.sidebarFontFamily},
        {QStringLiteral("fontSize"), a.sidebarFontPoint},
        {QStringLiteral("lineHeightFactor"), a.sidebarLineHeightFactor},
        {QStringLiteral("width"), a.sidebarWidth},
        {QStringLiteral("folderColor"), colorToString(a.sidebarFolderColor)},
        {QStringLiteral("folderScale"), a.sidebarFolderScale},
    };

    QJsonObject imageSelection{
        {QStringLiteral("cornerShare"), a.imageCornerShare},
        {QStringLiteral("cornerMinLength"), a.imageCornerMinLength},
        {QStringLiteral("cornerWidth"), a.imageCornerWidth},
        {QStringLiteral("cornerOffset"), a.imageCornerOffset},
    };

    QJsonObject statusBar{
        {QStringLiteral("family"), a.statusFamily},
        {QStringLiteral("fontPoints"), a.statusFontPoints},
        {QStringLiteral("padding"), a.statusPadding},
        {QStringLiteral("paddingTop"), a.statusPaddingTop},
        {QStringLiteral("background"), colorToString(a.statusBackground)},
        {QStringLiteral("textColor"), colorToString(a.statusTextColor)},
        {QStringLiteral("separatorColor"), colorToString(a.statusSeparatorColor)},
    };

    QJsonObject toolbar{
        {QStringLiteral("iconSize"), a.toolbarIconSize},
        {QStringLiteral("buttonPadding"), a.toolbarButtonPadding},
        {QStringLiteral("groupSpacing"), a.toolbarGroupSpacing},
        {QStringLiteral("background"), colorToString(a.toolbarBackground)},
        {QStringLiteral("iconColor"), colorToString(a.toolbarIconColor)},
        {QStringLiteral("iconHoverColor"), colorToString(a.toolbarIconHoverColor)},
        {QStringLiteral("iconOnColor"), colorToString(a.toolbarIconOnColor)},
        {QStringLiteral("iconMarkColor"), colorToString(a.toolbarIconMarkColor)},
        {QStringLiteral("iconDisabledColor"), colorToString(a.toolbarIconDisabledColor)},
        {QStringLiteral("hoverBackground"), colorToString(a.toolbarHoverBackground)},
        {QStringLiteral("separatorColor"), colorToString(a.toolbarSeparatorColor)},
    };

    QJsonObject find{
        {QStringLiteral("fontDelta"), a.findFontDelta},
        {QStringLiteral("previousGlyph"), a.findPreviousGlyph},
        {QStringLiteral("nextGlyph"), a.findNextGlyph},
        {QStringLiteral("historyGlyph"), a.findHistoryGlyph},
        {QStringLiteral("historyLimit"), a.findHistoryLimit},
    };

    QJsonArray special;
    for (const auto& [keys, text] : a.specialKeys) {
        QJsonArray pair;
        pair.append(keys);
        pair.append(text);
        special.append(pair);
    }

    QJsonObject editor{
        {QStringLiteral("autosaveDelayMs"), a.autosaveDelayMs},
        {QStringLiteral("undoCoalesceMs"), a.undoCoalesceMs},
        {QStringLiteral("undoLimit"), a.undoLimit},
        {QStringLiteral("undoRunChars"), a.undoRunChars},
        {QStringLiteral("historyMergeChars"), a.historyMergeChars},
        {QStringLiteral("historyMergeHours"), a.historyMergeHours},
        {QStringLiteral("undoBudgetMb"), a.undoBudgetMb},
        {QStringLiteral("imageCacheSizeMb"), a.imageCacheSizeMb},
        {QStringLiteral("maxLoadedImageSize"), a.maxLoadedImageSize},
        {QStringLiteral("documentCacheSizeMb"), a.documentCacheSizeMb},
        {QStringLiteral("toggleTaskKey"), a.toggleTaskKey},
        {QStringLiteral("moveUpKey"), a.moveUpKey},
        {QStringLiteral("moveDownKey"), a.moveDownKey},
        {QStringLiteral("makeBulletKey"), a.makeBulletKey},
        {QStringLiteral("makeOrderedKey"), a.makeOrderedKey},
        {QStringLiteral("makeTaskKey"), a.makeTaskKey},
        {QStringLiteral("makeParagraphKey"), a.makeParagraphKey},
        {QStringLiteral("makeCommentKey"), a.makeCommentKey},
        {QStringLiteral("codeTabWidth"), a.codeTabWidth},
        {QStringLiteral("special"), special},
        {QStringLiteral("externalEditor"), a.externalEditor},
    };

    // ТОЛЬКО S. Остальные числа импорта настройке не подлежат — решение
    // владельца, и вот его причина: они выведены замерами на большом корпусе и
    // СОГЛАСОВАНЫ МЕЖДУ СОБОЙ. Произвольная смена одного ломает логику
    // остальных — например, качество и порог пережатия подобраны так, чтобы
    // спор с исходником имел смысл; сдвинь одно, и правило станет либо
    // бесполезным, либо вредным. Значения живут в settings.h, и менять их
    // можно только правкой кода, то есть осознанно и с новым замером.
    QJsonObject images{
        {QStringLiteral("maxImportedImageSize"), a.images.maxImportedImageSize},
    };

    // Сочетания клавиш, которым суждено разойтись по системам. Пока их два —
    // ходьба по изменённым местам в истории: на маке F4 занята системой.
    QJsonObject shortcuts{
        {QStringLiteral("diffNext"), a.diffNextKey},
        {QStringLiteral("diffPrevious"), a.diffPreviousKey},
    };

    QJsonObject zoom{
        {QStringLiteral("step"), a.zoomStep},
        {QStringLiteral("min"), a.zoomMin},
        {QStringLiteral("max"), a.zoomMax},
    };

    return QJsonObject{
        {QStringLiteral("font"), font},
        {QStringLiteral("pdf"), pdf},
        {QStringLiteral("layout"), layout},
        {QStringLiteral("colors"), colors},
        {QStringLiteral("list"), list},
        {QStringLiteral("checkbox"), checkbox},
        {QStringLiteral("notes"), notes},
        {QStringLiteral("sidebar"), sidebar},
        {QStringLiteral("noteList"), noteList},
        {QStringLiteral("toolbar"), toolbar},
        {QStringLiteral("statusBar"), statusBar},
        {QStringLiteral("imageSelection"), imageSelection},
        {QStringLiteral("find"), find},
        {QStringLiteral("editor"), editor},
        {QStringLiteral("tables"), tables},
        {QStringLiteral("images"), images},
        {QStringLiteral("shortcuts"), shortcuts},
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

    const QJsonObject tables = root.value(QStringLiteral("tables")).toObject();
    readReal(tables, "cellPadding", a.tables.cellPadding);
    readReal(tables, "cellPaddingY", a.tables.cellPaddingY);
    readColor(tables, "borderColor", a.tables.borderColor);
    readReal(tables, "horizontalBorder", a.tables.horizontalBorder);
    readReal(tables, "verticalBorder", a.tables.verticalBorder);
    readReal(tables, "headerSeparator", a.tables.headerSeparator);
    readReal(tables, "rowSeparator", a.tables.rowSeparator);
    readReal(tables, "columnSeparator", a.tables.columnSeparator);
    readColor(tables, "headerColor", a.tables.headerColor);
    readColor(tables, "tableColor", a.tables.tableColor);
    readColor(tables, "altTableColor", a.tables.altTableColor);

    const QJsonObject paper = root.value(QStringLiteral("pdf")).toObject();
    readString(paper, "fontFamily", a.pdf.fontFamily);
    readReal(paper, "pointSize", a.pdf.pointSize);
    readString(paper, "codeFamily", a.pdf.codeFamily);
    readReal(paper, "codePointSize", a.pdf.codePointSize);
    readReal(paper, "marginMm", a.pdf.marginMm);
    readInt(paper, "imageDpi", a.pdf.imageDpi);
    readInt(paper, "maxExportedImageSize", a.pdf.maxExportedImageSize);
    readReal(paper, "codeStripHeight", a.pdf.codeStripHeight);
    const QJsonArray paperHeads = paper.value(QStringLiteral("headingScale")).toArray();
    for (int i = 0; i < paperHeads.size() && i < int(a.pdf.headingScale.size()); ++i)
        if (paperHeads.at(i).isDouble()) a.pdf.headingScale[size_t(i)] = paperHeads.at(i).toDouble();

    const QJsonObject layout = root.value(QStringLiteral("layout")).toObject();
    readReal(layout, "lineHeightFactor", a.lineHeightFactor);
    readReal(layout, "listLineHeightFactor", a.listLineHeightFactor);
    readReal(layout, "blockSpacing", a.blockSpacing);
    readReal(layout, "listIndent", a.listIndent);
    readReal(layout, "codeIndent", a.codeIndent);
    readReal(layout, "codePadLeft", a.codePadLeft);
    readReal(layout, "codeStripHeight", a.codeStripHeight);
    readReal(layout, "codePadTop", a.codePadTop);
    readReal(layout, "codeCornerRadius", a.codeCornerRadius);
    readReal(layout, "codeLangPointSize", a.codeLangPointSize);
    readReal(layout, "codeStripPadding", a.codeStripPadding);
    readReal(layout, "codeLangGap", a.codeLangGap);
    readReal(layout, "quoteIndent", a.quoteIndent);
    readReal(layout, "sideMargin", a.sideMargin);
    readReal(layout, "verticalMargin", a.verticalMargin);
    readReal(layout, "maxContentWidth", a.maxContentWidth);
    readReal(layout, "caretWidth", a.caretWidth);
    readReal(layout, "dividerWidth", a.dividerWidth);

    const QJsonObject colors = root.value(QStringLiteral("colors")).toObject();
    readColor(colors, "pageBackground", a.pageBackground);
    readColor(colors, "historyBackground", a.historyBackground);
    readColor(colors, "selectionBackground", a.selectionBackground);
    readColor(colors, "searchHighlight", a.searchHighlight);
    readColor(colors, "diffAdded", a.diffAdded);
    readColor(colors, "diffRemoved", a.diffRemoved);
    readColor(colors, "diffChanged", a.diffChanged);
    readColor(colors, "link", a.linkColor);
    readColor(colors, "quote", a.quoteColor);
    readColor(colors, "rawSource", a.rawColor);
    readColor(colors, "divider", a.dividerColor);
    readColor(colors, "codeBackground", a.codeBackground);
    readColor(colors, "codeLang", a.codeLangColor);
    readColor(colors, "caret", a.caretColor);

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
    readReal(list, "bulletStrokeWidth", a.bulletStrokeWidth);
    readReal(list, "bulletSquareSide", a.bulletSquareSide);

    // Фигуры по уровням — списком строк. Пустой список пропускаем: остаться
    // вовсе без фигур значит остаться без буллетов.
    const QJsonValue shapes = list.value(QStringLiteral("bulletShapes"));
    if (shapes.isArray()) {
        std::vector<BulletShape> parsed;
        for (const QJsonValue& value : shapes.toArray()) {
            const QString name = value.toString();
            if (name == QLatin1String("circle")) parsed.push_back(BulletShape::Circle);
            else if (name == QLatin1String("square")) parsed.push_back(BulletShape::Square);
            else if (name == QLatin1String("disc")) parsed.push_back(BulletShape::Disc);
        }
        if (!parsed.empty()) a.bulletShapes = std::move(parsed);
    }
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
    readString(notes, "title", a.storeTitle);
    const QJsonValue watch = notes.value(QStringLiteral("watchFolder"));
    if (watch.isBool()) a.watchStore = watch.toBool();

    const QJsonObject noteList = root.value(QStringLiteral("noteList")).toObject();
    const QJsonValue listWidth = noteList.value(QStringLiteral("width"));
    if (listWidth.isDouble()) a.noteListWidth = listWidth.toInt();
    const QJsonValue snippetLines = noteList.value(QStringLiteral("snippetLines"));
    if (snippetLines.isDouble()) a.noteListSnippetLines = qMax(0, snippetLines.toInt());
    readColor(noteList, "snippetColor", a.noteListSnippetColor);
    readColor(noteList, "dateColor", a.noteListDateColor);

    const QJsonObject sidebar = root.value(QStringLiteral("sidebar")).toObject();
    readString(sidebar, "fontFamily", a.sidebarFontFamily);
    readReal(sidebar, "fontSize", a.sidebarFontPoint);
    readReal(sidebar, "lineHeightFactor", a.sidebarLineHeightFactor);
    readColor(sidebar, "folderColor", a.sidebarFolderColor);
    readReal(sidebar, "folderScale", a.sidebarFolderScale);
    const QJsonValue width = sidebar.value(QStringLiteral("width"));
    if (width.isDouble()) a.sidebarWidth = width.toInt();

    const QJsonObject imageSelection =
        root.value(QStringLiteral("imageSelection")).toObject();
    const QJsonValue cornerShare = imageSelection.value(QStringLiteral("cornerShare"));
    if (cornerShare.isDouble()) a.imageCornerShare = std::clamp(cornerShare.toDouble(), 0.0, 0.5);
    const QJsonValue cornerMin = imageSelection.value(QStringLiteral("cornerMinLength"));
    if (cornerMin.isDouble()) a.imageCornerMinLength = qMax(0, cornerMin.toInt());
    const QJsonValue cornerWidth = imageSelection.value(QStringLiteral("cornerWidth"));
    if (cornerWidth.isDouble()) a.imageCornerWidth = qMax(0.5, cornerWidth.toDouble());
    const QJsonValue cornerOffset = imageSelection.value(QStringLiteral("cornerOffset"));
    if (cornerOffset.isDouble()) a.imageCornerOffset = qMax(0.0, cornerOffset.toDouble());

    const QJsonObject statusBar = root.value(QStringLiteral("statusBar")).toObject();
    const QJsonValue statusFamily = statusBar.value(QStringLiteral("family"));
    if (statusFamily.isString()) a.statusFamily = statusFamily.toString();
    const QJsonValue statusPoints = statusBar.value(QStringLiteral("fontPoints"));
    if (statusPoints.isDouble()) a.statusFontPoints = std::clamp(statusPoints.toInt(), 6, 24);
    const QJsonValue statusPadding = statusBar.value(QStringLiteral("padding"));
    if (statusPadding.isDouble()) a.statusPadding = qMax(0, statusPadding.toInt());
    const QJsonValue statusPaddingTop = statusBar.value(QStringLiteral("paddingTop"));
    if (statusPaddingTop.isDouble()) a.statusPaddingTop = qMax(0, statusPaddingTop.toInt());
    readColor(statusBar, "background", a.statusBackground);
    readColor(statusBar, "textColor", a.statusTextColor);
    readColor(statusBar, "separatorColor", a.statusSeparatorColor);

    const QJsonObject toolbar = root.value(QStringLiteral("toolbar")).toObject();
    const QJsonValue iconSize = toolbar.value(QStringLiteral("iconSize"));
    // Иконка меньше двенадцати точек перестаёт читаться, больше шестидесяти
    // ломает высоту тулбара. Границы не вкус, а пределы, за которыми настройка
    // портит окно, а не настраивает его.
    if (iconSize.isDouble()) a.toolbarIconSize = std::clamp(iconSize.toInt(), 12, 64);
    const QJsonValue buttonPadding = toolbar.value(QStringLiteral("buttonPadding"));
    if (buttonPadding.isDouble()) a.toolbarButtonPadding = qMax(0, buttonPadding.toInt());
    const QJsonValue groupSpacing = toolbar.value(QStringLiteral("groupSpacing"));
    if (groupSpacing.isDouble()) a.toolbarGroupSpacing = qMax(0, groupSpacing.toInt());
    readColor(toolbar, "background", a.toolbarBackground);
    readColor(toolbar, "iconColor", a.toolbarIconColor);
    readColor(toolbar, "iconHoverColor", a.toolbarIconHoverColor);
    readColor(toolbar, "iconOnColor", a.toolbarIconOnColor);
    readColor(toolbar, "iconMarkColor", a.toolbarIconMarkColor);
    readColor(toolbar, "iconDisabledColor", a.toolbarIconDisabledColor);
    readColor(toolbar, "hoverBackground", a.toolbarHoverBackground);
    readColor(toolbar, "separatorColor", a.toolbarSeparatorColor);

    const QJsonObject find = root.value(QStringLiteral("find")).toObject();
    readReal(find, "fontDelta", a.findFontDelta);
    readString(find, "previousGlyph", a.findPreviousGlyph);
    readString(find, "nextGlyph", a.findNextGlyph);
    readString(find, "historyGlyph", a.findHistoryGlyph);
    const QJsonValue historyLimit = find.value(QStringLiteral("historyLimit"));
    if (historyLimit.isDouble()) a.findHistoryLimit = qMax(0, historyLimit.toInt());

    const QJsonObject shortcuts = root.value(QStringLiteral("shortcuts")).toObject();
    readString(shortcuts, "diffNext", a.diffNextKey);
    readString(shortcuts, "diffPrevious", a.diffPreviousKey);

    const QJsonObject editor = root.value(QStringLiteral("editor")).toObject();
    const QJsonValue delay = editor.value(QStringLiteral("autosaveDelayMs"));
    if (delay.isDouble()) a.autosaveDelayMs = delay.toInt();
    const QJsonValue coalesce = editor.value(QStringLiteral("undoCoalesceMs"));
    if (coalesce.isDouble()) a.undoCoalesceMs = coalesce.toInt();
    readInt(editor, "undoRunChars", a.undoRunChars);
    readInt(editor, "codeTabWidth", a.codeTabWidth);
    readInt(editor, "historyMergeChars", a.historyMergeChars);
    readInt(editor, "historyMergeHours", a.historyMergeHours);
    const QJsonValue limit = editor.value(QStringLiteral("undoLimit"));
    if (limit.isDouble()) a.undoLimit = limit.toInt();
    const QJsonValue budget = editor.value(QStringLiteral("undoBudgetMb"));
    if (budget.isDouble()) a.undoBudgetMb = budget.toInt();
    const QJsonValue images = editor.value(QStringLiteral("imageCacheSizeMb"));
    if (images.isDouble()) a.imageCacheSizeMb = images.toInt();
    const QJsonValue loaded = editor.value(QStringLiteral("maxLoadedImageSize"));
    if (loaded.isDouble()) a.maxLoadedImageSize = loaded.toInt();
    const QJsonValue documents = editor.value(QStringLiteral("documentCacheSizeMb"));
    if (documents.isDouble()) a.documentCacheSizeMb = documents.toInt();
    readString(editor, "toggleTaskKey", a.toggleTaskKey);
    readString(editor, "moveUpKey", a.moveUpKey);
    readString(editor, "moveDownKey", a.moveDownKey);
    readString(editor, "makeBulletKey", a.makeBulletKey);
    readString(editor, "makeOrderedKey", a.makeOrderedKey);
    readString(editor, "makeTaskKey", a.makeTaskKey);
    readString(editor, "makeParagraphKey", a.makeParagraphKey);
    readString(editor, "makeCommentKey", a.makeCommentKey);
    // Автозамены: список пар [сочетание, что вставить]. Заданный список
    // заменяет умолчания целиком — иначе от умолчания было бы не избавиться.
    const QJsonValue special = editor.value(QStringLiteral("special"));
    if (special.isArray()) {
        a.specialKeys.clear();
        for (const QJsonValue& entry : special.toArray()) {
            const QJsonArray pair = entry.toArray();
            if (pair.size() != 2 || !pair.at(0).isString() || !pair.at(1).isString())
                continue;   // битую запись пропускаем, соседние живут
            const QString keys = pair.at(0).toString();
            if (keys.isEmpty()) continue;
            a.specialKeys.push_back({keys, pair.at(1).toString()});
        }
    }
    readString(editor, "externalEditor", a.externalEditor);

    // Имя своё, а не "images": в этой же области уже живёт значение ключа
    // editor.imageCacheSizeMb под этим именем.
    const QJsonObject importGroup = root.value(QStringLiteral("images")).toObject();
    readInt(importGroup, "maxImportedImageSize", a.images.maxImportedImageSize);

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

CodePlate codePlate(qreal zoom) {
    // Единицы те же, что у сборщика документа: по вертикали — высота строки
    // кода (гарнитура текста в кегле кода, как её считает document_builder),
    // по горизонтали — ширина "A" основного шрифта.
    QFont base{QString(g_appearance.fontFamily)};
    base.setPointSizeF(g_appearance.baseFontPoint * zoom);
    base.setStyleHint(QFont::Monospace);
    const qreal charUnit = QFontMetricsF(base).horizontalAdvance(QLatin1Char('A'));

    QFont codeLine = base;
    codeLine.setPointSizeF(g_appearance.codePointSize > 0.0
                               ? g_appearance.codePointSize * zoom
                               : g_appearance.baseFontPoint * zoom);
    const qreal lineUnit =
        std::round(QFontMetricsF(codeLine).height() * g_appearance.lineHeightFactor);

    CodePlate plate;
    plate.strip = std::round(g_appearance.codeStripHeight * lineUnit);
    plate.padTop = std::round(g_appearance.codePadTop * lineUnit);
    plate.padLeft = g_appearance.codePadLeft * charUnit;
    plate.indent = g_appearance.codeIndent * charUnit;
    plate.radius = g_appearance.codeCornerRadius * zoom;
    plate.stripPadding = g_appearance.codeStripPadding * charUnit;
    plate.langGap = g_appearance.codeLangGap * charUnit;
    return plate;
}

QFont codeLangFont(qreal zoom) {
    QFont font{QString(g_appearance.sidebarFontFamily)};
    const qreal point = g_appearance.codeLangPointSize > 0.0
                            ? g_appearance.codeLangPointSize
                            : g_appearance.sidebarFontPoint;
    font.setPointSizeF(point * zoom);
    return font;
}


QByteArray defaultAppearanceJson() {
    return QJsonDocument(appearanceToJson(Appearance{})).toJson(QJsonDocument::Indented);
}

namespace {

// Первый проход: комментарии.
QByteArray stripComments(const QByteArray& json) {
    QByteArray out;
    out.reserve(json.size());
    bool inString = false;
    bool escaped = false;
    for (int i = 0; i < json.size(); ++i) {
        const char c = json.at(i);
        if (inString) {
            out.append(c);
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') inString = false;
            continue;
        }
        if (c == '"') {
            inString = true;
            out.append(c);
            continue;
        }
        if (c == '/' && i + 1 < json.size() && json.at(i + 1) == '/') {
            // До конца строки. Сам перевод строки оставляем: номера строк в
            // сообщении об ошибке разбора обязаны сойтись с файлом.
            while (i < json.size() && json.at(i) != '\n') ++i;
            if (i < json.size()) out.append('\n');
            continue;
        }
        out.append(c);
    }
    return out;
}

// Второй проход: висячие запятые. Отдельным проходом, а не в том же цикле,
// ровно потому, что заглядывать вперёд надо УЖЕ без комментариев: между
// запятой и скобкой человек запросто напишет «// последняя».
QByteArray stripTrailingCommas(const QByteArray& json) {
    QByteArray out;
    out.reserve(json.size());
    bool inString = false;
    bool escaped = false;
    for (int i = 0; i < json.size(); ++i) {
        const char c = json.at(i);
        if (inString) {
            out.append(c);
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') inString = false;
            continue;
        }
        if (c == '"') {
            inString = true;
            out.append(c);
            continue;
        }
        if (c == ',') {
            int at = i + 1;
            while (at < json.size() && std::isspace(static_cast<unsigned char>(json.at(at))))
                ++at;
            // Запятая перед закрывающей скобкой — висячая: выбрасываем её, а
            // пробелы и переводы строк за ней оставляем как есть.
            if (at < json.size() && (json.at(at) == '}' || json.at(at) == ']')) continue;
        }
        out.append(c);
    }
    return out;
}

}  // namespace

QByteArray stripJsonSugar(const QByteArray& json) {
    return stripTrailingCommas(stripComments(json));
}

bool writeConfigTemplate(QString* error) {
    const QString path = configPath();
    if (QFile::exists(path)) return true;   // там правки человека
    QDir().mkpath(QFileInfo(path).absolutePath());

    // Всё тело — комментарием, снаружи пустой объект. Так файл и остаётся
    // действующим (отклонений нет), и служит меню: раскомментировал строку —
    // получил отклонение.
    const QList<QByteArray> lines = defaultAppearanceJson().split('\n');
    QByteArray out =
        "// Конфиг zametti. Здесь перечислено ВСЁ, что можно покрутить, со\n"
        "// значениями по умолчанию, и всё закомментировано: действующий конфиг —\n"
        "// это список ОТКЛОНЕНИЙ от умолчаний, а не их копия. Раскомментируйте\n"
        "// строку (уберите «//» в начале) — и значение станет вашим.\n"
        "//\n"
        "// Комментарии понимаются только такие: «//» до конца строки. Внутри\n"
        "// кавычек они не срезаются, так что «https://» писать можно.\n"
        "{\n";
    for (const QByteArray& line : lines) {
        const QByteArray trimmed = line.trimmed();
        if (trimmed.isEmpty() || trimmed == "{" || trimmed == "}") continue;
        out += "    // " + line.trimmed() + "\n";
    }
    out += "}\n";

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error != nullptr)
            *error = QStringLiteral("конфиг не записать: %1 (%2)").arg(path, file.errorString());
        return false;
    }
    if (file.write(out) != out.size()) {
        if (error != nullptr) *error = QStringLiteral("конфиг записан не целиком: ") + path;
        return false;
    }
    return true;
}

QString configPath() {
    return QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) +
           QStringLiteral("/config.json");
}

QString statePath() {
    return QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) +
           QStringLiteral("/state.json");
}

// Потолок на одну разжатую картинку — одна восьмая бюджета кэша. Отдельным
// ключом в конфиге его не задают: два числа про одно и то же разъехались бы
// от первой же правки, а «одна картинка не занимает заметную долю кэша» —
// правило, а не настройка.
//
// Сверх потолка Qt картинку не разжимает вовсе и отдаёт пустой QImage: это и
// есть защита от бомбы, показывать вместо такой картинки нечего, кроме рамки
// с надписью. Ставится здесь, а не при старте, чтобы следовать за конфигом —
// это единственная точка, через которую настройки попадают в программу.
namespace {

// Сколько всего памяти у машины, мегабайты; 0 — не удалось узнать. Портативного
// способа у Qt нет, поэтому спрашиваем систему напрямую; не Linux — не знаем и
// не гадаем.
int totalMemoryMb() {
#ifdef Q_OS_LINUX
    QFile meminfo(QStringLiteral("/proc/meminfo"));
    if (!meminfo.open(QIODevice::ReadOnly)) return 0;
    const QByteArray text = meminfo.readAll();
    const int at = text.indexOf("MemTotal:");
    if (at < 0) return 0;
    return text.mid(at + 9, 32).trimmed().split(' ').first().toInt() / 1024;
#else
    return 0;
#endif
}

// Предел стороны, выведенный самими. Правило простое: держать в памяти
// картинку крупнее, чем экран способен показать, незачем ни на какой машине.
// Запаса на зум не даём — колонка текста и так заметно уже экрана. Слабая
// машина опускает предел ещё: там дороже каждая копия.
int derivedImageSizeLimit() {
    int screenSide = 0;
    for (const QScreen* screen : QGuiApplication::screens()) {
        const QSize size = screen->size() * screen->devicePixelRatio();
        screenSide = qMax(screenSide, qMax(size.width(), size.height()));
    }
    // Экрана может не быть вовсе (offscreen, тесты) — тогда берём разумное
    // настольное значение, а не ноль.
    if (screenSide <= 0) screenSide = 1920;
    int limit = screenSide;

    const int memory = totalMemoryMb();
    if (memory > 0 && memory < 4096) limit = qMin(limit, 1600);
    else if (memory > 0 && memory < 8192) limit = qMin(limit, 2560);
    return qBound(1024, limit, 4096);
}

int g_loadedImageSizeLimit = 0;

}  // namespace

int loadedImageSizeLimit() {
    if (g_appearance.maxLoadedImageSize > 0) return g_appearance.maxLoadedImageSize;
    if (g_loadedImageSizeLimit == 0) g_loadedImageSizeLimit = derivedImageSizeLimit();
    return g_loadedImageSizeLimit;
}

void applyImageAllocationLimit() {
    // Разжатие — трата разовая: картинка тут же ужимается до
    // maxLoadedImageSize, а полный образ освобождается. Поэтому допускаем,
    // чтобы она временно заняла четверть кэша: при 512 МБ это 128 МБ, то есть
    // 32 Мп. Обычное фото с телефона на 24 Мп (91 МБ разжатым) проходит,
    // а что крупнее — уже не фотография, и вместо неё честнее показать рамку
    // с надписью, чем встать колом на полминуты.
    //
    // Восьмая доля, которую я взял сначала, давала 64 МБ и отвергала как раз
    // обычные телефонные фото.
    const int budget = qMax(8, g_appearance.imageCacheSizeMb);
    QImageReader::setAllocationLimit(qMax(1, budget / 4));
}

QStringList unknownConfigKeys(const QJsonObject& root) {
    // Сверяемся с полным списком умолчаний: он и есть словарь всех имён.
    const QJsonObject known =
        QJsonDocument::fromJson(defaultAppearanceJson()).object();
    QStringList out;
    for (auto section = root.begin(); section != root.end(); ++section) {
        if (!known.contains(section.key())) {
            out << section.key();
            continue;
        }
        if (!section.value().isObject() || !known.value(section.key()).isObject()) continue;
        const QJsonObject mine = section.value().toObject();
        const QJsonObject theirs = known.value(section.key()).toObject();
        for (auto item = mine.begin(); item != mine.end(); ++item)
            if (!theirs.contains(item.key()))
                out << section.key() + QLatin1Char('.') + item.key();
    }
    return out;
}

bool loadAppearance(QString* error, QStringList* unknown) {
    const QString path = configPath();
    QFile file(path);

    if (file.exists()) {
        if (!file.open(QIODevice::ReadOnly)) {
            if (error != nullptr) *error = QStringLiteral("не читается: ") + path;
            return false;
        }
        QJsonParseError parseError{};
        // Сначала срезаем //-комментарии: сгенерированный конфиг из них и
        // состоит, и без этого он бы вовсе не разобрался.
        const QJsonDocument doc =
            QJsonDocument::fromJson(stripJsonSugar(file.readAll()), &parseError);
        file.close();
        if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
            if (error != nullptr)
                *error = path + QStringLiteral(": ") + parseError.errorString();
            return false;
        }
        if (unknown != nullptr) *unknown = unknownConfigKeys(doc.object());
        appearanceFromJson(doc.object(), g_appearance);
    }
    applyImageAllocationLimit();
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
    session.storeRoot = root.value(QStringLiteral("storeRoot")).toString();
    session.treeSort = root.value(QStringLiteral("treeSort")).toString();
    session.scrollRatio = root.value(QStringLiteral("scrollRatio")).toDouble(0.0);
    session.zoom = root.value(QStringLiteral("zoom")).toDouble(1.0);
    session.windowGeometry = QByteArray::fromBase64(
        root.value(QStringLiteral("windowGeometry")).toString().toLatin1());
    session.splitterState = QByteArray::fromBase64(
        root.value(QStringLiteral("splitterState")).toString().toLatin1());
    session.panelsHidden = root.value(QStringLiteral("panelsHidden")).toBool(false);
    session.exportDir = root.value(QStringLiteral("exportDir")).toString();
    session.diffPlainView = root.value(QStringLiteral("diffPlainView")).toBool(false);
    session.exportKeepMeta = root.value(QStringLiteral("exportKeepMeta")).toBool(false);
    for (const QJsonValue& v : root.value(QStringLiteral("expandedDirs")).toArray())
        if (v.isString()) session.expandedDirs.append(v.toString());
    for (const QJsonValue& v : root.value(QStringLiteral("searchHistory")).toArray())
        if (v.isString()) session.searchHistory.append(v.toString());
    return session;
}

void saveSession(const Session& session) {
    QJsonArray expanded;
    for (const QString& dir : session.expandedDirs) expanded.append(dir);
    QJsonArray searches;
    for (const QString& query : session.searchHistory) searches.append(query);

    writeJson(statePath(),
              QJsonObject{
                  {QStringLiteral("lastFile"), session.lastFile},
                  {QStringLiteral("storeRoot"), session.storeRoot},
                  {QStringLiteral("treeSort"), session.treeSort},
                  {QStringLiteral("scrollRatio"), session.scrollRatio},
                  {QStringLiteral("zoom"), session.zoom},
                  {QStringLiteral("windowGeometry"),
                   QString::fromLatin1(session.windowGeometry.toBase64())},
                  {QStringLiteral("splitterState"),
                   QString::fromLatin1(session.splitterState.toBase64())},
                  {QStringLiteral("panelsHidden"), session.panelsHidden},
                  {QStringLiteral("exportDir"), session.exportDir},
                  {QStringLiteral("diffPlainView"), session.diffPlainView},
                  {QStringLiteral("exportKeepMeta"), session.exportKeepMeta},
                  {QStringLiteral("expandedDirs"), expanded},
                  {QStringLiteral("searchHistory"), searches},
              });
}

}  // namespace zametti
