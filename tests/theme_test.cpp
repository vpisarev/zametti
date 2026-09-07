// ТЕМЫ: роли вместо россыпи цветов.
//
// Четыре вопроса, и первый из них — главный:
//
//   1. СВЕТЛАЯ ТЕМА — ЭТО НЫНЕШНИЙ ОБЛИК, А НЕ НОВЫЙ. Полсотни цветов уехали
//      из config.json в тему; если хоть один при этом изменился, программа
//      после уборки выглядит иначе, чем до неё, — а такого уговора не было.
//      Сверяется каждое поле, которое тема красит.
//   2. НАСЛЕДОВАНИЕ РАБОТАЕТ: своя тема кладётся поверх встроенной, роли,
//      которых в ней нет, остаются светлыми. Иначе тема из трёх строк давала
//      бы чёрный текст на чёрном фоне.
//   3. НЕИЗВЕСТНОЕ ИМЯ И ЦИКЛ — ОШИБКА, а не молчание: показать не ту тему,
//      которую попросили, хуже, чем сказать вслух. Прежние настройки при этом
//      остаются.
//   4. ОПЕЧАТКА В РОЛИ НАЗЫВАЕТСЯ. «bacground» вместо «background» иначе
//      выглядит как «тема не работает».

#include "settings.h"
#include "theme.h"

#include "settings_hook.h"
#include "test_util.h"
#include "testdata.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <string>
#include <vector>

namespace {

std::string s(const QString& q) { return q.toStdString(); }
std::string hex(const QColor& c) {
    return (c.alpha() == 255 ? c.name(QColor::HexRgb) : c.name(QColor::HexArgb)).toStdString();
}

void writeConfig(const QString& text) {
    QFile file(zametti::configPath());
    QDir().mkpath(QFileInfo(file.fileName()).absolutePath());
    ZT_TRUE("конфиг записан", file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write(text.toUtf8());
}

void writeTheme(const QString& name, const QString& text) {
    const QString path =
        zametti::configDir() + QStringLiteral("/themes/") + name + QStringLiteral(".json");
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    ZT_TRUE("тема записана", file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write(text.toUtf8());
}

// --- 1. СВЕТЛАЯ ТЕМА РАВНА УМОЛЧАНИЯМ ----------------------------------------
void checkLightIsTodaysLook() {
    zametti::ZTheme light;
    QString error;
    ZT_TRUE("светлая тема разрешилась: " + s(error),
            zametti::ZTheme::resolve(QStringLiteral("light"), &light, &error));

    const zametti::ZSettings defaults;
    zametti::ZSettings themed;
    light.applyTo(themed);

    // Каждое поле, которое красит applyTo. Список написан руками нарочно: он и
    // есть вопрос «а всё ли тема покрывает» — поле, забытое в applyTo, здесь
    // покажет расхождение с умолчанием.
    const std::vector<std::pair<const char*, std::pair<QColor, QColor>>> pairs = {
        {"pageBackground", {defaults.style().pageBackground(), themed.style().pageBackground()}},
        {"textColor", {defaults.style().textColor(), themed.style().textColor()}},
        {"historyBackground",
         {defaults.style().historyBackground(), themed.style().historyBackground()}},
        {"bulletColor", {defaults.style().bulletColor(), themed.style().bulletColor()}},
        {"orderedColor", {defaults.style().orderedColor(), themed.style().orderedColor()}},
        {"selectionBackground",
         {defaults.style().selectionBackground(), themed.style().selectionBackground()}},
        {"selectionForeground",
         {defaults.style().selectionForeground(), themed.style().selectionForeground()}},
        {"searchHighlight", {defaults.style().searchHighlight(), themed.style().searchHighlight()}},
        {"caretColor", {defaults.style().caretColor(), themed.style().caretColor()}},
        {"checkboxChecked",
         {defaults.style().checkboxCheckedColor(), themed.style().checkboxCheckedColor()}},
        {"checkboxUnchecked",
         {defaults.style().checkboxUncheckedColor(), themed.style().checkboxUncheckedColor()}},
        {"checkboxTick", {defaults.style().checkboxTickColor(), themed.style().checkboxTickColor()}},
        {"linkColor", {defaults.style().linkColor(), themed.style().linkColor()}},
        {"quoteColor", {defaults.style().quoteColor(), themed.style().quoteColor()}},
        {"dividerColor", {defaults.style().dividerColor(), themed.style().dividerColor()}},
        {"rawColor", {defaults.style().rawColor(), themed.style().rawColor()}},
        {"imageCaptionColor",
         {defaults.style().imageCaptionColor(), themed.style().imageCaptionColor()}},
        {"codeBackground", {defaults.style().codeBackground(), themed.style().codeBackground()}},
        {"codeLangColor", {defaults.style().codeLangColor(), themed.style().codeLangColor()}},
        {"inlineCodeColor", {defaults.style().inlineCodeColor(), themed.style().inlineCodeColor()}},
        {"bookmark", {defaults.style().bookmarkColor(), themed.style().bookmarkColor()}},
        {"diffAdded", {defaults.style().diffAdded(), themed.style().diffAdded()}},
        {"diffRemoved", {defaults.style().diffRemoved(), themed.style().diffRemoved()}},
        {"diffChanged", {defaults.style().diffChanged(), themed.style().diffChanged()}},
        {"sidebarBackground",
         {defaults.ui().sidebarBackground(), themed.ui().sidebarBackground()}},
        {"sidebarFolderColor",
         {defaults.ui().sidebarFolderColor(), themed.ui().sidebarFolderColor()}},
        {"toolbarBackground", {defaults.ui().toolbarBackground(), themed.ui().toolbarBackground()}},
        {"toolbarIconColor", {defaults.ui().toolbarIconColor(), themed.ui().toolbarIconColor()}},
        {"toolbarIconHover",
         {defaults.ui().toolbarIconHoverColor(), themed.ui().toolbarIconHoverColor()}},
        {"toolbarIconOn", {defaults.ui().toolbarIconOnColor(), themed.ui().toolbarIconOnColor()}},
        {"toolbarIconMark",
         {defaults.ui().toolbarIconMarkColor(), themed.ui().toolbarIconMarkColor()}},
        {"toolbarIconOff",
         {defaults.ui().toolbarIconDisabledColor(), themed.ui().toolbarIconDisabledColor()}},
        {"toolbarHover",
         {defaults.ui().toolbarHoverBackground(), themed.ui().toolbarHoverBackground()}},
        {"toolbarSeparator",
         {defaults.ui().toolbarSeparatorColor(), themed.ui().toolbarSeparatorColor()}},
        {"statusBackground", {defaults.ui().statusBackground(), themed.ui().statusBackground()}},
        {"statusTextColor", {defaults.ui().statusTextColor(), themed.ui().statusTextColor()}},
        {"statusSeparator",
         {defaults.ui().statusSeparatorColor(), themed.ui().statusSeparatorColor()}},
        {"statusSuspect", {defaults.ui().statusSuspectColor(), themed.ui().statusSuspectColor()}},
        {"findBadPattern",
         {defaults.ui().findBadPatternColor(), themed.ui().findBadPatternColor()}},
        {"noteListSnippet",
         {defaults.ui().noteListSnippetColor(), themed.ui().noteListSnippetColor()}},
        {"noteListDate", {defaults.ui().noteListDateColor(), themed.ui().noteListDateColor()}},
        {"tableBorder", {defaults.tables().borderColor(), themed.tables().borderColor()}},
        {"tableHeader", {defaults.tables().headerColor(), themed.tables().headerColor()}},
        {"tableFill", {defaults.tables().tableColor(), themed.tables().tableColor()}},
        {"tableAlt", {defaults.tables().altTableColor(), themed.tables().altTableColor()}},
        {"viewerBackground",
         {defaults.imageViewer().background(), themed.imageViewer().background()}},
        {"viewerCaption",
         {defaults.imageViewer().captionColor(), themed.imageViewer().captionColor()}},
        {"markdown.accent",
         {defaults.markdownHighlighting().accent(), themed.markdownHighlighting().accent()}},
        {"markdown.comment",
         {defaults.markdownHighlighting().comment(), themed.markdownHighlighting().comment()}},
        {"markdown.image",
         {defaults.markdownHighlighting().image(), themed.markdownHighlighting().image()}},
        {"markdown.link",
         {defaults.markdownHighlighting().link(), themed.markdownHighlighting().link()}},
        {"markdown.code",
         {defaults.markdownHighlighting().codeBackground(),
          themed.markdownHighlighting().codeBackground()}},
        {"json.key", {defaults.jsonEditing().key(), themed.jsonEditing().key()}},
        {"json.string", {defaults.jsonEditing().string(), themed.jsonEditing().string()}},
        {"json.number", {defaults.jsonEditing().number(), themed.jsonEditing().number()}},
        {"json.keyword", {defaults.jsonEditing().keyword(), themed.jsonEditing().keyword()}},
        {"json.comment", {defaults.jsonEditing().comment(), themed.jsonEditing().comment()}},
        {"json.punctuation",
         {defaults.jsonEditing().punctuation(), themed.jsonEditing().punctuation()}},
    };
    for (const auto& [what, colours] : pairs)
        ZT_EQ(std::string("светлая тема = умолчание: ") + what, hex(colours.first),
              hex(colours.second));

    // И ни одна роль не осталась без цвета: пустая роль — это поле, которое
    // тема молча не покрасит.
    for (const QString& role : zametti::ZTheme::roleNames())
        ZT_TRUE("роль имеет цвет: " + s(role), light.color(role).isValid());
}

// --- 2. НАСЛЕДОВАНИЕ ---------------------------------------------------------
void checkExtends() {
    writeTheme(QStringLiteral("bumaga"), QStringLiteral(R"({
  "extends": "light",
  "background": "#f5efdc",
  "markdown": { "comment": "#a08040" }
})"));

    zametti::ZTheme theme;
    QString error;
    ZT_TRUE("своя тема разрешилась: " + s(error),
            zametti::ZTheme::resolve(QStringLiteral("bumaga"), &theme, &error));
    ZT_EQ("своя роль взята", std::string("#f5efdc"),
          hex(theme.color(QStringLiteral("background"))));
    ZT_EQ("вложенная своя роль тоже", std::string("#a08040"),
          hex(theme.color(QStringLiteral("markdown.comment"))));
    ZT_EQ("а незаданная осталась светлой", std::string("#325cc0"),
          hex(theme.color(QStringLiteral("link"))));

    // Цепочка: своя поверх своей поверх встроенной.
    writeTheme(QStringLiteral("bumaga-plus"),
               QStringLiteral("{ \"extends\": \"bumaga\", \"link\": \"#804000\" }"));
    ZT_TRUE("цепочка разрешилась",
            zametti::ZTheme::resolve(QStringLiteral("bumaga-plus"), &theme, &error));
    ZT_EQ("верхняя тема перебила", std::string("#804000"),
          hex(theme.color(QStringLiteral("link"))));
    ZT_EQ("средняя осталась в силе", std::string("#f5efdc"),
          hex(theme.color(QStringLiteral("background"))));
}

// --- 3. ОШИБКИ ---------------------------------------------------------------
void checkErrorsAreLoud() {
    zametti::ZTheme theme;
    QString error;
    ZT_TRUE("неизвестная тема — не молчание",
            !zametti::ZTheme::resolve(QStringLiteral("нетакой"), &theme, &error));
    ZT_TRUE("и причина названа: " + s(error), !error.isEmpty());

    // Цикл: две темы ссылаются друг на друга.
    writeTheme(QStringLiteral("krug1"), QStringLiteral("{ \"extends\": \"krug2\" }"));
    writeTheme(QStringLiteral("krug2"), QStringLiteral("{ \"extends\": \"krug1\" }"));
    error.clear();
    ZT_TRUE("цикл наследования — не зависание и не молчание",
            !zametti::ZTheme::resolve(QStringLiteral("krug1"), &theme, &error));
    ZT_TRUE("и про него сказано: " + s(error), error.contains(QStringLiteral("loop")));

    // ЧЕРЕЗ ЗАГРУЗКУ КОНФИГА: неизвестная тема не даёт применить конфиг вовсе,
    // и прежние настройки остаются — как с битым JSON.
    writeConfig(QStringLiteral("{ \"fonts\": { \"noteSize\": 13 } }"));
    QString why;
    ZT_TRUE("рабочий конфиг прочитан", zametti::loadSettings(&why));
    ZT_EQ("и применён", std::to_string(13.0),
          std::to_string(zametti::settings().style().baseFontPoint()));

    writeConfig(QStringLiteral("{ \"theme\": { \"extends\": \"нетакой\" },"
                               "  \"fonts\": { \"noteSize\": 17 } }"));
    why.clear();
    ZT_TRUE("конфиг с несуществующей темой не принят", !zametti::loadSettings(&why));
    ZT_TRUE("и причина названа: " + s(why), !why.isEmpty());
    ZT_EQ("прежний кегль остался", std::to_string(13.0),
          std::to_string(zametti::settings().style().baseFontPoint()));
}

// --- 4. ТЕМА ИЗ КОНФИГА И ОПЕЧАТКИ В РОЛЯХ -----------------------------------
void checkThemeFromConfig() {
    writeConfig(QStringLiteral(R"({
  "theme": {
    "accent": "#00a0a0",
    "added": "#101010",
    "bacground": "#000000",
    "diff": { "removed": "#654321" },
    "markdown": { "marker": "#207020", "markr": "#000000" }
  }
})"));
    QString error;
    QStringList unknown;
    ZT_TRUE("конфиг с темой прочитан: " + s(error), zametti::loadSettings(&error, &unknown));
    ZT_EQ("акцент из конфига применён ко всем своим местам",
          std::string("#00a0a0 #00a0a0"),
          hex(zametti::settings().style().caretColor()) + " " +
              hex(zametti::settings().style().checkboxCheckedColor()));
    ZT_EQ("вложенная роль тоже", std::string("#207020"),
          hex(zametti::settings().markdownHighlighting().accent()));
    // Цвета разности живут группой "diff" (решение владельца 04.09.2026):
    // вложенное имя применяется, а СТАРОЕ ПЛОСКОЕ ("added") — неизвестное, и
    // называется вслух, а не читается молча по старой памяти.
    ZT_EQ("роль из группы diff применена", std::string("#654321"),
          hex(zametti::settings().style().diffRemoved()));
    ZT_EQ("опечатки и старый плоский ключ названы поимённо",
          std::string("theme.added, theme.bacground, theme.markdown.markr"),
          s(unknown.join(QStringLiteral(", "))));

    // Конфиг без темы — светлая: цвета возвращаются к умолчанию.
    writeConfig(QStringLiteral("{}"));
    ZT_TRUE("пустой конфиг прочитан", zametti::loadSettings(&error));
    ZT_EQ("каретка вернулась к светлой теме",
          hex(zametti::ZSettings{}.style().caretColor()),
          hex(zametti::settings().style().caretColor()));
}

}  // namespace

static int ztRunSuite(int argc, char** argv) {
    (void)argc;
    (void)argv;
    // Свой каталог настроек: набор пишет настоящие конфиги и файлы тем, и
    // оставлять их соседям нельзя — наборы идут одним процессом.
    QTemporaryDir home;
    const QByteArray previous = qgetenv(zametti::kConfigDirVar);
    qputenv(zametti::kConfigDirVar, home.path().toLocal8Bit());
    const zametti::ZSettings before = zametti::settings();

    checkLightIsTodaysLook();
    checkExtends();
    checkErrorsAreLoud();
    checkThemeFromConfig();

    zametti::mutableSettingsForTests() = before;
    qputenv(zametti::kConfigDirVar, previous);
    return zt::report("theme");
}

TEST(Theme, All) {
    std::vector<QByteArray> ztArgs{QByteArrayLiteral("theme_test")};
    std::vector<char*> ztArgv;
    for (QByteArray& a : ztArgs) ztArgv.push_back(a.data());
    EXPECT_EQ(0, ztRunSuite(int(ztArgv.size()), ztArgv.data()));
}
