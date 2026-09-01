#include "settings.h"

#include "doc_model.h"
#include "theme.h"

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
#include <QSet>
#include <QStandardPaths>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

namespace zametti {
namespace {

ZSettings g_settings;

// Значение из объекта, если оно там есть и нужного вида. Чужие и битые ключи
// молча пропускаем: конфиг правят руками, и опечатка в одном параметре не
// должна ронять остальные.
// ЧТЕНИЕ ЧЕРЕЗ СЕТТЕР, А НЕ В ПОЛЕ. Сеттер обрезает число до допустимого
// диапазона и отвечает, приняла ли настройка значение как есть; нет — пишем в
// лог: настройки пользователя — пожелания, робастность выше них (решение
// владельца), а молчать о поправленном нельзя — человек должен видеть, что его
// число не взяли.
void complainClamped(const char* section, const char* key, const QJsonValue& v) {
    std::fprintf(stderr,
                 "setting «%s.%s» = %g outside the allowed range — clamped to the edge\n",
                 section, key, v.toDouble());
}

// ОДНА ТАБЛИЦА НА ВЕСЬ ПУБЛИЧНЫЙ КОНФИГ.
//
// Здесь стояли две рукописные функции — settingsToJson и settingsFromJson, —
// и они повторяли друг друга ключ в ключ: двести строк «записать» и двести
// строк «прочитать». Разойтись им ничего не мешало, и они расходились: ключ,
// который писали, но не читали, выглядел как работающая настройка ровно до
// первой попытки ею воспользоваться.
//
// Теперь у ключа одна строка, и из неё выводится ВСЁ: --dump-config, шаблон
// конфига (с подписью у каждого ключа — он же и есть меню для человека),
// словарь известных имён и общая проверка «каждый ключ переживает круг JSON».
//
// ЧЕГО ЗДЕСЬ НЕТ — того нет и в конфиге. Внутренние коэффициенты отрисовки
// (посадка буллета, скругление плашки кода, углы рамки выделенной картинки,
// потолки поиска, бюджеты кэшей) остались полями со своими умолчаниями в
// settings.h, но наружу не выходят: решение владельца — «убрать из JSON
// совсем». Публичной настройке полагается своя предметная секция; если её
// нет, сперва надо решить, должна ли настройка быть публичной вообще.

using Key = ZSettings::Key;

// Чтение идёт ЧЕРЕЗ СЕТТЕР, а он обрезает число до допустимого диапазона и
// отвечает, принял ли значение как есть: настройки пользователя — пожелания,
// робастность выше них (решение владельца). Ложь из set() значит «обрезано», и
// загрузчик про это скажет. Значение не того вида молча пропускается: конфиг
// правят руками, и опечатка в одном ключе не должна ронять остальные.
#define ZM_KEY_REAL(section, name, note, part, lower, Upper)                            \
    Key {                                                                               \
        section, name, note, [](const ZSettings& a) { return QJsonValue(a.part.lower()); }, \
            [](ZSettings& a, const QJsonValue& v) {                                     \
                return !v.isDouble() || a.part.set##Upper(v.toDouble());                \
            }                                                                           \
    }

#define ZM_KEY_INT(section, name, note, part, lower, Upper)                             \
    Key {                                                                               \
        section, name, note, [](const ZSettings& a) { return QJsonValue(a.part.lower()); }, \
            [](ZSettings& a, const QJsonValue& v) {                                     \
                return !v.isDouble() || a.part.set##Upper(v.toInt());                   \
            }                                                                           \
    }

#define ZM_KEY_BOOL(section, name, note, part, lower, Upper)                            \
    Key {                                                                               \
        section, name, note, [](const ZSettings& a) { return QJsonValue(a.part.lower()); }, \
            [](ZSettings& a, const QJsonValue& v) {                                     \
                if (v.isBool()) a.part.set##Upper(v.toBool());                          \
                return true;                                                            \
            }                                                                           \
    }

#define ZM_KEY_STR(section, name, note, part, lower, Upper)                             \
    Key {                                                                               \
        section, name, note, [](const ZSettings& a) { return QJsonValue(a.part.lower()); }, \
            [](ZSettings& a, const QJsonValue& v) {                                     \
                if (v.isString()) a.part.set##Upper(v.toString());                      \
                return true;                                                            \
            }                                                                           \
    }

// Ступени заголовков: шесть чисел по уровням. Короче списка — остальные
// уровни остаются какими были; длиннее — лишнее отбрасывается.
QJsonValue headingsToJson(const HeadingSteps& steps) {
    QJsonArray out;
    for (int v : steps) out.append(v);
    return out;
}

HeadingSteps headingsFromJson(const QJsonValue& v, HeadingSteps steps) {
    const QJsonArray given = v.toArray();
    for (int i = 0; i < given.size() && i < int(steps.size()); ++i)
        if (given.at(i).isDouble()) steps[size_t(i)] = given.at(i).toInt();
    return steps;
}

const std::vector<Key>& keys() {
    static const std::vector<Key> table = {
        // --- ШРИФТЫ -------------------------------------------------------
        // Кегли — в пунктах, ступени — в делениях лестницы Qt (−2…+4).
        ZM_KEY_STR("fonts", "noteFamily", "main font of a note; may be proportional",
                   style(), fontFamily, FontFamily),
        ZM_KEY_REAL("fonts", "noteSize", "size of the note font, in points", style(),
                    baseFontPoint, BaseFontPoint),
        ZM_KEY_STR("fonts", "monospaceFamily", "code, source mode, history and this editor",
                   style(), codeFamily, CodeFamily),
        ZM_KEY_REAL("fonts", "monospaceSize",
                    "size of the monospace font in the flat views, in points", style(),
                    monospacePoint, MonospacePoint),
        ZM_KEY_INT("fonts", "codeStep",
                   "size of code INSIDE a note, as a step from the note font (-2..+4)",
                   style(), codeStep, CodeStep),
        ZM_KEY_STR("fonts", "appFamily", "font of the shell: tree, list, panels, dialogs",
                   ui(), appFamily, AppFamily),
        ZM_KEY_REAL("fonts", "appSize",
                    "size of the shell font; toolbar icons and paddings follow it", ui(),
                    appPoint, AppPoint),
        Key{"fonts", "headingSteps", "size of headings 1..6, as steps from the note font",
            [](const ZSettings& a) { return headingsToJson(a.style().headingStep()); },
            [](ZSettings& a, const QJsonValue& v) {
                a.style().setHeadingStep(headingsFromJson(v, a.style().headingStep()));
                return true;
            }},
        ZM_KEY_REAL("fonts", "mathScale", "formula ink against text ink", formulas(),
                    mathScale, MathScale),

        // --- РИТМ СТРАНИЦЫ ------------------------------------------------
        ZM_KEY_REAL("layout", "maxContentWidth", "column width, in widths of 'A'", style(),
                    maxContentWidth, MaxContentWidth),
        ZM_KEY_REAL("layout", "lineHeightFactor", "line height, as a factor of the font height",
                    style(), lineHeightFactor, LineHeightFactor),
        ZM_KEY_REAL("layout", "blockSpacing", "air between blocks, in line heights", style(),
                    blockSpacing, BlockSpacing),
        ZM_KEY_REAL("layout", "listIndent", "indent of a list level, in widths of 'A'", style(),
                    listIndent, ListIndent),
        ZM_KEY_REAL("layout", "quoteIndent", "indent of a quote, in widths of 'A'", style(),
                    quoteIndent, QuoteIndent),

        // --- ПРАВКА --------------------------------------------------------
        ZM_KEY_INT("editor", "autosaveDelayMs", "pause in typing after which the note is written",
                   editor(), autosaveDelayMs, AutosaveDelayMs),
        ZM_KEY_INT("editor", "tabWidth", "tab stop in code, source mode and this editor, in spaces",
                   editor(), tabWidth, TabWidth),
        ZM_KEY_STR("editor", "externalEditor", "command to open a note elsewhere; %f is the path",
                   editor(), externalEditor, ExternalEditor),
        ZM_KEY_INT("editor", "historyMergeChars",
                   "an edit smaller than this replaces the previous history entry", history(),
                   historyMergeChars, HistoryMergeChars),
        ZM_KEY_INT("editor", "historyMergeHours", "...and only if that entry is younger than this",
                   history(), historyMergeHours, HistoryMergeHours),
        // Автозамены: список пар [сочетание, что вставить]. Заданный список
        // заменяет умолчания целиком — иначе от умолчания было бы не избавиться.
        Key{"editor", "special", "chords that insert a character: [[\"Alt+-\", \"---\"]]",
            [](const ZSettings& a) {
                QJsonArray out;
                for (const auto& pair : a.editor().specialKeys())
                    out.append(QJsonArray{pair.first, pair.second});
                return QJsonValue(out);
            },
            [](ZSettings& a, const QJsonValue& v) {
                if (!v.isArray()) return true;
                ZSettings::KeyPairs pairs;
                for (const QJsonValue& entry : v.toArray()) {
                    const QJsonArray pair = entry.toArray();
                    if (pair.size() != 2 || !pair.at(0).isString() || !pair.at(1).isString())
                        continue;   // битую запись пропускаем, соседние живут
                    if (pair.at(0).toString().isEmpty()) continue;
                    pairs.push_back({pair.at(0).toString(), pair.at(1).toString()});
                }
                a.editor().setSpecialKeys(std::move(pairs));
                return true;
            }},

        // --- СОЧЕТАНИЯ КЛАВИШ ----------------------------------------------
        // Строкой, как их пишет QKeySequence; несколько — через точку с
        // запятой; пустая строка убирает сочетание совсем. Суффикс «Key» у имён
        // не нужен: секция уже называется shortcuts.
        ZM_KEY_STR("shortcuts", "fullscreen", "hide everything but the text", editor(),
                   fullscreenKey, FullscreenKey),
        ZM_KEY_STR("shortcuts", "makeBullet", "turn blocks into a bullet list", editor(),
                   makeBulletKey, MakeBulletKey),
        ZM_KEY_STR("shortcuts", "makeOrdered", "turn blocks into a numbered list", editor(),
                   makeOrderedKey, MakeOrderedKey),
        ZM_KEY_STR("shortcuts", "makeTask", "turn blocks into a task list", editor(),
                   makeTaskKey, MakeTaskKey),
        ZM_KEY_STR("shortcuts", "makeParagraph", "turn blocks back into plain paragraphs",
                   editor(), makeParagraphKey, MakeParagraphKey),
        ZM_KEY_STR("shortcuts", "makeComment", "wrap blocks into an HTML comment", editor(),
                   makeCommentKey, MakeCommentKey),
        ZM_KEY_STR("shortcuts", "toggleTask", "check or uncheck a task", editor(), toggleTaskKey,
                   ToggleTaskKey),
        ZM_KEY_STR("shortcuts", "moveUp", "move the current item up", editor(), moveUpKey,
                   MoveUpKey),
        ZM_KEY_STR("shortcuts", "moveDown", "move the current item down", editor(), moveDownKey,
                   MoveDownKey),
        ZM_KEY_STR("shortcuts", "markdownMode", "show the note as raw markdown; empty by default",
                   editor(), markdownModeKey, MarkdownModeKey),
        ZM_KEY_STR("shortcuts", "diffNext", "next change in the history mode", editor(),
                   diffNextKey, DiffNextKey),
        ZM_KEY_STR("shortcuts", "diffPrevious", "previous change in the history mode", editor(),
                   diffPreviousKey, DiffPreviousKey),
        ZM_KEY_STR("shortcuts", "jsonComment", "comment out lines in this editor", jsonEditing(),
                   commentKey, CommentKey),

        // --- КАРТИНКИ -------------------------------------------------------
        ZM_KEY_INT("images", "maxImportedSize",
                   "imported pictures are shrunk to this side, in pixels", images(),
                   maxImportedImageSize, MaxImportedImageSize),
        ZM_KEY_INT("images", "maxDeletedSize",
                   "a picture kept in the trash is shrunk to this side, in pixels", images(),
                   maxDeletedImageSize, MaxDeletedImageSize),
        ZM_KEY_BOOL("images", "captions", "show the alt-text under a picture", style(),
                    imageCaption, ImageCaption),
        // Битый регэксп настройку не меняет: молча спрятать все подписи (или ни
        // одной) из-за опечатки в конфиге нельзя.
        Key{"images", "nonamePattern", "alt-texts matching this are NOT shown (camera names)",
            [](const ZSettings& a) {
                return QJsonValue(a.style().imageNonameCaption().pattern());
            },
            [](ZSettings& a, const QJsonValue& v) {
                if (!v.isString()) return true;
                const QRegularExpression candidate(v.toString(),
                                                   QRegularExpression::CaseInsensitiveOption);
                if (candidate.isValid()) a.style().setImageNonameCaption(candidate);
                return true;
            }},
        ZM_KEY_INT("images", "viewerMaxZoomPercent",
                   "how much the full-screen viewer may enlarge a small picture", imageViewer(),
                   maxZoomPercent, MaxZoomPercent),

        // --- ХРАНИЛИЩЕ ------------------------------------------------------
        ZM_KEY_STR("store", "root", "store to open when none is given; relative to $HOME",
                   store(), notesRoot, NotesRoot),
        ZM_KEY_STR("store", "title", "label of the root row; empty means 'All notes'", store(),
                   storeTitle, StoreTitle),
        ZM_KEY_BOOL("store", "watchFolder", "re-read the store when files change behind our back",
                    store(), watchStore, WatchStore),

        // --- ОБЛАКО ---------------------------------------------------------
        ZM_KEY_BOOL("sync", "onStart", "pull other devices' edits when starting", sync(), onStart,
                    OnStart),
        ZM_KEY_BOOL("sync", "onExit", "push our own edits when quitting", sync(), onExit, OnExit),

        // --- БУМАГА ---------------------------------------------------------
        // У бумаги своя типографика: экранный масштаб на неё не влияет вовсе.
        ZM_KEY_STR("pdf", "fontFamily", "main font on paper", pdf(), fontFamily, FontFamily),
        ZM_KEY_REAL("pdf", "fontSize", "size of the main font on paper, in points", pdf(),
                    pointSize, PointSize),
        ZM_KEY_STR("pdf", "monospaceFamily", "code font on paper", pdf(), codeFamily, CodeFamily),
        ZM_KEY_INT("pdf", "codeStep", "size of code on paper, as a step from the main font",
                   pdf(), codeStep, CodeStep),
        Key{"pdf", "headingSteps", "size of headings 1..6 on paper",
            [](const ZSettings& a) { return headingsToJson(a.pdf().headingStep()); },
            [](ZSettings& a, const QJsonValue& v) {
                a.pdf().setHeadingStep(headingsFromJson(v, a.pdf().headingStep()));
                return true;
            }},
        ZM_KEY_STR("pdf", "pageSize", "A4, Letter, A5...", pdf(), pageSize, PageSize),
        ZM_KEY_REAL("pdf", "marginMm", "page margins, in millimetres", pdf(), marginMm, MarginMm),
        ZM_KEY_INT("pdf", "imageDpi", "resolution of pictures on paper", pdf(), imageDpi,
                   ImageDpi),
        ZM_KEY_INT("pdf", "maxImageSize", "pictures are shrunk to this side before export", pdf(),
                   maxExportedImageSize, MaxExportedImageSize),

        // --- ЖУРНАЛЫ ---------------------------------------------------------
        ZM_KEY_BOOL("logs", "writeErrLog", "write err.log next to this file", logs(), writeErrLog,
                    WriteErrLog),
        ZM_KEY_BOOL("logs", "writeSyncLog", "write sync.log next to this file", logs(),
                    writeSyncLog, WriteSyncLog),
        ZM_KEY_INT("logs", "logSizeMb", "cap on the size of each log, in megabytes", logs(),
                   logSizeMb, LogSizeMb),
    };
    return table;
}

#undef ZM_KEY_REAL
#undef ZM_KEY_INT
#undef ZM_KEY_BOOL
#undef ZM_KEY_STR

QJsonObject settingsToJson(const ZSettings& a) {
    QJsonObject root;
    for (const Key& key : ZSettings::registry()) {
        const QString section = QString::fromLatin1(key.section);
        QJsonObject inner = root.value(section).toObject();
        inner.insert(QString::fromLatin1(key.name), key.get(a));
        root.insert(section, inner);
    }
    return root;
}

void settingsFromJson(const QJsonObject& root, ZSettings& a) {
    for (const Key& key : ZSettings::registry()) {
        const QJsonValue section = root.value(QString::fromLatin1(key.section));
        if (!section.isObject()) continue;
        const QJsonValue value = section.toObject().value(QString::fromLatin1(key.name));
        if (value.isUndefined()) continue;
        if (!key.set(a, value)) complainClamped(key.section, key.name, value);
    }
}

}  // namespace

const std::vector<ZSettings::Key>& ZSettings::registry() { return keys(); }

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

namespace {

// Значение ключа так, как его пишут в JSON, одной строкой. Строим через
// QJsonDocument, а не руками: экранирование кавычек и косых внутри строк
// (регэксп безымянной подписи — сплошные косые) руками не пишут.
QByteArray jsonValueText(const QJsonValue& value) {
    const QByteArray wrapped = QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact);
    // «[значение]» → «значение».
    return wrapped.mid(1, wrapped.size() - 2).trimmed();
}

}  // namespace

QByteArray configTemplate() {
    // ВСЁ ТЕЛО — КОММЕНТАРИЕМ, снаружи пустой объект. Так файл и остаётся
    // действующим (отклонений нет), и служит меню: раскомментировал строку —
    // получил отклонение.
    //
    // У каждого ключа своя подпись из реестра. Прежде шаблон был просто
    // закомментированным дампом умолчаний: двести имён без единого слова о
    // том, что они значат, — и «в чём тут разобраться» было честным вопросом.
    QByteArray out =
        "// zametti configuration.\n"
        "//\n"
        "// Everything the program lets you change is listed here with its default\n"
        "// value, and everything is commented out: the effective config is the list\n"
        "// of DEVIATIONS from the defaults, not a copy of them. Uncomment a line\n"
        "// (remove the leading \"//\") to make that value yours.\n"
        "//\n"
        "// Only \"//\" comments to the end of line are understood, and they are not\n"
        "// stripped inside quotes, so \"https://\" is fine. A trailing comma before\n"
        "// a closing brace is forgiven too.\n"
        "//\n"
        "// Colors are not here: they belong to the theme (see the \"theme\" section).\n"
        "// Zoom levels are not here either: they are per-machine and live in\n"
        "// state.json next to this file, changed with Ctrl+= and Ctrl+Alt+=.\n"
        "{\n";

    QString section;
    for (const ZSettings::Key& key : ZSettings::registry()) {
        const QString mine = QString::fromLatin1(key.section);
        if (mine != section) {
            if (!section.isEmpty()) out += "//     },\n//\n";
            section = mine;
            out += "//     \"" + section.toUtf8() + "\": {\n";
        }
        out += "//         \"" + QByteArray(key.name) + "\": " +
               jsonValueText(key.get(ZSettings{})) + ",";
        if (key.note != nullptr && *key.note != '\0')
            out += "   // " + QByteArray(key.note);
        out += "\n";
    }
    if (!section.isEmpty()) out += "//     },\n//\n";

    // ТЕМА — В ТОМ ЖЕ ФАЙЛЕ И ТЕМ ЖЕ СПОСОБОМ. Ролей четыре десятка, и каждая
    // здесь названа со своим цветом светлой темы: иначе про них знал бы только
    // тот, кто читал исходники. Своя тема — либо переопределения прямо здесь,
    // либо файл themes/<имя>.json и "extends" на него.
    out +=
        "//     \"theme\": {\n"
        "//         \"extends\": \"light\",   // a built-in theme, or themes/<name>.json "
        "next to this file\n";
    QString group;
    for (const ZTheme::RoleInfo& role : ZTheme::roles()) {
        const int dot = int(role.name.indexOf(QLatin1Char('.')));
        const QString mine = dot < 0 ? QString() : role.name.left(dot);
        if (mine != group) {
            if (!group.isEmpty()) out += "//         },\n";
            group = mine;
            if (!group.isEmpty()) out += "//         \"" + group.toUtf8() + "\": {\n";
        }
        const QByteArray pad = group.isEmpty() ? "//         " : "//             ";
        const QString leaf = dot < 0 ? role.name : role.name.mid(dot + 1);
        const QColor value = role.light;
        const QString text = value.alpha() == 255 ? value.name(QColor::HexRgb)
                                                  : value.name(QColor::HexArgb);
        out += pad + "\"" + leaf.toUtf8() + "\": \"" + text.toUtf8() + "\",   // " +
               role.note.toUtf8() + "\n";
    }
    if (!group.isEmpty()) out += "//         },\n";
    out += "//     },\n";

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

QString configDir() {
    // Проверяем на пустоту, а не на «задана ли»: пустая переменная — это не
    // каталог, и молча писать в корень мы не станем.
    const QByteArray override = qgetenv(kConfigDirVar);
    if (!override.isEmpty()) return QString::fromLocal8Bit(override);
    return QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
}

QString configPath() { return configDir() + QStringLiteral("/config.json"); }


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
// способа у Qt нет, поэтому спрашиваем систему напрямую; где спросить нечем —
// не знаем и не гадаем.
int totalMemoryMb() {
#if defined(Q_OS_LINUX)
    QFile meminfo(QStringLiteral("/proc/meminfo"));
    if (!meminfo.open(QIODevice::ReadOnly)) return 0;
    const QByteArray text = meminfo.readAll();
    const int at = text.indexOf("MemTotal:");
    if (at < 0) return 0;
    return text.mid(at + 9, 32).trimmed().split(' ').first().toInt() / 1024;
#elif defined(Q_OS_WIN)
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status) == 0) return 0;
    return int(status.ullTotalPhys / (1024 * 1024));
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
    // Словарь — РЕЕСТР, а не дамп умолчаний: имена ключей и так лежат в нём по
    // одному разу, и разбирать их обратно из напечатанного JSON было бы кругом
    // через свой же вывод.
    QSet<QString> sections;
    QSet<QString> names;
    for (const ZSettings::Key& key : ZSettings::registry()) {
        const QString section = QString::fromLatin1(key.section);
        sections.insert(section);
        names.insert(section + QLatin1Char('.') + QString::fromLatin1(key.name));
    }

    QStringList out;
    for (auto section = root.begin(); section != root.end(); ++section) {
        // Тема разбирается своими правилами (ролей десятки, и они вложенные) —
        // её ключи проверяет сама тема, а не эта сверка.
        if (section.key() == QLatin1String("theme")) continue;
        if (!sections.contains(section.key())) {
            out << section.key();
            continue;
        }
        if (!section.value().isObject()) continue;
        const QJsonObject mine = section.value().toObject();
        for (auto item = mine.begin(); item != mine.end(); ++item)
            if (!names.contains(section.key() + QLatin1Char('.') + item.key()))
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
        ZSettings fresh;
        // ТЕМА — ПЕРВОЙ, остальные ключи поверх. Цвета в конфиге больше не
        // лежат россыпью: их задаёт тема (theme.h), а секция "theme" — это имя
        // основания плюс переопределения ролей.
        //
        // Неизвестное имя темы — ОШИБКА ЗАГРУЗКИ, как битый JSON: прежние
        // настройки остаются, причина уходит наверх. Молча показать не ту тему,
        // которую попросили, хуже, чем сказать вслух.
        const QJsonObject themeSection =
            doc.object().value(QStringLiteral("theme")).toObject();
        const QString base = themeSection.value(QStringLiteral("extends")).toString(
            QStringLiteral("light"));
        ZTheme theme;
        QString themeError;
        if (!ZTheme::resolve(base, &theme, &themeError)) {
            if (error != nullptr) *error = themeError;
            return false;
        }
        theme.applyOverrides(themeSection, unknown);
        theme.applyTo(fresh);
        settingsFromJson(doc.object(), fresh);
        g_settings = fresh;
    }
    applyImageAllocationLimit();
    return true;
}

}  // namespace zametti
