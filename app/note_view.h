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
#include <QHash>
#include <QImage>
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

    // Каталог, от которого разрешаются относительные пути картинок, — каталог
    // открытой заметки. Ставится при открытии файла.
    void setImageBase(const QString& dir);

    // Сколько раз картинку читали с диска и разжимали, и сколько это заняло.
    // Счётчик — часть договора, а не отладка: правило «повторное открытие
    // заметки не декодирует ничего» иначе не проверить ни тестом, ни замером.
    // Общий на программу: кэш будет уровня приложения, а не виджета.
    static int imageDecodes();
    static qint64 imageDecodeMicros();
    static void resetImageDecodeCounters();

    // Сколько сейчас занято кэшем картинок и сколько в нём записей. Тоже часть
    // договора: потолок иначе не проверить.
    qint64 imageCacheBytes() const { return imageCacheBytes_; }
    int cachedImageCount() const { return int(imageCache_.size()); }

    // Прямоугольник фотографии блока в координатах вьюпорта; пустой, если
    // фотографии нет. По нему ресайз ловит угол, по нему же смотрят тесты.
    QRectF imageRectInViewport(const QTextBlock& block);

    // Перетаскивание угла: пока мышь не отпущена, фотография меряется этой
    // шириной (логические пиксели) вместо записанной. width <= 0 — снять.
    void setImageDragWidth(int blockNumber, qreal width);

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

    // Картинки. Текстовая модель их не знает: строка с картинкой остаётся
    // строкой (подпись image-спана или дословное вики-вложение "![[путь|ш]]"),
    // но рисуется на её месте сама фотография — текст строки закрашивается,
    // это хитро-отрисованная строка, как черта. Текст не показывается никогда,
    // пока строка остаётся картинкой: выделение рисуется тонировкой поверх
    // фотографии, а не вскрытой разметкой. Место под фотографию резервирует
    // syncImageSpace, ставя bottomMargin (у всех прочих блоков он ноль по
    // построению сборщика); от каретки резерв не зависит.
    struct ImageGeometry {
        bool valid = false;
        QRectF photo;            // координаты документа
        QRectF line;             // прямоугольник текста строки (для закраски)
    };
    ImageGeometry imageGeometry(const QTextBlock& block);
    void syncImageSpace();

    // Что мы знаем о картинке блока. Три состояния, и путать их нельзя:
    // картинка разжата (image не пуст); картинка слишком велика — Qt отказался
    // её разжимать, но размеры из заголовка файла известны и место под неё
    // резервируется как под настоящую; файла нет вовсе — строка остаётся
    // обычным текстом.
    struct CachedImage {
        QImage image;
        QSize declared;      // размеры из заголовка файла; нужны заглушке
        qint64 bytes = 0;    // вес разжатой; у заглушки ноль
        int limit = 0;       // предел стороны, которым ужимали: сменится — перечитаем
        bool tooBig = false;
    };

    QString absoluteImagePath(const QString& path) const;
    // Запись кэша для пути; nullptr — файла нет или он не картинка.
    const CachedImage* cachedImage(const QString& path);
    const QImage* imageFor(const QString& path);
    // Вытесняет с хвоста, пока кэш не уложится в бюджет. Не трогает картинки
    // открытой заметки и только что добавленную keep.
    void trimImageCache(const QString& keep);
    void touchImage(const QString& key);
    void paintTooBigImage(QPainter& painter, const QTextBlock& block,
                          const ImageGeometry& geometry, const CachedImage& entry);
    // Место под картинку на экране. Берёт размеры, а не саму картинку: у
    // слишком большой пикселей нет вовсе, а место она занимает то же.
    QSizeF imageDisplaySize(QSize natural, qreal widthHint, const QTextBlock& block) const;
    void paintImage(QPainter& painter, const QTextBlock& block);

    qreal zoom_ = 1.0;
    bool changingLayout_ = false;
    QTimer caretBlink_;
    bool caretOn_ = true;
    QString imageBase_;
    bool syncingImages_ = false;
    // Кэш разжатых картинок: абсолютный путь → запись. Живёт всю сессию, а не
    // заметку, поэтому у него есть потолок и вытеснение — см. imageCacheSizeMb.
    QHash<QString, CachedImage> imageCache_;
    QList<QString> imageOrder_;           // свежие в начале
    qint64 imageCacheBytes_ = 0;
    // Картинки открытой заметки: их не вытесняем никогда. Набор чистится в
    // начале syncImageSpace и наполняется самим cachedImage — тот зовётся
    // только для блоков текущего документа, так что отдельного прохода по
    // блокам заводить не надо.
    QSet<QString> currentNoteImages_;
    int imageDragBlock_ = -1;             // номер блока с перетаскиваемым углом
    qreal imageDragWidth_ = 0.0;
};

}  // namespace zametti

#endif  // ZAMETTI_NOTE_VIEW_H
