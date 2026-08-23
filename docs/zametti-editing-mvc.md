# Редактирование как операции над ZDocument

## Вопрос

какая правильная модель редактирования документа? Вот у меня есть класс, который инкапсулирует QTextDocument. Нажимаю кнопочки, например '*' SPACE в начале строки. Хочу это распознать и начать новый список. Как это должно происходить? Видимо это приходит в QTextEdit, возможно нужно перехватить вставку пробела, понять что у нас * перед ним и мы в начале строки, и создать новый блок со списком. Или нажали ENTER, определили что мы уже в списке, значит надо добавить еще один элемент списка. Как это правильно сделать с т.з. модели MVC и нашей текущей архитектуры?

## Ответ

Да. Я бы здесь сделал именно **controller/editor-command слой между `QTextEdit` и `ZDocument`**. Причём довольно тонкий: не надо строить вокруг Qt академический MVC.

Схема примерно такая:

```text
              raw user input
                   │
                   ▼
          QTextEdit
          keyPressEvent()
          inputMethodEvent()
                   │
                   ▼
          EditingController
        "что хотел сделать user?"
                   │
          ┌────────┴────────┐
          │                 │
          ▼                 ▼
   обычный ввод       semantic command
          │                 │
          ▼                 ▼
 QTextEdit default    ZDocument
 behaviour            edit operations
                            │
                            ▼
                       QTextDocument
```

### Что относится к какому слою

`ZDocument` должен знать:

```cpp
bool isListItem(const QTextBlock&) const;
ListInfo listInfo(const QTextBlock&) const;

void convertToListItem(QTextCursor&, ListKind);
void splitListItem(QTextCursor&);
void exitList(QTextCursor&);
void indentListItem(QTextCursor&);
void outdentListItem(QTextCursor&);
```

Но **не должен знать**, что `convertToListItem()` был вызван потому, что человек напечатал `* `.

Это важно, потому что ту же операцию могут вызвать:

```text
"* " в начале строки
toolbar button
keyboard shortcut
menu command
Markdown paste/import
```

и все должны в итоге использовать один и тот же код изменения модели.

---

## `* SPACE`: я бы перехватывал Space до его обычной обработки

Пользователь нажал `*`.

Ничего специального:

```text
QKeyEvent('*')
    ↓
QTextEdit::keyPressEvent()
    ↓
в QTextDocument появился '*'
```

Затем нажимает Space.

Вот здесь controller смотрит на состояние **до вставки пробела**:

```cpp
void QTextEdit::keyPressEvent(QKeyEvent *e)
{
    if (controller_.handleKeyPress(e))
        return;

    QTextEdit::keyPressEvent(e);
}
```

Условно:

```cpp
bool EditingController::handleKeyPress(QKeyEvent *e)
{
    if (e->key() == Qt::Key_Space) {
        QTextCursor c = editor_->textCursor();

        if (isUnorderedListPrefix(c)) {
            document_->convertToListItem(c, ListKind::Unordered);
            editor_->setTextCursor(c);
            return true; // Space больше QTextEdit не отдаём
        }
    }

    return false;
}
```

`isUnorderedListPrefix()` проверяет примерно:

```text
current block
cursor at position 1
text before cursor == "*"
```

или:

```text
"*"
"-"
"+"
```

в зависимости от того, какие shortcuts ты хочешь поддерживать.

### Причём новый block создавать здесь вообще не надо

До ввода:

```text
|
```

после `*`:

```text
*|
```

после распознавания Space текущий обычный block просто **превращается в list item**:

```text
• |
```

То есть semantic operation:

```text
Paragraph block
    ↓
remove typed '*'
    ↓
set ListId/ListKind/ListLevel/...
    ↓
insert/render ListMarkerObject
    ↓
ListItem block
```

Это очень естественно ложится на нашу модель `QTextDocument`.

---

# Enter я бы тоже перехватывал до стандартного `QTextEdit`

Если block обычный:

```text
hello|
```

controller ничего не делает:

```cpp
return false;
```

и стандартный `QTextEdit` создаёт новый paragraph.

Но если:

```text
• hello|
```

controller видит:

```cpp
document_->isListItem(cursor.block())
```

и вместо стандартного Enter вызывает:

```cpp
document_->splitListItem(cursor);
```

Результат:

```text
• hello
• |
```

Причём именно `ZDocument::splitListItem()` должен знать детали твоего внутреннего представления:

```text
ListId
ListLevel
ListKind
Start
TaskState
U+FFFC marker
block formatting
```

Controller этого знать не должен.

---

## И здесь появляется приятная вещь

Если Enter на пустом item:

```text
• |
```

то controller может решить:

```text
empty list item + Enter
        ↓
exitList()
```

и получить:

```text
|
```

Для вложенного списка политика может быть другой:

```text
    • |
Enter
    ↓
outdent first

• |
```

и только ещё один Enter завершает список.

Это уже **editing policy**.

Я бы даже не зашивал эту policy в `keyPressEvent`, а сделал semantic operation вроде:

```cpp
document_->breakParagraph(cursor);
```

или чуть точнее:

```cpp
document_->insertParagraphBreak(cursor);
```

которая сама знает:

```text
normal paragraph   → split paragraph
non-empty list     → split list item
empty nested list  → outdent
empty top list     → exit list
code block         → newline inside code
...
```

Тогда Enter превращается просто в:

```cpp
if (document_->needsCustomParagraphBreak(c)) {
    document_->insertParagraphBreak(c);
    return true;
}
```

Но я бы не делал `ZDocument::handleEnter()` — это уже имя, привязанное к UI. `insertParagraphBreak()` остаётся нормальной document-edit operation.

---

# Я бы вообще сформировал небольшой набор editor primitives

Не обязательно классическую Command hierarchy. Просто методы:

```cpp
class ZDocument {
public:
    // queries
    BlockKind blockKind(const QTextBlock&) const;
    std::optional<ListInfo> listInfo(const QTextBlock&) const;

    // structural edits
    void makeUnorderedList(QTextCursor&);
    void makeOrderedList(QTextCursor&, int start = 1);
    void removeList(QTextCursor&);

    void insertParagraphBreak(QTextCursor&);
    void indentListItem(QTextCursor&);
    void outdentListItem(QTextCursor&);

    void toggleBold(QTextCursor&);
    void toggleItalic(QTextCursor&);
    void toggleCode(QTextCursor&);
};
```

Тогда:

```text
Toolbar "bullet list" ────────┐
                              │
typing "* " ──────────────────┼─→ makeUnorderedList()
                              │
keyboard shortcut ────────────┘
```

Получается одна реализация semantics.

Я бы **не вводил для этого `QUndoCommand`/`QUndoStack`**, потому что у `QTextDocument` уже есть собственный undo stack. Второй command stack принесёт больше проблем, чем пользы.

---

# Undo для autoformat

Здесь есть интересный нюанс.

Когда пользователь вводит:

```text
*
```

этот `*` уже попадает в undo history стандартного `QTextEdit`.

На следующем Space ты делаешь:

```text
remove '*'
set list properties
insert marker
```

Это можно оформить:

```cpp
cursor.beginEditBlock();
...
cursor.endEditBlock();
```

чтобы **сама трансформация** была одной undo operation.

Тогда возможное поведение:

```text
typed "* "
    ↓
becomes bullet

Ctrl+Z
    ↓
returns "*"
```

Это, кстати, довольно хорошее поведение: первый Undo отменяет autoformat, а не обязательно весь ввод.

Если хочется, чтобы autoformat склеился с предыдущим вводом, у `QTextCursor` есть механизм `joinPreviousEditBlock()`, но я бы сначала не усложнял. Поведение:

```text
• hello

Ctrl+Z → *hello / исходный prefix
```

может быть даже более полезным пользователю.

---

# Почему не стоит распознавать это через `contentsChange()`

Технически можно:

```text
QTextDocument::contentsChange()
        ↓
увидели, что появилось "* "
        ↓
переформатировали block
```

Но я бы **не делал это основным механизмом интерактивного editing**.

Потому что сразу возникают:

```text
reentrancy
recursive contentsChange
undo grouping
cursor restoration
temporary "* " на экране
отличие user input от программного изменения
paste
external document replacement
```

Гораздо чище:

```text
input event
    ↓
recognize intent
    ↓
perform semantic edit
```

а `contentsChange()` оставить для вещей вроде:

```text
cache invalidation
dirty flag
search index
list-number cache revision
external observers
```

---

# Но есть Android/IME нюанс

Вот тут я бы сразу оставил архитектурную дверцу.

На desktop:

```cpp
keyPressEvent()
```

для `*`, Space, Enter работает прекрасно.

Но soft keyboard на Android и сложные IME могут вводить текст через:

```cpp
inputMethodEvent()
```

а не как красивую последовательность `QKeyEvent`.

Поэтому я бы не делал сам алгоритм распознавания частью `keyPressEvent()`.

Например:

```cpp
class EditingController {
public:
    bool insertText(QStringView text);
    bool paragraphBreak();
    bool backspace();
    bool tab();
};
```

Тогда frontend только преобразует platform input:

```text
desktop QKeyEvent
        │
        ▼
 EditingController
        ▲
        │
Android / IME commit
```

То есть сама логика:

```text
"перед вставкой пробела проверить prefix"
```

общая.

Для первой desktop реализации, конечно, можно просто начать с `keyPressEvent()`.

---

# Я бы разделил ещё одну вещь

Есть **source-like shortcuts**:

````text
* + Space       → unordered list
1. + Space      → ordered list
# + Space       → heading
> + Space       → blockquote
``` ` + ...       → code
````

И есть **structural editing**:

```text
Enter           → continue structure
Backspace       → remove/outdent structure
Tab             → indent
Shift+Tab       → outdent
```

Это два разных кусочка controller logic:

```text
EditingController
    │
    ├── AutoFormatRecognizer
    │       "* "
    │       "1. "
    │       "# "
    │       "> "
    │
    └── StructuralEditing
            Enter
            Backspace
            Tab
```

Не обязательно даже делать отдельные классы — просто концептуально полезно разделять.

---

## В результате я бы для Zametti сделал примерно так

```cpp
class ZamettiEditor : public QTextEdit
{
protected:
    void keyPressEvent(QKeyEvent *) override;
    void inputMethodEvent(QInputMethodEvent *) override;

private:
    EditingController controller_;
};
```

```cpp
class EditingController
{
public:
    bool handleKeyPress(QKeyEvent *);
    bool handleTextInput(QStringView);
    bool handleParagraphBreak();

private:
    ZamettiEditor *view_;
    ZDocument *document_;
};
```

и:

```cpp
class ZDocument
{
public:
    QTextDocument &textDocument();

    // semantic queries
    bool isListItem(QTextBlock) const;
    BlockKind blockKind(QTextBlock) const;

    // atomic semantic editing operations
    void convertToListItem(QTextCursor&, ListKind);
    void insertParagraphBreak(QTextCursor&);
    void indentListItem(QTextCursor&);
    void outdentListItem(QTextCursor&);
    ...
};
```

При этом `ZDocument` остаётся полностью **headless**: никакого `QTextEdit`, `QKeyEvent`, QWidget или desktop-specific кода. И те же document operations можно будет использовать из QML/Android frontend.

Самое главное правило я бы сформулировал так:

> **View сообщает о пользовательском вводе; controller распознаёт editing intent; `ZDocument` выполняет структурное изменение через `QTextCursor`.**

Для нашей нынешней архитектуры это, по-моему, очень естественная граница.
