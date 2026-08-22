// ПРОСМОТР КАРТИНОК ВО ВЕСЬ ЭКРАН (решение владельца): снимок на всём экране,
// стрелки листают снимки ЭТОЙ заметки, снизу — подпись, фон свой (по умолчанию
// почти чёрный: снимок смотрят на тёмном, а не на белом листе заметки).
//
// ОТДЕЛЬНОЕ ОКНО, А НЕ СТРАНИЦА СТЕКА. У режимов правки общего с ним ровно
// ничего: там текст заметки, тут показ файла; и уходить из него надо мгновенно
// (Esc), не трогая ни документа, ни фокуса редактора. Своё окно — самый простой
// способ ничего не задеть.
//
// Картинки берутся у заметки одним глаголом (ZDocument::attachments) и одним
// списком: порядок показа — порядок в тексте, как человек их и видел.
//
// Мелкая картинка увеличивается не больше, чем позволено настройкой
// (imageViewer.maxZoomPercent, втрое по умолчанию): растянутый на весь экран
// значок в 64 пикселя — это каша, а не показ.

#ifndef ZAMETTI_IMAGE_VIEWER_H
#define ZAMETTI_IMAGE_VIEWER_H

#include <QImage>
#include <QString>
#include <QStringList>
#include <QWidget>

#include <vector>

namespace zametti {

class ImageViewer : public QWidget {
    Q_OBJECT

public:
    // Один снимок: файл и подпись под ним (подпись может быть пустой).
    struct Shot {
        QString path;
        QString caption;
    };

    explicit ImageViewer(QWidget* parent = nullptr);

    // Показать список, начиная с этого номера. Пустой список — не показывает
    // ничего и отвечает ложью: смотреть нечего.
    bool show(std::vector<Shot> shots, int at);
    int current() const { return at_; }
    int count() const { return int(shots_.size()); }

    void refreshAppearance();

signals:
    void closed();

protected:
    void keyPressEvent(QKeyEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;

private:
    void step(int delta);
    void load();

    std::vector<Shot> shots_;
    int at_ = 0;
    QImage shown_;      // разжатая картинка текущего снимка
    QString failed_;    // почему не показалась, если не показалась
};

}  // namespace zametti

#endif  // ZAMETTI_IMAGE_VIEWER_H
