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
#include <cstdio>

namespace zametti {
namespace {

ZSettings g_settings;

QString colorToString(const QColor& c) {
    return c.alpha() == 255 ? c.name(QColor::HexRgb) : c.name(QColor::HexArgb);
}

// Значение из объекта, если оно там есть и нужного вида. Чужие и битые ключи
// молча пропускаем: конфиг правят руками, и опечатка в одном параметре не
// должна ронять остальные.
// ЧТЕНИЕ ЧЕРЕЗ СЕТТЕР, А НЕ В ПОЛЕ. Сеттер обрезает число до допустимого
// диапазона и отвечает, приняла ли настройка значение как есть; нет — пишем в
// лог: настройки пользователя — пожелания, робастность выше них (решение
// владельца), а молчать о поправленном нельзя — человек должен видеть, что его
// число не взяли.
void complainClamped(const QJsonObject& o, const char* key, const QJsonValue& v) {
    Q_UNUSED(o);
    std::fprintf(stderr, "setting «%s» = %g outside the allowed range — clamped to the edge\n",
                 key, v.toDouble());
}

template <class Obj>
void readReal(const QJsonObject& o, const char* key, Obj& obj, bool (Obj::*set)(qreal)) {
    const QJsonValue v = o.value(QLatin1String(key));
    if (v.isDouble() && !(obj.*set)(v.toDouble())) complainClamped(o, key, v);
}

template <class Obj>
void readInt(const QJsonObject& o, const char* key, Obj& obj, bool (Obj::*set)(int)) {
    const QJsonValue v = o.value(QLatin1String(key));
    if (v.isDouble() && !(obj.*set)(v.toInt())) complainClamped(o, key, v);
}

template <class Obj>
void readBool(const QJsonObject& o, const char* key, Obj& obj, bool (Obj::*set)(bool)) {
    const QJsonValue v = o.value(QLatin1String(key));
    if (v.isBool()) (obj.*set)(v.toBool());
}

template <class Obj>
void readString(const QJsonObject& o, const char* key, Obj& obj, bool (Obj::*set)(QString)) {
    const QJsonValue v = o.value(QLatin1String(key));
    if (v.isString()) (obj.*set)(v.toString());
}

template <class Obj>
void readColor(const QJsonObject& o, const char* key, Obj& obj, bool (Obj::*set)(QColor)) {
    const QJsonValue v = o.value(QLatin1String(key));
    if (!v.isString()) return;
    const QColor parsed = QColor::fromString(v.toString());
    if (parsed.isValid()) (obj.*set)(parsed);
}

QString styleToString(CheckboxStyle s) {
    switch (s) {
        case CheckboxStyle::Glyph: return QStringLiteral("glyph");
        case CheckboxStyle::Ascii: return QStringLiteral("ascii");
        case CheckboxStyle::Drawn: return QStringLiteral("drawn");
    }
    return QStringLiteral("drawn");
}

template <class Obj>
void readStyle(const QJsonObject& o, const char* key, Obj& obj, bool (Obj::*set)(CheckboxStyle)) {
    const QJsonValue v = o.value(QLatin1String(key));
    if (!v.isString()) return;
    const QString s = v.toString();
    if (s == QLatin1String("glyph")) (obj.*set)(CheckboxStyle::Glyph);
    else if (s == QLatin1String("ascii")) (obj.*set)(CheckboxStyle::Ascii);
    else if (s == QLatin1String("drawn")) (obj.*set)(CheckboxStyle::Drawn);
}

QJsonObject settingsToJson(const ZSettings& a) {
    QJsonArray shapes;
    for (BulletShape shape : a.style().bulletShapes())
        shapes.append(shape == BulletShape::Circle   ? QStringLiteral("circle")
                      : shape == BulletShape::Square ? QStringLiteral("square")
                                                     : QStringLiteral("disc"));

    QJsonArray headings;
    for (int v : a.style().headingStep()) headings.append(v);

    QJsonObject font{
        {QStringLiteral("family"), a.style().fontFamily()},
        {QStringLiteral("pointSize"), a.style().baseFontPoint()},
        {QStringLiteral("symbolFamily"), a.style().symbolFamily()},
        {QStringLiteral("codeFamily"), a.style().codeFamily()},
        {QStringLiteral("codeLangFamily"), a.style().codeLangFamily()},
        {QStringLiteral("codeStep"), a.style().codeStep()},
        {QStringLiteral("diffStep"), a.style().diffStep()},
        {QStringLiteral("headingStep"), headings},
        {QStringLiteral("fallbackStep"), a.style().fallbackStep()},
    };

    QJsonObject formulas{
        {QStringLiteral("inlineScale"), a.formulas().inlineScale()},
        {QStringLiteral("displayScale"), a.formulas().displayScale()},
    };

    QJsonObject tables{
        {QStringLiteral("cellPadding"), a.tables().cellPadding()},
        {QStringLiteral("cellPaddingY"), a.tables().cellPaddingY()},
        {QStringLiteral("borderColor"), colorToString(a.tables().borderColor())},
        {QStringLiteral("horizontalBorder"), a.tables().horizontalBorder()},
        {QStringLiteral("verticalBorder"), a.tables().verticalBorder()},
        {QStringLiteral("headerSeparator"), a.tables().headerSeparator()},
        {QStringLiteral("rowSeparator"), a.tables().rowSeparator()},
        {QStringLiteral("columnSeparator"), a.tables().columnSeparator()},
        {QStringLiteral("headerColor"), colorToString(a.tables().headerColor())},
        {QStringLiteral("tableColor"), colorToString(a.tables().tableColor())},
        {QStringLiteral("altTableColor"), colorToString(a.tables().altTableColor())},
    };

    QJsonArray paperHeadings;
    for (int v : a.pdf().headingStep()) paperHeadings.append(v);
    QJsonObject pdf{
        {QStringLiteral("fontFamily"), a.pdf().fontFamily()},
        {QStringLiteral("pointSize"), a.pdf().pointSize()},
        {QStringLiteral("codeFamily"), a.pdf().codeFamily()},
        {QStringLiteral("codeStep"), a.pdf().codeStep()},
        {QStringLiteral("headingStep"), paperHeadings},
        {QStringLiteral("pageSize"), a.pdf().pageSize()},
        {QStringLiteral("marginMm"), a.pdf().marginMm()},
        {QStringLiteral("imageDpi"), a.pdf().imageDpi()},
        {QStringLiteral("maxExportedImageSize"), a.pdf().maxExportedImageSize()},
        {QStringLiteral("codeStripHeight"), a.pdf().codeStripHeight()},
    };

    QJsonObject layout{
        {QStringLiteral("lineHeightFactor"), a.style().lineHeightFactor()},
        {QStringLiteral("listLineHeightFactor"), a.style().listLineHeightFactor()},
        {QStringLiteral("blockSpacing"), a.style().blockSpacing()},
        {QStringLiteral("listIndent"), a.style().listIndent()},
        {QStringLiteral("codeIndent"), a.style().codeIndent()},
        {QStringLiteral("codePadLeft"), a.style().codePadLeft()},
        {QStringLiteral("codeStripHeight"), a.style().codeStripHeight()},
        {QStringLiteral("codePadTop"), a.style().codePadTop()},
        {QStringLiteral("codeCornerRadius"), a.style().codeCornerRadius()},
        {QStringLiteral("codeCopyIconScale"), a.style().codeCopyIconScale()},
        {QStringLiteral("codeLangPointSize"), a.style().codeLangPointSize()},
        {QStringLiteral("codeStripPadding"), a.style().codeStripPadding()},
        {QStringLiteral("codeLangGap"), a.style().codeLangGap()},
        {QStringLiteral("quoteIndent"), a.style().quoteIndent()},
        {QStringLiteral("sideMargin"), a.style().sideMargin()},
        {QStringLiteral("verticalMargin"), a.style().verticalMargin()},
        {QStringLiteral("maxContentWidth"), a.style().maxContentWidth()},
        {QStringLiteral("caretWidth"), a.style().caretWidth()},
        {QStringLiteral("dividerWidth"), a.style().dividerWidth()},
    };

    QJsonObject colors{
        {QStringLiteral("pageBackground"), colorToString(a.style().pageBackground())},
        {QStringLiteral("historyBackground"), colorToString(a.style().historyBackground())},
        {QStringLiteral("selectionBackground"), colorToString(a.style().selectionBackground())},
        {QStringLiteral("searchHighlight"), colorToString(a.style().searchHighlight())},
        {QStringLiteral("diffAdded"), colorToString(a.style().diffAdded())},
        {QStringLiteral("diffRemoved"), colorToString(a.style().diffRemoved())},
        {QStringLiteral("diffChanged"), colorToString(a.style().diffChanged())},
        {QStringLiteral("link"), colorToString(a.style().linkColor())},
        {QStringLiteral("quote"), colorToString(a.style().quoteColor())},
        {QStringLiteral("rawSource"), colorToString(a.style().rawColor())},
        {QStringLiteral("divider"), colorToString(a.style().dividerColor())},
        {QStringLiteral("codeBackground"), colorToString(a.style().codeBackground())},
        {QStringLiteral("codeLang"), colorToString(a.style().codeLangColor())},
        {QStringLiteral("caret"), colorToString(a.style().caretColor())},
    };

    QJsonObject list{
        {QStringLiteral("bulletColor"), colorToString(a.style().bulletColor())},
        {QStringLiteral("orderedColor"), colorToString(a.style().orderedColor())},
        {QStringLiteral("bulletStyle"),
         a.style().bulletStyle() == BulletStyle::Glyph ? QStringLiteral("glyph")
                                             : QStringLiteral("drawn")},
        {QStringLiteral("bulletDiameter"), a.style().bulletDiameter()},
        {QStringLiteral("bulletStrokeWidth"), a.style().bulletStrokeWidth()},
        {QStringLiteral("bulletSquareSide"), a.style().bulletSquareSide()},
        {QStringLiteral("bulletShapes"), shapes},
        {QStringLiteral("bulletRise"), a.style().bulletRise()},
        {QStringLiteral("orderedRise"), a.style().orderedRise()},
        {QStringLiteral("bullet"), a.style().bulletGlyph()},
        {QStringLiteral("bulletScale"), a.style().bulletScale()},
        {QStringLiteral("bulletTextGap"), a.style().bulletTextGap()},
        {QStringLiteral("orderedTextGap"), a.style().orderedTextGap()},
    };

    QJsonObject checkbox{
        {QStringLiteral("style"), styleToString(a.style().checkboxStyle())},
        {QStringLiteral("checkedColor"), colorToString(a.style().checkboxCheckedColor())},
        {QStringLiteral("uncheckedColor"), colorToString(a.style().checkboxUncheckedColor())},
        {QStringLiteral("tickColor"), colorToString(a.style().checkboxTickColor())},
        {QStringLiteral("penWidth"), a.style().checkboxPenWidth()},
        {QStringLiteral("cornerRadius"), a.style().checkboxCornerRadius()},
        {QStringLiteral("opticalRise"), a.style().checkboxOpticalRise()},
        {QStringLiteral("glyphScale"), a.style().checkboxGlyphScale()},
        {QStringLiteral("textGap"), a.style().checkboxTextGap()},
    };

    QJsonObject notes{
        {QStringLiteral("root"), a.store().notesRoot()},
        {QStringLiteral("title"), a.store().storeTitle()},
        {QStringLiteral("watchFolder"), a.store().watchStore()},
    };

    QJsonObject noteList{
        {QStringLiteral("width"), a.ui().noteListWidth()},
        {QStringLiteral("snippetLines"), a.ui().noteListSnippetLines()},
        {QStringLiteral("snippetColor"), colorToString(a.ui().noteListSnippetColor())},
        {QStringLiteral("dateColor"), colorToString(a.ui().noteListDateColor())},
    };

    QJsonObject sidebar{
        {QStringLiteral("fontFamily"), a.ui().sidebarFontFamily()},
        {QStringLiteral("fontSize"), a.ui().sidebarFontPoint()},
        {QStringLiteral("lineHeightFactor"), a.ui().sidebarLineHeightFactor()},
        {QStringLiteral("width"), a.ui().sidebarWidth()},
        {QStringLiteral("folderColor"), colorToString(a.ui().sidebarFolderColor())},
        {QStringLiteral("folderScale"), a.ui().sidebarFolderScale()},
    };

    QJsonObject imageSelection{
        {QStringLiteral("cornerShare"), a.style().imageCornerShare()},
        {QStringLiteral("cornerMinLength"), a.style().imageCornerMinLength()},
        {QStringLiteral("cornerWidth"), a.style().imageCornerWidth()},
        {QStringLiteral("cornerOffset"), a.style().imageCornerOffset()},
    };

    QJsonObject imageCaption{
        {QStringLiteral("shown"), a.style().imageCaption()},
        {QStringLiteral("family"), a.style().imageCaptionFamily()},
        {QStringLiteral("fontPoints"), a.style().imageCaptionPoints()},
        {QStringLiteral("gap"), a.style().imageCaptionGap()},
        {QStringLiteral("color"), colorToString(a.style().imageCaptionColor())},
        {QStringLiteral("noname"), a.style().imageNonameCaption().pattern()},
    };

    QJsonObject statusBar{
        {QStringLiteral("family"), a.ui().statusFamily()},
        {QStringLiteral("fontPoints"), a.ui().statusFontPoints()},
        {QStringLiteral("padding"), a.ui().statusPadding()},
        {QStringLiteral("paddingTop"), a.ui().statusPaddingTop()},
        {QStringLiteral("background"), colorToString(a.ui().statusBackground())},
        {QStringLiteral("textColor"), colorToString(a.ui().statusTextColor())},
        {QStringLiteral("separatorColor"), colorToString(a.ui().statusSeparatorColor())},
    };

    QJsonObject toolbar{
        {QStringLiteral("iconSize"), a.ui().toolbarIconSize()},
        {QStringLiteral("buttonPadding"), a.ui().toolbarButtonPadding()},
        {QStringLiteral("groupSpacing"), a.ui().toolbarGroupSpacing()},
        {QStringLiteral("background"), colorToString(a.ui().toolbarBackground())},
        {QStringLiteral("iconColor"), colorToString(a.ui().toolbarIconColor())},
        {QStringLiteral("iconHoverColor"), colorToString(a.ui().toolbarIconHoverColor())},
        {QStringLiteral("iconOnColor"), colorToString(a.ui().toolbarIconOnColor())},
        {QStringLiteral("iconMarkColor"), colorToString(a.ui().toolbarIconMarkColor())},
        {QStringLiteral("iconDisabledColor"), colorToString(a.ui().toolbarIconDisabledColor())},
        {QStringLiteral("hoverBackground"), colorToString(a.ui().toolbarHoverBackground())},
        {QStringLiteral("separatorColor"), colorToString(a.ui().toolbarSeparatorColor())},
    };

    QJsonObject find{
        {QStringLiteral("fontDelta"), a.ui().findFontDelta()},
        {QStringLiteral("previousGlyph"), a.ui().findPreviousGlyph()},
        {QStringLiteral("nextGlyph"), a.ui().findNextGlyph()},
        {QStringLiteral("historyGlyph"), a.ui().findHistoryGlyph()},
        {QStringLiteral("historyLimit"), a.ui().findHistoryLimit()},
    };

    QJsonArray special;
    for (const auto& [keys, text] : a.editor().specialKeys()) {
        QJsonArray pair;
        pair.append(keys);
        pair.append(text);
        special.append(pair);
    }

    QJsonObject editor{
        {QStringLiteral("autosaveDelayMs"), a.editor().autosaveDelayMs()},
        {QStringLiteral("undoCoalesceMs"), a.editor().undoCoalesceMs()},
        {QStringLiteral("undoLimit"), a.editor().undoLimit()},
        {QStringLiteral("undoRunChars"), a.editor().undoRunChars()},
        {QStringLiteral("historyMergeChars"), a.history().historyMergeChars()},
        {QStringLiteral("historyMergeHours"), a.history().historyMergeHours()},
        {QStringLiteral("undoBudgetMb"), a.editor().undoBudgetMb()},
        {QStringLiteral("imageCacheSizeMb"), a.cache().imageCacheSizeMb()},
        {QStringLiteral("maxLoadedImageSize"), a.cache().maxLoadedImageSize()},
        {QStringLiteral("documentCacheSizeMb"), a.cache().documentCacheSizeMb()},
        {QStringLiteral("toggleTaskKey"), a.editor().toggleTaskKey()},
        {QStringLiteral("moveUpKey"), a.editor().moveUpKey()},
        {QStringLiteral("moveDownKey"), a.editor().moveDownKey()},
        {QStringLiteral("makeBulletKey"), a.editor().makeBulletKey()},
        {QStringLiteral("makeOrderedKey"), a.editor().makeOrderedKey()},
        {QStringLiteral("makeTaskKey"), a.editor().makeTaskKey()},
        {QStringLiteral("makeParagraphKey"), a.editor().makeParagraphKey()},
        {QStringLiteral("makeCommentKey"), a.editor().makeCommentKey()},
        {QStringLiteral("markdownModeKey"), a.editor().markdownModeKey()},
        {QStringLiteral("codeTabWidth"), a.editor().codeTabWidth()},
        {QStringLiteral("special"), special},
        {QStringLiteral("externalEditor"), a.editor().externalEditor()},
    };

    // ТОЛЬКО S. Остальные числа импорта настройке не подлежат — решение
    // владельца, и вот его причина: они выведены замерами на большом корпусе и
    // СОГЛАСОВАНЫ МЕЖДУ СОБОЙ. Произвольная смена одного ломает логику
    // остальных — например, качество и порог пережатия подобраны так, чтобы
    // спор с исходником имел смысл; сдвинь одно, и правило станет либо
    // бесполезным, либо вредным. Значения живут в settings.h, и менять их
    // можно только правкой кода, то есть осознанно и с новым замером.
    QJsonObject images{
        {QStringLiteral("maxImportedImageSize"), a.images().maxImportedImageSize()},
    };

    // Сочетания клавиш, которым суждено разойтись по системам. Пока их два —
    // ходьба по изменённым местам в истории: на маке F4 занята системой.
    QJsonObject shortcuts{
        {QStringLiteral("diffNext"), a.editor().diffNextKey()},
        {QStringLiteral("diffPrevious"), a.editor().diffPreviousKey()},
    };

    QJsonObject scroll{
        {QStringLiteral("smooth"), a.ui().smoothScroll()},
        {QStringLiteral("smoothMs"), a.ui().smoothScrollMs()},
    };

    QJsonObject zoom{
        {QStringLiteral("step"), a.ui().zoomStep()},
        {QStringLiteral("min"), a.ui().zoomMin()},
        {QStringLiteral("max"), a.ui().zoomMax()},
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
        {QStringLiteral("markdownHighlighting"),
         QJsonObject{{QStringLiteral("accent"), colorToString(a.markdownHighlighting().accent())},
                     {QStringLiteral("codeBackground"),
                      colorToString(a.markdownHighlighting().codeBackground())},
                     {QStringLiteral("link"), colorToString(a.markdownHighlighting().link())},
                     {QStringLiteral("image"), colorToString(a.markdownHighlighting().image())},
                     {QStringLiteral("comment"), colorToString(a.markdownHighlighting().comment())},
                     {QStringLiteral("headingStep"), a.markdownHighlighting().headingStep()},
                     {QStringLiteral("largeHeadingLevels"),
                      a.markdownHighlighting().largeHeadingLevels()}}},
        {QStringLiteral("jsonEditing"),
         QJsonObject{{QStringLiteral("key"), colorToString(a.jsonEditing().key())},
                     {QStringLiteral("string"), colorToString(a.jsonEditing().string())},
                     {QStringLiteral("number"), colorToString(a.jsonEditing().number())},
                     {QStringLiteral("keyword"), colorToString(a.jsonEditing().keyword())},
                     {QStringLiteral("comment"), colorToString(a.jsonEditing().comment())},
                     {QStringLiteral("punctuation"), colorToString(a.jsonEditing().punctuation())},
                     {QStringLiteral("tabIndent"), a.jsonEditing().tabIndent()},
                     {QStringLiteral("commentKey"), a.jsonEditing().commentKey()}}},
        {QStringLiteral("formulas"), formulas},
        {QStringLiteral("images"), images},
        {QStringLiteral("shortcuts"), shortcuts},
        {QStringLiteral("scroll"), scroll},
        {QStringLiteral("zoom"), zoom},
    };
}

void settingsFromJson(const QJsonObject& root, ZSettings& a) {
    const QJsonObject font = root.value(QStringLiteral("font")).toObject();
    readString(font, "family", a.style(), &ZDocStyle::setFontFamily);
    readReal(font, "pointSize", a.style(), &ZDocStyle::setBaseFontPoint);
    readString(font, "symbolFamily", a.style(), &ZDocStyle::setSymbolFamily);
    readString(font, "codeFamily", a.style(), &ZDocStyle::setCodeFamily);
    readString(font, "codeLangFamily", a.style(), &ZDocStyle::setCodeLangFamily);
    readInt(font, "codeStep", a.style(), &ZDocStyle::setCodeStep);
    readInt(font, "diffStep", a.style(), &ZDocStyle::setDiffStep);
    readInt(font, "fallbackStep", a.style(), &ZDocStyle::setFallbackStep);
    const QJsonArray headings = font.value(QStringLiteral("headingStep")).toArray();
    {
        ZSettings::HeadingSteps steps = a.style().headingStep();
        for (int i = 0; i < headings.size() && i < int(steps.size()); ++i)
            if (headings.at(i).isDouble()) steps[size_t(i)] = headings.at(i).toInt();
        a.style().setHeadingStep(steps);
    }

    const QJsonObject formulas = root.value(QStringLiteral("formulas")).toObject();
    readReal(formulas, "inlineScale", a.formulas(), &ZSettings::Formulas::setInlineScale);
    readReal(formulas, "displayScale", a.formulas(), &ZSettings::Formulas::setDisplayScale);

    const QJsonObject markdown = root.value(QStringLiteral("markdownHighlighting")).toObject();
    readColor(markdown, "accent", a.markdownHighlighting(), &ZSettings::MarkdownHighlighting::setAccent);
    readColor(markdown, "codeBackground", a.markdownHighlighting(),
              &ZSettings::MarkdownHighlighting::setCodeBackground);
    readColor(markdown, "link", a.markdownHighlighting(), &ZSettings::MarkdownHighlighting::setLink);
    readColor(markdown, "image", a.markdownHighlighting(), &ZSettings::MarkdownHighlighting::setImage);
    readColor(markdown, "comment", a.markdownHighlighting(), &ZSettings::MarkdownHighlighting::setComment);
    readInt(markdown, "headingStep", a.markdownHighlighting(),
            &ZSettings::MarkdownHighlighting::setHeadingStep);
    readInt(markdown, "largeHeadingLevels", a.markdownHighlighting(),
            &ZSettings::MarkdownHighlighting::setLargeHeadingLevels);

    const QJsonObject json = root.value(QStringLiteral("jsonEditing")).toObject();
    readColor(json, "key", a.jsonEditing(), &ZSettings::JsonEditing::setKey);
    readColor(json, "string", a.jsonEditing(), &ZSettings::JsonEditing::setString);
    readColor(json, "number", a.jsonEditing(), &ZSettings::JsonEditing::setNumber);
    readColor(json, "keyword", a.jsonEditing(), &ZSettings::JsonEditing::setKeyword);
    readColor(json, "comment", a.jsonEditing(), &ZSettings::JsonEditing::setComment);
    readColor(json, "punctuation", a.jsonEditing(), &ZSettings::JsonEditing::setPunctuation);
    readInt(json, "tabIndent", a.jsonEditing(), &ZSettings::JsonEditing::setTabIndent);
    readString(json, "commentKey", a.jsonEditing(), &ZSettings::JsonEditing::setCommentKey);

    const QJsonObject tables = root.value(QStringLiteral("tables")).toObject();
    readReal(tables, "cellPadding", a.tables(), &ZSettings::Tables::setCellPadding);
    readReal(tables, "cellPaddingY", a.tables(), &ZSettings::Tables::setCellPaddingY);
    readColor(tables, "borderColor", a.tables(), &ZSettings::Tables::setBorderColor);
    readReal(tables, "horizontalBorder", a.tables(), &ZSettings::Tables::setHorizontalBorder);
    readReal(tables, "verticalBorder", a.tables(), &ZSettings::Tables::setVerticalBorder);
    readReal(tables, "headerSeparator", a.tables(), &ZSettings::Tables::setHeaderSeparator);
    readReal(tables, "rowSeparator", a.tables(), &ZSettings::Tables::setRowSeparator);
    readReal(tables, "columnSeparator", a.tables(), &ZSettings::Tables::setColumnSeparator);
    readColor(tables, "headerColor", a.tables(), &ZSettings::Tables::setHeaderColor);
    readColor(tables, "tableColor", a.tables(), &ZSettings::Tables::setTableColor);
    readColor(tables, "altTableColor", a.tables(), &ZSettings::Tables::setAltTableColor);

    const QJsonObject paper = root.value(QStringLiteral("pdf")).toObject();
    readString(paper, "fontFamily", a.pdf(), &ZSettings::Pdf::setFontFamily);
    readReal(paper, "pointSize", a.pdf(), &ZSettings::Pdf::setPointSize);
    readString(paper, "codeFamily", a.pdf(), &ZSettings::Pdf::setCodeFamily);
    readInt(paper, "codeStep", a.pdf(), &ZSettings::Pdf::setCodeStep);
    readString(paper, "pageSize", a.pdf(), &ZSettings::Pdf::setPageSize);
    readReal(paper, "marginMm", a.pdf(), &ZSettings::Pdf::setMarginMm);
    readInt(paper, "imageDpi", a.pdf(), &ZSettings::Pdf::setImageDpi);
    readInt(paper, "maxExportedImageSize", a.pdf(), &ZSettings::Pdf::setMaxExportedImageSize);
    readReal(paper, "codeStripHeight", a.pdf(), &ZSettings::Pdf::setCodeStripHeight);
    const QJsonArray paperHeads = paper.value(QStringLiteral("headingStep")).toArray();
    {
        ZSettings::HeadingSteps steps = a.pdf().headingStep();
        for (int i = 0; i < paperHeads.size() && i < int(steps.size()); ++i)
            if (paperHeads.at(i).isDouble()) steps[size_t(i)] = paperHeads.at(i).toInt();
        a.pdf().setHeadingStep(steps);
    }

    const QJsonObject layout = root.value(QStringLiteral("layout")).toObject();
    readReal(layout, "lineHeightFactor", a.style(), &ZDocStyle::setLineHeightFactor);
    readReal(layout, "listLineHeightFactor", a.style(), &ZDocStyle::setListLineHeightFactor);
    readReal(layout, "blockSpacing", a.style(), &ZDocStyle::setBlockSpacing);
    readReal(layout, "listIndent", a.style(), &ZDocStyle::setListIndent);
    readReal(layout, "codeIndent", a.style(), &ZDocStyle::setCodeIndent);
    readReal(layout, "codePadLeft", a.style(), &ZDocStyle::setCodePadLeft);
    readReal(layout, "codeStripHeight", a.style(), &ZDocStyle::setCodeStripHeight);
    readReal(layout, "codePadTop", a.style(), &ZDocStyle::setCodePadTop);
    readReal(layout, "codeCornerRadius", a.style(), &ZDocStyle::setCodeCornerRadius);
    readReal(layout, "codeCopyIconScale", a.style(), &ZDocStyle::setCodeCopyIconScale);
    readReal(layout, "codeLangPointSize", a.style(), &ZDocStyle::setCodeLangPointSize);
    readReal(layout, "codeStripPadding", a.style(), &ZDocStyle::setCodeStripPadding);
    readReal(layout, "codeLangGap", a.style(), &ZDocStyle::setCodeLangGap);
    readReal(layout, "quoteIndent", a.style(), &ZDocStyle::setQuoteIndent);
    readReal(layout, "sideMargin", a.style(), &ZDocStyle::setSideMargin);
    readReal(layout, "verticalMargin", a.style(), &ZDocStyle::setVerticalMargin);
    readReal(layout, "maxContentWidth", a.style(), &ZDocStyle::setMaxContentWidth);
    readReal(layout, "caretWidth", a.style(), &ZDocStyle::setCaretWidth);
    readReal(layout, "dividerWidth", a.style(), &ZDocStyle::setDividerWidth);

    const QJsonObject colors = root.value(QStringLiteral("colors")).toObject();
    readColor(colors, "pageBackground", a.style(), &ZDocStyle::setPageBackground);
    readColor(colors, "historyBackground", a.style(), &ZDocStyle::setHistoryBackground);
    readColor(colors, "selectionBackground", a.style(), &ZDocStyle::setSelectionBackground);
    readColor(colors, "searchHighlight", a.style(), &ZDocStyle::setSearchHighlight);
    readColor(colors, "diffAdded", a.style(), &ZDocStyle::setDiffAdded);
    readColor(colors, "diffRemoved", a.style(), &ZDocStyle::setDiffRemoved);
    readColor(colors, "diffChanged", a.style(), &ZDocStyle::setDiffChanged);
    readColor(colors, "link", a.style(), &ZDocStyle::setLinkColor);
    readColor(colors, "quote", a.style(), &ZDocStyle::setQuoteColor);
    readColor(colors, "rawSource", a.style(), &ZDocStyle::setRawColor);
    readColor(colors, "divider", a.style(), &ZDocStyle::setDividerColor);
    readColor(colors, "codeBackground", a.style(), &ZDocStyle::setCodeBackground);
    readColor(colors, "codeLang", a.style(), &ZDocStyle::setCodeLangColor);
    readColor(colors, "caret", a.style(), &ZDocStyle::setCaretColor);

    const QJsonObject list = root.value(QStringLiteral("list")).toObject();
    readColor(list, "bulletColor", a.style(), &ZDocStyle::setBulletColor);
    readColor(list, "orderedColor", a.style(), &ZDocStyle::setOrderedColor);
    const QJsonValue bulletStyle = list.value(QStringLiteral("bulletStyle"));
    if (bulletStyle.isString()) {
        a.style().setBulletStyle(bulletStyle.toString() == QLatin1String("glyph") ? BulletStyle::Glyph
                                                                               : BulletStyle::Drawn);
    }
    readReal(list, "bulletDiameter", a.style(), &ZDocStyle::setBulletDiameter);
    readReal(list, "bulletRise", a.style(), &ZDocStyle::setBulletRise);
    readReal(list, "bulletStrokeWidth", a.style(), &ZDocStyle::setBulletStrokeWidth);
    readReal(list, "bulletSquareSide", a.style(), &ZDocStyle::setBulletSquareSide);

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
        if (!parsed.empty()) a.style().setBulletShapes(std::move(parsed));
    }
    readReal(list, "orderedRise", a.style(), &ZDocStyle::setOrderedRise);
    readString(list, "bullet", a.style(), &ZDocStyle::setBulletGlyph);
    readReal(list, "bulletScale", a.style(), &ZDocStyle::setBulletScale);
    readReal(list, "bulletTextGap", a.style(), &ZDocStyle::setBulletTextGap);
    readReal(list, "orderedTextGap", a.style(), &ZDocStyle::setOrderedTextGap);

    const QJsonObject checkbox = root.value(QStringLiteral("checkbox")).toObject();
    readStyle(checkbox, "style", a.style(), &ZDocStyle::setCheckboxStyle);
    readColor(checkbox, "checkedColor", a.style(), &ZDocStyle::setCheckboxCheckedColor);
    readColor(checkbox, "uncheckedColor", a.style(), &ZDocStyle::setCheckboxUncheckedColor);
    readColor(checkbox, "tickColor", a.style(), &ZDocStyle::setCheckboxTickColor);
    readReal(checkbox, "penWidth", a.style(), &ZDocStyle::setCheckboxPenWidth);
    readReal(checkbox, "cornerRadius", a.style(), &ZDocStyle::setCheckboxCornerRadius);
    readReal(checkbox, "opticalRise", a.style(), &ZDocStyle::setCheckboxOpticalRise);
    readReal(checkbox, "glyphScale", a.style(), &ZDocStyle::setCheckboxGlyphScale);
    readReal(checkbox, "textGap", a.style(), &ZDocStyle::setCheckboxTextGap);

    const QJsonObject notes = root.value(QStringLiteral("notes")).toObject();
    readString(notes, "root", a.store(), &ZSettings::Store::setNotesRoot);
    readString(notes, "title", a.store(), &ZSettings::Store::setStoreTitle);
    const QJsonValue watch = notes.value(QStringLiteral("watchFolder"));
    if (watch.isBool()) a.store().setWatchStore(watch.toBool());
    const QJsonObject noteList = root.value(QStringLiteral("noteList")).toObject();
    readInt(noteList, "width", a.ui(), &ZSettings::Ui::setNoteListWidth);
    readInt(noteList, "snippetLines", a.ui(), &ZSettings::Ui::setNoteListSnippetLines);
    readColor(noteList, "snippetColor", a.ui(), &ZSettings::Ui::setNoteListSnippetColor);
    readColor(noteList, "dateColor", a.ui(), &ZSettings::Ui::setNoteListDateColor);

    const QJsonObject sidebar = root.value(QStringLiteral("sidebar")).toObject();
    readString(sidebar, "fontFamily", a.ui(), &ZSettings::Ui::setSidebarFontFamily);
    readReal(sidebar, "fontSize", a.ui(), &ZSettings::Ui::setSidebarFontPoint);
    readReal(sidebar, "lineHeightFactor", a.ui(), &ZSettings::Ui::setSidebarLineHeightFactor);
    readColor(sidebar, "folderColor", a.ui(), &ZSettings::Ui::setSidebarFolderColor);
    readReal(sidebar, "folderScale", a.ui(), &ZSettings::Ui::setSidebarFolderScale);
    readInt(sidebar, "width", a.ui(), &ZSettings::Ui::setSidebarWidth);
    const QJsonObject imageSelection =
        root.value(QStringLiteral("imageSelection")).toObject();
    readReal(imageSelection, "cornerShare", a.style(), &ZDocStyle::setImageCornerShare);
    readInt(imageSelection, "cornerMinLength", a.style(), &ZDocStyle::setImageCornerMinLength);
    readReal(imageSelection, "cornerWidth", a.style(), &ZDocStyle::setImageCornerWidth);
    readReal(imageSelection, "cornerOffset", a.style(), &ZDocStyle::setImageCornerOffset);
    const QJsonObject imageCaption = root.value(QStringLiteral("imageCaption")).toObject();
    readBool(imageCaption, "shown", a.style(), &ZDocStyle::setImageCaption);
    const QJsonValue captionFamily = imageCaption.value(QStringLiteral("family"));
    if (captionFamily.isString()) a.style().setImageCaptionFamily(captionFamily.toString());
    readReal(imageCaption, "fontPoints", a.style(), &ZDocStyle::setImageCaptionPoints);
    readReal(imageCaption, "gap", a.style(), &ZDocStyle::setImageCaptionGap);
    readColor(imageCaption, "color", a.style(), &ZDocStyle::setImageCaptionColor);
    // Битый регэксп настройку не меняет: молча прятать все подписи (или ни
    // одной) из-за опечатки в конфиге нельзя.
    const QJsonValue noname = imageCaption.value(QStringLiteral("noname"));
    if (noname.isString()) {
        const QRegularExpression candidate(noname.toString(),
                                           QRegularExpression::CaseInsensitiveOption);
        if (candidate.isValid()) a.style().setImageNonameCaption(candidate);
    }

    const QJsonObject statusBar = root.value(QStringLiteral("statusBar")).toObject();
    const QJsonValue statusFamily = statusBar.value(QStringLiteral("family"));
    if (statusFamily.isString()) a.ui().setStatusFamily(statusFamily.toString());
    readInt(statusBar, "fontPoints", a.ui(), &ZSettings::Ui::setStatusFontPoints);
    readInt(statusBar, "padding", a.ui(), &ZSettings::Ui::setStatusPadding);
    readInt(statusBar, "paddingTop", a.ui(), &ZSettings::Ui::setStatusPaddingTop);
    readColor(statusBar, "background", a.ui(), &ZSettings::Ui::setStatusBackground);
    readColor(statusBar, "textColor", a.ui(), &ZSettings::Ui::setStatusTextColor);
    readColor(statusBar, "separatorColor", a.ui(), &ZSettings::Ui::setStatusSeparatorColor);

    const QJsonObject toolbar = root.value(QStringLiteral("toolbar")).toObject();
    // Иконка меньше двенадцати точек перестаёт читаться, больше шестидесяти
    // ломает высоту тулбара; границы — у самой настройки (ZM_SETTING в
    // settings.h), как и у всех остальных чисел.
    readInt(toolbar, "iconSize", a.ui(), &ZSettings::Ui::setToolbarIconSize);
    readInt(toolbar, "buttonPadding", a.ui(), &ZSettings::Ui::setToolbarButtonPadding);
    readInt(toolbar, "groupSpacing", a.ui(), &ZSettings::Ui::setToolbarGroupSpacing);
    readColor(toolbar, "background", a.ui(), &ZSettings::Ui::setToolbarBackground);
    readColor(toolbar, "iconColor", a.ui(), &ZSettings::Ui::setToolbarIconColor);
    readColor(toolbar, "iconHoverColor", a.ui(), &ZSettings::Ui::setToolbarIconHoverColor);
    readColor(toolbar, "iconOnColor", a.ui(), &ZSettings::Ui::setToolbarIconOnColor);
    readColor(toolbar, "iconMarkColor", a.ui(), &ZSettings::Ui::setToolbarIconMarkColor);
    readColor(toolbar, "iconDisabledColor", a.ui(), &ZSettings::Ui::setToolbarIconDisabledColor);
    readColor(toolbar, "hoverBackground", a.ui(), &ZSettings::Ui::setToolbarHoverBackground);
    readColor(toolbar, "separatorColor", a.ui(), &ZSettings::Ui::setToolbarSeparatorColor);

    const QJsonObject find = root.value(QStringLiteral("find")).toObject();
    readReal(find, "fontDelta", a.ui(), &ZSettings::Ui::setFindFontDelta);
    readString(find, "previousGlyph", a.ui(), &ZSettings::Ui::setFindPreviousGlyph);
    readString(find, "nextGlyph", a.ui(), &ZSettings::Ui::setFindNextGlyph);
    readString(find, "historyGlyph", a.ui(), &ZSettings::Ui::setFindHistoryGlyph);
    readInt(find, "historyLimit", a.ui(), &ZSettings::Ui::setFindHistoryLimit);
    const QJsonObject shortcuts = root.value(QStringLiteral("shortcuts")).toObject();
    readString(shortcuts, "diffNext", a.editor(), &ZSettings::Editor::setDiffNextKey);
    readString(shortcuts, "diffPrevious", a.editor(), &ZSettings::Editor::setDiffPreviousKey);

    const QJsonObject editor = root.value(QStringLiteral("editor")).toObject();
    readInt(editor, "autosaveDelayMs", a.editor(), &ZSettings::Editor::setAutosaveDelayMs);
    readInt(editor, "undoCoalesceMs", a.editor(), &ZSettings::Editor::setUndoCoalesceMs);
    readInt(editor, "undoRunChars", a.editor(), &ZSettings::Editor::setUndoRunChars);
    readInt(editor, "codeTabWidth", a.editor(), &ZSettings::Editor::setCodeTabWidth);
    readInt(editor, "historyMergeChars", a.history(), &ZSettings::History::setHistoryMergeChars);
    readInt(editor, "historyMergeHours", a.history(), &ZSettings::History::setHistoryMergeHours);
    readInt(editor, "undoLimit", a.editor(), &ZSettings::Editor::setUndoLimit);
    readInt(editor, "undoBudgetMb", a.editor(), &ZSettings::Editor::setUndoBudgetMb);
    readInt(editor, "imageCacheSizeMb", a.cache(), &ZSettings::Cache::setImageCacheSizeMb);
    readInt(editor, "maxLoadedImageSize", a.cache(), &ZSettings::Cache::setMaxLoadedImageSize);
    readInt(editor, "documentCacheSizeMb", a.cache(), &ZSettings::Cache::setDocumentCacheSizeMb);
    readString(editor, "toggleTaskKey", a.editor(), &ZSettings::Editor::setToggleTaskKey);
    readString(editor, "moveUpKey", a.editor(), &ZSettings::Editor::setMoveUpKey);
    readString(editor, "moveDownKey", a.editor(), &ZSettings::Editor::setMoveDownKey);
    readString(editor, "makeBulletKey", a.editor(), &ZSettings::Editor::setMakeBulletKey);
    readString(editor, "makeOrderedKey", a.editor(), &ZSettings::Editor::setMakeOrderedKey);
    readString(editor, "makeTaskKey", a.editor(), &ZSettings::Editor::setMakeTaskKey);
    readString(editor, "makeParagraphKey", a.editor(), &ZSettings::Editor::setMakeParagraphKey);
    readString(editor, "makeCommentKey", a.editor(), &ZSettings::Editor::setMakeCommentKey);
    readString(editor, "markdownModeKey", a.editor(),
               &ZSettings::Editor::setMarkdownModeKey);
    // Автозамены: список пар [сочетание, что вставить]. Заданный список
    // заменяет умолчания целиком — иначе от умолчания было бы не избавиться.
    const QJsonValue special = editor.value(QStringLiteral("special"));
    if (special.isArray()) {
        ZSettings::KeyPairs pairs;
        for (const QJsonValue& entry : special.toArray()) {
            const QJsonArray pair = entry.toArray();
            if (pair.size() != 2 || !pair.at(0).isString() || !pair.at(1).isString())
                continue;   // битую запись пропускаем, соседние живут
            const QString keys = pair.at(0).toString();
            if (keys.isEmpty()) continue;
            pairs.push_back({keys, pair.at(1).toString()});
        }
        a.editor().setSpecialKeys(std::move(pairs));
    }
    readString(editor, "externalEditor", a.editor(), &ZSettings::Editor::setExternalEditor);

    // Имя своё, а не "images": в этой же области уже живёт значение ключа
    // editor.imageCacheSizeMb под этим именем.
    const QJsonObject importGroup = root.value(QStringLiteral("images")).toObject();
    readInt(importGroup, "maxImportedImageSize", a.images(), &ZSettings::Images::setMaxImportedImageSize);

    const QJsonObject scroll = root.value(QStringLiteral("scroll")).toObject();
    readBool(scroll, "smooth", a.ui(), &ZSettings::Ui::setSmoothScroll);
    readInt(scroll, "smoothMs", a.ui(), &ZSettings::Ui::setSmoothScrollMs);

    const QJsonObject zoom = root.value(QStringLiteral("zoom")).toObject();
    readReal(zoom, "step", a.ui(), &ZSettings::Ui::setZoomStep);
    readReal(zoom, "min", a.ui(), &ZSettings::Ui::setZoomMin);
    readReal(zoom, "max", a.ui(), &ZSettings::Ui::setZoomMax);
}


}  // namespace

const ZSettings& settings() { return g_settings; }
// Люк наборов (tests/settings_hook.h): в боевых заголовках его нет.
ZSettings& mutableSettingsForTests() { return g_settings; }

CodePlate codePlate(const ZDocStyle& look, qreal scale) {
    if (!(scale > 0.0)) scale = 1.0;
    // Единицы те же, что у сборщика документа: по вертикали — высота строки
    // кода (гарнитура текста в кегле кода, как её считает document_builder),
    // по горизонтали — ширина "A" основного шрифта.
    //
    // Масштаба здесь нет и быть не может: геометрия документа строится один раз
    // и живёт в пикселях, а зум — это шрифт документа. Спрашивать масштаб тут
    // значило бы разойтись с резервом, который сборщик уже положил в поля
    // блока.
    QFont base{QString(look.fontFamily())};
    base.setPointSizeF(look.baseFontPoint() * scale);
    base.setStyleHint(QFont::Monospace);
    const qreal charUnit = QFontMetricsF(base).horizontalAdvance(QLatin1Char('A'));

    QFont codeLine = base;
    codeLine.setPointSizeF(look.baseFontPoint() * scale * fontStepFactor(look.codeStep()));
    const qreal lineUnit =
        std::round(QFontMetricsF(codeLine).height() * look.lineHeightFactor());

    // Полоска и значок — от ШРИФТА ПОДПИСИ (см. codeStripHeight в settings.h):
    // один регулятор — кегль подписи.
    const qreal langUnit = QFontMetricsF(codeLangFont(look, scale)).height();

    CodePlate plate;
    plate.strip = std::round(look.codeStripHeight() * langUnit);
    plate.padTop = std::round(look.codePadTop() * lineUnit);
    plate.iconSide = std::round(look.codeCopyIconScale() * langUnit);
    plate.padLeft = look.codePadLeft() * charUnit;
    plate.indent = look.codeIndent() * charUnit;
    plate.radius = look.codeCornerRadius() * scale;
    plate.stripPadding = look.codeStripPadding() * charUnit;
    plate.langGap = look.codeLangGap() * charUnit;
    return plate;
}

QFont codeLangFont(const ZDocStyle& style, qreal scale) {
    QFont font{QString(style.codeLangFamily())};
    font.setPointSizeF(style.codeLangPointSize() * (scale > 0.0 ? scale : 1.0));
    return font;
}

QFont codeLangFont() { return codeLangFont(g_settings.style()); }


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

QByteArray configTemplate() {
    // Всё тело — комментарием, снаружи пустой объект. Так файл и остаётся
    // действующим (отклонений нет), и служит меню: раскомментировал строку —
    // получил отклонение.
    const QList<QByteArray> lines = defaultSettingsJson().split('\n');
    QByteArray out =
        "// zametti configuration. EVERYTHING that can be tuned is listed here with\n"
        "// its default value, and everything is commented out: the effective config\n"
        "// is the list of DEVIATIONS from the defaults, not a copy of them.\n"
        "// Uncomment a line (remove the leading \"//\") to make the value yours.\n"
        "//\n"
        "// Only \"//\" comments to the end of line are understood. They are not\n"
        "// stripped inside quotes, so \"https://\" is fine.\n"
        "{\n";
    for (const QByteArray& line : lines) {
        const QByteArray trimmed = line.trimmed();
        if (trimmed.isEmpty() || trimmed == "{" || trimmed == "}") continue;
        out += "    // " + line.trimmed() + "\n";
    }
    out += "}\n";
    return out;
}

bool writeConfigTemplate(QString* error) {
    const QString path = configPath();
    if (QFile::exists(path)) return true;   // там правки человека
    QDir().mkpath(QFileInfo(path).absolutePath());

    const QByteArray out = configTemplate();
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error != nullptr)
            *error = QStringLiteral("cannot write config: %1 (%2)").arg(path, file.errorString());
        return false;
    }
    if (file.write(out) != out.size()) {
        if (error != nullptr) *error = QStringLiteral("config written incompletely: ") + path;
        return false;
    }
    return true;
}

QString configPath() {
    return QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) +
           QStringLiteral("/config.json");
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
    if (g_settings.cache().maxLoadedImageSize() > 0) return g_settings.cache().maxLoadedImageSize();
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
    const int budget = qMax(8, g_settings.cache().imageCacheSizeMb());
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
            if (error != nullptr) *error = QStringLiteral("cannot read: ") + path;
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
        // С чистого листа: конфиг — отклонения от умолчаний (см. заголовок).
        g_settings = ZSettings{};
        settingsFromJson(doc.object(), g_settings);
    }
    applyImageAllocationLimit();
    return true;
}

}  // namespace zametti
