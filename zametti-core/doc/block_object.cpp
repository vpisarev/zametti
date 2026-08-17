#include "block_object.h"

#include "doc_model.h"
#include "table.h"

#include <QTextBlock>
#include <QTextDocument>


namespace zametti {

BlockObject objectOf(const QTextBlock& block) {
    if (!block.isValid()) return {};

    // Картинка — обычный абзац, который целиком является ссылкой на файл.
    // Спрашиваем МОДЕЛЬ, а не вид: показана ли она сейчас фотографией или
    // рамкой с надписью — дело вида, а объектом она является в любом случае.
    if (blockImageRef(block).valid)
        return {ObjectKind::Image, block.blockNumber(), block.blockNumber()};

    // Выключная формула — ОБЪЕКТ, то есть блок рода Kind::Math. Раскрытая на
    // правку формула им не является: это обычный абзац с исходником, и все
    // правила объекта (Enter правит, буквы не проходят, стрелки перепрыгивают)
    // ему только мешали бы — набранное в него не попадало вовсе.
    //
    // Спрашиваем РОД БЛОКА, а не «показана ли она вёрсткой»: род — это модель,
    // а показ — дело вида.
    if (!isRawBlock(block) && kindOf(block) == Kind::Math)
        if (const BlockFormulaRef formula = blockFormulaRef(block);
            formula.valid && formula.display)
            return {ObjectKind::Formula, block.blockNumber(), block.blockNumber()};

    // Таблица — дословный кусок, который выглядит таблицей. Дословным его
    // сделал разбор (md4c назвал таблицей то, что IR выразить не может), а
    // здесь мы только узнаём его в лицо: первая строка с палкой, вторая —
    // разделитель. Дословный кусок лежит одним блоком — и таблица тоже.
    if (!isRawBlock(block)) return {};
    if (!looksLikeTable(sourceTextOf(block))) return {};
    return {ObjectKind::Table, block.blockNumber(), block.blockNumber()};
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
        // И стрелки его перешагивают, а не ходят внутри: исходник объекта на
        // экране закрыт вёрсткой, и каретке там негде быть.
        if (plain && (key == Qt::Key_Left || key == Qt::Key_Right || key == Qt::Key_Up ||
                      key == Qt::Key_Down))
            return ObjectAction::StepOver;
        // Сочетание переключения (Ctrl+Space) на объекте — спрятать подпись
        // под ним или вернуть спрятанную. Какое это сочетание, знает
        // вызывающий: оно настраиваемое (см. ObjectContext::toggleKey).
        if (where.toggleKey) return ObjectAction::ToggleCaption;
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

bool blocksMayJoin(const QTextBlock& previous, const QTextBlock& next) {
    if (!blocksWouldMerge(previous, next)) return false;
    return !objectOf(previous).valid() && !objectOf(next).valid();
}

}  // namespace zametti
