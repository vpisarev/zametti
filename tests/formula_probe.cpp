// ПРОБНИК БАЗОВОЙ ЛИНИИ — первая задача этапа 16, до всего остального.
//
// Вопрос ровно один: на какой высоте инлайн-формула садится в строку текста.
// Ответ у движка есть — `Render::getBaseline()`, — но отдаёт он ДОЛЮ ascent от
// полной высоты, а не пиксели (грабли, записанные разведкой). Ошибиться здесь
// значит получить формулу, плавающую над строкой или тонущую под ней, и
// заметить это только глазами.
//
// Поэтому пробник делает две вещи:
//
//   1. СПРАШИВАЕТ ЧИСЛОМ. Чернила `$x$` обязаны кончаться НА базовой линии
//      (буква без нижнего выносного), а у `$\frac{a}{b}$` и `$\sqrt{x}$` —
//      уходить под неё. Если базовую линию считать высотой картинки, первая
//      проверка краснеет сразу.
//   2. КЛАДЁТ ЛИНЕЙКУ НА СНИМОК. Строка настоящего текста тем же кеглем, в неё
//      посажены формулы, поверх — красная черта базовой линии текста. Право на
//      жизнь посадке даёт снимок, а не документация (требование брифа).

#include "formula.h"
#include "resources.h"
#include "settings.h"
#include "test_util.h"

#include <QFont>
#include <QFontInfo>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QDir>

#include <string>

namespace {

QString g_shots;

std::string num(double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.2f", v);
    return buf;
}

// Границы чернил: первая и последняя строки картинки, где есть хоть что-то.
// Пусто — обе -1.
struct Ink {
    int top = -1;
    int bottom = -1;
};

Ink inkRows(const QImage& image) {
    Ink ink;
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x)
            if (qAlpha(image.pixel(x, y)) > 24) {
                if (ink.top < 0) ink.top = y;
                ink.bottom = y;
                break;
            }
    return ink;
}

// --- 1. числа --------------------------------------------------------------

// Размер em в ПИКСЕЛЯХ — то, чем меряет движок. Кегль в пунктах сюда
// передавать нельзя: на экране 96 dpi 11 пунктов это 15 пикселей, и формула
// вышла бы на треть мельче текста (я на этом и попался — увидел на первом же
// снимке, что математика мельче букв).
qreal textPixelSize() {
    QFont font(zametti::settings().style().fontFamily());
    font.setPointSizeF(zametti::settings().style().baseFontPoint());
    return QFontInfo(font).pixelSize();
}

void checkBaselineIsWhereItSays() {
    const qreal point = textPixelSize();

    // «x» — буква без нижнего выносного элемента: её чернила обязаны кончаться
    // на базовой линии. Допуск в полторы точки — это сглаживание и наклон
    // курсивной математической «x», а не запас на ошибку в разы.
    const zametti::FormulaImage x =
        zametti::Formulas::render(QStringLiteral("x"), false, point, Qt::black, 1.0);
    ZT_TRUE("формула «x» отрисовалась: " + x.error.toStdString(), x.ok());
    if (!x.ok()) return;
    const Ink xi = inkRows(x.image);
    ZT_TRUE("у «x» есть чернила", xi.top >= 0);
    ZT_TRUE("чернила «x» кончаются на базовой линии: низ " + std::to_string(xi.bottom) +
                ", базовая " + num(x.baseline),
            std::fabs(xi.bottom - x.baseline) <= 1.5);
    ZT_TRUE("и глубина под линией у «x» нулевая: " + num(x.depth), x.depth <= 0.5);

    // Дробь свисает под базовую линию — и это видно и в метрике, и в чернилах.
    const zametti::FormulaImage frac =
        zametti::Formulas::render(QStringLiteral("\\frac{a}{b}"), false, point, Qt::black, 1.0);
    ZT_TRUE("дробь отрисовалась: " + frac.error.toStdString(), frac.ok());
    if (!frac.ok()) return;
    const Ink fi = inkRows(frac.image);
    ZT_TRUE("глубина дроби положительна: " + num(frac.depth), frac.depth > 1.0);
    ZT_TRUE("чернила дроби уходят под базовую линию: низ " + std::to_string(fi.bottom) +
                ", базовая " + num(frac.baseline),
            fi.bottom > frac.baseline + 1.0);
    ZT_TRUE("и поднимаются над ней", fi.top < frac.baseline - 1.0);
    // Сходимость двух источников: глубина из метрик и глубина из чернил
    // обязаны говорить одно и то же. Именно эта проверка краснеет, если
    // спутать долю с пикселями.
    ZT_TRUE("метрика и чернила сходятся: глубина " + num(frac.depth) + " против " +
                num(fi.bottom - frac.baseline),
            std::fabs(frac.depth - (fi.bottom - frac.baseline)) <= 2.0);

    // У корня хвостик радикала уходит чуть ниже базовой линии — и движок об
    // этом честно говорит глубиной. Спрашиваем не «стоит на линии», а
    // СХОДИМОСТЬ ДВУХ ИСТОЧНИКОВ: чернила кончаются там, где обещали метрики.
    // Первая редакция проверки требовала «низ = базовая» и покраснела на
    // 2.2 точках — требовала она неправду.
    const zametti::FormulaImage root =
        zametti::Formulas::render(QStringLiteral("\\sqrt{x}"), false, point, Qt::black, 1.0);
    ZT_TRUE("корень отрисовался: " + root.error.toStdString(), root.ok());
    if (root.ok()) {
        const Ink ri = inkRows(root.image);
        ZT_TRUE("у корня чернила кончаются там, где обещает метрика: низ " +
                    std::to_string(ri.bottom) + ", базовая+глубина " +
                    num(root.baseline + root.depth),
                std::fabs(ri.bottom - (root.baseline + root.depth)) <= 2.0);
        ZT_TRUE("и свисает он немного: глубина " + num(root.depth) + " при высоте " +
                    num(root.height),
                root.depth < root.height * 0.25);
    }

    // Кегль работает: вдвое крупнее — вдвое выше, с точностью до округления.
    const zametti::FormulaImage twice =
        zametti::Formulas::render(QStringLiteral("\\frac{a}{b}"), false, point * 2, Qt::black, 1.0);
    ZT_TRUE("двойной кегль отрисовался", twice.ok());
    if (twice.ok())
        ZT_TRUE("высота выросла вдвое: " + num(twice.height) + " против " + num(frac.height),
                std::fabs(twice.height - frac.height * 2) < frac.height * 0.15);
}

// Плотность экрана: картинка растёт в физических точках, а логические размеры
// остаются теми же. Иначе на плотном экране формула вылезла бы вдвое крупнее.
void checkDeviceRatio() {
    const qreal point = textPixelSize();
    const zametti::FormulaImage one =
        zametti::Formulas::render(QStringLiteral("x^2"), false, point, Qt::black, 1.0);
    const zametti::FormulaImage two =
        zametti::Formulas::render(QStringLiteral("x^2"), false, point, Qt::black, 2.0);
    ZT_TRUE("обе отрисовались", one.ok() && two.ok());
    if (!one.ok() || !two.ok()) return;
    ZT_TRUE("растр вдвое крупнее: " + std::to_string(two.image.width()) + " против " +
                std::to_string(one.image.width()),
            std::abs(two.image.width() - one.image.width() * 2) <= 2);
    ZT_TRUE("а логическая ширина та же: " + num(two.width) + " против " + num(one.width),
            std::fabs(two.width - one.width) <= 1.0);
    // ПОМЕТКИ ПЛОТНОСТИ У КАРТИНКИ НЕТ, и это осознанно (см. formula.cpp): с
    // ней «логический» размер считает Qt, и рисование начинает зависеть от
    // того, какой формой drawImage её попросили нарисовать. Здесь картинка —
    // просто физические пиксели, а логические размеры отдаются числами рядом.
    ZT_TRUE("плотность картинке не проставлена",
            std::fabs(two.image.devicePixelRatio() - 1.0) < 0.01);
}

// ПРОБЕЛЫ ЮНИКОДА ВНУТРИ ФОРМУЛЫ — ОБЫЧНЫЕ ПРОБЕЛЫ.
//
// Заметка владельца пришла из Apple Notes, и отступы в ней — неразрывные
// пробелы (U+00A0). Движок рисует их настоящими пробелами: матрица разъезжается
// дырами между столбцами, вдвое шире, чем должна быть (376 против 199 точек на
// нашем кегле). KaTeX и MathJax, которыми эти заметки читают на стороне, такие
// пробелы просто игнорируют — и мы теперь тоже, но только в том, что уходит в
// движок: файл не меняется ни на байт.
void checkUnicodeSpaces() {
    const QString plain = QStringLiteral(
        "\\begin{matrix} a & b \\\\ c & d \\end{matrix}");
    QString exotic = plain;
    exotic.replace(QLatin1Char(' '), QChar(0x00A0));
    QString thin = plain;
    thin.replace(QLatin1Char(' '), QChar(0x2009));

    const zametti::FormulaImage a = zametti::Formulas::render(plain, true, 16.0, Qt::black, 1.0);
    const zametti::FormulaImage b = zametti::Formulas::render(exotic, true, 16.0, Qt::black, 1.0);
    const zametti::FormulaImage c = zametti::Formulas::render(thin, true, 16.0, Qt::black, 1.0);
    ZT_TRUE("все три отрисовались", a.ok() && b.ok() && c.ok());
    if (!a.ok() || !b.ok() || !c.ok()) return;
    ZT_TRUE("неразрывные пробелы ширину не меняют: " + num(b.width) + " против " + num(a.width),
            std::fabs(b.width - a.width) < 1.0);
    ZT_TRUE("тонкие пробелы тоже: " + num(c.width) + " против " + num(a.width),
            std::fabs(c.width - a.width) < 1.0);
}

// СОРАЗМЕРНОСТЬ ЭЙЛЕРА ТЕКСТУ. Разведка предупреждала: при одном кегле Euler
// компактнее прочих гарнитур — и на первом же снимке это видно глазом, формулы
// мельче соседних букв. Меряем отношение ростов строчных (x-height): по нему
// владелец и выставит formulas.inlineScale.
void measureXHeight() {
    const zametti::ZDocStyle& look = zametti::settings().style();
    QFont font(look.fontFamily());
    font.setPointSizeF(look.baseFontPoint());
    const QFontMetricsF metrics(font);

    // ЧЕМ НАБРАН ТЕКСТ НА ЛИСТЕ. Спрашиваем не настройку, а то, что Qt и
    // правда выбрала: не окажись гарнитуры среди загруженных, она молча
    // подставит другую, и лист приёмки будет врать о соразмерности.
    ZT_EQ("текст на листе набран заказанной гарнитурой", look.fontFamily().toStdString(),
          QFontInfo(font).family().toStdString());
    std::printf("гарнитуры: текст «%s», математика «%s»\n",
                QFontInfo(font).family().toUtf8().constData(),
                zametti::Formulas::mathFontName().toUtf8().constData());

    const zametti::FormulaImage x =
        zametti::Formulas::render(QStringLiteral("x"), false, textPixelSize(), Qt::black, 1.0);
    if (!x.ok()) return;
    const Ink ink = inkRows(x.image);
    const double mathX = double(ink.bottom - ink.top + 1);
    const double textX = metrics.xHeight();
    std::printf("рост строчных: текст %s, математика %s, отношение %s (в конфиге %s)\n",
                num(textX).c_str(), num(mathX).c_str(), num(textX / mathX).c_str(),
                num(zametti::settings().formulas().inlineScale()).c_str());
    // Коэффициент из конфига обязан и правда равнять рост строчных: если
    // однажды сменится гарнитура, эта проверка покраснеет первой.
    const zametti::FormulaImage scaled = zametti::Formulas::render(
        QStringLiteral("x"), false, textPixelSize() * zametti::settings().formulas().inlineScale(),
        Qt::black, 1.0);
    if (scaled.ok()) {
        const Ink si = inkRows(scaled.image);
        ZT_TRUE("с коэффициентом из конфига рост строчных сходится с текстом: " +
                    num(si.bottom - si.top + 1) + " против " + num(textX),
                std::fabs(double(si.bottom - si.top + 1) - textX) <= 1.0);
    }
    std::printf("  ширина «x»: текст %s, математика %s\n",
                num(metrics.horizontalAdvance(QStringLiteral("x"))).c_str(), num(x.width).c_str());
}

// --- 2. линейка на снимке --------------------------------------------------
//
// То, ради чего пробник и затевался: формулы, посаженные в настоящую строку
// текста тем же кеглем, и красная черта базовой линии ТЕКСТА поверх всего.
// Совпадение базовых линий видно глазом, а не выводится из чисел.
// Сравнение двух коэффициентов на одном листе: сверху формулы кегль в кегль с
// текстом (inlineScale = 1.00), снизу — подтянутые к росту строчных Plex
// (1.10). Одинаковая строка, одинаковый текст, две базовые линии: разницу
// видно, только когда они рядом.
void shootComparison(int zoom) {
    const zametti::ZDocStyle& look = zametti::settings().style();
    QFont font(look.fontFamily());
    font.setPointSizeF(look.baseFontPoint() * zoom);
    const QFontMetricsF metrics(font);
    QFont label(zametti::settings().ui().sidebarFontFamily());
    label.setPointSizeF(look.baseFontPoint() * zoom * 0.62);

    struct Piece {
        QString before;
        QString latex;
    };
    const Piece pieces[] = {
        {QStringLiteral("дробь "), QStringLiteral("\\frac{a}{b}")},
        {QStringLiteral(" степень "), QStringLiteral("x^2")},
        {QStringLiteral(" индекс "), QStringLiteral("a_{i+1}")},
        {QStringLiteral(" корень "), QStringLiteral("\\sqrt{x+1}")},
        {QStringLiteral(" и текст после."), QString()},
    };
    // Сравниваем единицу с тем, что стоит в конфиге: лист обязан показывать
    // то, чем программа и правда рисует, а не число, вписанное в набор.
    const double scales[] = {1.00, zametti::settings().formulas().inlineScale()};

    const int pad = 24;
    const qreal step = metrics.height() * 2.6;
    QImage sheet(620 * zoom, int(pad * 2 + step * 2 + metrics.height()), QImage::Format_RGB32);
    sheet.fill(QColor(0xfe, 0xfe, 0xfb));
    {
        QPainter painter(&sheet);
        painter.setRenderHint(QPainter::Antialiasing, true);

        int row = 0;
        for (const double scale : scales) {
            const qreal baseline = pad + metrics.ascent() + step * row + metrics.height() * 0.9;
            painter.setFont(label);
            painter.setPen(QColor(0x7a, 0x80, 0x88));
            painter.drawText(QPointF(pad, baseline - metrics.ascent() - 4),
                             QStringLiteral("inlineScale = %1").arg(scale, 0, 'f', 2));

            painter.setFont(font);
            painter.setPen(QColor(0x1a, 0x1a, 0x1a));
            qreal x = pad;
            for (const Piece& piece : pieces) {
                painter.drawText(QPointF(x, baseline), piece.before);
                x += metrics.horizontalAdvance(piece.before);
                if (piece.latex.isEmpty()) continue;
                const zametti::FormulaImage formula = zametti::Formulas::render(
                    piece.latex, false, QFontInfo(font).pixelSize() * scale,
                    QColor(0x1a, 0x1a, 0x1a), 1.0);
                if (!formula.ok()) continue;
                painter.drawImage(QPointF(x, baseline - formula.baseline), formula.image);
                x += formula.width;
            }

            QPen pen(QColor(220, 60, 60, 130));
            painter.setPen(pen);
            painter.drawLine(QPointF(0, baseline), QPointF(sheet.width(), baseline));
            pen.setColor(QColor(60, 120, 220, 80));
            painter.setPen(pen);
            painter.drawLine(QPointF(0, baseline - metrics.xHeight()),
                             QPointF(sheet.width(), baseline - metrics.xHeight()));
            ++row;
        }
    }
    const QString path = QDir(g_shots).filePath(QStringLiteral("сравнение-масштабов-x%1.png").arg(zoom));
    if (!sheet.save(path)) std::printf("НЕ СОХРАНИЛСЯ снимок %s\n", qPrintable(path));
}

void shootRuler(int zoom) {
    const zametti::ZDocStyle& look = zametti::settings().style();
    QFont font(look.fontFamily());
    // Второй лист рисуется втрое крупнее — и именно РИСУЕТСЯ, а не растягивается:
    // формула векторная, и увеличенная растяжкой она бы мылила ровно там, где
    // её и надо разглядывать.
    font.setPointSizeF(look.baseFontPoint() * zoom);
    const QFontMetricsF metrics(font);

    struct Piece {
        QString before;
        QString latex;
    };
    const Piece pieces[] = {
        {QStringLiteral("дробь "), QStringLiteral("\\frac{a}{b}")},
        {QStringLiteral(" степень "), QStringLiteral("x^2")},
        {QStringLiteral(" индекс "), QStringLiteral("a_{i+1}")},
        {QStringLiteral(" корень "), QStringLiteral("\\sqrt{x+1}")},
        {QStringLiteral(" сумма "), QStringLiteral("\\sum_{i=1}^{n} i")},
        {QStringLiteral(" и текст после."), QString()},
    };

    const int pad = 24;
    const qreal baseline = pad + metrics.ascent() + 18;
    // Ширина листа растёт вместе с кеглем: строка та же самая, просто крупнее.
    // Первая редакция брала 340 на единицу зума, и лист ×3 обрезался на слове
    // «корень» — увидел это только глазами на снимке.
    QImage sheet(700 * zoom, int(baseline + metrics.descent() + 48), QImage::Format_RGB32);
    sheet.fill(QColor(0xfe, 0xfe, 0xfb));
    {
        QPainter painter(&sheet);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setFont(font);
        painter.setPen(QColor(0x1a, 0x1a, 0x1a));

        qreal x = pad;
        for (const Piece& piece : pieces) {
            painter.drawText(QPointF(x, baseline), piece.before);
            x += metrics.horizontalAdvance(piece.before);
            if (piece.latex.isEmpty()) continue;
            const zametti::FormulaImage formula = zametti::Formulas::render(
                piece.latex, false,
                QFontInfo(font).pixelSize() * zametti::settings().formulas().inlineScale(),
                QColor(0x1a, 0x1a, 0x1a), 1.0);
            if (!formula.ok()) continue;
            // ВОТ ОНА, ПОСАДКА: верх картинки = базовая линия текста минус
            // подъём формулы над своей базовой линией.
            painter.drawImage(QPointF(x, baseline - formula.baseline), formula.image);
            x += formula.width;
        }

        // Линейка: базовая линия текста через весь лист.
        QPen pen(QColor(220, 60, 60, 150));
        pen.setWidthF(1.0);
        painter.setPen(pen);
        painter.drawLine(QPointF(0, baseline), QPointF(sheet.width(), baseline));
        // И вторая черта — линия x-height: по ней видно, соразмерна ли
        // математика тексту (у Эйлера свой рост строчных).
        pen.setColor(QColor(60, 120, 220, 90));
        painter.setPen(pen);
        painter.drawLine(QPointF(0, baseline - metrics.xHeight()),
                         QPointF(sheet.width(), baseline - metrics.xHeight()));
    }
    const QString path = QDir(g_shots).filePath(
        zoom == 1 ? QStringLiteral("базовая-линия.png")
                  : QStringLiteral("базовая-линия-x%1.png").arg(zoom));
    if (!sheet.save(path)) std::printf("НЕ СОХРАНИЛСЯ снимок %s\n", qPrintable(path));
}

}  // namespace

int ztFormulaProbe(int argc, char** argv) {
    g_shots = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QDir::tempPath();
    QDir().mkpath(g_shots);
    zametti::loadEmbeddedFonts();

    QString error;
    ZT_TRUE("движок формул поднялся: " + error.toStdString(),
            zametti::Formulas::init(&error));
    if (!zametti::Formulas::ready()) return zt::report("формулы: базовая линия");
    ZT_TRUE("математическая гарнитура названа: " +
                zametti::Formulas::mathFontName().toStdString(),
            !zametti::Formulas::mathFontName().isEmpty());

    checkUnicodeSpaces();
    checkBaselineIsWhereItSays();
    checkDeviceRatio();
    measureXHeight();
    shootRuler(1);
    shootRuler(3);
    shootComparison(3);

    std::printf("снимки: %s\n", qPrintable(g_shots));
    return zt::report("формулы: базовая линия");
}
