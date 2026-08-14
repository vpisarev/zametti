# Zoom в Zametti поверх `QTextDocument`

## Цель

Этот документ фиксирует предлагаемый дизайн масштабирования (`Ctrl+`, `Ctrl-`, `Ctrl+0`) в Zametti при использовании `QTextDocument` как основной live/editing модели документа.

Главная проблема: `QTextDocument` хранит типографические размеры и layout в document units, но отдельного универсального свойства:

```text
documentZoom = 1.25
```

у него нет.

При этом обычный zoom редактора не должен становиться частью Markdown-семантики, undo/redo или persistent state документа.

---

# 1. Главный принцип

Zoom — это **presentation state**, а не semantic state документа.

```text
Markdown semantics
       │
       ▼
QTextDocument
       │
       ▼
DocumentStyle + zoomFactor
       │
       ▼
layout / rendering
```

То есть:

```text
zoomFactor = 1.0
zoomFactor = 1.25
zoomFactor = 1.50
```

не должен:

- менять Markdown;
- создавать undo-команды;
- изменять канонические свойства заметки;
- сериализоваться как часть документа.

При необходимости zoom может сохраняться как UI preference отдельно от заметки.

---

# 2. Два разных смысла zoom

Важно различать два принципиально разных режима.

## 2.1. Reflow zoom

При увеличении шрифта доступная ширина остаётся прежней:

```text
100%:

This is a rather long line of text
that wraps here.

150%:

This is a rather
long line of text
that wraps here.
```

Меняются:

- glyph metrics;
- переносы строк;
- высоты blocks;
- положение последующих элементов.

Полный relayout в таком режиме **неизбежен по определению**.

## 2.2. Geometric / page zoom

Весь документ масштабируется как уже сверстанная страница:

```text
100%

┌──────────────────┐
│ Some text        │
│ [ image ]        │
│ table            │
└──────────────────┘

150%

┌────────────────────────────┐
│ Some text                  │
│ [       image       ]       │
│ table                      │
└────────────────────────────┘
```

При этом line breaks сохраняются.

Концептуально:

```text
layout once
    ↓
multiply coordinates by zoom
    ↓
display
```

Это наиболее чистое представление "масштаба документа".

У `QTextEdit` нет полноценного публичного view transform для этого режима.

---

# 3. Что делает штатный `QTextEdit::zoomIn()`

Штатный zoom `QTextEdit` в первую очередь меняет базовый font size.

Концептуально:

```text
Ctrl+
   ↓
default font 12 pt → 13 pt
   ↓
QTextDocument layout invalidated
   ↓
full relayout
```

Это не rebuild документа:

```text
Markdown → parse → QTextDocument
```

не выполняется.

Сохраняются:

- blocks;
- text;
- formats;
- undo stack;
- custom semantic properties.

Но такой zoom недостаточен для Zametti, потому что документ содержит не только body text:

```text
body text
headings
code blocks
formulas
images
list markers
tables
margins
padding
borders
```

Если эти параметры заданы абсолютными величинами, один `setDefaultFont()` их не масштабирует.

---

# 4. Относительные размеры шрифтов

Для текстовой части Qt предоставляет полезный механизм:

```text
QTextFormat::FontSizeAdjustment
```

Это позволяет не задавать heading font size как абсолютный point size.

Вместо:

```text
Body = 12 pt
H1   = 29 pt
H2   = 24 pt
H3   = 18 pt
```

можно хранить:

```text
Body
    inherit document default font size

Code
    inherit document default font size
    family = monospace

H1
    FontSizeAdjustment = +3

H2
    FontSizeAdjustment = +2

H3
    FontSizeAdjustment = +1
```

Тогда:

```text
default font = 12 pt

body = 12
code = 12
headings = relative sizes
```

после:

```text
default font = 15 pt
```

вся текстовая иерархия масштабируется автоматически.

---

# 5. Главное правило для `QTextCharFormat`

По возможности **не задавать абсолютный `FontPointSize`** в inline/semantic styles.

Например:

```text
Body:
    no explicit point size

Bold:
    weight only

Italic:
    italic only

Link:
    href + color/underline
    no explicit point size

InlineCode:
    monospace family
    no explicit point size

CodeBlock:
    monospace family
    no explicit point size

Heading:
    FontSizeAdjustment
```

Тогда один:

```cpp
QFont f = document.defaultFont();
f.setPointSizeF(basePointSize * zoomFactor);
document.setDefaultFont(f);
```

масштабирует почти всю текстовую часть документа.

---

# 6. Формулы

Формулы в Zametti предполагаются как custom objects:

```text
U+FFFC
    objectType = FormulaObject
    FormulaSource = ...
```

Поскольку renderer принадлежит приложению, формулу легко сделать относительной к default font.

Концептуально:

```cpp
const QFont base = doc->defaultFont();

QFont mathFont = base;
mathFont.setPointSizeF(
    base.pointSizeF() * formulaScale);
```

`intrinsicSize()` пересчитывает размер формулы из текущего default font.

```text
default font
     │
     ├── body
     ├── code
     ├── headings
     └── formula renderer
```

Следовательно, формулы естественно следуют за zoom.

---

# 7. Custom list markers

То же самое относится к custom markers ordered/unordered/task lists.

Не хранить:

```text
markerWidth = 24 px
markerHeight = 18 px
```

как semantic state.

Лучше:

```text
markerWidth = 1.8 em
markerGap   = 0.5 em
```

и в renderer вычислять effective dimensions относительно базового font metric.

Например:

```cpp
qreal em =
    QFontMetricsF(doc->defaultFont()).height();

qreal markerWidth =
    style.markerWidthEm * em;
```

После изменения default font `QTextDocument` relayout'ится, `intrinsicSize()` вызывается снова, и marker получает новый размер.

---

# 8. Geometry не имеет универсального CSS-like `em`

Для многих layout properties Qt использует абсолютный `qreal`.

Например:

```text
QTextBlockFormat:
    topMargin
    bottomMargin
    leftMargin
    rightMargin

QTextTableFormat:
    cellPadding
    cellSpacing
    border

QTextFrameFormat:
    margins
    padding
    border

QTextImageFormat:
    width
    height
```

У `QTextDocument` нет универсальной единицы:

```text
0.5em
2rem
25%
```

для всех этих свойств.

Поэтому для geometry имеет смысл ввести собственный `DocumentStyle`.

---

# 9. `DocumentStyle` в относительных единицах

Например:

```cpp
struct DocumentStyle
{
    double paragraphTopMarginEm    = 0.40;
    double paragraphBottomMarginEm = 0.40;

    double codeBlockPaddingEm = 0.50;

    double listIndentEm  = 1.80;
    double markerGapEm   = 0.50;

    double tableCellPaddingEm = 0.40;

    // Не всё обязано масштабироваться:
    double tableBorderPx = 1.0;

    int h1SizeAdjustment = +3;
    int h2SizeAdjustment = +2;
    int h3SizeAdjustment = +1;
};
```

Дальше:

```cpp
qreal em() const
{
    return QFontMetricsF(document.defaultFont()).height();
}
```

или использовать собственную логическую базовую величину, рассчитанную из body font size.

Применение:

```cpp
const qreal e = em();

blockFormat.setTopMargin(
    style.paragraphTopMarginEm * e);

blockFormat.setBottomMargin(
    style.paragraphBottomMarginEm * e);

tableFormat.setCellPadding(
    style.tableCellPaddingEm * e);
```

---

# 10. Не все размеры обязаны масштабироваться одинаково

Полезно различать три категории style metrics.

## 10.1. Масштабируются линейно

```text
font size
formula size
list marker size
paragraph spacing
list indentation
table padding
```

Например:

```text
effective = logical * zoomFactor
```

или через текущий `em`.

## 10.2. Остаются фиксированными

Некоторые purely visual параметры разумнее не увеличивать:

```text
1 px table border
hairline separator
selection outline
caret width
focus ring
```

Например:

```text
border = 1 px
```

и при 150% он остаётся 1 px.

## 10.3. Могут масштабироваться нелинейно

Иногда можно использовать условный:

```text
sqrt(zoom)
```

или clamp:

```text
min/max
```

Например corner radius или shadow.

Это уже policy Zametti, а не свойство `QTextDocument`.

---

# 11. Semantic style vs derived visual style

Важно не смешивать:

```text
BlockKind = Heading
ListKind = Ordered
FormulaKind = Display
```

с:

```text
font size = 28.8
margin = 8.4
padding = 6.2
```

Первое — semantic state.

Второе — derived presentation.

Концептуально:

```text
semantic properties
        │
        ▼
DocumentStyle
        │
    zoomFactor
        │
        ▼
effective QTextFormats
```

---

# 12. Нужно ли проходить по документу при zoom

Для font properties, построенных через inheritance + `FontSizeAdjustment`, нет необходимости вручную обновлять каждый fragment.

Достаточно изменить:

```text
document default font
```

Но для geometry, уже записанной в `QTextBlockFormat`, `QTextTableFormat` и других formats, значения сами автоматически не поменяются.

Поэтому возможны два подхода.

## Вариант A: небольшой style pass

При изменении zoom:

```text
change default font
        +
reapply derived geometry
        +
one full relayout
```

Например пройти:

```text
blocks
tables
custom structural objects
```

и обновить только visual metrics.

Поскольку сам relayout всё равно требуется, дополнительный линейный проход может оказаться достаточно дешёвым.

## Вариант B: максимально вычислять geometry на лету

Для custom objects:

```text
formula
list marker
custom image
```

размер можно вычислять непосредственно в `intrinsicSize()` из текущего style + default font.

Тогда при zoom не требуется записывать новый размер обратно в semantic properties.

---

# 13. Изображения

Изображения — отдельный вопрос, потому что для них нужно выбрать семантику zoom.

## 13.1. Browser-like text zoom

```text
text grows
images keep presentation size
```

Тогда:

```text
ImageWidth = 900
```

остаётся 900 document units.

## 13.2. Document/page zoom

```text
text grows
images grow
tables grow
formulas grow
```

Тогда хранить:

```text
preferredWidth = 900
```

как semantic/presentation base size.

Effective size:

```text
effectiveWidth = preferredWidth * zoomFactor
```

Лучше не записывать `900 * zoom` обратно в semantic state.

---

# 14. Штатный `QTextImageFormat` vs custom image object

Если используется обычный `QTextImageFormat`, width/height являются format properties и не получают автоматический multiplier от default font.

Следовательно, при document zoom потребуется либо:

```text
style pass → update width/height
```

либо иной механизм.

Custom `QTextObjectInterface` даёт больше контроля:

```text
ImageId
preferredWidth
preferredHeight
```

остаются постоянными, а:

```cpp
intrinsicSize()
```

возвращает:

```text
preferredSize * zoomFactor
```

Это особенно удобно, если image renderer уже интегрирован с собственным `ImageStore`/LRU.

Но ради одного zoom переход на custom image object не обязателен.

---

# 15. Responsive width и zoom — разные вещи

Не следует смешивать:

```text
zoom
```

и:

```text
available content width
```

Например пользователь задал изображению:

```text
preferred width = 900
```

а mobile content width равен 600.

Можно получить:

```text
effective width = min(
    preferredWidth * zoom,
    availableContentWidth)
```

или другую policy.

Важно, что:

```text
preferred width
```

не меняется при переходе между desktop/mobile.

---

# 16. Page width при geometric zoom

Если хочется сохранить практически те же line breaks при relayout-based реализации geometric zoom, можно масштабировать одновременно:

```text
font size
page/content width
margins
indentation
images
custom objects
padding
```

Например:

```text
100%:

page width = 1000
body font  = 14
margin     = 60
image      = 800
```

при 150%:

```text
page width = 1500
body font  = 21
margin     = 90
image      = 1200
```

Тогда отношения:

```text
font / page width
margin / page width
image / page width
```

остаются примерно неизменными.

Qt всё равно делает полный relayout, но геометрия получается близкой к простому масштабированию уже готового документа.

Это может быть хорошим компромиссом для Widgets.

---

# 17. Почему настоящий view transform сложнее в `QTextEdit`

Кажется заманчивым сделать:

```cpp
painter.scale(zoom, zoom);
```

Но `QTextEdit` сам рассчитывает:

```text
mouse hit testing
cursor geometry
selection
scroll bars
IME rectangle
drag & drop
context menu position
```

в собственных координатах viewport/document.

Если масштабировать только painting:

```text
render coordinate != input coordinate
```

и взаимодействие ломается.

Настоящий geometric zoom поверх Widgets потребовал бы фактически custom editor view:

```text
QTextDocument
      ↓
custom viewport
      ↓
QPainter transform

mouse position
      ↓
inverse transform
      ↓
document hitTest
```

плюс отдельную поддержку scrollbars, cursor, selection и IME.

Это значительно дороже обычного relayout.

---

# 18. Qt Quick / QML

В Qt Quick geometric transform естественнее, потому что `QQuickItem` имеет штатный:

```text
scale
```

и scene graph умеет преобразовывать координаты.

Концептуально:

```text
TextEdit / document item
        ↓
scale = zoomFactor
        ↓
scene graph transform
```

Это делает smooth pinch zoom значительно естественнее, чем в Widgets.

Однако desktop Widgets frontend не обязательно переписывать только ради этого.

---

# 19. Дискретный zoom vs smooth pinch zoom

## Desktop `Ctrl+` / `Ctrl-`

Можно спокойно делать:

```text
one zoom step
    ↓
update default font/style metrics
    ↓
one full relayout
```

Это простой и предсказуемый вариант.

## Smooth pinch zoom

Не стоит делать полный relayout на каждое промежуточное:

```text
1.000
1.014
1.031
1.048
...
1.50
```

Предпочтительная схема:

```text
pinch starts
    ↓
use temporary visual transform
    ↓
1.00 → 1.12 → 1.29 → 1.43
    ↓
pinch ends
    ↓
commit zoomFactor = 1.43
    ↓
ONE real relayout
```

В Qt Quick это естественно.

В Widgets такая оптимизация требует дополнительной view-level логики и может быть отложена до появления реальной необходимости.

---

# 20. Предлагаемый zoom state

Например:

```cpp
class DocumentViewStyle
{
public:
    double baseBodyPointSize = 12.0;
    double zoomFactor = 1.0;

    DocumentStyle style;

    double effectiveBodyPointSize() const
    {
        return baseBodyPointSize * zoomFactor;
    }
};
```

Важно:

```text
baseBodyPointSize
DocumentStyle
```

могут быть persistent user/theme preferences.

А:

```text
zoomFactor
```

— view/session preference.

---

# 21. Suggested implementation flow

## При создании документа

```text
1. set document default font
2. build semantic QTextDocument
3. headings use FontSizeAdjustment
4. code inherits size and changes family only
5. formulas/markers derive size from default font
6. geometry is generated from DocumentStyle
```

## При Ctrl+

```text
zoomFactor *= step

↓

// textual hierarchy
document.setDefaultFont(
    baseFont scaled by zoomFactor)

// derived geometry
reapplyVisualMetrics(document, style, zoomFactor)

↓

// QTextDocument performs relayout
```

Никакой semantic property не меняется.

---

# 22. Undo/redo

Zoom не должен быть QTextDocument edit operation.

То есть:

```text
Ctrl+
Ctrl+
Ctrl-
```

не добавляет ничего в:

```text
QTextDocument undo stack
```

Undo остаётся только для:

```text
text edits
formatting edits
table edits
formula edits
image placement edits
...
```

Размер изображения, установленный пользователем как content property, и zoom — разные вещи.

Например:

```text
image preferred width = 900
zoom = 150%
```

`Ctrl+Z` может отменить изменение:

```text
900 → 700
```

но не должен отменять `zoom = 150%`.

---

# 23. Практическая рекомендуемая политика Zametti

Для первого варианта реализации:

### Fonts

```text
body       → inherited default font
code       → inherited size + monospace family
headings   → FontSizeAdjustment
links      → inherited size
inline code→ inherited size
```

### Custom objects

```text
formulas    → size relative to default font
list marker → size relative to default font
```

### Geometry

```text
paragraph spacing → em-based DocumentStyle
list indent       → em-based
table padding     → em-based
border            → fixed or separately scaled
```

### Images

Выбрать одну явную policy:

```text
document zoom scales images
```

или:

```text
text zoom does not scale images
```

Для Zametti более естественным кажется **document zoom**, то есть изображения масштабируются вместе с остальным содержимым.

### Implementation

```text
Ctrl+/Ctrl-
     ↓
change zoomFactor
     ↓
change document default font
     ↓
reapply derived geometry
     ↓
one QTextDocument relayout
```

---

# 24. Итог

У `QTextDocument` нет универсальной CSS-like relative layout system и отдельного `zoomFactor`.

Но значительная часть задачи решается достаточно чисто:

```text
text sizes
    → inheritance from default font
    → FontSizeAdjustment for headings

formula/list marker
    → custom QTextObjectInterface
    → derive metrics from default font

margins/padding/indent
    → own DocumentStyle in em-like units

images
    → preferred size + view zoom policy
```

Главная архитектурная идея:

> **Не хранить zoomed размеры как semantic state. Хранить логические/относительные style values и получать effective presentation из `DocumentStyle × zoomFactor`.**

При дискретном desktop zoom полный relayout `QTextDocument` является приемлемой и естественной операцией.

Если позднее потребуется абсолютно плавный geometric pinch zoom, это уже вопрос конкретного view layer: в Qt Quick он реализуется значительно естественнее, чем в `QTextEdit`, но менять document model ради этого не требуется.
