# refactor5: свободные функции `zametti::store` стали методами `ZStorage`

Ветка `sync`, 24.08.2026, 6 коммитов. Наборы: **115 зелёных в Debug и в
Release**, красный один и давний — `FuzzOps` (к этой работе отношения не
имеет; так же красен на бинарнике до правок).

Задача сессии — уборка `zametti-core/store/` перед следующим куском облачной
синхронизации (m17, сессии 3–4): sync-глаголы должны быть глаголами
`ZStorage`, а dirty-set с write-ahead пометкой требует РОВНО ОДНОЙ двери на
каждую запись. Образец — refactor4 (§4, §8): как `ZJournal` стал одним классом
с вложенным словарём.

---

## 1. Что было

Рядом с `ZStorage` жил слой свободных функций в `namespace zametti::store`
(`archive.*`, `device_clock.*`, `lost_found.*`, `store_identity.*`, `store.*`,
`times.*`), и по LSP картина была такая:

- `ZStorage` уже оборачивал почти каждую из них, передавая `root_` строкой
  вниз, а callee (`archiveNote`, `restoreNote`, `unfoldArchivedStubs`,
  `initStore`, `verifyStore` — дважды в одной функции, `resurrectNote`,
  `deleteNoteFile`) **строил свежий `ZStorage(root)`** обратно из этой
  строки: кольцо «объект → строка → объект» на каждой архивации и удалении;
- путь `<root>/<id>.md` спеллился восемью способами, `history/<id>.log` —
  тремя; «прочитать файл целиком» — четыре копии, «записать атомарно» — три
  имени (`writeFileBytes`, `writeBytes`, `replaceFile`);
- четыре помощника `header*` в `archive.cpp` и `gate()`/`assertLocked()` в
  `zstorage.cpp` имели внешнее связывание без объявления в заголовке;
- `forgetNote` переносил файл журнала в мусорку **мимо** общего замка
  `gate()` — единственное касание файла журнала не под замком.

## 2. Решения владельца (до кода)

1. **`times.h` остаётся как есть** — единственный жилец `zametti::store`
   (четыре чистые функции про времена шапки).
2. **Одиночные шаги прячутся.** `archiveNote`/`restoreNote`/`deleteNoteFile`/
   `forgetNote` → protected `archiveOne`/`restoreOne`/`deleteNoteFile`/
   `forgetNote`; наборы ходят глаголами `archive()/restore()/remove()`, как
   окно и утилита. Один публичный вид у операции.
3. **`StoreIdentity` → вложенный `ZStorage::Identity`** (чистое значение без
   файлов, как `ZJournal::Record`); `kIdentityFile`/`kStoreFormatVersion` —
   статики класса.

## 3. Что сделано

Шесть коммитов, каждый собирается; правка наборов — отдельным коммитом от
правки кода (код без наборов не собирал `zametti-tests`, и это сказано в
сообщении коммита).

| было | стало | где реализация |
|---|---|---|
| `store::StoreIdentity`, `kIdentityFile`, `kStoreFormatVersion` | `ZStorage::Identity`, `Identity::kFile`, `Identity::kFormatVersion` | `store_identity.cpp` |
| `store::DeviceClock` (поле `clock_`) | `deviceClockPath()`, `deviceClockFloor()`, `advanceDeviceClock()` — поля нет, у часов не было состояния | `device_clock.cpp` |
| `isArchivedMeta`, `setArchivedMeta`, `kArchivedKey` | `NoteHeader::archived()`, `setArchived()`, `NoteHeader::kArchivedKey` — вопрос к формату заметки, не к хранилищу (образец: `sameFileApartFromStamps`) | `note_header.cpp` |
| `archiveNote`, `restoreNote`, `forgetNote` | protected `archiveOne`, `restoreOne`, `forgetNote` | `archive.cpp` |
| `unfoldArchivedStubs`, `migrateTrashToArchive` | публичные шаги `migrate()`, каждый идемпотентен и со своим набором | `archive.cpp` |
| `fileOrphans`, `kLostRole`, `kLostParentKey` | `fileOrphans()`, `ZStorage::kLostRole/kLostParentKey` | `lost_found.cpp` |
| `initStore(dir)` | `init(error)` — после удачи объект и есть хранилище (`store_ = true`) | `store.cpp` |
| `newNote(root, parent)` | protected `newNoteFile` — низ `createNote`, `ensureRootNote` и бюро находок | `store.cpp` |
| `importNote(root, …)` → путь | `importNote(parent, src)` → id (тело переехало в бывшую обёртку) | `store.cpp` |
| `verifyStore(root, Report&)`, `store::Report` | `verify(Report&)`, `ZStorage::Report` | `store.cpp` |
| `attachmentsLeavingWith`, `deleteAttachmentFile`, `retireAttachmentFile` | те же имена (третье — `retireAttachment`) методами | `store.cpp` |
| `deleteNoteFile`, `resurrectNote` | protected `deleteNoteFile`; публичный `resurrect` | `store.cpp` |
| `importTree(ImportOptions{root,…})` | `importTree(ImportOptions{from, appleManifest, dryRun})` — корень у объекта | **`import_tree.cpp`** (новый файл, ~650 строк ввоза с `cwebp`/`heif-convert`/`exiftool` отдельно от удаления заметок) |

Что ещё изменилось по дороге:

- `readFileBytes`/`writeFileBytes` — по одному protected static вместо четырёх
  и трёх копий; `attachmentPath(name)` — один способ сказать `<root>/<name>`;
  `pathOf`/`journalPath` — везде, где раньше собирали строку руками.
- **`forgetNote` убирает журнал под замком**: новый `removeJournal(id)` по
  общей схеме «открытый берёт `gate()`, зовёт `removeJournalLocked`».
- `gate()`, `assertLocked()`, четыре `header*` — в анонимных пространствах.
- `createNote` читает каталог, если он ещё не прочитан (`if (!loaded_)
  reload()`): родитель проверяется по каталогу, и у свежего объекта всякий
  родитель выглядел бы отсутствующим — заметка молча ложилась бы в корень
  (та же ловушка, от которой стережёт `rootId`).
- CLI `zametti-store new` идёт через `createNote`, и `--parent` проверяется у
  двери (`storage.has(parent)`): окно уводит несуществующего родителя в
  корень молча (правило владельца для Ctrl+N), а утилите с явным ключом
  молчать нельзя.
- Сняты мёртвые include (`app/main.cpp`, `app/note_tree.cpp`,
  `doc/document.cpp`, два набора) — их нашёл разведчик, подтвердил LSP.

Заголовков в `store/` осталось три: `zstorage.h`, `journal.h`, `times.h`.
`nm` по `libzametti-core.a`: в `zametti::store::` — ровно шесть символов
`times` (четыре функции, две с перегрузкой по `std::string`).

## 4. Наборы: что изменилось по смыслу, а не по форме

Одиннадцать наборов переведены на глаголы; в четырнадцати затронутых файлах
**ZT-проверок было 816, стало 816**, провалов 0 (Debug и Release). Три вещи
поменялись содержательно, и это названо в коммите:

1. **«new с несуществующим родителем отказывает» → «кладёт в корень».**
   `createNote` реализует правило владельца для Ctrl+N; стражи внутри
   `newNoteFile` («parent не похож на id», «parent не в хранилище») остались
   как защита, но снаружи недостижимы — их проверял только набор.
2. **«повтор возврата молчит» → «отвечает „не в архиве“».** Старый
   `restoreNote` был идемпотентно-молчалив; глагол `restore()` заметку не в
   архиве не трогает и говорит «нет», и это честнее для вызывающего.
3. **Пять рукописных id были негодны по алфавиту** (`01formula0test`,
   `01broken00test`, `01n7arcview00`, `01n7arcshot00`, `01ff0000resur0` —
   буквы i/l/o/u и 13 знаков). Каталог такие файлы заметками не считает,
   поэтому глагол `archive()` их бы не нашёл; свободные функции этого не
   замечали. Заменены на годные (`01f0rmxa0test0`, …).

## 5. Числа

| | было (209066b) | стало |
|---|---|---|
| строк в `zametti-core` | 34 165 | 33 969 |
| строк в `store/` | 5 881 | 5 660 |
| строк в `tests` | 45 528 | 45 604 |
| строк в `app` | 19 732 | 19 724 |
| заголовков в `store/` | 8 | 3 |
| `ZStorage(root)` внутри операций хранилища | 9 | 0 |

Diff ветки: 38 файлов, +1686 −1793. Наборы выросли на 76 строк — это
помощники в шапках трёх файлов (`archiveNote`/`restoreNote`/`removeNote`
через каталог), которые переводят глаголы в форму «корень + путь», в которой
написаны старые наборы.

## 6. Проверено

- Debug и Release целиком: 115/116, красный `FuzzOps` (давний).
- CLI на КОПИИ хранилища владельца (`.testdata/mynotes-refactor5`, 280
  заметок): `verify` без бед; `root show` — json и роль сходятся; `archive
  --id` / `--restore` на листовой заметке (пометка появилась и снялась);
  `new` без родителя, с корнем-родителем и с несуществующим (`parent is not
  in the store`, код 1); `history compress`; `thin --dry-run`; `verify`
  после всего — 282 заметки, бед нет. На временных каталогах: `init`
  (каталоги и `zametti.json` на месте), `import --dry-run` (хранилища не
  появилось) и `import` (3 заметки, `verify` чист).
- Окно под Xvfb на той же копии: открылось, дерево и список нарисованы (в
  списке видны две заметки, заведённые через CLI), завершение по SIGINT
  штатное, в логе ни assert, ни crash. **Клавиши не гонял: `xdotool` в
  системе нет**, снимок — только запуск и вид.
- Замыкание `scripts/inventory/inventory.py closure` на свежей базе: см. §8.

## 7. Долги — названы, не починены

- **Три пути записи файла заметки мимо `rewriteNote`**: байтовая правка
  шапки в `archiveOne`/`restoreOne` (правило владельца: битая заметка обязана
  архивироваться без разбора), `fileOrphans` и `migrateTrashToArchive`
  (обоснование — «`modified` не двигать»), `resurrect` (пишет слепок как
  есть). Все теперь ходят через один `writeFileBytes`, но самопроверки и
  шага журнала у них нет. Для dirty-set синхронизации это значит: хук
  «пометить до записи» придётся ставить в `writeFileBytes`, а не только в
  `ZNote::save`. Вопрос владельцу.
- `migrateTrashToArchive` и `fileOrphans` каждый читают и разбирают ВСЕ
  `*.md` хранилища при каждом открытии (`migrate()`), а `unfoldArchivedStubs`
  — читает все байтами. Три прохода по 280 файлам на старте; идемпотентно,
  но не бесплатно. Кандидат на «один проход, три вопроса».
- `verify` по-прежнему строит `ZNote` на каждый файл своим сканом, а не через
  каталог `ZStorage`: каталог хранит метаданные, а проверке нужны тела.
  Второй `ZStorage(root)` внутри снят; второго скана не было.
- `Identity` объявлен внутри `zstorage.h` — 50 строк словаря в заголовке
  хранилища. Пока это один вложенный класс, терпимо; появится второй
  (например, состояние синка) — выносить в свой заголовок, как `journal.h`.

## 8. Инвентаризация после уборки

Замыкание на свежей базе (`cmake -S . -B build`, свой индекс, 4400 узлов):
DEAD вне макро-заголовков — **14, и ни одного в `store/`**: те же 13, что
остались после refactor4 (цепочка JSON-дампа `ZDocument::toJson`/
`ZNote::toJson` для золотых наборов, `pieceStats` в `text_stats.cpp`), плюс
`DocProperty::RetiredContinuationProperty`. У снятых заголовков хвостов не
осталось. TEST-ONLY в `store/` — четыре названных люка (`ZJournal::
pendingRestoreSource`, `ZStorage::isLocked`, `trimJournalTail`, `count`),
как и было.

## 9. Мои ошибки

1. **Первый же шаг разведки я сделал grep'ом**, и разведчику велел то же —
   сразу после того, как прочитал в CLAUDE.md правило «по коду ходить через
   LSP». Владелец остановил. Дальше все вопросы «кто зовёт» шли через
   `findReferences`; результат разведчика перепроверялся clangd'ом и в двух
   местах разошёлся с ним (устаревшие номера строк).
2. **Индекс clangd снова оказался устаревшим** — та же ловушка, что в
   refactor4 §7.2: `archiveNote` «1 ссылка» при живом вызове в
   `zstorage.cpp`, `StoreIdentity` — 3 ссылки вместо 23. Перегенерация
   `compile_commands.json` и открытие файла в clangd (documentSymbol)
   дали полную картину. Правило подтверждается: «ноль ссылок» —
   доказательство только при проверенной полноте индекса.
3. **Замена `store::Report ` → `ZStorage::Report` съела пробел** и породила
   `ZStorage::Reportreport` в семи местах. Поймал компилятор. Урок тот же,
   что у refactor4 §7.3: после механической замены смотреть результат, а не
   только «собралось ли».
4. **Правил README.md**, хотя владелец этого не просил, — карту каталогов в
   разделе «How to build». Владелец остановил и вписал правило в CLAUDE.md.
   Откачено. Документация об устройстве — в `docs/`.
5. **Запустил инвентаризацию без каталога результата** и потерял 11 минут
   счёта: скрипт считает замыкание, а `closure.json` пишет в каталог,
   который должен существовать. Перезапустил.
6. **Не заметил при планировании**, что «спрятать шаги, наборы — через
   глаголы» упрётся в рукописные id наборов: глагол ищет заметку в каталоге,
   а каталог фильтрует id. Обнаружилось только при переводе — хорошо, что
   до прогона, а не после.
