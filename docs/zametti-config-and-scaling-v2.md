# Zametti: конфигурация, темы и масштабирование — версия 2

## Цели

Новая система конфигурации Zametti должна быть одновременно:

- существенно проще текущей;
- достаточно гибкой для реальных пользовательских предпочтений;
- пригодной для светлых, тёмных и пользовательских тем;
- стабильной с точки зрения публичного config API;
- независимой от внутренних параметров painter/layout implementation;
- предсказуемой при High-DPI scaling на Linux и macOS.

Основная идея:

> Пользовательский config описывает то, что пользователь действительно хочет менять. Theme описывает семантические цвета интерфейса. `state.json` хранит интерактивное состояние. Внутренние оптические коэффициенты остаются в коде.

---

# 1. Итоговое разделение данных

Предлагается использовать три пользовательски значимых уровня и один внутренний.

## `config.jsonc`

Постоянные пользовательские настройки:

```text
theme
fonts
layout
editor
shortcuts
images
pdf
logs
```

## `state.json`

Интерактивное состояние конкретной установки:

- zoom levels;
- размеры sidebar/note list;
- geometry/state окна;
- splitter positions;
- выбранное хранилище и другие runtime-state данные.

## `themes/<name>.json`

Пользовательские темы.

Пример пути:

```text
./config/zametti/themes/my-dark.json
```

Также есть набор встроенных тем, например:

```text
light
dark
```

На встроенные и пользовательские темы можно ссылаться через `extends`.

## Internal style constants

Остаются в исходном коде и не являются публичной конфигурацией:

```text
bulletRise
glyphScale
cornerOffset
caretWidth
codeCopyIconScale
toolbar button padding
checkbox optical offsets
image-selection corner geometry
...
```

---

# 2. Предлагаемая верхнеуровневая структура config

```jsonc
{
    "theme": {
        "extends": "light",
        "accent": "#cc8822"
    },

    "fonts": {
        "noteFamily": "IBM Plex Serif",
        "noteSize": 11,

        "monospaceFamily": "IBM Plex Mono",
        "monospaceSize": 10.5,

        "appFamily": "IBM Plex Sans SemiCondensed",
        "appSize": 12,

        "headingSteps": [3, 2, 1, 0, -1, -1],
        "mathScale": 1.1
    },

    "layout": {
        "maxContentWidth": 90,
        "lineHeightFactor": 1.15,
        "blockSpacing": 0.667
    },

    "editor": {
        "autosaveDelayMs": 60000,
        "codeTabWidth": 4,
        "externalEditor": "",

        "historyMergeChars": 100,
        "historyMergeHours": 24,

        "special": [
            ["Alt+-", "—"]
        ]
    },

    "shortcuts": {
        "fullscreen": "F11; Ctrl+Meta+F",

        "makeBullet": "Ctrl+8; Ctrl+Shift+8",
        "makeOrdered": "Ctrl+7; Ctrl+Shift+7",
        "makeTask": "Ctrl+9; Ctrl+Shift+9",
        "makeParagraph": "Ctrl+Shift+0",
        "makeComment": "Ctrl+Shift+C",
        "toggleTask": "Ctrl+D; Ctrl+Space",

        "moveUp": "Ctrl+Up",
        "moveDown": "Ctrl+Down",

        "markdownMode": "",

        "diffNext": "F4; Ctrl+]",
        "diffPrevious": "Shift+F4; Ctrl+[",

        "jsonComment": "Ctrl+/"
    },

    "images": {
        "maxImportedSize": 2160,
        "maxDeletedSize": 100,

        "captions": true,
        "nonamePattern": "...",

        "viewerMaxZoomPercent": 300
    },

    "pdf": {
        "fontFamily": "IBM Plex Sans",
        "fontSize": 11,

        "monospaceFamily": "IBM Plex Mono",
        "monospaceSize": 10.5,

        "headingSteps": [3, 2, 1, 0, -1, -1],

        "background": "#ffffff",
        "foreground": "#181818",
        "link": "#325cc0",
        "quote": "#5a626a",
        "codeBackground": "#f4f4f2",

        "pageSize": "A4",
        "marginMm": 15,

        "imageDpi": 200,
        "maxImageSize": 2000
    },

    "logs": {
        "logSizeMb": 10,
        "writeErrLog": false,
        "writeSyncLog": false
    }
}
```

Это не означает, что все эти параметры обязаны присутствовать в реальном файле.

Как и раньше, предпочтительна модель:

> config содержит только отклонения от defaults.

---

# 3. Theme

Theme описывает **экранный внешний вид Zametti**.

Она относится не только к заметке, но и к:

- sidebar;
- toolbar;
- status bar;
- note list;
- Markdown source mode;
- JSON editor;
- другим экранным режимам приложения.

PDF theme сюда не входит.

PDF является отдельным output format и настраивается в секции `pdf`.

---

# 4. Наследование тем

Простейшее использование:

```jsonc
"theme": {
    "extends": "light",
    "accent": "#cc8822"
}
```

В этом случае:

1. загружается встроенная тема `light`;
2. поверх неё накладываются значения из объекта `theme`.

Например:

```jsonc
"theme": {
    "extends": "light",

    "accent": "#cc8822",
    "selectionBackground": "#fae8a8",
    "selectionForeground": "#453d1f"
}
```

---

# 5. Полностью самостоятельная тема

`extends` не является обязательным.

Можно задать тему полностью с нуля:

```jsonc
{
    "background": "#181818",
    "foreground": "#e5e5e5",

    "sidebarBackground": "#181818",

    "panelBackground": "#202020",
    "panelForeground": "#dddddd",
    "panelSeparator": "#343434",

    "accent": "#cc8822",

    "selectionBackground": "#5f4820",
    "selectionForeground": "#ffffff",

    "link": "#6ca0ff",
    "quote": "#aaaaaa",
    "divider": "#444444",
    "codeBackground": "#242424",

    "markdown": {
        "marker": "#d49a3a",
        "heading": "#e0b55e",
        "comment": "#808080",
        "image": "#c98be8"
    },

    "json": {
        "comment": "#808080",
        "key": "#6ca0ff",
        "keyword": "#c98be8",
        "number": "#e39552",
        "punctuation": "#bbbbbb",
        "string": "#e28eb8"
    }
}
```

Для standalone theme лучше требовать наличие всех обязательных semantic colors.

```text
extends есть
    → partial theme разрешена

extends отсутствует
    → тема должна быть полной
```

---

# 6. Где искать пользовательские темы

Предлагаемое соглашение:

```text
./config/zametti/themes/<themename>.json
```

Например:

```text
./config/zametti/themes/paper.json
./config/zametti/themes/my-dark.json
```

Использование:

```jsonc
"theme": {
    "extends": "my-dark"
}
```

Алгоритм поиска имени:

1. встроенная тема;
2. пользовательская тема в `themes/`.

Можно разрешить цепочки:

```text
my-orange
    extends paper
        extends light
```

Правила:

- поздний override побеждает;
- циклическое наследование является ошибкой;
- неизвестная тема является ошибкой загрузки config/theme.

---

# 7. Semantic colors основной темы

Предлагаемый базовый набор:

```jsonc
{
    "background": "#fefefb",
    "foreground": "#1a1a1a",

    "sidebarBackground": "#fefefb",

    "panelBackground": "#f5f5f2",
    "panelForeground": "#1a1a1a",
    "panelSeparator": "#dde1e5",

    "accent": "#cc8822",

    "selectionBackground": "#fae8a8",
    "selectionForeground": "#453d1f",

    "link": "#325cc0",
    "quote": "#5a626a",
    "divider": "#c8cdd2",

    "codeBackground": "#0e000000"
}
```

### Семантика

- `background` — фон основной области заметки;
- `foreground` — основной текст, обычные bullets и ordered-list markers;
- `sidebarBackground` — фон sidebar/note-list area;
- `panelBackground` — toolbar/status bar и другие panel-like areas;
- `panelForeground` — обычный текст и обычные иконки панелей;
- `panelSeparator` — разделители панелей;
- `accent` — caret, выполненные задачи, checked checkbox, active toolbar state;
- `selectionBackground` / `selectionForeground` — выделение текста;
- `link` — ссылки;
- `quote` — quote-specific оформление;
- `divider` — document divider;
- `codeBackground` — фон code areas.

Обычные list bullets не используют `accent`.

---

# 8. Заголовки в обычном режиме

Обычный rendered mode не обязан иметь отдельный heading color.

По умолчанию:

```text
heading color = foreground
```

Главные признаки heading:

- size;
- weight;
- spacing.

Если позднее появится реальная потребность в отдельном цвете headings, его можно добавить как ещё одну semantic role.

---

# 9. Markdown source mode

Для Markdown source mode полезна отдельная компактная palette:

```jsonc
"markdown": {
    "marker": "#325cc0",
    "heading": "#325cc0",
    "comment": "#808080",
    "image": "#7a3e9d"
}
```

`markdown.marker` используется для structural markers:

```text
-
*
+
1.
>
[ ]
[x]
```

При желании туда же можно отнести другую Markdown punctuation.

`markdown.heading` отдельно задаёт цвет headings в raw source.

`markdown.comment` — comments.

`markdown.image` — image-related syntax.

Links могут использовать общий:

```text
theme.link
```

Code background на первом этапе также может использовать общий:

```text
theme.codeBackground
```

---

# 10. JSON highlighting

JSON syntax colors также являются частью theme:

```jsonc
"json": {
    "comment": "#808080",
    "key": "#325cc0",
    "keyword": "#7a3e9d",
    "number": "#b05a00",
    "punctuation": "#50565e",
    "string": "#802050"
}
```

Behavioural settings вроде tab width к theme не относятся.

---

# 11. Возможный будущий `reader`

Позднее можно естественно добавить:

```jsonc
"reader": {
    ...
}
```

для режима чтения книг.

Базовая архитектура theme при этом не меняется.

---

# 12. Fonts

Предлагаемая секция:

```jsonc
"fonts": {
    "noteFamily": "IBM Plex Serif",
    "noteSize": 11,

    "monospaceFamily": "IBM Plex Mono",
    "monospaceSize": 10.5,

    "appFamily": "IBM Plex Sans SemiCondensed",
    "appSize": 12,

    "headingSteps": [3, 2, 1, 0, -1, -1],

    "mathScale": 1.1
}
```

Все размеры шрифтов задаются в points.

Имена используют единый суффикс:

```text
*Size
```

а не смесь:

```text
pointSize
fontPoints
fontSize
```

---

# 13. `noteFamily` / `noteSize`

Основной шрифт заметки.

Пользователь может выбрать proportional или monospace font.

Базовый размер остаётся пользовательской настройкой, потому что разные font families при одинаковом nominal point size визуально отличаются.

---

# 14. `monospaceFamily` / `monospaceSize`

Общий monospace font для:

- fenced code;
- inline code;
- Markdown source mode;
- history/diff source representation;
- других code-like экранных режимов.

Отдельный размер нужен для согласования x-height и общего визуального размера с `noteFamily`.

---

# 15. `appFamily` / `appSize`

Общий UI font:

- sidebar;
- note list;
- status bar;
- labels;
- dialogs;
- captions;
- другие собственные элементы интерфейса Zametti.

После этого отдельные:

```text
sidebar.fontSize
statusBar.fontPoints
imageCaption.fontPoints
```

не нужны.

---

# 16. `headingSteps`

```jsonc
"headingSteps": [3, 2, 1, 0, -1, -1]
```

Это пользовательская typography preference.

Она задаёт относительный размер heading levels относительно базового note font.

---

# 17. `mathScale`

Текущие:

```text
formulas.inlineScale
formulas.displayScale
```

предлагается заменить одним:

```jsonc
"mathScale": 1.1
```

Математические шрифты уже согласованы между собой; задача пользователя — согласовать их с основным note font.

```text
math base size =
    noteSize × mathScale
```

При экранном zoom:

```text
effective math size =
    noteSize × noteZoom × mathScale
```

Название `mathScale` предпочтительнее `formulaScale`.

---

# 18. Layout

Секция остаётся, но должна содержать только реальные typography/layout preferences.

Например:

```jsonc
"layout": {
    "maxContentWidth": 90,
    "lineHeightFactor": 1.15,
    "blockSpacing": 0.667
}
```

Возможно полезно оставить:

```text
listIndent
quoteIndent
```

если нужен пользовательский контроль над плотностью layout.

Параметры вроде:

```text
caretWidth
codeCornerRadius
codeCopyIconScale
codeStripPadding
bulletRise
bulletStrokeWidth
```

должны быть internal.

---

# 19. Editor

Предлагается сильно сократить текущую секцию:

```jsonc
"editor": {
    "autosaveDelayMs": 60000,
    "codeTabWidth": 4,
    "externalEditor": "",

    "historyMergeChars": 100,
    "historyMergeHours": 24,

    "special": [
        ["Alt+-", "—"]
    ]
}
```

Внутренними defaults становятся implementation knobs вроде:

```text
documentCacheSizeMb
imageCacheSizeMb
undoBudgetMb
undoCoalesceMs
undoLimit
undoRunChars
statsDelayMs
```

Не следует создавать `advanced` только для их хранения.

---

# 20. Shortcuts

Все shortcuts собираются в одном месте:

```jsonc
"shortcuts": {
    "fullscreen": "F11; Ctrl+Meta+F",

    "makeBullet": "Ctrl+8; Ctrl+Shift+8",
    "makeOrdered": "Ctrl+7; Ctrl+Shift+7",
    "makeTask": "Ctrl+9; Ctrl+Shift+9",
    "makeParagraph": "Ctrl+Shift+0",
    "makeComment": "Ctrl+Shift+C",
    "toggleTask": "Ctrl+D; Ctrl+Space",

    "moveUp": "Ctrl+Up",
    "moveDown": "Ctrl+Down",

    "markdownMode": "",

    "diffNext": "F4; Ctrl+]",
    "diffPrevious": "Shift+F4; Ctrl+[",

    "jsonComment": "Ctrl+/"
}
```

Суффикс `Key` не нужен: namespace уже называется `shortcuts`.

---

# 21. Find

Секция `find` удаляется целиком.

Текущие:

```text
badPatternColor
fontDelta
historyLimit
matchLimit
```

относятся либо к theme/internal styling, либо к implementation/performance limits.

---

# 22. Images

Полезные части нынешних:

```text
imageCaption
imageSelection
imageViewer
images
```

объединяются в одну секцию:

```jsonc
"images": {
    "maxImportedSize": 2160,
    "maxDeletedSize": 100,

    "captions": true,
    "nonamePattern": "...",

    "viewerMaxZoomPercent": 300
}
```

Из `imageSelection` внутрь уходят:

```text
cornerMinLength
cornerOffset
cornerShare
cornerWidth
```

Из `imageCaption`:

```text
color
family
fontPoints
gap
```

Из `imageViewer`:

```text
background
captionColor
captionPoints
margin
```

Они определяются theme/fonts/internal style.

---

# 23. PDF

PDF настраивается независимо от экранной theme.

Это особенно важно для dark mode:

```text
screen:
    background = dark
    foreground = light

PDF:
    background = white
    foreground = dark
```

Предлагаемая секция:

```jsonc
"pdf": {
    "fontFamily": "IBM Plex Sans",
    "fontSize": 11,

    "monospaceFamily": "IBM Plex Mono",
    "monospaceSize": 10.5,

    "headingSteps": [3, 2, 1, 0, -1, -1],

    "background": "#ffffff",
    "foreground": "#181818",

    "link": "#325cc0",
    "quote": "#5a626a",
    "codeBackground": "#f4f4f2",

    "pageSize": "A4",
    "marginMm": 15,

    "imageDpi": 200,
    "maxImageSize": 2000
}
```

PDF может иметь собственный monospace font.

Экранные zoom levels никак не влияют на PDF export.

---

# 24. Scroll

Текущую секцию:

```jsonc
"scroll": {
    "smooth": true,
    "smoothMs": 140
}
```

предлагается убрать из публичного config.

Если smooth scrolling работает хорошо, это нормальное поведение приложения, а не пользовательская настройка.

---

# 25. Notes и sync

Если эти данные уже имеют отдельный source of truth в `state.json` или storage configuration, дублировать их в основном config не следует.

---

# 26. Logs

Секция остаётся:

```jsonc
"logs": {
    "logSizeMb": 10,
    "writeErrLog": false,
    "writeSyncLog": false
}
```

Это конкретная предметная область и хороший пример настройки, которую не нужно прятать в `advanced`.

---

# 27. Почему не нужен `advanced`

Не следует заводить:

```jsonc
"advanced": {
    ...
}
```

как постоянную секцию.

Она быстро становится местом для всего, что:

- жалко удалить;
- некуда положить;
- нужно только разработчику;
- может когда-нибудь пригодиться.

Правило:

> У публичной настройки должна быть естественная предметная секция.

Если такой секции нет, сначала надо решить, действительно ли настройка должна быть публичной.

---

# 28. `state.json`

Пример:

```json
{
    "zoom": {
        "note": 0,
        "source": 0,
        "history": 0,
        "interface": 0
    },

    "sidebar": {
        "width": 260
    },

    "noteList": {
        "width": 320
    },

    "window": {
    }
}
```

Размеры resizeable UI areas принадлежат state, а не config.

---

# 29. Четыре независимых zoom level

```text
note
source
history
interface
```

- `note` — rendered note;
- `source` — Markdown source mode;
- `history` — history/diff viewer;
- `interface` — sidebar, toolbar, status bar, note list и остальной UI.

---

# 30. Единая шкала zoom

Рекомендуемая функция:

```cpp
double zoomScale(int k)
{
    return std::exp2(double(k) / 12.0);
}
```

То есть:

```text
scale(k) = 2^(k/12)
```

Один шаг:

```text
≈ 1.059463
```

или примерно `+5.95%`.

12 шагов дают `×2`.

---

# 31. Диапазон

Старый диапазон:

```text
0.5 ... 4.0
```

точно соответствует:

```text
kMin = -12
kMax = +24
```

| k | scale | percent |
|---:|---:|---:|
| -12 | 0.5000 | 50% |
| -8 | 0.6300 | 63% |
| -6 | 0.7071 | 71% |
| -4 | 0.7937 | 79% |
| -2 | 0.8909 | 89% |
| 0 | 1.0000 | 100% |
| +2 | 1.1225 | 112% |
| +4 | 1.2599 | 126% |
| +6 | 1.4142 | 141% |
| +8 | 1.5874 | 159% |
| +12 | 2.0000 | 200% |
| +24 | 4.0000 | 400% |

Старые:

```text
zoom.min
zoom.max
zoom.step
```

из config исчезают.

---

# 32. Zoom хранится как integer

Нельзя хранить накопленный floating-point scale.

Плохо:

```cpp
currentScale *= 1.059463;
```

Правильно:

```cpp
double scale = zoomScale(k);
double effectiveSize = baseSize * scale;
```

Source of truth:

```text
base config value + integer k
```

---

# 33. Применение zoom

Rendered note:

```text
note text =
    noteSize × noteZoom
```

Code внутри note:

```text
code =
    monospaceSize × noteZoom
```

Math:

```text
math =
    noteSize × mathScale × noteZoom
```

Markdown source:

```text
source =
    monospaceSize × sourceZoom
```

History/diff (ступень слита с исходником, 02.09.2026):

```text
history =
    monospaceSize × sourceZoom
```

UI:

```text
application font =
    appSize × interfaceZoom
```

---

# 34. Interface zoom

Interface zoom влияет также на:

- toolbar icon size;
- toolbar height;
- button geometry;
- sidebar row height;
- status bar height;
- panel paddings;
- custom controls.

Но все значения считаются от immutable base metrics:

```cpp
iconSize =
    qRound(baseIconSize * zoomScale(interfaceZoom));
```

Никакого последовательного умножения уже масштабированного значения.

---

# 35. Shortcuts для zoom

```text
Ctrl+=
Ctrl+-
Ctrl+0
```

управляют активным content view:

```text
note / source / history
```

А:

```text
Ctrl+Alt+=
Ctrl+Alt+-
Ctrl+Alt+0
```

управляют interface zoom.

---

# 36. Не использовать `QTextEdit::zoomIn()` как модель Zametti zoom

Стандартный:

```cpp
QTextEdit::zoomIn(1)
QPlainTextEdit::zoomIn(1)
```

добавляет один point к размеру шрифта, а не фиксированный процент.

Поэтому:

```text
9 → 10 pt  = +11.1%
10 → 11 pt = +10.0%
11 → 12 pt =  +9.1%
16 → 17 pt =  +6.25%
```

Для Zametti нужен собственный единый механизм zoom.

---

# 37. Системный High-DPI scaling и Zametti zoom

Это два разных уровня:

```text
Zametti base style
        │
        ▼
user zoom 2^(k/12)
        │
        ▼
Qt device-independent coordinates
        │
        ▼
Qt / OS High-DPI scaling
        │
        ▼
physical pixels
```

Zametti отвечает за user zoom.

Qt и ОС отвечают за monitor scaling.

---

# 38. Не умножать обычную UI geometry на DPR

Для обычных Qt Widgets нельзя делать:

```cpp
size *= window->devicePixelRatio();
```

Qt 6 уже использует device-independent coordinates.

Ручное умножение приводит к двойному scaling.

---

# 39. Что Qt предоставляет

Для `QScreen`:

```cpp
screen->logicalDotsPerInch();
screen->physicalDotsPerInch();
screen->physicalSize();
screen->devicePixelRatio();
```

Для конкретного окна:

```cpp
window->devicePixelRatio();
```

Если target window известен, window DPR предпочтительнее screen DPR для low-level rendering decisions.

---

# 40. Logical DPI

`logicalDotsPerInch()` участвует в системном преобразовании point sizes.

Например:

```cpp
font.setPointSizeF(11.0);
```

не означает фиксированное число physical pixels.

Qt переводит point size с учётом platform logical DPI.

Zametti не должен повторять эту операцию вручную.

---

# 41. Physical DPI

`physicalDotsPerInch()` не следует использовать для автоматического UI scaling Zametti.

Причины:

- Qt уже учитывает platform scaling;
- physical display size может быть сообщён неточно;
- physical DPI не определяет user preference;
- пользователь мог сознательно выбрать другой desktop scale.

---

# 42. Когда DPR всё-таки нужен

DPR нужен для low-level raster resources:

- `QImage` backing buffer;
- pre-rendered pixmap;
- rasterized custom icon;
- offscreen buffer;
- physical-pixel cache.

Например:

```cpp
QSize logicalSize = ...;
qreal dpr = window->devicePixelRatio();

QImage image(
    QSize(
        qRound(logicalSize.width()  * dpr),
        qRound(logicalSize.height() * dpr)),
    QImage::Format_ARGB32_Premultiplied);

image.setDevicePixelRatio(dpr);
```

Обычные QWidget sizes остаются logical.

---

# 43. Перенос между мониторами

Окно может перемещаться:

```text
Retina → non-Retina
DPR 2 → DPR 1
обычный monitor → fractional Wayland monitor
```

Qt может уведомлять через:

```cpp
QWindow::screenChanged(QScreen *)
```

и:

```cpp
QEvent::DevicePixelRatioChange
```

При таком изменении:

- `interfaceZoom` не меняется;
- logical UI sizes остаются теми же;
- Qt меняет physical mapping;
- Zametti инвалидирует собственные DPR-dependent raster caches.

---

# 44. Изменение logical DPI

`QScreen` имеет:

```cpp
QScreen::logicalDotsPerInchChanged(qreal)
```

Если Zametti кэширует:

```text
QFontMetrics
layout data
derived typography metrics
```

их следует пересчитать.

Не нужно делать:

```text
ui *= newDpi / oldDpi
```

Нужно заново построить derived style из:

```text
config + zoom state
```

и позволить Qt выполнить platform mapping.

---

# 45. macOS

На macOS Qt получает Retina/HiDPI scaling от Cocoa.

Нельзя предполагать:

```text
macOS == DPR 2
```

Возможны встроенный Retina, внешний HiDPI, обычный внешний монитор и разные display scaling modes.

Модель Zametti остаётся:

```text
base style × Zametti user zoom
```

а системный display scale обрабатывается Qt.

---

# 46. Linux

На Linux также следует полагаться на Qt platform integration.

Особенно на Wayland нельзя предполагать:

```text
DPR всегда integer
```

или:

```text
screen DPR всегда равен window DPR
```

`Ctrl+Alt+=/-` — это не workaround для ошибок DPI Qt, а нормальная application-specific density/accessibility настройка.

---

# 47. Derived style

Полезно иметь immutable config и вычисляемый `EffectiveStyle`.

Например:

```cpp
struct FontSpec
{
    QString family;
    double size;
};

struct FontsConfig
{
    FontSpec note;
    FontSpec monospace;
    FontSpec app;

    std::array<int, 6> headingSteps;
    double mathScale;
};

struct ZoomLevels
{
    int note = 0;
    int source = 0;
    int history = 0;
    int interface = 0;
};
```

И:

```cpp
struct EffectiveStyle
{
    QFont noteFont;
    QFont noteCodeFont;
    QFont sourceFont;
    QFont historyFont;
    QFont appFont;

    int toolbarIconSize;
    int sidebarRowHeight;
    int statusBarHeight;

    // ...
};
```

---

# 48. Source of truth

Итоговая цепочка:

```text
built-in defaults
       +
user config
       +
resolved theme
       +
state.json integer zoom levels
       │
       ▼
derived effective style
       │
       ▼
Qt logical layout
       │
       ▼
platform High-DPI mapping
       │
       ▼
physical display
```

Ни один effective value не становится source of truth для следующего масштабирования.

---

# 49. Итог

Основной config:

```text
theme
fonts
layout
editor
shortcuts
images
pdf
logs
```

Отдельно:

```text
state.json
```

и:

```text
themes/<name>.json
```

Theme отвечает за экранные semantic colors.

PDF имеет независимую typography/color configuration.

State хранит zoom и UI geometry.

Renderer-specific optical constants остаются в коде.

Так Zametti остаётся хорошо настраиваемым, но config перестаёт быть CSS-подобным дампом внутренних параметров painter/layout engine.
