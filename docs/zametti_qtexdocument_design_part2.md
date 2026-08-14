# Дизайн ZDocument как обертки над QTextDocument

## Вопрос

Всё таки появляется риск рассинхрона, когда ZamettiDocument может содержать еще какие-то данные, которые разойдутся с содержимым QTextDocument, тот же кэш номеров для нумерованных списков. Разве что каким-то образом перехватывать любое редактирование QTextDocument и инвалидировать кэш.

## Ответ

Да. И я бы именно так и строил `ZamettiDocument`: **`QTextDocument` — единственное изменяемое semantic state; всё остальное либо независимые metadata/config, либо полностью disposable derived cache.**

Тогда наличие кэша само по себе не создаёт второй модели. Инвариант должен быть:

```text
listNumberCache = f(QTextDocument)
```

а не:

```text
QTextDocument  ←→  listNumberCache
       два равноправных состояния
```

То есть кэш всегда можно в любой момент выбросить и восстановить из документа.

### И да — подписаться на `contentsChange()`

Я бы прямо в конструкторе `ZamettiDocument` сделал:

```cpp
ZamettiDocument::ZamettiDocument()
{
    connect(&document_, &QTextDocument::contentsChange,
            this,
            [this](int pos, int removed, int added)
            {
                onDocumentChanged(pos, removed, added);
            });

    connect(&document_, &QTextDocument::blockCountChanged,
            this,
            [this](int count)
            {
                listNumberCache_.resize(count);
            });
}
```

Это ловит изменения независимо от того, откуда они пришли:

```text
QTextEdit
QTextCursor
ZamettiDocument methods
paste
undo
redo
external replacement
```

Поскольку все они в конечном итоге меняют **тот же `QTextDocument`**.

И что особенно важно, документация Qt прямо говорит, что `contentsChange()` испускается и при изменении форматирования, не только текста. Поэтому наши semantic properties в `QTextBlockFormat::UserProperty` тоже покрываются этим механизмом. ([Qt Documentation][1])

---

## Я бы даже сделал это фундаментальным правилом

```cpp
class ZamettiDocument
{
    QTextDocument document_;       // SOURCE OF TRUTH

    ListNumberCache listCache_;    // derived
    ImageCache imageCache_;        // derived/external resources
    // ...
};
```

Причём:

```cpp
void ZamettiDocument::clearDerivedState()
{
    listCache_.clear();
    ...
}
```

должен всегда оставлять объект в совершенно корректном состоянии.

Если после:

```cpp
clearDerivedState();
```

ZamettiDocument перестал работать — значит в cache случайно попала semantic information, которой там быть не должно.

Это хороший тест архитектуры.

---

# Но с list cache есть нюанс производительности

Самый простой вариант:

```cpp
void onDocumentChanged(...)
{
    ++cacheRevision_;
}
```

и все cached numbers считаются stale.

Корректность идеальная.

Но представь список из 100 000 элементов, курсор в последнем:

```text
1.
2.
...
99999. foo|
100000.
```

нажал `x`.

Если каждое изменение обычного текста инвалидирует **всю нумерацию**, следующий repaint номера `100000` потенциально заставит опять идти через весь список.

Это уже глупо, потому что:

```text
"foo" → "foox"
```

никак не меняет numbering.

Поэтому я бы разделил **correctness mechanism** и последующую оптимизацию.

---

## Первая реализация — вообще можно инвалидировать всё

Сначала:

```cpp
connect(&document_, &QTextDocument::contentsChange,
        this, [this](...) {
            listCache_.invalidate();
        });
```

И посмотреть на реальные размеры документов.

Учитывая ленивое заполнение твоего cache, вполне возможно, этого будет достаточно.

Если нет — оптимизировать уже очень легко, потому что архитектура правильная.

---

# Более хороший вариант: structural revision

У нас ведь numbering зависит не от текста:

```text
"купить молоко"
```

а только от структуры:

```text
ListId
ListKind
ListLevel
Start
положение block'ов
```

Поэтому концептуально есть две revision:

```cpp
uint64_t documentRevision_;
uint64_t listStructureRevision_;
```

Обычная печать:

```text
hello → hellox

documentRevision++
listStructureRevision stays
```

Структурная операция:

```text
Paragraph → ListItem
ListLevel 1 → 2
insert/remove list block
```

делает:

```text
documentRevision++
listStructureRevision++
```

Тогда:

```cpp
struct ListCacheEntry {
    uint64_t structuralRevision;
    int ordinal;
};
```

---

## Откуда брать structural revision?

Вот здесь есть два уровня защиты.

Наши собственные операции точно знают, что произошло:

```cpp
void ZamettiDocument::convertToListItem(...)
{
    ...
    invalidateListStructure();
}

void ZamettiDocument::indentListItem(...)
{
    ...
    invalidateListStructure();
}
```

А `contentsChange()` остаётся **страховочной сеткой** для изменений, пришедших напрямую через `QTextEdit`.

Например обычная вставка символов сама по себе numbering не меняет.

Но Enter может изменить число blocks; Qt даёт отдельный `blockCountChanged()`. ([Qt Documentation][1])

Получается:

```text
contentsChange
       │
       ├── generic caches/dirty state
       │
       └── возможно conservative invalidation

blockCountChanged
       │
       └── list structural cache definitely affected

our semantic list commands
       │
       └── list structural cache definitely affected
```

---

# Но я бы не стал слишком рано это усложнять

Есть гораздо более простой robust вариант:

```text
любое contentsChange
        ↓
list cache marked dirty FROM changed block
```

Не весь cache.

Допустим:

```text
1. aaa        cached 1
2. bbb        cached 2
3. ccc        cached 3
4. ddd        cached 4
5. eee        cached 5
```

изменился третий block:

```text
1. aaa        cached 1
2. bbb        cached 2
3. xxx        DIRTY
4. ddd        DIRTY
5. eee        DIRTY
```

При запросе номера пятого:

```text
нашли предыдущий valid = 2
↓
3
4
5
```

То есть ровно тот алгоритм, который мы уже обсуждали.

А если оказалось, что изменение было всего лишь:

```text
ccc → cccd
```

мы зря пересчитали три числа — ничего страшного.

Зато нет O(N)-очистки cache.

---

# Ещё одна причина хранить semantics в `QTextFormat::UserProperty`

Вот здесь наше прежнее решение становится особенно важным.

Если сделать:

```cpp
blockFormat.setProperty(ListId, ...);
blockFormat.setProperty(ListLevel, ...);
```

то это часть `QTextDocument`, и изменение форматирования приводит к `contentsChange()`. ([Qt Documentation][1])

Если бы мы положили semantic state в:

```cpp
QTextBlockUserData
```

то это уже отдельное состояние вне содержимого `QTextDocument`; обычные document edit/undo механизмы его нормально не восстанавливают.

Поэтому:

```text
QTextBlockFormat::UserProperty
    → semantics

QTextBlockUserData
    → disposable cache only
```

становится практически обязательным правилом.

---

## Кстати, сам cache можно положить в `QTextBlockUserData`

Это тоже вариант, и в каком-то смысле даже красивый:

```cpp
struct ZamettiBlockCache : QTextBlockUserData
{
    int listNumber = -1;
    uint64_t listRevision = 0;
};
```

Плюс такого решения: cache физически прикреплён к block, поэтому не надо синхронизировать:

```text
vector index ↔ blockNumber()
```

при вставке blocks.

Если block исчез:

```text
cache исчез
```

если появился новый:

```text
cache отсутствует → вычисляем
```

Undo не восстановил userData?

Отлично — это cache, его и **не надо** восстанавливать.

Я сейчас даже склоняюсь к этому варианту больше, чем к нашему первоначальному:

```cpp
std::vector<CacheEntry>[blockNumber]
```

потому что исчезает целый класс bookkeeping при insert/delete blocks.

Можно иметь:

```cpp
class ZamettiBlockData : public QTextBlockUserData
{
public:
    // ONLY DERIVED DATA

    int listNumber = -1;
    uint64_t listGeneration = 0;

    // возможно другие block-local caches
};
```

И принцип остаётся железным:

> **Удаление всех `ZamettiBlockData` никак не меняет смысл документа.**

---

# В итоге я бы сформулировал архитектуру так

```text
                         source of truth
                               │
                               ▼
                     ┌───────────────────┐
                     │   QTextDocument   │
                     │                   │
                     │ text              │
                     │ block semantics   │
                     │ inline semantics  │
                     │ tables/images/... │
                     └─────────┬─────────┘
                               │
                        contentsChange()
                               │
                ┌──────────────┼──────────────┐
                ▼              ▼              ▼
          list-number      search/index    rendering
             cache            cache          cache
                │              │              │
                └──────── disposable ─────────┘
```

И `ZamettiDocument` в этом смысле не вторая модель, а скорее:

```text
QTextDocument
    +
semantic operations
    +
metadata
    +
derived caches
    +
resource management
```

### Я бы ввёл очень простой архитектурный invariant

**Никогда не хранить в поле `ZamettiDocument` то, что можно считать semantic свойством конкретного block/fragment/table и что может изменяться при редактировании.**

Такое значение должно жить **в `QTextDocument`**.

В полях wrapper могут жить:

```text
note metadata             // отдельная сущность
image store / resource IDs
style configuration
zoom/view state           // если wrapper вообще должен его знать
derived caches
```

Но не:

```text
vector<ListItem> lists;
map<BlockId, HeadingLevel> headings;
vector<Formula> formulas;
```

если всё это уже представлено внутри `QTextDocument`.

Тогда рассинхрон действительно почти конструктивно исключается: единственное, что может устареть, — cache, а его всегда безопасно выбросить и пересчитать.

[1]: https://doc.qt.io/qt-6/qtextdocument.html?utm_source=chatgpt.com "QTextDocument Class | Qt GUI | Qt 6.11.1"
