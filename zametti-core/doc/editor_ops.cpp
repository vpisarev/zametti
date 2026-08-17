#include "editor_ops.h"

#include "document_impl.h"

#include "block_object.h"
#include "doc_model.h"
#include "math_scan.h"
#include "document_pieces.h"
#include "document_builder.h"
#include "serializer.h"
#include "marker.h"
#include "settings.h"
#include "table.h"

#include <QFont>
#include <QFontMetricsF>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextDocumentFragment>

#include <algorithm>
#include <functional>
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
// Ступень кегля, на которой стоит блок. Держит её формат знаков блока — тот
// самый, что поставил сборщик.
int blockFontStep(const QTextBlock& block) {
    return block.blockFormat().hasProperty(QTextFormat::FontSizeAdjustment)
               ? block.blockFormat().intProperty(QTextFormat::FontSizeAdjustment)
               : block.charFormat().intProperty(QTextFormat::FontSizeAdjustment);
}

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
    // Заведённые пустые строки сдвигают номера блоков: диапазон для списков
    // раздвигаем ровно на столько же, иначе последний пункт остался бы
    // непроверенным — и осиротевший вложенный пункт так и уехал бы в файл.
    const int added = syncGaps(doc, range);
    syncLists(doc, {range.first, range.last + added});
}

}  // namespace

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

namespace {

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
// markdown не выражает, пустая строка ничьей вложенности не имеет; дословный
// кусок (таблица), формула, комментарий — годятся: объекты внутри пунктов
// любой глубины — решение владельца (сессия 5), и Tab/Shift+Tab двигают их
// уровень так же, как у абзаца.
bool canLiveInsideItem(const QTextBlock& block) {
    if (isVSpaceBlock(block)) return false;
    if (isRawBlock(block)) return true;
    const Kind kind = kindOf(block);
    return kind == Kind::Paragraph || kind == Kind::Quote || kind == Kind::Code ||
           kind == Kind::Math || kind == Kind::Html;
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
            format.clearProperty(QTextFormat::BlockLeftMargin);
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

static bool indentListItems(QTextDocument& doc, QTextCursor& cursor) {
    const BlockRange range = selectedBlocks(doc, cursor);
    const QTextBlock first = doc.findBlockByNumber(range.first);

    // Tab на абзаце (объекте, блоке кода) под списком привязывает его к пункту:
    // получается содержимое этого пункта. Правило Tab при этом одно на всех —
    // «сделать блок глубже», просто у абзаца и у пункта это значит разное.
    // Уже привязанный блок уходит НА ОДИН уровень глубже за нажатие, пока
    // есть куда: под блоком уровня N абзац уровня M < N по Tab идёт к M+1,
    // M+2, … до N (решение владельца) — глубже пункта над ним не бывает.
    if (!isListBlock(first)) {
        if (!canLiveInsideItem(first)) return false;
        const int above = levelAbove(doc, range.first);
        const int current = levelOf(first);
        if (above < 0 || current >= above) return false;
        return setInsideLevel(doc, range, current + 1);
    }

    // Отступать можно только под уже существующий пункт: иначе получился бы
    // прыжок через уровень, которого в файле не бывает.
    const QTextBlock prev = first.previous();
    if (!prev.isValid() || !isListBlock(prev) || levelOf(prev) < levelOf(first)) return false;

    shiftLevels(doc, range, 1);
    return true;
}

static bool outdentListItems(QTextDocument& doc, QTextCursor& cursor) {
    const BlockRange range = selectedBlocks(doc, cursor);
    const QTextBlock first = doc.findBlockByNumber(range.first);

    // Shift+Tab на втором абзаце (объекте, коде) пункта — на уровень выше; с
    // нулевого уровня — отвязывает обратно в обычный текст.
    if (!isListBlock(first)) {
        if (levelOf(first) < 0) return false;
        return setInsideLevel(doc, range, levelOf(first) - 1);
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

// Логические блоки куска документа. Тем же приёмом, что и копирование в буфер:
// кусок кладётся во временный документ, и смещения считать не приходится вовсе.
std::vector<Piece> piecesOfRange(QTextDocument& doc, int from, int to) {
    if (from >= to) return {};
    QTextCursor range(&doc);
    range.setPosition(from);
    range.setPosition(to, QTextCursor::KeepAnchor);
    return selectionPieces(range);
}

// Логические блоки диапазона. Блок ВЛАДЕЕТ своим текстом, поэтому
// переставлять, резать и склеивать их можно как обычные значения — ни арены,
// ни смещений, ни переноса байтов между документами.
//
// ДИАПАЗОНОМ, А НЕ ДОКУМЕНТОМ ЦЕЛИКОМ: правка трогает несколько блоков, и
// платить за неё размером заметки нельзя.
std::vector<Piece> piecesOfBlocks(const QTextDocument& doc, int firstBlock, int lastBlock) {
    std::vector<Piece> out;
    walkPieces(
        doc,
        [&out](const Piece& piece) {
            out.push_back(piece);
            return true;
        },
        firstBlock, lastBlock);
    return out;
}

// Часть ОДНОГО логического блока — как логические блоки, с сохранением его
// лица: рода, маркера, уровня, заголовочности. Всё это лежит в формате блока,
// поэтому лицо и переносится форматом целиком.
//
// Отдельно от piecesOfRange, и это не дубль: piecesOfRange живёт по правилам
// БУФЕРА ОБМЕНА (в буфер уходит законченный markdown, годный сам по себе) и
// потому нарочно снимает формат с куска строки и схлопывает уровни списка. Здесь
// кусок остаётся жить в той же заметке, на том же месте, и обязан остаться
// собой: разрезанный пополам пункт — это тот же пункт, а не два абзаца
// (владелец: «несколько строк невозможно превратить в блок кода не разрушив
// весь элемент списка»).
std::vector<Piece> piecesOfPart(QTextDocument& doc, int from, int to, const QTextBlock& origin) {
    if (from >= to) return {};
    QTextCursor range(&doc);
    range.setPosition(from);
    range.setPosition(to, QTextCursor::KeepAnchor);

    QTextDocument temp;
    QTextCursor paste(&temp);
    paste.insertFragment(range.selection());
    QTextCursor fix(&temp);
    fix.setPosition(0);
    fix.setBlockFormat(origin.blockFormat());
    return piecesOfBlocks(temp, 0, temp.blockCount() - 1);
}

// Блок заметки == QTextBlock: и код, и дословный кусок лежат одним блоком.
int docBlocksOf(const std::vector<Piece>& pieces) { return int(pieces.size()); }

// Блок кода из готовых байтов. Завершающий перевод строки лежит и в тексте, и
// в признаке — таков канон логического блока (см. document_pieces.h).
//
// УРОВЕНЬ — ТАКАЯ ЖЕ ПРИНАДЛЕЖНОСТЬ БЛОКА, КАК РОД, и передаётся он снаружи
// обязательно: блок кода, забывший уровень, выпадает из пункта наружу, а список
// за ним начинает нумерацию заново (нашёл владелец).
Piece codePiece(QString code, int level) {
    Piece out;
    out.kind = Kind::Code;
    out.level = level;
    out.trailingNewline = code.endsWith(QLatin1Char('\n'));
    out.text = std::move(code);
    return out;
}

// На каком уровне блоку кода СТОЯТЬ ПРАВО ИМЕЕТ. Содержимым пункта код может
// быть только тогда, когда пункт над ним остался: пункт, ставший кодом целиком,
// уносит с собой и свой уровень, и код после него оказывается уже ничьим.
//
// Не педантизм: единственный пункт заметки, превращённый в код, давал блок кода
// с уровнем 0 при полном отсутствии списка. Markdown этого не видел (отступать
// не от чего), а вот геометрия видела — заплатка расходилась с полной сборкой на
// левом поле, и ассерт в отладочной сборке ловил это сразу.
//
// keepsHead — над кодом остаётся голова того же блока: тогда спрашивать соседа
// сверху незачем, уровень даёт она сама.
int levelForCode(const QTextDocument& doc, int firstBlock, int wanted, bool keepsHead) {
    if (wanted < 0 || keepsHead) return wanted;
    return qMin(wanted, levelAbove(doc, firstBlock));
}

// Самый внешний уровень среди кусков: наружу списка (-1) внешнее всего. Блок,
// собранный из нескольких, встаёт туда, где стоял внешний из них.
int outermostLevel(const std::vector<Piece>& pieces) {
    int level = -1;
    bool first = true;
    for (const Piece& piece : pieces) {
        if (first || piece.level < level) level = piece.level;
        first = false;
    }
    return level;
}

}  // namespace

// Пробелы при переходе в код и обратно.
//
// В КОДЕ пробел значим сам по себе: его копируют в терминал, и неразрывный там
// не нужен, а нужен ровно тот, что виден. Поэтому в блоке кода неразрывные
// становятся обычными.
//
// В АБЗАЦЕ наоборот: markdown схлопывает несколько пробелов в один и съедает
// ведущие. Столбик из кода — "int a     = 5" — превратился бы в "int a = 5", и
// выравнивание пропало бы навсегда. Поэтому обратный ход делает неразрывными и
// ведущие пробелы, и СЕРИИ из двух и более в середине строки. Одиночные не
// трогаем: между словами неразрывный пробел не нужен, а мусор из чужих
// выгрузок мы как раз убираем (см. spacesNormalised в document_saver.cpp).
QString spacesForCode(QString text) {
    text.replace(QChar::Nbsp, QLatin1Char(' '));
    return text;
}

QString spacesForProse(QStringView text) {
    QString out;
    out.reserve(text.size());
    qsizetype i = 0;
    while (i < text.size()) {
        qsizetype lineEnd = text.indexOf(u'\n', i);
        if (lineEnd < 0) lineEnd = text.size();
        bool leading = true;
        qsizetype at = i;
        while (at < lineEnd) {
            if (text.at(at) != u' ') {
                leading = false;
                out += text.at(at);
                ++at;
                continue;
            }
            qsizetype run = 0;
            while (at + run < lineEnd && text.at(at + run) == u' ') ++run;
            // Ведущие — всегда, серия из двух и более — всегда: и то и другое
            // markdown иначе потеряет. Одиночный пробел между словами остаётся
            // обычным.
            const bool hold = leading || run > 1;
            out += QString(run, hold ? QChar(QChar::Nbsp) : QChar(u' '));
            at += run;
            leading = false;
        }
        if (lineEnd < text.size()) out += u'\n';
        i = lineEnd + 1;
    }
    return out;
}

// Выделенное — в блок кода и обратно. Блоки диапазона отдаются вызывающему
// вместе с его границами: положить их обратно — дело заметки.
//
// РАБОТАЕТ ТОЛЬКО НАД ДИАПАЗОНОМ. Прежде операция читала логические блоки ВСЕГО
// документа и возвращала его целиком; заметка потом собирала его заново. Теперь
// и читается, и собирается только выделенное с его блоками.
struct CodeBlockEdit {
    bool done = false;
    int firstBlock = 0;         // границы в номерах QTextBlock
    int lastBlock = 0;
    std::vector<Piece> blocks;  // чем этот диапазон становится
    int landed = 0;             // номер блока, куда встать каретке
};

static CodeBlockEdit toggleCodeBlock(QTextDocument& doc, const QTextCursor& cursor) {
    const int start = qMin(cursor.selectionStart(), cursor.selectionEnd());
    const int end = qMax(cursor.selectionStart(), cursor.selectionEnd());

    int firstBlock = doc.findBlock(start).blockNumber();
    int lastBlock = doc.findBlock(end).blockNumber();

    // Крайние пустые строки выделения — не код: клавиатурное выделение легко
    // цепляет соседний VSpace (Shift+Down с пустой строки или до неё), и без
    // обрезки блок кода съедал отбивку у соседа (поймано владельцем: черта
    // слипалась с забором). Выделение из одних пустых строк — не операция.
    while (firstBlock <= lastBlock && isVSpaceBlock(doc.findBlockByNumber(firstBlock)))
        ++firstBlock;
    while (lastBlock >= firstBlock && isVSpaceBlock(doc.findBlockByNumber(lastBlock)))
        lastBlock = lastBlock - 1;
    if (firstBlock > lastBlock) return {};

    const QTextBlock headBlock = doc.findBlockByNumber(firstBlock);
    const QTextBlock tailBlock = doc.findBlockByNumber(lastBlock);
    if (!headBlock.isValid() || !tailBlock.isValid()) return {};

    // Позиции выделения — в пределы обрезанного диапазона: край, стоявший на
    // пустой строке, иначе продолжал бы командовать разниманием ниже.
    const int selStart = qMax(start, headBlock.position());
    const int selEnd = qMin(end, tailBlock.position() + tailBlock.length() - 1);

    // Абзац с мягкими переносами — один блок, а выделить в нём человек может
    // несколько строк из многих. Тогда блок надо разнять: что осталось снаружи,
    // остаётся как было. Границы притягиваются к краям строк.
    const QTextBlock firstDocBlock = doc.findBlock(selStart);
    const QTextBlock lastDocBlock = doc.findBlock(selEnd);
    const int lineStart = lineStartAt(doc, selStart);
    // Конец выделения ровно на начале строки: эту строку человек не выделял, и
    // тянуть её в блок кода незачем.
    const int lineEnd = (selEnd > selStart && selEnd == lineStartAt(doc, selEnd))
                            ? selEnd - 1
                            : lineEndAt(doc, selEnd);
    const int blockEnd = lastDocBlock.position() + lastDocBlock.length() - 1;

    CodeBlockEdit out;
    out.firstBlock = firstBlock;
    out.lastBlock = lastBlock;

    // Разделители строк на срезах в куски не берём: иначе оставшийся кусок
    // кончался бы пустой строкой, а она блок заканчивает.
    const std::vector<Piece> head =
        piecesOfPart(doc, firstDocBlock.position(),
                     lineStart > firstDocBlock.position() ? lineStart - 1 : lineStart,
                     firstDocBlock);
    const std::vector<Piece> tail =
        piecesOfPart(doc, lineEnd < blockEnd ? lineEnd + 1 : lineEnd, blockEnd, lastDocBlock);
    const std::vector<Piece> chosen = piecesOfRange(doc, lineStart, lineEnd);
    if (!head.empty() || !tail.empty()) {
        if (chosen.empty()) return {};
        // Куски приехали из отдельных документов, но блок владеет своим
        // текстом — переносить байты некуда, обычное копирование значения.
        std::vector<Piece> result(head.begin(), head.end());

        QString code;
        for (const Piece& piece : chosen) {
            if (!code.isEmpty()) code += u'\n';
            code += piece.text;
        }
        if (!code.isEmpty() && !code.endsWith(u'\n')) code += u'\n';
        out.landed = firstBlock + docBlocksOf(result);
        result.push_back(codePiece(spacesForCode(code), levelForCode(doc, firstBlock,
                                                                    levelOf(firstDocBlock),
                                                                    !head.empty())));

        // ПУНКТ ОБЯЗАН УЦЕЛЕТЬ. Голова и хвост приехали из одного блока, и
        // маркер списка достался обеим — а маркер у пункта один. Достаётся он
        // тому, кто идёт первым; хвост за ним остаётся продолжением того же
        // пункта, то есть блоком БЕЗ маркера на его уровне. Прежде хвост
        // становился НОВЫМ пунктом, и список разъезжался пополам.
        for (Piece piece : tail) {
            if (!head.empty() && isList(piece.kind)) piece.kind = Kind::Paragraph;
            result.push_back(std::move(piece));
        }

        out.done = true;
        out.blocks = std::move(result);
        return out;
    }

    const std::vector<Piece> selected = piecesOfBlocks(doc, firstBlock, lastBlock);
    if (selected.empty()) return {};

    // Дословные куски не трогаем вовсе: их текст выводится как есть.
    for (const Piece& piece : selected)
        if (piece.raw) return {};

    bool allCode = true;
    for (const Piece& piece : selected)
        if (piece.kind != Kind::Code) allCode = false;

    std::vector<Piece> result;
    if (allCode) {
        // Обратный ход: каждая строка кода становится строкой обычного текста.
        // Один блок кода — один абзац: переводы строк внутри абзаца жить умеют.
        // Уровень остаётся тот же: код, живший внутри пункта, вернётся туда же
        // абзацем, а не выпадет из списка.
        for (const Piece& piece : selected) {
            QStringView body = piece.text;
            while (body.endsWith(u'\n')) body.chop(1);
            Piece plain;
            plain.level = piece.level;
            plain.text = spacesForProse(body);
            result.push_back(std::move(plain));
        }
    } else {
        QString code;
        for (const Piece& piece : selected) {
            if (!code.isEmpty()) code += u'\n';
            code += piece.text;
        }
        if (!code.isEmpty() && !code.endsWith(u'\n')) code += u'\n';
        result.push_back(
            codePiece(spacesForCode(code), levelForCode(doc, firstBlock,
                                                        outermostLevel(selected), false)));
    }
    out.landed = firstBlock;
    out.done = true;
    out.blocks = std::move(result);
    return out;
}

// Пункт меняется местами с соседом того же уровня, вместе с поддеревьями.
// Тоже только над диапазоном: двигаются два поддерева и пустая строка между
// ними, и больше ничего документ не касается.
struct MoveEdit {
    bool done = false;
    int firstBlock = 0;
    int lastBlock = 0;
    std::vector<Piece> blocks;
    int landed = 0;          // номер блока, куда переехала каретка
    int offsetInBlock = 0;
};

static MoveEdit moveListItem(const QTextDocument& doc, const QTextCursor& cursor,
                             int direction) {
    const QTextBlock block = cursor.block();
    if (!isListBlock(block)) return {};

    const int number = block.blockNumber();
    const int sibling = direction < 0 ? previousSibling(doc, number) : nextSibling(doc, number);
    if (sibling < 0) return {};

    const bool selfIsUpper = direction > 0;
    const int upperFirst = selfIsUpper ? number : sibling;
    const int lowerFirst = selfIsUpper ? sibling : number;
    const int upperLast = subtreeEnd(doc, upperFirst);
    const int lowerLast = subtreeEnd(doc, lowerFirst);
    if (upperLast + 1 > lowerFirst) return {};

    // Пункты меняются местами, а пустая строка остаётся на месте: она
    // принадлежит стыку, а не пункту, и уехав с ним, порвала бы список там, где
    // человек ничего не трогал.
    const std::vector<Piece> upper = piecesOfBlocks(doc, upperFirst, upperLast);
    const std::vector<Piece> gap = upperLast + 1 <= lowerFirst - 1
                                       ? piecesOfBlocks(doc, upperLast + 1, lowerFirst - 1)
                                       : std::vector<Piece>{};
    const std::vector<Piece> lower = piecesOfBlocks(doc, lowerFirst, lowerLast);
    if (upper.empty() || lower.empty()) return {};

    MoveEdit out;
    out.firstBlock = upperFirst;
    out.lastBlock = lowerLast;
    out.blocks.insert(out.blocks.end(), lower.begin(), lower.end());
    out.blocks.insert(out.blocks.end(), gap.begin(), gap.end());
    out.blocks.insert(out.blocks.end(), upper.begin(), upper.end());

    // Куда уехал сам пункт: верхний уходит за пустую строку и соседа, нижний
    // встаёт в самое начало. Строки внутри поддерева считаются блоками, а не
    // логическими блоками: каретка стоит в конкретной строке.
    const int selfStart = selfIsUpper ? upperFirst : lowerFirst;
    const int newSelfStart =
        selfIsUpper ? upperFirst + docBlocksOf(lower) + docBlocksOf(gap) : upperFirst;
    out.landed = newSelfStart + (number - selfStart);
    out.offsetInBlock = cursor.positionInBlock();
    out.done = true;
    return out;
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

// Как выглядит встроенный код: семейство, ступень кегля, подложка. Одно место
// на всех — правило кавычек и набор с клавиатуры красят одинаково.
//
// Ступень отсчитывается от окружения — от того, что стоит в формате самого
// блока: код внутри заголовка обязан ехать вместе с заголовком, ровно как у
// сборщика.
void applyCodeLook(QTextCharFormat& format, int surroundingStep, const ZDocStyle& style) {
    setFontStep(format, surroundingStep + style.codeStep());
    if (!style.codeFamily().isEmpty()) format.setFontFamilies({QString(style.codeFamily())});
    format.setBackground(style.codeBackground());
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
        format.setProperty(KindProperty, int(Kind::Paragraph));
        format.clearProperty(MarkerProperty);
        format.clearProperty(CheckedProperty);
        format.clearProperty(LevelProperty);
        // РОД ПИШЕМ, ОСТАЛЬНОЕ ЧИСТИМ — ровно как сборщик. Он ставит род
        // ВСЕГДА, включая обычный абзац, а маркер, отметку, уровень и поля у
        // обычного блока не ставит вовсе. И то и другое важно: явный ноль там,
        // где сборщик молчит, и молчание там, где он пишет ноль, одинаково
        // делают блок непохожим на собранный. Пока после каждой операции шла
        // полная пересборка, разницы не было видно; теперь её ловит сверка.
        format.clearProperty(QTextFormat::BlockLeftMargin);
        format.clearProperty(QTextFormat::HeadingLevel);
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
        format.clearProperty(QTextFormat::HeadingLevel);
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

// Знак, с которого начинается заголовок при наборе. В файл уходит всегда
// решётка — знак номера живёт только на клавиатуре, канон его не знает.
bool isHeadingMark(QChar c) {
    return c == QLatin1Char('#') || c == QChar(0x2116);   // # и №
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

    // Заголовок: от одной решётки до шести. Знак номера работает наравне с
    // решёткой (просьба владельца): на русской раскладке «#» набирается только
    // переключением на латиницу, а «№» стоит на той же клавише в кириллице.
    // Ряд должен быть однородным — «#№ » ничего не значит и заголовком не
    // становится.
    if (!body.isEmpty() && isHeadingMark(body.at(0))) {
        const QChar mark = body.at(0);
        int marks = 0;
        while (marks < body.size() && body.at(marks) == mark) ++marks;
        if (marks >= 1 && marks <= 6 && marks == body.size() && kind != Kind::Heading)
            return {{Kind::Heading, Marker::Bullet, false}, marks, int(typed.size()), true};
    }

    return {};
}

}  // namespace

std::vector<Piece> selectionPieces(const QTextCursor& cursor) {
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

    std::vector<Piece> ir = piecesOfBlocks(temp, 0, temp.blockCount() - 1);

    // Выделение, кончающееся ровно на начале блока, этого блока не захватывает:
    // человек довёл до него курсор, но не выделял. Qt всё равно кладёт во
    // фрагмент пустой хвостовой блок, и в буфер уходил лишний пустой пункт —
    // "- два" копировалось как "- два\n-\n". Раньше он молча слипался при
    // вставке, а как только вставка стала беречь строение, стал виден.
    if (to > from && to == cursor.document()->findBlock(to).position() && !ir.empty()) {
        const Piece& tail = ir.back();
        if (!tail.raw && tail.text.isEmpty()) ir.pop_back();
    }

    int deepest = -1;
    for (const Piece& block : ir)
        if (!block.raw && isList(block.kind))
            deepest = deepest < 0 ? block.level : qMin(deepest, block.level);
    if (deepest > 0)
        for (Piece& block : ir)
            if (!block.raw && isList(block.kind)) block.level -= deepest;

    return ir;
}

QString selectionToMarkdown(const QTextCursor& cursor) {
    const std::vector<Piece> ir = selectionPieces(cursor);
    if (ir.empty()) return {};

    QString text = writePieces(ir);
    const bool inlineOnly =
        ir.size() == 1 && !ir.front().raw && ir.front().kind == Kind::Paragraph;
    if (inlineOnly && text.endsWith(QLatin1Char('\n'))) text.chop(1);
    return text;
}

static bool applyInputRuleAtCursor(QTextDocument& doc, QTextCursor& cursor) {
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

    // Хвост многострочного блока — прежним блоком: правило меняет только
    // строку каретки. «Июль» становится заголовком, а «просто текст» строкой
    // ниже остаётся текстом (поймано владельцем). Резать сзади — до переднего
    // разреза: позиции не плывут.
    int lineEnd = int(text.size());
    for (int i = at; i < int(text.size()); ++i)
        if (text.at(i) == QChar::LineSeparator) {
            lineEnd = i;
            break;
        }
    if (lineEnd < int(text.size())) {
        edit.setPosition(block.position() + lineEnd);
        edit.setPosition(block.position() + lineEnd + 1, QTextCursor::KeepAnchor);
        edit.removeSelectedText();
        QTextBlockFormat carry = block.blockFormat();
        edit.insertBlock(carry, block.charFormat());
    }

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
        format.clearProperty(QTextFormat::HeadingLevel);
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

static bool applyCodeSpanRuleAtCursor(QTextDocument& doc, QTextCursor& cursor) {
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
    applyCodeLook(code, blockFontStep(cursor.block()), styleOf(*cursor.document()));
    edit.mergeCharFormat(code);
    edit.endEditBlock();

    cursor.setPosition(block.position() + end - 2);
    return true;
}

static bool toggleBold(QTextDocument& doc, QTextCursor& cursor) {
    return toggleInlineStyle(doc, cursor, SpanBold);
}

static bool toggleCode(QTextDocument& doc, QTextCursor& cursor) {
    return toggleInlineStyle(doc, cursor, SpanCode);
}

static bool toggleItalic(QTextDocument& doc, QTextCursor& cursor) {
    return toggleInlineStyle(doc, cursor, SpanItalic);
}

static bool toggleStrike(QTextDocument& doc, QTextCursor& cursor) {
    return toggleInlineStyle(doc, cursor, SpanStrike);
}

namespace {

// Обернуть или развернуть кусок долларами. Одна механика на строчную и
// выключную: разница только в числе долларов и в том, что берётся — выделение
// или весь абзац.
bool toggleMath(QTextCursor& cursor, const QString& fence, bool wholeBlock) {
    QTextCursor at = cursor;
    if (wholeBlock) {
        at.setPosition(at.block().position());
        at.setPosition(at.block().position() + at.block().length() - 1,
                       QTextCursor::KeepAnchor);
    }
    const QString text = at.selectedText();
    if (text.isEmpty() && !wholeBlock) {
        // Пустая каретка: ставим пару и встаём между ними — дальше человек
        // просто печатает формулу.
        at.insertText(fence + fence);
        at.setPosition(at.position() - fence.size());
        cursor = at;
        return true;
    }
    if (text.isEmpty()) return false;

    // Уже формула — снимаем доллары. Смотрим на сам текст, а не на признаки
    // показа: жест обязан работать и на исходнике, который человек набрал
    // руками минуту назад.
    if (text.size() > 2 * fence.size() && text.startsWith(fence) && text.endsWith(fence)) {
        at.insertText(text.mid(fence.size(), text.size() - 2 * fence.size()));
        cursor = at;
        return true;
    }
    at.insertText(fence + text + fence);
    cursor = at;
    return true;
}

}  // namespace

static bool toggleInlineMath(QTextDocument& doc, QTextCursor& cursor) {
    Q_UNUSED(doc);
    return toggleMath(cursor, QStringLiteral("$"), false);
}

static bool toggleDisplayMath(QTextDocument& doc, QTextCursor& cursor) {
    Q_UNUSED(doc);
    // Пустой абзац выключной формулой не делаем: получились бы четыре доллара
    // и ничего между ними.
    if (cursor.block().text().trimmed().isEmpty()) return false;
    return toggleMath(cursor, QStringLiteral("$$"), true);
}

static QTextCharFormat inlineStyleForTyping(const QTextBlock& block, const QTextCharFormat& current,
                                     int style) {
    const int now = current.intProperty(SpanStyleProperty);
    const int next = (now & style) != 0 ? (now & ~style) : (now | style);

    // За основу берём формат блока — это и есть «обычный текст здесь»: в
    // заголовке он крупнее, в пункте обычный. Дальше кладём на него признаки.
    QTextCharFormat format = block.charFormat();
    format.merge(formatForStyle(next));
    if ((next & SpanCode) != 0) applyCodeLook(format, blockFontStep(block), styleOf(*block.document()));
    return format;
}

static bool makeBullet(QTextDocument& doc, QTextCursor& cursor) {
    return setBlockKind(doc, cursor, {Kind::ListItem, Marker::Bullet, false});
}

static bool makeOrdered(QTextDocument& doc, QTextCursor& cursor) {
    return setBlockKind(doc, cursor, {Kind::ListItem, Marker::Ordered, false});
}

static bool makeTask(QTextDocument& doc, QTextCursor& cursor) {
    return setBlockKind(doc, cursor, {Kind::ListItem, Marker::Task, false});
}

// Вырезать строки [from, to] блока в ОТДЕЛЬНЫЙ блок. Возвращает номер блока,
// в котором они оказались.
//
// Зачем. Строка внутри абзаца — не блок: абзац со стихотворными переносами это
// ОДИН блок с мягкими переносами внутри. Операция, работающая поблочно,
// превращает в заголовок весь абзац разом — владелец наткнулся на это дважды,
// сперва при наборе «# », теперь при смене уровня. Поэтому правило живёт
// отдельной функцией, а не внутри одной операции.
//
// Режем СЗАДИ НАПЕРЁД: передний разрез сдвинул бы позиции заднего.
int isolateLines(QTextDocument& doc, QTextCursor& edit, const QTextBlock& block, int from,
                 int to) {
    const QString text = block.text();
    if (text.isEmpty()) return block.blockNumber();

    int lineStart = 0;
    for (int i = qMin(from, int(text.size())) - 1; i >= 0; --i)
        if (text.at(i) == QChar::LineSeparator) {
            lineStart = i + 1;
            break;
        }
    int lineEnd = int(text.size());
    for (int i = qMax(0, to); i < int(text.size()); ++i)
        if (text.at(i) == QChar::LineSeparator) {
            lineEnd = i;
            break;
        }
    if (lineStart == 0 && lineEnd == int(text.size())) return block.blockNumber();

    const int base = block.position();
    if (lineEnd < int(text.size())) {
        edit.setPosition(base + lineEnd);
        edit.setPosition(base + lineEnd + 1, QTextCursor::KeepAnchor);
        edit.removeSelectedText();
        edit.insertBlock(block.blockFormat(), block.charFormat());
    }
    int start = base;
    if (lineStart > 0) {
        edit.setPosition(base + lineStart - 1);
        edit.setPosition(base + lineStart, QTextCursor::KeepAnchor);
        edit.removeSelectedText();
        QTextBlockFormat carry = block.blockFormat();
        carry.clearProperty(TrailingNewlineProperty);
        edit.insertBlock(carry, block.charFormat());
        start = edit.position();
    }
    return doc.findBlock(start).blockNumber();
}

static bool setHeadingLevel(QTextDocument& doc, QTextCursor& cursor, int level) {
    BlockRange range = selectedBlocks(doc, cursor);
    level = std::clamp(level, 0, 6);

    QTextCursor edit(&doc);
    edit.beginEditBlock();

    // Строку абзаца сперва делаем блоком: иначе заголовком станет весь абзац,
    // а человек показал на одну строку.
    //
    // isolateLines режет блок С ОБОИХ КОНЦОВ за один вызов, поэтому случаев
    // ровно два, и путать их нельзя: на одном блоке зовём её один раз, на
    // нескольких — по разу с краёв, и обязательно СНАЧАЛА с хвоста, иначе
    // передний разрез сдвинет позиции заднего. Первый заход звал её дважды и
    // на одном блоке — второй вызов работал по устаревшим номерам, и заголовок
    // уезжал на соседнюю строку.
    {
        const int from = qMin(cursor.anchor(), cursor.position());
        const int to = qMax(cursor.anchor(), cursor.position());
        if (range.first == range.last) {
            const QTextBlock only = doc.findBlockByNumber(range.first);
            if (only.isValid()) {
                range.first = isolateLines(doc, edit, only, from - only.position(),
                                           to - only.position());
                range.last = range.first;
            }
        } else {
            const QTextBlock tail = doc.findBlockByNumber(range.last);
            if (tail.isValid()) range.last = isolateLines(doc, edit, tail, 0,
                                                          to - tail.position());
            const QTextBlock head = doc.findBlockByNumber(range.first);
            if (head.isValid()) {
                const int moved = isolateLines(doc, edit, head, from - head.position(),
                                               int(head.text().size()));
                range.last += moved - range.first;
                range.first = moved;
            }
        }
    }

    bool any = false;
    int first = -1;
    int last = -1;
    for (int i = range.first; i <= range.last; ++i) {
        const QTextBlock block = doc.findBlockByNumber(i);
        if (!block.isValid() || isRawBlock(block)) continue;
        const Kind from = kindOf(block);
        if (from == Kind::Code || from == Kind::VSpace || from == Kind::Divider) continue;
        if (block.text().isEmpty()) continue;   // заголовка из ничего не бывает

        QTextBlockFormat format = block.blockFormat();
        if (level == 0) {
            format.setProperty(KindProperty, int(Kind::Paragraph));
            format.clearProperty(MarkerProperty);
            format.clearProperty(CheckedProperty);
            format.clearProperty(LevelProperty);
            format.clearProperty(QTextFormat::BlockLeftMargin);
            format.clearProperty(QTextFormat::HeadingLevel);
        } else {
            // Пункт списка, ставший заголовком, перестаёт быть пунктом: свойства
            // списка снимаются целиком, иначе он уехал бы в файл как "- # текст".
            format.clearProperty(MarkerProperty);
            format.clearProperty(CheckedProperty);
            format.clearProperty(LevelProperty);
            format.clearProperty(QTextFormat::BlockLeftMargin);
            format.setProperty(KindProperty, int(Kind::Heading));
            format.setHeadingLevel(level);
        }
        QTextCursor at(&doc);
        at.setPosition(block.position());
        at.setBlockFormat(format);
        any = true;
        if (first < 0) first = i;
        last = i;
    }

    // Пустых строк вокруг заголовка НЕ заводим (решение владельца): операция
    // меняет только то, о чём попросили. Там, где без разделителя блоки
    // слиплись бы в файле, пустую строку поставит сам инвариант — normalise
    // ниже это и делает.

    if (any) normalise(doc, {qMax(0, first - 1), qMin(doc.blockCount() - 1, last + 1)});
    edit.endEditBlock();
    return any;
}

static bool makeParagraph(QTextDocument& doc, QTextCursor& cursor) {
    return setBlockKind(doc, cursor, {Kind::Paragraph, Marker::Bullet, false});
}

namespace {

// Записать картинку строкой заново — в той же форме, в какой она записана.
// Ширина и выравнивание идут рядом, умолчания не пишутся; см. imageRefText.
bool rewriteImageRef(QTextCursor& cursor, const QTextBlock& block, const BlockImageRef& ref) {
    const QString text = imageRefText(ref);
    // ФОТОГРАФИЯ-ОБЪЕКТ ТЕКСТА НЕ ИМЕЕТ: в блоке стоит один знак U+FFFC, а
    // исходник лежит в свойствах его формата — его и правим. Пока правился
    // текст, новая ширина ДОПИСЫВАЛАСЬ рядом с объектом, и в файл уезжали две
    // картинки вместо одной.
    if (block.text().size() == 1 &&
        block.text().at(0) == QChar::ObjectReplacementCharacter) {
        QTextCursor edit(block);
        edit.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
        QTextCharFormat format = edit.charFormat();
        if (ref.wiki) {
            if (format.property(ObjectSourceProperty).toString() == text) return false;
            format.setProperty(ObjectSourceProperty, text);
        } else {
            if (format.anchorHref() == text) return false;
            format.setAnchorHref(text);
        }
        edit.setCharFormat(format);
        cursor.setPosition(block.position());
        return true;
    }
    if (ref.wiki) {
        if (block.text() == text) return false;
        QTextCursor edit(block);
        edit.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
        edit.insertText(text);
    } else {
        QTextCursor edit(block);
        edit.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
        if (edit.charFormat().anchorHref() == text) return false;
        QTextCharFormat format;
        format.setAnchorHref(text);
        edit.mergeCharFormat(format);
    }
    // Каретка — к началу строки: строка хитро-отрисованная, внутри неё каретке
    // делать нечего.
    cursor.setPosition(block.position());
    return true;
}

}  // namespace

static bool setImageWidthAtCursor(QTextDocument& doc, QTextCursor& cursor, int width) {
    Q_UNUSED(doc);
    const QTextBlock block = cursor.block();
    BlockImageRef ref = blockImageRef(block);
    if (!ref.valid || width <= 0) return false;
    if (!ref.wiki && qRound(ref.widthHint) == width) return false;
    ref.widthHint = width;
    return rewriteImageRef(cursor, block, ref);
}

static bool setImageAlignAtCursor(QTextDocument& doc, QTextCursor& cursor, ImageAlign align) {
    Q_UNUSED(doc);
    const QTextBlock block = cursor.block();
    BlockImageRef ref = blockImageRef(block);
    if (!ref.valid || ref.align == align) return false;
    ref.align = align;
    return rewriteImageRef(cursor, block, ref);
}

// Подпись картинки — как и адрес, свойство ОБЪЕКТА (ObjectAltProperty): текста
// у объекта нет, в блоке один знак U+FFFC. Пустая подпись законна — картинка
// остаётся картинкой, в файл уходит «![](путь)». У вики-вложения подписи нет.
static bool setImageCaptionAtCursor(QTextCursor& cursor, const QString& caption) {
    const QTextBlock block = cursor.block();
    const BlockImageRef ref = blockImageRef(block);
    if (!ref.valid || ref.wiki) return false;
    if (block.text().size() != 1 || block.text().at(0) != QChar::ObjectReplacementCharacter)
        return false;
    // Подпись — одна строка: перевод строки в alt в файле развалил бы картинку
    // на текст, а U+2028 — на две строки на экране.
    QString alt = caption;
    alt.replace(QLatin1Char('\n'), QLatin1Char(' '));
    alt.replace(QLatin1Char('\r'), QLatin1Char(' '));
    alt.replace(QChar::LineSeparator, QLatin1Char(' '));
    alt.replace(QChar::ParagraphSeparator, QLatin1Char(' '));
    if (alt == ref.alt) return false;
    QTextCursor edit(block);
    edit.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
    QTextCharFormat format = edit.charFormat();
    format.setProperty(ObjectAltProperty, alt);
    format.setProperty(ObjectSourceProperty, alt);
    edit.setCharFormat(format);
    cursor.setPosition(block.position());
    return true;
}

// Убрать блок-строку с текстом целиком — текст и разделитель, не тронув
// соседей. Как в removeLineBlock: позиции и формат выжившего — ДО правки
// (хэндлы протухают), формат выжившего ставится явно (Qt при слиянии
// оставляет формат не того блока). Каретка edit остаётся на месте строки.
void removeTextLine(QTextCursor& edit, const QTextBlock& block) {
    const QTextBlock after = block.next();
    const QTextBlock before = block.previous();
    const QTextBlockFormat keep =
        after.isValid() ? after.blockFormat()
                        : (before.isValid() ? before.blockFormat() : QTextBlockFormat());
    const int at = block.position();
    const int length = block.length();

    if (after.isValid()) {
        edit.setPosition(at);
        edit.setPosition(at + length, QTextCursor::KeepAnchor);
        edit.removeSelectedText();
        edit.setPosition(at);
        edit.setBlockFormat(keep);
    } else if (before.isValid()) {
        edit.setPosition(at - 1);
        edit.movePosition(QTextCursor::End, QTextCursor::KeepAnchor);
        edit.removeSelectedText();
        edit.setBlockFormat(keep);
    } else {
        // Единственный блок документа: остаётся пустой абзац.
        edit.setPosition(0);
        edit.movePosition(QTextCursor::End, QTextCursor::KeepAnchor);
        edit.removeSelectedText();
        edit.setBlockFormat(QTextBlockFormat());
        edit.setCharFormat(QTextCharFormat());
    }
}

static bool cutImageLineAtCursor(QTextDocument& doc, QTextCursor& cursor) {
    Q_UNUSED(doc);
    const QTextBlock block = cursor.block();
    if (!blockImageRef(block).valid) return false;
    QTextCursor edit(cursor);
    removeTextLine(edit, block);
    cursor = edit;
    return true;
}

// Enter на фотографии: ПУСТАЯ СТРОКА ПОСЛЕ НЕЁ, а сама она цела.
//
// Правило владельца: каретка, стоящая на фотографии, считается стоящей сразу
// ЗА ней. Отсюда и поведение — Enter начинает новую строку после картинки, а
// не делит её блок. Деление разрушало разметку "![alt](путь)" и оставляло
// подпись, показанную ссылкой: вроде картинка и есть, а вроде её и нет.
//
// Это же закрывает «за последней картинкой некуда встать»: нажал Enter —
// получил строку.
static bool newLineAfterImage(QTextDocument& doc, QTextCursor& cursor) {
    Q_UNUSED(doc);
    if (cursor.hasSelection()) return false;
    const QTextBlock photo = cursor.block();
    if (!blockImageRef(photo).valid) return false;

    QTextCursor edit(cursor);
    edit.setPosition(photo.position() + photo.length() - 1);
    edit.insertBlock(vspaceBlockFormat(false, false, styleOf(*edit.document())), QTextCharFormat());
    cursor = edit;
    return true;
}

static bool deleteImageLineBackward(QTextDocument& doc, QTextCursor& cursor) {
    Q_UNUSED(doc);
    if (cursor.hasSelection() || !cursor.atBlockStart()) return false;
    const QTextBlock photo = cursor.block().previous();
    if (!photo.isValid() || !blockImageRef(photo).valid) return false;
    QTextCursor edit(cursor);
    removeTextLine(edit, photo);
    // Каретка — в начале своей строки, поднявшейся на место фотографии.
    cursor = edit;
    return true;
}

static bool deleteImageLineForward(QTextDocument& doc, QTextCursor& cursor) {
    Q_UNUSED(doc);
    if (cursor.hasSelection()) return false;
    const QTextBlock block = cursor.block();
    if (cursor.position() != block.position() + block.length() - 1) return false;
    const QTextBlock photo = block.next();
    if (!photo.isValid() || !blockImageRef(photo).valid) return false;
    const int keepAt = cursor.position();
    QTextCursor edit(cursor);
    removeTextLine(edit, photo);
    edit.setPosition(keepAt);
    cursor = edit;
    return true;
}

// --- блок кода: выход и табуляция -------------------------------------------

namespace {

// БЛОК КОДА — ОДИН QTextBlock (решение владельца, сессия refactor2). Его строки
// разделяет U+2028 с пометкой BreakSourceProperty — тот же мягкий перенос, что
// и в абзаце. «Строка кода» здесь — отрезок [start, end) внутри блока, без
// разделителя; всё, что делается со строками (Tab, Shift+Tab, Enter, забор),
// делается с этими отрезками, а не с блоками. Прежде каждая строка лежала
// отдельным QTextBlock-продолжением, и всякая правка внутри длинного блока
// пересобирала его целиком (замер: 85 мс на нажатие в блоке на 3000 строк).
bool isCodeBlock(const QTextBlock& block) {
    return block.isValid() && !isRawBlock(block) && kindOf(block) == Kind::Code;
}

// Литеральный блок — код ИЛИ дословный кусок: у обоих строки — отрезки одного
// блока (сессия 5 refactor2: дословное догнало код), и слой строк — Enter,
// Tab, Shift+Tab — общий. Забор и язык остаются делом кода: у дословного их нет.
bool isLiteralBlock(const QTextBlock& block) {
    return block.isValid() && (isRawBlock(block) || kindOf(block) == Kind::Code);
}

struct CodeLine {
    int start = 0;   // позиция первого знака строки в документе
    int end = 0;     // позиция за последним знаком (там разделитель или конец блока)
};

CodeLine codeLineAt(const QTextDocument& doc, int position) {
    return {lineStartAt(doc, position), lineEndAt(doc, position)};
}

QString codeLineText(const QTextDocument& doc, const CodeLine& line) {
    const QTextBlock block = doc.findBlock(line.start);
    return block.text().mid(line.start - block.position(), line.end - line.start);
}

// Видимая колонка позиции в строке: знак табуляции доводит до следующего
// стопа, остальные знаки стоят по одному. Без этого «до стопа» считалось бы
// по числу знаков, и строка со старым табом отступала бы не туда, где её
// рисуют.
int visualColumn(const QString& text, int upTo, int width) {
    int column = 0;
    for (int i = 0; i < upTo && i < text.size(); ++i) {
        if (text.at(i) == QLatin1Char('\t')) column += width - (column % width);
        else ++column;
    }
    return column;
}

// Отступ строки в пробелах и сколько знаков он занимает в тексте.
struct Lead {
    int columns = 0;   // видимая ширина отступа
    int chars = 0;     // сколько знаков текста он занимает
};

Lead leadingIndent(const QString& text, int width) {
    Lead lead;
    while (lead.chars < text.size()) {
        const QChar c = text.at(lead.chars);
        if (c == QLatin1Char(' ')) lead.columns += 1;
        else if (c == QLatin1Char('\t')) lead.columns += width - (lead.columns % width);
        else break;
        ++lead.chars;
    }
    return lead;
}

int codeTabWidth() { return qMax(1, settings().editor().codeTabWidth()); }

// Переписать отступ строки на columns пробелов. Возвращает, на сколько знаков
// строка стала длиннее (может быть отрицательным).
int setLineIndent(QTextCursor& edit, const QTextDocument& doc, const CodeLine& line,
                  int columns) {
    columns = qMax(0, columns);
    const Lead lead = leadingIndent(codeLineText(doc, line), codeTabWidth());
    if (lead.columns == columns && lead.chars == columns) return 0;
    edit.setPosition(line.start);
    edit.setPosition(line.start + lead.chars, QTextCursor::KeepAnchor);
    edit.insertText(QString(columns, QLatin1Char(' ')));
    return columns - lead.chars;
}

// Строки блока кода, задетые курсором: от строки начала выделения до строки
// его конца, в пределах одного блока. Пусто — курсор не в коде.
QVector<CodeLine> touchedCodeLines(const QTextDocument& doc, const QTextCursor& cursor) {
    QVector<CodeLine> lines;
    const int from = qMin(cursor.anchor(), cursor.position());
    const int to = qMax(cursor.anchor(), cursor.position());
    const QTextBlock block = doc.findBlock(from);
    if (!isLiteralBlock(block)) return lines;
    const int blockEnd = block.position() + block.length() - 1;
    const int last = qMin(to, blockEnd);
    for (int at = lineStartAt(doc, from);;) {
        const CodeLine line{at, lineEndAt(doc, at)};
        lines.push_back(line);
        if (line.end >= last || line.end >= blockEnd) break;
        at = line.end + 1;
    }
    return lines;
}

// Позиция «та же колонка той же строки» после того, как строки получили новые
// отступы: delta[i] — на сколько знаков изменилась строка i, lines — строки до
// правки. Колонка левее нового начала текста прижимается к началу строки.
int shiftedPosition(const QVector<CodeLine>& lines, const QVector<int>& delta, int position) {
    int shift = 0;
    for (int i = 0; i < lines.size(); ++i) {
        const CodeLine& line = lines[i];
        if (position < line.start) break;
        if (position <= line.end) return line.start + shift + qMax(0, position - line.start + delta[i]);
        shift += delta[i];
    }
    return position + shift;
}

}  // namespace

static QString sanitiseCodeLanguage(QString language) {
    // Пробелы и заборы — единственное, что в имени языка сломало бы файл:
    // после забора идёт info-строка, и пробел в ней означает конец имени.
    language.remove(QLatin1Char('`'));
    language.remove(QLatin1Char('~'));
    return language.simplified().remove(QLatin1Char(' '));
}

static bool setCodeLanguage(QTextDocument& doc, QTextCursor& cursor, const QString& language) {
    const QTextBlock block = cursor.block();
    if (!isCodeBlock(block)) return false;

    const QString want = sanitiseCodeLanguage(language);
    if (block.blockFormat().stringProperty(InfoProperty) == want) return false;

    QTextBlockFormat format = block.blockFormat();
    if (want.isEmpty()) format.clearProperty(InfoProperty);
    else format.setProperty(InfoProperty, want);
    QTextCursor edit(&doc);
    edit.setPosition(block.position());
    edit.setBlockFormat(format);
    return true;
}

static QStringList codeLanguagesNear(const QTextDocument& doc, int blockNumber) {
    struct Found {
        QString name;
        int distance = 0;
        bool above = false;
    };
    std::vector<Found> found;
    for (QTextBlock block = doc.begin(); block.isValid(); block = block.next()) {
        if (!isCodeBlock(block)) continue;
        const QString info = block.blockFormat().stringProperty(InfoProperty);
        if (info.isEmpty()) continue;
        if (block.blockNumber() == blockNumber) continue;
        found.push_back({info, qAbs(block.blockNumber() - blockNumber),
                         block.blockNumber() < blockNumber});
    }
    // Ближайший выше — первым: продолжают обычно то, что писали только что.
    std::stable_sort(found.begin(), found.end(), [](const Found& a, const Found& b) {
        if (a.above != b.above) return a.above;
        return a.distance < b.distance;
    });
    QStringList out;
    for (const Found& item : found)
        if (!out.contains(item.name)) out << item.name;
    return out;
}

static bool leaveCodeBlockAtCursor(QTextDocument& doc, QTextCursor& cursor) {
    (void)doc;   // документ берётся у курсора; подпись общая у всех операций
    if (!isCodeBlock(cursor.block())) return false;

    // Пустая строка, а не пустой абзац: пустая строка в этой модели — блок
    // VSpace, и ровно она получается при чтении файла. Абзац без текста был бы
    // состоянием, которого чтение файла не даёт, — а такого у нас не бывает.
    const QTextBlock last = cursor.block();
    QTextCursor edit(cursor);
    edit.setPosition(last.position() + last.length() - 1);
    edit.insertBlock(vspaceBlockFormat(false, false, styleOf(*edit.document())), QTextCharFormat());
    cursor = edit;
    return true;
}

static bool indentCodeAtCursor(QTextDocument& doc, QTextCursor& cursor) {
    const QVector<CodeLine> lines = touchedCodeLines(doc, cursor);
    if (lines.isEmpty()) return false;
    const int width = codeTabWidth();

    QTextCursor edit(cursor);
    // Одна строка и нет выделения — это набор: пробелы встают ПОД КАРЕТКОЙ и
    // ровно до ближайшего стопа, а не полной шириной. Всё остальное — отступ
    // строк целиком, на целый стоп каждая.
    if (lines.size() == 1 && !cursor.hasSelection()) {
        const CodeLine& line = lines.front();
        const int at = cursor.position() - line.start;
        const int column = visualColumn(codeLineText(doc, line), at, width);
        const int spaces = width - (column % width);
        edit.setPosition(cursor.position());
        edit.insertText(QString(spaces, QLatin1Char(' ')));
        cursor = edit;
        return true;
    }

    // Границы выделения держим смещениями в строках: текст под ними едет, а
    // «та же колонка той же строки» переживает правку. Правим с последней
    // строки к первой, чтобы позиции ещё не тронутых строк не плыли.
    const int anchor = cursor.anchor();
    const int position = cursor.position();
    QVector<int> delta(lines.size(), 0);
    for (int i = lines.size() - 1; i >= 0; --i) {
        const Lead lead = leadingIndent(codeLineText(doc, lines[i]), width);
        delta[i] = setLineIndent(edit, doc, lines[i], lead.columns + width);
    }

    QTextCursor restored(&doc);
    restored.setPosition(shiftedPosition(lines, delta, anchor));
    restored.setPosition(shiftedPosition(lines, delta, position), QTextCursor::KeepAnchor);
    cursor = restored;
    return true;
}

static bool outdentCodeAtCursor(QTextDocument& doc, QTextCursor& cursor) {
    const QVector<CodeLine> lines = touchedCodeLines(doc, cursor);
    if (lines.isEmpty()) return false;
    const int width = codeTabWidth();

    const int anchor = cursor.anchor();
    const int position = cursor.position();
    QTextCursor edit(cursor);
    QVector<int> delta(lines.size(), 0);
    bool moved = false;
    for (int i = lines.size() - 1; i >= 0; --i) {
        const Lead lead = leadingIndent(codeLineText(doc, lines[i]), width);
        if (lead.columns == 0) continue;
        // До БЛИЖАЙШЕГО стопа вниз, а не на целый стоп: строка, отступившая на
        // шесть пробелов, встаёт на четыре, а не на два.
        const int target = ((lead.columns - 1) / width) * width;
        delta[i] = setLineIndent(edit, doc, lines[i], target);
        if (delta[i] != 0) moved = true;
    }
    // Снимать нечего — но нажатие всё равно наше: Shift+Tab в коде не должен
    // проваливаться в списки или уводить фокус из окна.
    if (!moved) return true;

    QTextCursor restored(&doc);
    restored.setPosition(shiftedPosition(lines, delta, anchor));
    if (cursor.hasSelection())
        restored.setPosition(shiftedPosition(lines, delta, position), QTextCursor::KeepAnchor);
    cursor = restored;
    return true;
}

static bool toggleCommentAtCursor(QTextDocument& doc, QTextCursor& cursor) {
    const int from = qMin(cursor.anchor(), cursor.position());
    const int to = qMax(cursor.anchor(), cursor.position());
    const QTextBlock block = doc.findBlock(from);
    if (!block.isValid() || isRawBlock(block)) return false;

    switch (kindOf(block)) {
        case Kind::Math:
            // Формула комментарием не становится: её текст литерален, и `<!--`
            // внутри него — часть исходника, а не разметка.
            return false;
        case Kind::Html:
            // Обратно — целыми блоками: комментарий и так один блок.
            return setBlockKind(doc, cursor, {Kind::Paragraph, Marker::Bullet, false});
        case Kind::Paragraph:
        case Kind::Heading:
        case Kind::Quote:
        case Kind::ListItem:
            break;
        case Kind::Code:
        case Kind::VSpace:
        case Kind::Divider:
            // Коду комментарий не светит (там текст буквальный), пустой строке
            // и черте — нечего комментировать.
            return false;
    }

    // Ctrl+/ работает по СТРОКАМ, как в редакторах кода: перенос внутри блока
    // — не граница блока, и без выкройки комментарием становился бы весь блок
    // («выделил вторую строку пункта — закомментировался и сам пункт»).
    // Затронутые строки выкраиваются в свой блок, соседние строки остаются
    // тем, чем были. Многоблочное выделение работает целыми блоками.
    const QString text = block.text();
    if (doc.findBlock(to) == block && text.contains(QChar::LineSeparator)) {
        const int base = block.position();
        const int selBegin = from - base;
        const int selEnd = qMin(to - base, int(text.size()));
        const int lineStart =
            selBegin > 0
                ? int(text.lastIndexOf(QChar::LineSeparator, selBegin - 1)) + 1
                : 0;
        int lineEnd = int(text.indexOf(QChar::LineSeparator, selEnd));
        if (lineEnd < 0) lineEnd = int(text.size());

        if (lineStart > 0 || lineEnd < int(text.size())) {
            const Kind original = kindOf(block);
            const int firstNumber = block.blockNumber();
            QTextCursor edit(&doc);
            edit.beginEditBlock();
            // Разрезы: сзади, потом спереди — позиции не плывут. Разделитель
            // строк заменяется границей блока один в один, длины сохраняются.
            if (lineEnd < int(text.size())) {
                edit.setPosition(base + lineEnd);
                edit.setPosition(base + lineEnd + 1, QTextCursor::KeepAnchor);
                edit.removeSelectedText();
                edit.insertBlock();
            }
            if (lineStart > 0) {
                edit.setPosition(base + lineStart - 1);
                edit.setPosition(base + lineStart, QTextCursor::KeepAnchor);
                edit.removeSelectedText();
                edit.insertBlock();
            }

            // Выкроенная строка — комментарий; уровень наследуется, так что
            // внутри пункта он остаётся внутри пункта.
            QTextBlock target = doc.findBlock(base + lineStart);
            QTextBlockFormat tf = target.blockFormat();
            tf.setProperty(KindProperty, int(Kind::Html));
            tf.clearProperty(MarkerProperty);
            tf.clearProperty(CheckedProperty);
            tf.setHeadingLevel(0);
            edit.setPosition(target.position());
            edit.setBlockFormat(tf);

            // Хвостовые строки пункта маркера не имеют — это продолжение, а не
            // новый пункт: род снимается, уровень остаётся.
            if (original == Kind::ListItem && lineEnd < int(text.size())) {
                const QTextBlock suffix = target.next();
                if (suffix.isValid()) {
                    QTextBlockFormat sf = suffix.blockFormat();
                    sf.setProperty(KindProperty, int(Kind::Paragraph));
                    sf.clearProperty(MarkerProperty);
                    sf.clearProperty(CheckedProperty);
                    edit.setPosition(suffix.position());
                    edit.setBlockFormat(sf);
                }
            }
            const QTextBlock lastTouched =
                target.next().isValid() ? target.next() : target;
            normalise(doc, {firstNumber, lastTouched.blockNumber()});
            edit.endEditBlock();
            cursor.setPosition(base + lineStart);
            return true;
        }
    }

    return setBlockKind(doc, cursor, {Kind::Html, Marker::Bullet, false});
}

static bool uncommentAtBlockStart(QTextDocument& doc, QTextCursor& cursor) {
    if (!cursor.atBlockStart() || cursor.hasSelection()) return false;
    const QTextBlock block = cursor.block();
    if (!block.isValid() || isRawBlock(block) || kindOf(block) != Kind::Html) return false;
    return setBlockKind(doc, cursor, {Kind::Paragraph, Marker::Bullet, false});
}

static bool toggleTaskAtCursor(QTextDocument& doc, QTextCursor& cursor) {
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

static bool splitBlockAtCursor(QTextDocument& doc, QTextCursor& cursor);

static bool splitBlockOtherwiseAtCursor(QTextDocument& doc, QTextCursor& cursor) {
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

// Enter в литеральном блоке (код, дословный кусок): новая строка внутри того же блока — разделитель с
// пометкой перевода строки, а за ним отступ предыдущей строки (в коде он почти
// всегда тот же, и набирать его заново на каждой строке мучительно). Всё одной
// скобкой: Enter с отступом отменяется одним Ctrl+Z.
bool insertCodeLine(QTextDocument& doc, QTextCursor& cursor, const QTextBlock& block) {
    const int from = qMin(cursor.anchor(), cursor.position());
    const CodeLine line = codeLineAt(doc, from);
    const QString text = codeLineText(doc, line);
    int i = 0;
    while (i < text.size() && (text.at(i) == QLatin1Char(' ') || text.at(i) == QLatin1Char('\t')))
        ++i;
    // Отступ берём только до каретки: если она левее отступа, копировать нечего.
    const QString indent = text.left(qMin(i, from - line.start));

    QTextCharFormat separator = block.charFormat();
    separator.setProperty(BreakSourceProperty, int(BreakNewline));
    QTextCharFormat plain = block.charFormat();
    plain.clearProperty(BreakSourceProperty);

    cursor.beginEditBlock();
    if (cursor.hasSelection()) cursor.removeSelectedText();
    cursor.insertText(QString(QChar::LineSeparator), separator);
    if (!indent.isEmpty()) cursor.insertText(indent, plain);
    // Текст блока кончается переводом строки — признак ставится, даже если у
    // блока его не было: без него только что заведённая пустая строка исчезала
    // бы при записи (текст «⏎» без признака — это одна строка, а не две).
    if (!block.blockFormat().boolProperty(TrailingNewlineProperty)) {
        QTextBlockFormat format = block.blockFormat();
        format.setProperty(TrailingNewlineProperty, true);
        QTextCursor fix(&doc);
        fix.setPosition(block.position());
        fix.setBlockFormat(format);
    }
    cursor.endEditBlock();
    return true;
}

}  // namespace

static bool splitBlockAtCursor(QTextDocument& doc, QTextCursor& cursor) {
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
    // Блок кода — один QTextBlock, забор — его ПОСЛЕДНЯЯ СТРОКА (не первая:
    // блоку с одной строкой-забором закрываться нечем), и каретка стоит на ней.
    if (isCodeBlock(block)) {
        const QString text = block.text();
        const int lastSep = int(text.lastIndexOf(QChar::LineSeparator));
        QString closing;
        const bool closedByFence = lastSep >= 0 && cursor.positionInBlock() > lastSep &&
                                   !cursor.hasSelection() &&
                                   fenceLanguage(text.mid(lastSep + 1), closing) &&
                                   closing.isEmpty();
        if (closedByFence) {
            // Высоту строки ставим ТАК ЖЕ, КАК СБОРЩИК, а не копией у соседа:
            // копия тащит за собой и явные нули там, где сборщик не пишет ничего.
            QTextBlockFormat plain;
            const ZDocStyle& style = styleOf(*cursor.document());
            applyLineHeight(plain, style.lineHeightFactor(), layoutBaseFont(style).pointSizeF(),
                            layoutBaseFont(style), style);

            cursor.beginEditBlock();
            // Забор в текст не переносим: он был командой закрыть блок, а не
            // содержимым, — уходит вместе со своим разделителем.
            cursor.setPosition(block.position() + lastSep);
            cursor.setPosition(block.position() + text.size(), QTextCursor::KeepAnchor);
            cursor.removeSelectedText();
            // За блоком — обычный абзац, и каретка в нём.
            cursor.insertBlock(plain, QTextCharFormat());
            normalise(doc, around(cursor.blockNumber()));
            cursor.endEditBlock();
            return true;
        }
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

    // Пустой пункт списка: Enter выводит из него, а не заводит ещё один пустой.
    // С любой глубины сразу: подниматься по уровню за раз, как это делают другие
    // редакторы, здесь не годится — из вложенного списка приходилось бы выходить
    // тремя нажатиями вместо двух, и это сразу заметили на живой заметке. А
    // подняться на уровень, оставшись в списке, и так можно: Enter и Shift+Tab.
    //
    // Соседей при этом не трогаем. Раньше пустой пункт снимал маркер со
    // СЛЕДУЮЩЕГО — это был жест «разлепить два слипшихся списка». Отличить его
    // от «дописал пункт и выхожу» по документу нечем, а цена ошибки высока:
    // два Enter в середине списка снимали чекбокс у пункта, которого человек не
    // касался. Разлепить списки можно и без этого: выйти из списка и набрать
    // разделяющий абзац — markdown всё равно разделяет их только абзацем с
    // содержимым, пустая строка между пунктами не разделяет ничего.
    if (isListBlock(block) && block.text().isEmpty()) {
        QTextBlockFormat next = block.blockFormat();
        next.setProperty(KindProperty, int(Kind::Paragraph));
        next.clearProperty(MarkerProperty);
        next.clearProperty(CheckedProperty);
        next.clearProperty(LevelProperty);
        next.clearProperty(QTextFormat::BlockLeftMargin);

        cursor.beginEditBlock();
        cursor.setPosition(block.position());
        cursor.setBlockFormat(next);
        // Место держим курсором, а не номером блока: нормализация заводит перед
        // абзацем пустую строку, и номер устаревает прямо посреди операции.
        QTextCursor landing(&doc);
        landing.setPosition(block.position());
        normalise(doc, around(number));
        cursor.endEditBlock();
        cursor.setPosition(landing.position());
        return true;
    }

    // Литеральный блок (код, дословный кусок): Enter — новая СТРОКА того же
    // блока, а не новый блок.
    if (isLiteralBlock(block)) return insertCodeLine(doc, cursor, block);

    // Разрез в начале пункта переставляет половинки ролями: текст целиком
    // уезжает в НИЖНЮЮ, а пустым остаётся верхний блок (подробнее — ниже, там
    // где курсор возвращается наверх). Формат готовится с оглядкой на это.
    const bool atListStart = isListBlock(block) && cursor.positionInBlock() == 0;
    QTextBlockFormat next = format;

    {
        next.clearProperty(TrailingNewlineProperty);
        // Новый пункт всегда невыполненный: отмечать за человека нечего.
        // В начале пункта новый — это ВЕРХНИЙ блок, а нижнему достаётся весь
        // прежний текст, и отметку он обязан сохранить. Её снимает отдельная
        // ветка после разреза.
        if (isTaskBlock(block) && !atListStart) next.setProperty(CheckedProperty, false);
        switch (kindOf(block)) {
            case Kind::Math:
                // Enter внутри формулы — просто перевод строки в её исходнике:
                // выключная формула законно занимает несколько строк.
                break;
            case Kind::Heading:
                // За заголовком идёт обычный текст, а не второй заголовок.
                //
                // Но только когда режем по тексту: в начале строки текст целиком
                // уезжает в НИЖНЮЮ половину, и заголовком перестал бы быть он
                // сам. Так "## Редактор" превращался в пустой "##" и абзац
                // "Редактор" — а человек всего лишь хотел отбить заголовок
                // сверху пустой строкой.
                if (cursor.positionInBlock() > 0) {
                    next.setProperty(KindProperty, int(Kind::Paragraph));
                    next.setHeadingLevel(0);
                }
                break;
            case Kind::Divider:
                // Черта одна, и текста в ней нет: всё, что Enter заводит под
                // ней, — обычный текст.
                next.setProperty(KindProperty, int(Kind::Paragraph));
                break;
            case Kind::Html:
                // Комментарий не расползается: новая строка под ним — обычный
                // текст, а многострочный комментарий делается Shift+Enter.
                next.setProperty(KindProperty, int(Kind::Paragraph));
                break;
            case Kind::Paragraph:
            case Kind::Code:
            case Kind::Quote:
            case Kind::VSpace:
            case Kind::ListItem:
                break;
        }
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
    // Enter в начале заголовка отбивает его сверху пустой строкой: заголовок
    // уезжает вниз целиком, а над ним встаёт пустая строка. Курсор остаётся с
    // заголовком — человек двигал именно его.
    const bool atHeadingStart = kindOf(block) == Kind::Heading &&
                                cursor.positionInBlock() == 0;

    cursor.beginEditBlock();
    // Половинки просто разъезжаются. Пустую строку между ними, если markdown её
    // требует, поставит нормализующий проход — правило записано там одно на все
    // операции, и второй его копии здесь быть не должно.
    cursor.insertBlock(next, block.charFormat());

    // Номера блоков считаем после правки, а не до. При выделении из нескольких
    // блоков разрез его же и съедает, и номера съезжают: взятые заранее
    // указывали бы мимо, и нормализация проходила бы не по тому месту.
    const int landed = cursor.blockNumber();
    // Верхняя половина пункта пуста — это и есть только что заведённый пункт,
    // и выполненным ему быть не с чего. Отмечено было то, что уехало вниз.
    if (atListStart && landed > 0 && isTaskBlock(block)) {
        QTextBlockFormat fresh = doc.findBlockByNumber(landed - 1).blockFormat();
        fresh.setProperty(CheckedProperty, false);
        QTextCursor above(&doc);
        above.setPosition(doc.findBlockByNumber(landed - 1).position());
        above.setBlockFormat(fresh);
    }
    // Верхняя половина заголовка пуста и заголовком быть не должна: это та самая
    // пустая строка, ради которой Enter и нажали.
    if (atHeadingStart && landed > 0) {
        QTextBlockFormat blank = doc.findBlockByNumber(landed - 1).blockFormat();
        blank.setProperty(KindProperty, int(Kind::Paragraph));
        blank.setHeadingLevel(0);
        QTextCursor above(&doc);
        above.setPosition(doc.findBlockByNumber(landed - 1).position());
        above.setBlockFormat(blank);
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

namespace {
// Определён ниже, у операций пустых строк; нужен и снятию маркера.
void joinWithNext(QTextDocument& doc, QTextCursor& edit, int number);
}  // namespace

static bool unwrapListItemAtCursor(QTextDocument& doc, QTextCursor& cursor) {
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
    if (sameList) {
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
    plain.setProperty(KindProperty, int(Kind::Paragraph));
    plain.clearProperty(LevelProperty);
    plain.clearProperty(QTextFormat::BlockLeftMargin);

    const int number = block.blockNumber();
    cursor.beginEditBlock();
    cursor.setBlockFormat(plain);
    // Разжалованный абзац может слипаться с верхним соседом — тогда они
    // сливаются в один блок мягким переносом: строки на экране как стояли,
    // так и стоят. Иначе нормализация вставила бы обязательную пустую строку,
    // и документ РОС бы от нажатия Backspace.
    QTextCursor landing(&doc);
    landing.setPosition(cursor.block().position());
    if (number > 0 && blocksWouldMerge(doc.findBlockByNumber(number - 1),
                                       doc.findBlockByNumber(number))) {
        QTextCursor edit(&doc);
        joinWithNext(doc, edit, number - 1);
    }
    normalise(doc, around(qMax(0, number - 1)));
    cursor.endEditBlock();
    cursor.setPosition(landing.position());
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
// backspace — зовут удалением назад: у прогона пустых строк это значит «строка
// выше исчезает, каретка уходит вверх», а не «нижняя подтягивается на место».
// Убрать блок-строку целиком, не тронув соседей. Qt при слиянии оставляет
// формат не того блока, который выжил, поэтому формат выжившего ставится явно.
void removeLineBlock(QTextCursor& edit, const QTextBlock& block) {
    const QTextBlock after = block.next();
    const QTextBlock keeper = after.isValid() ? after : block.previous();
    const QTextBlockFormat keep = keeper.blockFormat();
    // Позиции берём ДО правки: после неё хэндл блока протухает, и его
    // position() может отдать что угодно — формат уезжал в нулевой блок.
    const int at = block.position();
    const int keeperAt = keeper.position();
    if (after.isValid()) {
        edit.setPosition(at);
        edit.setPosition(at + 1, QTextCursor::KeepAnchor);
        edit.removeSelectedText();
        edit.setPosition(at);
    } else {
        edit.setPosition(at - 1);
        edit.setPosition(at, QTextCursor::KeepAnchor);
        edit.removeSelectedText();
        edit.setPosition(keeperAt);
    }
    edit.setBlockFormat(keep);
}

bool removeVSpaceAndMaybeJoin(QTextDocument& doc, QTextCursor& cursor, int gapNumber,
                              bool backspace = false) {
    const QTextBlock gap = doc.findBlockByNumber(gapNumber);
    if (!isVSpaceBlock(gap)) return false;
    const QTextBlock before = gap.previous();
    const QTextBlock after = gap.next();
    if (!before.isValid() && !after.isValid()) return false;
    const bool fromGap = cursor.blockNumber() == gapNumber;

    // Под пустой строкой пустой же абзац — человек видит две пустых строки
    // подряд, и одна из них дубль. Ветка применима, ТОЛЬКО когда обычный путь
    // сломал бы: сосед сверху слипается с пустым абзацем (пункт, абзац,
    // цитата), и слияние утащило бы пустой абзац внутрь — обе строки исчезали
    // разом. Если соседство законно (черта, заголовок), работает общий путь:
    // гибнет то, что над кареткой. Черта тоже пуста, но абзацем не является.
    const bool caretOnAfter = after.isValid() && cursor.blockNumber() == after.blockNumber();
    if (after.isValid() && !isRawBlock(after) && after.text().isEmpty() &&
        !isVSpaceBlock(after) && !isListBlock(after) && kindOf(after) != Kind::Divider &&
        before.isValid() && blocksWouldMerge(before, after) && (fromGap || caretOnAfter)) {
        QTextCursor edit(&doc);
        edit.beginEditBlock();
        edit.setPosition(after.position() - 1);
        edit.setPosition(after.position(), QTextCursor::KeepAnchor);
        edit.removeSelectedText();
        edit.setPosition(doc.findBlockByNumber(gapNumber).position());
        edit.setBlockFormat(vspaceBlockFormat(isVSpaceBlock(before), gapNumber == 0, styleOf(doc)));
        normalise(doc, around(gapNumber));
        edit.endEditBlock();
        if (caretOnAfter && !fromGap) {
            // Каретка стояла на пустом абзаце: остаётся на пустой строке,
            // вставшей на его место, — а не прыгает через неё вверх.
            cursor.setPosition(doc.findBlockByNumber(gapNumber).position());
        } else {
            const QTextBlock landed = doc.findBlockByNumber(qMax(0, gapNumber - 1));
            cursor.setPosition(landed.position() + landed.length() - 1);
        }
        return true;
    }

    // ПУСТУЮ СТРОКУ РЯДОМ С ОБЪЕКТОМ УБРАТЬ НЕЛЬЗЯ: она там обязательна — без
    // неё абзац объекта слипся бы в файле с соседом. Убрав её, мы получили бы
    // её же обратно от инварианта, и клавиша выглядела бы сломанной; слив
    // соседей, потеряли бы объект.
    //
    // Отказ и шаг: каретка идёт туда, куда её вёл Backspace, а документ цел.
    // То же правило, что у пустой строки ПОД объектом, — и по той же причине.
    if ((before.isValid() && objectOf(before).valid()) ||
        (after.isValid() && objectOf(after).valid())) {
        if (backspace && before.isValid())
            cursor.setPosition(before.position() + before.length() - 1);
        else if (after.isValid())
            cursor.setPosition(after.position());
        return true;
    }

    // Жертва всегда одна — сама пустая строка, и слипшиеся после её ухода
    // ТЕКСТЫ сливаются в один блок: это и есть смысл Backspace на стыке.
    // Разделителю особый случай не нужен: канон "___" ни с чем не слипается.
    const bool join = blocksMayJoin(before, after);

    // Куда встать. Backspace с самой пустой строки — удаление назад: каретка
    // уходит в конец строки выше, какой бы та ни была (текст, пустая, черта).
    // Всё прочее — Delete и Backspace из-под стыка — оставляет каретку на
    // строке под стыком. При слиянии текстов оба адреса — одна и та же точка.
    QTextCursor landing(&doc);
    if (backspace && fromGap && before.isValid()) {
        // Слияние текстов вставляет мягкий перенос ровно в точку посадки «в
        // конец строки выше» — без флага каретку проталкивало за перенос, на
        // начало нижней строки. Посадке «на начало строки ниже» (ветки ниже)
        // флаг, наоборот, вредил бы: ей за перенос уехать и положено.
        landing.setKeepPositionOnInsert(true);
        landing.setPosition(before.position() + before.length() - 1);
    }
    else if (after.isValid())
        landing.setPosition(after.position());
    else
        landing.setPosition(before.position() + before.length() - 1);

    QTextCursor edit(&doc);
    edit.beginEditBlock();
    removeLineBlock(edit, gap);
    if (join) joinWithNext(doc, edit, gapNumber - 1);
    normalise(doc, around(qMax(0, gapNumber - 1)));
    edit.endEditBlock();
    cursor.setPosition(landing.position());
    return true;
}

}  // namespace

static bool deleteDividerAbove(QTextDocument& doc, QTextCursor& cursor) {
    if (cursor.hasSelection() || !cursor.atBlockStart()) return false;
    const QTextBlock prev = cursor.block().previous();
    if (!prev.isValid() || isRawBlock(prev) || kindOf(prev) != Kind::Divider) return false;

    const int number = prev.blockNumber();
    QTextCursor landing(&doc);
    landing.setPosition(cursor.block().position());
    // Соседи, оставшиеся без черты между ними, могут слипнуться. Слипшиеся
    // тексты сливаются в один блок — как при удалении пустой строки: убрать
    // строку Backspace-ом и получить взамен новую пустую было бы нелепо.
    const bool join = blocksMayJoin(prev.previous(), cursor.block());
    QTextCursor edit(&doc);
    edit.beginEditBlock();
    removeLineBlock(edit, prev);
    if (join) joinWithNext(doc, edit, number - 1);
    normalise(doc, around(qMax(0, number - 1)));
    edit.endEditBlock();
    cursor.setPosition(landing.position());
    return true;
}

static bool applyDividerRuleAtCursor(QTextDocument& doc, QTextCursor& cursor) {
    const QTextBlock block = cursor.block();
    if (isRawBlock(block) || kindOf(block) != Kind::Paragraph) return false;
    if (levelOf(block) >= 0) return false;      // в списке дефисы — текст
    if (!cursor.atBlockEnd()) return false;     // пробел в середине — просто пробел

    // Enter в абзаце — перенос строки внутри блока, а не новый блок, поэтому
    // правило смотрит на последнюю СТРОКУ, а не на весь текст. Зовётся ДО
    // вставки пробела или Enter — они не должны попадать в шаг истории, — так
    // что строка обычно "---"; один концевой пробел всё же прощаем. Дальше —
    // только дефисы или только подчёркивания, как в каноне.
    const QString text = block.text();
    const int lineStart = int(text.lastIndexOf(QChar::LineSeparator)) + 1;
    QString line = text.mid(lineStart);
    if (line.endsWith(QLatin1Char(' '))) line.chop(1);
    if (line.size() < 3) return false;
    const QChar mark = line.at(0);
    if (mark != QLatin1Char('-') && mark != QLatin1Char('_')) return false;
    for (const QChar& c : line)
        if (c != mark) return false;

    const int number = block.blockNumber();
    QTextCursor edit(&doc);
    edit.beginEditBlock();
    QTextBlock dividerBlock;
    if (lineStart == 0) {
        // Вся строка и есть блок: он и становится чертой. Дефисы стираем —
        // черта это блок без текста, её рисует вид.
        edit.setPosition(block.position());
        edit.setPosition(block.position() + text.size(), QTextCursor::KeepAnchor);
        edit.removeSelectedText();
        QTextBlockFormat divider = block.blockFormat();
        divider.setProperty(KindProperty, int(Kind::Divider));
        divider.clearProperty(LevelProperty);
        divider.clearProperty(QTextFormat::BlockLeftMargin);
        edit.setBlockFormat(divider);
        dividerBlock = block;
    } else {
        // Дефисы — последняя строка абзаца: строка вместе со своим переносом
        // уходит из блока, черта встаёт отдельным блоком под ним. Пустую строку
        // между абзацем и чертой вернёт normalise — без неё черта прочлась бы
        // setext-заголовком.
        edit.setPosition(block.position() + lineStart - 1);
        edit.setPosition(block.position() + text.size(), QTextCursor::KeepAnchor);
        edit.removeSelectedText();
        edit.movePosition(QTextCursor::EndOfBlock);
        edit.insertBlock();
        QTextBlockFormat divider = edit.blockFormat();
        divider.setProperty(KindProperty, int(Kind::Divider));
        divider.clearProperty(LevelProperty);
        divider.clearProperty(QTextFormat::BlockLeftMargin);
        edit.setBlockFormat(divider);
        dividerBlock = edit.block();
    }
    // Каретке на черте делать нечего: набор на ней превратил бы её обратно в
    // текст. Есть блок ниже — уходим на него; черта последняя — заводим под ней
    // пустой абзац: при сохранении хвостовой пустой блок и так не печатается.
    QTextCursor landing(&doc);
    if (dividerBlock.next().isValid()) {
        landing.setPosition(dividerBlock.next().position());
    } else {
        edit.setPosition(dividerBlock.position());
        edit.insertBlock();
        QTextBlockFormat plain = edit.blockFormat();
        plain.setProperty(KindProperty, int(Kind::Paragraph));
        edit.setBlockFormat(plain);
        landing.setPosition(edit.position());
    }
    normalise(doc, around(number));
    edit.endEditBlock();
    cursor.setPosition(landing.position());
    return true;
}

static bool repairAfterTyping(QTextDocument& doc, QTextCursor& cursor) {
    const QTextBlock block = cursor.block();
    const int number = block.blockNumber();
    // Набрали прямо на пустой строке или на черте: тем, чем были, они быть
    // перестали — ТЕКСТ делает из них обычный абзац. Пробелы текстом не
    // считаются: markdown пробельную строку считает пустой, а превращение в
    // абзац запускало слияние соседей — набранный на пустой строке пробел
    // приклеивал текст под ней к списку над ней. Пробелы умрут сами, когда
    // каретка уйдёт со строки (tidyLeftLine).
    const bool filled = (isVSpaceBlock(block) ||
                         (!isRawBlock(block) && kindOf(block) == Kind::Divider)) &&
                        !block.text().trimmed().isEmpty();
    // Или набрали поверх выделения, съевшего границу блоков, и рядом оказались
    // соседи, которых markdown раздельно не выражает.
    //
    // ЗАМЕЧЕНО НА ЭТАПЕ 11, НЕ ПОЧИНЕНО: строки одного блока кода — это тоже
    // «соседи, которые слились бы», и первый же набранный в блоке знак склеивает
    // их в один QTextBlock. На смысл это не влияет (файл выходит тот же), но
    // нарезка блока кода по строкам, ради которой всё затевалось (4257 мкс
    // против 109, см. doc_model.h), после первой правки пропадает. Оговорка
    // «строки одного литерального блока не сливать» ломает инвариант разбивки
    // на всех трёх фаззерах, то есть трогать надо не здесь; решение за
    // владельцем, замер и разбор — в отчёте этапа 11.
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

    // Слияние соседей меняет разделитель блока на перенос строки В ТОЧКЕ
    // каретки; без этого флага каретка уезжала за вставленный перенос, и
    // продолжение набора ложилось в начало нижней строки.
    const bool kept = cursor.keepPositionOnInsert();
    cursor.setKeepPositionOnInsert(true);

    QTextCursor edit(&doc);
    edit.beginEditBlock();
    if (filled) {
        QTextBlockFormat format = block.blockFormat();
        format.setProperty(KindProperty, int(Kind::Paragraph));
        format.clearProperty(LevelProperty);
        format.clearProperty(QTextFormat::BlockLeftMargin);
        setBlockFormat(edit, block, format);
    }

    // Соседей сливаем, а не раздвигаем пустой строкой: набор ничего на экране
    // раздвигать не должен. Строки как стояли, так и стоят, меняется только
    // строение — три строки подряд без пустой между ними markdown и называет
    // одним абзацем.
    // ОБЪЕКТ НЕ СЛИВАЕТСЯ НИ С ЧЕМ (blocksMayJoin): он атом, и слияние делает
    // его строкой обычного текста. Раздвинуть соседей пустой строкой — не
    // «двигать текст на экране» без нужды: в файле они слиплись бы, и пустая
    // строка там ОБЯЗАНА стоять. Её и поставит инвариант ниже (normalise).
    if (blocksMayJoin(doc.findBlockByNumber(number), doc.findBlockByNumber(number + 1)))
        joinWithNext(doc, edit, number);
    if (number > 0 &&
        blocksMayJoin(doc.findBlockByNumber(number - 1), doc.findBlockByNumber(number)))
        joinWithNext(doc, edit, number - 1);

    normalise(doc, around(qMax(0, number - 1)));
    edit.endEditBlock();
    cursor.setKeepPositionOnInsert(kept);
    // Позицию флаг удержал, а якорь уехал за вставленный перенос — каретка
    // обязана остаться схлопнутой, иначе следующий знак заменит перенос.
    cursor.setPosition(cursor.position());
    return true;
}

static bool joinAcrossVSpaceBackward(QTextDocument& doc, QTextCursor& cursor) {
    if (cursor.hasSelection() || !cursor.atBlockStart()) return false;
    const QTextBlock block = cursor.block();
    // Курсор стоит на самой пустой строке: Backspace убирает её и уводит курсор
    // в конец предыдущей — как в любом редакторе.
    if (isVSpaceBlock(block)) {
        if (!block.previous().isValid()) return false;   // выше ничего нет
        return removeVSpaceAndMaybeJoin(doc, cursor, block.blockNumber(), true);
    }
    const QTextBlock gap = block.previous();
    if (!isVSpaceBlock(gap)) return false;
    return removeVSpaceAndMaybeJoin(doc, cursor, gap.blockNumber(), true);
}

static bool joinAcrossVSpaceForward(QTextDocument& doc, QTextCursor& cursor) {
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

// СОБСТВЕННЫЙ отступ блока — тот, что был бы у него вне всякого списка: у кода
// это плашка со своим внутренним полем, у цитаты её поле, у прочих ноль.
// Спрашивается он в двух местах — внутри пункта прибавляется к колонке пункта, а
// снаружи списка он и есть весь отступ, — и потому живёт одной функцией. Числа
// те же, что кладёт сборщик (document_builder, Kind::Code и Kind::Quote).
static qreal ownLeftMargin(const QTextBlock& block, const CodePlate& plate, qreal charUnit) {
    if (isRawBlock(block)) return 0;
    switch (kindOf(block)) {
        case Kind::Code:  return plate.indent + plate.padLeft;
        case Kind::Quote: return styleOf(*block.document()).quoteIndent() * charUnit;
        // У формулы собственного отступа нет: она встаёт по центру колонки, а её
        // исходник виден только в правке.
        case Kind::Math:
        case Kind::Paragraph:
        case Kind::Heading:
        case Kind::VSpace:
        case Kind::ListItem:
        case Kind::Divider:
        case Kind::Html:
            break;
    }
    return 0;
}

// Поставить блоку левое поле, а нулевое — снять вовсе: у блока без отступа
// свойства нет ВООБЩЕ, и свойство со значением 0 — это уже другой формат.
// Отладочная сверка со сборкой сравнивает свойства поимённо и такую разницу
// видит (на ней я и поймал, что снятый уровень уносил с собой плашку кода).
static void setLeftMarginTo(QTextCursor& cursor, const QTextBlock& block, qreal margin) {
    QTextBlockFormat format = block.blockFormat();
    const bool has = format.hasProperty(QTextFormat::BlockLeftMargin);
    if (margin <= 0.0) {
        if (!has) return;
        format.clearProperty(QTextFormat::BlockLeftMargin);
        setBlockFormat(cursor, block, format);
        return;
    }
    if (has && std::fabs(format.leftMargin() - margin) < 0.01) return;
    format.setLeftMargin(margin);
    setBlockFormat(cursor, block, format);
}

void applyListGeometry(QTextDocument& doc, BlockRange range) {
    const BlockRange full = expandToRuns(doc, range);
    const ZDocStyle& style = styleOf(doc);
    const QFont base = layoutBaseFont(style);
    // Единицы — те же, что у сборщика: геометрия строится в базовом шрифте и
    // за зумом не идёт (см. layoutCharUnit).
    const qreal charUnit = layoutCharUnit(style);
    const qreal indent = style.listIndent() * charUnit;
    const CodePlate plate = codePlate(style);

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
                qMax(widest[size_t(run)], markerColumn(style, ordinal, level, base));
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
        // Пустая строка отступа не имеет никакого — ни своего, ни списочного.
        if (run == kSkip) {
            setLeftMarginTo(cursor, block, 0);
            continue;
        }
        if (run == kInside) {
            // Колонка своего пункта — та самая, от которой начинается его текст.
            // К ней прибавляется собственный отступ блока: у кода и цитаты он
            // свой, и внутри пункта он тоже нужен.
            const size_t at = size_t(qMax(0, levelOf(block))) + 1;
            // Колонки своего пункта нет — значит пункта над ним в этом прогоне
            // и нет: блок стоит сам по себе и отступ у него собственный.
            const qreal own = ownLeftMargin(block, plate, charUnit);
            if (at >= contentCol.size()) {
                setLeftMarginTo(cursor, block, own);
                continue;
            }
            setLeftMarginTo(cursor, block, indent + contentCol[at] + own);
            continue;
        }
        // Блок вне всякого списка: весь его отступ — собственный. Ставит его
        // ТОЖЕ ЭТА функция, а не только сборщик: правка, снявшая с блока
        // уровень, уносила вместе с уровнем и левое поле — блок кода терял
        // плашку, и заплатка расходилась с полной сборкой.
        if (run < 0) {
            setLeftMarginTo(cursor, block, ownLeftMargin(block, plate, charUnit));
            contentCol.assign(1, 0.0);
            continue;
        }

        const int level = qMax(0, levelOf(block));
        if (int(contentCol.size()) <= level + 1) contentCol.resize(size_t(level) + 2, 0.0);

        const qreal cell = widest[size_t(run)];
        contentCol[size_t(level) + 1] = contentCol[size_t(level)] + cell;

        // Формат не трогаем, если поле и так верное: любая запись помечает
        // документ изменённым и тянет за собой автосохранение (это внутри
        // setLeftMarginTo).
        setLeftMarginTo(cursor, block, indent + contentCol[size_t(level)] + cell);
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
        if (isVSpaceBlock(block)) {
            // У пустой строки уровня не бывает по построению (levelOf для неё
            // всегда -1) — застрявшее свойство и отступ вычищаем, чтобы мусор
            // от правок не жил в документе и не рисовал ложный отступ.
            if (block.blockFormat().hasProperty(LevelProperty)) {
                QTextBlockFormat format = block.blockFormat();
                format.clearProperty(LevelProperty);
                format.clearProperty(QTextFormat::BlockLeftMargin);
                setBlockFormat(cursor, block, format);
            }
            continue;   // просторный список — тот же список
        }
        if (!isListBlock(block)) {
            const int inside = levelOf(block);
            if (inside < 0) {
                open.clear();
                continue;
            }
            // Блок внутри пункта: глубже открытого уровня ему быть не с чего, а
            // без списка вокруг он и вовсе обычный абзац. МЕЛЬЧЕ открытого —
            // можно: абзац (объект, код) на уровне M под пунктом уровня N > M
            // принадлежит пункту уровня M и ЗАКРЫВАЕТ вложенные — ровно так
            // читает файл md4c («  текст» после «  - b» — продолжение «- a»).
            // Так работают Tab/Shift+Tab по одному уровню (решение владельца,
            // сессия 5); прежде такой блок прижимался к самому глубокому.
            const int deepest = int(open.size()) - 1;
            if (inside == deepest) continue;
            if (inside < deepest) {
                open.resize(size_t(inside) + 1);
                continue;
            }
            QTextBlockFormat format = block.blockFormat();
            if (deepest < 0) {
                format.clearProperty(LevelProperty);
                format.clearProperty(QTextFormat::BlockLeftMargin);
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
    fix.setBlockFormat(vspaceBlockFormat(false, number == 0, styleOf(doc)));
    // И ФОРМАТ ЗНАКОВ ТОЖЕ ЧИСТЫЙ. Разрез копирует его у соседа, а сосед бывает
    // блоком кода — и пустая строка оставалась набранной моноширинным шрифтом.
    // Пока после каждой операции шла полная пересборка, это чинилось само;
    // теперь пересборки нет, и поймала сверка со сборкой.
    QTextCharFormat plain;
    setFontStep(plain, 0);
    fix.setBlockCharFormat(plain);
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
            // ПРОБЕЛЫ ТЕКСТОМ НЕ СЧИТАЮТСЯ — так же, как их не считает
            // сторож инварианта (gapInvariantHolds): пока каретка на строке,
            // пробелы живут, а уйдёт — их снимет уборка. Считать их текстом
            // значило бы, что пробел на пустой строке превращает её в абзац и
            // склеивает соседей (поймал набор списков).
            if (!isVSpaceBlock(block) || block.text().trimmed().isEmpty()) continue;
            QTextBlockFormat format = block.blockFormat();
            format.setProperty(KindProperty, int(Kind::Paragraph));
            format.clearProperty(LevelProperty);
            format.clearProperty(QTextFormat::BlockLeftMargin);
            // И СЛЕДЫ ПУСТОЙ СТРОКИ. Её формат ставит нижнее поле и высоту
            // строки ЯВНО (vspaceBlockFormat), а у обычного блока сборщик их не
            // ставит вовсе — и оставленные следы делали блок непохожим на
            // собранный. Прежде это чинила полная пересборка после каждой
            // правки; её больше нет, и чинить надо здесь.
            format.clearProperty(QTextFormat::BlockBottomMargin);
            format.clearProperty(QTextFormat::LineHeight);
            format.clearProperty(QTextFormat::LineHeightType);
            const ZDocStyle& style = styleOf(*edit.document());
            applyLineHeight(format, style.lineHeightFactor(), layoutBaseFont(style).pointSizeF(),
                            layoutBaseFont(style), style);
            setBlockFormat(edit, block, format);
        }
    }

    // ФОРМАТ ЗНАКОВ ПУСТОЙ СТРОКИ — ЧИСТЫЙ. Блок, ставший пустой строкой (текст
    // сняла уборка, соседа съело удаление), приносит с собой формат знаков
    // прежнего содержимого: моноширинный шрифт кода, пометку мягкого переноса.
    // Сборщик у пустой строки не ставит ничего, кроме ступени кегля, — и
    // оставленный след делал блок непохожим на собранный.
    {
        QTextCharFormat plain;
        setFontStep(plain, 0);
        int i = qMax(0, range.first);
        const int last = qMin(range.last, doc.blockCount() - 1);
        for (; i <= last; ++i) {
            const QTextBlock block = doc.findBlockByNumber(i);
            if (!isVSpaceBlock(block) || block.charFormat() == plain) continue;
            QTextCursor fix(&doc);
            fix.setPosition(block.position());
            fix.setBlockCharFormat(plain);
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
            if (!blocksWouldMerge(block.previous(), block)) continue;
            insertVSpaceBefore(doc, i);
            ++added;
            ++i;      // на месте i теперь только что заведённая пустая строка
            ++last;
        }
    }

    // Поля сверху: их держит соседство, и после вставки они могли устареть.
    {
        const ZDocStyle& style = styleOf(doc);
        const qreal lineUnit = layoutLineUnit(style);
        int i = qMax(0, range.first);
        const int last = qMin(range.last + 2, doc.blockCount() - 1);
        for (; i <= last; ++i) {
            const QTextBlock block = doc.findBlockByNumber(i);
            if (!block.isValid()) break;
            // Поле спрашиваем у сборщика целиком, в пикселях: кроме отбивки в
            // нём живёт резерв под полоску блока кода, и считать его тут
            // заново значило бы стирать резерв на каждой операции.
            const qreal want = blockTopMarginPx(kindOf(block), isRawBlock(block),
                                                isVSpaceBlock(block.previous()), i == 0,
                                                lineUnit, style);
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
        if (isVSpaceBlock(block) && !block.text().trimmed().isEmpty()) {
            // Пробельное содержимое допустимо: пока каретка на строке, пробелы
            // живут; уйдёт — их снимет tidyLeftLine.
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

QString tidyProblem(const QTextDocument& doc, const QTextCursor& caret) {
    const int caretBlock = caret.blockNumber();
    int caretLine = 0;
    {
        const QString text = caret.block().text();
        for (int i = 0; i < caret.positionInBlock() && i < text.size(); ++i)
            if (text.at(i) == QChar::LineSeparator) ++caretLine;
    }
    for (QTextBlock block = doc.begin(); block.isValid(); block = block.next()) {
        if (isRawBlock(block)) continue;
        if (kindOf(block) == Kind::Code) continue;
        const QString text = block.text();
        int line = 0;
        int from = 0;
        for (int i = 0; i <= text.size(); ++i, ++from) {
            if (i != text.size() && text.at(i) != QChar::LineSeparator) continue;
            const bool caretHere =
                block.blockNumber() == caretBlock && line == caretLine;
            if (!caretHere && i > 0 &&
                (text.at(i - 1) == QLatin1Char(' ') || text.at(i - 1) == QLatin1Char('\t'))) {
                return QStringLiteral("блок %1, строка %2: хвостовые пробелы")
                    .arg(block.blockNumber())
                    .arg(line);
            }
            ++line;
        }
    }
    return {};
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


// --- ГЛАГОЛЫ ЗАМЕТКИ --------------------------------------------------------
//
// Всё, что выше, — РАЗМЕТКА БЛОКОВ: местные функции этого файла, наружу не
// выходящие. Наружу выходит только то, что ниже, — глаголы самой заметки.
// Второго входа в правку не бывает: у каждой операции ровно один публичный вид,
// и он метод ZDocument.
//
// Скобку отмены, пересборку тронутого и шов держит runLocalEdit (document_edit.cpp);
// здесь остаётся только то, ЧТО делает операция.

namespace {

int bitsOf(ZDocument::Style style) {
    switch (style) {
        case ZDocument::Style::Bold: return SpanBold;
        case ZDocument::Style::Italic: return SpanItalic;
        case ZDocument::Style::Strike: return SpanStrike;
        case ZDocument::Style::Code: return SpanCode;
    }
    return 0;
}


}  // namespace


// --- НАБОР, ENTER, BACKSPACE ------------------------------------------------
//
// Их объединяет одно: они НЕ ходят через markdown. Обычный ввод — решение
// владельца («через разбор идут только структурные операции»), а Enter и
// Backspace структурны по существу: режут и склеивают блоки, а не переписывают
// их текст. Заметке они нужны затем, чтобы шов приводился к канону здесь — на
// месте правки, а не потом обходом всего документа.

bool ZDocument::insertText(QTextCursor& at, const QString& text) {
    if (at.document() != &d_->text) return false;
    return insertText(at, text, at.block().charFormat());
}

bool ZDocument::insertText(QTextCursor& at, const QString& text,
                           const QTextCharFormat& format) {
    if (text.isEmpty() && !at.hasSelection()) return false;
    // ПОМЕТКА РАЗДЕЛИТЕЛЯ СТРОК НАБРАННОЙ БУКВЕ НЕ ПРИНАДЛЕЖИТ. Формат для
    // следующей буквы вид берёт у знака слева от каретки, а слева бывает мягкий
    // перенос — и набранное за ним наследовало его пометку, то есть само
    // становилось «переносом» для писателя. Поймала сверка со сборкой.
    QTextCharFormat clean = format;
    clean.clearProperty(BreakSourceProperty);
    // И ОБЪЕКТОМ НАБРАННАЯ БУКВА НЕ СТАНОВИТСЯ. Формат для следующей буквы вид
    // берёт у знака слева, а слева бывает объект — фотография. Буква
    // наследовала его род и его исходник, и обход документа выписывал этот
    // исходник ВТОРОЙ раз: в файл уехало бы две картинки вместо одной.
    if (clean.objectType() != QTextFormat::NoObject) {
        clean = at.block().charFormat();
        clean.clearProperty(BreakSourceProperty);
    }

    // БЛОК, КОТОРЫЙ ЗАВЁЛ САМ Qt, свойств не имеет вовсе — так выглядит
    // единственный блок опустевшей заметки. Набранное в него делает заметку
    // непустой, и оформить блок должен сборщик: экономить пересборку здесь
    // нельзя, зато и случается это ровно один раз на заметку.
    return runLocalEdit(at, [this, &text, &clean](QTextCursor& edit) {
        if (edit.hasSelection()) edit.removeSelectedText();
        if (!text.isEmpty()) {
            const int from = edit.position();
            edit.insertText(text, clean);
            // И ФОРМАТ НАБРАННОГО СТАВИМ ЯВНО. Qt вливает вставленное в соседний
            // кусок, когда формат «достаточно похож», и набранное за мягким
            // переносом наследовало его пометку — то есть само становилось
            // переносом для писателя. Поймала сверка со сборкой.
            QTextCursor typed(&d_->text);
            typed.setPosition(from);
            typed.setPosition(edit.position(), QTextCursor::KeepAnchor);
            typed.setCharFormat(clean);
        }

        // НАБОР ЛОМАЕТ ИНВАРИАНТЫ ДВУМЯ СПОСОБАМИ: текстом на пустой строке (она
        // перестаёт быть пустой) и выделением, съевшим границу блоков (рядом
        // оказываются соседи, которых markdown раздельно не выражает). Чиним
        // здесь же, в той же скобке правки, — а не потом и не по всему документу.
        QTextCursor repair(edit);
        if (repairAfterTyping(d_->text, repair)) edit = repair;
        return true;
    });
}

bool ZDocument::breakBlock(QTextCursor& at, BreakKind kind) {
    // Shift+Enter В КОНЦЕ ЛИТЕРАЛЬНОГО БЛОКА (код, дословный кусок) — продолжить
    // текст ПОД ним, на том же уровне пункта (решение владельца, сессия 5): в
    // пункт кладут код и продолжают пункт словами. Внутри блока Shift+Enter —
    // как и был, строка внутри блока.
    if (kind == BreakKind::Otherwise && at.document() == &d_->text && !at.hasSelection()) {
        const QTextBlock block = at.block();
        if (isLiteralBlock(block) && at.positionInBlock() == block.length() - 1)
            return insertAfterObject(at, block.blockNumber(), true);
    }
    return runLocalEdit(at, [this, kind](QTextCursor& edit) {
        switch (kind) {
            case BreakKind::Plain:
                // Порядок важен и взят у прежнего обработчика: сперва объект
                // (фотография — атом), потом правило черты, и только потом
                // обычный разрез блока.
                return newLineAfterImage(d_->text, edit) ||
                       applyDividerRuleAtCursor(d_->text, edit) ||
                       splitBlockAtCursor(d_->text, edit);
            case BreakKind::Otherwise:
                return splitBlockOtherwiseAtCursor(d_->text, edit);
            case BreakKind::LeaveCode:
                return leaveCodeBlockAtCursor(d_->text, edit);
        }
        return false;
    });
}

bool ZDocument::deleteBack(QTextCursor& at) {
    return runLocalEdit(at, [this](QTextCursor& edit) {
        // Жесты, у которых своё правило: снять комментарность, снять пункт,
        // убрать черту над кареткой, склеить через пустую строку, убрать
        // фотографию целиком.
        const bool special = uncommentAtBlockStart(d_->text, edit) ||
                             unwrapListItemAtCursor(d_->text, edit) ||
                             deleteDividerAbove(d_->text, edit) ||
                             deleteImageLineBackward(d_->text, edit) ||
                             joinAcrossVSpaceBackward(d_->text, edit);
        if (!special) {
            // НА САМОЙ ЧЕРТЕ УДАЛЯТЬ СЛЕВА НЕЧЕГО. По плоской модели слева от
            // каретки стоит перевод строки, но черта не живёт в строке текста, и
            // обычное удаление съело бы не то. Отказываемся — что делать дальше,
            // решает вызывающий (он уводит каретку в конец строки выше).
            const QTextBlock here = edit.block();
            if (!edit.hasSelection() && !isRawBlock(here) && kindOf(here) == Kind::Divider)
                return false;
            // Обычное удаление знака. Делаем сами, а не отдаём Qt: тогда починка
            // шва попадает в тот же шаг отмены, что и само удаление.
            if (edit.hasSelection()) edit.removeSelectedText();
            else if (edit.position() > 0) edit.deletePreviousChar();
            else return false;
        }
        QTextCursor repair(edit);
        if (repairAfterTyping(d_->text, repair)) edit = repair;
        return true;
    });
}

bool ZDocument::deleteForward(QTextCursor& at) {
    return runLocalEdit(at, [this](QTextCursor& edit) {
        const bool special = deleteImageLineForward(d_->text, edit) ||
                             joinAcrossVSpaceForward(d_->text, edit);
        if (!special) {
            if (edit.hasSelection()) edit.removeSelectedText();
            else if (edit.position() < d_->text.characterCount() - 1) edit.deleteChar();
            else return false;
        }
        QTextCursor repair(edit);
        if (repairAfterTyping(d_->text, repair)) edit = repair;
        return true;
    });
}

// --- НАЧЕРТАНИЕ -------------------------------------------------------------

bool ZDocument::toggleStyle(QTextCursor& at, Style style) {
    return runLocalEdit(at, [this, style](QTextCursor& edit) {
        switch (style) {
            case Style::Bold: return toggleBold(d_->text, edit);
            case Style::Italic: return toggleItalic(d_->text, edit);
            case Style::Strike: return toggleStrike(d_->text, edit);
            case Style::Code: return toggleCode(d_->text, edit);
        }
        return false;
    });
}

QTextCharFormat ZDocument::styleForTyping(const QTextCursor& at,
                                          const QTextCharFormat& current, Style style) const {
    if (at.document() != &d_->text) return current;
    // В блоке кода и в дословном куске текст буквальный — начертанию там взяться
    // неоткуда, ровно как и при выделении. Спрашивают об этом заметку, а не
    // смотрят на блок снаружи: снаружи про блоки кода знать не должны.
    const QTextBlock block = at.block();
    if (isRawBlock(block) || kindOf(block) == Kind::Code) return current;
    return inlineStyleForTyping(block, current, bitsOf(style));
}

// --- РОД БЛОКА --------------------------------------------------------------

bool ZDocument::setHeadingLevel(QTextCursor& at, int level) {
    return runLocalEdit(at, [this, level](QTextCursor& edit) {
        return zametti::setHeadingLevel(d_->text, edit, level);
    });
}

bool ZDocument::makeBullet(QTextCursor& at) {
    return runLocalEdit(at, [this](QTextCursor& edit) {
        return zametti::makeBullet(d_->text, edit);
    });
}

bool ZDocument::makeOrdered(QTextCursor& at) {
    return runLocalEdit(at, [this](QTextCursor& edit) {
        return zametti::makeOrdered(d_->text, edit);
    });
}

bool ZDocument::makeTask(QTextCursor& at) {
    return runLocalEdit(at, [this](QTextCursor& edit) {
        return zametti::makeTask(d_->text, edit);
    });
}

bool ZDocument::makeParagraph(QTextCursor& at) {
    return runLocalEdit(at, [this](QTextCursor& edit) {
        return zametti::makeParagraph(d_->text, edit);
    });
}

bool ZDocument::toggleComment(QTextCursor& at) {
    return runLocalEdit(at, [this](QTextCursor& edit) {
        return toggleCommentAtCursor(d_->text, edit);
    });
}

bool ZDocument::toggleTask(QTextCursor& at) {
    return runLocalEdit(at, [this](QTextCursor& edit) {
        return toggleTaskAtCursor(d_->text, edit);
    });
}

// --- АВТОЗАМЕНЫ ПРИ НАБОРЕ --------------------------------------------------

bool ZDocument::applyInputRule(QTextCursor& at) {
    return runLocalEdit(at, [this](QTextCursor& edit) {
        return applyInputRuleAtCursor(d_->text, edit);
    });
}

bool ZDocument::applyCodeSpanRule(QTextCursor& at) {
    return runLocalEdit(at, [this](QTextCursor& edit) {
        return applyCodeSpanRuleAtCursor(d_->text, edit);
    });
}

bool ZDocument::applyDividerRule(QTextCursor& at) {
    return runLocalEdit(at, [this](QTextCursor& edit) {
        return applyDividerRuleAtCursor(d_->text, edit);
    });
}

// --- ФОТОГРАФИЯ -------------------------------------------------------------

bool ZDocument::setImageWidth(QTextCursor& at, int width) {
    return runLocalEdit(at, [this, width](QTextCursor& edit) {
        return setImageWidthAtCursor(d_->text, edit, width);
    });
}

bool ZDocument::setImageAlign(QTextCursor& at, ImageAlign align) {
    return runLocalEdit(at, [this, align](QTextCursor& edit) {
        return setImageAlignAtCursor(d_->text, edit, align);
    });
}

bool ZDocument::setImageCaption(QTextCursor& at, const QString& caption) {
    return runLocalEdit(at, [caption](QTextCursor& edit) {
        return setImageCaptionAtCursor(edit, caption);
    });
}

bool ZDocument::toggleImageCaption(QTextCursor& at, QChar mark) {
    const BlockImageRef ref = blockImageRef(at.block());
    if (!ref.valid || ref.wiki) return false;
    // Прятать нечего: пустая подпись и так не показывается, а знак перед
    // пустотой был бы мусором в файле.
    if (ref.alt.trimmed().isEmpty()) return false;
    if (mark != QLatin1Char('~') && mark != QLatin1Char('-')) return false;
    return setImageCaption(at, captionWithHidingToggled(ref.alt, mark));
}

bool ZDocument::cutImageLine(QTextCursor& at) {
    return runLocalEdit(at, [this](QTextCursor& edit) {
        return cutImageLineAtCursor(d_->text, edit);
    });
}

bool ZDocument::deleteImageAbove(QTextCursor& at) {
    return runLocalEdit(at, [this](QTextCursor& edit) {
        return deleteImageLineBackward(d_->text, edit);
    });
}

bool ZDocument::deleteImageBelow(QTextCursor& at) {
    return runLocalEdit(at, [this](QTextCursor& edit) {
        return deleteImageLineForward(d_->text, edit);
    });
}

// --- БЛОК КОДА --------------------------------------------------------------

bool ZDocument::setCodeLanguage(QTextCursor& at, const QString& language) {
    return runLocalEdit(at, [this, language](QTextCursor& edit) {
        return zametti::setCodeLanguage(d_->text, edit, language);
    });
}

QStringList ZDocument::codeLanguagesNear(int blockNumber) const {
    return zametti::codeLanguagesNear(d_->text, blockNumber);
}

// --- СЛОЙ ОБЪЕКТОВ ----------------------------------------------------------

// Маркер пункта, содержимым которого является этот блок: ближайший пункт выше
// на том же уровне. Не нашёлся — буллет.
static Marker markerOfItemAbove(const QTextDocument& doc, int number, int level) {
    for (int i = number - 1; i >= 0; --i) {
        const QTextBlock block = doc.findBlockByNumber(i);
        if (isVSpaceBlock(block)) continue;
        const int at = levelOf(block);
        if (at < level) break;
        if (at == level && isListBlock(block)) return markerOf(block).marker;
    }
    return Marker::Bullet;
}

// ПОСЛЕ ОБЪЕКТА — Ctrl+Enter и Shift+Enter (решение владельца, сессия 5).
//
// Оба заводят пустую строку под объектом (в файле она обязана быть: абзац или
// пункт вплотную к таблице стал бы её рядом, к фотографии — её продолжением),
// а дальше — что именно:
//   Ctrl+Enter  — новый ПУНКТ на уровне объекта, если объект стоит внутри
//                 пункта (маркер — как у пункта над ним); вне списка — только
//                 пустая строка, набор превратит её в абзац (как всегда);
//   Shift+Enter — ПРОДОЛЖЕНИЕ ТОГО ЖЕ ПУНКТА: абзац на уровне объекта, без
//                 маркера. Так в пункт вставляют формулу, таблицу, картинку и
//                 продолжают пункт текстом, не уходя во внешний редактор.
// Оба идут через сборщик (replaceBlocks): новые блоки — ровно такие, какими
// собрал бы их разбор файла.
namespace {

enum class AfterObject { NewItem, ContinueItem };

// Что встанет после объекта: сам объект (пересобранный сборщиком), пустая
// строка и — новый пункт или абзац-продолжение. Возвращает блоки и номер того
// из них (от блока объекта), в который встанет каретка.
std::vector<Piece> piecesAfterObject(const QTextDocument& doc, int blockIndex, AfterObject what,
                                     int* landing) {
    const QTextBlock block = doc.findBlockByNumber(blockIndex);
    if (!block.isValid()) return {};
    const int level = levelOf(block);
    std::vector<Piece> pieces = piecesOfBlocks(doc, blockIndex, blockIndex);
    if (pieces.size() != 1) return {};
    Piece gap;
    gap.kind = Kind::VSpace;
    pieces.push_back(gap);
    *landing = 1;
    if (what == AfterObject::NewItem && level >= 0) {
        Piece item;
        item.kind = Kind::ListItem;
        item.level = level;
        item.marker = markerOfItemAbove(doc, blockIndex, level);
        pieces.push_back(item);
        *landing = 2;
    } else if (what == AfterObject::ContinueItem) {
        // Пустой абзац — переходное состояние: набор наполнит его, а уход
        // каретки (tidyLeftLine) обратит в пустую строку. В файл он и так уходит
        // пустой строкой.
        Piece paragraph;
        paragraph.kind = Kind::Paragraph;
        paragraph.level = level;
        pieces.push_back(paragraph);
        *landing = 2;
    }
    return pieces;
}

}  // namespace

bool ZDocument::insertLineAfter(QTextCursor& at, int blockIndex) {
    return insertAfterObject(at, blockIndex, false);
}

bool ZDocument::continueItemAfter(QTextCursor& at, int blockIndex) {
    return insertAfterObject(at, blockIndex, true);
}

bool ZDocument::insertAfterObject(QTextCursor& at, int blockIndex, bool continueItem) {
    if (at.document() != &d_->text) return false;
    int landing = 0;
    const std::vector<Piece> pieces = piecesAfterObject(
        d_->text, blockIndex, continueItem ? AfterObject::ContinueItem : AfterObject::NewItem,
        &landing);
    if (pieces.empty()) return false;
    QTextCursor edit(at);
    edit.beginEditBlock();
    replaceBlocks(blockIndex, blockIndex, pieces);
    settleSeam(blockIndex, blockIndex + int(pieces.size()) - 1);
    edit.endEditBlock();
#ifndef NDEBUG
    checkCanonical();
#endif
    at = caretAtBlock(blockIndex + landing);
    return true;
}

bool ZDocument::dropEmptyBlockAfterObject(QTextCursor& at, int number) {
    return runLocalEdit(at, [this, number](QTextCursor& edit) {
        const QTextBlock block = d_->text.findBlockByNumber(number);
        if (!block.isValid() || !block.text().isEmpty()) return false;
        // Пустая строка над пустым блоком заведена вместе с ним (Ctrl+Enter):
        // если за блоком ничего нет или снова пустая строка, она больше не
        // разделяет объект с текстом — убираем и её, возвращая документ к виду
        // до Ctrl+Enter. Между объектом и текстом ниже пустая строка обязана
        // остаться — там её не трогаем.
        const QTextBlock above = block.previous();
        const QTextBlock below = block.next();
        const bool dropGap = above.isValid() && isVSpaceBlock(above) &&
                             objectOf(above.previous()).valid() &&
                             (!below.isValid() || isVSpaceBlock(below));
        const QTextBlock from = dropGap ? above : block;
        // Вместе с разделителем блока — иначе на месте блока остаётся пустая
        // строка, которой не заказывали. У последнего блока документа
        // разделителя после нет: убираем разделитель ПЕРЕД ним — тогда хвост
        // сливается в предыдущий блок, и остаётся его формат, а не пустой
        // блок в конце.
        int start = from.position();
        int end = block.position() + block.length();
        if (end > d_->text.characterCount() - 1) {
            end = d_->text.characterCount() - 1;
            start = qMax(0, start - 1);
        }
        edit.setPosition(start);
        edit.setPosition(end, QTextCursor::KeepAnchor);
        edit.removeSelectedText();
        return true;
    });
}

bool ZDocument::removeBlocks(QTextCursor& at, int first, int last) {
    return runLocalEdit(at, [this, first, last](QTextCursor& edit) {
        const QTextBlock from = d_->text.findBlockByNumber(first);
        const QTextBlock to = d_->text.findBlockByNumber(last);
        if (!from.isValid() || !to.isValid()) return false;
        edit.setPosition(from.position());
        // Вместе с разделителем блока: иначе от объекта остаётся пустая строка,
        // которой в файле не было.
        const int end = qMin(to.position() + to.length(), d_->text.characterCount() - 1);
        edit.setPosition(end, QTextCursor::KeepAnchor);
        edit.removeSelectedText();
        return true;
    });
}

// --- ЗАМЕНА ПО ВСЕЙ ЗАМЕТКЕ -------------------------------------------------

int ZDocument::replaceAll(const QString& text, bool caseSensitive, const QString& with) {
    if (text.isEmpty()) return 0;
    QTextDocument::FindFlags flags;
    if (caseSensitive) flags |= QTextDocument::FindCaseSensitively;

    int replaced = 0;
    QTextCursor group(&d_->text);
    group.beginEditBlock();
    QTextCursor at(&d_->text);
    while (true) {
        at = d_->text.find(text, at, flags);
        if (at.isNull()) break;
        at.insertText(with);
        ++replaced;
    }
    // ОБЪЕКТЫ — ПО ИСХОДНИКУ (таблица, формула): в тексте блока их слов нет,
    // QTextDocument::find их не видит. Идём с конца: судья может заменить один
    // блок несколькими, и номера ниже съезжают, а выше — нет.
    const Qt::CaseSensitivity sensitivity = caseSensitive ? Qt::CaseSensitive : Qt::CaseInsensitive;
    for (int number = d_->text.blockCount() - 1; number >= 0; --number) {
        const QTextBlock block = d_->text.findBlockByNumber(number);
        bool inObject = false;
        QString source = searchableTextOf(block, &inObject);
        if (!inObject) continue;
        int here = 0;
        qsizetype pos = source.indexOf(text, 0, sensitivity);
        while (pos >= 0) {
            source.replace(pos, text.size(), with);
            ++here;
            pos = source.indexOf(text, pos + with.size(), sensitivity);
        }
        if (here == 0) continue;
        QTextCursor scratch(&d_->text);
        scratch.setPosition(block.position());
        if (rejudgeBlock(scratch, number, source)) replaced += here;
    }
    if (replaced > 0) {
        // ШОВ ПО ВСЕЙ ЗАМЕТКЕ — здесь это законно: вхождения рассыпаны по ней
        // целиком, и дешевле шва на каждое одно на всё.
        settleSeam(0, d_->text.blockCount() - 1);
        rebuildRange(0, d_->text.blockCount() - 1);
    }
    group.endEditBlock();

#ifndef NDEBUG
    if (replaced > 0) checkCanonical();
#endif
    return replaced;
}

// --- УБОРКА -----------------------------------------------------------------

bool ZDocument::repairAfterEdit(QTextCursor& at) {
    if (at.document() != &d_->text) return false;
    return repairAfterTyping(d_->text, at);
}

bool ZDocument::tidyRange(int firstBlock, int lastBlock, const QTextCursor& caret) {
    const int first = qMax(0, firstBlock);
    const int afterLast = qMin(d_->text.blockCount() - 1, lastBlock);

    const int caretBlock = caret.blockNumber();
    int caretLine = 0;
    {
        const QString text = caret.block().text();
        for (int i = 0; i < caret.positionInBlock() && i < text.size(); ++i)
            if (text.at(i) == QChar::LineSeparator) ++caretLine;
    }

    // Сначала собрать, потом резать с конца: позиции не плывут.
    std::vector<std::pair<int, int>> cuts;
    QTextBlock block = d_->text.findBlockByNumber(first);
    for (int number = first; number <= afterLast && block.isValid();
         ++number, block = block.next()) {
        if (isRawBlock(block)) continue;
        if (kindOf(block) == Kind::Code) continue;
        const QString text = block.text();
        int line = 0;
        int lineStart = 0;
        for (int i = 0; i <= text.size(); ++i) {
            if (i != text.size() && text.at(i) != QChar::LineSeparator) continue;
            if (!(block.blockNumber() == caretBlock && line == caretLine)) {
                int cut = i;
                while (cut > lineStart && (text.at(cut - 1) == QLatin1Char(' ') ||
                                           text.at(cut - 1) == QLatin1Char('\t')))
                    --cut;
                if (cut < i) cuts.push_back({block.position() + cut, block.position() + i});
            }
            lineStart = i + 1;
            ++line;
        }
    }
    if (cuts.empty()) return false;

    QTextCursor edit(&d_->text);
    edit.beginEditBlock();
    for (auto it = cuts.rbegin(); it != cuts.rend(); ++it) {
        edit.setPosition(it->first);
        edit.setPosition(it->second, QTextCursor::KeepAnchor);
        edit.removeSelectedText();
    }
    edit.endEditBlock();
    return true;
}

bool ZDocument::tidyLine(const QTextCursor& left) {
    if (left.document() != &d_->text) return false;
    const QTextBlock block = left.block();
    if (!block.isValid() || isRawBlock(block)) return false;
    const Kind kind = kindOf(block);
    // Пустую строку и черту чистим тоже: на них могли пожить пробелы, пока
    // каретка там стояла. Не трогаем только код: там хвостовые пробелы —
    // содержимое.
    if (kind == Kind::Code) return false;

    const QString text = block.text();
    // Границы строки, на которой стояла каретка.
    int from = 0;
    for (int i = left.positionInBlock() - 1; i >= 0; --i)
        if (text.at(i) == QChar::LineSeparator) {
            from = i + 1;
            break;
        }
    int to = text.size();
    for (int i = left.positionInBlock(); i < text.size(); ++i)
        if (text.at(i) == QChar::LineSeparator) {
            to = i;
            break;
        }
    int cut = to;
    while (cut > from && (text.at(cut - 1) == QLatin1Char(' ') ||
                          text.at(cut - 1) == QLatin1Char('\t')))
        --cut;
    const bool emptied = cut == from;
    // Пустая ХВОСТОВАЯ строка многострочного блока — мусор от удаления: при
    // сохранении она затвердела бы в неразрывный пробел. Отрезаем её в
    // настоящую пустую строку. Серединные пустые не трогаем: ими человек
    // намеренно отбивает куски внутри блока.
    const bool tailOfBlock = to == text.size();
    if (cut == to && !(emptied && tailOfBlock && from > 0)) return false;

    QTextCursor edit(&d_->text);
    edit.beginEditBlock();
    if (cut < to) {
        edit.setPosition(block.position() + cut);
        edit.setPosition(block.position() + to, QTextCursor::KeepAnchor);
        edit.removeSelectedText();
    }
    const QTextBlock after = edit.block();
    if (emptied && tailOfBlock && from > 0) {
        // Снять перенос перед опустевшей строкой и завести настоящую пустую
        // строку после блока.
        edit.setPosition(block.position() + from - 1);
        edit.setPosition(block.position() + from, QTextCursor::KeepAnchor);
        edit.removeSelectedText();
        edit.movePosition(QTextCursor::EndOfBlock);
        edit.insertBlock(vspaceBlockFormat(false, false, styleOf(*edit.document())));
        settleSeam(block.blockNumber(), block.blockNumber() + 1);
    } else if (after.text().isEmpty() && kind == Kind::Paragraph) {
        // Строка (и весь блок) опустела: это настоящая пустая строка.
        edit.setBlockFormat(vspaceBlockFormat(
            after.previous().isValid() && isVSpaceBlock(after.previous()),
            after.blockNumber() == 0, styleOf(*edit.document())));
        settleSeam(after.blockNumber(), after.blockNumber());
    }
    edit.endEditBlock();
    return true;
}

// --- СТОРОЖ СТРОЕНИЯ --------------------------------------------------------

QString ZDocument::structureProblem() const {
    QString problem;
    if (!listInvariantHolds(d_->text, &problem)) return QStringLiteral("списки: ") + problem;
    if (!gapInvariantHolds(d_->text, &problem))
        return QStringLiteral("пустые строки: ") + problem;
    return {};
}

// --- ФОРМУЛА ИЗ-ПОД КЛАВИАТУРЫ ----------------------------------------------

bool ZDocument::toggleInlineMath(QTextCursor& at) {
    return runLocalEdit(at, [this](QTextCursor& edit) {
        return zametti::toggleInlineMath(d_->text, edit);
    });
}

bool ZDocument::toggleDisplayMath(QTextCursor& at) {
    return runLocalEdit(at, [this](QTextCursor& edit) {
        return zametti::toggleDisplayMath(d_->text, edit);
    });
}

// --- ПРАВКА ФОРМУЛЫ ---------------------------------------------------------

bool ZDocument::openFormula(QTextCursor& at) {
    if (at.document() != &d_->text) return false;
    const QTextBlock block = at.block();
    const BlockFormulaRef ref = blockFormulaRef(block);
    if (!ref.valid || !ref.display) return false;
    // Уже раскрыта: в блоке текст, а не объект — править и так можно.
    if (block.text().size() != 1 ||
        block.text().at(0) != QChar::ObjectReplacementCharacter)
        return false;

    const int number = block.blockNumber();
    // РАСКРЫТАЯ ФОРМУЛА — АБЗАЦ С КУСКОМ-МАТЕМАТИКОЙ, а не голый текст. Так её
    // узнаёт blockFormulaRef (и потому её можно свернуть обратно), так же её
    // прочтёт и файл: `$$…$$` отдельной строкой — это выключная формула.
    Piece piece;
    piece.text = ref.source;
    // Уровень пункта переживает раскрытие: формула внутри пункта раскрывается
    // абзацем внутри того же пункта, а не выпадает из списка.
    piece.level = levelOf(block);
    Run run;
    run.start = 0;
    run.end = int32_t(piece.text.size());
    run.flags = InlineMath;
    piece.runs.push_back(run);
    QTextCursor edit(at);
    edit.beginEditBlock();
    replaceBlocks(number, number, {piece});
    settleSeam(number, number);
    edit.endEditBlock();

#ifndef NDEBUG
    checkCanonical();
#endif
    // Каретка — в НАЧАЛО исходника (решение владельца: Enter на объекте ставит
    // каретку в начало; у таблицы так же).
    at = caretAtBlock(number);
    return true;
}

bool ZDocument::closeFormula(QTextCursor& at) {
    if (at.document() != &d_->text) return false;
    const QTextBlock block = at.block();
    if (isRawBlock(block) || kindOf(block) != Kind::Paragraph) return false;

    // СПРАШИВАЕМ ТЕКСТ, А НЕ РАЗМЕТКУ. Пока формула раскрыта, человек в ней
    // ПЕЧАТАЕТ, и набранные знаки пометки математики не несут: спрашивать
    // blockFormulaRef здесь значило бы «свернуть можно только то, чего не
    // трогали» — Esc после правки не делал ничего (нашёл владелец).
    //
    // Канон общий, из ядра: формула это то, что scanMath считает формулой.
    const int number = block.blockNumber();
    const std::vector<Piece> now = piecesOfBlocks(d_->text, number, number);
    if (now.size() != 1 || now.front().raw) return false;
    const QString& source = now.front().text;
    const std::vector<MathSpan> found = scanMath(source);
    // Правкой формулу разорвали — она осталась обычным текстом, и это законно:
    // в файл уйдёт то, что написано.
    if (found.size() != 1 || found.front().start != 0 || found.front().end != source.size())
        return false;

    Piece piece;
    piece.kind = Kind::Math;
    piece.text = source;
    piece.level = levelOf(block);
    QTextCursor edit(at);
    edit.beginEditBlock();
    replaceBlocks(number, number, {piece});
    settleSeam(number, number);
    edit.endEditBlock();

#ifndef NDEBUG
    checkCanonical();
#endif
    at = caretAtBlock(number);
    return true;
}

// --- ПРАВКА ТАБЛИЦЫ: ОБЪЕКТ ⇄ ИСХОДНИК ---------------------------------------
//
// Тот же приём, что у формулы: таблица-объект заменяется дословным блоком с
// её исходником (обычный литеральный блок — буквы проходят, Enter даёт строку,
// как в коде), а закрытие спрашивает СУДЬЮ ФАЙЛА: текст блока разбирается
// parsePieces — ровно тем, чем читается файл, — и блок заменяется тем, что
// получилось: одна таблица (объект снова), таблица и абзац после пустой
// строки, или вовсе не таблица. Так «закрыл» == «перечитал файл», и картина
// после Esc та же, что после ухода-возврата.

bool ZDocument::openTable(QTextCursor& at, int sourceOffset) {
    if (at.document() != &d_->text) return false;
    const QTextBlock block = at.block();
    if (!isTableObjectBlock(block)) return false;
    const int number = block.blockNumber();
    const QString source = tableSourceOf(block);

    Piece piece;
    piece.raw = true;
    piece.text = source + QLatin1Char('\n');
    piece.trailingNewline = block.blockFormat().boolProperty(TrailingNewlineProperty);
    if (!piece.trailingNewline) piece.text.chop(1);
    // Уровень пункта переживает раскрытие: таблица внутри пункта раскрывается
    // дословным блоком внутри того же пункта.
    piece.level = levelOf(block);
    QTextCursor edit(at);
    edit.beginEditBlock();
    replaceBlocks(number, number, {piece});
    settleSeam(number, number);
    edit.endEditBlock();

#ifndef NDEBUG
    checkCanonical();
#endif
    // Каретка — в начало исходника (решение владельца) или в указанную ячейку.
    at = caretAtBlock(number);
    const QTextBlock opened = d_->text.findBlockByNumber(number);
    if (sourceOffset > 0 && opened.isValid())
        at.setPosition(opened.position() + qBound(0, sourceOffset, opened.length() - 1));
    return true;
}

// Судья закрытия: блок → то, что прочёл бы файл. Общий для closeTable и для
// переписывания исходника объекта (rewriteObjectSource).
bool ZDocument::rejudgeBlock(QTextCursor& at, int number, const QString& source) {
    const QTextBlock block = d_->text.findBlockByNumber(number);
    if (!block.isValid()) return false;
    const int level = levelOf(block);

    std::vector<Piece> pieces;
    NoteHeader ignored;
    parsePieces(source.endsWith(QLatin1Char('\n')) ? source : source + QLatin1Char('\n'), pieces,
                ignored);
    if (pieces.empty()) {
        // Пусто — пустая строка: пустого блока в документе не бывает.
        Piece gap;
        gap.kind = Kind::VSpace;
        pieces.push_back(gap);
    }
    // Хвостовые пустые строки читатель отбрасывает — как и файл.
    while (pieces.size() > 1 && !pieces.back().raw && pieces.back().kind == Kind::VSpace)
        pieces.pop_back();
    for (Piece& piece : pieces)
        if (piece.level < 0 && !(piece.kind == Kind::VSpace && !piece.raw)) piece.level = level;

    QTextCursor edit(at);
    edit.beginEditBlock();
    replaceBlocks(number, number, pieces);
    settleSeam(number, number + int(pieces.size()) - 1);
    edit.endEditBlock();

#ifndef NDEBUG
    checkCanonical();
#endif
    at = caretAtBlock(number);
    return true;
}

bool ZDocument::closeTable(QTextCursor& at) {
    if (at.document() != &d_->text) return false;
    const QTextBlock block = at.block();
    // Закрывать есть что только у ДОСЛОВНОГО блока, который таблицей читается:
    // иначе это просто текст, и трогать его — значит трогать чужое.
    if (!isRawBlock(block) || isTableObjectBlock(block)) return false;
    const QString source = sourceTextOf(block);
    if (!looksLikeTable(source)) return false;
    return rejudgeBlock(at, block.blockNumber(), source);
}

bool ZDocument::rewriteObjectSource(QTextCursor& at, int blockNumber, const QString& source) {
    if (at.document() != &d_->text) return false;
    return rejudgeBlock(at, blockNumber, source);
}

// --- БЛОК КОДА ИЗ ВЫДЕЛЕНИЯ И ПЕРЕСТАНОВКА ПУНКТОВ --------------------------
//
// Обе правки знают, ЧЕМ блоки должны стать, поэтому идут не через
// runLocalEdit, а прямо через replaceBlocks: пересобирать по их же результату
// незачем — он уже собран.

bool ZDocument::toggleCodeBlock(QTextCursor& at) {
    if (at.document() != &d_->text) return false;
    const CodeBlockEdit edit = zametti::toggleCodeBlock(d_->text, at);
    if (!edit.done) return false;

    QTextCursor group(&d_->text);
    group.beginEditBlock();
    replaceBlocks(edit.firstBlock, edit.lastBlock, edit.blocks);
    settleSeam(edit.firstBlock, edit.firstBlock + docBlocksOf(edit.blocks));
    group.endEditBlock();

#ifndef NDEBUG
    checkCanonical();
#endif
    at = caretAtBlock(edit.landed);
    return true;
}

bool ZDocument::moveListItem(QTextCursor& at, int direction) {
    if (at.document() != &d_->text) return false;
    const MoveEdit edit = zametti::moveListItem(d_->text, at, direction);
    if (!edit.done) return false;

    QTextCursor group(&d_->text);
    group.beginEditBlock();
    replaceBlocks(edit.firstBlock, edit.lastBlock, edit.blocks);
    settleSeam(edit.firstBlock, edit.lastBlock);
    group.endEditBlock();

#ifndef NDEBUG
    checkCanonical();
#endif
    at = caretAtBlock(edit.landed);
    const QTextBlock landed = at.block();
    at.setPosition(at.position() + qBound(0, edit.offsetInBlock, landed.length() - 1));
    return true;
}

// --- ОТСТУП -----------------------------------------------------------------
//
// ЧТО ЗНАЧИТ Tab, РЕШАЕТ МЕСТО, и решает его заметка: снаружи спрашивать «а мы
// сейчас в коде?» некому — там про блоки кода знать не должны вовсе.

// Двинуть ВЕСЬ блок кода: внутрь пункта выше или обратно наружу.
//
// ЖЕСТ НАЗВАН ВЛАДЕЛЬЦЕМ и взят у внешнего редактора, где он и есть
// единственный разумный: ВЫДЕЛИТЬ ВСЕ СТРОКИ БЛОКА И НАЖАТЬ Tab. Там выделение
// захватывает и заборы, и весь кусок уезжает вправо, становясь содержимым
// пункта; у нас заборов в документе нет, поэтому «весь блок» — это все его
// строки, от первой до последней.
//
// Второй вход — каретка в самом начале блока без выделения: то же место, где и
// у всякого другого блока Tab значит «сделать блок глубже».
//
// Отступ текста при этом не теряется. Двинуть блок можно ровно один раз: внутри
// пункта он уже стоит, глубже пункта над ним не бывает — и следующий же Tab по
// тому же выделению снова отступает код, как и раньше. Некуда двигать (списка
// над блоком нет) — сразу отступает код.
static bool moveCodeBlock(QTextDocument& doc, QTextCursor& cursor, int direction) {
    const QVector<CodeLine> lines = touchedCodeLines(doc, cursor);
    if (lines.isEmpty()) return false;
    const QTextBlock head = doc.findBlock(lines.front().start);

    const int first = head.blockNumber();
    const int last = first;   // блок кода — один QTextBlock
    if (cursor.hasSelection()) {
        // Блок целиком, а не кусок: выделение обязано задеть все его строки.
        if (lines.front().start != head.position() ||
            lines.back().end != head.position() + head.length() - 1)
            return false;
    } else if (cursor.positionInBlock() != 0) {
        return false;
    }

    const BlockRange range{first, last};
    if (direction > 0) {
        // Глубже — только под уже существующий пункт: прыжка через уровень в
        // файле не бывает. То же правило, что у абзаца (indentListItems).
        if (levelOf(head) >= 0) return false;
        const int level = levelAbove(doc, first);
        if (level < 0) return false;
        return setInsideLevel(doc, range, level);
    }
    if (levelOf(head) < 0) return false;
    // Есть что снять с самих строк — снимаем сперва их: Shift+Tab по коду с
    // отступом должен убирать отступ, а не выкидывать блок из пункта.
    const int width = codeTabWidth();
    for (const CodeLine& line : lines)
        if (leadingIndent(codeLineText(doc, line), width).columns > 0) return false;
    return setInsideLevel(doc, range, -1);
}

// ВЫДЕЛЕНИЕ ПОД СПИСКОМ + Tab — НОВЫЙ ПОСЛЕДНИЙ ПУНКТ (просьба владельца,
// сессия 5: «после списка идёт абзац или набор абзацев — текст, код, объекты,
// списки, лишь бы без заголовков; выделяю всё, жму Tab — и набор добавляется к
// списку как последний пункт»). Первый абзац выделения становится пунктом
// (маркер — как у пункта над ним, уровень — его), остальное — содержимым
// этого пункта: абзацы, код, объекты на уровне пункта, вложенные в выделение
// списки — уровнем глубже. Заголовок и черта внутри пункта не живут — тогда
// отказ, ничего не трогаем.
bool ZDocument::attachRunAsLastItem(QTextCursor& at) {
    if (at.document() != &d_->text || !at.hasSelection()) return false;
    const BlockRange range = selectedBlocks(d_->text, at);
    if (range.last <= range.first) return false;
    const QTextBlock first = d_->text.findBlockByNumber(range.first);
    if (!first.isValid() || isRawBlock(first) || kindOf(first) != Kind::Paragraph ||
        levelOf(first) >= 0)
        return false;
    const int level = levelAbove(d_->text, range.first);
    if (level < 0) return false;

    std::vector<Piece> pieces = piecesOfBlocks(d_->text, range.first, range.last);
    if (pieces.size() < 2) return false;
    for (const Piece& piece : pieces)
        if (!piece.raw && (piece.kind == Kind::Heading || piece.kind == Kind::Divider))
            return false;
    pieces.front().kind = Kind::ListItem;
    pieces.front().marker = markerOfItemAbove(d_->text, range.first, level);
    pieces.front().checked = false;
    pieces.front().level = level;
    for (size_t i = 1; i < pieces.size(); ++i) {
        Piece& piece = pieces[i];
        if (!piece.raw && piece.kind == Kind::VSpace) continue;
        // Всё, что уже стояло в каком-то списке внутри выделения, уезжает
        // глубже пункта; остальное становится его содержимым.
        piece.level = piece.level >= 0 ? piece.level + level + 1 : level;
    }

    QTextCursor edit(at);
    edit.beginEditBlock();
    replaceBlocks(range.first, range.last, pieces);
    settleSeam(range.first, range.first + int(pieces.size()) - 1);
    edit.endEditBlock();
#ifndef NDEBUG
    checkCanonical();
#endif
    at = caretAtBlock(range.first);
    return true;
}

bool ZDocument::indent(QTextCursor& at) {
    if (attachRunAsLastItem(at)) return true;
    return runLocalEdit(at, [this](QTextCursor& edit) {
        return moveCodeBlock(d_->text, edit, 1) || indentCodeAtCursor(d_->text, edit) ||
               indentListItems(d_->text, edit);
    });
}

bool ZDocument::outdent(QTextCursor& at) {
    return runLocalEdit(at, [this](QTextCursor& edit) {
        return moveCodeBlock(d_->text, edit, -1) || outdentCodeAtCursor(d_->text, edit) ||
               outdentListItems(d_->text, edit);
    });
}


}  // namespace zametti
