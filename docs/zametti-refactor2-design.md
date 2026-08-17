# Рефакторинг 2 — дизайн: классы, матрицы взаимодействия, аудит состояния

Документ для обсуждения ДО кода (правило проекта: матрицы взаимодействия
пишутся на бумаге). Собран 17.08.2026 по аудиту кода на ветке `refactor2`.
Цель владельца названа: настоящие классы с protected-полями, каждый из которых
сам держит свою консистентность; критерий пригодности архитектуры — **две
заметки в split-view или во вкладках без переделок**.

---

## 1. Аудит: где сегодня живёт состояние

### 1.1. Глобальное и статическое

| что | где | чьё по смыслу | замечание |
|---|---|---|---|
| `Appearance g_appearance` + `appearance()` (НЕКОНСТАНТНАЯ ссылка) | `settings.cpp:26,634` | настройки | 237 мест чтения в app+core, 110 в тестах; писать может кто угодно — договор «меняется только при загрузке конфига» ничем не защищён; тесты пишут поля напрямую в 15 местах |
| `g_loadedImageSizeLimit` | `settings.cpp:851` | кэш картинок | ленивый вывод из настроек, читает только `note_view.cpp` |
| `g_imageDecodes`, `g_imageDecodeMicros` | `note_view.cpp:513` | кэш картинок (телеметрия) | глобальны, хотя сам кэш — поля экземпляра `NoteView`: два уровня жизни у одной сущности |
| `g_cache` иконок | `icons.cpp:28` | ресурсы приложения | законный кэш приложения, но глобальный |
| `g_active` (`ImageImporter*`) | `image_importer.cpp:13` | сессия правки | «кто сейчас везёт картинки» — глобальный флаг для `editingAllowed()` |
| `g_ready/g_renders/g_mathFont`, байты шрифта, `engineLock` | `formula.cpp` | движок формул | глобальный контекст microtex — оправдано, но должно быть одним объектом `FormulaEngine` |
| `metricsOf` статики | `marker.cpp:91` | вид (мемоизация) | зависит от `appearance()` |
| замок журналов `gate()` | `journal.cpp:26` | хранилище | один на процесс — правильно, но должен принадлежать объекту хранилища |
| `storeLock` (`QLockFile`), `about` (`QPointer`) | `main.cpp:413, 2585` | хранилище / окно | |
| `Session` (`loadSession/saveSession`) | `settings.h:920` | смесь: окно (геометрия, сплиттер), приложение (`storeRoot`, `exportDir`, история поиска), дерево (`treeSort`, `expandedDirs`), сессия правки (`lastFile`, `caret`, `anchor`, `zoom`) | одна структура на четыре владельца |

### 1.2. `NoteEditor` — 139 обращений к `note_.`, 151 к `current_.`

`NoteSession` (21 поле) по владельцам: **журнал 5** (`journalTail`, `journalTailTime`,
`journalTailKnown`, `journalCompressed`, `restoreSource`), хранилище 3 (`path`,
`digest`, `lastSaved`), заметка 3 (`note`, `meta`, `stats`), сессия правки 4
(`cursor`, `anchor`, `scroll`, `modified`), вид 2 (`built`, `builtValid`), кэш 1
(`bytes`), диагностика 3.

`CurrentNoteState` (31 поле): **режим истории/разности 17** (`live`, `timeline`,
`historyIndex`, `snapshot`, `base`, тексты, два `diff::Result`, четыре документа
разности с метками, `diffSlot`), поиск 4 (`matches`, `currentMatch`,
`matchText`, `matchCaseSensitive`), внешняя правка 3, сессия правки 6.

Плюс поля самого виджета: `storeRoot_` (13 обращений — хранилище внутри
виджета), `watcher_`/`externalSettle_`/`autosave_` (хранилище), `caretMemory_`
(память о каретке ВСЕХ заметок сеанса — приложение), `noteCache_` (кэш заметок
приложения), `diffPlainView_/diffPeek_/diffFromFresh_/diffMarks_/diffSource_`
(дубли `current_`), привязки клавиш (снимок настроек), `importer_`/`importedBatch_`.

`journal::History history(storeRoot_)` создаётся **на каждый вызов** в шести
местах редактора: объекта хранилища не существует нигде.

### 1.3. `NoteView` — три уровня жизни в одном объекте

Кадр (`exportRatio_`, `imageDirty_`), открытая заметка (`imageBase_`,
`currentNoteImages_`, `tables_`, `formulas_`), вся сессия приложения
(`imageCache_`, `imageOrder_`, `imageCacheBytes_` + глобальные счётчики).

### 1.4. `main()` — окно без класса, операции хранилища россыпью лямбд

Объекты на стеке `main` (окно, сплиттер, тулбар, статус, дерево, редактор,
список, модель дерева, панель поиска, история, результаты, фоновый поиск,
сторожа конфига и хранилища, `KeyWalk`, `NewNoteGrab`) связаны ~40 лямбдами,
захватывающими друг друга по ссылке. Состояние окна — локали `main`:
`rootSort`, `revealing`, `openedByFolderPick`, `syncSortToSelection`,
`lastHistoryIndex`, `storeNames`, `exportDir/exportKeepMeta`, `keptSizes`;
раскрытые ветки нигде не хранятся и вычисляются обходом представления.

Операции хранилища по слоям:

* **только ядро** (`store/*`): создать, импортировать, архивировать, вернуть,
  забыть, удалить файл, вложения, бюро находок, журнал, проверка;
* **только лямбды `main.cpp`**, без единого теста: переименовать, перенести,
  создать папку, пометить сортировку, перечитать хранилище, восстановить цепочку
  папок, вывоз — и общий примитив записи `rewriteNote` (`main.cpp:1305`:
  `readFile` + `ZDocument` + `std::ofstream`, **без QSaveFile и без журнала** —
  это обходная запись на диск, которой по правилам проекта не существует);
* **раздвоено**: переименовать / перенести / пометить сортировку имеют по две
  ветки — «заметка открыта» (через редактор) и «закрыта» (через `rewriteNote`),
  и различие проверяется `file == editor.filePath()` в четырёх местах.

`NoteTreeModel` — единственный читатель диска для корпуса и единственный кэш
заголовков (`Node`: title, snippet, modified, created, sortMark, archived…);
файлы не пишет никогда (правильно), но хранит и состояние представления
(`expanded_`). `NoteListModel` — чистая проекция дерева.

**Вывод аудита.** Объекта «хранилище» нет — его роль делят `NoteTreeModel`
(чтение), лямбды `main()` (запись) и `NoteEditor` (журнал, сторож файла,
автосохранение). Объекта «открытая заметка» нет — он размазан по
`NoteSession`/`CurrentNoteState`/полям виджета. Объекта «кэш картинок» нет —
он поле вида плюс два глобальных счётчика. Именно поэтому split-view сегодня —
переделка, а не добавка.

---

## 2. Предлагаемые классы

Принцип: **модель не знает о видах; вид держит указатель на модель и слушает
её сигналы; операции — только методами модели; консистентность держит класс, а
не вызывающий.** Умный указатель один — `shared_ptr`.

### 2.1. `ZStorage` — хранилище как модель

Владеет: корень; замок `store.lock`; **каталог заметок** (по id: parent, title,
snippet, created/modified (моменты UTC), archived, folder, lost, sortMark, path);
производное дерево папок; `History` (журналы) — один объект на хранилище;
сторож каталога (опционально); правила истории (`history::Rules` из настроек).

Публичные группы методов (черновик):

```
// жизнь
static shared_ptr<ZStorage> open(root, error)      // замок, миграции, находки — ОДИН раз
QString root() const;  bool isStore() const;
// каталог (только чтение, O(1) по id)
NoteInfo info(id) const; QStringList children(parentId, sortOrder) const;
QString parentOf(id); QString titleOf(id); bool isFolder(id); bool isArchived(id); ...
std::optional<SortOrder> sortOf(id) / effectiveSortFor(id)
// операции (каждая — одна точка правды, пишет файл ЧЕРЕЗ заметку, отмечает журнал, шлёт сигнал)
QString createNote(parentId, title = {}); QString createFolder(parentId, title);
bool rename(id, title); bool move(id, newParent); bool setSort(id, order);
bool archive(id); bool restore(id); bool forget(id); bool remove(id);
QStringList importFiles(parentId, paths); (importTree — CLI)
// перечитывание
void reload(); void refreshNote(id);
// открытие
shared_ptr<OpenNote> openNote(id);   // из кэша сеанса или с диска; см. 2.3
NoteHistory& historyOf(id);          // см. 2.2
// сигналы: noteChanged(id), noteAdded(id, parent), noteRemoved(id), moved(id, from, to), reloaded()
```

**Как «открытая» и «закрытая» заметка перестают быть двумя ветками.** Все
правки шапки (rename/move/setSort/archive) идут через `OpenNote` этой заметки:
если она открыта в каком-то виде — тот же объект; если нет — `ZStorage` берёт
её из кэша сеанса или поднимает с диска на время операции. Одна реализация,
одна запись (`saveTo` со всеми правилами: самопроверка, атомарная запись,
журнал), один сигнал `noteChanged` — и дерево, и список, и открытые виды
обновляются одинаково.

### 2.2. `NoteHistory` — журнал одной заметки, с правилами

Существующий `journal::History` — механика файла (замок, поколения, чтение,
прореживание, чистка) — остаётся как есть внутри `ZStorage`. Сверху — фасад
на одну заметку, забирающий из `NoteEditor` его пять полей и шесть мест
создания `History`:

```
class NoteHistory {           // выдаётся ZStorage::historyOf(id); живёт в ZStorage
  bool ensureBaseline(bytes, fileTime);        // recordBaseline
  bool record(Kind, bytes, source = 0);        // recordHistory: разжатие хвоста, ленивая чистка, decideStep, truncate/append
  const Journal& timeline();                   // ленивое чтение
  QByteArray snapshotAt(index);
  void compressOnce();                         // journalCompressed
  // состояние: tail, tailTime, tailKnown, compressed — здесь, а не в редакторе
};
```

### 2.3. `OpenNote` — открытая заметка (сессия правки)

То, что сегодня `NoteSession` + часть `CurrentNoteState` + поля виджета:

```
class OpenNote {              // shared_ptr, живёт в кэше сеанса ZStorage; вид держит копию shared_ptr
  ZDocument& note();          // единственная живая модель
  QString id(), path();
  Digest fileDigest(); QByteArray lastSaved();      // «изменилось ли» — здесь
  bool modified() const;                            // документ ≠ последняя запись
  SaveOutcome save(force, interactive);             // ЕДИНСТВЕННЫЙ путь на диск (saveTo + NoteHistory::record)
  void adoptExternal(bytes);  bool externalPending(); ...   // сторож файла и внешняя правка — здесь
  NoteStats stats(); bool statsFresh();
  CaretSpot caret(); void setCaret(...); int scroll(); void setScroll(...)   // место человека в заметке
  QTimer autosave;                                  // автосохранение — свойство сессии, не виджета
  // сигналы: saved(), externalChanged(), statsChanged(), titleChanged()
};
```

`NoteEditor` становится **видом**: держит `shared_ptr<OpenNote>`, `setDocument`
через люк, переводит клавиши в глаголы `ZDocument`, рисует; на смену заметки
просто меняет указатель. Split-view = второй `NoteEditor` с тем же или другим
`OpenNote`; общие вещи (кэш заметок, каретка на заметку) — в `ZStorage`, а не в
виджете (`caretMemory_`, `noteCache_` уезжают).

Спорный вопрос — **каретка на заметку** (`caretMemory_`): по смыслу это память
приложения о человеке («где он был в этой заметке»), а не заметки; предлагаю
держать её в `OpenNote` пока заметка в кэше и сериализовать в `state.json` по
всем известным заметкам сеанса, как и сейчас последнюю.

### 2.4. `HistorySession` — режим истории/разности

17 полей `CurrentNoteState` — в один объект, который **существует, только пока
идёт режим**: `timeline`, `index`, `snapshot`, `base`, тексты, оба `diff::Result`,
четыре готовых документа разности, `slot`, `peek/plain/fromFresh`. Создаётся
`OpenNote::enterHistory(index)`, живёт у `OpenNote` (`shared_ptr`, null вне
режима), уничтожается `leaveHistory`. Дубли в виджете (`diffMarks_`,
`diffSource_`) исчезают: вид спрашивает объект.

### 2.5. `ImageCache` — кэш картинок приложения

Из `NoteView` уезжают `imageCache_`, `imageOrder_`, `imageCacheBytes_`,
`currentNoteImages_` (→ «защищённые ключи» по видам), счётчики декодов и
`g_loadedImageSizeLimit`. Один объект на приложение (`shared_ptr` у
`ZStorage`? нет — у приложения: кэш не про хранилище, а про память машины;
предлагаю поле объекта `Application`/`AppServices`, который создаётся в `main`
и раздаётся видам). API: `info(path)` (заголовок без декода), `pixels(path)`
(ленивое декодирование, бюджет, LRU), `protect(view, keys)`, `budgetBytes()`,
телеметрия. `NoteImage` = сегодняшняя `CachedImage` (declared, pixels, facts,
state) — значение, которым кэш отвечает.

### 2.6. `NoteSearch` — запрос отдельно от найденного (§4.2 отчёта 6)

`SearchQuery` (текст, регистр, история) — один на программу, у панели поиска.
`SearchResults` — производное от (запрос, документ): вектор совпадений,
текущее, — принадлежит **виду** (пересчитывается при смене документа/правке,
подсветка только видимой области → снимает 207 мс на «Карамазовых»). Глобальный
поиск по хранилищу и по истории — как есть (`StoreSearch`, `searchNoteHistory`),
но результаты — модель приложения, а не окна.

### 2.7. `Appearance` — доступ только на чтение

`const Appearance& appearance()`; запись — только загрузчиком; тестам —
`ScopedAppearance` (RAII: меняет и возвращает). Снимки привязок клавиш в
`NoteEditor` (`bindings_`, `specialKeys_`…) — в объект `KeyMap`, который
пересобирается при перечитывании конфига.

### 2.8. `MainWindow` — окно как класс

Локали `main()` и ~40 лямбд — поля и слоты одного класса; `KeyWalk`,
`NewNoteGrab` — его вложенные помощники; `Session` делится на `WindowState`
(геометрия, сплиттер, раскрытые ветки, спрятанные панели) и `AppState`
(корень, каталог вывоза, история поиска). Порядок этого шага — последним: он
самый большой по строкам и самый безопасный по смыслу.

---

## 3. Матрицы взаимодействия (кто кого держит, что передаёт)

| A → B | держит? | что передаёт / спрашивает | чего не хватает сегодня |
|---|---|---|---|
| `MainWindow` → `ZStorage` | `shared_ptr` | id заметки/папки, порядок сортировки, команды (create/rename/move/archive…), `openNote(id)` | самого `ZStorage`; сегодня — модель дерева + лямбды |
| `NoteTreeModel`/`NoteListModel` → `ZStorage` | указатель, слушают сигналы | `children(parent, sort)`, `info(id)`; на `noteChanged(id)` — `dataChanged`, на структурные — перестройка узла | сигналы модели хранилища; сегодня модель сама читает диск и держит кэш |
| `NoteEditor` (вид) → `OpenNote` | `shared_ptr` (копия) | `note()` для глаголов, `setDocument(note.getDocument())`, каретка/прокрутка при смене, `save` по таймеру/уходу | `OpenNote`; сегодня всё в самом виджете |
| `OpenNote` → `ZDocument` | значение (ручка) | правка глаголами, `saveTo`, `toMarkdown` | ничего — есть |
| `OpenNote` → `NoteHistory` | ссылка через `ZStorage` | `record(kind, bytes)`, `timeline()`, `snapshotAt` | фасада нет; поля журнала в `NoteSession` |
| `OpenNote` → `HistorySession` | `shared_ptr` (null вне режима) | вход/выход, индекс, слепок, база, документы разности | объекта нет — 17 полей |
| `NoteView` → `ImageCache` | указатель на объект приложения | `info(path)`, `pixels(path)`, `protect(keys)` | кэш внутри вида |
| `NoteEditor` → `SearchResults` | значение | пересчёт при смене документа/правке; подсветка видимого | размазано между виджетом, панелью, `main.cpp` |
| `FindBar` → `SearchQuery` | значение | текст/регистр/история | панель хранит их сама — приемлемо, но счётчик приходит снаружи |
| `ZStorage` → `journal::History` | значение | всё, что сегодня зовут шесть мест редактора и `archive.cpp` | ничего — есть, но объект нигде не живёт |
| `ZStorage` → файловая система | — | чтение каталога, `saveTo` через `OpenNote`, `QSaveFile` | `rewriteNote` в `main` идёт мимо |
| `Application` → `Appearance`, `ImageCache`, `FormulaEngine`, иконки | владеет | раздаёт видам | глобальные |

Правило владения простое: **окно ⊃ виды; приложение ⊃ хранилище ⊃ открытые
заметки ⊃ история/разность; вид → модель указателем, модель → вид сигналом.**

---

## 4. Тесты как критерий дизайна

Для каждого класса — какие инварианты он ОБЯЗАН держать сам, и какой злой тест
их ломает:

* `ZStorage`: после любой операции каталог в памяти == каталог на диске
  (перечитать и сравнить); rename/move открытой и закрытой заметки дают байт в
  байт один файл; операция над заметкой, открытой в двух видах, видна в обоих;
  падение между «журнал» и «стаб» при архивации идемпотентно (уже есть в
  `Archive`); злой тест — случайная последовательность из 200 операций над
  копией хранилища владельца с проверкой инвариантов после каждой и
  `verifyStore` в конце.
* `NoteHistory`: журнал только растёт (кроме двух названных случаев); повторное
  `record` тех же байт не пишет ничего; `snapshotAt(i)` == тому, что писали;
  злой тест — оборванный хвост, чужие байты, два объекта на один журнал.
* `OpenNote`: `modified()` ⇔ документ ≠ `lastSaved`; `save()` не пишет, если
  не изменилось; внешняя правка при несохранённом — ждёт; злой тест — правка
  снаружи во время автосохранения; каретка переживает уход/возврат/перезапуск.
* `HistorySession`: вход/выход не меняет живую заметку ни на байт; после выхода
  объект уничтожен, документов разности не осталось (счётчик).
* `ImageCache`: бюджет никогда не превышается; повторное открытие не декодирует;
  защищённые ключи не вытесняются; злой тест — тысяча картинок при бюджете в
  одну.
* `SearchResults`: смена документа сбрасывает; подсветок вне видимого нет;
  счётчик == размеру найденного (§4.2).
* Общий критерий: **два `NoteEditor` на одном `OpenNote`** — правка в одном
  видна в другом, отмена общая, сохранение одно.

---

## 5. Предлагаемый порядок переезда (E)

1. **`OpenNote` + `NoteHistory`** — самое размазанное и самое опасное (журнал,
   сохранение); переезд полей из `NoteSession`/`CurrentNoteState`; `NoteEditor`
   худеет вдвое; кэш заметок и `caretMemory_` уходят из виджета в новый объект
   `NoteSessions` (будущая часть `ZStorage`). Наборы `Editor`, `HistoryWrite`,
   `ExternalChange`, `Session`, `DiffView` стерегут поведение — не трогать их
   ожидания без причины.
2. **`ZStorage`** — каталог из `NoteTreeModel`, операции из лямбд `main.cpp`,
   `rewriteNote` уничтожается (запись только через `OpenNote::save`), сигналы;
   дерево и список — проекции. Здесь же уходит раздвоение «открыта/закрыта».
3. **`ImageCache`, `Appearance` const, `SearchResults`** — независимые, можно
   вклинивать между.
4. **`HistorySession`** — вслед за `OpenNote`.
5. **`MainWindow`** — последним, когда операций в лямбдах не останется.

Каждый шаг — с приёмкой «два вида на одну заметку» как дымовым тестом
архитектуры (пусть без UI: два `NoteEditor` в наборе).

---

## 6. Вопросы владельцу

1. `caretMemory_` (место каретки на заметку) — свойство сессии заметки (в
   `OpenNote`, живёт с кэшем) или приложения (отдельная карта, как сейчас)?
2. `ImageCache` — у приложения (мой вариант) или у хранилища (все картинки —
   вложения хранилища)?
3. `Appearance`: согласны сделать `appearance()` константной и дать тестам
   `ScopedAppearance`? Это правка 15 мест в тестах и ни одного в коде.
4. Порядок E: `OpenNote` первым (риск в журнале, но именно там сегодня хуже
   всего) или `ZStorage` первым (больше строк, меньше риска потери данных)?
