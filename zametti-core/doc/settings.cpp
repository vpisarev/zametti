#include "settings.h"

#include "doc_model.h"

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

ZSettings g_settings;

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


void readBool(const QJsonObject& o, const char* key, bool& out) {
    const QJsonValue v = o.value(QLatin1String(key));
    if (v.isBool()) out = v.toBool();
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

QJsonObject settingsToJson(const ZSettings& a) {
    QJsonArray shapes;
    for (BulletShape shape : a.look.bulletShapes)
        shapes.append(shape == BulletShape::Circle   ? QStringLiteral("circle")
                      : shape == BulletShape::Square ? QStringLiteral("square")
                                                     : QStringLiteral("disc"));

    QJsonArray headings;
    for (int v : a.look.headingStep) headings.append(v);

    QJsonObject font{
        {QStringLiteral("family"), a.look.fontFamily},
        {QStringLiteral("pointSize"), a.look.baseFontPoint},
        {QStringLiteral("symbolFamily"), a.look.symbolFamily},
        {QStringLiteral("codeFamily"), a.look.codeFamily},
        {QStringLiteral("codeStep"), a.look.codeStep},
        {QStringLiteral("headingStep"), headings},
        {QStringLiteral("fallbackStep"), a.look.fallbackStep},
    };

    QJsonObject formulas{
        {QStringLiteral("inlineScale"), a.formulas.inlineScale},
        {QStringLiteral("displayScale"), a.formulas.displayScale},
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
    for (int v : a.pdf.headingStep) paperHeadings.append(v);
    QJsonObject pdf{
        {QStringLiteral("fontFamily"), a.pdf.fontFamily},
        {QStringLiteral("pointSize"), a.pdf.pointSize},
        {QStringLiteral("codeFamily"), a.pdf.codeFamily},
        {QStringLiteral("codeStep"), a.pdf.codeStep},
        {QStringLiteral("headingStep"), paperHeadings},
        {QStringLiteral("marginMm"), a.pdf.marginMm},
        {QStringLiteral("imageDpi"), a.pdf.imageDpi},
        {QStringLiteral("maxExportedImageSize"), a.pdf.maxExportedImageSize},
        {QStringLiteral("codeStripHeight"), a.pdf.codeStripHeight},
    };

    QJsonObject layout{
        {QStringLiteral("lineHeightFactor"), a.look.lineHeightFactor},
        {QStringLiteral("listLineHeightFactor"), a.look.listLineHeightFactor},
        {QStringLiteral("blockSpacing"), a.look.blockSpacing},
        {QStringLiteral("listIndent"), a.look.listIndent},
        {QStringLiteral("codeIndent"), a.look.codeIndent},
        {QStringLiteral("codePadLeft"), a.look.codePadLeft},
        {QStringLiteral("codeStripHeight"), a.look.codeStripHeight},
        {QStringLiteral("codePadTop"), a.look.codePadTop},
        {QStringLiteral("codeCornerRadius"), a.look.codeCornerRadius},
        {QStringLiteral("codeLangPointSize"), a.look.codeLangPointSize},
        {QStringLiteral("codeStripPadding"), a.look.codeStripPadding},
        {QStringLiteral("codeLangGap"), a.look.codeLangGap},
        {QStringLiteral("quoteIndent"), a.look.quoteIndent},
        {QStringLiteral("sideMargin"), a.look.sideMargin},
        {QStringLiteral("verticalMargin"), a.look.verticalMargin},
        {QStringLiteral("maxContentWidth"), a.look.maxContentWidth},
        {QStringLiteral("caretWidth"), a.look.caretWidth},
        {QStringLiteral("dividerWidth"), a.look.dividerWidth},
    };

    QJsonObject colors{
        {QStringLiteral("pageBackground"), colorToString(a.look.pageBackground)},
        {QStringLiteral("historyBackground"), colorToString(a.look.historyBackground)},
        {QStringLiteral("selectionBackground"), colorToString(a.look.selectionBackground)},
        {QStringLiteral("searchHighlight"), colorToString(a.look.searchHighlight)},
        {QStringLiteral("diffAdded"), colorToString(a.look.diffAdded)},
        {QStringLiteral("diffRemoved"), colorToString(a.look.diffRemoved)},
        {QStringLiteral("diffChanged"), colorToString(a.look.diffChanged)},
        {QStringLiteral("link"), colorToString(a.look.linkColor)},
        {QStringLiteral("quote"), colorToString(a.look.quoteColor)},
        {QStringLiteral("rawSource"), colorToString(a.look.rawColor)},
        {QStringLiteral("divider"), colorToString(a.look.dividerColor)},
        {QStringLiteral("codeBackground"), colorToString(a.look.codeBackground)},
        {QStringLiteral("codeLang"), colorToString(a.look.codeLangColor)},
        {QStringLiteral("caret"), colorToString(a.look.caretColor)},
    };

    QJsonObject list{
        {QStringLiteral("bulletColor"), colorToString(a.look.bulletColor)},
        {QStringLiteral("orderedColor"), colorToString(a.look.orderedColor)},
        {QStringLiteral("bulletStyle"),
         a.look.bulletStyle == BulletStyle::Glyph ? QStringLiteral("glyph")
                                             : QStringLiteral("drawn")},
        {QStringLiteral("bulletDiameter"), a.look.bulletDiameter},
        {QStringLiteral("bulletStrokeWidth"), a.look.bulletStrokeWidth},
        {QStringLiteral("bulletSquareSide"), a.look.bulletSquareSide},
        {QStringLiteral("bulletShapes"), shapes},
        {QStringLiteral("bulletRise"), a.look.bulletRise},
        {QStringLiteral("orderedRise"), a.look.orderedRise},
        {QStringLiteral("bullet"), a.look.bulletGlyph},
        {QStringLiteral("bulletScale"), a.look.bulletScale},
        {QStringLiteral("bulletTextGap"), a.look.bulletTextGap},
        {QStringLiteral("orderedTextGap"), a.look.orderedTextGap},
    };

    QJsonObject checkbox{
        {QStringLiteral("style"), styleToString(a.look.checkboxStyle)},
        {QStringLiteral("checkedColor"), colorToString(a.look.checkboxCheckedColor)},
        {QStringLiteral("uncheckedColor"), colorToString(a.look.checkboxUncheckedColor)},
        {QStringLiteral("tickColor"), colorToString(a.look.checkboxTickColor)},
        {QStringLiteral("penWidth"), a.look.checkboxPenWidth},
        {QStringLiteral("cornerRadius"), a.look.checkboxCornerRadius},
        {QStringLiteral("opticalRise"), a.look.checkboxOpticalRise},
        {QStringLiteral("glyphScale"), a.look.checkboxGlyphScale},
        {QStringLiteral("textGap"), a.look.checkboxTextGap},
    };

    QJsonObject notes{
        {QStringLiteral("root"), a.store.notesRoot},
        {QStringLiteral("title"), a.store.storeTitle},
        {QStringLiteral("watchFolder"), a.store.watchStore},
    };

    QJsonObject noteList{
        {QStringLiteral("width"), a.look.noteListWidth},
        {QStringLiteral("snippetLines"), a.look.noteListSnippetLines},
        {QStringLiteral("snippetColor"), colorToString(a.look.noteListSnippetColor)},
        {QStringLiteral("dateColor"), colorToString(a.look.noteListDateColor)},
    };

    QJsonObject sidebar{
        {QStringLiteral("fontFamily"), a.look.sidebarFontFamily},
        {QStringLiteral("fontSize"), a.look.sidebarFontPoint},
        {QStringLiteral("lineHeightFactor"), a.look.sidebarLineHeightFactor},
        {QStringLiteral("width"), a.look.sidebarWidth},
        {QStringLiteral("folderColor"), colorToString(a.look.sidebarFolderColor)},
        {QStringLiteral("folderScale"), a.look.sidebarFolderScale},
    };

    QJsonObject imageSelection{
        {QStringLiteral("cornerShare"), a.look.imageCornerShare},
        {QStringLiteral("cornerMinLength"), a.look.imageCornerMinLength},
        {QStringLiteral("cornerWidth"), a.look.imageCornerWidth},
        {QStringLiteral("cornerOffset"), a.look.imageCornerOffset},
    };

    QJsonObject imageCaption{
        {QStringLiteral("shown"), a.look.imageCaption},
        {QStringLiteral("family"), a.look.imageCaptionFamily},
        {QStringLiteral("fontPoints"), a.look.imageCaptionPoints},
        {QStringLiteral("gap"), a.look.imageCaptionGap},
        {QStringLiteral("color"), colorToString(a.look.imageCaptionColor)},
        {QStringLiteral("noname"), a.look.imageNonameCaption.pattern()},
    };

    QJsonObject statusBar{
        {QStringLiteral("family"), a.look.statusFamily},
        {QStringLiteral("fontPoints"), a.look.statusFontPoints},
        {QStringLiteral("padding"), a.look.statusPadding},
        {QStringLiteral("paddingTop"), a.look.statusPaddingTop},
        {QStringLiteral("background"), colorToString(a.look.statusBackground)},
        {QStringLiteral("textColor"), colorToString(a.look.statusTextColor)},
        {QStringLiteral("separatorColor"), colorToString(a.look.statusSeparatorColor)},
    };

    QJsonObject toolbar{
        {QStringLiteral("iconSize"), a.look.toolbarIconSize},
        {QStringLiteral("buttonPadding"), a.look.toolbarButtonPadding},
        {QStringLiteral("groupSpacing"), a.look.toolbarGroupSpacing},
        {QStringLiteral("background"), colorToString(a.look.toolbarBackground)},
        {QStringLiteral("iconColor"), colorToString(a.look.toolbarIconColor)},
        {QStringLiteral("iconHoverColor"), colorToString(a.look.toolbarIconHoverColor)},
        {QStringLiteral("iconOnColor"), colorToString(a.look.toolbarIconOnColor)},
        {QStringLiteral("iconMarkColor"), colorToString(a.look.toolbarIconMarkColor)},
        {QStringLiteral("iconDisabledColor"), colorToString(a.look.toolbarIconDisabledColor)},
        {QStringLiteral("hoverBackground"), colorToString(a.look.toolbarHoverBackground)},
        {QStringLiteral("separatorColor"), colorToString(a.look.toolbarSeparatorColor)},
    };

    QJsonObject find{
        {QStringLiteral("fontDelta"), a.look.findFontDelta},
        {QStringLiteral("previousGlyph"), a.look.findPreviousGlyph},
        {QStringLiteral("nextGlyph"), a.look.findNextGlyph},
        {QStringLiteral("historyGlyph"), a.look.findHistoryGlyph},
        {QStringLiteral("historyLimit"), a.look.findHistoryLimit},
    };

    QJsonArray special;
    for (const auto& [keys, text] : a.editor.specialKeys) {
        QJsonArray pair;
        pair.append(keys);
        pair.append(text);
        special.append(pair);
    }

    QJsonObject editor{
        {QStringLiteral("autosaveDelayMs"), a.editor.autosaveDelayMs},
        {QStringLiteral("undoCoalesceMs"), a.editor.undoCoalesceMs},
        {QStringLiteral("undoLimit"), a.editor.undoLimit},
        {QStringLiteral("undoRunChars"), a.editor.undoRunChars},
        {QStringLiteral("historyMergeChars"), a.history.historyMergeChars},
        {QStringLiteral("historyMergeHours"), a.history.historyMergeHours},
        {QStringLiteral("undoBudgetMb"), a.editor.undoBudgetMb},
        {QStringLiteral("imageCacheSizeMb"), a.cache.imageCacheSizeMb},
        {QStringLiteral("maxLoadedImageSize"), a.cache.maxLoadedImageSize},
        {QStringLiteral("documentCacheSizeMb"), a.cache.documentCacheSizeMb},
        {QStringLiteral("toggleTaskKey"), a.editor.toggleTaskKey},
        {QStringLiteral("moveUpKey"), a.editor.moveUpKey},
        {QStringLiteral("moveDownKey"), a.editor.moveDownKey},
        {QStringLiteral("makeBulletKey"), a.editor.makeBulletKey},
        {QStringLiteral("makeOrderedKey"), a.editor.makeOrderedKey},
        {QStringLiteral("makeTaskKey"), a.editor.makeTaskKey},
        {QStringLiteral("makeParagraphKey"), a.editor.makeParagraphKey},
        {QStringLiteral("makeCommentKey"), a.editor.makeCommentKey},
        {QStringLiteral("codeTabWidth"), a.editor.codeTabWidth},
        {QStringLiteral("special"), special},
        {QStringLiteral("externalEditor"), a.editor.externalEditor},
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
        {QStringLiteral("diffNext"), a.editor.diffNextKey},
        {QStringLiteral("diffPrevious"), a.editor.diffPreviousKey},
    };

    QJsonObject scroll{
        {QStringLiteral("smooth"), a.look.smoothScroll},
        {QStringLiteral("smoothMs"), a.look.smoothScrollMs},
    };

    QJsonObject zoom{
        {QStringLiteral("step"), a.look.zoomStep},
        {QStringLiteral("min"), a.look.zoomMin},
        {QStringLiteral("max"), a.look.zoomMax},
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
        {QStringLiteral("imageCaption"), imageCaption},
        {QStringLiteral("find"), find},
        {QStringLiteral("editor"), editor},
        {QStringLiteral("tables"), tables},
        {QStringLiteral("formulas"), formulas},
        {QStringLiteral("images"), images},
        {QStringLiteral("shortcuts"), shortcuts},
        {QStringLiteral("scroll"), scroll},
        {QStringLiteral("zoom"), zoom},
    };
}

void settingsFromJson(const QJsonObject& root, ZSettings& a) {
    const QJsonObject font = root.value(QStringLiteral("font")).toObject();
    readString(font, "family", a.look.fontFamily);
    readReal(font, "pointSize", a.look.baseFontPoint);
    readString(font, "symbolFamily", a.look.symbolFamily);
    readString(font, "codeFamily", a.look.codeFamily);
    readInt(font, "codeStep", a.look.codeStep);
    readInt(font, "fallbackStep", a.look.fallbackStep);
    const QJsonArray headings = font.value(QStringLiteral("headingStep")).toArray();
    for (int i = 0; i < headings.size() && i < int(a.look.headingStep.size()); ++i)
        if (headings.at(i).isDouble()) a.look.headingStep[size_t(i)] = headings.at(i).toInt();

    const QJsonObject formulas = root.value(QStringLiteral("formulas")).toObject();
    readReal(formulas, "inlineScale", a.formulas.inlineScale);
    readReal(formulas, "displayScale", a.formulas.displayScale);

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
    readInt(paper, "codeStep", a.pdf.codeStep);
    readReal(paper, "marginMm", a.pdf.marginMm);
    readInt(paper, "imageDpi", a.pdf.imageDpi);
    readInt(paper, "maxExportedImageSize", a.pdf.maxExportedImageSize);
    readReal(paper, "codeStripHeight", a.pdf.codeStripHeight);
    const QJsonArray paperHeads = paper.value(QStringLiteral("headingStep")).toArray();
    for (int i = 0; i < paperHeads.size() && i < int(a.pdf.headingStep.size()); ++i)
        if (paperHeads.at(i).isDouble()) a.pdf.headingStep[size_t(i)] = paperHeads.at(i).toInt();

    const QJsonObject layout = root.value(QStringLiteral("layout")).toObject();
    readReal(layout, "lineHeightFactor", a.look.lineHeightFactor);
    readReal(layout, "listLineHeightFactor", a.look.listLineHeightFactor);
    readReal(layout, "blockSpacing", a.look.blockSpacing);
    readReal(layout, "listIndent", a.look.listIndent);
    readReal(layout, "codeIndent", a.look.codeIndent);
    readReal(layout, "codePadLeft", a.look.codePadLeft);
    readReal(layout, "codeStripHeight", a.look.codeStripHeight);
    readReal(layout, "codePadTop", a.look.codePadTop);
    readReal(layout, "codeCornerRadius", a.look.codeCornerRadius);
    readReal(layout, "codeLangPointSize", a.look.codeLangPointSize);
    readReal(layout, "codeStripPadding", a.look.codeStripPadding);
    readReal(layout, "codeLangGap", a.look.codeLangGap);
    readReal(layout, "quoteIndent", a.look.quoteIndent);
    readReal(layout, "sideMargin", a.look.sideMargin);
    readReal(layout, "verticalMargin", a.look.verticalMargin);
    readReal(layout, "maxContentWidth", a.look.maxContentWidth);
    readReal(layout, "caretWidth", a.look.caretWidth);
    readReal(layout, "dividerWidth", a.look.dividerWidth);

    const QJsonObject colors = root.value(QStringLiteral("colors")).toObject();
    readColor(colors, "pageBackground", a.look.pageBackground);
    readColor(colors, "historyBackground", a.look.historyBackground);
    readColor(colors, "selectionBackground", a.look.selectionBackground);
    readColor(colors, "searchHighlight", a.look.searchHighlight);
    readColor(colors, "diffAdded", a.look.diffAdded);
    readColor(colors, "diffRemoved", a.look.diffRemoved);
    readColor(colors, "diffChanged", a.look.diffChanged);
    readColor(colors, "link", a.look.linkColor);
    readColor(colors, "quote", a.look.quoteColor);
    readColor(colors, "rawSource", a.look.rawColor);
    readColor(colors, "divider", a.look.dividerColor);
    readColor(colors, "codeBackground", a.look.codeBackground);
    readColor(colors, "codeLang", a.look.codeLangColor);
    readColor(colors, "caret", a.look.caretColor);

    const QJsonObject list = root.value(QStringLiteral("list")).toObject();
    readColor(list, "bulletColor", a.look.bulletColor);
    readColor(list, "orderedColor", a.look.orderedColor);
    const QJsonValue bulletStyle = list.value(QStringLiteral("bulletStyle"));
    if (bulletStyle.isString()) {
        a.look.bulletStyle = bulletStyle.toString() == QLatin1String("glyph") ? BulletStyle::Glyph
                                                                        : BulletStyle::Drawn;
    }
    readReal(list, "bulletDiameter", a.look.bulletDiameter);
    readReal(list, "bulletRise", a.look.bulletRise);
    readReal(list, "bulletStrokeWidth", a.look.bulletStrokeWidth);
    readReal(list, "bulletSquareSide", a.look.bulletSquareSide);

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
        if (!parsed.empty()) a.look.bulletShapes = std::move(parsed);
    }
    readReal(list, "orderedRise", a.look.orderedRise);
    readString(list, "bullet", a.look.bulletGlyph);
    readReal(list, "bulletScale", a.look.bulletScale);
    readReal(list, "bulletTextGap", a.look.bulletTextGap);
    readReal(list, "orderedTextGap", a.look.orderedTextGap);

    const QJsonObject checkbox = root.value(QStringLiteral("checkbox")).toObject();
    readStyle(checkbox, "style", a.look.checkboxStyle);
    readColor(checkbox, "checkedColor", a.look.checkboxCheckedColor);
    readColor(checkbox, "uncheckedColor", a.look.checkboxUncheckedColor);
    readColor(checkbox, "tickColor", a.look.checkboxTickColor);
    readReal(checkbox, "penWidth", a.look.checkboxPenWidth);
    readReal(checkbox, "cornerRadius", a.look.checkboxCornerRadius);
    readReal(checkbox, "opticalRise", a.look.checkboxOpticalRise);
    readReal(checkbox, "glyphScale", a.look.checkboxGlyphScale);
    readReal(checkbox, "textGap", a.look.checkboxTextGap);

    const QJsonObject notes = root.value(QStringLiteral("notes")).toObject();
    readString(notes, "root", a.store.notesRoot);
    readString(notes, "title", a.store.storeTitle);
    const QJsonValue watch = notes.value(QStringLiteral("watchFolder"));
    if (watch.isBool()) a.store.watchStore = watch.toBool();

    const QJsonObject noteList = root.value(QStringLiteral("noteList")).toObject();
    const QJsonValue listWidth = noteList.value(QStringLiteral("width"));
    if (listWidth.isDouble()) a.look.noteListWidth = listWidth.toInt();
    const QJsonValue snippetLines = noteList.value(QStringLiteral("snippetLines"));
    if (snippetLines.isDouble()) a.look.noteListSnippetLines = qMax(0, snippetLines.toInt());
    readColor(noteList, "snippetColor", a.look.noteListSnippetColor);
    readColor(noteList, "dateColor", a.look.noteListDateColor);

    const QJsonObject sidebar = root.value(QStringLiteral("sidebar")).toObject();
    readString(sidebar, "fontFamily", a.look.sidebarFontFamily);
    readReal(sidebar, "fontSize", a.look.sidebarFontPoint);
    readReal(sidebar, "lineHeightFactor", a.look.sidebarLineHeightFactor);
    readColor(sidebar, "folderColor", a.look.sidebarFolderColor);
    readReal(sidebar, "folderScale", a.look.sidebarFolderScale);
    const QJsonValue width = sidebar.value(QStringLiteral("width"));
    if (width.isDouble()) a.look.sidebarWidth = width.toInt();

    const QJsonObject imageSelection =
        root.value(QStringLiteral("imageSelection")).toObject();
    const QJsonValue cornerShare = imageSelection.value(QStringLiteral("cornerShare"));
    if (cornerShare.isDouble()) a.look.imageCornerShare = std::clamp(cornerShare.toDouble(), 0.0, 0.5);
    const QJsonValue cornerMin = imageSelection.value(QStringLiteral("cornerMinLength"));
    if (cornerMin.isDouble()) a.look.imageCornerMinLength = qMax(0, cornerMin.toInt());
    const QJsonValue cornerWidth = imageSelection.value(QStringLiteral("cornerWidth"));
    if (cornerWidth.isDouble()) a.look.imageCornerWidth = qMax(0.5, cornerWidth.toDouble());
    const QJsonValue cornerOffset = imageSelection.value(QStringLiteral("cornerOffset"));
    if (cornerOffset.isDouble()) a.look.imageCornerOffset = qMax(0.0, cornerOffset.toDouble());

    const QJsonObject imageCaption = root.value(QStringLiteral("imageCaption")).toObject();
    readBool(imageCaption, "shown", a.look.imageCaption);
    const QJsonValue captionFamily = imageCaption.value(QStringLiteral("family"));
    if (captionFamily.isString()) a.look.imageCaptionFamily = captionFamily.toString();
    const QJsonValue captionPoints = imageCaption.value(QStringLiteral("fontPoints"));
    if (captionPoints.isDouble())
        a.look.imageCaptionPoints = std::clamp(captionPoints.toDouble(), 6.0, 24.0);
    readReal(imageCaption, "gap", a.look.imageCaptionGap);
    readColor(imageCaption, "color", a.look.imageCaptionColor);
    // Битый регэксп настройку не меняет: молча прятать все подписи (или ни
    // одной) из-за опечатки в конфиге нельзя.
    const QJsonValue noname = imageCaption.value(QStringLiteral("noname"));
    if (noname.isString()) {
        const QRegularExpression candidate(noname.toString(),
                                           QRegularExpression::CaseInsensitiveOption);
        if (candidate.isValid()) a.look.imageNonameCaption = candidate;
    }

    const QJsonObject statusBar = root.value(QStringLiteral("statusBar")).toObject();
    const QJsonValue statusFamily = statusBar.value(QStringLiteral("family"));
    if (statusFamily.isString()) a.look.statusFamily = statusFamily.toString();
    const QJsonValue statusPoints = statusBar.value(QStringLiteral("fontPoints"));
    if (statusPoints.isDouble()) a.look.statusFontPoints = std::clamp(statusPoints.toInt(), 6, 24);
    const QJsonValue statusPadding = statusBar.value(QStringLiteral("padding"));
    if (statusPadding.isDouble()) a.look.statusPadding = qMax(0, statusPadding.toInt());
    const QJsonValue statusPaddingTop = statusBar.value(QStringLiteral("paddingTop"));
    if (statusPaddingTop.isDouble()) a.look.statusPaddingTop = qMax(0, statusPaddingTop.toInt());
    readColor(statusBar, "background", a.look.statusBackground);
    readColor(statusBar, "textColor", a.look.statusTextColor);
    readColor(statusBar, "separatorColor", a.look.statusSeparatorColor);

    const QJsonObject toolbar = root.value(QStringLiteral("toolbar")).toObject();
    const QJsonValue iconSize = toolbar.value(QStringLiteral("iconSize"));
    // Иконка меньше двенадцати точек перестаёт читаться, больше шестидесяти
    // ломает высоту тулбара. Границы не вкус, а пределы, за которыми настройка
    // портит окно, а не настраивает его.
    if (iconSize.isDouble()) a.look.toolbarIconSize = std::clamp(iconSize.toInt(), 12, 64);
    const QJsonValue buttonPadding = toolbar.value(QStringLiteral("buttonPadding"));
    if (buttonPadding.isDouble()) a.look.toolbarButtonPadding = qMax(0, buttonPadding.toInt());
    const QJsonValue groupSpacing = toolbar.value(QStringLiteral("groupSpacing"));
    if (groupSpacing.isDouble()) a.look.toolbarGroupSpacing = qMax(0, groupSpacing.toInt());
    readColor(toolbar, "background", a.look.toolbarBackground);
    readColor(toolbar, "iconColor", a.look.toolbarIconColor);
    readColor(toolbar, "iconHoverColor", a.look.toolbarIconHoverColor);
    readColor(toolbar, "iconOnColor", a.look.toolbarIconOnColor);
    readColor(toolbar, "iconMarkColor", a.look.toolbarIconMarkColor);
    readColor(toolbar, "iconDisabledColor", a.look.toolbarIconDisabledColor);
    readColor(toolbar, "hoverBackground", a.look.toolbarHoverBackground);
    readColor(toolbar, "separatorColor", a.look.toolbarSeparatorColor);

    const QJsonObject find = root.value(QStringLiteral("find")).toObject();
    readReal(find, "fontDelta", a.look.findFontDelta);
    readString(find, "previousGlyph", a.look.findPreviousGlyph);
    readString(find, "nextGlyph", a.look.findNextGlyph);
    readString(find, "historyGlyph", a.look.findHistoryGlyph);
    const QJsonValue historyLimit = find.value(QStringLiteral("historyLimit"));
    if (historyLimit.isDouble()) a.look.findHistoryLimit = qMax(0, historyLimit.toInt());

    const QJsonObject shortcuts = root.value(QStringLiteral("shortcuts")).toObject();
    readString(shortcuts, "diffNext", a.editor.diffNextKey);
    readString(shortcuts, "diffPrevious", a.editor.diffPreviousKey);

    const QJsonObject editor = root.value(QStringLiteral("editor")).toObject();
    const QJsonValue delay = editor.value(QStringLiteral("autosaveDelayMs"));
    if (delay.isDouble()) a.editor.autosaveDelayMs = delay.toInt();
    const QJsonValue coalesce = editor.value(QStringLiteral("undoCoalesceMs"));
    if (coalesce.isDouble()) a.editor.undoCoalesceMs = coalesce.toInt();
    readInt(editor, "undoRunChars", a.editor.undoRunChars);
    readInt(editor, "codeTabWidth", a.editor.codeTabWidth);
    readInt(editor, "historyMergeChars", a.history.historyMergeChars);
    readInt(editor, "historyMergeHours", a.history.historyMergeHours);
    const QJsonValue limit = editor.value(QStringLiteral("undoLimit"));
    if (limit.isDouble()) a.editor.undoLimit = limit.toInt();
    const QJsonValue budget = editor.value(QStringLiteral("undoBudgetMb"));
    if (budget.isDouble()) a.editor.undoBudgetMb = budget.toInt();
    const QJsonValue images = editor.value(QStringLiteral("imageCacheSizeMb"));
    if (images.isDouble()) a.cache.imageCacheSizeMb = images.toInt();
    const QJsonValue loaded = editor.value(QStringLiteral("maxLoadedImageSize"));
    if (loaded.isDouble()) a.cache.maxLoadedImageSize = loaded.toInt();
    const QJsonValue documents = editor.value(QStringLiteral("documentCacheSizeMb"));
    if (documents.isDouble()) a.cache.documentCacheSizeMb = documents.toInt();
    readString(editor, "toggleTaskKey", a.editor.toggleTaskKey);
    readString(editor, "moveUpKey", a.editor.moveUpKey);
    readString(editor, "moveDownKey", a.editor.moveDownKey);
    readString(editor, "makeBulletKey", a.editor.makeBulletKey);
    readString(editor, "makeOrderedKey", a.editor.makeOrderedKey);
    readString(editor, "makeTaskKey", a.editor.makeTaskKey);
    readString(editor, "makeParagraphKey", a.editor.makeParagraphKey);
    readString(editor, "makeCommentKey", a.editor.makeCommentKey);
    // Автозамены: список пар [сочетание, что вставить]. Заданный список
    // заменяет умолчания целиком — иначе от умолчания было бы не избавиться.
    const QJsonValue special = editor.value(QStringLiteral("special"));
    if (special.isArray()) {
        a.editor.specialKeys.clear();
        for (const QJsonValue& entry : special.toArray()) {
            const QJsonArray pair = entry.toArray();
            if (pair.size() != 2 || !pair.at(0).isString() || !pair.at(1).isString())
                continue;   // битую запись пропускаем, соседние живут
            const QString keys = pair.at(0).toString();
            if (keys.isEmpty()) continue;
            a.editor.specialKeys.push_back({keys, pair.at(1).toString()});
        }
    }
    readString(editor, "externalEditor", a.editor.externalEditor);

    // Имя своё, а не "images": в этой же области уже живёт значение ключа
    // editor.imageCacheSizeMb под этим именем.
    const QJsonObject importGroup = root.value(QStringLiteral("images")).toObject();
    readInt(importGroup, "maxImportedImageSize", a.images.maxImportedImageSize);

    const QJsonObject scroll = root.value(QStringLiteral("scroll")).toObject();
    readBool(scroll, "smooth", a.look.smoothScroll);
    readInt(scroll, "smoothMs", a.look.smoothScrollMs);

    const QJsonObject zoom = root.value(QStringLiteral("zoom")).toObject();
    readReal(zoom, "step", a.look.zoomStep);
    readReal(zoom, "min", a.look.zoomMin);
    readReal(zoom, "max", a.look.zoomMax);
}

bool writeJson(const QString& path, const QJsonObject& root) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    return true;
}

}  // namespace

const ZSettings& settings() { return g_settings; }
ZSettings& editSettings() { return g_settings; }

CodePlate codePlate() {
    // Единицы те же, что у сборщика документа: по вертикали — высота строки
    // кода (гарнитура текста в кегле кода, как её считает document_builder),
    // по горизонтали — ширина "A" основного шрифта.
    //
    // Масштаба здесь нет и быть не может: геометрия документа строится один раз
    // и живёт в пикселях, а зум — это шрифт документа. Спрашивать масштаб тут
    // значило бы разойтись с резервом, который сборщик уже положил в поля
    // блока.
    QFont base{QString(g_settings.look.fontFamily)};
    base.setPointSizeF(g_settings.look.baseFontPoint);
    base.setStyleHint(QFont::Monospace);
    const qreal charUnit = QFontMetricsF(base).horizontalAdvance(QLatin1Char('A'));

    QFont codeLine = base;
    codeLine.setPointSizeF(g_settings.look.baseFontPoint * fontStepFactor(g_settings.look.codeStep));
    const qreal lineUnit =
        std::round(QFontMetricsF(codeLine).height() * g_settings.look.lineHeightFactor);

    CodePlate plate;
    plate.strip = std::round(g_settings.look.codeStripHeight * lineUnit);
    plate.padTop = std::round(g_settings.look.codePadTop * lineUnit);
    plate.padLeft = g_settings.look.codePadLeft * charUnit;
    plate.indent = g_settings.look.codeIndent * charUnit;
    plate.radius = g_settings.look.codeCornerRadius;
    plate.stripPadding = g_settings.look.codeStripPadding * charUnit;
    plate.langGap = g_settings.look.codeLangGap * charUnit;
    return plate;
}

QFont codeLangFont() {
    QFont font{QString(g_settings.look.sidebarFontFamily)};
    const qreal point = g_settings.look.codeLangPointSize > 0.0
                            ? g_settings.look.codeLangPointSize
                            : g_settings.look.sidebarFontPoint;
    font.setPointSizeF(point);
    return font;
}


QByteArray defaultSettingsJson() {
    return QJsonDocument(settingsToJson(ZSettings{})).toJson(QJsonDocument::Indented);
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
    const QList<QByteArray> lines = defaultSettingsJson().split('\n');
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
    if (g_settings.cache.maxLoadedImageSize > 0) return g_settings.cache.maxLoadedImageSize;
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
    const int budget = qMax(8, g_settings.cache.imageCacheSizeMb);
    QImageReader::setAllocationLimit(qMax(1, budget / 4));
}

QStringList unknownConfigKeys(const QJsonObject& root) {
    // Сверяемся с полным списком умолчаний: он и есть словарь всех имён.
    const QJsonObject known =
        QJsonDocument::fromJson(defaultSettingsJson()).object();
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

bool loadSettings(QString* error, QStringList* unknown) {
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
        settingsFromJson(doc.object(), g_settings);
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
    session.caret = root.value(QStringLiteral("caret")).toInt(0);
    session.anchor = root.value(QStringLiteral("anchor")).toInt(session.caret);
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
                  {QStringLiteral("caret"), session.caret},
                  {QStringLiteral("anchor"), session.anchor},
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
