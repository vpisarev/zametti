# Быстрый block-based layout для QTextDocument

## Цель

Сделать специализированный `QAbstractTextDocumentLayout` для Markdown-редактора, который сохраняет штатную текстовую инфраструктуру Qt:

* `QTextDocument`;
* `QTextCursor`;
* undo/redo;
* `QTextCharFormat`;
* Unicode shaping;
* bidi;
* selection;
* IME;
* clipboard;
* `QTextLayout`;

но отказывается от тяжёлой general-purpose модели `QTextDocumentLayout` с frames, tables, floats и прочей произвольной вложенной геометрией.

Документ рассматривается как простая последовательность независимых блоков:

```text
Block 0
Block 1
Block 2
...
Block N-1
```

Каждый блок может быть либо обычным текстовым блоком, либо полностью кастомным визуальным блоком.

---

## Базовая модель

```cpp
enum class BlockKind {
    Text,
    Image,
    Formula,
    Diagram,
    Table
};
```

Для каждого блока хранится только его **локальная геометрия**:

```cpp
struct BlockLayoutInfo {
    float height;
    float width;

    uint32_t layoutRevision;

    BlockKind kind;

    // дополнительные данные:
    // indent, margins, marker type, etc.
};
```

Абсолютную вертикальную координату `y` блока хранить не нужно.

Вместо этого:

```text
y(block) = sum(height[0 ... block-1])
```

Это позволяет изменять высоту одного блока, не обновляя геометрию всех последующих блоков.

---

# Индекс высот

Для быстрого вычисления prefix sum используется иерархическое дерево с большим branching factor, например 32.

Уровень 0 содержит высоты отдельных блоков:

```text
h0 h1 h2 h3 ... hN
```

Уровень 1 содержит суммы групп по 32 блока:

```text
Σ[0..31]
Σ[32..63]
Σ[64..95]
...
```

Уровень 2 содержит суммы групп по 1024 блока:

```text
Σ[0..1023]
Σ[1024..2047]
...
```

и далее:

```text
32
1024
32768
1048576
...
```

Таким образом даже для документа порядка миллиона блоков дерево имеет всего около четырёх уровней.

Условно:

```cpp
class HeightIndex {
public:
    void setHeight(int block, float height);

    float height(int block) const;

    // Σ height[0 ... block-1]
    float prefixSum(int block) const;

    // найти блок, содержащий вертикальную координату y
    int blockAtY(float y) const;

    float totalHeight() const;
};
```

---

# Основные операции

## Изменение одного блока

Пользователь редактирует блок:

```text
old height = 61 px
new height = 82 px
```

Обновляется только:

```cpp
heightIndex.setHeight(blockNumber, 82);
```

и по одному aggregate value на каждом уровне дерева.

Сложность:

```text
O(log₃₂ N)
```

Для реальных размеров документа это практически константа.

---

## Получение прямоугольника блока

```cpp
QRectF blockRect(int i)
{
    const float y = heightIndex.prefixSum(i);

    return QRectF(
        0,
        y,
        documentWidth,
        blocks[i].height
    );
}
```

Ключевой принцип:

> Не хранить абсолютный `y` каждого блока.

Если один ранний блок изменил высоту, геометрия всех следующих блоков логически сдвинулась автоматически благодаря prefix sum.

---

# Поиск блока по координате

Для mouse hit testing или определения видимой области:

```cpp
int block = heightIndex.blockAtY(y);

float blockY = heightIndex.prefixSum(block);
float localY = y - blockY;
```

После этого hit testing выполняется только внутри найденного блока.

Для обычного текста:

```cpp
QTextLayout *layout = textBlock.layout();
```

и дальше используются штатные `QTextLine`.

---

# Отрисовка

Viewport сначала определяет первый и последний видимые блоки:

```cpp
int first = heightIndex.blockAtY(viewTop);
int last  = heightIndex.blockAtY(viewBottom);
```

После этого обходятся только реально видимые блоки:

```cpp
for (int i = first; i <= last; ++i)
    drawBlock(i);
```

Таким образом стоимость paint практически не зависит от общей длины документа.

---

# Обычные текстовые блоки

Внутри текстового блока используется штатный `QTextLayout`.

Пример layout:

```cpp
QTextLayout *layout = block.layout();

layout->beginLayout();

float y = topMargin;

while (true) {
    QTextLine line = layout->createLine();

    if (!line.isValid())
        break;

    line.setLineWidth(availableWidth);

    line.setPosition(
        QPointF(textLeft, y)
    );

    y += line.height();
}

layout->endLayout();
```

Qt по-прежнему отвечает за:

* Unicode shaping;
* ligatures;
* bidi;
* font fallback;
* line breaking;
* char formats.

Custom layout отвечает только за геометрию блока.

---

# Списки

Маркер списка вообще не обязан существовать в тексте.

Документ содержит:

```text
Some list item
```

а layout задаёт:

```text
      ●  Some list item which is sufficiently long
         to continue onto the next visual line
```

Например:

```cpp
float markerLeft = baseMargin + level * indent;
float textLeft   = markerLeft + markerAreaWidth;
```

Сам `QTextLayout` получает:

```cpp
line.setLineWidth(documentWidth - textLeft - rightMargin);
line.setPosition(QPointF(textLeft, y));
```

А marker рисуется отдельно:

```cpp
drawListMarker(...);
```

Преимущества:

* курсор никогда не останавливается на marker;
* marker не попадает в selection;
* marker не попадает в clipboard;
* можно использовать произвольную графику;
* numbering можно полностью контролировать;
* легко делать hanging indent.

---

# Кастомные блоки

Картинка, display formula или диаграмма представляют собой полноценный блок известной высоты:

```text
┌─────────────────────────────────────┐
│                                     │
│               IMAGE                 │
│                                     │
└─────────────────────────────────────┘
```

Для него не требуется `QTextObjectInterface`.

Например:

```cpp
blocks[i].kind = BlockKind::Image;
blocks[i].height = calculatedImageHeight;
```

При отрисовке:

```cpp
switch (block.kind) {
case BlockKind::Text:
    drawTextBlock(...);
    break;

case BlockKind::Image:
    drawImageBlock(...);
    break;

case BlockKind::Formula:
    drawFormulaBlock(...);
    break;
}
```

Таким образом можно сказать:

> «Этот горизонтальный диапазон документа полностью принадлежит custom renderer».

---

# Изменение ширины документа

Изменение ширины окна — особый случай.

Для текстовых блоков меняется wrapping:

```text
width ↓
    → lines ↑
    → block height ↑
```

Поэтому потенциально требуется relayout всех текстовых блоков.

Простейший вариант:

```cpp
for (all text blocks)
    relayout(block);
```

и затем перестроить `HeightIndex`.

Сначала имеет смысл проверить производительность именно такого простого решения.

Если понадобится дополнительная оптимизация, можно использовать revision:

```cpp
uint32_t currentWidthRevision;
```

Каждый блок хранит:

```cpp
uint32_t widthRevision;
```

и:

```cpp
void ensureLayout(Block &block)
{
    if (block.widthRevision == currentWidthRevision)
        return;

    relayout(block);
    block.widthRevision = currentWidthRevision;
}
```

Но полностью lazy layout усложняет вычисление абсолютных координат ещё не переразложенных блоков, поэтому его лучше добавлять только при реальной необходимости.

---

# Структура классов

Примерно:

```text
QTextDocument
      │
      ▼
ZamettiDocumentLayout
      │
      ├── QTextLayout каждого текстового блока
      │
      ├── BlockLayoutInfo[]
      │
      ├── HeightIndex
      │
      └── custom renderers
              ├── image
              ├── formula
              ├── diagram
              └── table
```

Сам layout:

```cpp
class ZamettiDocumentLayout
    : public QAbstractTextDocumentLayout
{
public:
    QRectF blockBoundingRect(
        const QTextBlock &block) const override;

    QRectF frameBoundingRect(
        QTextFrame *frame) const override;

    QSizeF documentSize() const override;

    int pageCount() const override;

    void draw(
        QPainter *painter,
        const PaintContext &context) override;

    int hitTest(
        const QPointF &point,
        Qt::HitTestAccuracy accuracy) const override;

protected:
    void documentChanged(
        int position,
        int charsRemoved,
        int charsAdded) override;

private:
    std::vector<BlockLayoutInfo> blocks;
    HeightIndex heights;
};
```

---

# Главный invariant

Можно намеренно отказаться от general-purpose структуры `QTextDocument`:

```text
root frame
    block
    block
    block
    block
    ...
```

То есть layout поддерживает только линейную последовательность Markdown-блоков.

Не поддерживаются как arbitrary document structure:

* вложенные `QTextFrame`;
* floating frames;
* произвольные Qt tables;
* arbitrary nested rich-text layout.

Markdown-конструкции реализуются непосредственно block layout-ом.

Это ограничение одновременно является основной оптимизацией.

---

# Ожидаемая сложность

При локальном редактировании:

```text
relayout изменённого QTextBlock
+
O(log₃₂ N) update HeightIndex
+
repaint видимой области
```

Вместо потенциального распространения геометрических изменений по всему документу.

Типичные операции:

```text
block → y               O(log₃₂ N)
y → block               O(log₃₂ N)
change block height     O(log₃₂ N)
document height         O(1)
paint                    O(number of visible blocks)
```

При branching factor 32 значение `log₃₂ N` для практически любого реалистичного документа составляет несколько операций.

---

# Первый прототип

Перед реализацией Markdown-specific возможностей имеет смысл сделать минимальный benchmark:

```text
QTextDocumentLayout
QPlainTextDocumentLayout
ZamettiDocumentLayout
```

Первая версия `ZamettiDocumentLayout` должна поддерживать только:

1. линейную последовательность `QTextBlock`;
2. штатный `QTextLayout`;
3. word wrapping;
4. `blockBoundingRect()`;
5. `hitTest()`;
6. viewport painting;
7. `HeightIndex`.

Без:

```text
images
formulas
lists
tables
custom objects
```

Если уже эта версия по производительности оказывается близкой к `QPlainTextDocumentLayout`, архитектурная гипотеза подтверждена.

После этого можно постепенно добавить:

1. per-block margins;
2. list indentation;
3. custom list markers;
4. arbitrary-height custom blocks;
5. images;
6. display formulas;
7. diagrams;
8. tables.

---

# Основной принцип

> `QTextLayout` решает сложную задачу отображения текста внутри одного блока.
> `ZamettiDocumentLayout` решает простую задачу размещения блоков друг под другом.

А вертикальная геометрия документа представляется не массивом абсолютных координат, а иерархическим индексом локальных высот.

Это должно позволить сохранить почти всю полезную текстовую инфраструктуру Qt, но приблизить стоимость layout-а к `QPlainTextDocumentLayout`.
