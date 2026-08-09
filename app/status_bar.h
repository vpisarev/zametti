// Полоса сведений под окном: что за заметка сейчас открыта и где каретка.
//
// Устройство то же, что у тулбара: виджет ничего не считает и ничего не знает
// про хранилище. Ему приносят готовые числа, он их раскладывает. Всё, что
// касается заметки, живёт в объекте заметки — панель лишь показывает.
//
// Полей два ряда по смыслу, но строка одна: слева — про заметку целиком (путь,
// размер, даты), справа — про место каретки и объём текста. Слева обрезается
// многоточием, справа — никогда: числа не режутся.
//
// Число слов бывает НЕИЗВЕСТНО. Считается оно не на каждое нажатие, а только
// когда IR строится целиком (см. text_stats.h), и между этими мгновениями
// показывается «?» — устаревшее число врало бы молча, а «?» честно говорит
// «пересчитаю на ближайшем сохранении».

#ifndef ZAMETTI_STATUS_BAR_H
#define ZAMETTI_STATUS_BAR_H

#include <QDateTime>
#include <QSize>
#include <QString>
#include <QWidget>

class QLabel;

namespace zametti {

class StatusBar : public QWidget {
    Q_OBJECT

public:
    explicit StatusBar(QWidget* parent = nullptr);

    // Что известно про открытую заметку. Всё вместе, одной кучкой: полей много,
    // и раздавать их по одному значило бы завести столько же способов забыть
    // одно из них.
    struct NoteInfo {
        QString path;
        qint64 bytes = 0;
        QDateTime created;
        QDateTime modified;
        int words = 0;
        int lines = 1;
        int images = 0;   // ноль — про картинки не пишем вовсе
        bool wordsKnown = false;   // false — показать «?»
        bool valid = false;        // заметки нет: панель пуста
    };

    void setNote(const NoteInfo& info);

    // Картинка под кареткой. Пока она есть, левая половина показывает её, а не
    // заметку: спрашивают «что это за снимок» именно тогда, когда стоят на нём.
    struct ImageInfo {
        QString name;
        QString caption;
        QString format;
        QSize size;
        // Цветовое пространство и глубина берутся у УЖЕ РАЗЖАТОЙ копии, если
        // она есть: разжимать снимок ради строчки в панели нельзя. Пусто и
        // ноль — картинку ещё не показывали, и врать нам нечем.
        QString colorSpace;
        int bits = 0;
        qint64 bytes = 0;
        int frames = 1;
        bool exists = false;
        bool valid = false;
    };
    void setImage(const ImageInfo& info);
    // Место каретки. Отдельно от прочего: меняется на каждое движение, а
    // остальное — раз в полторы секунды.
    void setCaret(int line, int column);
    // Сообщение поверх сведений о заметке: «сохранено», «файл изменился
    // снаружи». Пустая строка возвращает обычный вид.
    void setMessage(const QString& text);

    void refreshAppearance();

protected:
    void paintEvent(QPaintEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;

private:
    void relayout();
    void showLeft();
    void showImage();

    QLabel* left_ = nullptr;
    QLabel* right_ = nullptr;
    NoteInfo note_;
    ImageInfo image_;
    QString message_;
    int line_ = 1;
    int column_ = 1;
};

// Человеческий размер файла: «847 Б», «12.4 КБ», «1.2 МБ». Наружу — набору.
QString humanBytes(qint64 bytes);
// Число с разрядами: «36 828». Пробел неразрывный, иначе перенос порвёт число.
QString humanCount(int value);
// Дата в местном времени, коротко: «14.03.2019 09:26».
QString humanDate(const QDateTime& when);

}  // namespace zametti

#endif  // ZAMETTI_STATUS_BAR_H
