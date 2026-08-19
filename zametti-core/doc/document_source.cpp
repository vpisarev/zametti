// ПРАВКА ЗАМЕТКИ ЕЁ ЖЕ ИСХОДНИКОМ: наложить правленый markdown, перевести
// каретку туда и обратно, отменить.
//
// ЗАЧЕМ ОТДЕЛЬНЫЙ ФАЙЛ. Это политика поверх базиса (replaceBlocks + settleSeam),
// и предмет у неё свой: человек ушёл править текст в другом виде, вернулся — и
// заметка обязана принять его работу, НЕ ПЕРЕСОБИРАЯСЬ ЦЕЛИКОМ. Полная
// пересборка стоила бы одного шага отмены размером во всю заметку, выбила бы
// каретки, раскрытые объекты и найденное, а на большой заметке — ещё и
// заметного времени.
//
// ЦЕНА НАЗВАНА ВСЛУХ. Наложение — O(N) от размера заметки, и не однажды: канон
// нынешнего, разбор нового, канон нового, построчное сравнение и сверка на
// выходе. На заметке 500 КБ это десятки миллисекунд. Это законно ровно потому,
// что переключение режима — редкое ЯВНОЕ действие человека, предмет которого и
// есть вся заметка (как у replaceAll и у записи), а не нажатие клавиши и не
// кадр прокрутки. На горячий путь ничего из этого не выходит.

#include "document_impl.h"

#include "diff.h"
#include "doc_model.h"
#include "document_builder.h"
#include "document_pieces.h"
#include "serializer.h"

#include <QTextBlock>
#include <QTextCursor>

#include <algorithm>
#include <vector>

namespace zametti {
namespace {

// Строки канона без хвостовой пустоты. Текст канона кончается переводом
// строки, и split даёт после него пустой элемент — строкой файла он не
// является (так же его снимает diff::textOf).
QStringList linesOf(const QString& canonical) {
    QStringList lines = canonical.split(QLatin1Char('\n'));
    if (!lines.isEmpty() && lines.last().isEmpty()) lines.removeLast();
    return lines;
}

// НЕВИДИМЫЙ ЗНАК НЕ СЧИТАЕТСЯ ПРАВКОЙ.
//
// В заметках владельца встречаются CRLF внутри блоков — например, в исходнике
// выключной формулы, приехавшей копированием откуда-то ещё. В файле они лежат
// дословно (текст свят), а вот в ТЕКСТОВОМ виде их не существует: любой
// плоский виджет превращает CRLF в LF, и человек не видит их и набрать не
// может. Сравнивай мы как есть — один заход в режим и выход из него
// переписывали бы каждый такой блок, молча съедая байты.
//
// Поэтому для СРАВНЕНИЯ возврат каретки снимается с обеих сторон. Блок,
// отличающийся только им, оказывается нетронутым — а нетронутое не
// перекладывается вовсе, и CR в нём остаются жить. Тронутый блок человек
// перенабрал сам, и там их законно не станет.
//
// Число строк от этого не меняется: CR стоит внутри строки (перед LF), а не
// вместо неё, — значит карта «строка → блок» остаётся верной.
QString withoutReturns(const QString& text) {
    QString out = text;
    out.remove(QLatin1Char('\r'));
    return out;
}

// Карта «строка → номер блока». Блок, не давший ни строки, в ней не участвует.
std::vector<int> blockOfLine(const std::vector<BlockLines>& map, int lineCount) {
    std::vector<int> out(size_t(qMax(0, lineCount)), -1);
    for (size_t block = 0; block < map.size(); ++block)
        for (int k = 0; k < map[block].count; ++k) {
            const int line = map[block].first + k;
            if (line >= 0 && size_t(line) < out.size()) out[size_t(line)] = int(block);
        }
    return out;
}

// СОПОСТАВЛЕНИЕ СТРОК БЛОКА С ЕГО СОДЕРЖИМЫМ.
//
// Писатель ставит слева от содержимого то, чего в блоке нет вовсе: маркер
// пункта, решётки заголовка, отступ вложенности, экранирующие косые; а сверху и
// снизу — целые строки, которых в блоке тоже нет (забор ``` у кода). Спрашивать
// об этом писателя вторым списком правил нельзя — он разошёлся бы с первым.
// Поэтому соответствие ВЫВОДИТСЯ из того, что писатель уже напечатал: ищем
// сдвиг lead, при котором КАЖДАЯ строка содержимого оказывается хвостом своей
// строки канона.
//
// Не нашлось — значит писатель добавил знаки не только слева (разметка внутри
// строки: `**жирный**`, ссылка, доллары формулы). Тогда честный ответ — «строку
// знаю, колонку нет», и он так и отдаётся: found == false.
struct LineFit {
    int lead = 0;       // сколько строк канона стоит ДО первой строки содержимого
    bool found = false; // и подошло ли соответствие целиком
};

LineFit fitLines(const QStringList& canonicalLines, const BlockLines& where,
                 const QStringList& content) {
    LineFit fit;
    if (content.isEmpty() || where.count <= 0) return fit;
    const int room = where.count - content.size();
    for (int lead = 0; lead <= room; ++lead) {
        bool all = true;
        for (int k = 0; k < content.size() && all; ++k) {
            const int line = where.first + lead + k;
            if (line < 0 || line >= canonicalLines.size()) { all = false; break; }
            all = canonicalLines.at(line).endsWith(content.at(k));
        }
        if (all) {
            fit.lead = lead;
            fit.found = true;
            return fit;
        }
    }
    return fit;
}

// Строки содержимого блока — такие, какими они лягут в файл (sourceTextOf
// возвращает разделители теми знаками, из которых они пришли).
QStringList contentLinesOf(const QTextBlock& block) {
    QString text = sourceTextOf(block);
    text.replace(QChar::LineSeparator, QLatin1Char('\n'));
    if (text.endsWith(QLatin1Char('\n'))) text.chop(1);
    return text.split(QLatin1Char('\n'));
}

// Куски текста блока между разделителями строк — в координатах ДОКУМЕНТА
// (block.text()), а не файла: по ним ходит каретка.
struct Segment {
    int start = 0;
    int length = 0;
};

std::vector<Segment> segmentsOf(const QTextBlock& block) {
    const QString text = block.text();
    std::vector<Segment> out;
    int start = 0;
    for (int i = 0; i <= text.size(); ++i) {
        if (i == text.size() || text.at(i) == QChar::LineSeparator) {
            out.push_back(Segment{start, i - start});
            start = i + 1;
        }
    }
    return out;
}

}  // namespace

// --- отмена -----------------------------------------------------------------

int ZDocument::undoSteps() const { return d_->text.availableUndoSteps(); }

bool ZDocument::undo() {
    if (d_->text.availableUndoSteps() <= 0) return false;
    d_->text.undo();
    return true;
}

bool ZDocument::redo() {
    if (d_->text.availableRedoSteps() <= 0) return false;
    d_->text.redo();
    return true;
}

void ZDocument::setUndoEnabled(bool on) { d_->text.setUndoRedoEnabled(on); }

bool ZDocument::undoEnabled() const { return d_->text.isUndoRedoEnabled(); }

// --- правка исходника снаружи ----------------------------------------------

bool ZDocument::sourceEditing() const { return d_->sourceEditing; }

void ZDocument::setSourceEditing(bool on) { d_->sourceEditing = on; }

// --- каретка: документ ⇄ исходник -------------------------------------------

SourcePos ZDocument::sourcePosOf(const QTextCursor& at) const {
    SourcePos pos;
    if (at.document() != &d_->text) return pos;

    std::vector<BlockLines> map;
    const QStringList lines = linesOf(canonicalWithMap(&map));

    const QTextBlock block = at.block();
    const size_t number = size_t(qMax(0, block.blockNumber()));
    if (number >= map.size()) return pos;
    const BlockLines where = map[number];
    pos.line = where.first;

    const QStringList content = contentLinesOf(block);
    const LineFit fit = fitLines(lines, where, content);
    pos.exact = fit.found;

    // Какая строка блока под кареткой и сколько знаков левее неё.
    const int offset = at.position() - block.position();
    const std::vector<Segment> segments = segmentsOf(block);
    int k = 0;
    while (size_t(k + 1) < segments.size() &&
           offset > segments[size_t(k)].start + segments[size_t(k)].length)
        ++k;
    pos.line = where.first + fit.lead + k;
    if (pos.line >= where.first + where.count) pos.line = where.first + qMax(0, where.count - 1);

    if (!fit.found || k >= content.size() || pos.line >= lines.size()) {
        pos.column = 0;
        pos.exact = false;
        return pos;
    }
    const QString& canonical = lines.at(pos.line);
    const QString segment = block.text().mid(segments[size_t(k)].start, segments[size_t(k)].length);
    if (!canonical.endsWith(segment)) {
        pos.column = 0;
        pos.exact = false;
        return pos;
    }
    const int inSegment = qBound(0, offset - segments[size_t(k)].start, int(segment.size()));
    pos.column = int(canonical.size() - segment.size()) + inSegment;
    return pos;
}

QTextCursor ZDocument::cursorAtSourcePos(SourcePos pos) {
    std::vector<BlockLines> map;
    const QStringList lines = linesOf(canonicalWithMap(&map));

    // Блок, которому принадлежит строка. Карта отсортирована по first —
    // двоичный поиск, а не проход: у заметки на сотню тысяч строк проход был бы
    // виден.
    int found = -1;
    int low = 0;
    int high = int(map.size()) - 1;
    while (low <= high) {
        const int mid = (low + high) / 2;
        const BlockLines& where = map[size_t(mid)];
        if (pos.line < where.first) {
            high = mid - 1;
        } else if (where.count > 0 && pos.line >= where.first + where.count) {
            low = mid + 1;
        } else {
            found = mid;
            break;
        }
    }
    if (found < 0) {
        // За последней строкой — конец заметки; до первой — начало.
        QTextCursor end(&d_->text);
        end.movePosition(pos.line < 0 ? QTextCursor::Start : QTextCursor::End);
        return end;
    }

    const QTextBlock block = d_->text.findBlockByNumber(found);
    if (!block.isValid()) return QTextCursor(&d_->text);
    QTextCursor caret(block);

    const BlockLines& where = map[size_t(found)];
    const QStringList content = contentLinesOf(block);
    const LineFit fit = fitLines(lines, where, content);
    const int k = pos.line - where.first - fit.lead;
    // Строка забора и прочие строки, которых в блоке нет: каретка — в начало
    // блока. Соврать иначе нечем — знака под этой строкой в документе нет.
    if (!fit.found || k < 0 || k >= content.size()) return caret;

    const std::vector<Segment> segments = segmentsOf(block);
    if (size_t(k) >= segments.size()) return caret;
    const QString& canonical = lines.at(qBound(0, pos.line, int(lines.size()) - 1));
    const QString segment = block.text().mid(segments[size_t(k)].start, segments[size_t(k)].length);
    int inSegment = 0;
    if (canonical.endsWith(segment))
        inSegment = qBound(0, pos.column - int(canonical.size() - segment.size()),
                           int(segment.size()));
    caret.setPosition(block.position() + segments[size_t(k)].start + inSegment);
    return caret;
}

// --- наложение правленого исходника -----------------------------------------

int ZDocument::applySourceText(const QString& text, QTextCursor* caret) {
    // 1. КАНОН НОВОГО. Разбор — тот же, что у файла (normaliseSpaces + полный
    // разбор ядра): вторым, упрощённым, круг разошёлся бы.
    NoteHeader stray;
    std::vector<Piece> fresh;
    parsePieces(normaliseSpaces(text), fresh, stray);
    // ШАПКУ МОЛЧА СЪЕДАТЬ НЕЛЬЗЯ. В исходнике её человеку не показывают (она у
    // заметки, и modified в ней меняется на каждой записи), но набрать он её
    // может — и разбор поднял бы её из тела вместе со следующей пустой строкой.
    // Текст свят: отказываемся целиком, документ не трогаем.
    if (stray.present()) return -1;

    std::vector<BlockLines> mapAfter;
    const QString after = writePieces(fresh, NoteHeader{}, &mapAfter);

    // 2. КАНОН НЫНЕШНЕГО И БЫСТРЫЙ ВЫХОД — ДО первой скобки правки: скобка
    // поднимает ревизию документа, а на ревизии стоит всё производное (счёт
    // слов, найденное). Сравнение строк — memcmp, дешевле любого сравнения.
    std::vector<BlockLines> mapBefore;
    const QString before = canonicalWithMap(&mapBefore);
    if (withoutReturns(before) == withoutReturns(after)) return 0;

    // Весь алгоритм стоит на том, что номер блока в карте — это номер
    // QTextBlock. Так оно и есть с тех пор, как код лёг одним блоком; но
    // держится это соглашением, а не типом, и потому проверяется.
    const int liveBlocks = d_->text.blockCount();
    if (!mapBefore.empty() && int(mapBefore.size()) != liveBlocks) return -2;

    const QStringList linesBefore = linesOf(withoutReturns(before));
    const QStringList linesAfter = linesOf(withoutReturns(after));
    const std::vector<int> ofLineBefore = blockOfLine(mapBefore, int(linesBefore.size()));
    const std::vector<int> ofLineAfter = blockOfLine(mapAfter, int(linesAfter.size()));

    const int oldCount = int(mapBefore.size());
    const int newCount = int(mapAfter.size());

    // 3. ЗАМЕТКА ОПУСТЕЛА ЦЕЛИКОМ. Отдельной веткой: replaceBlocks нечего
    // класть, а buildDocument звать нельзя — он начинается с clear(), а clear()
    // в стек отмены не ложится, и один Ctrl+Z оставил бы от заметки пустой лист.
    if (newCount == 0) {
        QTextCursor edit(&d_->text);
        edit.beginEditBlock();
        edit.movePosition(QTextCursor::Start);
        edit.movePosition(QTextCursor::End, QTextCursor::KeepAnchor);
        edit.removeSelectedText();
        // Пустому документу — формат головы, какой ставит сборщик: без рода и
        // без признака дословности. Иначе это «пустой абзац», а не «пустая
        // заметка», и читатель их различает.
        QTextDocument staging;
        attachStyle(staging, attachedStyle(d_->text));
        buildDocument({}, staging);
        QTextCursor head(&d_->text);
        head.setBlockFormat(staging.firstBlock().blockFormat());
        head.setBlockCharFormat(staging.firstBlock().charFormat());
        edit.endEditBlock();
        if (caret != nullptr) *caret = QTextCursor(&d_->text);
        return 1;
    }

    // 3б. ЗАМЕТКА БЫЛА ПУСТА. Блоков у неё ноль, а QTextBlock один — фантомный,
    // которого обход не видит. Куску не от чего оттолкнуться, и все блоки
    // нового текста кладутся на него разом.
    if (oldCount == 0) {
        QTextCursor edit(&d_->text);
        edit.beginEditBlock();
        replaceBlocks(0, 0, fresh);
        edit.endEditBlock();
        if (withoutReturns(toMarkdownText()) != withoutReturns(after)) {
            d_->text.undo();
            return -2;
        }
        if (caret != nullptr) *caret = caretAtBlock(0);
        return 1;
    }

    // 4. ПОСТРОЧНОЕ СРАВНЕНИЕ — им НАХОДЯТ тронутое; накладывают потом БЛОКАМИ.
    const diff::Result rows = diff::compare(linesBefore, linesAfter);

    // 5. ГРЯЗЕН ВЕСЬ БЛОК, У КОТОРОГО ТРОНУТА ХОТЬ ОДНА СТРОКА. Отсюда само
    // собой выходит, что правка одной строки внутри блока кода перекладывает
    // блок кода целиком: строки забора остались прежними, но блок один.
    std::vector<char> dirtyOld(size_t(qMax(0, oldCount)), 0);
    std::vector<char> dirtyNew(size_t(qMax(0, newCount)), 0);
    for (int i = 0; i < oldCount; ++i)
        if (mapBefore[size_t(i)].count <= 0) dirtyOld[size_t(i)] = 1;
    for (int i = 0; i < newCount; ++i)
        if (mapAfter[size_t(i)].count <= 0) dirtyNew[size_t(i)] = 1;

    const auto blockOfBefore = [&](int line) {
        return line >= 0 && size_t(line) < ofLineBefore.size() ? ofLineBefore[size_t(line)] : -1;
    };
    const auto blockOfAfter = [&](int line) {
        return line >= 0 && size_t(line) < ofLineAfter.size() ? ofLineAfter[size_t(line)] : -1;
    };

    for (const diff::Row& row : rows.rows) {
        if (row.mark == diff::Mark::Same) continue;
        const int ob = blockOfBefore(row.before);
        const int nb = blockOfAfter(row.after);
        if (ob >= 0) dirtyOld[size_t(ob)] = 1;
        if (nb >= 0) dirtyNew[size_t(nb)] = 1;
    }

    // ГРЯЗЬ ПЕРЕТЕКАЕТ ЧЕРЕЗ ЦЕЛЫЕ СТРОКИ. Строка могла не измениться, а блок,
    // в который она попала, — измениться (абзац подрос второй строкой). Тогда
    // грязен и блок с той стороны: класть половину блока нельзя.
    for (bool moved = true; moved;) {
        moved = false;
        for (const diff::Row& row : rows.rows) {
            if (row.mark != diff::Mark::Same) continue;
            const int ob = blockOfBefore(row.before);
            const int nb = blockOfAfter(row.after);
            if (ob < 0 || nb < 0) continue;
            if (dirtyOld[size_t(ob)] && !dirtyNew[size_t(nb)]) {
                dirtyNew[size_t(nb)] = 1;
                moved = true;
            }
            if (dirtyNew[size_t(nb)] && !dirtyOld[size_t(ob)]) {
                dirtyOld[size_t(ob)] = 1;
                moved = true;
            }
        }
    }

    // 6. ЦЕЛЫЕ ПАРЫ «блок до ↔ блок после». Между ними и лежат куски: так
    // вставка (старых блоков нет вовсе) и удаление (нет новых) описываются
    // одинаково, без двух веток.
    struct Pair {
        int oldBlock = 0;
        int newBlock = 0;
    };
    std::vector<Pair> pairs;
    for (const diff::Row& row : rows.rows) {
        if (row.mark != diff::Mark::Same) continue;
        const int ob = blockOfBefore(row.before);
        const int nb = blockOfAfter(row.after);
        if (ob < 0 || nb < 0) continue;
        if (dirtyOld[size_t(ob)] || dirtyNew[size_t(nb)]) continue;
        if (!pairs.empty() && pairs.back().oldBlock == ob) continue;
        pairs.push_back(Pair{ob, nb});
    }

    struct Hunk {
        int oldFirst = 0;
        int oldLast = -1;
        int newFirst = 0;
        int newLast = -1;
        int gap = 0;   // между pairs[gap-1] и pairs[gap]
    };
    std::vector<Hunk> hunks;
    for (int gap = 0; gap <= int(pairs.size()); ++gap) {
        Hunk h;
        h.gap = gap;
        h.oldFirst = gap == 0 ? 0 : pairs[size_t(gap) - 1].oldBlock + 1;
        h.newFirst = gap == 0 ? 0 : pairs[size_t(gap) - 1].newBlock + 1;
        h.oldLast = gap == int(pairs.size()) ? oldCount - 1 : pairs[size_t(gap)].oldBlock - 1;
        h.newLast = gap == int(pairs.size()) ? newCount - 1 : pairs[size_t(gap)].newBlock - 1;
        if (h.oldFirst > h.oldLast && h.newFirst > h.newLast) continue;
        hunks.push_back(h);
    }
    if (hunks.empty()) return -2;   // тексты разные, а тронутого нет — этого не бывает

    // 7. СКЛЕЙКА через одну целую пару: две правки по разные стороны пустой
    // строки — для человека одно место, и шов между ними всё равно общий.
    std::vector<Hunk> merged;
    for (const Hunk& h : hunks) {
        if (!merged.empty() && h.gap - merged.back().gap <= 1) {
            merged.back().oldLast = h.oldLast;
            merged.back().newLast = h.newLast;
            merged.back().gap = h.gap;
            continue;
        }
        merged.push_back(h);
    }

    // 8. ПУСТАЯ СТОРОНА РАЗДВИГАЕТСЯ НА СОСЕДА. Чистая вставка (старых блоков
    // нет) и чистое удаление (нет новых) — законные случаи, а replaceBlocks не
    // берёт ни пустой диапазон, ни пустую замену. Соседний целый блок
    // переписывается сам собой: цена — микросекунды, а путь остаётся один.
    for (Hunk& h : merged) {
        if (h.oldFirst <= h.oldLast && h.newFirst <= h.newLast) continue;
        if (h.gap > 0) {
            const Pair& left = pairs[size_t(h.gap) - 1];
            h.oldFirst = left.oldBlock;
            h.newFirst = left.newBlock;
        } else if (h.gap < int(pairs.size())) {
            const Pair& right = pairs[size_t(h.gap)];
            h.oldLast = right.oldBlock;
            h.newLast = right.newBlock;
        } else {
            h.oldFirst = 0;
            h.oldLast = oldCount - 1;
            h.newFirst = 0;
            h.newLast = newCount - 1;
        }
        if (h.oldFirst > h.oldLast || h.newFirst > h.newLast) return -2;
    }

    // 9. НАКЛАДЫВАЕМ С КОНЦА К НАЧАЛУ. Куски адресуются НОМЕРАМИ блоков, а
    // номера считаются от начала: наложенный кусок другой высоты сдвинул бы
    // всё, что ниже. Идя с конца, неналоженные куски не уезжают, и бегущая
    // поправка — лишнее состояние, которое однажды ошибётся, — не нужна вовсе.
    int firstTouched = -1;
    QTextCursor edit(&d_->text);
    edit.beginEditBlock();
    for (auto h = merged.rbegin(); h != merged.rend(); ++h) {
        const std::vector<Piece> put(fresh.begin() + h->newFirst, fresh.begin() + h->newLast + 1);
        replaceBlocks(h->oldFirst, h->oldLast, put);
        // ШОВ — ради ОФОРМЛЕНИЯ, а не ради канона: верхнее поле соседа зависит
        // от того, стоит ли перед ним пустая строка, а колонка списка — от
        // самого широкого маркера всего прогона (вставили десятый пункт —
        // поехала колонка у всех).
        settleSeam(h->oldFirst - 1, h->oldFirst + int(put.size()));
        firstTouched = h->oldFirst;
    }
    edit.endEditBlock();

    // 10. СВЕРКА, И ОНА НЕ УКРАШЕНИЕ. Расхождение здесь — это молча испорченная
    // заметка владельца, а цена сверки — один проход писателя на редком явном
    // действии. Разошлось — ОТКАТЫВАЕМ СВОЮ ЖЕ ПРАВКУ (мы были под одной
    // скобкой, откат точен) и говорим об этом числом: заплатки вида «пересобрать
    // целиком» здесь запрещены, они спрятали бы дефект навсегда.
    if (withoutReturns(toMarkdownText()) != withoutReturns(after)) {
        d_->text.undo();
        return -2;
    }
#ifndef NDEBUG
    checkCanonical();
#endif

    if (caret != nullptr && firstTouched >= 0) *caret = caretAtBlock(firstTouched);
    return int(merged.size());
}

}  // namespace zametti
