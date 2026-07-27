#include "editor_ops.h"

#include "doc_model.h"
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
BlockRange expandToRuns(const QTextDocument& doc, BlockRange range) {
    const int count = doc.blockCount();
    range.first = qBound(0, range.first, count - 1);
    range.last = qBound(range.first, range.last, count - 1);

    while (range.first > 0 && isListBlock(doc.findBlockByNumber(range.first - 1)))
        --range.first;
    while (range.last + 1 < count && isListBlock(doc.findBlockByNumber(range.last + 1)))
        ++range.last;
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

// Диапазон, который надо привести в порядок после правки одного блока: сам блок
// и его соседи. Дальше расширят сами нормализующие проходы.
BlockRange around(int number) { return {number - 1, number + 1}; }

void normalise(QTextDocument& doc, BlockRange range) {
    syncLiteralBlocks(doc, range);
    syncLists(doc, range);
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
    const QTextBlock last = doc.findBlockByNumber(range.last);
    if (isListBlock(last)) {
        const int base = levelOf(last);
        while (range.last + 1 < doc.blockCount()) {
            const QTextBlock next = doc.findBlockByNumber(range.last + 1);
            if (!isListBlock(next) || levelOf(next) <= base) break;
            ++range.last;
        }
    }
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

bool indentListItems(QTextDocument& doc, QTextCursor& cursor) {
    const BlockRange range = selectedBlocks(doc, cursor);
    const QTextBlock first = doc.findBlockByNumber(range.first);
    if (!isListBlock(first)) return false;

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
    if (!isListBlock(first) || levelOf(first) == 0) return false;

    shiftLevels(doc, range, -1);
    return true;
}

namespace {

// Последний блок поддерева этого пункта: всё, что следом и глубже.
int subtreeEnd(const QTextDocument& doc, int number) {
    const int level = levelOf(doc.findBlockByNumber(number));
    int last = number;
    while (last + 1 < doc.blockCount()) {
        const QTextBlock next = doc.findBlockByNumber(last + 1);
        if (!isListBlock(next) || levelOf(next) <= level) break;
        ++last;
    }
    return last;
}

// Пункт того же уровня перед этим. Отрицательное — соседа нет: либо кончился
// список, либо мы вышли на уровень выше.
int previousSibling(const QTextDocument& doc, int number) {
    const int level = levelOf(doc.findBlockByNumber(number));
    for (int i = number - 1; i >= 0; --i) {
        const QTextBlock block = doc.findBlockByNumber(i);
        if (!isListBlock(block)) return -1;      // через абзац не прыгаем
        const int other = levelOf(block);
        if (other < level) return -1;            // вышли из своего уровня
        if (other == level) return i;
    }
    return -1;
}

int nextSibling(const QTextDocument& doc, int number) {
    const int level = levelOf(doc.findBlockByNumber(number));
    const int after = subtreeEnd(doc, number) + 1;
    if (after >= doc.blockCount()) return -1;
    const QTextBlock block = doc.findBlockByNumber(after);
    if (!isListBlock(block) || levelOf(block) != level) return -1;
    return after;
}

}  // namespace

MoveResult moveListItem(const QTextDocument& doc, const QTextCursor& cursor, int direction) {
    const QTextBlock block = cursor.block();
    if (!isListBlock(block)) return {};

    const int number = block.blockNumber();
    const int sibling = direction < 0 ? previousSibling(doc, number) : nextSibling(doc, number);
    if (sibling < 0) return {};

    // Оба куска — пункт с поддеревом; в IR они лежат подряд, потому что
    // списочные блоки один к одному с блоками IR.
    const int selfFirst = number;
    const int selfLast = subtreeEnd(doc, number);
    const int otherFirst = sibling;
    const int otherLast = subtreeEnd(doc, sibling);

    const int firstIr = irIndexOfBlock(doc.findBlockByNumber(qMin(selfFirst, otherFirst)));
    const int selfSize = selfLast - selfFirst + 1;
    const int otherSize = otherLast - otherFirst + 1;

    MoveResult result;
    result.doc = readDocument(doc);
    if (firstIr + selfSize + otherSize > int(result.doc.size())) return {};

    // Перестановка двух соседних кусков — это поворот их объединения.
    const auto begin = result.doc.begin() + firstIr;
    const auto middle = begin + (direction < 0 ? otherSize : selfSize);
    const auto end = begin + selfSize + otherSize;
    std::rotate(begin, middle, end);

    // Пункт переехал на размер соседа: вверх — назад, вниз — вперёд.
    const int selfIr = irIndexOfBlock(block);
    result.irBlock = direction < 0 ? selfIr - otherSize : selfIr + otherSize;
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

// Внутри встроенного кода разметки не бывает, а литеральные блоки буквальны
// целиком — такие куски в выделение не берём вовсе.
std::vector<StyleRun> styleRuns(const QTextDocument& doc, int from, int to) {
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
            if ((style & SpanCode) != 0) continue;
            runs.push_back({start, end, style});
        }
    }
    return runs;
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

    const std::vector<StyleRun> runs = styleRuns(doc, from, to);
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

// Метка задачи, которой она становится в тексте при переходе в род, где
// чекбокса не бывает. В нумерованном пункте "[x] " — обычный текст, это
// проверено на ядре, поэтому отметка переживает такой переход.
QString taskMark(Kind kind) {
    return kind == Kind::TaskChecked ? QStringLiteral("[x] ") : QStringLiteral("[ ] ");
}

// Смена рода одного блока. Блок берётся по номеру, а не ссылкой: вставка и
// удаление внутри цикла двигают позиции, и заранее взятый блок устарел бы.
// Возвращает false, если блок трогать не следует.
bool convertBlock(QTextDocument& doc, int number, Kind target) {
    const QTextBlock block = doc.findBlockByNumber(number);
    if (!block.isValid() || isRawBlock(block)) return false;
    const Kind from = kindOf(block);
    if (from == Kind::Code) return false;   // код списком быть не может

    QTextBlockFormat format = block.blockFormat();
    QString insert;
    int strip = 0;

    if (target == Kind::Paragraph) {
        format.clearProperty(KindProperty);
        format.clearProperty(LevelProperty);
        format.setLeftMargin(0);
        format.setHeadingLevel(0);
    } else {
        Kind actual = target;
        // Отметка задачи не должна пропадать молча: в нумерованном пункте она
        // становится обычным текстом в начале содержимого. Что "[x] " там
        // именно текст, а не чекбокс, проверено на ядре.
        if (isTaskBlock(block) && target == Kind::Ordered) insert = taskMark(from);
        // Обратный ход: буллет, содержимое которого начинается с отметки, файл
        // всё равно прочтёт задачей. Делаем задачу сразу и убираем отметку из
        // текста — иначе документ разошёлся бы с тем, что окажется на диске.
        if (target == Kind::Bullet) {
            const QString text = block.text();
            if (text.startsWith(QStringLiteral("[x] ")) ||
                text.startsWith(QStringLiteral("[X] "))) {
                actual = Kind::TaskChecked;
                strip = 4;
            } else if (text.startsWith(QStringLiteral("[ ] "))) {
                actual = Kind::TaskUnchecked;
                strip = 4;
            }
        }
        format.setProperty(KindProperty, int(actual));
        format.setProperty(LevelProperty, isListBlock(block) ? levelOf(block) : 0);
        format.setHeadingLevel(0);
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

bool setBlockKind(QTextDocument& doc, QTextCursor& cursor, Kind target) {
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
    Kind kind = Kind::Paragraph;
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
Kind matchCheckbox(const QString& body, bool allowUnclosed, bool& matched) {
    matched = true;
    if (body == QStringLiteral("[]") || body == QStringLiteral("[ ]"))
        return Kind::TaskUnchecked;
    if (body == QStringLiteral("[x]") || body == QStringLiteral("[X]"))
        return Kind::TaskChecked;
    if (allowUnclosed && body == QStringLiteral("[")) return Kind::TaskUnchecked;
    matched = false;
    return Kind::Paragraph;
}

InputRule matchInputRule(const QTextBlock& block, const QString& typed) {
    const bool list = isListBlock(block);
    const Kind kind = kindOf(block);
    // Пробел в конце уже проверен вызывающим.
    const QString body = typed.left(typed.size() - 1);

    // Чекбокс в начале буллета. Только в буллете: в нумерованном пункте "[x]" —
    // обычный текст, и превращать его в чекбокс нельзя.
    if (kind == Kind::Bullet) {
        bool matched = false;
        const Kind task = matchCheckbox(body, false, matched);
        if (matched) return {task, 0, int(typed.size()), true};
    }

    if (list) return {};   // список списком уже не сделаешь

    // Задача одним махом: "-[", "-[]", "-[x]" и то же с пробелом после маркера.
    // Набирать "- " и ждать, пока сработает первая автозамена, не обязательно.
    if (body.size() >= 2 && isBulletMarker(body.at(0))) {
        const int after = body.at(1) == QLatin1Char(' ') ? 2 : 1;
        bool matched = false;
        // Слитно с маркером — короткий путь, недописанная скобка допустима.
        const Kind task = matchCheckbox(body.mid(after), after == 1, matched);
        if (matched) return {task, 0, int(typed.size()), true};
    }

    // Маркер буллета: любой из трёх, как и в файле. В файл уйдёт дефис — знак
    // маркера канон не хранит.
    if (body.size() == 1 && isBulletMarker(body.at(0)))
        return {Kind::Bullet, 0, 2, true};

    // Номер: цифры и точка или скобка.
    int digits = 0;
    while (digits < body.size() && body.at(digits).isDigit()) ++digits;
    if (digits > 0 && digits + 1 == body.size() &&
        (body.at(digits) == QLatin1Char('.') || body.at(digits) == QLatin1Char(')')))
        return {Kind::Ordered, 0, int(typed.size()), true};

    // Заголовок: от одной решётки до шести.
    int hashes = 0;
    while (hashes < body.size() && body.at(hashes) == QLatin1Char('#')) ++hashes;
    if (hashes >= 1 && hashes <= 6 && hashes == body.size() && kind != Kind::Heading)
        return {Kind::Heading, hashes, int(typed.size()), true};

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

    // Правило срабатывает только на пробел сразу за началом блока: набранное
    // должно быть целиком тем, что мы опознаём.
    const int typedLength = cursor.position() - block.position();
    if (typedLength <= 0 || typedLength > 8) return false;
    const QString typed = block.text().left(typedLength);
    if (!typed.endsWith(QLatin1Char(' '))) return false;

    const InputRule rule = matchInputRule(block, typed);
    if (!rule.matched) return false;

    QTextBlockFormat format = block.blockFormat();
    if (rule.kind == Kind::Heading) {
        format.clearProperty(LevelProperty);
        format.setProperty(KindProperty, int(Kind::Heading));
        format.setHeadingLevel(rule.headingLevel);
    } else {
        format.setProperty(KindProperty, int(rule.kind));
        format.setProperty(LevelProperty, isListBlock(block) ? levelOf(block) : 0);
        format.setHeadingLevel(0);
    }

    QTextCursor edit(&doc);
    edit.beginEditBlock();
    edit.setPosition(block.position());
    edit.setBlockFormat(format);
    edit.setPosition(block.position());
    edit.setPosition(block.position() + rule.prefix, QTextCursor::KeepAnchor);
    edit.removeSelectedText();
    normalise(doc, around(block.blockNumber()));
    edit.endEditBlock();

    cursor.setPosition(edit.position());
    return true;
}

bool toggleBold(QTextDocument& doc, QTextCursor& cursor) {
    return toggleInlineStyle(doc, cursor, SpanBold);
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
    return setBlockKind(doc, cursor, Kind::Bullet);
}

bool makeOrdered(QTextDocument& doc, QTextCursor& cursor) {
    return setBlockKind(doc, cursor, Kind::Ordered);
}

bool makeTask(QTextDocument& doc, QTextCursor& cursor) {
    return setBlockKind(doc, cursor, Kind::TaskUnchecked);
}

bool makeParagraph(QTextDocument& doc, QTextCursor& cursor) {
    return setBlockKind(doc, cursor, Kind::Paragraph);
}

bool toggleTaskAtCursor(QTextDocument& doc, QTextCursor& cursor) {
    const BlockRange range = selectedBlocks(doc, cursor);

    // Направление задаёт первая задача выделения: остальные идут за ней.
    bool found = false;
    Kind target = Kind::TaskUnchecked;
    QTextBlock block = doc.findBlockByNumber(range.first);
    for (int i = range.first; i <= range.last && block.isValid(); ++i, block = block.next()) {
        if (isRawBlock(block) || !isTaskBlock(block)) continue;
        target = kindOf(block) == Kind::TaskChecked ? Kind::TaskUnchecked : Kind::TaskChecked;
        found = true;
        break;
    }
    if (!found) return false;

    QTextCursor edit(&doc);
    edit.beginEditBlock();
    block = doc.findBlockByNumber(range.first);
    for (int i = range.first; i <= range.last && block.isValid(); ++i, block = block.next()) {
        if (isRawBlock(block) || !isTaskBlock(block)) continue;
        if (kindOf(block) == target) continue;
        QTextBlockFormat format = block.blockFormat();
        format.setProperty(KindProperty, int(target));
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
void insertSoftBreak(QTextCursor& cursor, const QTextBlock& block) {
    QTextCharFormat format = block.charFormat();
    format.setProperty(BreakSourceProperty, int(BreakNewline));
    cursor.beginEditBlock();
    cursor.insertText(QString(QChar::LineSeparator), format);
    cursor.endEditBlock();
}

// Стоим ли на пустой строке внутри блока: сразу за мягким переносом.
bool onEmptySoftLine(const QTextCursor& cursor, const QTextBlock& block) {
    const int inBlock = cursor.positionInBlock();
    return inBlock > 0 && block.text().at(inBlock - 1) == QChar::LineSeparator;
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
        insertSoftBreak(cursor, block);
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

    // Из блока кода выходят двумя способами: пустой строкой и Enter — как из
    // списка, — либо забором в последней строке, как это пишут в файле. Забор
    // посреди блока при этом остаётся содержимым: показывать в коде разметку
    // никто не запрещал.
    QString closing;
    const bool closedByFence = !isRawBlock(block) && kindOf(block) == Kind::Code &&
                               lastLineOfLiteral(block) && isContinuationBlock(block) &&
                               fenceLanguage(block.text(), closing) && closing.isEmpty();
    if (!isRawBlock(block) && kindOf(block) == Kind::Code &&
        (block.text().isEmpty() || closedByFence) && lastLineOfLiteral(block) &&
        isContinuationBlock(block)) {
        QTextBlockFormat plain;
        plain.setLineHeight(block.blockFormat().lineHeight(),
                            block.blockFormat().lineHeightType());

        cursor.beginEditBlock();
        cursor.setPosition(block.position());
        cursor.setBlockFormat(plain);
        // Забор в текст не переносим: он был командой закрыть блок, а не
        // содержимым.
        if (closedByFence) {
            cursor.setPosition(block.position());
            cursor.setPosition(block.position() + block.length() - 1,
                               QTextCursor::KeepAnchor);
            cursor.removeSelectedText();
        }
        normalise(doc, around(block.blockNumber()));
        cursor.endEditBlock();
        return true;
    }

    // Обычный текст и цитата: Enter переносит строку внутри абзаца. Второй
    // подряд, на пустой строке, абзац всё-таки разрезает — пустую строку внутри
    // абзаца markdown не выражает, она его и заканчивает.
    if (!isRawBlock(block) &&
        (kindOf(block) == Kind::Paragraph || kindOf(block) == Kind::Quote)) {
        if (!onEmptySoftLine(cursor, block)) {
            insertSoftBreak(cursor, block);
            return true;
        }
        // Висящий перенос убираем: он был началом этой пустой строки.
        cursor.beginEditBlock();
        cursor.deletePreviousChar();
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
    if (isListBlock(block) && block.text().isEmpty()) {
        QTextBlockFormat plain = format;
        plain.clearProperty(KindProperty);
        plain.clearProperty(LevelProperty);
        plain.setLeftMargin(0);

        cursor.beginEditBlock();
        cursor.setBlockFormat(plain);
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
        switch (kindOf(block)) {
            case Kind::TaskChecked:
                // Новый пункт всегда невыполненный: отмечать за человека нечего.
                next.setProperty(KindProperty, int(Kind::TaskUnchecked));
                break;
            case Kind::Heading:
                // За заголовком идёт обычный текст, а не второй заголовок.
                next.clearProperty(KindProperty);
                next.setHeadingLevel(0);
                break;
            default:
                break;
        }
    }

    cursor.beginEditBlock();
    cursor.insertBlock(next, block.charFormat());
    if (literal && format.boolProperty(TrailingNewlineProperty)) {
        QTextBlockFormat head = format;
        head.clearProperty(TrailingNewlineProperty);
        QTextCursor headCursor(&doc);
        headCursor.setPosition(doc.findBlockByNumber(number).position());
        headCursor.setBlockFormat(head);
    }
    normalise(doc, {number, number + 1});
    cursor.endEditBlock();
    return true;
}

}  // namespace

bool unwrapListItemAtCursor(QTextDocument& doc, QTextCursor& cursor) {
    if (cursor.hasSelection() || !cursor.atBlockStart()) return false;
    const QTextBlock block = cursor.block();
    if (!isListBlock(block)) return false;

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

void applyListGeometry(QTextDocument& doc, BlockRange range) {
    const BlockRange full = expandToRuns(doc, range);
    const QFont base = baseFontOf(doc);
    const qreal charUnit = QFontMetricsF(base).horizontalAdvance(QLatin1Char('A'));
    const qreal indent = appearance().listIndent * charUnit;

    // Первый проход: к какому прогону принадлежит каждый блок и какой маркер в
    // этом прогоне самый широкий. Колонку текста задаёт именно он: иначе под
    // "10." текст начинался бы правее, чем под "1.", и левый край списка
    // выходил бы рваным. Прогон опознаём по номеру: единица значит, что на этом
    // уровне начался новый список.
    std::vector<int> runOf;
    std::vector<qreal> widest;
    {
        ListRuns runs;
        std::vector<int> currentRun;
        QTextBlock block = doc.findBlockByNumber(full.first);
        for (int i = full.first; i <= full.last && block.isValid(); ++i, block = block.next()) {
            if (!isListBlock(block)) {
                runs.reset();
                currentRun.clear();
                runOf.push_back(-1);
                continue;
            }
            const Kind kind = kindOf(block);
            const int level = qMax(0, levelOf(block));
            const int ordinal = runs.next(level, isOrdered(kind));
            if (int(currentRun.size()) <= level) currentRun.resize(size_t(level) + 1, -1);
            if (ordinal == 1 || currentRun[size_t(level)] < 0) {
                currentRun[size_t(level)] = int(widest.size());
                widest.push_back(0.0);
            }
            const int run = currentRun[size_t(level)];
            runOf.push_back(run);
            widest[size_t(run)] =
                qMax(widest[size_t(run)], markerColumn(kind, ordinal, base));
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
        if (!isListBlock(block)) {
            open.clear();
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
        if (!isListBlock(block)) {
            prevLevel = -1;
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
