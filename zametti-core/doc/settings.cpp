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
    std::fprintf(stderr, "настройка «%s» = %g вне допустимого диапазона — обрезана до края\n",
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
    for (BulletShape shape : a.look().bulletShapes())
        shapes.append(shape == BulletShape::Circle   ? QStringLiteral("circle")
                      : shape == BulletShape::Square ? QStringLiteral("square")
                                                     : QStringLiteral("disc"));

    QJsonArray headings;
    for (int v : a.look().headingStep()) headings.append(v);

    QJsonObject font{
        {QStringLiteral("family"), a.look().fontFamily()},
        {QStringLiteral("pointSize"), a.look().baseFontPoint()},
        {QStringLiteral("symbolFamily"), a.look().symbolFamily()},
        {QStringLiteral("codeFamily"), a.look().codeFamily()},
        {QStringLiteral("codeStep"), a.look().codeStep()},
        {QStringLiteral("headingStep"), headings},
        {QStringLiteral("fallbackStep"), a.look().fallbackStep()},
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
        {QStringLiteral("marginMm"), a.pdf().marginMm()},
        {QStringLiteral("imageDpi"), a.pdf().imageDpi()},
        {QStringLiteral("maxExportedImageSize"), a.pdf().maxExportedImageSize()},
        {QStringLiteral("codeStripHeight"), a.pdf().codeStripHeight()},
    };

    QJsonObject layout{
        {QStringLiteral("lineHeightFactor"), a.look().lineHeightFactor()},
        {QStringLiteral("listLineHeightFactor"), a.look().listLineHeightFactor()},
        {QStringLiteral("blockSpacing"), a.look().blockSpacing()},
        {QStringLiteral("listIndent"), a.look().listIndent()},
        {QStringLiteral("codeIndent"), a.look().codeIndent()},
        {QStringLiteral("codePadLeft"), a.look().codePadLeft()},
        {QStringLiteral("codeStripHeight"), a.look().codeStripHeight()},
        {QStringLiteral("codePadTop"), a.look().codePadTop()},
        {QStringLiteral("codeCornerRadius"), a.look().codeCornerRadius()},
        {QStringLiteral("codeLangPointSize"), a.look().codeLangPointSize()},
        {QStringLiteral("codeStripPadding"), a.look().codeStripPadding()},
        {QStringLiteral("codeLangGap"), a.look().codeLangGap()},
        {QStringLiteral("quoteIndent"), a.look().quoteIndent()},
        {QStringLiteral("sideMargin"), a.look().sideMargin()},
        {QStringLiteral("verticalMargin"), a.look().verticalMargin()},
        {QStringLiteral("maxContentWidth"), a.look().maxContentWidth()},
        {QStringLiteral("caretWidth"), a.look().caretWidth()},
        {QStringLiteral("dividerWidth"), a.look().dividerWidth()},
    };

    QJsonObject colors{
        {QStringLiteral("pageBackground"), colorToString(a.look().pageBackground())},
        {QStringLiteral("historyBackground"), colorToString(a.look().historyBackground())},
        {QStringLiteral("selectionBackground"), colorToString(a.look().selectionBackground())},
        {QStringLiteral("searchHighlight"), colorToString(a.look().searchHighlight())},
        {QStringLiteral("diffAdded"), colorToString(a.look().diffAdded())},
        {QStringLiteral("diffRemoved"), colorToString(a.look().diffRemoved())},
        {QStringLiteral("diffChanged"), colorToString(a.look().diffChanged())},
        {QStringLiteral("link"), colorToString(a.look().linkColor())},
        {QStringLiteral("quote"), colorToString(a.look().quoteColor())},
        {QStringLiteral("rawSource"), colorToString(a.look().rawColor())},
        {QStringLiteral("divider"), colorToString(a.look().dividerColor())},
        {QStringLiteral("codeBackground"), colorToString(a.look().codeBackground())},
        {QStringLiteral("codeLang"), colorToString(a.look().codeLangColor())},
        {QStringLiteral("caret"), colorToString(a.look().caretColor())},
    };

    QJsonObject list{
        {QStringLiteral("bulletColor"), colorToString(a.look().bulletColor())},
        {QStringLiteral("orderedColor"), colorToString(a.look().orderedColor())},
        {QStringLiteral("bulletStyle"),
         a.look().bulletStyle() == BulletStyle::Glyph ? QStringLiteral("glyph")
                                             : QStringLiteral("drawn")},
        {QStringLiteral("bulletDiameter"), a.look().bulletDiameter()},
        {QStringLiteral("bulletStrokeWidth"), a.look().bulletStrokeWidth()},
        {QStringLiteral("bulletSquareSide"), a.look().bulletSquareSide()},
        {QStringLiteral("bulletShapes"), shapes},
        {QStringLiteral("bulletRise"), a.look().bulletRise()},
        {QStringLiteral("orderedRise"), a.look().orderedRise()},
        {QStringLiteral("bullet"), a.look().bulletGlyph()},
        {QStringLiteral("bulletScale"), a.look().bulletScale()},
        {QStringLiteral("bulletTextGap"), a.look().bulletTextGap()},
        {QStringLiteral("orderedTextGap"), a.look().orderedTextGap()},
    };

    QJsonObject checkbox{
        {QStringLiteral("style"), styleToString(a.look().checkboxStyle())},
        {QStringLiteral("checkedColor"), colorToString(a.look().checkboxCheckedColor())},
        {QStringLiteral("uncheckedColor"), colorToString(a.look().checkboxUncheckedColor())},
        {QStringLiteral("tickColor"), colorToString(a.look().checkboxTickColor())},
        {QStringLiteral("penWidth"), a.look().checkboxPenWidth()},
        {QStringLiteral("cornerRadius"), a.look().checkboxCornerRadius()},
        {QStringLiteral("opticalRise"), a.look().checkboxOpticalRise()},
        {QStringLiteral("glyphScale"), a.look().checkboxGlyphScale()},
        {QStringLiteral("textGap"), a.look().checkboxTextGap()},
    };

    QJsonObject notes{
        {QStringLiteral("root"), a.store().notesRoot()},
        {QStringLiteral("title"), a.store().storeTitle()},
        {QStringLiteral("watchFolder"), a.store().watchStore()},
    };

    QJsonObject noteList{
        {QStringLiteral("width"), a.look().noteListWidth()},
        {QStringLiteral("snippetLines"), a.look().noteListSnippetLines()},
        {QStringLiteral("snippetColor"), colorToString(a.look().noteListSnippetColor())},
        {QStringLiteral("dateColor"), colorToString(a.look().noteListDateColor())},
    };

    QJsonObject sidebar{
        {QStringLiteral("fontFamily"), a.look().sidebarFontFamily()},
        {QStringLiteral("fontSize"), a.look().sidebarFontPoint()},
        {QStringLiteral("lineHeightFactor"), a.look().sidebarLineHeightFactor()},
        {QStringLiteral("width"), a.look().sidebarWidth()},
        {QStringLiteral("folderColor"), colorToString(a.look().sidebarFolderColor())},
        {QStringLiteral("folderScale"), a.look().sidebarFolderScale()},
    };

    QJsonObject imageSelection{
        {QStringLiteral("cornerShare"), a.look().imageCornerShare()},
        {QStringLiteral("cornerMinLength"), a.look().imageCornerMinLength()},
        {QStringLiteral("cornerWidth"), a.look().imageCornerWidth()},
        {QStringLiteral("cornerOffset"), a.look().imageCornerOffset()},
    };

    QJsonObject imageCaption{
        {QStringLiteral("shown"), a.look().imageCaption()},
        {QStringLiteral("family"), a.look().imageCaptionFamily()},
        {QStringLiteral("fontPoints"), a.look().imageCaptionPoints()},
        {QStringLiteral("gap"), a.look().imageCaptionGap()},
        {QStringLiteral("color"), colorToString(a.look().imageCaptionColor())},
        {QStringLiteral("noname"), a.look().imageNonameCaption().pattern()},
    };

    QJsonObject statusBar{
        {QStringLiteral("family"), a.look().statusFamily()},
        {QStringLiteral("fontPoints"), a.look().statusFontPoints()},
        {QStringLiteral("padding"), a.look().statusPadding()},
        {QStringLiteral("paddingTop"), a.look().statusPaddingTop()},
        {QStringLiteral("background"), colorToString(a.look().statusBackground())},
        {QStringLiteral("textColor"), colorToString(a.look().statusTextColor())},
        {QStringLiteral("separatorColor"), colorToString(a.look().statusSeparatorColor())},
    };

    QJsonObject toolbar{
        {QStringLiteral("iconSize"), a.look().toolbarIconSize()},
        {QStringLiteral("buttonPadding"), a.look().toolbarButtonPadding()},
        {QStringLiteral("groupSpacing"), a.look().toolbarGroupSpacing()},
        {QStringLiteral("background"), colorToString(a.look().toolbarBackground())},
        {QStringLiteral("iconColor"), colorToString(a.look().toolbarIconColor())},
        {QStringLiteral("iconHoverColor"), colorToString(a.look().toolbarIconHoverColor())},
        {QStringLiteral("iconOnColor"), colorToString(a.look().toolbarIconOnColor())},
        {QStringLiteral("iconMarkColor"), colorToString(a.look().toolbarIconMarkColor())},
        {QStringLiteral("iconDisabledColor"), colorToString(a.look().toolbarIconDisabledColor())},
        {QStringLiteral("hoverBackground"), colorToString(a.look().toolbarHoverBackground())},
        {QStringLiteral("separatorColor"), colorToString(a.look().toolbarSeparatorColor())},
    };

    QJsonObject find{
        {QStringLiteral("fontDelta"), a.look().findFontDelta()},
        {QStringLiteral("previousGlyph"), a.look().findPreviousGlyph()},
        {QStringLiteral("nextGlyph"), a.look().findNextGlyph()},
        {QStringLiteral("historyGlyph"), a.look().findHistoryGlyph()},
        {QStringLiteral("historyLimit"), a.look().findHistoryLimit()},
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
        {QStringLiteral("smooth"), a.look().smoothScroll()},
        {QStringLiteral("smoothMs"), a.look().smoothScrollMs()},
    };

    QJsonObject zoom{
        {QStringLiteral("step"), a.look().zoomStep()},
        {QStringLiteral("min"), a.look().zoomMin()},
        {QStringLiteral("max"), a.look().zoomMax()},
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
    readString(font, "family", a.look(), &ZSettings::Look::setFontFamily);
    readReal(font, "pointSize", a.look(), &ZSettings::Look::setBaseFontPoint);
    readString(font, "symbolFamily", a.look(), &ZSettings::Look::setSymbolFamily);
    readString(font, "codeFamily", a.look(), &ZSettings::Look::setCodeFamily);
    readInt(font, "codeStep", a.look(), &ZSettings::Look::setCodeStep);
    readInt(font, "fallbackStep", a.look(), &ZSettings::Look::setFallbackStep);
    const QJsonArray headings = font.value(QStringLiteral("headingStep")).toArray();
    {
        ZSettings::HeadingSteps steps = a.look().headingStep();
        for (int i = 0; i < headings.size() && i < int(steps.size()); ++i)
            if (headings.at(i).isDouble()) steps[size_t(i)] = headings.at(i).toInt();
        a.look().setHeadingStep(steps);
    }

    const QJsonObject formulas = root.value(QStringLiteral("formulas")).toObject();
    readReal(formulas, "inlineScale", a.formulas(), &ZSettings::Formulas::setInlineScale);
    readReal(formulas, "displayScale", a.formulas(), &ZSettings::Formulas::setDisplayScale);

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
    readReal(layout, "lineHeightFactor", a.look(), &ZSettings::Look::setLineHeightFactor);
    readReal(layout, "listLineHeightFactor", a.look(), &ZSettings::Look::setListLineHeightFactor);
    readReal(layout, "blockSpacing", a.look(), &ZSettings::Look::setBlockSpacing);
    readReal(layout, "listIndent", a.look(), &ZSettings::Look::setListIndent);
    readReal(layout, "codeIndent", a.look(), &ZSettings::Look::setCodeIndent);
    readReal(layout, "codePadLeft", a.look(), &ZSettings::Look::setCodePadLeft);
    readReal(layout, "codeStripHeight", a.look(), &ZSettings::Look::setCodeStripHeight);
    readReal(layout, "codePadTop", a.look(), &ZSettings::Look::setCodePadTop);
    readReal(layout, "codeCornerRadius", a.look(), &ZSettings::Look::setCodeCornerRadius);
    readReal(layout, "codeLangPointSize", a.look(), &ZSettings::Look::setCodeLangPointSize);
    readReal(layout, "codeStripPadding", a.look(), &ZSettings::Look::setCodeStripPadding);
    readReal(layout, "codeLangGap", a.look(), &ZSettings::Look::setCodeLangGap);
    readReal(layout, "quoteIndent", a.look(), &ZSettings::Look::setQuoteIndent);
    readReal(layout, "sideMargin", a.look(), &ZSettings::Look::setSideMargin);
    readReal(layout, "verticalMargin", a.look(), &ZSettings::Look::setVerticalMargin);
    readReal(layout, "maxContentWidth", a.look(), &ZSettings::Look::setMaxContentWidth);
    readReal(layout, "caretWidth", a.look(), &ZSettings::Look::setCaretWidth);
    readReal(layout, "dividerWidth", a.look(), &ZSettings::Look::setDividerWidth);

    const QJsonObject colors = root.value(QStringLiteral("colors")).toObject();
    readColor(colors, "pageBackground", a.look(), &ZSettings::Look::setPageBackground);
    readColor(colors, "historyBackground", a.look(), &ZSettings::Look::setHistoryBackground);
    readColor(colors, "selectionBackground", a.look(), &ZSettings::Look::setSelectionBackground);
    readColor(colors, "searchHighlight", a.look(), &ZSettings::Look::setSearchHighlight);
    readColor(colors, "diffAdded", a.look(), &ZSettings::Look::setDiffAdded);
    readColor(colors, "diffRemoved", a.look(), &ZSettings::Look::setDiffRemoved);
    readColor(colors, "diffChanged", a.look(), &ZSettings::Look::setDiffChanged);
    readColor(colors, "link", a.look(), &ZSettings::Look::setLinkColor);
    readColor(colors, "quote", a.look(), &ZSettings::Look::setQuoteColor);
    readColor(colors, "rawSource", a.look(), &ZSettings::Look::setRawColor);
    readColor(colors, "divider", a.look(), &ZSettings::Look::setDividerColor);
    readColor(colors, "codeBackground", a.look(), &ZSettings::Look::setCodeBackground);
    readColor(colors, "codeLang", a.look(), &ZSettings::Look::setCodeLangColor);
    readColor(colors, "caret", a.look(), &ZSettings::Look::setCaretColor);

    const QJsonObject list = root.value(QStringLiteral("list")).toObject();
    readColor(list, "bulletColor", a.look(), &ZSettings::Look::setBulletColor);
    readColor(list, "orderedColor", a.look(), &ZSettings::Look::setOrderedColor);
    const QJsonValue bulletStyle = list.value(QStringLiteral("bulletStyle"));
    if (bulletStyle.isString()) {
        a.look().setBulletStyle(bulletStyle.toString() == QLatin1String("glyph") ? BulletStyle::Glyph
                                                                               : BulletStyle::Drawn);
    }
    readReal(list, "bulletDiameter", a.look(), &ZSettings::Look::setBulletDiameter);
    readReal(list, "bulletRise", a.look(), &ZSettings::Look::setBulletRise);
    readReal(list, "bulletStrokeWidth", a.look(), &ZSettings::Look::setBulletStrokeWidth);
    readReal(list, "bulletSquareSide", a.look(), &ZSettings::Look::setBulletSquareSide);

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
        if (!parsed.empty()) a.look().setBulletShapes(std::move(parsed));
    }
    readReal(list, "orderedRise", a.look(), &ZSettings::Look::setOrderedRise);
    readString(list, "bullet", a.look(), &ZSettings::Look::setBulletGlyph);
    readReal(list, "bulletScale", a.look(), &ZSettings::Look::setBulletScale);
    readReal(list, "bulletTextGap", a.look(), &ZSettings::Look::setBulletTextGap);
    readReal(list, "orderedTextGap", a.look(), &ZSettings::Look::setOrderedTextGap);

    const QJsonObject checkbox = root.value(QStringLiteral("checkbox")).toObject();
    readStyle(checkbox, "style", a.look(), &ZSettings::Look::setCheckboxStyle);
    readColor(checkbox, "checkedColor", a.look(), &ZSettings::Look::setCheckboxCheckedColor);
    readColor(checkbox, "uncheckedColor", a.look(), &ZSettings::Look::setCheckboxUncheckedColor);
    readColor(checkbox, "tickColor", a.look(), &ZSettings::Look::setCheckboxTickColor);
    readReal(checkbox, "penWidth", a.look(), &ZSettings::Look::setCheckboxPenWidth);
    readReal(checkbox, "cornerRadius", a.look(), &ZSettings::Look::setCheckboxCornerRadius);
    readReal(checkbox, "opticalRise", a.look(), &ZSettings::Look::setCheckboxOpticalRise);
    readReal(checkbox, "glyphScale", a.look(), &ZSettings::Look::setCheckboxGlyphScale);
    readReal(checkbox, "textGap", a.look(), &ZSettings::Look::setCheckboxTextGap);

    const QJsonObject notes = root.value(QStringLiteral("notes")).toObject();
    readString(notes, "root", a.store(), &ZSettings::Store::setNotesRoot);
    readString(notes, "title", a.store(), &ZSettings::Store::setStoreTitle);
    const QJsonValue watch = notes.value(QStringLiteral("watchFolder"));
    if (watch.isBool()) a.store().setWatchStore(watch.toBool());
    const QJsonObject noteList = root.value(QStringLiteral("noteList")).toObject();
    readInt(noteList, "width", a.look(), &ZSettings::Look::setNoteListWidth);
    readInt(noteList, "snippetLines", a.look(), &ZSettings::Look::setNoteListSnippetLines);
    readColor(noteList, "snippetColor", a.look(), &ZSettings::Look::setNoteListSnippetColor);
    readColor(noteList, "dateColor", a.look(), &ZSettings::Look::setNoteListDateColor);

    const QJsonObject sidebar = root.value(QStringLiteral("sidebar")).toObject();
    readString(sidebar, "fontFamily", a.look(), &ZSettings::Look::setSidebarFontFamily);
    readReal(sidebar, "fontSize", a.look(), &ZSettings::Look::setSidebarFontPoint);
    readReal(sidebar, "lineHeightFactor", a.look(), &ZSettings::Look::setSidebarLineHeightFactor);
    readColor(sidebar, "folderColor", a.look(), &ZSettings::Look::setSidebarFolderColor);
    readReal(sidebar, "folderScale", a.look(), &ZSettings::Look::setSidebarFolderScale);
    readInt(sidebar, "width", a.look(), &ZSettings::Look::setSidebarWidth);
    const QJsonObject imageSelection =
        root.value(QStringLiteral("imageSelection")).toObject();
    readReal(imageSelection, "cornerShare", a.look(), &ZSettings::Look::setImageCornerShare);
    readInt(imageSelection, "cornerMinLength", a.look(), &ZSettings::Look::setImageCornerMinLength);
    readReal(imageSelection, "cornerWidth", a.look(), &ZSettings::Look::setImageCornerWidth);
    readReal(imageSelection, "cornerOffset", a.look(), &ZSettings::Look::setImageCornerOffset);
    const QJsonObject imageCaption = root.value(QStringLiteral("imageCaption")).toObject();
    readBool(imageCaption, "shown", a.look(), &ZSettings::Look::setImageCaption);
    const QJsonValue captionFamily = imageCaption.value(QStringLiteral("family"));
    if (captionFamily.isString()) a.look().setImageCaptionFamily(captionFamily.toString());
    readReal(imageCaption, "fontPoints", a.look(), &ZSettings::Look::setImageCaptionPoints);
    readReal(imageCaption, "gap", a.look(), &ZSettings::Look::setImageCaptionGap);
    readColor(imageCaption, "color", a.look(), &ZSettings::Look::setImageCaptionColor);
    // Битый регэксп настройку не меняет: молча прятать все подписи (или ни
    // одной) из-за опечатки в конфиге нельзя.
    const QJsonValue noname = imageCaption.value(QStringLiteral("noname"));
    if (noname.isString()) {
        const QRegularExpression candidate(noname.toString(),
                                           QRegularExpression::CaseInsensitiveOption);
        if (candidate.isValid()) a.look().setImageNonameCaption(candidate);
    }

    const QJsonObject statusBar = root.value(QStringLiteral("statusBar")).toObject();
    const QJsonValue statusFamily = statusBar.value(QStringLiteral("family"));
    if (statusFamily.isString()) a.look().setStatusFamily(statusFamily.toString());
    readInt(statusBar, "fontPoints", a.look(), &ZSettings::Look::setStatusFontPoints);
    readInt(statusBar, "padding", a.look(), &ZSettings::Look::setStatusPadding);
    readInt(statusBar, "paddingTop", a.look(), &ZSettings::Look::setStatusPaddingTop);
    readColor(statusBar, "background", a.look(), &ZSettings::Look::setStatusBackground);
    readColor(statusBar, "textColor", a.look(), &ZSettings::Look::setStatusTextColor);
    readColor(statusBar, "separatorColor", a.look(), &ZSettings::Look::setStatusSeparatorColor);

    const QJsonObject toolbar = root.value(QStringLiteral("toolbar")).toObject();
    // Иконка меньше двенадцати точек перестаёт читаться, больше шестидесяти
    // ломает высоту тулбара; границы — у самой настройки (ZM_SETTING в
    // settings.h), как и у всех остальных чисел.
    readInt(toolbar, "iconSize", a.look(), &ZSettings::Look::setToolbarIconSize);
    readInt(toolbar, "buttonPadding", a.look(), &ZSettings::Look::setToolbarButtonPadding);
    readInt(toolbar, "groupSpacing", a.look(), &ZSettings::Look::setToolbarGroupSpacing);
    readColor(toolbar, "background", a.look(), &ZSettings::Look::setToolbarBackground);
    readColor(toolbar, "iconColor", a.look(), &ZSettings::Look::setToolbarIconColor);
    readColor(toolbar, "iconHoverColor", a.look(), &ZSettings::Look::setToolbarIconHoverColor);
    readColor(toolbar, "iconOnColor", a.look(), &ZSettings::Look::setToolbarIconOnColor);
    readColor(toolbar, "iconMarkColor", a.look(), &ZSettings::Look::setToolbarIconMarkColor);
    readColor(toolbar, "iconDisabledColor", a.look(), &ZSettings::Look::setToolbarIconDisabledColor);
    readColor(toolbar, "hoverBackground", a.look(), &ZSettings::Look::setToolbarHoverBackground);
    readColor(toolbar, "separatorColor", a.look(), &ZSettings::Look::setToolbarSeparatorColor);

    const QJsonObject find = root.value(QStringLiteral("find")).toObject();
    readReal(find, "fontDelta", a.look(), &ZSettings::Look::setFindFontDelta);
    readString(find, "previousGlyph", a.look(), &ZSettings::Look::setFindPreviousGlyph);
    readString(find, "nextGlyph", a.look(), &ZSettings::Look::setFindNextGlyph);
    readString(find, "historyGlyph", a.look(), &ZSettings::Look::setFindHistoryGlyph);
    readInt(find, "historyLimit", a.look(), &ZSettings::Look::setFindHistoryLimit);
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
    readBool(scroll, "smooth", a.look(), &ZSettings::Look::setSmoothScroll);
    readInt(scroll, "smoothMs", a.look(), &ZSettings::Look::setSmoothScrollMs);

    const QJsonObject zoom = root.value(QStringLiteral("zoom")).toObject();
    readReal(zoom, "step", a.look(), &ZSettings::Look::setZoomStep);
    readReal(zoom, "min", a.look(), &ZSettings::Look::setZoomMin);
    readReal(zoom, "max", a.look(), &ZSettings::Look::setZoomMax);
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
    QFont base{QString(g_settings.look().fontFamily())};
    base.setPointSizeF(g_settings.look().baseFontPoint());
    base.setStyleHint(QFont::Monospace);
    const qreal charUnit = QFontMetricsF(base).horizontalAdvance(QLatin1Char('A'));

    QFont codeLine = base;
    codeLine.setPointSizeF(g_settings.look().baseFontPoint() * fontStepFactor(g_settings.look().codeStep()));
    const qreal lineUnit =
        std::round(QFontMetricsF(codeLine).height() * g_settings.look().lineHeightFactor());

    CodePlate plate;
    plate.strip = std::round(g_settings.look().codeStripHeight() * lineUnit);
    plate.padTop = std::round(g_settings.look().codePadTop() * lineUnit);
    plate.padLeft = g_settings.look().codePadLeft() * charUnit;
    plate.indent = g_settings.look().codeIndent() * charUnit;
    plate.radius = g_settings.look().codeCornerRadius();
    plate.stripPadding = g_settings.look().codeStripPadding() * charUnit;
    plate.langGap = g_settings.look().codeLangGap() * charUnit;
    return plate;
}

QFont codeLangFont() {
    QFont font{QString(g_settings.look().sidebarFontFamily())};
    const qreal point = g_settings.look().codeLangPointSize() > 0.0
                            ? g_settings.look().codeLangPointSize()
                            : g_settings.look().sidebarFontPoint();
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
