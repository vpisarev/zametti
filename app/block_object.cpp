#include "block_object.h"

#include "doc_model.h"
#include "table.h"

#include <QTextBlock>
#include <QTextDocument>

#include <string>

namespace zametti {
namespace {

// Первая строка дословного куска, считая от этой.
QTextBlock literalStart(QTextBlock block) {
    while (isContinuationBlock(block) && block.previous().isValid())
        block = block.previous();
    return block;
}

// Последняя строка того же дословного куска.
QTextBlock literalEnd(QTextBlock block) {
    while (block.next().isValid() && isContinuationBlock(block.next()))
        block = block.next();
    return block;
}

// Текст дословного куска целиком, строками через перевод.
std::string literalText(const QTextBlock& first, const QTextBlock& last) {
    std::string out;
    for (QTextBlock block = first; block.isValid(); block = block.next()) {
        out += block.text().toStdString();
        out += '\n';
        if (block == last) break;
    }
    return out;
}

}  // namespace

BlockObject objectOf(const QTextBlock& block) {
    if (!block.isValid()) return {};

    // Картинка — обычный абзац, который целиком является ссылкой на файл.
    // Спрашиваем МОДЕЛЬ, а не вид: показана ли она сейчас фотографией или
    // рамкой с надписью — дело вида, а объектом она является в любом случае.
    if (blockImageRef(block).valid)
        return {ObjectKind::Image, block.blockNumber(), block.blockNumber()};

    // Таблица — дословный кусок, который выглядит таблицей. Дословным его
    // сделал разбор (md4c назвал таблицей то, что IR выразить не может), а
    // здесь мы только узнаём его в лицо: первая строка с палкой, вторая —
    // разделитель.
    if (!isRawBlock(block)) return {};
    const QTextBlock first = literalStart(block);
    const QTextBlock last = literalEnd(first);
    if (!looksLikeTable(literalText(first, last))) return {};
    return {ObjectKind::Table, first.blockNumber(), last.blockNumber()};
}

BlockObject objectAt(const QTextDocument& doc, int blockNumber) {
    return objectOf(doc.findBlockByNumber(blockNumber));
}

// ПРАВИЛА ОДНИМ МЕСТОМ. Порядок веток здесь и есть порядок правил, и читать
// его надо сверху вниз: первое подошедшее и есть ответ.
ObjectAction actionFor(int key, Qt::KeyboardModifiers mods, const ObjectContext& where) {
    const bool plain = (mods & ~Qt::KeypadModifier) == Qt::NoModifier;
    const bool ctrl = (mods & ~Qt::KeypadModifier) == Qt::ControlModifier;
    const bool enter = key == Qt::Key_Return || key == Qt::Key_Enter;

    // Выделение — не наше дело: человек выделил кусок текста и правит его как
    // текст, даже если внутрь попал объект. Разбирать такие случаи по-своему
    // значило бы удивлять на ровном месте.
    if (where.hasSelection) return ObjectAction::None;

    // Ctrl+Enter — параграф после объекта. Одинаково у картинки, таблицы,
    // формулы и блока кода: жест общий, и это его определение.
    if (enter && ctrl && where.onObject) return ObjectAction::LineAfter;

    if (where.onObject) {
        // Enter — править объект. У таблицы это исходник с палками, у картинки
        // подпись; что именно, решает не слой, а вызывающий.
        if (enter && plain) return ObjectAction::Edit;
        // Объект — атом: клавиши удаления убирают его целиком, а не грызут
        // его текст по буквам.
        if ((key == Qt::Key_Backspace || key == Qt::Key_Delete) && plain)
            return ObjectAction::Remove;
        return ObjectAction::None;
    }

    if (!plain) return ObjectAction::None;

    // Дальше — правила КРАЁВ, ради которых слой и заведён. Каретка стоит рядом
    // с объектом, и удаление «в его сторону» означает удаление объекта, а не
    // слияние строк.
    if (key == Qt::Key_Backspace && where.atBlockStart) {
        if (where.objectAbove) return ObjectAction::Remove;
        // Пустая строка между объектом и текстом неудаляема: без неё они
        // слиплись бы в одну строку файла, и объект рассыпался бы в огрызок
        // разметки. Отказ и шаг: каретка встаёт на объект, следующее нажатие
        // убирает его целиком.
        if (where.objectAboveGap) return ObjectAction::Select;
    }
    if (key == Qt::Key_Delete && where.atBlockEnd) {
        if (where.objectBelow) return ObjectAction::Remove;
        if (where.objectBelowGap) return ObjectAction::Select;
    }

    // Каретка на самой пустой строке рядом с объектом: удалять её в его
    // сторону нельзя по той же причине — шаг на объект.
    if (where.onGap) {
        if (key == Qt::Key_Backspace && where.objectAbove) return ObjectAction::Select;
        if (key == Qt::Key_Delete && where.objectBelow) return ObjectAction::Select;
    }

    return ObjectAction::None;
}

}  // namespace zametti
