#include "editor_ops.h"

#include "doc_model.h"
#include "document_builder.h"
#include "document_reader.h"
#include "serializer.h"
#include "marker.h"
#include "settings.h"

#include <QFont>
#include <QFontMetricsF>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextDocumentFragment>

#include <algorithm>
#include <cmath>
#include <vector>

namespace zametti {
namespace {

// Отступ вложенного пункта отсчитывается от маркеров родителей, а номер — от
// начала прогона. И то и другое известно только с начала списка, поэтому любой
// диапазон растягивается до целых прогонов. Заодно захватывается сосед за
// границей: смена рода блока меняет уровни того, что за ним.
//
// Пустая строка прогон не рвёт — просторный список остаётся одним списком, —
// поэтому она проходится насквозь.
BlockRange expandToRuns(const QTextDocument& doc, BlockRange range) {
    const int count = doc.blockCount();
    range.first = qBound(0, range.first, count - 1);
    range.last = qBound(range.first, range.last, count - 1);

    auto partOfRun = [&doc](int number) {
        const QTextBlock block = doc.findBlockByNumber(number);
        return isListBlock(block) || isVSpaceBlock(block) || levelOf(block) >= 0;
    };
    while (range.first > 0 && partOfRun(range.first - 1)) --range.first;
    while (range.last + 1 < count && partOfRun(range.last + 1)) ++range.last;
    return range;
}

// Шрифт документа — тот самый, которым его собрали, вместе с масштабом окна.
// Брать его отсюда, а не передавать параметром: иначе операция и отрисовка
// могли бы разойтись в том, какой сейчас кегль.
QFont baseFontOf(const QTextDocument& doc) { return doc.defaultFont(); }

void setBlockFormat(QTextCursor& cursor, const QTextBlock& block,
                    const QTextBlockFormat& format) {
    cursor.setPosition(block.position());
    cursor.setBlockFormat(format);
}

}  // namespace

namespace {

// Последний блок поддерева этого пункта: всё, что следом и глубже. Объявлена
// заранее — ею пользуется и разбор выделения.
int subtreeEnd(const QTextDocument& doc, int number);

// Диапазон, который надо привести в порядок после правки одного блока: сам блок
// и его соседи. Дальше расширят сами нормализующие проходы.
BlockRange around(int number) { return {number - 1, number + 1}; }

void normalise(QTextDocument& doc, BlockRange range) {
    syncLiteralBlocks(doc, range);
    // Заведённые пустые строки сдвигают номера блоков: диапазон для списков
    // раздвигаем ровно на столько же, иначе последний пункт остался бы
    // непроверенным — и осиротевший вложенный пункт так и уехал бы в файл.
    const int added = syncGaps(doc, range);
    syncLists(doc, {range.first, range.last + added});
}

// Блоки, которых касается курсор, вместе с поддеревьями. Конец выделения,
// стоящий ровно на начале блока, этот блок не захватывает: человек его не
// выделял, только довёл до него курсор.
BlockRange selectedBlocks(const QTextDocument& doc, const QTextCursor& cursor) {
    const int start = qMin(cursor.anchor(), cursor.position());
    const int end = qMax(cursor.anchor(), cursor.position());

    BlockRange range{doc.findBlock(start).blockNumber(), doc.findBlock(end).blockNumber()};
    if (end > start && doc.findBlock(end).position() == end && range.last > range.first)
        --range.last;

    // Поддерево последнего пункта: всё, что глубже него, принадлежит ему.
    if (isListBlock(doc.findBlockByNumber(range.last)))
        range.last = subtreeEnd(doc, range.last);
    return range;
}

// Сдвигает уровень списочных блоков диапазона. Несписочные не трогает: выделение
// могло зацепить и абзац, и его отступ тут ни при чём.
void shiftLevels(QTextDocument& doc, BlockRange range, int delta) {
    QTextCursor cursor(&doc);
    cursor.beginEditBlock();
    QTextBlock block = doc.findBlockByNumber(range.first);
    for (int i = range.first; i <= range.last && block.isValid(); ++i, block = block.next()) {
        if (!isListBlock(block)) continue;
        QTextBlockFormat format = block.blockFormat();
        format.setProperty(LevelProperty, qMax(0, levelOf(block) + delta));
        setBlockFormat(cursor, block, format);
    }
    normalise(doc, range);
    cursor.endEditBlock();
}

}  // namespace

namespace {

// Годится ли этот блок на роль содержимого пункта. Заголовок внутри пункта
// markdown не выражает, пустая строка ничьей вложенности не имеет, дословный
// кусок мы не трогаем вовсе.
bool canLiveInsideItem(const QTextBlock& block) {
    if (isRawBlock(block) || isVSpaceBlock(block)) return false;
    const Kind kind = kindOf(block);
    return kind == Kind::Paragraph || kind == Kind::Quote || kind == Kind::Code;
}

// Меняет уровень блоков внутри пункта: привязывает их к пункту выше или
// отвязывает обратно в обычный текст. Возвращает false, если привязывать не к
// чему или отвязывать нечего.
bool setInsideLevel(QTextDocument& doc, BlockRange range, int level) {
    QTextCursor edit(&doc);
    edit.beginEditBlock();
    bool any = false;
    QTextBlock block = doc.findBlockByNumber(range.first);
    for (int i = range.first; i <= range.last && block.isValid(); ++i, block = block.next()) {
        if (!canLiveInsideItem(block)) continue;
        QTextBlockFormat format = block.blockFormat();
        if (level < 0) {
            format.clearProperty(LevelProperty);
            format.setLeftMargin(0);
        } else {
            format.setProperty(LevelProperty, level);
        }
        setBlockFormat(edit, block, format);
        any = true;
    }
    if (any) normalise(doc, range);
    edit.endEditBlock();
    return any;
}

// Пункт, внутри которого оказался бы блок, если его привязать: ближайший выше,
// у которого уровень есть. Пустые строки по дороге не в счёт.
int levelAbove(const QTextDocument& doc, int number) {
    for (int i = number - 1; i >= 0; --i) {
        const QTextBlock block = doc.findBlockByNumber(i);
        if (isVSpaceBlock(block)) continue;
        return levelOf(block);
    }
    return -1;
}

}  // namespace

bool indentListItems(QTextDocument& doc, QTextCursor& cursor) {
    const BlockRange range = selectedBlocks(doc, cursor);
    const QTextBlock first = doc.findBlockByNumber(range.first);

    // Tab на абзаце сразу под списком привязывает его к пункту: получается
    // второй абзац этого пункта. Правило Tab при этом одно на всех — «сделать
    // блок глубже», просто у абзаца и у пункта это значит разное.
    if (!isListBlock(first)) {
        if (levelOf(first) >= 0 || !canLiveInsideItem(first)) return false;
        const int level = levelAbove(doc, range.first);
        if (level < 0) return false;
        return setInsideLevel(doc, range, level);
    }

    // Отступать можно только под уже существующий пункт: иначе получился бы
    // прыжок через уровень, которого в файле не бывает.
    const QTextBlock prev = first.previous();
    if (!prev.isValid() || !isListBlock(prev) || levelOf(prev) < levelOf(first)) return false;

    shiftLevels(doc, range, 1);
    return true;
}

bool outdentListItems(QTextDocument& doc, QTextCursor& cursor) {
    const BlockRange range = selectedBlocks(doc, cursor);
    const QTextBlock first = doc.findBlockByNumber(range.first);

    // Shift+Tab на втором абзаце пункта отвязывает его обратно в обычный текст.
    if (!isListBlock(first)) {
        if (levelOf(first) < 0) return false;
        return setInsideLevel(doc, range, -1);
    }
    if (levelOf(first) == 0) return false;

    shiftLevels(doc, range, -1);
    return true;
}

namespace {

// Первый блок за этим, который не пустая строка. Может выйти за конец
// документа — проверяет вызывающий.
int skipVSpace(const QTextDocument& doc, int number) {
    while (number < doc.blockCount() && isVSpaceBlock(doc.findBlockByNumber(number))) ++number;
    return number;
}

// Последний блок поддерева этого пункта: всё, что следом и глубже. Пустые
// строки внутри поддерева его не заканчивают, но и хвостом не висят: их берём
// только вместе с тем, что за ними.
int subtreeEnd(const QTextDocument& doc, int number) {
    const int level = levelOf(doc.findBlockByNumber(number));
    int last = number;
    for (;;) {
        const int next = skipVSpace(doc, last + 1);
        if (next >= doc.blockCount()) break;
        const QTextBlock block = doc.findBlockByNumber(next);
        const int other = levelOf(block);
        // Вложенный пункт принадлежит этому; блок без маркера на том же уровне —
        // это его же второй абзац, и он тоже часть пункта.
        const bool mine = isListBlock(block) ? other > level : other >= level;
        if (other < 0 || !mine) break;
        last = next;
    }
    return last;
}

// Пункт того же уровня перед этим. Отрицательное — соседа нет: либо кончился
// список, либо мы вышли на уровень выше.
int previousSibling(const QTextDocument& doc, int number) {
    const int level = levelOf(doc.findBlockByNumber(number));
    for (int i = number - 1; i >= 0; --i) {
        const QTextBlock block = doc.findBlockByNumber(i);
        if (isVSpaceBlock(block)) continue;      // просторный список — тот же список
        if (!isListBlock(block)) {
            if (levelOf(block) >= 0) continue;   // второй абзац пункта — часть списка
            return -1;                           // через абзац не прыгаем
        }
        const int other = levelOf(block);
        if (other < level) return -1;            // вышли из своего уровня
        if (other == level) return i;
    }
    return -1;
}

int nextSibling(const QTextDocument& doc, int number) {
    const int level = levelOf(doc.findBlockByNumber(number));
    const int after = skipVSpace(doc, subtreeEnd(doc, number) + 1);
    if (after >= doc.blockCount()) return -1;
    const QTextBlock block = doc.findBlockByNumber(after);
    if (!isListBlock(block) || levelOf(block) != level) return -1;
    return after;
}

}  // namespace

namespace {

// Начало и конец строки, на которой стоит эта позиция, в пределах своего блока.
// Блок кода строится из целых строк: полстроки в него не положишь.
int lineStartAt(const QTextDocument& doc, int position) {
    const QTextBlock block = doc.findBlock(position);
    const QString text = block.text();
    for (int i = position - block.position() - 1; i >= 0; --i)
        if (text.at(i) == QChar::LineSeparator) return block.position() + i + 1;
    return block.position();
}

int lineEndAt(const QTextDocument& doc, int position) {
    const QTextBlock block = doc.findBlock(position);
    const QString text = block.text();
    for (int i = position - block.position(); i < text.size(); ++i)
        if (text.at(i) == QChar::LineSeparator) return block.position() + i;
    return block.position() + block.length() - 1;
}

// IR куска документа. Тем же приёмом, что и копирование в буфер: кусок кладётся
// во временный документ, и смещения считать не приходится вовсе.
Document irOfRange(QTextDocument& doc, int from, int to) {
    if (from >= to) return {};
    QTextCursor range(&doc);
    range.setPosition(from);
    range.setPosition(to, QTextCursor::KeepAnchor);
    return selectionToIr(range);
}

}  // namespace

MoveResult toggleCodeBlock(QTextDocument& doc, const QTextCursor& cursor) {
    Document ir = readDocument(doc);
    if (ir.empty()) return {};

    // Границы выделения в номерах блоков IR: в документе литеральный блок лежит
    // построчно, и номера блоков документа с номерами IR не совпадают.
    const int start = qMin(cursor.selectionStart(), cursor.selectionEnd());
    const int end = qMax(cursor.selectionStart(), cursor.selectionEnd());
    const int first = irIndexOfBlock(doc.findBlock(start));
    const int last = irIndexOfBlock(doc.findBlock(end));
    if (first < 0 || last >= int(ir.size()) || first > last) return {};

    // Абзац с мягкими переносами — один блок, а выделить в нём человек может
    // несколько строк из многих. Тогда блок надо разнять: что осталось снаружи,
    // остаётся как было. Границы притягиваются к краям строк.
    const QTextBlock firstBlock = doc.findBlock(start);
    const QTextBlock lastBlock = doc.findBlock(end);
    const int lineStart = lineStartAt(doc, start);
    // Конец выделения ровно на начале строки: эту строку человек не выделял, и
    // тянуть её в блок кода незачем.
    const int lineEnd = (end > start && end == lineStartAt(doc, end))
                            ? end - 1
                            : lineEndAt(doc, end);
    const int blockEnd = lastBlock.position() + lastBlock.length() - 1;

    // Разделители строк на срезах в куски не берём: иначе оставшийся кусок
    // кончался бы пустой строкой, а она блок заканчивает.
    const Document head = irOfRange(
        doc, firstBlock.position(), lineStart > firstBlock.position() ? lineStart - 1 : lineStart);
    const Document tail =
        irOfRange(doc, lineEnd < blockEnd ? lineEnd + 1 : lineEnd, blockEnd);
    const Document chosen = irOfRange(doc, lineStart, lineEnd);
    if (!head.empty() || !tail.empty()) {
        if (chosen.empty()) return {};
        Document result(ir.begin(), ir.begin() + first);
        result.insert(result.end(), head.begin(), head.end());

        Block code;
        code.kind = Kind::Code;
        for (const Block& piece : chosen) {
            if (!code.text.empty()) code.text.push_back('\n');
            code.text += piece.text;
        }
        if (!code.text.empty() && code.text.back() != '\n') code.text.push_back('\n');
        result.push_back(code);

        const int landed = int(result.size()) - 1;
        result.insert(result.end(), tail.begin(), tail.end());
        result.insert(result.end(), ir.begin() + last + 1, ir.end());
        return {true, result, landed, 0};
    }

    // Дословные куски не трогаем вовсе: их текст выводится как есть.
    for (int i = first; i <= last; ++i)
        if (!ir[size_t(i)].rawSource.empty()) return {};

    bool allCode = true;
    for (int i = first; i <= last; ++i)
        if (ir[size_t(i)].kind != Kind::Code) allCode = false;

    Document result(ir.begin(), ir.begin() + first);
    if (allCode) {
        // Обратный ход: каждая строка кода становится строкой обычного текста.
        // Один блок кода — один абзац: переводы строк внутри абзаца жить умеют.
        for (int i = first; i <= last; ++i) {
            Block plain;
            plain.text = ir[size_t(i)].text;
            while (!plain.text.empty() && plain.text.back() == '\n') plain.text.pop_back();
            result.push_back(plain);
        }
    } else {
        Block code;
        code.kind = Kind::Code;
        for (int i = first; i <= last; ++i) {
            if (!code.text.empty()) code.text.push_back('\n');
            code.text += ir[size_t(i)].text;
        }
        if (!code.text.empty() && code.text.back() != '\n') code.text.push_back('\n');
        result.push_back(code);
    }
    const int landed = int(result.size()) - 1;
    result.insert(result.end(), ir.begin() + last + 1, ir.end());

    return {true, result, landed, 0};
}

MoveResult moveListItem(const QTextDocument& doc, const QTextCursor& cursor, int direction) {
    const QTextBlock block = cursor.block();
    if (!isListBlock(block)) return {};

    const int number = block.blockNumber();
    const int sibling = direction < 0 ? previousSibling(doc, number) : nextSibling(doc, number);
    if (sibling < 0) return {};

    // Оба куска — пункт с поддеревом; в IR они лежат подряд, потому что
    // списочные блоки один к одному с блоками IR. Между ними может стоять
    // пустая строка — просторный список тоже список.
    const bool selfIsUpper = direction > 0;
    const int upperFirst = selfIsUpper ? number : sibling;
    const int lowerFirst = selfIsUpper ? sibling : number;
    const int upperLast = subtreeEnd(doc, upperFirst);
    const int lowerLast = subtreeEnd(doc, lowerFirst);

    const int firstIr = irIndexOfBlock(doc.findBlockByNumber(upperFirst));
    const int upperSize = upperLast - upperFirst + 1;
    const int gapSize = lowerFirst - upperLast - 1;
    const int lowerSize = lowerLast - lowerFirst + 1;
    const int wholeSize = upperSize + gapSize + lowerSize;

    MoveResult result;
    result.doc = readDocument(doc);
    if (firstIr < 0 || firstIr + wholeSize > int(result.doc.size())) return {};

    // Пункты меняются местами, а пустая строка остаётся на месте: она
    // принадлежит стыку, а не пункту, и уехав с ним, порвала бы список там, где
    // человек ничего не трогал.
    const auto begin = result.doc.begin() + firstIr;
    Document reordered;
    reordered.reserve(size_t(wholeSize));
    reordered.insert(reordered.end(), begin + upperSize + gapSize, begin + wholeSize);
    reordered.insert(reordered.end(), begin + upperSize, begin + upperSize + gapSize);
    reordered.insert(reordered.end(), begin, begin + upperSize);
    std::copy(reordered.begin(), reordered.end(), begin);

    // Место, куда переехал сам пункт: верхний уходит за пустую строку и соседа,
    // нижний встаёт в самое начало.
    const int selfIr = irIndexOfBlock(block);
    const int selfStartIr = firstIr + (selfIsUpper ? 0 : upperSize + gapSize);
    const int landedIr = selfIsUpper ? firstIr + lowerSize + gapSize : firstIr;
    result.irBlock = landedIr + (selfIr - selfStartIr);
    result.offsetInBlock = cursor.positionInBlock();
    result.done = true;
    return result;
}

namespace {

// Кусок выделения с одинаковым начертанием. Собираем их заранее: правка формата
// на лету портит обход кусков блока.
struct StyleRun {
    int from = 0;
    int to = 0;
    int style = 0;
};

// Литеральные блоки буквальны целиком — их куски в выделение не берём вовсе.
// Куски встроенного кода берём только тогда, когда меняют сам код: жирный
// внутри него не действует.
std::vector<StyleRun> styleRuns(const QTextDocument& doc, int from, int to,
                                bool includeCode) {
    std::vector<StyleRun> runs;
    for (QTextBlock block = doc.findBlock(from); block.isValid(); block = block.next()) {
        if (block.position() >= to) break;
        if (isRawBlock(block) || kindOf(block) == Kind::Code) continue;
        for (QTextBlock::iterator it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            if (!fragment.isValid()) continue;
            const int start = qMax(from, fragment.position());
            const int end = qMin(to, fragment.position() + fragment.length());
            if (end <= start) continue;
            const int style = fragment.charFormat().intProperty(SpanStyleProperty);
            if (!includeCode && (style & SpanCode) != 0) continue;
            runs.push_back({start, end, style});
        }
    }
    return runs;
}

// Кегль встроенного кода — тот же, каким его собрал бы сборщик документа.
qreal codePointSize(const QTextDocument& doc) {
    const qreal base = doc.defaultFont().pointSizeF();
    if (appearance().codePointSize <= 0.0 || appearance().baseFontPoint <= 0.0) return base;
    return appearance().codePointSize * base / appearance().baseFontPoint;
}

// Оформление, отвечающее набору признаков. Ставим все три явно: снимать
// начертание — это тоже назначить его, только обычным.
QTextCharFormat formatForStyle(int style) {
    QTextCharFormat format;
    format.setProperty(SpanStyleProperty, style);
    format.setFontWeight((style & SpanBold) != 0 ? QFont::Bold : QFont::Normal);
    format.setFontItalic((style & SpanItalic) != 0);
    format.setFontStrikeOut((style & SpanStrike) != 0);
    return format;
}

bool toggleInlineStyle(QTextDocument& doc, QTextCursor& cursor, int style) {
    if (!cursor.hasSelection()) return false;
    const int from = qMin(cursor.anchor(), cursor.position());
    const int to = qMax(cursor.anchor(), cursor.position());

    // Снимать код с куска кода надо уметь, а вот жирный внутри кода не действует:
    // содержимое там буквальное, разметке взяться неоткуда.
    const std::vector<StyleRun> runs = styleRuns(doc, from, to, style == SpanCode);
    if (runs.empty()) return false;

    // Снимаем, только если начертание есть везде: иначе одно нажатие на
    // наполовину жирном тексте делало бы его наполовину обычным.
    bool everywhere = true;
    for (const StyleRun& run : runs) everywhere = everywhere && (run.style & style) != 0;

    QTextCursor edit(&doc);
    edit.beginEditBlock();
    for (const StyleRun& run : runs) {
        const int updated = everywhere ? (run.style & ~style) : (run.style | style);
        edit.setPosition(run.from);
        edit.setPosition(run.to, QTextCursor::KeepAnchor);
        edit.mergeCharFormat(formatForStyle(updated));
    }
    edit.endEditBlock();
    return true;
}


// Во что превращаем блок: род, а у пункта — ещё и маркер с отметкой. Родом
// маркер не выражается: род у всех пунктов один.
struct BlockTarget {
    Kind kind = Kind::Paragraph;
    Marker marker = Marker::Bullet;
    bool checked = false;
};

// Смена рода одного блока. Блок берётся по номеру, а не ссылкой: вставка и
// удаление внутри цикла двигают позиции, и заранее взятый блок устарел бы.
// Возвращает false, если блок трогать не следует.
bool convertBlock(QTextDocument& doc, int number, BlockTarget target) {
    const QTextBlock block = doc.findBlockByNumber(number);
    if (!block.isValid() || isRawBlock(block)) return false;
    const Kind from = kindOf(block);
    if (from == Kind::Code) return false;     // код списком быть не может
    if (from == Kind::VSpace) return false;   // пустая строка пунктом тоже не бывает

    QTextBlockFormat format = block.blockFormat();
    QString insert;
    int strip = 0;

    if (target.kind == Kind::Paragraph) {
        format.clearProperty(KindProperty);
        format.clearProperty(MarkerProperty);
        format.clearProperty(CheckedProperty);
        format.clearProperty(LevelProperty);
        format.setLeftMargin(0);
        format.setHeadingLevel(0);
    } else {
        // Отметка задачи при переходе в другой вид списка просто исчезает.
        // Раньше она переезжала в начало содержимого обычным текстом — чтобы не
        // пропадала молча, — но выглядело это как "1. [x] дело", то есть как
        // ошибка, а не как забота.
        //
        // Обратный ход: буллет, содержимое которого начинается с отметки, файл
        // всё равно прочтёт задачей. Делаем задачу сразу и убираем отметку из
        // текста — иначе документ разошёлся бы с тем, что окажется на диске.
        if (target.kind == Kind::ListItem && target.marker == Marker::Bullet) {
            const QString text = block.text();
            if (text.startsWith(QStringLiteral("[x] ")) ||
                text.startsWith(QStringLiteral("[X] "))) {
                target.marker = Marker::Task;
                target.checked = true;
                strip = 4;
            } else if (text.startsWith(QStringLiteral("[ ] "))) {
                target.marker = Marker::Task;
                target.checked = false;
                strip = 4;
            }
        }
        format.setProperty(KindProperty, int(target.kind));
        format.setHeadingLevel(0);
        if (target.kind == Kind::ListItem) {
            format.setProperty(MarkerProperty, int(target.marker));
            format.setProperty(CheckedProperty, target.checked);
            format.setProperty(LevelProperty, isListBlock(block) ? levelOf(block) : 0);
        }
    }

    QTextCursor edit(&doc);
    edit.setPosition(block.position());
    edit.setBlockFormat(format);

    if (strip > 0) {
        edit.setPosition(block.position());
        edit.setPosition(block.position() + strip, QTextCursor::KeepAnchor);
        edit.removeSelectedText();
    } else if (!insert.isEmpty()) {
        edit.setPosition(block.position());
        edit.insertText(insert, block.charFormat());
    }
    return true;
}

bool setBlockKind(QTextDocument& doc, QTextCursor& cursor, BlockTarget target) {
    const BlockRange range = selectedBlocks(doc, cursor);

    QTextCursor edit(&doc);
    edit.beginEditBlock();
    bool any = false;
    for (int i = range.first; i <= range.last; ++i)
        if (convertBlock(doc, i, target)) any = true;
    normalise(doc, range);
    edit.endEditBlock();
    return any;
}

}  // namespace

namespace {

// Что за автозамену просит набранное. Пусто — ничего не просит.
struct InputRule {
    BlockTarget target;
    int headingLevel = 0;
    int prefix = 0;        // сколько знаков убрать из начала блока
    bool matched = false;
};

bool isBulletMarker(QChar c) {
    return c == QLatin1Char('-') || c == QLatin1Char('*') || c == QLatin1Char('+');
}

// Скобочная часть автозамены — то, что человек набрал вместо чекбокса; пробел в
// конце уже отрезан.
//
// Недописанная "[" принимается только там, где маркер списка набран слитно
// ("-["). В уже готовом буллете её принимать нельзя: тогда при наборе полного
// "- [ ] " правило срабатывало бы на "[ " и закрывающая скобка оставалась бы в
// тексте — проверено на живом редакторе, выходило "- [ ] ] дело".
BlockTarget matchCheckbox(const QString& body, bool allowUnclosed, bool& matched) {
    matched = true;
    const BlockTarget task{Kind::ListItem, Marker::Task, false};
    if (body == QStringLiteral("[]") || body == QStringLiteral("[ ]")) return task;
    if (body == QStringLiteral("[x]") || body == QStringLiteral("[X]"))
        return {Kind::ListItem, Marker::Task, true};
    if (allowUnclosed && body == QStringLiteral("[")) return task;
    matched = false;
    return {};
}

InputRule matchInputRule(const QTextBlock& block, const QString& typed) {
    const bool list = isListBlock(block);
    const Kind kind = kindOf(block);
    // Пробел в конце уже проверен вызывающим.
    const QString body = typed.left(typed.size() - 1);

    // Чекбокс в начале буллета. Только в буллете: в нумерованном пункте "[x]" —
    // обычный текст, и превращать его в чекбокс нельзя.
    if (isListBlock(block) && markerOf(block).marker == Marker::Bullet) {
        bool matched = false;
        const BlockTarget task = matchCheckbox(body, false, matched);
        if (matched) return {task, 0, int(typed.size()), true};
    }

    if (list) return {};   // список списком уже не сделаешь

    // Задача одним махом: "-[", "-[]", "-[x]" и то же с пробелом после маркера.
    // Набирать "- " и ждать, пока сработает первая автозамена, не обязательно.
    if (body.size() >= 2 && isBulletMarker(body.at(0))) {
        const int after = body.at(1) == QLatin1Char(' ') ? 2 : 1;
        bool matched = false;
        // Слитно с маркером — короткий путь, недописанная скобка допустима.
        const BlockTarget task = matchCheckbox(body.mid(after), after == 1, matched);
        if (matched) return {task, 0, int(typed.size()), true};
    }

    // Маркер буллета: любой из трёх, как и в файле. В файл уйдёт дефис — знак
    // маркера канон не хранит.
    if (body.size() == 1 && isBulletMarker(body.at(0)))
        return {{Kind::ListItem, Marker::Bullet, false}, 0, 2, true};

    // Номер: цифры и точка или скобка.
    int digits = 0;
    while (digits < body.size() && body.at(digits).isDigit()) ++digits;
    if (digits > 0 && digits + 1 == body.size() &&
        (body.at(digits) == QLatin1Char('.') || body.at(digits) == QLatin1Char(')')))
        return {{Kind::ListItem, Marker::Ordered, false}, 0, int(typed.size()), true};

    // Заголовок: от одной решётки до шести.
    int hashes = 0;
    while (hashes < body.size() && body.at(hashes) == QLatin1Char('#')) ++hashes;
    if (hashes >= 1 && hashes <= 6 && hashes == body.size() && kind != Kind::Heading)
        return {{Kind::Heading, Marker::Bullet, false}, hashes, int(typed.size()), true};

    return {};
}

}  // namespace

Document selectionToIr(const QTextCursor& cursor) {
    if (!cursor.hasSelection() || cursor.document() == nullptr) return {};

    QTextDocument temp;
    QTextCursor paste(&temp);
    paste.insertFragment(cursor.selection());

    const int from = qMin(cursor.anchor(), cursor.position());
    const int to = qMax(cursor.anchor(), cursor.position());
    const QTextBlock first = cursor.document()->findBlock(from);
    const QTextBlock last = cursor.document()->findBlock(to);

    // Формат первого блока до фрагмента не доезжает, если выделение начинается
    // не с его начала, — Qt отдаёт такой кусок как чистый текст. Возвращаем его
    // сами: частично выделенный пункт обязан остаться пунктом.
    //
    // Исключение — выделение внутри одного блока, покрывающее его не целиком:
    // кусок строки это просто слова, а не пункт списка.
    const bool wholeBlock = from == first.position() &&
                            to >= first.position() + first.length() - 1;
    if (first.blockNumber() != last.blockNumber() || wholeBlock) {
        QTextCursor fix(&temp);
        fix.setPosition(0);
        fix.setBlockFormat(first.blockFormat());
    }

    Document ir = readDocument(temp);

    int deepest = -1;
    for (const Block& block : ir)
        if (block.rawSource.empty() && isList(block.kind))
            deepest = deepest < 0 ? block.level : qMin(deepest, block.level);
    if (deepest > 0)
        for (Block& block : ir)
            if (block.rawSource.empty() && isList(block.kind)) block.level -= deepest;

    return ir;
}

QString selectionToMarkdown(const QTextCursor& cursor) {
    const Document ir = selectionToIr(cursor);
    if (ir.empty()) return {};

    std::string text = serialize(ir);
    const bool inlineOnly = ir.size() == 1 && ir.front().rawSource.empty() &&
                            ir.front().kind == Kind::Paragraph;
    if (inlineOnly && !text.empty() && text.back() == '\n') text.pop_back();
    return QString::fromUtf8(text.data(), qsizetype(text.size()));
}

bool applyInputRuleAtCursor(QTextDocument& doc, QTextCursor& cursor) {
    const QTextBlock block = cursor.block();
    if (isRawBlock(block) || kindOf(block) == Kind::Code) return false;

    // Правило срабатывает на пробел сразу за началом СТРОКИ, а не блока. С тех
    // пор как Enter переносит строку внутри абзаца, начало блока и начало строки
    // перестали совпадать: список, начатый со второй строки абзаца, иначе не
    // завёлся бы вовсе.
    const QString text = block.text();
    const int at = cursor.position() - block.position();
    int lineStart = 0;
    for (int i = at - 1; i >= 0; --i)
        if (text.at(i) == QChar::LineSeparator) {
            lineStart = i + 1;
            break;
        }

    const int typedLength = at - lineStart;
    if (typedLength <= 0 || typedLength > 8) return false;
    const QString typed = text.mid(lineStart, typedLength);
    if (!typed.endsWith(QLatin1Char(' '))) return false;

    const InputRule rule = matchInputRule(block, typed);
    if (!rule.matched) return false;

    QTextCursor edit(&doc);
    edit.beginEditBlock();

    // Строка внутри абзаца отдельным блоком быть не может, а список — может
    // только блоком: режем по началу строки, убирая её разделитель.
    int start = block.position();
    if (lineStart > 0) {
        edit.setPosition(block.position() + lineStart - 1);
        edit.setPosition(block.position() + lineStart, QTextCursor::KeepAnchor);
        edit.removeSelectedText();
        QTextBlockFormat carry = block.blockFormat();
        carry.clearProperty(TrailingNewlineProperty);
        edit.insertBlock(carry, block.charFormat());
        start = edit.position();
    }

    QTextBlockFormat format = doc.findBlock(start).blockFormat();
    if (rule.target.kind == Kind::Heading) {
        format.clearProperty(LevelProperty);
        format.clearProperty(MarkerProperty);
        format.clearProperty(CheckedProperty);
        format.setProperty(KindProperty, int(Kind::Heading));
        format.setHeadingLevel(rule.headingLevel);
    } else {
        format.setProperty(KindProperty, int(rule.target.kind));
        format.setProperty(MarkerProperty, int(rule.target.marker));
        format.setProperty(CheckedProperty, rule.target.checked);
        format.setProperty(LevelProperty, isListBlock(block) ? levelOf(block) : 0);
        format.setHeadingLevel(0);
    }

    edit.setPosition(start);
    edit.setBlockFormat(format);
    edit.setPosition(start);
    edit.setPosition(start + rule.prefix, QTextCursor::KeepAnchor);
    edit.removeSelectedText();
    normalise(doc, around(doc.findBlock(start).blockNumber()));
    edit.endEditBlock();

    cursor.setPosition(edit.position());
    return true;
}

bool applyCodeSpanRuleAtCursor(QTextDocument& doc, QTextCursor& cursor) {
    const QTextBlock block = cursor.block();
    if (isRawBlock(block) || kindOf(block) == Kind::Code) return false;

    // Курсор стоит сразу за только что набранной кавычкой.
    const int end = cursor.positionInBlock();
    const QString text = block.text();
    if (end < 2 || text.at(end - 1) != QLatin1Char('`')) return false;

    // Ищем открывающую — в пределах своей строки: разметка через перенос не
    // тянется.
    int open = -1;
    for (int i = end - 2; i >= 0; --i) {
        const QChar c = text.at(i);
        if (c == QChar::LineSeparator) break;
        if (c == QLatin1Char('`')) {
            open = i;
            break;
        }
    }
    if (open < 0 || open + 1 == end - 1) return false;   // пусто между кавычками

    // Внутри уже размеченного куска правило молчит: там текст буквальный.
    QTextCursor probe(&doc);
    probe.setPosition(block.position() + open + 1);
    if ((probe.charFormat().intProperty(SpanStyleProperty) & SpanCode) != 0) return false;

    QTextCursor edit(&doc);
    edit.beginEditBlock();
    // Сначала закрывающая, потом открывающая: так смещения не разъезжаются.
    edit.setPosition(block.position() + end - 1);
    edit.setPosition(block.position() + end, QTextCursor::KeepAnchor);
    edit.removeSelectedText();
    edit.setPosition(block.position() + open);
    edit.setPosition(block.position() + open + 1, QTextCursor::KeepAnchor);
    edit.removeSelectedText();

    edit.setPosition(block.position() + open);
    edit.setPosition(block.position() + end - 2, QTextCursor::KeepAnchor);
    QTextCharFormat code = formatForStyle(SpanCode);
    code.setFontPointSize(codePointSize(doc));
    if (!appearance().codeFamily.isEmpty())
        code.setFontFamilies({QString(appearance().codeFamily)});
    code.setBackground(appearance().codeBackground);
    edit.mergeCharFormat(code);
    edit.endEditBlock();

    cursor.setPosition(block.position() + end - 2);
    return true;
}

bool toggleBold(QTextDocument& doc, QTextCursor& cursor) {
    return toggleInlineStyle(doc, cursor, SpanBold);
}

bool toggleCode(QTextDocument& doc, QTextCursor& cursor) {
    return toggleInlineStyle(doc, cursor, SpanCode);
}

bool toggleItalic(QTextDocument& doc, QTextCursor& cursor) {
    return toggleInlineStyle(doc, cursor, SpanItalic);
}

bool toggleStrike(QTextDocument& doc, QTextCursor& cursor) {
    return toggleInlineStyle(doc, cursor, SpanStrike);
}

QTextCharFormat inlineStyleForTyping(const QTextCharFormat& current, int style) {
    const int now = current.intProperty(SpanStyleProperty);
    return formatForStyle((now & style) != 0 ? (now & ~style) : (now | style));
}

bool makeBullet(QTextDocument& doc, QTextCursor& cursor) {
    return setBlockKind(doc, cursor, {Kind::ListItem, Marker::Bullet, false});
}

bool makeOrdered(QTextDocument& doc, QTextCursor& cursor) {
    return setBlockKind(doc, cursor, {Kind::ListItem, Marker::Ordered, false});
}

bool makeTask(QTextDocument& doc, QTextCursor& cursor) {
    return setBlockKind(doc, cursor, {Kind::ListItem, Marker::Task, false});
}

bool makeParagraph(QTextDocument& doc, QTextCursor& cursor) {
    return setBlockKind(doc, cursor, {Kind::Paragraph, Marker::Bullet, false});
}

bool toggleTaskAtCursor(QTextDocument& doc, QTextCursor& cursor) {
    const BlockRange range = selectedBlocks(doc, cursor);

    // Направление задаёт первая задача выделения: остальные идут за ней.
    bool found = false;
    bool target = true;
    QTextBlock block = doc.findBlockByNumber(range.first);
    for (int i = range.first; i <= range.last && block.isValid(); ++i, block = block.next()) {
        if (isRawBlock(block) || !isTaskBlock(block)) continue;
        target = !markerOf(block).checked;
        found = true;
        break;
    }
    if (!found) return false;

    QTextCursor edit(&doc);
    edit.beginEditBlock();
    block = doc.findBlockByNumber(range.first);
    for (int i = range.first; i <= range.last && block.isValid(); ++i, block = block.next()) {
        if (isRawBlock(block) || !isTaskBlock(block)) continue;
        if (markerOf(block).checked == target) continue;
        QTextBlockFormat format = block.blockFormat();
        format.setProperty(CheckedProperty, target);
        setBlockFormat(edit, block, format);
    }
    // Ширина рамки у обеих задач одна, но выделение могло зацепить и соседей:
    // геометрию пересчитываем на всякий случай, стоит она копейки.
    normalise(doc, range);
    edit.endEditBlock();
    return true;
}

namespace {

// Разрез блока по курсору — то, что делает Enter там, где перенос строки не
// подходит. Объявлена заранее: ею пользуются оба входа.
bool hardSplit(QTextDocument& doc, QTextCursor& cursor);

// Мягкий перенос: строка внутри того же блока. В документе это разделитель
// строк, а признак говорит читателю, что вернуть надо перевод строки, а не
// чужой U+2028 из самого текста заметки.
void insertSoftBreak(QTextDocument& doc, QTextCursor& cursor, const QTextBlock& block) {
    QTextCharFormat format = block.charFormat();
    format.setProperty(BreakSourceProperty, int(BreakNewline));
    cursor.beginEditBlock();
    cursor.insertText(QString(QChar::LineSeparator), format);
    // Перенос кажется правкой внутри одного блока, но с выделением он их
    // склеивает: выделенное уходит, и рядом оказываются те, кто раньше стоял
    // порознь. Нормализуем, как после всякой правки строения.
    normalise(doc, around(cursor.blockNumber()));
    cursor.endEditBlock();
}

// Пустая ли строка, на которой стоит курсор, и сколько знаков она занимает
// вместе со своим разделителем. Ноль — строка не пуста.
//
// Пустой считается и строка из одних пробелов: человек нажал Enter, потыкал
// пробел и нажал Enter снова. Строку из пробелов внутри абзаца markdown не
// выражает — она его заканчивает, — и оставить её значило бы каждый раз
// проваливать самопроверку.
int emptyLineTail(const QTextCursor& cursor, const QTextBlock& block) {
    const QString text = block.text();
    auto blank = [](QChar c) {
        return c == QLatin1Char(' ') || c == QLatin1Char('\t') || c == QChar::Nbsp;
    };

    // Влево — до начала строки. Если по дороге попался не пробел, строка не
    // пуста, и разрезать нечего.
    int i = cursor.positionInBlock();
    int spaces = 0;
    while (i > 0 && blank(text.at(i - 1))) {
        --i;
        ++spaces;
    }
    if (i == 0 || text.at(i - 1) != QChar::LineSeparator) return 0;

    // И вправо, до конца строки: строка пуста, только если после курсора тоже
    // ничего нет. Без этой половины проверки Enter в начале СТРОКИ С ТЕКСТОМ
    // считался вторым нажатием подряд и разрезал абзац — десять нажатий давали
    // пять пустых строк вместо десяти.
    for (int k = cursor.positionInBlock(); k < text.size(); ++k) {
        if (text.at(k) == QChar::LineSeparator) break;
        if (!blank(text.at(k))) return 0;
    }
    return spaces + 1;
}

// Переносится ли строка внутри этого блока без потерь. Проверено на ядре:
// абзац, цитата и пункты списка — да; заголовок — нет, многострочного
// заголовка markdown не знает.
bool acceptsSoftBreak(const QTextBlock& block) {
    if (isRawBlock(block)) return false;
    const Kind kind = kindOf(block);
    return kind == Kind::Paragraph || kind == Kind::Quote || isList(kind);
}

}  // namespace

bool splitBlockOtherwiseAtCursor(QTextDocument& doc, QTextCursor& cursor) {
    const QTextBlock block = cursor.block();
    // Где Enter переносит строку — режем; где заводит блок — переносим.
    if (!isRawBlock(block) && kindOf(block) == Kind::Paragraph) return hardSplit(doc, cursor);
    if (!isRawBlock(block) && kindOf(block) == Kind::Quote) return hardSplit(doc, cursor);
    if (acceptsSoftBreak(block)) {
        insertSoftBreak(doc, cursor, block);
        return true;
    }
    return splitBlockAtCursor(doc, cursor);
}

namespace {

// Забор блока кода: три кавычки или три тильды и, возможно, язык за ними.
// Пусто — не забор.
bool fenceLanguage(const QString& text, QString& language) {
    if (text.size() < 3) return false;
    const QChar mark = text.at(0);
    if (mark != QLatin1Char('`') && mark != QLatin1Char('~')) return false;
    int marks = 0;
    while (marks < text.size() && text.at(marks) == mark) ++marks;
    if (marks < 3) return false;
    language = text.mid(marks).trimmed();
    // В заборе из кавычек кавычке в языке взяться неоткуда.
    return !language.contains(QLatin1Char('`'));
}

// Последняя ли это строка своего литерального блока.
bool lastLineOfLiteral(const QTextBlock& block) {
    const QTextBlock next = block.next();
    return !next.isValid() || !isContinuationBlock(next);
}

}  // namespace

bool splitBlockAtCursor(QTextDocument& doc, QTextCursor& cursor) {
    const QTextBlock block = cursor.block();

    // Три кавычки и Enter заводят блок кода — так его и пишут в файле. Язык за
    // забором переезжает в свойство блока.
    QString language;
    if (!isRawBlock(block) && kindOf(block) == Kind::Paragraph &&
        fenceLanguage(block.text(), language)) {
        QTextBlockFormat format = block.blockFormat();
        format.setProperty(KindProperty, int(Kind::Code));
        format.setProperty(InfoProperty, language);
        format.clearProperty(LevelProperty);
        format.setHeadingLevel(0);

        cursor.beginEditBlock();
        cursor.setPosition(block.position());
        cursor.setBlockFormat(format);
        cursor.setPosition(block.position());
        cursor.setPosition(block.position() + block.length() - 1, QTextCursor::KeepAnchor);
        cursor.removeSelectedText();
        normalise(doc, around(block.blockNumber()));
        cursor.endEditBlock();
        return true;
    }

    // Из блока кода выходят забором в последней строке — как это и пишут в
    // файле. Пустой строкой выйти нельзя, и это намеренно: в длинном коде
    // пустые строки разделяют логические части, и выкидывать человека из блока
    // на каждой из них было бы мучением. Забор посреди блока при этом остаётся
    // содержимым: показывать в коде разметку никто не запрещал.
    QString closing;
    const bool closedByFence = !isRawBlock(block) && kindOf(block) == Kind::Code &&
                               lastLineOfLiteral(block) && isContinuationBlock(block) &&
                               fenceLanguage(block.text(), closing) && closing.isEmpty();
    if (closedByFence) {
        QTextBlockFormat plain;
        plain.setLineHeight(block.blockFormat().lineHeight(),
                            block.blockFormat().lineHeightType());

        cursor.beginEditBlock();
        cursor.setPosition(block.position());
        cursor.setBlockFormat(plain);
        // Забор в текст не переносим: он был командой закрыть блок, а не
        // содержимым.
        cursor.setPosition(block.position());
        cursor.setPosition(block.position() + block.length() - 1, QTextCursor::KeepAnchor);
        cursor.removeSelectedText();
        normalise(doc, around(block.blockNumber()));
        cursor.endEditBlock();
        return true;
    }

    // Обычный текст и цитата: Enter переносит строку внутри абзаца. Второй
    // подряд, на пустой строке, абзац всё-таки разрезает — пустую строку внутри
    // абзаца markdown не выражает, она его и заканчивает.
    if (!isRawBlock(block) &&
        (kindOf(block) == Kind::Paragraph || kindOf(block) == Kind::Quote)) {
        const int tail = emptyLineTail(cursor, block);
        if (tail == 0) {
            insertSoftBreak(doc, cursor, block);
            return true;
        }
        // Пустую строку убираем вместе с её разделителем: она была не
        // содержимым, а вторым нажатием Enter.
        cursor.beginEditBlock();
        for (int k = 0; k < tail; ++k) cursor.deletePreviousChar();
        const bool done = hardSplit(doc, cursor);
        cursor.endEditBlock();
        return done;
    }

    return hardSplit(doc, cursor);
}

namespace {

bool hardSplit(QTextDocument& doc, QTextCursor& cursor) {
    const QTextBlock block = cursor.block();
    const QTextBlockFormat format = block.blockFormat();
    const int number = block.blockNumber();

    // Пустой пункт списка: Enter снимает список, а не заводит ещё один пустой
    // пункт. Иначе выйти из списка можно было бы только двумя нажатиями.
    //
    // А если за пустым пунктом идёт ещё пункт, то список не кончается, а
    // разрывается: сам пустой пункт исчезает, и список снимается со СЛЕДУЮЩЕГО
    // пункта. Он становится абзацем — и разделяет списки.
    //
    // Пустой абзац для этого не годится, и это проверено на ядре: пустая строка
    // между пунктами не разделяет ничего, "- раз\n- два" и "- раз\n\n- два"
    // дают тот же самый IR. Писать в файл нечего, и разрыв, который человек
    // видел на экране, пропадал при первом же сохранении. Разделяет только
    // абзац с содержимым — а вокруг него пустые строки работают как обычно.
    if (isListBlock(block) && block.text().isEmpty()) {
        const QTextBlock next = block.next();
        const bool splitsList = next.isValid() && isListBlock(next);
        const QTextBlock target = splitsList ? next : block;

        QTextBlockFormat plain = target.blockFormat();
        plain.clearProperty(KindProperty);
        plain.clearProperty(LevelProperty);
        plain.setLeftMargin(0);

        cursor.beginEditBlock();
        cursor.setPosition(target.position());
        cursor.setBlockFormat(plain);
        if (splitsList) {
            // Пустой пункт был лишь способом сказать "разорви здесь".
            cursor.setPosition(block.position());
            cursor.setPosition(block.position() + block.length(), QTextCursor::KeepAnchor);
            cursor.removeSelectedText();
        }
        normalise(doc, around(number));
        cursor.endEditBlock();
        return true;
    }

    const bool literal = isRawBlock(block) || kindOf(block) == Kind::Code;
    QTextBlockFormat next = format;

    const bool wasLast = literal && lastLineOfLiteral(block);
    if (literal) {
        // Строка литерального блока: новая строка того же блока, а не новый
        // блок кода. Признак завершающего перевода переезжает на неё — она
        // теперь последняя.
        //
        // И ставится, даже если у прежней его не было: без него текст блока
        // кончался бы одним переводом строки, а такой текст при сборке даёт
        // одну строку, и только что заведённая пустая строка исчезала бы на
        // глазах.
        next.setProperty(ContinuationProperty, true);
        if (wasLast) next.setProperty(TrailingNewlineProperty, true);
        else next.clearProperty(TrailingNewlineProperty);
    } else {
        next.clearProperty(ContinuationProperty);
        next.clearProperty(TrailingNewlineProperty);
        // Новый пункт всегда невыполненный: отмечать за человека нечего.
        if (isTaskBlock(block)) next.setProperty(CheckedProperty, false);
        switch (kindOf(block)) {
            case Kind::Heading:
                // За заголовком идёт обычный текст, а не второй заголовок.
                next.clearProperty(KindProperty);
                next.setHeadingLevel(0);
                break;
            case Kind::Paragraph:
            case Kind::Code:
            case Kind::Quote:
            case Kind::VSpace:
            case Kind::ListItem:
                break;
        }
    }

    // Отступ предыдущей строки — в коде он почти всегда тот же, и набирать его
    // заново на каждой строке мучительно. Всё внутри одного edit block: Enter с
    // отступом отменяется одним Ctrl+Z.
    QString indent;
    if (literal) {
        const QString line = block.text();
        int i = 0;
        while (i < line.size() && (line.at(i) == QLatin1Char(' ') ||
                                   line.at(i) == QLatin1Char('\t')))
            ++i;
        // Отступ берём только до курсора: если он левее отступа, копировать
        // нечего.
        indent = line.left(qMin(i, cursor.positionInBlock()));
    }

    // Enter в начале пункта заводит пустой пункт НАД текущим, и курсор остаётся
    // в нём. Это одно решение сразу для двух задач.
    //
    // Вставить пункт между двумя — встать в начало второго, нажать Enter и
    // печатать: курсор уже там, где нужно.
    //
    // Разлепить два слипшихся списка — там же нажать Enter дважды: первый
    // заводит пустой пункт, второй снимает с него список, и получается абзац.
    // Абзац списки и разделяет — иначе никак, это проверено на ядре: два
    // соседних списка одного семейства markdown не различает вовсе.
    //
    // Если бы курсор уезжал вниз с текстом, не работало бы ни то ни другое:
    // печатать пришлось бы не там, а второй Enter заводил бы ещё один пустой
    // пункт вместо разрыва — от этого они и множились.
    const bool atListStart = isListBlock(block) && cursor.positionInBlock() == 0;

    cursor.beginEditBlock();
    // Половинки просто разъезжаются. Пустую строку между ними, если markdown её
    // требует, поставит нормализующий проход — правило записано там одно на все
    // операции, и второй его копии здесь быть не должно.
    cursor.insertBlock(next, block.charFormat());
    if (!indent.isEmpty()) cursor.insertText(indent, block.charFormat());

    // Номера блоков считаем после правки, а не до. При выделении из нескольких
    // блоков разрез его же и съедает, и номера съезжают: взятые заранее
    // указывали бы мимо, и нормализация проходила бы не по тому месту.
    const int landed = cursor.blockNumber();
    if (literal && format.boolProperty(TrailingNewlineProperty) && landed > 0) {
        QTextBlockFormat head = format;
        head.clearProperty(TrailingNewlineProperty);
        QTextCursor headCursor(&doc);
        headCursor.setPosition(doc.findBlockByNumber(landed - 1).position());
        headCursor.setBlockFormat(head);
    }
    // Место, куда встать, держим курсором: нормализация может завести пустую
    // строку выше, и номер устареет прямо посреди операции.
    QTextCursor above(&doc);
    if (landed > 0) above.setPosition(doc.findBlockByNumber(landed - 1).position());
    normalise(doc, {landed - 1, landed + 1});
    if (atListStart && landed > 0) cursor.setPosition(above.position());
    cursor.endEditBlock();
    return true;
}

}  // namespace

bool unwrapListItemAtCursor(QTextDocument& doc, QTextCursor& cursor) {
    if (cursor.hasSelection() || !cursor.atBlockStart()) return false;
    const QTextBlock block = cursor.block();
    if (!isListBlock(block)) return false;

    // Сливаемся с предыдущим пунктом, если он из ТОГО ЖЕ списка: тот же вид
    // маркера и тот же уровень. Так Backspace ведёт себя всюду, и это привычнее,
    // чем превращение пункта в абзац на месте.
    //
    // Из чужого списка — не сливаемся. Буллет, притянутый к вложенной задаче,
    // давал "- [ ] вложенная задачаБуллет": строение при этом рушится молча, а
    // человек всего лишь хотел снять маркер. В таком случае маркер и снимаем.
    const QTextBlock previous = block.previous();
    const bool sameList = previous.isValid() && isListBlock(previous) &&
                          levelOf(previous) == levelOf(block) &&
                          isOrderedBlock(previous) == isOrderedBlock(block) &&
                          isTaskBlock(previous) == isTaskBlock(block);
    if (sameList && !isContinuationBlock(block)) {
        const int join = previous.position() + previous.length() - 1;
        QTextCursor edit(&doc);
        edit.beginEditBlock();
        // Убираем границу блоков; текст пункта уезжает в конец предыдущего сам.
        edit.setPosition(join);
        edit.setPosition(block.position(), QTextCursor::KeepAnchor);
        edit.removeSelectedText();
        normalise(doc, around(previous.blockNumber()));
        edit.endEditBlock();
        cursor.setPosition(join);
        return true;
    }

    QTextBlockFormat plain = block.blockFormat();
    plain.clearProperty(KindProperty);
    plain.clearProperty(LevelProperty);
    plain.setLeftMargin(0);

    cursor.beginEditBlock();
    cursor.setBlockFormat(plain);
    normalise(doc, around(block.blockNumber()));
    cursor.endEditBlock();
    return true;
}

namespace {

// Снимает границу между этим блоком и следующим, ставя вместо неё мягкий
// перенос: два блока становятся одним, а на экране ничего не двигается.
void joinWithNext(QTextDocument& doc, QTextCursor& edit, int number) {
    const QTextBlock head = doc.findBlockByNumber(number);
    if (!head.isValid() || !head.next().isValid()) return;
    const int at = head.position() + head.length() - 1;
    QTextCharFormat breakFormat = head.charFormat();
    breakFormat.setProperty(BreakSourceProperty, int(BreakNewline));
    edit.setPosition(at);
    edit.deleteChar();
    edit.insertText(QString(QChar::LineSeparator), breakFormat);
}

// Убирает блок пустой строки и, если без неё соседи в файле слиплись бы,
// сливает их в один блок с мягким переносом.
//
// Слияние здесь не прихоть, а следствие инварианта: обязательную пустую строку
// нормализующий проход тут же вернул бы на место, и клавиша выглядела бы
// сломанной. А слитые половинки — ровно то, что человек и видит: две строки
// подряд без пустой между ними.
bool removeVSpaceAndMaybeJoin(QTextDocument& doc, QTextCursor& cursor, int gapNumber) {
    const QTextBlock gap = doc.findBlockByNumber(gapNumber);
    if (!isVSpaceBlock(gap)) return false;
    const QTextBlock before = gap.previous();
    const QTextBlock after = gap.next();
    if (!before.isValid() && !after.isValid()) return false;
    const bool join = blocksWouldMerge(before, after);

    // Куда встать после правки — началом того, что стояло под пустой строкой.
    // Курсором, а не числом: при слиянии оно само приедет на стык половинок, и
    // считать смещения не приходится.
    QTextCursor landing(&doc);
    if (after.isValid()) landing.setPosition(after.position());
    else landing.setPosition(before.position() + before.length() - 1);

    QTextCursor edit(&doc);
    edit.beginEditBlock();
    if (before.isValid()) {
        // Снимаем границу перед пустой строкой: содержимого в ней нет, и
        // предыдущий блок остаётся при своём формате.
        edit.setPosition(gap.position() - 1);
        edit.setPosition(gap.position(), QTextCursor::KeepAnchor);
        edit.removeSelectedText();
    } else if (after.isValid()) {
        // Пустая строка первая в документе: убираем границу за ней, а формат
        // берём у соседа — иначе весь его текст остался бы пустой строкой.
        const QTextBlockFormat keep = after.blockFormat();
        edit.setPosition(gap.position());
        edit.setPosition(gap.position() + 1, QTextCursor::KeepAnchor);
        edit.removeSelectedText();
        edit.setPosition(0);
        edit.setBlockFormat(keep);
    }

    if (join) joinWithNext(doc, edit, gapNumber - 1);
    normalise(doc, around(qMax(0, gapNumber - 1)));
    edit.endEditBlock();
    cursor.setPosition(landing.position());
    return true;
}

}  // namespace

bool repairAfterTyping(QTextDocument& doc, QTextCursor& cursor) {
    const QTextBlock block = cursor.block();
    const int number = block.blockNumber();
    // Набрали прямо на пустой строке: пустой строкой она быть перестала.
    const bool filled = isVSpaceBlock(block) && !block.text().isEmpty();
    // Или набрали поверх выделения, съевшего границу блоков, и рядом оказались
    // соседи, которых markdown раздельно не выражает.
    const bool mergesAhead =
        blocksWouldMerge(block, doc.findBlockByNumber(number + 1));
    const bool mergesBehind =
        number > 0 && blocksWouldMerge(doc.findBlockByNumber(number - 1), block);
    // И осиротевший вложенный пункт: набор поверх выделения съедает границу
    // блоков, и то, что стояло под пунктом, может остаться без родителя.
    const QTextBlock next = doc.findBlockByNumber(number + 1);
    const bool levelJump =
        isListBlock(next) && levelOf(next) > (isListBlock(block) ? levelOf(block) : -1) + 1;
    // И сам блок мог остаться с уровнем от пункта, которого больше нет: набор
    // поверх выделения съедает и пункты.
    const bool orphan =
        !isListBlock(block) && levelOf(block) >= 0 && levelAbove(doc, number) < levelOf(block);
    if (!filled && !mergesAhead && !mergesBehind && !levelJump && !orphan) return false;

    QTextCursor edit(&doc);
    edit.beginEditBlock();
    if (filled) {
        QTextBlockFormat format = block.blockFormat();
        format.clearProperty(KindProperty);
        format.clearProperty(LevelProperty);
        format.setLeftMargin(0);
        setBlockFormat(edit, block, format);
    }

    // Соседей сливаем, а не раздвигаем пустой строкой: набор ничего на экране
    // раздвигать не должен. Строки как стояли, так и стоят, меняется только
    // строение — три строки подряд без пустой между ними markdown и называет
    // одним абзацем.
    if (blocksWouldMerge(doc.findBlockByNumber(number), doc.findBlockByNumber(number + 1)))
        joinWithNext(doc, edit, number);
    if (number > 0 &&
        blocksWouldMerge(doc.findBlockByNumber(number - 1), doc.findBlockByNumber(number)))
        joinWithNext(doc, edit, number - 1);

    normalise(doc, around(qMax(0, number - 1)));
    edit.endEditBlock();
    return true;
}

bool joinAcrossVSpaceBackward(QTextDocument& doc, QTextCursor& cursor) {
    if (cursor.hasSelection() || !cursor.atBlockStart()) return false;
    const QTextBlock block = cursor.block();
    // Курсор стоит на самой пустой строке: Backspace убирает её и уводит курсор
    // в конец предыдущей — как в любом редакторе.
    if (isVSpaceBlock(block)) {
        if (!block.previous().isValid()) return false;   // выше ничего нет
        return removeVSpaceAndMaybeJoin(doc, cursor, block.blockNumber());
    }
    const QTextBlock gap = block.previous();
    if (!isVSpaceBlock(gap)) return false;
    return removeVSpaceAndMaybeJoin(doc, cursor, gap.blockNumber());
}

bool joinAcrossVSpaceForward(QTextDocument& doc, QTextCursor& cursor) {
    if (cursor.hasSelection() || !cursor.atBlockEnd()) return false;
    const QTextBlock block = cursor.block();
    // На самой пустой строке Delete подтягивает следующую наверх — то же
    // самое, что убрать эту пустую строку.
    if (isVSpaceBlock(block)) {
        if (!block.next().isValid()) return false;
        return removeVSpaceAndMaybeJoin(doc, cursor, block.blockNumber());
    }
    const QTextBlock gap = block.next();
    if (!isVSpaceBlock(gap)) return false;
    return removeVSpaceAndMaybeJoin(doc, cursor, gap.blockNumber());
}

void applyListGeometry(QTextDocument& doc, BlockRange range) {
    const BlockRange full = expandToRuns(doc, range);
    const QFont base = baseFontOf(doc);
    const qreal charUnit = QFontMetricsF(base).horizontalAdvance(QLatin1Char('A'));
    const qreal indent = appearance().listIndent * charUnit;

    // Первый проход: к какой колонке принадлежит каждый блок и какой маркер в
    // ней самый широкий. Задаёт колонку именно он: иначе под "10." текст
    // начинался бы правее, чем под "1.", и левый край списка выходил бы рваным.
    //
    // Колонка кончается там же, где список (номер снова единица), — и ещё там,
    // где меняется сам маркер. Буллеты и задачи для нумерации одна семья, и
    // раньше они делили колонку: кружки равнялись по ширине чекбокса, а стоило
    // отцепить их от задач — прыгали влево. Ширина кружка от соседей зависеть
    // не должна.
    // Метка «этот блок пропускаем, ничего не меняя» — ею помечены пустые строки.
    constexpr int kSkip = -2;
    // Метка «блок стоит внутри пункта»: колонку он берёт у своего пункта, а сам
    // ни в какой прогон не входит и ширину маркеров не задаёт.
    constexpr int kInside = -3;
    std::vector<int> runOf;
    std::vector<qreal> widest;
    {
        ListRuns runs;
        std::vector<int> currentRun;
        std::vector<int> currentFamily;
        QTextBlock block = doc.findBlockByNumber(full.first);
        for (int i = full.first; i <= full.last && block.isValid(); ++i, block = block.next()) {
            // Пустая строка ни к какой колонке не принадлежит и прогон не рвёт:
            // помечаем её и идём дальше, ничего не сбрасывая.
            if (isVSpaceBlock(block)) {
                runOf.push_back(kSkip);
                continue;
            }
            if (!isListBlock(block)) {
                // Блок внутри пункта список не заканчивает: прогон живёт дальше.
                if (levelOf(block) >= 0) {
                    runOf.push_back(kInside);
                    continue;
                }
                runs.reset();
                currentRun.clear();
                currentFamily.clear();
                runOf.push_back(-1);
                continue;
            }
            const MarkerStyle style = markerOf(block);
            const int level = qMax(0, levelOf(block));
            const bool ordered = style.marker == Marker::Ordered;
            const int ordinal = runs.next(level, ordered);
            const int family = ordered ? 2 : (style.marker == Marker::Task ? 1 : 0);
            if (int(currentRun.size()) <= level) {
                currentRun.resize(size_t(level) + 1, -1);
                currentFamily.resize(size_t(level) + 1, -1);
            }
            if (ordinal == 1 || currentRun[size_t(level)] < 0 ||
                currentFamily[size_t(level)] != family) {
                currentRun[size_t(level)] = int(widest.size());
                currentFamily[size_t(level)] = family;
                widest.push_back(0.0);
            }
            const int run = currentRun[size_t(level)];
            runOf.push_back(run);
            widest[size_t(run)] =
                qMax(widest[size_t(run)], markerColumn(style, ordinal, base));
        }
    }

    // Второй проход: колонка текста каждого уровня. Уровень глубже родителя не
    // больше чем на единицу (это инвариант), поэтому к моменту чтения ячейка
    // всегда заполнена родителем.
    std::vector<qreal> contentCol(1, 0.0);
    QTextCursor cursor(&doc);
    QTextBlock block = doc.findBlockByNumber(full.first);
    for (int i = full.first; i <= full.last && block.isValid(); ++i, block = block.next()) {
        const int run = runOf[size_t(i - full.first)];
        if (run == kSkip) continue;
        if (run == kInside) {
            // Колонка своего пункта — та самая, от которой начинается его текст.
            // К ней прибавляется собственный отступ блока: у кода и цитаты он
            // свой, и внутри пункта он тоже нужен.
            const size_t at = size_t(qMax(0, levelOf(block))) + 1;
            if (at >= contentCol.size()) continue;
            qreal own = 0;
            switch (kindOf(block)) {
                case Kind::Code:  own = appearance().codeIndent * charUnit; break;
                case Kind::Quote: own = appearance().quoteIndent * charUnit; break;
                case Kind::Paragraph:
                case Kind::Heading:
                case Kind::VSpace:
                case Kind::ListItem:
                    break;
            }
            const qreal margin = indent + contentCol[at] + own;
            QTextBlockFormat format = block.blockFormat();
            if (std::fabs(format.leftMargin() - margin) < 0.01) continue;
            format.setLeftMargin(margin);
            setBlockFormat(cursor, block, format);
            continue;
        }
        if (run < 0) {
            contentCol.assign(1, 0.0);
            continue;
        }

        const int level = qMax(0, levelOf(block));
        if (int(contentCol.size()) <= level + 1) contentCol.resize(size_t(level) + 2, 0.0);

        const qreal cell = widest[size_t(run)];
        contentCol[size_t(level) + 1] = contentCol[size_t(level)] + cell;

        const qreal margin = indent + contentCol[size_t(level)] + cell;
        QTextBlockFormat format = block.blockFormat();
        // Не трогаем формат, если поле и так верное: любая запись помечает
        // документ изменённым и тянет за собой автосохранение.
        if (std::fabs(format.leftMargin() - margin) < 0.01) continue;
        format.setLeftMargin(margin);
        setBlockFormat(cursor, block, format);
    }
}

void syncLists(QTextDocument& doc, BlockRange range) {
    const BlockRange full = expandToRuns(doc, range);

    QTextCursor cursor(&doc);
    cursor.beginEditBlock();

    // Уровни приводятся к допустимым сдвигом, а не обрезкой: обрезка ломает
    // структуру. Два пункта с уровнем 2 подряд — это братья; обрежь их
    // поодиночке «не глубже предыдущего плюс один», и второй станет ребёнком
    // первого. Поэтому держим стопку открытых уровней исходника: глубина стопки
    // и есть уровень в документе, а равный уровень исходника означает брата.
    std::vector<int> open;
    QTextBlock block = doc.findBlockByNumber(full.first);
    for (int i = full.first; i <= full.last && block.isValid(); ++i, block = block.next()) {
        if (isVSpaceBlock(block)) continue;   // просторный список — тот же список
        if (!isListBlock(block)) {
            const int inside = levelOf(block);
            if (inside < 0) {
                open.clear();
                continue;
            }
            // Блок внутри пункта: глубже открытого уровня ему быть не с чего, а
            // без списка вокруг он и вовсе обычный абзац.
            const int deepest = int(open.size()) - 1;
            if (inside == deepest) continue;
            QTextBlockFormat format = block.blockFormat();
            if (deepest < 0) {
                format.clearProperty(LevelProperty);
                format.setLeftMargin(0);
            } else {
                format.setProperty(LevelProperty, deepest);
            }
            setBlockFormat(cursor, block, format);
            continue;
        }
        const int was = qMax(0, levelOf(block));
        while (!open.empty() && open.back() > was) open.pop_back();
        if (open.empty() || open.back() < was) open.push_back(was);

        const int level = int(open.size()) - 1;
        if (level != levelOf(block)) {
            QTextBlockFormat format = block.blockFormat();
            format.setProperty(LevelProperty, level);
            setBlockFormat(cursor, block, format);
        }
    }

    applyListGeometry(doc, full);
    cursor.endEditBlock();
}

// Может ли этот блок быть продолжением предыдущего.
static bool mayContinue(const QTextBlock& block, const QTextBlock& prev) {
    if (!prev.isValid()) return false;
    if (isRawBlock(block) != isRawBlock(prev)) return false;
    if (isRawBlock(block)) return true;
    // Строки одного блока кода. Заголовок и абзац продолжений не имеют вовсе:
    // они лежат в документе одним блоком.
    return kindOf(block) == Kind::Code && kindOf(prev) == Kind::Code;
}

namespace {

// Заводит пустую строку перед этим блоком. Курсоры, стоящие на его начале,
// Qt переносит на текст сам, поэтому вызывающему поправлять их не нужно.
void insertVSpaceBefore(QTextDocument& doc, int number) {
    const QTextBlock block = doc.findBlockByNumber(number);
    if (!block.isValid()) return;

    // Разрез в самом начале блока: содержимое уезжает во второй кусок вместе со
    // своим форматом, а первый остаётся пустым — ему и достаётся пустая строка.
    QTextCursor edit(&doc);
    edit.setPosition(block.position());
    edit.insertBlock(block.blockFormat(), block.charFormat());

    QTextCursor fix(&doc);
    fix.setPosition(doc.findBlockByNumber(number).position());
    fix.setBlockFormat(vspaceBlockFormat(doc, false, number == 0));
}

}  // namespace

int syncGaps(QTextDocument& doc, BlockRange range) {
    int added = 0;
    QTextCursor edit(&doc);
    edit.beginEditBlock();

    // Пустая строка, в которую попал текст, пустой строкой быть перестала. Так
    // выглядит набор поверх пустой строки и слияние блоков через неё.
    {
        int i = qMax(0, range.first);
        const int last = qMin(range.last, doc.blockCount() - 1);
        for (; i <= last; ++i) {
            const QTextBlock block = doc.findBlockByNumber(i);
            if (!isVSpaceBlock(block) || block.text().isEmpty()) continue;
            QTextBlockFormat format = block.blockFormat();
            format.clearProperty(KindProperty);
            format.clearProperty(LevelProperty);
            format.setLeftMargin(0);
            setBlockFormat(edit, block, format);
        }
    }

    // Между блоками, которые в файле слиплись бы, обязана стоять пустая строка.
    // Смотрим и на стык за концом диапазона: операция могла свести новых соседей.
    {
        int i = qMax(1, range.first);
        int last = qMin(range.last + 1, doc.blockCount() - 1);
        for (; i <= last && i < doc.blockCount(); ++i) {
            const QTextBlock block = doc.findBlockByNumber(i);
            // Строки одного литерального блока стоят вплотную по своей природе.
            if (isContinuationBlock(block)) continue;
            if (!blocksWouldMerge(block.previous(), block)) continue;
            insertVSpaceBefore(doc, i);
            ++added;
            ++i;      // на месте i теперь только что заведённая пустая строка
            ++last;
        }
    }

    // Поля сверху: их держит соседство, и после вставки они могли устареть.
    {
        const qreal lineUnit = QFontMetricsF(baseFontOf(doc)).height();
        int i = qMax(0, range.first);
        const int last = qMin(range.last + 2, doc.blockCount() - 1);
        for (; i <= last; ++i) {
            const QTextBlock block = doc.findBlockByNumber(i);
            if (!block.isValid()) break;
            const qreal want =
                isContinuationBlock(block)
                    ? 0.0
                    : blockTopMargin(kindOf(block), isRawBlock(block),
                                     isVSpaceBlock(block.previous()), i == 0) *
                          lineUnit;
            QTextBlockFormat format = block.blockFormat();
            // Не трогаем формат, если поле и так верное: любая запись помечает
            // документ изменённым и тянет за собой автосохранение.
            if (std::fabs(format.topMargin() - want) < 0.01) continue;
            format.setTopMargin(want);
            setBlockFormat(edit, block, format);
        }
    }

    edit.endEditBlock();
    return added;
}

bool gapInvariantHolds(const QTextDocument& doc, QString* problem) {
    int number = 0;
    for (QTextBlock block = doc.begin(); block.isValid(); block = block.next(), ++number) {
        if (isContinuationBlock(block)) continue;
        if (isVSpaceBlock(block) && !block.text().isEmpty()) {
            if (problem != nullptr)
                *problem = QStringLiteral("блок %1: пустая строка с текстом").arg(number);
            return false;
        }
        if (!blocksWouldMerge(block.previous(), block)) continue;
        if (problem != nullptr) {
            *problem = QStringLiteral("блок %1: слипся бы с предыдущим, а пустой строки нет")
                           .arg(number);
        }
        return false;
    }
    return true;
}

void syncLiteralBlocks(QTextDocument& doc, BlockRange range) {
    const int count = doc.blockCount();
    const int first = qBound(0, range.first, count - 1);
    const int last = qBound(first, range.last, count - 1);

    QTextCursor cursor(&doc);
    cursor.beginEditBlock();
    QTextBlock block = doc.findBlockByNumber(first);
    for (int i = first; i <= last && block.isValid(); ++i, block = block.next()) {
        if (!isContinuationBlock(block)) continue;
        if (mayContinue(block, block.previous())) continue;
        QTextBlockFormat format = block.blockFormat();
        format.clearProperty(ContinuationProperty);
        setBlockFormat(cursor, block, format);
    }
    cursor.endEditBlock();
}

bool literalInvariantHolds(const QTextDocument& doc, QString* problem) {
    int number = 0;
    for (QTextBlock block = doc.begin(); block.isValid(); block = block.next(), ++number) {
        if (!isContinuationBlock(block)) continue;
        if (mayContinue(block, block.previous())) continue;
        if (problem != nullptr) {
            *problem = number == 0
                           ? QStringLiteral("блок 0 помечен продолжением, а продолжать нечего")
                           : QStringLiteral("блок %1: продолжение при несовместимом предыдущем")
                                 .arg(number);
        }
        return false;
    }
    return true;
}

bool listInvariantHolds(const QTextDocument& doc, QString* problem) {
    int prevLevel = -1;
    int number = 0;
    for (QTextBlock block = doc.begin(); block.isValid(); block = block.next(), ++number) {
        if (isVSpaceBlock(block)) continue;   // прогон она не рвёт — см. сериализатор
        if (!isListBlock(block)) {
            const int inside = levelOf(block);
            if (inside < 0) {
                prevLevel = -1;
                continue;
            }
            // Блок внутри пункта живёт на уровне уже открытого пункта: открыть
            // список сам он не может, у него нет маркера.
            if (inside > prevLevel) {
                if (problem != nullptr) {
                    *problem =
                        QStringLiteral("блок %1: уровень %2 внутри пункта при уровне %3 выше")
                            .arg(number)
                            .arg(inside)
                            .arg(prevLevel);
                }
                return false;
            }
            prevLevel = inside;
            continue;
        }
        const int level = levelOf(block);
        if (level < 0 || level > prevLevel + 1) {
            if (problem != nullptr) {
                *problem = QStringLiteral("блок %1: уровень %2 при уровне %3 у предыдущего")
                               .arg(number)
                               .arg(level)
                               .arg(prevLevel);
            }
            return false;
        }
        prevLevel = level;
    }
    return true;
}

}  // namespace zametti
