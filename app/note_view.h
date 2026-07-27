// Область просмотра заметки.
//
// Два дела, которых нет у обычного QTextBrowser. Первое — маркеры списка: они
// рисуются поверх вьюпорта в поле слева от текста, а не живут в самом тексте
// (почему так — см. marker.h). Второе — предельная ширина колонки: на широком
// экране строка длиной во весь монитор не читается, поэтому лишняя ширина
// уходит в поля, а текст остаётся посередине.

#ifndef ZAMETTI_NOTE_VIEW_H
#define ZAMETTI_NOTE_VIEW_H

#include <QFont>
#include <QTextBrowser>
#include <QtGlobal>

class QWidget;

namespace zametti {

// Цвета страницы и выделения. Ставятся и просмотрщику, и дереву заметок,
// поэтому живут отдельной функцией, а не в конструкторе.
void applyPalette(QWidget& view);

class NoteView : public QTextBrowser {
    Q_OBJECT

public:
    using QTextBrowser::QTextBrowser;

    // Шрифт, которым собран документ: им же меряется геометрия маркеров.
    QFont baseFont() const;

    // Масштаб нужен маркерам: их шрифт строится тем же кеглем, что и текст.
    void setZoom(qreal zoom);
    qreal zoom() const { return zoom_; }

    // Пересчитывает поля под текущую ширину вьюпорта. Вызывается после каждой
    // пересборки документа: сборщик ставит поля по умолчанию, ничего не зная
    // о размере окна.
    void applyContentWidth();

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

    // Правка полей — это смена облика, но документу она неотличима от правки
    // текста: QTextDocument шлёт contentsChanged и на неё. Наследник смотрит на
    // этот признак, чтобы не записать перекладку окна в историю.
    bool changingLayout() const { return changingLayout_; }

    // Подложка блоков кода. Рисуется здесь, а не свойством формата блока:
    // blockBoundingRect отдаёт естественную высоту строки, а шаг между блоками
    // идёт по назначенной, и заливка по прямоугольнику блока оставляла бы между
    // строками кода незакрашенные полосы (замер: 1.9 px на строку).
    void paintCodeBackground(QPainter& painter, const QRectF& visible);

    // Подложка выделения. По той же причине, что и подложка кода: высота строки
    // назначена, а Qt красит выделение по естественной высоте — между полосами
    // остаётся незакрашенный ряд. Там, где следующая строка короче предыдущей,
    // он читается сколом на верхнем углу.
    void paintSelectionBackground(QPainter& painter, const QRectF& visible);

private:
    qreal zoom_ = 1.0;
    bool changingLayout_ = false;
};

}  // namespace zametti

#endif  // ZAMETTI_NOTE_VIEW_H
