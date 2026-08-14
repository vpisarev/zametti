# Зуммирование в Zametti

## Вопрос

Можно ли сделать zoom in/zoom out документа без полной пересборки QTextDocument?

## Ответ

Да, **частично Qt это умеет нативно**, и тут есть очень интересная возможность, которую я раньше упустил: `QTextFormat::FontSizeAdjustment`. Она как раз позволяет строить размеры заголовков **относительно default font**.

Но полноценной CSS-подобной системы `em/rem/%` для *всех* размеров в `QTextDocument` нет. Поэтому я бы разделил параметры стиля на две группы.

### Шрифты — можно сделать почти идеально относительными

Для основного текста вообще не задаём `FontPointSize`:

```cpp
QTextCharFormat body;
// size отсутствует → берётся из document()->defaultFont()
```

Для code:

```cpp
QTextCharFormat code;
code.setFontFamilies({"JetBrains Mono", "monospace"});
code.setFontFixedPitch(true);

// point size НЕ задаём
```

Тогда code font меняет гарнитуру, но **размер наследует от default font**.

А для заголовков есть специальное:

```cpp
fmt.setProperty(QTextFormat::FontSizeAdjustment, +3); // H1
fmt.setProperty(QTextFormat::FontSizeAdjustment, +2); // H2
fmt.setProperty(QTextFormat::FontSizeAdjustment, +1); // H3
...
```

И это не просто теоретическая возможность — **сам Markdown importer Qt делает именно так**: для H1…H6 задаёт `FontSizeAdjustment = 4 - level`. ([CodeBrowser][1])

Внутри Qt этот adjustment преобразуется в относительный коэффициент:

```text
0.7, 0.8, 1.0, 1.2, 1.5, 2.0, 2.4
```

и умножается на размер `defaultFont`. ([CodeBrowser][2])

То есть получается именно то, что нам нужно:

```text
defaultFont = 12 pt

body = 12
H3   = 1.5 × 12 = 18
H2   = 2.0 × 12 = 24
H1   = 2.4 × 12 = 28.8
```

поменяли zoom:

```cpp
defaultFont = 15 pt;
```

автоматически:

```text
body = 15
H3   = 22.5
H2   = 30
H1   = 36
```

**И никакие char formats заголовков трогать не надо.**

Это довольно существенная находка для твоего дизайна.

---

## То есть шрифтовую часть я бы строил так

Не:

```cpp
H1.fontSize = 29;
H2.fontSize = 24;
H3.fontSize = 18;
Code.fontSize = 12;
Body.fontSize = 12;
```

а:

```text
Document default font
    └── 12 pt

Body
    └── inherit size

Code
    ├── inherit size
    └── family = monospace

H1
    └── FontSizeAdjustment = +3

H2
    └── FontSizeAdjustment = +2

H3
    └── FontSizeAdjustment = +1
```

Тогда Ctrl+ работает очень красиво:

```cpp
QFont f = document()->defaultFont();
f.setPointSizeF(basePointSize * zoom);
document()->setDefaultFont(f);
```

и **body/code/headings масштабируются автоматически одной операцией**. `FontSizeAdjustment` специально разрешается относительно default font. ([Qt Documentation][3])

---

# Но размеры geometry — уже хуже

Например:

```cpp
block.setTopMargin(8);
block.setBottomMargin(8);

table.setCellPadding(6);
frame.setBorder(1);

image.setWidth(900);
```

Эти `qreal` — абсолютные document units. Для них нет:

```cpp
setTopMargin(0.5_em);
```

или:

```cpp
setCellPadding(0.4_em);
```

`QTextLength` поддерживает percentage/fixed/variable lengths, но применяется только в отдельных местах вроде размеров frame/table; это не универсальный CSS length type для всех properties. ([Qt Documentation][4])

Есть несколько исключений. Например line spacing может быть proportional, а не абсолютным. ([Qt Documentation][5])

Но:

```text
block margins
table padding
border width
image width/height
custom marker size
```

универсально в `em` задать нельзя.

---

# Поэтому я бы сделал собственный `DocumentStyle` в относительных единицах

Например:

```cpp
struct DocumentStyle
{
    // relative to body font size
    double paragraphTopMarginEm    = 0.40;
    double paragraphBottomMarginEm = 0.40;

    double codePaddingEm = 0.50;

    double listIndentEm  = 1.8;
    double markerGapEm   = 0.5;

    double tableCellPaddingEm = 0.40;
    double tableBorderEm      = 0.06;

    // font adjustments understood directly by Qt
    int h1SizeAdjustment = +3;
    int h2SizeAdjustment = +2;
    int h3SizeAdjustment = +1;
};
```

И одна базовая величина:

```cpp
qreal em() const
{
    return QFontMetricsF(document()->defaultFont()).height();
}
```

или, что даже лучше для предсказуемости layout:

```cpp
qreal em() const
{
    return logicalBodyFontSize * dpi / 72.0;
}
```

Тогда:

```cpp
bf.setTopMargin(style.paragraphTopMarginEm * em());
bf.setBottomMargin(style.paragraphBottomMarginEm * em());

tableFormat.setCellPadding(
    style.tableCellPaddingEm * em());
```

---

## Но тут ты сразу заметишь проблему

При zoom:

```text
12 pt → 15 pt
```

Qt сам пересчитает:

```text
body
headings
code font
```

но сохранённое:

```text
topMargin = 6.4
cellPadding = 5.2
```

само не поменяется.

Поэтому **для geometry нужна повторная application стиля**.

И здесь я бы не переписывал весь документ blindly. Я бы отделил **semantic format** от **derived visual format**.

Например у блока сохраняется:

```text
BlockKind = Paragraph
```

или:

```text
BlockKind = Code
```

а конкретные:

```text
topMargin = 6.4
bottomMargin = 6.4
background = ...
```

считаются из текущего:

```text
style + zoom
```

---

# Но тогда zoom требует прохода по документу?

Да, если масштабировать geometry через стандартные `QTextFormat` properties.

И это ключевой недостаток.

Однако проход:

```text
for every block/table/image:
    update derived geometry
```

не обязательно ужасен. Он всё равно будет сопровождаться полным relayout.

Но есть ещё более интересный компромисс.

---

# Я бы НЕ масштабировал всё при Ctrl+

Для text editor я бы подумал, какие размеры действительно должны меняться.

Например:

```text
body font           × zoom
heading fonts       × zoom
code font           × zoom
formula font        × zoom
list marker         × zoom
```

Это обязательно.

А вот:

```text
paragraph margin    ?
table border        ?
table padding       ?
page margin         ?
```

совсем необязательно линейно масштабировать.

Во многих редакторах zoom фактически является **text/content zoom**, а не увеличением каждого пикселя интерфейса документа.

Например:

```text
12 pt body
6 px cell padding
1 px border
```

→ 150%:

```text
18 pt body
6 px cell padding
1 px border
```

может выглядеть совершенно нормально.

Возможно padding хочется увеличить до 8–9 px, но border почти наверняка хочется оставить 1 px.

Поэтому можно разделить стиль:

```cpp
struct StyleMetric {
    enum ScaleMode {
        Fixed,
        WithZoom,
        WithSqrtZoom
    };
};
```

Например:

```text
font             WithZoom
formula          WithZoom
list marker      WithZoom

paragraph spacing WithZoom
table padding     WithZoom

border width      Fixed
corner radius     maybe Fixed
```

---

# Формулы

С формулами всё совсем просто, поскольку renderer твой.

Если formula font определяется от:

```cpp
document->defaultFont()
```

то:

```cpp
auto baseFont = doc->defaultFont();

QFont mathFont = baseFont;
mathFont.setPointSizeF(
    baseFont.pointSizeF() * formulaRelativeScale);
```

и `intrinsicSize()` автоматически получит новый размер после изменения default font.

То есть:

```text
default font
     │
     ├── text
     ├── headings via FontSizeAdjustment
     ├── code
     └── formula renderer
```

получается единая система.

---

# Custom list markers

То же самое.

Не:

```cpp
markerWidth = 24;
```

а:

```cpp
markerWidth =
    QFontMetricsF(doc->defaultFont()).height()
    * style.markerWidthEm;
```

В `intrinsicSize()` у тебя есть `QTextDocument *`, так что renderer всегда может посмотреть текущий `defaultFont`.

После `setDefaultFont()` document layout будет invalidated, следовательно `intrinsicSize()` вызовется с новым размером.

Очень удобно.

---

# Картинки — особый случай

Здесь есть два разных смысла zoom.

Если:

```text
image presentation width = 900 document units
```

то можно решить:

### A. Images don't zoom

```text
Ctrl+:
text bigger
image remains 900
```

похоже на browser text zoom.

### B. Images zoom together with document

Тогда semantic state лучше хранить не как effective width:

```text
ImageWidth = 900 * zoom
```

а как:

```text
PreferredImageWidth = 900
```

в custom property.

А effective:

```cpp
effectiveWidth =
    preferredWidth * viewZoom;
```

вычисляет custom image object.

Именно здесь custom `QTextObjectInterface` снова становится удобнее штатного `QTextImageFormat`.

---

# Но можно и штатный `QTextImageFormat`

Есть ещё интересная возможность: начиная с Qt 6.8 у image format есть `maximumWidth` как `QTextLength`, который может быть процентным относительно доступной ширины. ([Qt Documentation][3])

Это хорошо для:

```text
image preferred width = 900
max width = 100%
```

чтобы картинка не вылезала за экран.

Но arbitrary:

```text
width = 40em
```

Qt не поддерживает.

---

# Поэтому архитектура может получиться довольно симпатичная

```text
                    base body font
                         12 pt
                           │
                   Ctrl+ → 15 pt
                           │
          ┌────────────────┼──────────────────┐
          │                │                  │
          ▼                ▼                  ▼
       body/code       headings          custom objects
       inherited       relative          formulas
                       adjustment         list markers
                                           │
                                           ▼
                                  intrinsic size in em
```

И отдельно:

```text
DocumentStyle

paragraphGap = 0.5 em
listIndent   = 2.0 em
tablePadding = 0.4 em
border       = 1 px
...
```

Часть Qt разрешает автоматически, часть твой style engine превращает в `qreal`.

---

## Самая полезная находка здесь — `FontSizeAdjustment`

Я до этого предполагал, что заголовки придётся вручную обновлять при zoom. **Нет.**

Можно построить документ так, чтобы вообще нигде, кроме `defaultFont`, не было абсолютного font size:

```text
Body       → inherited
Code       → inherited + monospace
Bold       → inherited + weight
Italic     → inherited + italic
Heading    → inherited × FontSizeAdjustment
Link       → inherited
InlineCode → inherited + monospace
Formula    → derived from defaultFont
```

Тогда **один `setDefaultFont()` действительно решает практически всю текстовую часть zoom**. Qt сам именно так представляет Markdown headings. ([CodeBrowser][1])

А оставшаяся проблема уже сильно меньше: картинки и несколько geometric style metrics. Для них я бы завёл относительные значения в твоём `DocumentStyle`, а не пытался хранить абсолютные размеры как semantic state документа.

[1]: https://codebrowser.dev/qt6/qtbase/src/gui/text/qtextmarkdownimporter.cpp.html?utm_source=chatgpt.com "qtextmarkdownimporter.cpp source code [qtbase/src/gui/text ..."
[2]: https://codebrowser.dev/qt6/qtbase/src/gui/text/qtextformat.cpp.html "qtextformat.cpp source code [qtbase/src/gui/text/qtextformat.cpp] - Codebrowser"
[3]: https://doc.qt.io/qt-6/qtextformat.html?utm_source=chatgpt.com "QTextFormat Class | Qt GUI | Qt 6.11.1"
[4]: https://doc.qt.io/qt-6/qtextlength.html?utm_source=chatgpt.com "QTextLength Class | Qt GUI | Qt 6.11.1"
[5]: https://doc.qt.io/qt-6/qtextblockformat.html?utm_source=chatgpt.com "QTextBlockFormat Class | Qt GUI | Qt 6.11.1"
