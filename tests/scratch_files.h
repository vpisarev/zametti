// РАЗРУШИТЕЛЬНОЕ В НАБОРАХ — ЧЕРЕЗ ТУ ЖЕ ДВЕРЬ, ЧТО И В ПРОГРАММЕ.
//
// Таб'у из первых строк CLAUDE.md распространяется на наборы и пробники
// НАРАВНЕ с программой, и не по педантизму: каталог владельца снёс 30.08.2026
// именно пробник (`zametti-bench store-monkey`). Сторож, обходящий tests/,
// обходил бы место происшествия.
//
// Здесь только сокращения: работу делает zametti::ZSystem, а эти три строчки
// избавляют восемьдесят наборов от повторения её длинного имени.

#ifndef ZAMETTI_TESTS_SCRATCH_FILES_H
#define ZAMETTI_TESTS_SCRATCH_FILES_H

#include "zsystem.h"

#include <QDir>
#include <QFileInfo>
#include <QString>

namespace zt {

// Снести свой временный каталог целиком. Годится только то, что лежит внутри
// каталога временных файлов — то есть то, что набор завёл сам.
inline bool dropTree(const QString& dir) {
    return zametti::ZSystem::removeScratchTree(dir);
}

// То же для каталогов, которые набор держит в каталоге СБОРКИ (zt::TestData::outDir).
inline bool dropTreeInside(const QString& sandbox, const QString& dir) {
    return zametti::ZSystem::removeTreeInside(sandbox, dir);
}

// Убрать один файл внутри названной области: хранилища, каталога-облака или
// своей песочницы. Область называется явно — в этом весь смысл двери.
inline bool dropFile(const QString& area, const QString& path) {
    return zametti::ZSystem(zametti::ZSystem::Area::Scratch, area).removeForever(path);
}

// Убрать файл, областью считая ЕГО СОБСТВЕННЫЙ каталог. Для одиночек, что
// лежат сами по себе: бухгалтерия синка, state.json, журналы. Область всё равно
// названа — просто она выводится из самого пути.
inline bool dropFileNextTo(const QString& path) {
    return dropFile(QFileInfo(path).absolutePath(), path);
}

// Переименовать внутри области.
inline bool moveFile(const QString& area, const QString& from, const QString& to) {
    return zametti::ZSystem(zametti::ZSystem::Area::Scratch, area).rename(from, to);
}

}  // namespace zt

#endif  // ZAMETTI_TESTS_SCRATCH_FILES_H
