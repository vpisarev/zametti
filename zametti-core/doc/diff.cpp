#include "diff.h"

#include "note_header.h"
#include "serializer.h"

#include "dtl/dtl.hpp"

#include <string>
#include <vector>

namespace zametti::diff {
namespace {

std::vector<std::string> toStd(const QStringList& lines) {
    std::vector<std::string> out;
    out.reserve(size_t(lines.size()));
    for (const QString& line : lines) out.push_back(line.toStdString());
    return out;
}

}  // namespace

QVector<int> Result::changedAfterLines() const {
    QVector<int> out;
    for (const Row& row : rows)
        if (row.mark != Mark::Same && row.after >= 0) out.append(row.after);
    return out;
}

Result compare(const QStringList& base, const QStringList& shown) {
    Result result;

    const std::vector<std::string> a = toStd(base);
    const std::vector<std::string> b = toStd(shown);
    dtl::Diff<std::string, std::vector<std::string>> engine(a, b);
    engine.compose();
    const auto& ses = engine.getSes().getSequence();

    // Разбираем скрипт правки кучками: подряд идущие «не общие» строки — это
    // одно место правки, и разбирать его надо целиком. Внутри кучки удаления
    // и добавления сходятся в пары: пара — это правка строки (оранжевая),
    // остаток — чистое удаление или добавление.
    //
    // Пары берутся по порядку, а не «по похожести»: строку, переставленную
    // сверху вниз, честнее показать удалением и добавлением, чем выдумывать ей
    // родство с непохожей соседкой. Владелец просил про перестановку прямо
    // (del+add), и набор её проверяет.
    QVector<Row> removed;
    QVector<Row> added;
    const auto flushHunk = [&] {
        const int pairs = qMin(removed.size(), added.size());
        for (int i = 0; i < pairs; ++i) {
            Row row;
            row.mark = Mark::Changed;
            row.before = removed[i].before;
            row.after = added[i].after;
            row.textBefore = removed[i].textBefore;
            row.textAfter = added[i].textAfter;
            result.rows.append(row);
            ++result.changed;
        }
        for (int i = pairs; i < removed.size(); ++i) {
            result.rows.append(removed[i]);
            ++result.changed;
        }
        for (int i = pairs; i < added.size(); ++i) {
            result.rows.append(added[i]);
            ++result.changed;
        }
        removed.clear();
        added.clear();
    };

    for (const auto& item : ses) {
        const QString text = QString::fromStdString(item.first);
        const dtl::elemInfo& info = item.second;
        if (info.type == dtl::SES_COMMON) {
            flushHunk();
            Row row;
            row.mark = Mark::Same;
            // Номера у dtl с единицы; наружу отдаём с нуля — так же, как их
            // считает карта блоков.
            row.before = int(info.beforeIdx) - 1;
            row.after = int(info.afterIdx) - 1;
            row.textBefore = text;
            row.textAfter = text;
            result.rows.append(row);
            continue;
        }
        Row row;
        if (info.type == dtl::SES_DELETE) {
            row.mark = Mark::Removed;
            row.before = int(info.beforeIdx) - 1;
            row.textBefore = text;
            removed.append(row);
        } else {
            row.mark = Mark::Added;
            row.after = int(info.afterIdx) - 1;
            row.textAfter = text;
            added.append(row);
        }
    }
    flushHunk();
    return result;
}

Text textOf(const std::vector<Piece>& blocks) {
    // Шапки здесь нет вовсе: логические блоки — это тело, а штамп modified
    // меняется на каждой записи и изменением заметки не является.
    Text out;
    std::vector<BlockLines> map;
    const QString text = writePieces(blocks, NoteHeader{}, &map);
    out.blocks.reserve(int(map.size()));
    for (const BlockLines& b : map) out.blocks.append(b);

    // split по '\n' даёт лишнюю пустую строку в конце (текст кончается
    // переводом строки) — её убираем: строкой файла она не является, а в дифф
    // лезла бы «изменением» при любой правке хвоста.
    out.lines = text.split(QLatin1Char('\n'));
    if (!out.lines.isEmpty() && out.lines.last().isEmpty()) out.lines.removeLast();
    return out;
}

namespace {

QString canonicalBody(std::string_view fileBytes) {
    // Тот же ввоз, что у ZDocument::loadMarkdown: нормализация пробелов —
    // часть чтения, а не отдельный шаг, иначе слепок читался бы не так, как
    // читает программа.
    std::vector<Piece> blocks;
    NoteHeader header;
    parsePieces(normaliseSpaces(QString::fromUtf8(fileBytes.data(), qsizetype(fileBytes.size()))),
                blocks, header);
    return writePieces(blocks, NoteHeader{});
}

}  // namespace

QStringList linesOf(std::string_view fileBytes) {
    QStringList lines = canonicalBody(fileBytes).split(QLatin1Char('\n'));
    if (!lines.isEmpty() && lines.last().isEmpty()) lines.removeLast();
    return lines;
}

std::string bodyOf(std::string_view fileBytes) {
    return canonicalBody(fileBytes).toStdString();
}

BlockMarks blockMarks(const Result& result, const QVector<BlockLines>& blocks) {
    BlockMarks marks;
    marks.blocks.fill(Mark::Same, blocks.size());
    // Показанная сторона — always after: сторону задаёт само сравнение.
    const auto lineOn = [](const Row& row) { return row.after; };
    const Mark missing = Mark::Removed;    // этих строк здесь нет — заглушка
    const Mark appearing = Mark::Added;    // а эти есть только здесь

    // Строка слепка → блок. Таблицей, а не поиском на каждую строку: и строк, и
    // блоков в большой заметке тысячи, а перебор был бы квадратом.
    QVector<int> blockOfLine;
    for (int i = 0; i < blocks.size(); ++i)
        for (int line = blocks[i].first; line < blocks[i].first + blocks[i].count; ++line) {
            if (blockOfLine.size() <= line) blockOfLine.resize(line + 1, -1);
            blockOfLine[line] = i;
        }

    QVector<int> touched;
    QVector<int> wholly;
    touched.fill(0, blocks.size());
    wholly.fill(0, blocks.size());
    for (const Row& row : result.rows) {
        const int line = lineOn(row);
        if (row.mark == Mark::Same || line < 0) continue;
        const int block = line < blockOfLine.size() ? blockOfLine[line] : -1;
        if (block < 0) continue;
        ++touched[block];
        if (row.mark == appearing) ++wholly[block];
    }
    for (int i = 0; i < blocks.size(); ++i) {
        if (touched[i] == 0) continue;
        // ЦЕЛИКОМ НОВЫЙ — только если новые ВСЕ его строки. Тронули одну из
        // трёх — это правка блока, а не его появление, и полоска должна быть
        // оранжевой, иначе зелёный цвет обещает то, чего не было.
        marks.blocks[i] = wholly[i] == blocks[i].count ? appearing : Mark::Changed;
    }

    // Заглушки: строки, которых на этой стороне нет вовсе. Место им ПЕРЕД тем
    // блоком, который идёт следом; кончились блоки — заглушка в хвосте.
    int pending = 0;
    for (const Row& row : result.rows) {
        if (row.mark == missing) {
            ++pending;
            continue;
        }
        const int line = lineOn(row);
        if (line < 0 || pending == 0) continue;
        const int block = line < blockOfLine.size() ? blockOfLine[line] : -1;
        if (block < 0) marks.gapAtEnd += pending;
        else marks.gapBefore[block] += pending;
        pending = 0;
    }
    marks.gapAtEnd += pending;
    return marks;
}

}  // namespace zametti::diff
