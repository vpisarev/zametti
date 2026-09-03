#include "theme.h"

#include "settings.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSet>

namespace zametti {
namespace {

// ВСЕ РОЛИ И ВСТРОЕННАЯ СВЕТЛАЯ ТЕМА — ОДНОЙ ТАБЛИЦЕЙ. Значения здесь ровно
// те, которыми программа красилась до появления тем: светлая тема — это не
// новый облик, а нынешний, названный по ролям.
struct Role {
    const char* name;
    const char* light;
    const char* note;
};

const Role kRoles[] = {
    // --- страница и текст ---
    {"background", "#fefefb", "page behind a note"},
    {"foreground", "#1a1a1a", "text, plain bullets, panel icons"},
    {"selectionBackground", "#fae8a8", "selected text"},
    {"selectionForeground", "#453d1f", "text inside a selection; transparent = leave as is"},
    {"searchHighlight", "#e3dac1", "matches of the current search"},
    {"accent", "#da9100", "caret, checked tasks, a pressed toolbar toggle"},
    {"link", "#325cc0", "links"},
    {"mark", "#7c3aed", "a sort order set by the folder itself"},
    {"quote", "#5a626a", "quoted text"},
    {"divider", "#c8cdd2", "the thematic break '---'"},
    {"raw", "#999fa6", "source we could not parse, kept verbatim"},
    {"caption", "#777e86", "picture captions and note-list snippets"},
    {"codeBackground", "#0e000000", "behind code, in blocks and inline"},
    {"codeLang", "#7a8088", "language name on the code plate"},
    {"historyBackground", "#f7f7f5", "page in the history mode: aged paper"},

    // --- панели ---
    {"sidebarBackground", "#fefefb", "tree and note list"},
    {"panelBackground", "#f5f5f2", "toolbar and status bar"},
    {"panelText", "#6b7179", "text of the status bar"},
    {"panelSeparator", "#dde1e5", "hairlines under the toolbar and over the status bar"},
    {"panelHover", "#12000000", "backdrop of a hovered toolbar button"},
    {"panelIconHover", "#000000", "icon under the cursor"},
    {"panelIconOff", "#b8bdc4", "icon of a disabled button"},
    {"listDate", "#8a9098", "date in the note list"},

    // --- знаки ---
    {"checkboxOff", "#acacac", "frame of an unchecked task"},
    {"checkboxTick", "#ffffff", "the tick itself"},

    // --- разность ---
    {"added", "#3fa255", "added lines"},
    {"removed", "#c03939", "removed lines"},
    {"changed", "#e0a850", "changed lines"},
    {"danger", "#c02828", "something went wrong: a suspect save, a broken pattern"},

    // --- таблицы ---
    {"tableBorder", "#000000", "table rules"},
    {"tableHeader", "#00000000", "fill of the header row"},
    {"tableFill", "#00000000", "fill of the body"},
    {"tableAlt", "#00000000", "fill of every other row"},

    // --- просмотр картинки во весь экран ---
    {"viewerBackground", "#101010", "behind a full-screen picture"},
    {"viewerCaption", "#d0d0d0", "its caption"},

    // --- подсветка исходника markdown ---
    // Цвета ЗАГОЛОВКА здесь нет намеренно: в сыром markdown заголовок
    // отличается кеглем и начертанием, а цвет у него — цвет текста. Прибить
    // ему свой означало бы перебить цвет строки разности, поверх которой та же
    // подсветка и работает.
    {"markdown.marker", "#325cc0", "list markers, '#', '>', task boxes, math"},
    {"markdown.comment", "#808080", "<!-- comments -->"},
    {"markdown.image", "#7a3e9d", "![picture](syntax)"},

    // --- подсветка JSON (правка конфига внутри программы) ---
    {"json.key", "#325cc0", "object keys"},
    {"json.string", "#802050", "string values"},
    {"json.number", "#b05a00", "numbers"},
    {"json.keyword", "#7a3e9d", "true, false, null"},
    {"json.comment", "#808080", "comments"},
    {"json.punctuation", "#50565e", "braces, colons, commas"},
};

// Плоское имя роли из вложенного объекта: "markdown" + "marker".
QString joined(const QString& group, const QString& name) {
    return group.isEmpty() ? name : group + QLatin1Char('.') + name;
}

QString themePath(const QString& name) {
    return configDir() + QStringLiteral("/themes/") + name + QStringLiteral(".json");
}

// Разбор темы с наследованием. seen ловит цикл: тема, которая через цепочку
// extends ссылается на себя, иначе крутила бы файлы до конца стека.
bool resolveInto(const QString& name, ZTheme* out, QSet<QString>* seen, QString* error) {
    if (seen->contains(name)) {
        if (error != nullptr)
            *error = QStringLiteral("theme «%1»: extends makes a loop").arg(name);
        return false;
    }
    seen->insert(name);

    if (ZTheme::isBuiltin(name)) {
        for (const Role& role : kRoles)
            out->setColor(QString::fromLatin1(role.name),
                          QColor::fromString(QLatin1String(role.light)));
        return true;
    }

    QFile file(themePath(name));
    if (!file.open(QIODevice::ReadOnly)) {
        if (error != nullptr)
            *error = QStringLiteral("theme «%1»: no built-in theme and no file %2")
                         .arg(name, themePath(name));
        return false;
    }
    QJsonParseError parse{};
    // Те же две поблажки, что у конфига: //-комментарии и висячие запятые.
    const QJsonDocument doc = QJsonDocument::fromJson(stripJsonSugar(file.readAll()), &parse);
    file.close();
    if (parse.error != QJsonParseError::NoError || !doc.isObject()) {
        if (error != nullptr)
            *error = QStringLiteral("theme «%1»: %2").arg(name, parse.errorString());
        return false;
    }
    const QJsonObject object = doc.object();

    // Сперва основание, потом своё поверх. Нет extends — тема самостоятельная,
    // и роли, которых в ней нет, останутся такими, какими были у того, кто её
    // разрешал (у конфига это встроенная светлая): чёрного текста на чёрном
    // фоне из-за забытой роли не выйдет.
    const QJsonValue base = object.value(QStringLiteral("extends"));
    if (base.isString() && !base.toString().isEmpty() &&
        !resolveInto(base.toString(), out, seen, error))
        return false;

    out->applyOverrides(object);
    return true;
}

}  // namespace

QStringList ZTheme::roleNames() {
    QStringList out;
    for (const Role& role : kRoles) out << QString::fromLatin1(role.name);
    return out;
}

std::vector<ZTheme::RoleInfo> ZTheme::roles() {
    std::vector<RoleInfo> out;
    out.reserve(std::size(kRoles));
    for (const Role& role : kRoles)
        out.push_back({QString::fromLatin1(role.name),
                       QColor::fromString(QLatin1String(role.light)),
                       QString::fromLatin1(role.note)});
    return out;
}

QStringList ZTheme::builtinNames() { return {QStringLiteral("light")}; }

bool ZTheme::isBuiltin(const QString& name) { return builtinNames().contains(name); }

bool ZTheme::resolve(const QString& name, ZTheme* out, QString* error) {
    if (out == nullptr) return false;
    // Начинаем со встроенной светлой: тема, задавшая пять ролей из сорока,
    // остаётся рабочей, а не наполовину бесцветной.
    ZTheme resolved;
    QSet<QString> seenBase;
    resolveInto(QStringLiteral("light"), &resolved, &seenBase, nullptr);
    if (name != QLatin1String("light")) {
        QSet<QString> seen;
        if (!resolveInto(name, &resolved, &seen, error)) return false;
    }
    *out = resolved;
    return true;
}

void ZTheme::applyOverrides(const QJsonObject& object, QStringList* unknown) {
    const QStringList known = roleNames();
    for (auto it = object.begin(); it != object.end(); ++it) {
        if (it.key() == QLatin1String("extends")) continue;
        if (it.value().isObject()) {
            const QJsonObject group = it.value().toObject();
            for (auto inner = group.begin(); inner != group.end(); ++inner) {
                const QString role = joined(it.key(), inner.key());
                if (!known.contains(role)) {
                    if (unknown != nullptr) *unknown << QStringLiteral("theme.") + role;
                    continue;
                }
                const QColor parsed = QColor::fromString(inner.value().toString());
                if (parsed.isValid()) setColor(role, parsed);
            }
            continue;
        }
        if (!known.contains(it.key())) {
            if (unknown != nullptr) *unknown << QStringLiteral("theme.") + it.key();
            continue;
        }
        // Битый цвет роль не меняет: опечатка в одной строке не должна красить
        // половину окна в чёрное.
        const QColor parsed = QColor::fromString(it.value().toString());
        if (parsed.isValid()) setColor(it.key(), parsed);
    }
}

QColor ZTheme::color(const QString& role) const { return colors_.value(role); }
bool ZTheme::has(const QString& role) const { return colors_.contains(role); }
void ZTheme::setColor(const QString& role, const QColor& value) { colors_.insert(role, value); }

void ZTheme::applyTo(ZSettings& settings) const {
    // РОЛЬ → КОНКРЕТНЫЕ ПОЛЯ. Одна роль обычно красит несколько мест, и это не
    // экономия строк, а смысл темы: «фон панелей» — одно понятие, а тулбар и
    // полоса сведений — два места, где оно видно.
    const auto colour = [this](const char* role) { return color(QString::fromLatin1(role)); };
    const auto set = [](auto&& setter, const QColor& value) {
        if (value.isValid()) setter(value);
    };

    ZDocStyle& look = settings.style();
    ZSettings::Ui& ui = settings.ui();

    set([&](const QColor& c) { look.setPageBackground(c); }, colour("background"));
    set([&](const QColor& c) { look.setHistoryBackground(c); }, colour("historyBackground"));
    // Цвет текста: им красятся и документ, и списки — через палитру приложения
    // (applyPalette в app). До появления тем своего цвета текста у программы не
    // было вовсе: она стояла на системной палитре, и тёмную тему это делало
    // невозможной в принципе.
    set([&](const QColor& c) { look.setTextColor(c); }, colour("foreground"));
    set([&](const QColor& c) { look.setBulletColor(c); }, colour("foreground"));
    set([&](const QColor& c) { look.setOrderedColor(c); }, colour("foreground"));
    set([&](const QColor& c) { look.setSelectionBackground(c); }, colour("selectionBackground"));
    set([&](const QColor& c) { look.setSelectionForeground(c); }, colour("selectionForeground"));
    set([&](const QColor& c) { look.setSearchHighlight(c); }, colour("searchHighlight"));
    set([&](const QColor& c) { look.setCaretColor(c); }, colour("accent"));
    set([&](const QColor& c) { look.setCheckboxCheckedColor(c); }, colour("accent"));
    set([&](const QColor& c) { look.setCheckboxUncheckedColor(c); }, colour("checkboxOff"));
    set([&](const QColor& c) { look.setCheckboxTickColor(c); }, colour("checkboxTick"));
    set([&](const QColor& c) { look.setLinkColor(c); }, colour("link"));
    set([&](const QColor& c) { look.setQuoteColor(c); }, colour("quote"));
    set([&](const QColor& c) { look.setDividerColor(c); }, colour("divider"));
    set([&](const QColor& c) { look.setRawColor(c); }, colour("raw"));
    set([&](const QColor& c) { look.setImageCaptionColor(c); }, colour("caption"));
    set([&](const QColor& c) { look.setCodeBackground(c); }, colour("codeBackground"));
    set([&](const QColor& c) { look.setCodeLangColor(c); }, colour("codeLang"));
    set([&](const QColor& c) { look.setDiffAdded(c); }, colour("added"));
    set([&](const QColor& c) { look.setDiffRemoved(c); }, colour("removed"));
    set([&](const QColor& c) { look.setDiffChanged(c); }, colour("changed"));

    set([&](const QColor& c) { ui.setSidebarBackground(c); }, colour("sidebarBackground"));
    set([&](const QColor& c) { ui.setSidebarFolderColor(c); }, colour("foreground"));
    set([&](const QColor& c) { ui.setToolbarIconColor(c); }, colour("foreground"));
    set([&](const QColor& c) { ui.setToolbarBackground(c); }, colour("panelBackground"));
    set([&](const QColor& c) { ui.setStatusBackground(c); }, colour("panelBackground"));
    set([&](const QColor& c) { ui.setStatusTextColor(c); }, colour("panelText"));
    set([&](const QColor& c) { ui.setToolbarSeparatorColor(c); }, colour("panelSeparator"));
    set([&](const QColor& c) { ui.setStatusSeparatorColor(c); }, colour("panelSeparator"));
    set([&](const QColor& c) { ui.setToolbarHoverBackground(c); }, colour("panelHover"));
    set([&](const QColor& c) { ui.setToolbarIconHoverColor(c); }, colour("panelIconHover"));
    set([&](const QColor& c) { ui.setToolbarIconDisabledColor(c); }, colour("panelIconOff"));
    set([&](const QColor& c) { ui.setToolbarIconOnColor(c); }, colour("accent"));
    set([&](const QColor& c) { ui.setToolbarIconMarkColor(c); }, colour("mark"));
    set([&](const QColor& c) { ui.setNoteListSnippetColor(c); }, colour("caption"));
    set([&](const QColor& c) { ui.setNoteListDateColor(c); }, colour("listDate"));
    set([&](const QColor& c) { ui.setStatusSuspectColor(c); }, colour("danger"));
    set([&](const QColor& c) { ui.setFindBadPatternColor(c); }, colour("danger"));

    ZSettings::Tables& tables = settings.tables();
    set([&](const QColor& c) { tables.setBorderColor(c); }, colour("tableBorder"));
    set([&](const QColor& c) { tables.setHeaderColor(c); }, colour("tableHeader"));
    set([&](const QColor& c) { tables.setTableColor(c); }, colour("tableFill"));
    set([&](const QColor& c) { tables.setAltTableColor(c); }, colour("tableAlt"));

    ZSettings::ImageViewer& viewer = settings.imageViewer();
    set([&](const QColor& c) { viewer.setBackground(c); }, colour("viewerBackground"));
    set([&](const QColor& c) { viewer.setCaptionColor(c); }, colour("viewerCaption"));

    ZSettings::MarkdownHighlighting& md = settings.markdownHighlighting();
    set([&](const QColor& c) { md.setAccent(c); }, colour("markdown.marker"));
    set([&](const QColor& c) { md.setComment(c); }, colour("markdown.comment"));
    set([&](const QColor& c) { md.setImage(c); }, colour("markdown.image"));
    set([&](const QColor& c) { md.setLink(c); }, colour("link"));
    set([&](const QColor& c) { md.setCodeBackground(c); }, colour("codeBackground"));

    ZSettings::JsonEditing& json = settings.jsonEditing();
    set([&](const QColor& c) { json.setKey(c); }, colour("json.key"));
    set([&](const QColor& c) { json.setString(c); }, colour("json.string"));
    set([&](const QColor& c) { json.setNumber(c); }, colour("json.number"));
    set([&](const QColor& c) { json.setKeyword(c); }, colour("json.keyword"));
    set([&](const QColor& c) { json.setComment(c); }, colour("json.comment"));
    set([&](const QColor& c) { json.setPunctuation(c); }, colour("json.punctuation"));
}

}  // namespace zametti
