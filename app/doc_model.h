// Что QTextDocument знает о себе сверх видимого текста.
//
// Договор один на всех: сборщик расставляет эти свойства, читатель по ним
// восстанавливает IR, отрисовка по ним рисует маркеры. Выводить род блока из
// оформления нельзя: заголовок набран жирным целиком, и «жирный» внутри него
// стал бы неотличим от самого заголовка.
//
// Область значений у каждого свойства маленькая и конечная. Это не случайность:
// QTextFormatCollection интернирует форматы, и уникальное значение на блок
// (номер пункта, отметка времени) завело бы отдельный формат на каждый блок.

#ifndef ZAMETTI_DOC_MODEL_H
#define ZAMETTI_DOC_MODEL_H

#include "ir.h"

#include <QTextFormat>

class QTextBlock;

namespace zametti {

enum DocProperty {
    // Свойства блока.
    KindProperty = QTextFormat::UserProperty,  // int(Kind)
    LevelProperty,                             // int, уровень вложенности списка
    InfoProperty,                              // QString, язык блока кода
    RawProperty,                               // bool, блок выводится дословно
    // bool: текст блока кончался переводом строки. Пустой блок кода и блок из
    // одной пустой строки выглядят одинаково, а в файл выводятся по-разному.
    TrailingNewlineProperty,

    // Свойства формата знаков.
    SpanStyleProperty,   // int, биты SpanStyle
    BreakSourceProperty, // int(BreakSource) на самом разделителе строк
};

// Три знака Qt в insertText трактует структурно и рвёт на них блок. Замерено
// перебором: ровно '\n', '\r' и U+2029, больше ничего (U+2028, U+0085, U+000B,
// U+000C, U+001C..U+001E проходят как обычный текст).
//
// Внутрь блока все три попадают разделителем строк U+2028 — он рисуется
// переносом и блок не рвёт, — а какой знак был на самом деле, помнит свойство.
// Без него не обойтись: точно такой же U+2028 может стоять и в самом тексте
// заметки, заметки из Apple Notes им кишат, и превращать его в перевод строки
// значило бы портить текст.
enum BreakSource {
    BreakNewline = 1,
    BreakCarriageReturn = 2,
    BreakParagraph = 3,
};

enum SpanStyle {
    SpanBold = 1,
    SpanItalic = 2,
    SpanStrike = 4,
    SpanCode = 8,
};

bool isRawBlock(const QTextBlock& block);
Kind kindOf(const QTextBlock& block);
int levelOf(const QTextBlock& block);
bool isListBlock(const QTextBlock& block);

// Номер пункта в прогоне. Считается обходом назад по тому же правилу, что и в
// сериализаторе: вложенный подсписок прогон не рвёт. Хранить номер в формате
// нельзя — см. про интернирование выше.
int ordinalOf(const QTextBlock& block);

}  // namespace zametti

#endif  // ZAMETTI_DOC_MODEL_H
