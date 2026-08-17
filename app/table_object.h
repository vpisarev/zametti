// ТАБЛИЦА — ОБЪЕКТ ТЕКСТА (сессия 5 refactor2, решение владельца).
//
// В документе таблица занимает один знак U+FFFC (TableObject в doc_model.h),
// исходник лежит в свойстве формата, а показ — сетка поверх полосы во всю
// ширину колонки, как у фотографии и выключной формулы. Всё, что вид умеет с
// таблицей, собрано ЗДЕСЬ, а не россыпью по note_view.cpp (владелец: «код
// про таблички — в минимальное количество файлов»):
//
//   * обработчик объекта — размер полосы для вёрстки Qt (TableObjectHandler);
//   * кэш раскладок ПО СОДЕРЖИМОМУ, а не по номеру блока (TableObjects): ключ —
//     исходник и место (ширина колонки, ширина с полями, масштаб, стиль);
//     номера блоков едут при каждой правке, а вёрстка спрашивает размер раньше,
//     чем кто-либо успел бы кэш по номерам перестроить (урок формул, см.
//     FormulaRender в note_view.h);
//   * геометрия сетки в документе, отрисовка сетки, ячейка под точкой.
//
// Раскладку считает table_view.h (RFC 1942, усадка, перенос) — она в ядре, ей
// виджет не нужен. Здесь только то, что связывает её с видом.

#ifndef ZAMETTI_TABLE_OBJECT_H
#define ZAMETTI_TABLE_OBJECT_H

#include "settings.h"
#include "table.h"
#include "table_view.h"

#include <QColor>
#include <QHash>
#include <QObject>
#include <QPainter>
#include <QPointF>
#include <QRectF>
#include <QSizeF>
#include <QString>
#include <QTextBlock>
#include <QTextObjectInterface>

#include <memory>

namespace zametti {

class NoteView;

// Таблица, показанная сеткой: раскладка и то, из чего она посчитана.
struct TableRender {
    Table table;         // разбор исходника — ячейки со смещениями
    TableLayout layout;  // геометрия
    QString source;      // исходник, по которому считали
    TableSpace space;    // место, в которое вписывали
    const ZDocStyle* style = nullptr;
};

// Что нужно отрисовке сетки от вида: цвета и масштаб. Плоская структура, а не
// указатель на вид, — сетку рисуют и на экране, и на бумаге, и в наборах.
struct TablePaint {
    ZSettings::Tables look;   // линии и заливки (конфиг tables.*)
    QColor text;              // цвет текста ячеек — перо палитры
    qreal scale = 1.0;        // displayScale вида
    // Подсветка поиска: смещения в исходнике [from, to) и признак «текущее».
    struct Highlight {
        int from = 0;
        int to = 0;
        bool current = false;
    };
    QVector<Highlight> highlights;
    QColor highlightColour;
};

class TableObjects {
public:
    // Раскладка этого исходника в этом месте — из кэша или заново. Кэш по
    // содержимому: правки, сдвигающие номера блоков, его не касаются.
    // Ограничен числом записей; переполнился — выбрасывается целиком.
    const TableRender* renderFor(const QString& source, const TableSpace& space,
                                 const ZDocStyle& style);
    void clear() { cache_.clear(); }
    int size() const { return cache_.size(); }

    // ГЕОМЕТРИЯ ОДНА С ОТРИСОВКОЙ. Полоса объекта — вся ширина колонки; сетка
    // стоит от левого края полосы (как в этапе 12: таблица начинается там же,
    // где колонка текста) и вправо может выйти в поле, если ей разрешил каскад
    // вписывания. Высота полосы — высота сетки плюс воздух.
    static QSizeF bandFor(const TableRender& render, qreal columnWidth, qreal gap);
    // Прямоугольник сетки в координатах документа для блока с этой раскладкой;
    // пустой — блок не размечен.
    static QRectF rectFor(const QTextBlock& block, const TableRender& render, qreal gap);

    // Нарисовать сетку и текст ячеек в area (координаты painter'а).
    static void paint(QPainter& painter, const QRectF& area, const TableRender& render,
                      const TablePaint& how);

    // Ячейка под точкой (в тех же координатах, что area): ряд, колонка и
    // смещение начала ячейки в исходнике. false — точка вне сетки.
    static bool cellAt(const TableRender& render, const QRectF& area, const QPointF& point,
                       int* row, int* column, int* sourceOffset);

    // Прямоугольник куска ячейки со смещениями исходника [from, to) —
    // подсветке поиска. Если исходник ячейки не совпадает с показанным
    // текстом (внутри разметка), подсвечивается вся ячейка. Пустой — смещения
    // не в таблице.
    static QRectF highlightRect(const TableRender& render, const QRectF& area, int from, int to);

private:
    QHash<QString, TableRender> cache_;
};

// Обработчик объекта: только РАЗМЕР. Рисует поверх готовой страницы вид (как
// у фото и формулы: выделение Qt кладётся на объект после drawObject).
class TableObjectHandler : public QObject, public QTextObjectInterface {
    Q_OBJECT
    Q_INTERFACES(QTextObjectInterface)

public:
    explicit TableObjectHandler(NoteView* view);

    QSizeF intrinsicSize(QTextDocument* doc, int posInDocument,
                         const QTextFormat& format) override;
    void drawObject(QPainter* painter, const QRectF& rect, QTextDocument* doc,
                    int posInDocument, const QTextFormat& format) override;

private:
    NoteView* view_ = nullptr;
};

}  // namespace zametti

#endif  // ZAMETTI_TABLE_OBJECT_H
