// Настройки внешнего вида и запомненное состояние сеанса.
//
// Оформление читается из ~/.config/zametti/config.json. Этот файл принадлежит
// пользователю: приложение создаёт его один раз и больше никогда не трогает.
// Рядом лежит config.defaults.json — полный список параметров со значениями по
// умолчанию, его приложение переписывает на каждом запуске и никогда не читает.
// Так новые параметры видно после обновления, но правки пользователя, включая
// опечатки и его собственные ключи, остаются нетронутыми.
//
// Состояние сеанса (последний файл, прокрутка, геометрия, зум) хранится третьим
// файлом: конфиг правят руками, состояние переписывается само на каждом выходе,
// и мешать их нельзя.

#ifndef ZAMETTI_SETTINGS_H
#define ZAMETTI_SETTINGS_H

#include <QByteArray>
#include <QColor>
#include <QString>
#include <QtGlobal>

#include <array>

namespace zametti {

// Чем рисовать чекбокс. Шрифтовые варианты просты, но размер, толщина линий и
// положение по базовой линии в них заданы шрифтом и не настраиваются.
enum class CheckboxStyle {
    Glyph,   // ☐ / ☑ из запасной гарнитуры
    Ascii,   // [ ] / [x] основной гарнитурой
    Drawn,   // рисуем сами, см. checkbox_object.cpp
};

struct Appearance {
    // --- шрифт ---
    QString fontFamily = QStringLiteral("IBM Plex Mono");
    qreal baseFontPoint = 11.0;
    // Нужен только шрифтовым вариантам чекбокса: в IBM Plex Mono нет U+2610.
    QString symbolFamily = QStringLiteral("DejaVu Sans Mono");
    // Кегль заголовков 1..6 относительно базового.
    std::array<qreal, 6> headingScale{1.7, 1.45, 1.25, 1.1, 1.0, 0.95};
    // Эмодзи приходят из запасного шрифта и рядом с моноширинным текстом
    // смотрятся мелко. Множитель применяется к любому знаку, которого нет в
    // основной гарнитуре.
    qreal fallbackScale = 1.15;

    // --- ритм страницы ---
    // У моноширинных гарнитур собственный межстрочный просвет уже приличный,
    // поэтому множитель нужен маленький. Пункты списка ставим плотно: список
    // читается как один объект. Расстояние между абзацами держат поля блока,
    // а не интерлиньяж — иначе, ужимая строки, мы бы сплющили и абзацы.
    qreal lineHeightFactor = 1.15;
    qreal listLineHeightFactor = 1.05;
    qreal blockSpacing = 13.0;
    // Боковые поля заметно больше вертикальных: строка не должна упираться в
    // край окна, читать так тяжело.
    qreal sideMargin = 56.0;
    qreal verticalMargin = 24.0;

    // --- цвета ---
    QColor pageBackground{0xfe, 0xfe, 0xfb};
    QColor selectionBackground{0xbf, 0xdb, 0xfe};
    QColor linkColor{0x32, 0x5c, 0xc0};
    QColor markerColor{0x7a, 0x82, 0x8c};   // "•" и "1." у списков
    QColor quoteColor{0x5a, 0x62, 0x6a};
    QColor rawColor{0x99, 0x9f, 0xa6};      // непонятое, дословный кусок
    QColor codeBackground{0, 0, 0, 14};

    // --- чекбокс ---
    CheckboxStyle checkboxStyle = CheckboxStyle::Drawn;
    QColor checkboxCheckedColor{0x32, 0x5c, 0xc0};    // заливка и цвет рамки
    QColor checkboxUncheckedColor{0x32, 0x5c, 0xc0};  // только рамка, без заливки
    QColor checkboxTickColor{0xff, 0xff, 0xff};
    qreal checkboxPenWidth = 1.4;
    qreal checkboxCornerRadius = 2.5;
    // Оптическая поправка положения. Геометрически рамка совпадает с чернилами
    // букв (от хвоста "y" до верхушки "i"), но читается чуть низкой: у сплошного
    // прямоугольника масса распределена равномерно, а у строчных букв собрана
    // выше. Доля от высоты, а не пиксели: должна пережить смену кегля.
    qreal checkboxOpticalRise = 0.03;
    // Кегль шрифтовых вариантов. К нарисованному отношения не имеет.
    qreal checkboxGlyphScale = 1.8;
    // Зазор между рамкой и текстом задачи, в ширинах буквы "A".
    qreal checkboxTextGap = 1.3;

    // --- масштаб ---
    qreal zoomStep = 1.1;
    qreal zoomMin = 0.5;
    qreal zoomMax = 4.0;
};

// Действующее оформление. Меняется только при загрузке конфига, на старте.
Appearance& appearance();

QString configPath();
QString defaultsPath();
QString statePath();

// Читает конфиг, если он есть, и обновляет справочник умолчаний рядом с ним.
// Возвращает false и заполняет error, если файл есть, но не разбирается: молча
// подставлять умолчания в этом случае нельзя, иначе опечатка выглядела бы как
// «настройка не работает».
bool loadAppearance(QString* error);

// Что запоминается между запусками.
struct Session {
    QString lastFile;
    double scrollRatio = 0.0;   // доля прокрутки: в пикселях она зависит от зума
    qreal zoom = 1.0;
    QByteArray windowGeometry;
};

Session loadSession();
void saveSession(const Session& session);

}  // namespace zametti

#endif  // ZAMETTI_SETTINGS_H
