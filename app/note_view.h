// Область просмотра заметки.
//
// Два дела, которых нет у обычного QTextBrowser. Первое — маркеры списка: они
// рисуются поверх вьюпорта в поле слева от текста, а не живут в самом тексте
// (почему так — см. marker.h). Второе — предельная ширина колонки: на широком
// экране строка длиной во весь монитор не читается, поэтому лишняя ширина
// уходит в поля, а текст остаётся посередине.

#ifndef ZAMETTI_NOTE_VIEW_H
#define ZAMETTI_NOTE_VIEW_H

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

    // Масштаб нужен маркерам: их шрифт строится тем же кеглем, что и текст.
    void setZoom(qreal zoom);

    // Пересчитывает поля под текущую ширину вьюпорта. Вызывается после каждой
    // пересборки документа: сборщик ставит поля по умолчанию, ничего не зная
    // о размере окна.
    void applyContentWidth();

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    qreal zoom_ = 1.0;
};

}  // namespace zametti

#endif  // ZAMETTI_NOTE_VIEW_H
