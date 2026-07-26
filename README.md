# zametti

take notes, organize 'em, encrypt, sync via cloud

Заметки на диске — обычный markdown: их можно править любыми инструментами и
держать в git. Приложение форматом не владеет, а только читает и пишет его.

## Состояние

Этап 1, ядро формата. `zametti-core` умеет `markdown → IR → markdown` с
побайтовой идемпотентностью на каноническом входе. Просмотрщика пока нет.

```
3rdparty/md4c/     md4c 0.5.3, вендоринг
zametti-core/      разбор, сериализация, JSON-дамп IR — без Qt
tests/             тесты ядра
docs/              бриф этапа и записка по ядру
```

## Сборка

```
cmake -S . -B build
cmake --build build -j
cd build && ctest
```

Ядру и его тестам Qt не нужен. Подробности, принятые по ходу решения и открытые
вопросы — в [docs/zametti-core-notes.md](docs/zametti-core-notes.md).

## Лицензия

GPL-3.0, см. [LICENSE](LICENSE). Вендоренный md4c — MIT, см.
[3rdparty/md4c/LICENSE.md](3rdparty/md4c/LICENSE.md).
