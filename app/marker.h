// Маркеры списка: буллет, номер, чекбокс.
//
// Маркер — это отрисовка, а не содержимое документа. Раньше он был знаком в
// тексте (объектным заполнителем U+FFFC либо просто буквами), и это давало
// сдвиги: объект с AlignMiddle Qt центрирует по прямоугольнику строки, а тот
// растёт, стоит в строке появиться знаку из запасного шрифта. Замер: на строке
// с "Ⓐ" рамка уезжала на 3 px относительно базовой линии своей же строки.
//
// Теперь маркер рисуется в поле слева от текста, по геометрии первой строки
// блока, и от содержимого строки не зависит вовсе. Заодно он перестал занимать
// позицию в тексте — это понадобится редактору: курсор, копирование и смещения
// спанов больше об него не спотыкаются.
//
// Род и уровень блока живут в его формате: другого способа узнать, что это
// пункт списка, у отрисовки нет.

#ifndef ZAMETTI_MARKER_H
#define ZAMETTI_MARKER_H

#include "ir.h"

#include <QFont>
#include <QRectF>
#include <QTextFormat>

class QPainter;
class QTextBlock;

namespace zametti {

enum BlockProperty {
    // int(Kind). Нет у дословных (rawSource) блоков: у них рода нет.
    KindProperty = QTextFormat::UserProperty,
    // Уровень вложенности списка, от нуля. Только у списочных блоков.
    LevelProperty,
};

bool isListBlock(const QTextBlock& block);
Kind kindOf(const QTextBlock& block);
int levelOf(const QTextBlock& block);

// Номер пункта в прогоне. Считается обходом назад по тому же правилу, что и в
// сериализаторе: вложенный подсписок прогон не рвёт. Хранить номер в формате
// нельзя — QTextFormatCollection интернирует форматы, и список на сотню пунктов
// завёл бы сотню уникальных форматов вместо одного.
int ordinalOf(const QTextBlock& block);

// Ширина колонки маркера: сам знак плюс зазор до текста. По ней сборщик
// документа задаёт левое поле блока.
qreal markerColumn(Kind kind, int ordinal, const QFont& base);

// Рамка чекбокса в координатах документа; пустая, если у блока её нет. Одна
// функция и на отрисовку, и на попадание мышью — двух копий геометрии быть не
// должно.
QRectF checkboxRect(const QTextBlock& block, const QFont& base);

// Рисует маркер блока. Блок должен быть списочным и уже разложенным.
void paintMarker(QPainter& painter, const QTextBlock& block, const QFont& base);

}  // namespace zametti

#endif  // ZAMETTI_MARKER_H
