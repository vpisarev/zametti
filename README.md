# zametti

take notes, organize 'em, encrypt, sync via cloud

Заметки на диске — обычный markdown: их можно править любыми инструментами и
держать в git. Приложение форматом не владеет, а только читает и пишет его.

## Состояние

Этап 1: ядро формата и просмотрщик. `zametti-core` умеет
`markdown → IR → markdown` с побайтовой идемпотентностью на каноническом входе.

```
3rdparty/md4c/     md4c 0.5.3, вендоринг
zametti-core/      разбор, сериализация, JSON-дамп IR — без Qt
app/               просмотрщик на QTextBrowser
tests/             тесты ядра
docs/              бриф этапа и записка по ядру
```

## Запуск

```
./build/app/zametti заметка.md            окно с отрендеренным документом
./build/app/zametti --noconfig заметка.md то же, но на умолчаниях
./build/app/zametti --check заметка.md    дифф с каноническим видом
./build/app/zametti --dump-config         список параметров оформления
```

Запуск без аргумента открывает то, что читали в прошлый раз.
`Ctrl+=` / `Ctrl+-` / `Ctrl+0` меняют масштаб.

Оформление настраивается в `~/.config/zametti/config.json` — приложение его
только читает. Умолчания со всеми параметрами лежат в
[default-config.json](default-config.json), копируйте оттуда нужное.

## Сборка

```
cmake -S . -B build
cmake --build build -j
cd build && ctest
```

Ядру и его тестам Qt не нужен; просмотрщику нужен `qt6-base-dev`, без него
собирается всё остальное. Подробности, принятые по ходу решения и открытые
вопросы — в [docs/zametti-core-notes.md](docs/zametti-core-notes.md).

## Лицензия

GPL-3.0, см. [LICENSE](LICENSE). Вендоренный md4c — MIT, см.
[3rdparty/md4c/LICENSE.md](3rdparty/md4c/LICENSE.md).
