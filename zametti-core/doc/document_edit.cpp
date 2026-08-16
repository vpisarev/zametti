// БАЗИС ОПЕРАЦИЙ ПРАВКИ: прочитать кусок как markdown и заменить кусок текстом.
//
// Обе — методы заметки, а не функции над её содержимым: снаружи с заметкой
// разговаривают только её глаголами. Диапазон приходит курсором и кареткой же
// уходит; курсор чужого документа отвергается сразу.
//
// ПОЧЕМУ ЭТО ДЕЛАЕТ РЕДАКТОР КОМПАКТНЕЕ. Не потому, что операций меньше, а
// потому, что исчезает слой уборки. Правка на месте кусочками способна оставить
// документ в состоянии, которого разбор никогда бы не породил, — и после каждой
// приходилось чинить инварианты ПО ВСЕМУ ДОКУМЕНТУ (syncLists, syncLiteralBlocks,
// syncGaps плюс сторожа к ним). При «заменить кусок markdown'ом» такого
// состояния не возникает по построению, и чинить остаётся только ШОВ.

#include "document_impl.h"

#include "doc_model.h"
#include "document_builder.h"
#include "document_pieces.h"
#include "editor_ops.h"
#include "serializer.h"

#include <QTextBlock>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextDocumentFragment>

#include <string>
#include <vector>

namespace zametti {
namespace {

// Разбор куска, приходящего снаружи. Полным разбором ядра, а не вторым
// упрощённым: его идемпотентность и гарантирует, что скопированное вставится
// без потерь.
std::vector<Piece> parseIncoming(const QString& markdown, ZDocument::PasteMode mode) {
    const QByteArray utf8 = markdown.toUtf8();
    const std::string source(utf8.constData(), size_t(utf8.size()));

    std::vector<Piece> pieces;
    if (mode == ZDocument::PasteMode::Literal) {
        // Один абзац с текстом как есть: переводы строк внутри блока сборщик
        // разметит сам, и они вернутся переводами, а не разметкой.
        Piece body;
        body.text = source;
        while (!body.text.empty() && body.text.back() == '\n') body.text.pop_back();
        pieces.push_back(std::move(body));
        return pieces;
    }
    // ЧЕРЕЗ ТУ ЖЕ ДВЕРЬ, ЧТО И ФАЙЛ. normaliseSpaces приводит неразрывные
    // пробелы к правилу заметки, и вызывать его обязаны оба входа: пока его
    // звал только loadMarkdown, одни и те же байты из файла и из буфера давали
    // разное.
    NoteHeader ignored;
    parsePieces(normaliseSpaces(source), pieces, ignored);
    return pieces;
}

// Вставлять в строку или своими блоками.
//
// Кусок из одного обычного абзаца входит в тот блок, куда его кладут:
// скопированные слова должны стать словами. Всё прочее — заголовок, пункт, код,
// цитата, да и просто несколько блоков — встаёт своими блоками: род блока это
// его свойство, и терять его при переносе нельзя.
bool needsOwnBlocks(const std::vector<Piece>& pieces) {
    const Piece& head = pieces.front();
    // Фотография — тоже блочная вещь: абзац из одного image-спана целиком и
    // вики-вложение "![[...]]" встают своей строкой, а не вклеиваются в текст
    // (в середине текста фотография не показывается — вклейка её потеряла бы).
    const std::string_view text = head.text;
    const std::vector<Run>& runs = head.runs;
    const bool wholeImage =
        !head.raw && head.kind == Kind::Paragraph &&
        ((runs.size() == 1 && runs[0].image() && runs[0].start == 0 &&
          size_t(runs[0].end) == text.size()) ||
         (text.rfind("![[", 0) == 0 && text.size() > 5 &&
          text.compare(text.size() - 2, 2, "]]") == 0));
    return pieces.size() > 1 || head.raw || head.kind != Kind::Paragraph || wholeImage;
}

}  // namespace

QTextCursor ZDocument::caretAtBlock(int index) {
    QTextCursor caret(&d_->text);
    const QTextBlock block = d_->text.findBlockByNumber(index);
    if (block.isValid()) caret.setPosition(block.position());
    return caret;
}

QString ZDocument::markdownOf(const QTextCursor& range) const {
    if (range.document() != &d_->text) return {};
    const std::vector<Piece> pieces = selectionPieces(range);
    if (pieces.empty()) return {};

    std::string text = writePieces(pieces);
    // У ОДИНОЧНОГО АБЗАЦА хвостовой перевод строки снимается: он не часть
    // текста, а разделитель блоков, и в чужом редакторе дал бы лишний перенос.
    const bool inlineOnly =
        pieces.size() == 1 && !pieces.front().raw && pieces.front().kind == Kind::Paragraph;
    if (inlineOnly && !text.empty() && text.back() == '\n') text.pop_back();
    return QString::fromUtf8(text.data(), qsizetype(text.size()));
}

bool ZDocument::replaceRange(QTextCursor& at, const QString& markdown, PasteMode mode) {
    if (at.document() != &d_->text) return false;
    if (markdown.isEmpty()) return false;

    // ПРИЁМНИК РЕШАЕТ. В блок кода и в дословный кусок вставляется ТОЛЬКО
    // ТЕКСТ: жирное, курсив, ссылки теряются, потому что разметки в коде не
    // бывает. Это не потеря, а единственно верный ответ — и решается он здесь,
    // по месту, а не подсказкой снаружи.
    //
    // Отдельный путь нужен не ради формата: строки блока кода — отдельные
    // блоки-продолжения, а не мягкие переносы внутри абзаца, и через разбор
    // куска их не получить.
    {
        const QTextBlock target = at.block();
        if (isRawBlock(target) || kindOf(target) == Kind::Code)
            return replaceInsideLiteral(at, markdown);
    }

    const std::vector<Piece> pieces = parseIncoming(markdown, mode);
    if (pieces.empty()) return false;

    // ВРЕМЕННЫЙ ДОКУМЕНТ — БЕЗ СТЕКА ОТМЕНЫ: в слепке одна версия текста,
    // отменять в нём нечего, а команды при сборке — чистая трата.
    QTextDocument staging;
    buildDocument(pieces, staging);
    const bool ownBlocks = needsOwnBlocks(pieces);

    QTextCursor edit(at);
    edit.beginEditBlock();
    if (edit.hasSelection()) edit.removeSelectedText();

    // Qt вливает первый блок куска в текущий блок, и формат берётся у ТЕКУЩЕГО:
    // вставленный заголовок становился обычным текстом, а вставка в начало
    // абзаца, наоборот, делала заголовком сам абзац. Поэтому под блочный кусок
    // заводим пустой блок и потом ставим ему формат первого блока куска.
    if (ownBlocks && !edit.block().text().isEmpty()) {
        QTextBlockFormat plain;
        plain.setLineHeight(edit.blockFormat().lineHeight(), edit.blockFormat().lineHeightType());
        if (edit.atBlockStart()) {
            // Пустой блок заводим НАД текущим и встаём в него: текст блока
            // уезжает вниз целиком и остаётся собой.
            QTextCursor tail(&d_->text);
            tail.setPosition(edit.position());
            edit.insertBlock(edit.blockFormat());
            edit.setPosition(tail.block().previous().position());
        } else {
            // ХВОСТ ОСТАЁТСЯ СОБОЙ. Резали посередине блока — значит его вторая
            // половина это тот же самый блок и обязана сохранить свой род,
            // уровень и поля. Прежде оба новых блока получали пустой формат, и
            // хвост становился безродным абзацем; полная пересборка следом это
            // чинила, а теперь пересборки нет — и сверка со сборкой поймала.
            const QTextBlockFormat sourceFormat = edit.blockFormat();
            const QTextCharFormat sourceChars = edit.blockCharFormat();
            const bool wasAtEnd = edit.atBlockEnd();
            edit.insertBlock(plain);
            if (!wasAtEnd) {
                // Вставлять надо МЕЖДУ половинками: заводим ещё один блок,
                // хвосту отдаём формат исходного, и встаём в оставшийся пустой.
                QTextCursor tail(&d_->text);
                tail.setPosition(edit.position());
                edit.insertBlock(sourceFormat, sourceChars);
                edit.setPosition(tail.block().previous().position());
            }
        }
    }

    const QTextBlockFormat headFormat = staging.firstBlock().blockFormat();
    // И ФОРМАТ ЗНАКОВ БЛОКА ТОЖЕ. Его Qt через фрагмент не доносит наравне с
    // форматом блока, а в нём живёт ступень кегля и насыщенность — то, чем
    // заголовок отличается от абзаца для СЛЕДУЮЩЕЙ набранной буквы. Поймала
    // отладочная сверка с полной сборкой: текст выглядел верно, а формат
    // пустого места в блоке был чужой.
    const QTextCharFormat headCharFormat = staging.firstBlock().charFormat();
    const int firstBlock = edit.blockNumber();
    edit.insertFragment(QTextDocumentFragment(&staging));
    const int landed = edit.position();
    const int lastBlock = edit.blockNumber();
    // Формат первого блока куска Qt через фрагмент не доносит — ставим сами.
    if (ownBlocks) {
        QTextCursor head(&d_->text);
        head.setPosition(d_->text.findBlockByNumber(firstBlock).position());
        head.setBlockFormat(headFormat);
        head.setBlockCharFormat(headCharFormat);
    }

    settleSeam(firstBlock, lastBlock);
    edit.endEditBlock();

#ifndef NDEBUG
    // ГЛАВНОЕ СВОЙСТВО БАЗИСА, и оно проверяется, а не обещается: после замены
    // куска документ обязан совпасть с тем, что собрал бы сборщик из его же
    // блоков. Совпал — значит уборка по всему документу не нужна не на словах.
    {
        std::vector<Piece> now;
        walkPieces(d_->text, [&now](const Piece& piece) {
            now.push_back(piece);
            return true;
        });
        checkMatchesBuild(now, d_->text);
    }
#endif

    at = QTextCursor(&d_->text);
    at.setPosition(qBound(0, landed, d_->text.characterCount() - 1));
    return true;
}

// Вставка внутрь литерального блока: текстом, форматом приёмника, строка за
// строкой. Каждая новая строка — блок-продолжение: так литеральное содержимое
// и лежит в документе (см. ContinuationProperty в doc_model.h).
bool ZDocument::replaceInsideLiteral(QTextCursor& at, const QString& markdown) {
    QString text = markdown;
    while (text.endsWith(QLatin1Char('\n'))) text.chop(1);
    if (text.isEmpty() && !at.hasSelection()) return false;

    QTextCursor edit(at);
    edit.beginEditBlock();
    if (edit.hasSelection()) edit.removeSelectedText();

    const QTextBlock target = edit.block();
    const QTextCharFormat chars = target.charFormat();
    QTextBlockFormat lineFormat = target.blockFormat();
    // Новая строка внутри литерального блока — продолжение предыдущей, а не
    // начало нового блока кода. Верхнее поле ей пересчитает шов.
    lineFormat.setProperty(ContinuationProperty, true);

    const int firstBlock = edit.blockNumber();
    const QStringList lines = text.split(QLatin1Char('\n'));
    for (int i = 0; i < lines.size(); ++i) {
        if (i > 0) edit.insertBlock(lineFormat, chars);
        if (!lines.at(i).isEmpty()) edit.insertText(lines.at(i), chars);
    }
    const int landed = edit.position();
    const int lastBlock = edit.blockNumber();

    settleSeam(firstBlock, lastBlock);
    edit.endEditBlock();

    at = QTextCursor(&d_->text);
    at.setPosition(qBound(0, landed, d_->text.characterCount() - 1));
    return true;
}

// --- НАБОР, ENTER, BACKSPACE ------------------------------------------------
//
// Их объединяет одно: они НЕ ходят через markdown. Обычный ввод — решение
// владельца («через разбор идут только структурные операции»), а Enter и
// Backspace структурны по существу: режут и склеивают блоки, а не переписывают
// их текст. Заметке они нужны затем, чтобы шов приводился к канону здесь — на
// месте правки, а не потом обходом всего документа.

bool ZDocument::insertText(QTextCursor& at, const QString& text,
                           const QTextCharFormat& format) {
    if (at.document() != &d_->text) return false;
    if (text.isEmpty() && !at.hasSelection()) return false;

    QTextCursor edit(at);
    edit.beginEditBlock();
    const int firstBlock = edit.blockNumber();
    if (edit.hasSelection()) edit.removeSelectedText();
    if (!text.isEmpty()) edit.insertText(text, format);

    // НАБОР ЛОМАЕТ ИНВАРИАНТЫ ДВУМЯ СПОСОБАМИ: текстом на пустой строке (она
    // перестаёт быть пустой) и выделением, съевшим границу блоков (рядом
    // оказываются соседи, которых markdown раздельно не выражает). Чиним здесь
    // же, в той же скобке правки, — а не потом и не по всему документу.
    QTextCursor repair(edit);
    if (repairAfterTyping(d_->text, repair)) edit = repair;
    settleSeam(qMin(firstBlock, edit.blockNumber()), edit.blockNumber());
    edit.endEditBlock();

    at = edit;
    return true;
}

bool ZDocument::breakBlock(QTextCursor& at, BreakKind kind) {
    if (at.document() != &d_->text) return false;

    QTextCursor edit(at);
    edit.beginEditBlock();
    const int firstBlock = edit.blockNumber();
    bool done = false;
    switch (kind) {
        case BreakKind::Plain:
            // Порядок важен и взят у прежнего обработчика: сперва объект
            // (фотография — атом), потом правило черты, и только потом обычный
            // разрез блока.
            done = newLineAfterImage(d_->text, edit) ||
                   applyDividerRuleAtCursor(d_->text, edit) ||
                   splitBlockAtCursor(d_->text, edit);
            break;
        case BreakKind::Otherwise:
            done = splitBlockOtherwiseAtCursor(d_->text, edit);
            break;
        case BreakKind::LeaveCode:
            done = leaveCodeBlockAtCursor(d_->text, edit);
            break;
    }
    if (!done) {
        edit.endEditBlock();
        return false;
    }
    settleSeam(qMin(firstBlock, edit.blockNumber()), qMax(firstBlock, edit.blockNumber()));
    edit.endEditBlock();

    at = edit;
    return true;
}

bool ZDocument::deleteBack(QTextCursor& at) {
    if (at.document() != &d_->text) return false;

    QTextCursor edit(at);
    edit.beginEditBlock();
    const int firstBlock = edit.blockNumber();
    // Жесты, у которых своё правило: снять комментарность, снять пункт, убрать
    // черту над кареткой, склеить через пустую строку, убрать фотографию целиком.
    bool done = uncommentAtBlockStart(d_->text, edit) ||
                unwrapListItemAtCursor(d_->text, edit) ||
                deleteDividerAbove(d_->text, edit) ||
                deleteImageLineBackward(d_->text, edit) ||
                joinAcrossVSpaceBackward(d_->text, edit);
    if (!done) {
        // НА САМОЙ ЧЕРТЕ УДАЛЯТЬ СЛЕВА НЕЧЕГО. По плоской модели слева от каретки
        // стоит перевод строки, но черта не живёт в строке текста, и обычное
        // удаление съело бы не то. Отказываемся — что делать дальше, решает
        // вызывающий (он уводит каретку в конец строки выше).
        const QTextBlock here = edit.block();
        if (!edit.hasSelection() && !isRawBlock(here) && kindOf(here) == Kind::Divider) {
            edit.endEditBlock();
            return false;
        }
        // Обычное удаление знака. Делаем сами, а не отдаём Qt: тогда починка
        // шва попадает в тот же шаг отмены, что и само удаление.
        if (edit.hasSelection()) edit.removeSelectedText();
        else if (edit.position() > 0) edit.deletePreviousChar();
        else {
            edit.endEditBlock();
            return false;
        }
        done = true;
    }
    const int lastBlock = edit.blockNumber();
    QTextCursor repair(edit);
    if (repairAfterTyping(d_->text, repair)) edit = repair;
    settleSeam(qMin(firstBlock, lastBlock), qMax(firstBlock, lastBlock));
    edit.endEditBlock();

    at = edit;
    return done;
}

bool ZDocument::deleteForward(QTextCursor& at) {
    if (at.document() != &d_->text) return false;

    QTextCursor edit(at);
    edit.beginEditBlock();
    const int firstBlock = edit.blockNumber();
    bool done = deleteImageLineForward(d_->text, edit) ||
                joinAcrossVSpaceForward(d_->text, edit);
    if (!done) {
        if (edit.hasSelection()) edit.removeSelectedText();
        else if (edit.position() < d_->text.characterCount() - 1) edit.deleteChar();
        else {
            edit.endEditBlock();
            return false;
        }
        done = true;
    }
    const int lastBlock = edit.blockNumber();
    QTextCursor repair(edit);
    if (repairAfterTyping(d_->text, repair)) edit = repair;
    settleSeam(qMin(firstBlock, lastBlock), qMax(firstBlock, lastBlock));
    edit.endEditBlock();

    at = edit;
    return done;
}

void ZDocument::rebuildRange(int firstBlock, int lastBlock, QTextCursor* caret) {
    const int total = d_->text.blockCount();
    firstBlock = qBound(0, firstBlock, total - 1);
    lastBlock = qBound(firstBlock, lastBlock, total - 1);

    // ГРАНИЦЫ — ПО ЛОГИЧЕСКОМУ БЛОКУ. Строки блока кода лежат в документе
    // отдельными QTextBlock; пересобрав половину блока кода, сборщик сделал бы
    // из неё самостоятельный блок, а оставшиеся строки повисли бы продолжением
    // неизвестно чего.
    while (firstBlock > 0 && isContinuationBlock(d_->text.findBlockByNumber(firstBlock)))
        --firstBlock;
    while (lastBlock + 1 < total &&
           isContinuationBlock(d_->text.findBlockByNumber(lastBlock + 1)))
        ++lastBlock;

    std::vector<Piece> pieces;
    walkPieces(
        d_->text,
        [&pieces](const Piece& piece) {
            pieces.push_back(piece);
            return true;
        },
        firstBlock, lastBlock);
    if (pieces.empty()) return;

    QTextDocument staging;
    buildDocument(pieces, staging);

    // Каретка и её якорь — номером блока и смещением в нём: позиции внутри
    // вырезаемого куска вырез не переживут, а номера переживут, потому что
    // строение после пересборки то же самое.
    struct Spot {
        int block = 0;
        int offset = 0;
    };
    auto spotOf = [this](int position) {
        const QTextBlock block = d_->text.findBlock(position);
        return Spot{block.blockNumber(), position - block.position()};
    };
    const Spot anchor = caret != nullptr ? spotOf(caret->anchor()) : Spot{};
    const Spot position = caret != nullptr ? spotOf(caret->position()) : Spot{};

    const QTextBlock head = d_->text.findBlockByNumber(firstBlock);
    const QTextBlock tail = d_->text.findBlockByNumber(lastBlock);

    QTextCursor edit(&d_->text);
    edit.beginEditBlock();
    edit.setPosition(head.position());
    edit.setPosition(tail.position() + tail.length() - 1, QTextCursor::KeepAnchor);
    edit.removeSelectedText();

    // Формат первого блока и формат его знаков Qt через фрагмент не доносит —
    // он берёт их у блока, в который вливает. Ставим сами, тем же приёмом, что
    // и замена куска (см. replaceRange).
    const QTextBlockFormat headFormat = staging.firstBlock().blockFormat();
    const QTextCharFormat headCharFormat = staging.firstBlock().charFormat();
    edit.insertFragment(QTextDocumentFragment(&staging));
    {
        QTextCursor fix(&d_->text);
        fix.setPosition(d_->text.findBlockByNumber(firstBlock).position());
        fix.setBlockFormat(headFormat);
        fix.setBlockCharFormat(headCharFormat);
    }
    edit.endEditBlock();

    if (caret != nullptr) {
        auto placeAt = [this](const Spot& spot) {
            const QTextBlock block = d_->text.findBlockByNumber(spot.block);
            if (!block.isValid()) return d_->text.characterCount() - 1;
            return block.position() + qBound(0, spot.offset, block.length() - 1);
        };
        QTextCursor moved(&d_->text);
        moved.setPosition(placeAt(anchor));
        if (anchor.block != position.block || anchor.offset != position.offset)
            moved.setPosition(placeAt(position), QTextCursor::KeepAnchor);
        *caret = moved;
    }
}

void ZDocument::settleSeam(int firstBlock, int lastBlock) {
    // ТОЛЬКО ШОВ, а не весь документ. Прежняя вставка чинила инварианты по всему
    // документу — «документ для этого достаточно мал», — и это было прямым
    // нарушением главного правила: стоимость правки не должна зависеть от
    // размера заметки.
    //
    // Соседа с каждой стороны берём нарочно: признак продолжения относителен
    // (он про связь с предыдущим блоком), а нужна ли пустая строка — вопрос про
    // пару соседей. Дальше первого соседа расходиться нечему.
    const int last = d_->text.blockCount() - 1;
    const BlockRange seam{qBound(0, firstBlock - 1, last), qBound(0, lastBlock + 1, last)};
    syncLiteralBlocks(d_->text, seam);
    // syncGaps сам говорит, сколько пустых строк завёл: на столько же съехали
    // номера ниже, и списки надо мерить уже по новым.
    const int added = syncGaps(d_->text, seam);
    syncLists(d_->text, {seam.first, qMin(seam.last + added, d_->text.blockCount() - 1)});
}

}  // namespace zametti
