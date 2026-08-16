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

    // ВСТАВКА — ТАКАЯ ЖЕ МЕСТНАЯ ПРАВКА, КАК ВСЕ ОСТАЛЬНЫЕ, и идёт тем же
    // путём: скобка отмены, пересборка тронутого, шов, сверка со сборкой.
    // Раньше она вела всё это сама, и оттого знала лишнее — какой формат Qt
    // доносит через фрагмент, а какой нет. Теперь не знает: что бы фрагмент ни
    // потерял, пересборка поставит верное.
    return runLocalEdit(at, [this, &staging, ownBlocks](QTextCursor& edit) {
        if (edit.hasSelection()) edit.removeSelectedText();

        // Qt вливает первый блок куска в текущий блок. Под БЛОЧНЫЙ кусок
        // заводим пустой блок: иначе вставленный заголовок сливался бы с
        // абзацем, в который его кладут.
        if (ownBlocks && !edit.block().text().isEmpty()) {
            if (edit.atBlockStart()) {
                // Пустой блок заводим НАД текущим и встаём в него: текст блока
                // уезжает вниз целиком и остаётся собой.
                QTextCursor tail(&d_->text);
                tail.setPosition(edit.position());
                edit.insertBlock(edit.blockFormat());
                edit.setPosition(tail.block().previous().position());
            } else {
                // ХВОСТ ОСТАЁТСЯ СОБОЙ. Резали посередине блока — значит его
                // вторая половина это тот же самый блок и обязана сохранить свой
                // род, уровень и поля.
                const QTextBlockFormat sourceFormat = edit.blockFormat();
                const QTextCharFormat sourceChars = edit.blockCharFormat();
                const bool wasAtEnd = edit.atBlockEnd();
                edit.insertBlock(QTextBlockFormat());
                if (!wasAtEnd) {
                    // Вставлять надо МЕЖДУ половинками: заводим ещё один блок,
                    // хвосту отдаём формат исходного, и встаём в пустой.
                    QTextCursor tail(&d_->text);
                    tail.setPosition(edit.position());
                    edit.insertBlock(sourceFormat, sourceChars);
                    edit.setPosition(tail.block().previous().position());
                }
            }
        }
        // ФОРМАТ ПЕРВОГО БЛОКА КУСКА Qt ЧЕРЕЗ ФРАГМЕНТ НЕ ДОНОСИТ: он вливает
        // его в тот блок, куда кладёт, и берёт формат у него. Пересборка это не
        // чинит и не может — она выводит ОФОРМЛЕНИЕ из смысла, а потерян тут
        // сам смысл: вставленный заголовок приезжал обычным абзацем.
        const QTextBlockFormat headFormat = staging.firstBlock().blockFormat();
        // И формат знаков блока тоже: в нём живёт ступень кегля и насыщенность —
        // то, чем заголовок отличается от абзаца для СЛЕДУЮЩЕЙ набранной буквы.
        const QTextCharFormat headCharFormat = staging.firstBlock().charFormat();
        const int firstBlock = edit.blockNumber();
        edit.insertFragment(QTextDocumentFragment(&staging));
        if (ownBlocks) {
            QTextCursor head(&d_->text);
            head.setPosition(d_->text.findBlockByNumber(firstBlock).position());
            head.setBlockFormat(headFormat);
            head.setBlockCharFormat(headCharFormat);
        }
        return true;
    });
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

void ZDocument::rebuildRange(int firstBlock, int lastBlock, QTextCursor* caret) {
    const int total = d_->text.blockCount();
    firstBlock = qBound(0, firstBlock, total - 1);
    lastBlock = qBound(firstBlock, lastBlock, total - 1);
    expandToWholeBlocks(firstBlock, lastBlock);

    std::vector<Piece> pieces;
    walkPieces(
        d_->text,
        [&pieces](const Piece& piece) {
            pieces.push_back(piece);
            return true;
        },
        firstBlock, lastBlock);
    if (pieces.empty()) return;
    replaceBlocks(firstBlock, lastBlock, pieces, caret);
}

void ZDocument::expandToWholeBlocks(int& firstBlock, int& lastBlock) const {
    // ГРАНИЦЫ — ПО ЛОГИЧЕСКОМУ БЛОКУ. Строки блока кода лежат в документе
    // отдельными QTextBlock; пересобрав половину блока кода, сборщик сделал бы
    // из неё самостоятельный блок, а оставшиеся строки повисли бы продолжением
    // неизвестно чего.
    const int total = d_->text.blockCount();
    while (firstBlock > 0 && isContinuationBlock(d_->text.findBlockByNumber(firstBlock)))
        --firstBlock;
    while (lastBlock + 1 < total &&
           isContinuationBlock(d_->text.findBlockByNumber(lastBlock + 1)))
        ++lastBlock;
}

void ZDocument::replaceBlocks(int firstBlock, int lastBlock, const std::vector<Piece>& to,
                              QTextCursor* caret) {
    if (to.empty()) return;
    QTextDocument staging;
    buildDocument(to, staging);

    // Каретка и её якорь — номером блока и смещением в нём: позиции внутри
    // вырезаемого куска вырез не переживут, а номера переживут. Когда блоков
    // стало меньше или больше, номер сам упрётся в границу — это и значит
    // «каретка была в том, чего больше нет».
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
    if (!head.isValid() || !tail.isValid()) return;

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
        const int last = d_->text.blockCount() - 1;
        auto placeAt = [this, last](const Spot& spot) {
            const QTextBlock block = d_->text.findBlockByNumber(qBound(0, spot.block, last));
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

#ifndef NDEBUG
void ZDocument::checkCanonical() const {
    std::vector<Piece> now;
    walkPieces(d_->text, [&now](const Piece& piece) {
        now.push_back(piece);
        return true;
    });
    // ПУСТАЯ ЗАМЕТКА СРАВНЕНИЮ НЕ ПОДЛЕЖИТ. Пустого QTextDocument не бывает:
    // один блок в нём есть всегда, и этот блок — Qt, а не наш. Обход его не
    // видит вовсе (isPhantomBlock), и сверять свойства нечего с чем.
    if (now.empty()) return;
    checkMatchesBuild(now, d_->text);
}
#endif

bool ZDocument::runLocalEdit(QTextCursor& at, const std::function<bool(QTextCursor&)>& body) {
    if (at.document() != &d_->text) return false;

    // Диапазон СЧИТАЕМ ТАК ЖЕ, КАК ЕГО СЧИТАЕТ САМА ПРАВКА: она трогает блоки
    // выделения вместе с поддеревьями пунктов, и пересобрать надо ровно их.
    const BlockRange wanted = selectedBlocks(d_->text, at);
    const int countBefore = d_->text.blockCount();

    QTextCursor edit(at);
    edit.beginEditBlock();
    if (!body(edit)) {
        edit.endEditBlock();
        return false;
    }

    // Правка могла завести блоки (разрез строки в отдельный блок, пустая строка
    // у заголовка) — на столько же съехало всё, что ниже.
    const int grew = qMax(0, d_->text.blockCount() - countBefore);
    const int here = d_->text.findBlock(edit.position()).blockNumber();
    const int there = d_->text.findBlock(edit.anchor()).blockNumber();
    const int first = qMin(qMin(wanted.first, here), there);
    const int last = qMax(qMax(wanted.last + grew, here), there);

    // ПЕРЕСОБИРАЕМ РОВНО ТРОНУТОЕ, а соседей чинит шов: их оформление от правки
    // внутри блока не меняется, а поля сверху пересчитает settleSeam.
    rebuildRange(first, last, &edit);
    settleSeam(first - 1, last + 1);
    edit.endEditBlock();

#ifndef NDEBUG
    checkCanonical();
#endif

    at = edit;
    return true;
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
    BlockRange seam{qBound(0, firstBlock - 1, last), qBound(0, lastBlock + 1, last)};
    // Сперва разнять литеральные строки: правка через границу блоков умеет
    // свести код и абзац в один блок, и мягкий перенос абзаца оказывается
    // внутри кода — состояние, которого разбор не породил бы никогда.
    seam.last += splitLiteralSoftBreaks(d_->text, seam);
    syncLiteralBlocks(d_->text, seam);
    // syncGaps сам говорит, сколько пустых строк завёл: на столько же съехали
    // номера ниже, и списки надо мерить уже по новым.
    const int added = syncGaps(d_->text, seam);
    syncLists(d_->text, {seam.first, qMin(seam.last + added, d_->text.blockCount() - 1)});
}

}  // namespace zametti
