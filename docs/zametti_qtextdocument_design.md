# Zametti: `QTextDocument` как единая in-memory модель Markdown-документа

## Статус документа

Этот документ фиксирует предлагаемый дизайн внутреннего представления заметок Zametti: отказ от отдельного постоянно живущего Markdown IR в пользу `QTextDocument` как единственной редактируемой in-memory модели документа.

При этом:

- **канонический формат на диске остаётся Markdown**;
- Markdown разбирается **md4c**;
- сериализация обратно в `.md` выполняется собственным writer'ом Zametti;
- вложения (в первую очередь изображения) хранятся отдельно;
- история правок хранится отдельно в виде сжатого журнала (`zstd` + дельты);
- `QTextDocument` не сериализуется и существует только в памяти;
- исходный Markdown при импорте канонизируется, поэтому сохранять конкретный синтаксический вариант записи одного и того же Markdown-конструкта не требуется.

---

# 1. Основная идея

Текущее устройство можно грубо представить так:

```text
.md
 ↓
md4c
 ↓
Markdown IR
 ↓
QTextDocument
 ↓
UI
```

При редактировании возникают две mutable-модели одного и того же документа:

```text
Markdown IR  ↔  QTextDocument
```

и требуется постоянно поддерживать их семантическую эквивалентность.

Предлагаемый вариант:

```text
                     md4c
                      ↓
.md  ───────────► ZDocument
                      │
                      ├── metadata
                      ├── QTextDocument
                      ├── runtime caches
                      └── image/resource access
                      │
                      ▼
               UI / CLI tools

ZDocument
      │
      ▼
canonical Markdown writer
      │
      ▼
     .md
```

Таким образом:

```text
persistent model = canonical Markdown
editing model    = QTextDocument
version model    = history journal
```

Отдельного постоянно живущего Markdown IR больше нет.

---

# 2. Канонический Markdown

Zametti не ставит целью byte-for-byte сохранение произвольного Markdown, импортированного извне.

При импорте внешний Markdown переводится в канонический диалект Zametti.

В частности:

- заметке назначается 14-значный ID, который используется как имя файла;
- добавляется служебная шапка в HTML-комментарии вида:

```markdown
<!-- zametti
...
-->
```

- хвостовые пробелы удаляются;
- файл всегда заканчивается одним `\n`;
- если заметка не начинается с заголовка, добавляется:

```markdown
# <старое имя файла>
```

- заголовки записываются только в ATX-форме:

```markdown
# Heading 1
## Heading 2
### Heading 3
```

а не в Setext-форме;

- unordered list использует `-`;
- ordered list сериализуется реальными номерами:

```markdown
1. one
2. two
3. three
```

а не формой `1. 1. 1.`;
- italic сериализуется в канонической форме `_text_`;
- остальные неоднозначные варианты Markdown-синтаксиса аналогично сводятся к одному варианту.

Следовательно, `QTextDocument` должен сохранять **семантику Markdown**, но не обязан помнить, каким из эквивалентных способов она была записана во входном файле.

Главный инвариант уже проверяется автоматически:

```text
parse(write(parse(x))) == parse(x)
```

где равенство понимается как семантическое равенство документов.

Полезно также считать канонизатор идемпотентным:

```text
canonicalize(canonicalize(x)) == canonicalize(x)
```

---

# 3. `ZDocument`: композиция вместо наследования

Предпочтительный дизайн — **не наследоваться от `QTextDocument`**, а владеть им.

Например:

```cpp
class ZDocument
{
public:
    QTextDocument& textDocument() noexcept { return document_; }
    const QTextDocument& textDocument() const noexcept { return document_; }

    NoteMetadata& metadata() noexcept { return metadata_; }
    const NoteMetadata& metadata() const noexcept { return metadata_; }

    void loadMarkdown(std::string_view markdown);
    std::string saveMarkdown() const;

private:
    NoteMetadata metadata_;
    QTextDocument document_;

    // runtime-only state
    ImageCache imageCache_;
    ListNumberCache listNumberCache_;
};
```

При этом нет смысла проксировать весь API `QTextDocument`.

Низкоуровневый код может явно получить:

```cpp
QTextDocument& doc = note.textDocument();
```

а `ZDocument` отвечает за семантически значимые операции уровня приложения.

Например:

```cpp
insertFormula(...);
toggleTask(...);
setHeadingLevel(...);
insertImage(...);
applyExternalFileChange(...);
```

## Почему композиция лучше наследования

`ZDocument` содержит данные и службы, которые не являются частью rich-text документа:

- metadata из HTML-шапки;
- ID заметки;
- путь к файлу;
- revision внешнего файла;
- cloud-sync state;
- image LRU cache;
- derived cache нумерации списков;
- runtime parser/editor state.

Это естественно является оболочкой вокруг `QTextDocument`, а не разновидностью самого `QTextDocument`.

---

# 4. Что именно хранит `QTextDocument`

Удобно рассматривать структуру следующим образом:

```text
QTextDocument
│
├── QTextBlock
│    ├── QTextBlockFormat
│    │     paragraph-level semantics / formatting
│    │
│    ├── QTextBlockUserData
│    │     runtime-only metadata/cache
│    │
│    └── QTextFragment...
│          └── QTextCharFormat
│                inline formatting / semantic properties
│
├── QTextTable / QTextFrame
│
└── inline objects
      U+FFFC + QTextCharFormat(objectType=...)
```

### Обычный текст и inline formatting

Обычный текст остаётся обычным текстом.

При смене `QTextCharFormat` блок логически разбивается на runs (`QTextFragment`):

```text
"Hello "
    normal

"bold"
    bold

" and "
    normal

"red"
    foreground=red
```

Никаких специальных символов для bold/italic/color/background не требуется.

### Custom inline object

Для формулы, custom marker и других атомарных inline-объектов используется:

```text
U+FFFC  Object Replacement Character
```

и соответствующий `QTextCharFormat`:

```text
objectType = FormulaObject
FormulaSource = ...
```

Один зарегистрированный `QTextObjectInterface` обслуживает все экземпляры данного типа.

---

# 5. Persistent metadata и runtime metadata

Нужно различать два вида метаданных.

## 5.1. Семантика документа

То, что должно участвовать в undo/redo и определять Markdown-содержимое, хранится в `QTextFormat` custom properties.

Application-specific свойства следует размещать начиная с:

```cpp
QTextFormat::UserProperty
```

Например:

```cpp
enum ZamettiTextProperty {
    ListId = QTextFormat::UserProperty + 1,
    ListKind,
    ListLevel,
    TaskState,

    FormulaSource,
    FormulaKind,

    FootnoteId,
    CodeBlockId
};
```

Такие свойства являются частью char/block format и естественно переживают undo/redo.

## 5.2. Derived/runtime state

Вещи вроде:

- рассчитанного номера list item;
- parser cache;
- dirty flags;
- temporary geometry;
- syntax state;

лучше хранить в:

```cpp
QTextBlockUserData
```

или во внешнем runtime cache.

`QTextBlockUserData` не является persistent semantic state и не восстанавливается как часть стандартного undo/redo после удаления/восстановления блока.

---

# 6. `QTextDocument` и layout

`QTextDocument` одновременно содержит:

1. структурированное rich-text содержимое;
2. информацию, необходимую layout engine;
3. связь с `QAbstractTextDocumentLayout`.

Но структура документа может быть полезна и без фактического расчёта layout.

Для bulk/headless operations:

```cpp
doc.setLayoutEnabled(false);
```

При выключенном layout можно выполнять структурные операции:

```text
blocks
fragments
formats
tables
frames
QTextCursor editing
undo/redo
Markdown serialization
custom semantic traversal
```

и не требуется постоянно пересчитывать строки и геометрию документа.

Layout-dependent операции следует считать отдельным слоем:

```text
QTextLayout / QTextLine
font shaping
glyph runs
pixel geometry
line wrapping
document size
blockBoundingRect()
hit testing
painting
printing
```

Удобный режим загрузки GUI-документа:

```cpp
doc.setLayoutEnabled(false);

parseWithMd4cAndBuildDocument(doc);

doc.setLayoutEnabled(true);
```

Таким образом при импорте большого Markdown-файла не требуется делать layout после каждого incremental изменения.

---

# 7. `QFont`, `QCoreApplication` и `QGuiApplication`

Это отдельный важный архитектурный нюанс.

## 7.1. `QTextDocument` относится к `QtGui`

Даже если документ используется как структурная модель без UI:

```text
QTextDocument
QTextBlock
QTextFormat
QFont
```

находятся в модуле:

```text
Qt6::Gui
```

а не только в `Qt6::Core`.

Следовательно, после отказа от собственного IR `zametti-core` становится зависим от `QtGui`.

Это осознанная зависимость: `QTextDocument` предоставляет существенно больше, чем простой контейнер.

## 7.2. Зачем `QTextDocument` нужен `QFont`, если layout выключен

Даже без layout `QTextCharFormat` содержит свойства шрифта:

```text
font family
size
weight
italic
underline
strikeout
letter spacing
...
```

Qt использует `QFont` как внутреннее resolved-представление этих font properties.

Это не означает, что при каждом изменении `QTextDocument` обязательно происходит:

```text
поиск реального файла шрифта
→ shaping
→ построение glyph run
→ вычисление pixel metrics
```

Эти тяжёлые операции относятся прежде всего к layout/rendering.

Иными словами:

```text
layout OFF

QTextFormat
   ↓
QFont как описание font attributes
   ↓
без необходимости считать glyph layout
```

## 7.3. Официальный контракт Qt

Документация `QFont` требует, чтобы перед использованием `QFont` существовал `QGuiApplication`.

Поэтому строго поддерживаемая схема для CLI:

```cpp
int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);

    QTextDocument doc;
    doc.setLayoutEnabled(false);

    ...
}
```

Никаких окон создавать не требуется.

Для headless Linux-процесса при необходимости можно использовать offscreen QPA platform.

Концептуально:

```text
headless CLI

QGuiApplication
     │
     └── no windows
         no QTextEdit
         layout disabled
         no rendering
```

## 7.4. Можно ли использовать только `QCoreApplication`

Текущая реализация Qt фактически делает значительную часть `QFont`/`QTextDocument` достаточно лениво, поэтому структурный `QTextDocument` без layout на практике может оказаться работоспособным и с:

```cpp
QCoreApplication
```

если никогда не обращаться к font metrics, shaping и rendering.

Но это **не следует считать гарантированным публичным контрактом Qt**, потому что документация `QFont` прямо требует `QGuiApplication`.

Поэтому варианты такие:

### Надёжный / поддерживаемый

```text
CLI
 ↓
QGuiApplication
 ↓
QTextDocument(layout OFF)
```

### Агрессивный headless optimization

```text
CLI
 ↓
QCoreApplication
 ↓
QTextDocument(layout OFF)
```

только если:

- этот режим действительно нужен;
- он покрыт regression/CI tests;
- поддерживаемые версии Qt фиксированы/контролируются;
- код не вызывает layout/font metrics/rendering.

По умолчанию предпочтителен первый вариант: стоимость `QGuiApplication` без окон значительно меньше стоимости поддержки второго полноценного Markdown IR.

---

# 8. Представление основных Markdown-конструкций

Ниже — предлагаемый mapping всех основных конструкций Zametti.

---

## 8.1. Обычный текст, пробелы, пустые строки и отступы

### Представление

```text
paragraph → QTextBlock
text      → characters
format    → QTextCharFormat runs
```

Пустой абзац естественно представлен пустым `QTextBlock`.

Несколько пробелов внутри структурной модели лучше хранить как реальные пробелы:

```text
"a   b"
```

а не превращать внутреннюю модель в Markdown-escaping representation.

Markdown writer уже отвечает за каноническую сериализацию whitespace.

### Важный принцип

Синтаксические отступы Markdown не должны автоматически превращаться в literal leading spaces.

Например:

```text
list nesting
blockquote nesting
continuation paragraph
```

лучше хранить как семантические свойства block'а.

---

## 8.2. Заголовки

Для heading semantics уже существует:

```cpp
QTextBlockFormat::setHeadingLevel(level);
```

Соответствие:

```text
headingLevel=1 → # ...
headingLevel=2 → ## ...
headingLevel=3 → ### ...
```

При сохранении writer всегда использует каноническую ATX-форму.

Нет необходимости хранить, был ли исходный заголовок записан через `##` или через Setext underline: эта информация удаляется во время canonical import.

---

## 8.3. Списки: ordered, unordered, task

Для полного контроля над fancy markers разумно не делать rendering зависимым от `QTextList`.

Каждый item можно представлять обычным `QTextBlock`:

```text
QTextBlock
    ListId
    ListKind
    ListLevel
    StartNumber   (при необходимости)
    TaskState     (для task list)
```

Например:

```text
Block "AAA": list=10 level=0 ordered
Block "xxx": list=11 level=1 unordered
Block "yyy": list=11 level=1 unordered
Block "BBB": list=10 level=0 ordered
```

### Custom marker

В начале list-item:

```text
U+FFFC
```

с:

```text
objectType = ListMarkerObject
```

Маркер рисуется через `QTextObjectInterface`.

Для правильного переноса строк используется hanging indent:

```text
      103.  Очень длинный текст элемента списка,
            который продолжается здесь.
```

### Нумерация ordered list

Номер — **derived state**, а не persistent semantic property.

Его можно вычислять лениво.

Для текущего блока:

1. получить block из `posInDocument`;
2. проверить cache по `blockNumber()`;
3. если cache пуст:
   - идти назад;
   - пропускать вложенные уровни;
   - остановиться на меньшем уровне или semantic boundary;
   - либо найти предыдущий sibling того же `ListId` с уже рассчитанным номером;
4. пройти обратно вперёд, заполняя cache до текущего item.

Пример:

```text
до:

... 99 100 101 ? ? ? [current] ...

после:

... 99 100 101 102 103 104 [105] ...
```

При последовательном использовании renderer'ом это даёт амортизированное:

```text
O(N)
```

для списка, а не `O(N²)`.

Кэш может быть просто:

```cpp
std::vector<ListCacheEntry>
```

с индексом по `blockNumber()`.

Лучше инвалидировать его по revision документа, а не на каждом repaint.

---

## 8.4. Inline formatting

Нативно представляется `QTextCharFormat`.

Например:

```text
bold
italic
strikethrough
inline code
highlight
foreground color
background color
```

становятся разными format runs (`QTextFragment`).

Специальный `U+FFFC` здесь не нужен.

Writer конвертирует semantic formatting в канонический Markdown:

```text
italic → _text_
strong → выбранная каноническая форма
...
```

Если некоторый Zametti-specific semantic style визуально совпадает со стандартным Qt format, но имеет другой смысл, его можно дополнительно отметить custom property.

---

## 8.5. Блоки кода

У Qt уже существуют Markdown-oriented block properties:

```text
BlockCodeFence
BlockCodeLanguage
```

Для Zametti имеет смысл также определить принадлежность последовательности блоков одному code block, например:

```text
CodeBlockId
```

Структура:

```text
Block #1: CodeBlockId=42, language=cpp
Block #2: CodeBlockId=42
Block #3: CodeBlockId=42
```

Конкретная длина исходного fence не нужна, поскольку writer всё равно создаёт канонический Markdown.

---

## 8.6. Изображения

Изображение следует разделять на:

```text
source image
    ≠
presentation size
```

Например:

```text
source:
3000 × 2000

document:
900 × 600
```

При resize документа меняется только presentation size. Оригинал остаётся нетронутым.

Для обычных изображений достаточно:

```text
QTextImageFormat
+
QTextDocument::ResourceProvider
```

### Image store / LRU cache

Рекомендуемая схема:

```text
QTextImageFormat
    URL / ImageId
    presentation width/height
             │
             ▼
       QTextDocument
             │
             ▼ resource request
         ImageStore
             │
        ┌────┴────┐
        │         │
       LRU      disk
```

`maxLoadedImageSize` ограничивает decoded working image:

```text
pixels <= maxLoadedImageSize²

width  <= 4 * maxLoadedImageSize
height <= 4 * maxLoadedImageSize
```

с сохранением aspect ratio.

При этом:

- original attachment не меняется;
- cache хранит уменьшенный decoded `QImage`;
- resizing в редакторе не является destructive resampling;
- при повторном увеличении используется доступная cached/source детализация;
- print/export при необходимости может использовать отдельную, более высокую resolution policy.

LRU лучше ограничивать по суммарному количеству байт, а не по числу изображений.

---

## 8.7. Таблицы

Markdown table естественно представляется:

```text
QTextTable
  ├── QTextTableCell
  │     └── QTextBlock...
  └── QTextTableCell
        └── QTextBlock...
```

Это особенно полезно, потому что содержимое ячейки остаётся нормальным rich text и сохраняет:

```text
cursor
selection
inline formatting
links
formulas
undo/redo
```

Markdown-specific свойства, которых нет в стандартной table model, можно хранить как custom properties.

Например:

```text
header semantics
column alignment
Zametti table style ID
```

Собственная визуальная отрисовка рамок/заливки может быть независима от Markdown semantics.

---

## 8.8. Формулы

Формула представляется custom inline object:

```text
U+FFFC
  QTextCharFormat:
      objectType = FormulaObject
      FormulaSource = "..."
      FormulaKind = Inline / Display
```

Один `QTextObjectInterface` регистрируется для `FormulaObject`.

### Inline formula

Находится внутри обычного paragraph и ведёт себя как атомарный glyph-like object.

### Display formula

Находится в отдельном `QTextBlock`.

Например:

```text
paragraph

        [ formula ]

paragraph
```

Блок может иметь:

```cpp
alignment = Qt::AlignHCenter
```

либо custom formula object может занимать всю content width и сам центрировать формулу внутри своей области.

Semantic source формулы должен храниться в char-format property, чтобы он участвовал в undo/redo.

---

## 8.9. Гиперссылки

Штатно используются свойства `QTextCharFormat`:

```cpp
setAnchor(true);
setAnchorHref(...);
```

То есть Markdown:

```markdown
[text](target)
```

превращается в semantic pair:

```text
display text + href
```

Reference-link vs inline-link форму хранить не требуется, потому что canonical writer имеет собственное правило сериализации.

---

## 8.10. Сноски

У `QTextDocument` нет отдельного высокоуровневого `QTextFootnote`, поэтому требуется Zametti-specific semantic convention.

Вариант:

### Reference

```text
U+FFFC
    objectType = FootnoteReferenceObject
    FootnoteId = "foo"
```

или специальный char-format run с `FootnoteId`.

### Definition

```text
QTextBlock
    BlockKind = FootnoteDefinition
    FootnoteId = "foo"
```

Для многоабзацной footnote несколько блоков имеют один и тот же `FootnoteId` или дополнительный structural ID.

Writer собирает references/definitions обратно в канонический Markdown.

---

## 8.11. Block quote

Qt уже имеет Markdown-oriented property:

```text
BlockQuoteLevel
```

Поэтому:

```markdown
> foo
>> bar
```

можно представить как:

```text
Block "foo": quoteLevel=1
Block "bar": quoteLevel=2
```

Это хорошо соответствует плоской block-модели документа.

---

# 9. Сводная таблица mapping

| Markdown-сущность | `QTextDocument` representation | Custom metadata нужна? |
|---|---|---|
| Обычный текст | `QTextBlock` + characters | обычно нет |
| Пустые строки | пустой `QTextBlock` | нет |
| Заголовки | `QTextBlockFormat::headingLevel` | обычно нет |
| Ordered list | `QTextBlock` | `ListId`, `ListKind`, `ListLevel` |
| Unordered list | `QTextBlock` | `ListId`, `ListKind`, `ListLevel` |
| Task list | `QTextBlock` | + `TaskState` |
| Bold/italic/etc. | `QTextCharFormat` | только для Zametti-specific semantics |
| Inline code | `QTextCharFormat` | при необходимости |
| Code block | blocks + code properties | возможно `CodeBlockId` |
| Image | `QTextImageFormat` | ImageId / placement policy при необходимости |
| Table | `QTextTable` | Markdown/style-specific properties |
| Inline formula | `U+FFFC` + custom object | `FormulaSource`, kind |
| Display formula | отдельный block + custom object | `FormulaSource`, kind |
| Hyperlink | `QTextCharFormat` anchor/href | обычно нет |
| Footnote | custom reference + definition blocks | да |
| Block quote | block quote level | обычно нет |

---

# 10. Undo/redo

Одно из существенных преимуществ `QTextDocument` как единственного live model — семантическое состояние может участвовать в том же undo stack, что и текст.

Undoable:

```text
character insertion/removal
block insertion/removal
QTextCharFormat changes
QTextBlockFormat changes
custom QTextFormat properties
```

Следовательно, например:

```text
formula source
list kind
heading level
image presentation size
task checked state
```

можно изменять как format state и получать штатный undo/redo.

Runtime cache в `QTextBlockUserData` в undo stack не входит.

---

# 11. Изменение файла снаружи

Внешнее изменение `.md` можно представить как один пользовательский edit:

```text
disk changed
    ↓
parse external version
    ↓
compute diff
    ↓
apply QTextCursor edits
inside beginEditBlock()/endEditBlock()
```

То есть для пользователя это аналог:

```text
Ctrl+A
Ctrl+V
```

но при небольшом изменении не требуется заменять весь buffer.

Лучший вариант:

```text
old document
    ↓ diff
small set of text changes
    ↓
apply from right to left
    ↓
one QTextDocument undo command
```

Это лучше сохраняет существующие blocks, runtime metadata и layout state.

Full replacement остаётся fallback для радикально изменившегося файла.

---

# 12. CLI utilities

После отказа от собственного IR CLI работает с тем же `ZDocument`.

```text
CLI
 │
 ├── md4c parser
 ├── ZDocument
 │      └── QTextDocument(layout OFF)
 │
 └── Markdown writer / transforms
```

CLI не нужны:

```text
QTextEdit
QWidget
window
painting
text layout
```

но по официальному Qt contract безопаснее всё равно запускать процесс с:

```text
QGuiApplication
```

поскольку `QTextDocument`/`QTextFormat` относятся к QtGui и используют `QFont`.

Это не означает создание GUI или окон.

---

# 13. Desktop и Android

`QTextDocument` следует считать частью document engine, а не конкретного desktop UI.

Архитектурно:

```text
                    ZDocument
                          │
                    QTextDocument
                    /             \
                   /               \
             Desktop             Android
                │                   │
            QTextEdit         Qt Quick/TextEdit
```

Таким образом UI можно менять независимо от document model.

Важно не протаскивать в core зависимости вида:

```text
QTextEdit*
QWidget*
QMouseEvent*
QPaintEvent*
QScrollBar*
```

В core допустима сознательная зависимость на тяжёлую text/document инфраструктуру Qt:

```text
QTextDocument
QTextCursor
QTextBlock
QTextFormat
QTextTable
QTextObjectInterface
QFont
```

---

# 14. Что остаётся вне `QTextDocument`

Не всё состояние Zametti следует помещать внутрь документа.

```text
ZDocument
│
├── NoteMetadata
│     id
│     sync metadata
│     file state
│
├── QTextDocument
│     semantic editable document
│
├── ImageStore / LRU
│
├── runtime caches
│
└── external history/sync state
```

На диске:

```text
<14-digit-id>.md
attachments/
history (zstd + delta)
```

`QTextDocument` никогда не является persistent serialization format.

---

# 15. Основные риски

## 15.1. Зависимость `zametti-core` от QtGui

Это главный архитектурный trade-off.

Плюс:

- исчезает второй mutable document model;
- исчезает постоянная IR ↔ QTextDocument conversion;
- один undoable state;
- готовые tables/blocks/formats/cursors;
- один model для GUI и CLI.

Минус:

- core больше нельзя считать Qt-independent;
- даже headless tools линкуются с QtGui;
- по официальному контракту `QFont` предполагает `QGuiApplication`.

Для текущей архитектуры Zametti это выглядит приемлемой ценой.

## 15.2. Не использовать private Qt API

Core должен опираться на public API:

```text
QTextDocument
QTextCursor
QTextBlock
QTextFormat
QTextObjectInterface
...
```

и не зависеть от:

```text
private/qtextdocument_p.h
private layout internals
QPA private APIs
```

## 15.3. Не превращать custom properties в второй скрытый IR

Custom properties должны хранить только настоящую семантику документа.

Не нужно сохранять там:

```text
какой именно Markdown delimiter использовался
какой bullet был в исходном imported file
какая длина fence была у внешнего файла
```

если canonical writer всё равно выбирает единственный вариант.

---

# 16. Предлагаемый migration path

Переход от существующего IR можно делать постепенно.

### Этап 1

Определить полный semantic mapping:

```text
IR block/span/object
    ↔
QTextBlock/QTextCharFormat/QTextTable/custom object
```

### Этап 2

Перенести Markdown writer на непосредственный обход `QTextDocument`.

### Этап 3

Перенести command-line transforms с IR на `ZDocument`.

### Этап 4

Использовать существующие проверки round-trip:

```text
parse(write(parse(x))) == parse(x)
```

для полного corpus заметок.

### Этап 5

На переходном этапе оставить IR только как test oracle:

```text
md4c → old IR
md4c → QTextDocument

compare semantics
```

### Этап 6

После стабилизации удалить persistent IR path.

---

# 17. Итоговая архитектура

```text
                         DISK
                          │
        ┌─────────────────┼─────────────────┐
        │                 │                 │
   canonical .md      attachments       history
        │                                   │
        │ md4c                              │
        ▼                                   │
┌──────────────────────────────────────────────┐
│               ZDocument                │
│                                              │
│  NoteMetadata                                │
│                                              │
│  QTextDocument                               │
│   ├── blocks                                 │
│   ├── char/block formats                     │
│   ├── custom semantic properties             │
│   ├── tables                                 │
│   └── custom inline objects                  │
│                                              │
│  ImageStore / LRU                            │
│  list numbering cache                        │
│  other runtime caches                        │
└───────────────────┬──────────────────────────┘
                    │
          ┌─────────┴─────────┐
          │                   │
          ▼                   ▼
       Desktop               CLI
     QTextEdit        QTextDocument layout OFF
          │
          ▼
       Android
       Qt Quick
```

Главный принцип:

> **Markdown является каноническим persistent format, а `QTextDocument` — единственным каноническим live/editing representation.**

Отдельный IR оправдан только в том случае, если существует реальная необходимость использовать полноценную модель Zametti без зависимости от QtGui.

Если такой необходимости нет, второй mutable document model создаёт больше сложности, чем архитектурной ценности.

---

# 18. Qt API, на которые опирается дизайн

Ключевые публичные API:

```text
QTextDocument
QTextCursor
QTextBlock
QTextFragment
QTextCharFormat
QTextBlockFormat
QTextBlockUserData
QTextFormat::UserProperty

QTextTable
QTextFrame

QTextImageFormat
QTextDocument::ResourceProvider

QTextObjectInterface
QTextFormat::UserObject
QChar::ObjectReplacementCharacter

QTextDocument::setLayoutEnabled()
QTextDocument::contentsChange()
QTextDocument::blockCountChanged()

QFont
QGuiApplication
```

Полезные официальные страницы Qt:

- `QTextDocument`: https://doc.qt.io/qt-6/qtextdocument.html
- Rich Text Processing: https://doc.qt.io/qt-6/richtext.html
- `QTextFormat`: https://doc.qt.io/qt-6/qtextformat.html
- `QTextBlockFormat`: https://doc.qt.io/qt-6/qtextblockformat.html
- `QTextCharFormat`: https://doc.qt.io/qt-6/qtextcharformat.html
- `QTextObjectInterface`: https://doc.qt.io/qt-6/qtextobjectinterface.html
- `QTextImageFormat`: https://doc.qt.io/qt-6/qtextimageformat.html
- `QTextTable`: https://doc.qt.io/qt-6/qtexttable.html
- `QFont`: https://doc.qt.io/qt-6/qfont.html
- `QGuiApplication`: https://doc.qt.io/qt-6/qguiapplication.html
