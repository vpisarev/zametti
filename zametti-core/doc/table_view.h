// Раскладка таблицы для показа: из разобранных ячеек — в геометрию.
//
// Живёт рядом с моделью документа, а не в виджете: раскладка нужна и
// отрисовке, и резерву места (высота), и попаданию мышью («точка → ячейка →
// позиция в тексте»), и наборам. Виджетов здесь нет — только QTextLayout,
// который лежит в QtGui.
//
// ПОЧЕМУ QTextLayout, А НЕ QTextDocument НА ЯЧЕЙКУ. Ячеек в таблице 50×8
// четыреста, и четыреста документов — это четыреста разметок, каждая со своей
// рамкой и полями. QTextLayout — ровно то, что нужно: текст, форматы кусков,
// ширина, перенос. Разметка ячейки этим и исчерпывается.
//
// ПОРЯДОК ВПИСЫВАНИЯ (правило владельца «как картинки»):
//   1. таблице разрешено занять пустующие поля колонки — вплоть до ширины окна;
//   2. не влезла — равномерная усадка шрифта до вписывания;
//   3. ниже пола масштаба усадка останавливается, и включается перенос в
//      ячейках как последний резерв.

#ifndef ZAMETTI_TABLE_VIEW_H
#define ZAMETTI_TABLE_VIEW_H

#include "table.h"

#include <QFont>
#include <QRectF>
#include <QString>
#include <QTextLayout>
#include <QVector>

#include <memory>

namespace zametti {

// Одна ячейка на экране: где стоит и что в ней.
struct TableCellBox {
    QRectF rect;                          // прямоугольник ячейки в координатах таблицы
    std::shared_ptr<QTextLayout> text;    // разметка текста ячейки; может быть пустой
    // Ширина САМОГО ТЕКСТА. Спрашивать её у boundingRect() раскладки нельзя:
    // там стоит ширина строки, а строку мы при измерении кладём в бесконечную
    // ширину — и выравнивание вправо уносило текст на километр за экран.
    qreal textWidth = 0.0;
    TableAlign align = TableAlign::Default;
    int row = 0;
    int column = 0;
};

struct TableLayout {
    QVector<qreal> columnWidth;
    QVector<qreal> rowHeight;
    QVector<TableCellBox> cells;   // подряд, рядами; первый ряд — шапка
    qreal width = 0.0;
    qreal height = 0.0;
    // Во сколько раз пришлось ужать шрифт, чтобы таблица влезла. Единица —
    // не ужимали. Меньше пола — значит пол сработал и включился перенос; это
    // сигнал владельцу (см. бриф), а не норма.
    qreal scale = 1.0;
    bool wrapped = false;          // перенос в ячейках включался
    int columns = 0;
    int rows = 0;

    const TableCellBox* at(int row, int column) const;
};

// Что раскладке нужно знать о месте, куда она встаёт.
struct TableSpace {
    qreal columnWidth = 0.0;   // ширина колонки текста
    qreal fullWidth = 0.0;     // ширина, доступная с полями (обычно — ширина окна)
    qreal zoom = 1.0;
};

// Разложить таблицу. Возвращает пустую раскладку, если таблица не разобрана.
TableLayout layoutTable(const Table& table, const TableSpace& space);

// Шрифт текста таблицы при этом масштабе — тот же, что у обычного текста, с
// поправкой на усадку.
QFont tableFont(qreal scale);
// Внутренние поля ячейки по горизонтали и вертикали, в пикселях.
qreal tableCellPadX(qreal scale);
qreal tableCellPadY(qreal scale);

}  // namespace zametti

#endif  // ZAMETTI_TABLE_VIEW_H
