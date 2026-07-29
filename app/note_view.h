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
#include <QTimer>
#include <QtGlobal>

class QWidget;

namespace zametti {

// Цвета страницы и выделения. Ставятся и просмотрщику, и дереву заметок,
// поэтому живут отдельной функцией, а не в конструкторе.
void applyPalette(QWidget& view);

class NoteView : public QTextBrowser {
    Q_OBJECT

public:
    explicit NoteView(QWidget* parent = nullptr);

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
    void focusInEvent(QFocusEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;

    // Правка полей — это смена облика, но документу она неотличима от правки
    // текста: QTextDocument шлёт contentsChanged и на неё. Наследник смотрит на
    // этот признак, чтобы не записать перекладку окна в историю.
    bool changingLayout() const { return changingLayout_; }

    // Подложка блоков кода. Рисуется здесь, а не свойством формата блока:
    // blockBoundingRect отдаёт естественную высоту строки, а шаг между блоками
    // идёт по назначенной, и заливка по прямоугольнику блока оставляла бы между
    // строками кода незакрашенные полосы (замер: 1.9 px на строку).
    void paintCodeBackground(QPainter& painter, const QRectF& visible);

private:
    // Каретку рисуем сами: своей Qt цвета не отдаёт (см. caretColor в
    // settings.h). Раз рисуем сами — сами и мигаем: частота та же, что у
    // системы, а на каждой правке и на каждом движении курсора каретка
    // зажигается заново. Мигающая под руками каретка мешает как раз там, где
    // её важнее всего видеть.
    QRect caretRect() const;
    // Колонка каретки, перерисованная без штатного курсора: его будят клавиши,
    // мышь и набор, погасить его насовсем Qt не даёт (ширина 0 на дробном
    // масштабе становится физическим пикселем). Поэтому после штатной
    // отрисовки колонка рисуется заново — фон, подложка кода, выделение,
    // текст, — и чужая каретка не переживает ни одного кадра.
    void repaintOverNativeCaret(QPainter& painter);
    void showCaret();

    qreal zoom_ = 1.0;
    bool changingLayout_ = false;
    QTimer caretBlink_;
    bool caretOn_ = true;
};

}  // namespace zametti

#endif  // ZAMETTI_NOTE_VIEW_H
