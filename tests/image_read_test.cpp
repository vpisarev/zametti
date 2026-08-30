// Своя дверь чтения картинок против чужого судьи.
//
// Набор появился вместе с отказом от реестра плагинов Qt: свои форматы (jxl,
// jpeg, tiff, webp, heif) мы читаем сами. Пока плагины Qt в системе есть, они
// служат СУДЬЁЙ — независимой реализацией, с которой сверяются и размеры, и
// пиксели. Когда судьи нет (плагин не собран), проверка честно печатает, что
// пропустила, и не притворяется зелёной.
//
// Проверяются ровно те четыре свойства, которые давал QImageReader сверх
// декода. Каждое из них потеряли бы молча, а увидел бы владелец:
//
//   1. размеры ПО ЗАГОЛОВКУ, без разжатия — по ним считается место под
//      фотографию, до первой отрисовки;
//   2. разжатие СРАЗУ в нужный размер — у JPEG восьмые доли;
//   3. поворот из заголовка;
//   4. предохранитель по памяти — отказ ДО разжатия, а не после.

#include "image_read.h"
#include "jpeg_read.h"
#include "tiff_reader.h"

#include "test_util.h"

#include <vector>
#include "testdata.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QImageReader>

#include <algorithm>
#include <cstdio>

namespace {

using namespace zametti;

struct Diff {
    double mean = 0;      // среднее расхождение канала, в единицах из 255
    int worst = 0;        // худшее по одному отсчёту
    bool sameSize = true;
};

// Среднее расхождение по всем каналам. Именно среднее, а не худшее: у двух
// верных декодеров одного JPEG отдельные отсчёты на резких границах расходятся
// на десятки (так устроено обратное преобразование), а среднее остаётся
// меньше двух. Грубая же беда — переставленные каналы, сдвиг строк, мусор —
// поднимает именно среднее, и сразу до десятков.
Diff compare(const QImage& a, const QImage& b) {
    Diff d;
    if (a.size() != b.size()) { d.sameSize = false; return d; }
    const QImage x = a.convertToFormat(QImage::Format_RGB888);
    const QImage y = b.convertToFormat(QImage::Format_RGB888);
    long long sum = 0;
    long long count = 0;
    for (int row = 0; row < x.height(); ++row) {
        const uchar* p = x.constScanLine(row);
        const uchar* q = y.constScanLine(row);
        for (int i = 0; i < x.width() * 3; ++i) {
            const int delta = std::abs(int(p[i]) - int(q[i]));
            d.worst = std::max(d.worst, delta);
            sum += delta;
            ++count;
        }
    }
    if (count) d.mean = double(sum) / double(count);
    return d;
}

// Судья ЗАВЕДОМО НЕПРАВ там, где мы САМИ перевели пространство.
//
// Qt читает CIELab и CMYK из TIFF наивно — ровно поэтому у нас своя libtiff и
// свой цветовой путь (этап 8: «Qt тихо перевирает CIELab», и картины Эрмитажа
// легли в хранилище перекрашенными, пока это не починили). Сверять наши
// пиксели с его пикселями на таких файлах бессмысленно: расхождение здесь
// признак того, что мы работаем, а не того, что мы сломались. За правильность
// цвета отвечает color_test — у него есть настоящие профили печати.
//
// Спрашиваем не по догадке, а у самого читателя: он выставляет converted,
// когда пространство переводили мы.
bool weConvertedColour(const QString& path, const ImageProbe& probe) {
    if (probe.format != QLatin1String("tiff")) return false;
    TiffImage tiff;
    QString error;
    if (!readTiff(path, &tiff, &error, 0)) return false;
    return tiff.converted;   // читаем целиком НАРОЧНО: это набор, не горячий путь
}

// Может ли Qt здесь прочесть ЭТОТ ФАЙЛ. Без судьи сверять нечего.
//
// СПРАШИВАЕМ ПРО ФАЙЛ, А НЕ ПРО ИМЯ ФОРМАТА, и это не придирка. Прежняя
// редакция сверяла имя контейнера со списком supportedImageFormats() и на
// «heif» отвечала «знает», если судья умеет hеic ИЛИ avif. Но AVIF и HEIC —
// ОДИН контейнер и РАЗНЫЕ кодеки (AV1 против HEVC), и судья, собранный с
// libaom, но без AV1-декодера, знает один и не знает другой. Здесь, на маке с
// qtimageformats из brew, список судьи содержит heic и heif и НЕ содержит avif:
// тринадцать файлов корпуса объявлялись «судья знает», судья возвращал пустоту,
// и набор краснел на сверке «прочли то же, что судья», хотя не сломано ничего.
//
// canRead() открывает файл и спрашивает плагины про его содержимое — то есть
// отвечает на тот вопрос, который нам и нужен, и не требует от нас знать
// расклад кодеков в чужой сборке.
bool judgeCanRead(const QString& path) {
    return QImageReader(path).canRead();
}

// Судья и мы декодируем РАЗНЫМ КОДОМ — или одним и тем же?
//
// Там, где код общий, расходиться негде и сверка требует точного совпадения.
// Там, где он разный, два верных декодера законно дают мелкий шум, и требовать
// нуля значило бы требовать невозможного:
//
//   jpeg  — у нас jpegli, у судьи libjpeg-turbo; стандарт JPEG САМ разрешает
//           соответствующим декодерам расходиться в обратном преобразовании;
//   heif  — у нас вендоренная libheif с libde265/libgav1, у судьи её же
//           системная сборка с другим декодером.
bool judgeDecodesDifferently(const QString& format) {
    return format == QLatin1String("jpeg") || format == QLatin1String("heif");
}

// СЫРЬЁ В ОБОЛОЧКЕ TIFF СУДЬЕ НЕ ПО ЗУБАМ, и дело не в качестве декодера.
// В DNG лежит НЕСКОЛЬКО картинок: сырой кадр с матрицы и уменьшенный
// предпросмотр. Какую из них отдать — решает читатель, и решают они по-разному:
// на olympus.dng судья отдаёт предпросмотр 192×256, мы — полный кадр; на
// proraw.dng кадры разные настолько, что среднее расхождение 52.8. Сверять
// тут нечего — это не «мы разошлись», а «мы читаем разные картинки».
//
// Своя проверка у сырья при этом остаётся: размер из шапки против размера
// разжатого (выше) и вся работа читателя ниже — они от судьи не зависят.
bool judgeIsUselessHere(const QString& path) {
    return QFileInfo(path).suffix().compare(QLatin1String("dng"), Qt::CaseInsensitive) == 0;
}

int files = 0;
int judged = 0;

void checkOne(const QString& path) {
    const QString name = QFileInfo(path).fileName();
    const ImageProbe probe = probeImageFile(path);
    if (!probe.valid()) return;   // не картинка либо шапка битая — не наше дело
    ++files;

    // ЧИТАЕМ ТАК, КАК ЧИТАЕТ ВВОЗ, И СВЕРЯЕМ ИМЕННО ЕГО ОБЕЩАНИЕ.
    // Правило владельца записано в docs/zametti-image-rules.md: при ввозе
    // пиксели уже повёрнуты, а Orientation сбрасывается в 1 — оставшийся тег
    // повернул бы их второй раз. Значит сверять надо ВЫПРЯМЛЕННЫЕ картинки с
    // обеих сторон; показу поворот не нужен (наши вложения и так стоят прямо),
    // но здесь корпус — ЧУЖИЕ файлы, то есть ровно случай ввоза.
    DecodeRequest upright;
    upright.applyOrientation = true;
    const QImage ours = decodeImageFile(path, upright);
    // ПОВОРОТ ПРИМЕНЯЮТ ОБА, ИНАЧЕ СРАВНИВАЮТСЯ РАЗНЫЕ ВЕЩИ. У QImageReader
    // автоповорот по умолчанию ВЫКЛЮЧЕН, а наш читатель ориентацию применяет
    // всегда: снимок с ориентацией 6 мы отдаём 3024×4032, судья — 4032×3024, и
    // набор краснел на «тот же размер, что у судьи», хотя обе картинки верны.
    // Измерено на heic-поворот6-p3.heic: с setAutoTransform судья даёт ровно
    // наш размер.
    QImageReader reader(path);
    reader.setAutoTransform(true);
    const QImage theirs = reader.read();
    const bool judgeReads = judgeCanRead(path);

    // ОБЕ СТОРОНЫ ПРИВОДЯТСЯ К ОДНОМУ СОГЛАШЕНИЮ О ПОВОРОТЕ, и это не
    // придирка: у нас оно РАЗНОЕ по форматам, и не по недосмотру. В HEIF поворот
    // лежит в самом контейнере (бокс irot), и libheif применяет его внутри
    // декодера — картинка выходит уже повёрнутой. В JPEG поворот — это метка
    // EXIF рядом с пикселями, декодер её не трогает, и применяет её вызывающий
    // (applyOrientation, у неё свой набор ниже). Судья же после
    // setAutoTransform поворачивает ВСЕГДА.
    //
    // Отсюда правило: если наша картинка стоит поперёк судейской, а файл
    // объявляет поворот — доворачиваем нашу. Определяется по размерам, а не
    // по формату: список форматов пришлось бы дописывать при каждом новом
    // читателе, а размеры говорят сами.
    //
    // ЧЕГО ЭТО НЕ ЛОВИТ: зеркал и поворота на 180 — у них размер не меняется, и
    // отличить «уже применено» от «ещё нет» нечем. В корпусе таких файлов нет
    // (есть только ориентация 6 у jpeg, heic и avif); появятся — понадобится
    // спрашивать сам читатель, применил ли он поворот.
    QImage mine = ours;
    if (probe.orientation != Orientation::Normal && !ours.isNull() && !theirs.isNull() &&
        ours.size() == theirs.size().transposed())
        mine = applyOrientation(mine, probe.orientation);

    // БОМБЫ ЧИТАТЬ НЕ ПОЛОЖЕНО. В корпусе лежат заведомые бомбы разжатия —
    // файлы в семьдесят байт, объявляющие миллиарды пикселей. Отказ на них не
    // беда, а работа предохранителя; требовать «прочли» значило бы требовать
    // сломать его.
    const bool bomb = ours.isNull() && theirs.isNull();
    if (bomb) return;

    // СВЕРЯТЬ «ПРОЧЛИ ИЛИ НЕТ» МОЖНО ТОЛЬКО ТАМ, ГДЕ СУДЬЯ ЗНАЕТ ФОРМАТ.
    // После отказа от плагинов Qt перестал читать jxl вовсе — и это не беда, а
    // ровно то, ради чего мы завели свой читатель: формат хранения заметок
    // больше не зависит от чужой сборки.
    if (judgeReads) {
        ZT_TRUE(("прочли то же, что судья: " + name).toStdString(),
                ours.isNull() == theirs.isNull());
    } else {
        ZT_TRUE(("прочли то, чего судья не умеет: " + name + " [" + probe.format + "]")
                    .toStdString(),
                !ours.isNull());
    }
    if (ours.isNull()) return;

    // 1. Размер из шапки обязан совпасть с размером разжатого. Это и есть
    // проверка «место под фотографию посчитано по настоящим размерам».
    //
    // У ПОВЁРНУТОГО СНИМКА ЖДЁМ ПОВЁРНУТЫЙ РАЗМЕР: мы читаем как ввоз, то есть
    // выпрямляем, и настоящий размер после выпрямления — переставленный.
    //
    // ✗ ДОЛГ, НАЙДЕННЫЙ ЗДЕСЬ И НЕ ЗАКРЫТЫЙ. Шапка отвечает про поворот
    // ПО-РАЗНОМУ в зависимости от формата: у HEIF поворот лежит в контейнере, и
    // libheif отдаёт probe.size уже переставленным; у JPEG поворот — метка
    // EXIF рядом с пикселями, и probe.size приходит как в файле. То есть
    // «сколько места занять под эту фотографию» два формата отвечают в разных
    // соглашениях, и место под чужой jpeg с поворотом будет зарезервировано
    // поперёк. Здесь это обойдено сравнением с оглядкой на probe.orientation;
    // чинить надо в самом probeImage — отдельной работой.
    const bool turned = probe.orientation == Orientation::Rotate90 ||
                        probe.orientation == Orientation::Rotate270 ||
                        probe.orientation == Orientation::Transpose ||
                        probe.orientation == Orientation::AntiTranspose;
    const QSize expected = turned && probe.size == ours.size().transposed()
                               ? probe.size.transposed()
                               : probe.size;
    ZT_EQ(("размер из шапки: " + name).toStdString(),
          QStringLiteral("%1x%2").arg(expected.width()).arg(expected.height()).toStdString(),
          QStringLiteral("%1x%2").arg(ours.width()).arg(ours.height()).toStdString());

    if (!judgeReads) return;
    if (theirs.isNull()) return;   // судья сам не смог — сверять нечего
    if (weConvertedColour(path, probe)) {
        std::printf("СУДЬЯ НЕ В СЧЁТ: %s — пространство переводили мы, у Qt оно наивное\n",
                    qPrintable(name));
        return;
    }
    ++judged;

    if (judgeIsUselessHere(path)) return;

    const Diff d = compare(mine, theirs);
    ZT_TRUE(("тот же размер, что у судьи: " + name).toStdString(), d.sameSize);
    if (!d.sameSize) return;

    // НА ЧТО ВООБЩЕ ГОДЕН ЧУЖОЙ СУДЬЯ — вопрос, на котором я ошибся дважды, и
    // оба раза потому, что ставил порог раньше замера.
    //
    // Сперва ждал, что яркость сойдётся точно: мол, обратное преобразование у
    // обоих одно. Неправда — у jpegli своё, и деквантование своё.
    // Потом взял SSIMULACRA2 и написал «замер показал, все выше 90», не сделав
    // замера. Замер показал 84–89 РОВНО У ВСЕХ, включая файл без прореживания
    // цветности. Причина не в нас: SSIMULACRA2 откалибрована на вопрос
    // «оригинал против сжатого» и штрафует ±1 младший разряд по всему кадру, а
    // два разных верных декодера ровно такой шум и дают.
    //
    // Правда в том, что СТАНДАРТ JPEG САМ РАЗРЕШАЕТ соответствующим декодерам
    // расходиться в обратном преобразовании. Значит сверка с чужим декодером
    // может отвечать только на вопрос «не сломались ли мы ГРУБО» — переставлены
    // каналы, сдвинуты строки, перевран цвет, мусор вместо картинки. Все эти
    // беды дают среднее расхождение в десятки; верный-но-иной декодер даёт
    // меньше двух (замер на корпусе: 0.41…2.06). Порог четыре — вдвое выше
    // худшего верного случая и втрое ниже самой мелкой грубой беды.
    //
    // Обещать по этой сверке больше — врать себе.
    //
    // ПОРОГ ЧЕТЫРЕ ДЕЙСТВУЕТ И ДЛЯ HEIF/AVIF — решение владельца 30.08.2026, и
    // по той же причине, что у jpeg: код у нас и у судьи РАЗНЫЙ. Мы читаем
    // вендоренной libheif с libde265/libgav1, судья — её системной сборкой с
    // другим декодером. Прежде здесь требовался ровно ноль по доводу «у
    // остальных форматов декодер один и тот же код», и на маке это оказалось
    // неправдой: девять файлов heic/heif/avif дали 0.35…0.73 — то есть ровно
    // тот мелкий шум, который два верных декодера и обязаны давать, и втрое
    // ниже даже самой мелкой грубой беды.
    if (!judgeDecodesDifferently(probe.format)) {
        // Где код общий (webp, png, tiff), расходиться негде.
        ZT_TRUE(("пиксели сошлись с судьёй: " + name + " (" +
                 QString::number(d.mean, 'f', 2) + ")").toStdString(),
                d.mean == 0.0);
        return;
    }
    ZT_TRUE(("нет грубого расхождения с судьёй: " + name + " (сред " +
             QString::number(d.mean, 'f', 2) + ")").toStdString(),
            d.mean <= 4.0);

    // А ВОТ ЭТА ПРОВЕРКА ОТ СУДЬИ НЕ ЗАВИСИТ ВОВСЕ, и она строже.
    //
    // Два наших пути — восьмибитный и шестнадцатибитный — идут через ОДИН
    // декодер и обязаны давать одну картинку с точностью до округления. Здесь
    // ловится именно моя работа: перекладка строк, формат QImage, шаг строки,
    // порядок каналов. Ошибку в них чужой судья простил бы (среднее осталось бы
    // мелким), а эта сверка — нет.
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return;
    const QByteArray bytes = file.readAll();
    // УМЕНЬШЕННОЕ разжатие, и это не экономия на проверке. Ловится здесь моя
    // работа — перекладка строк, формат QImage, шаг строки, порядок каналов, —
    // а она от размера картинки не зависит ВОВСЕ. Полный размер стоил на
    // шестидесятимегапиксельном снимке по секунде за разжатие, и набор
    // разбухал до трети всего прогона.
    const QSize small(800, 800);
    const QImage eight = decodeJpeg(bytes, small, JpegDepth::Eight);
    const QImage deep = decodeJpeg(bytes, small, JpegDepth::Sixteen);
    if (eight.isNull() || deep.isNull()) return;
    const Diff own = compare(eight, deep);
    ZT_TRUE(("восемь бит и шестнадцать дают одно: " + name + " (" +
             QString::number(own.mean, 'f', 3) + ")").toStdString(),
            own.sameSize && own.mean <= 1.0);
}

void walk(const QString& path) {
    const QFileInfo info(path);
    if (info.isDir()) {
        QDir dir(path);
        for (const QString& n : dir.entryList(QDir::Files, QDir::Name)) checkOne(dir.filePath(n));
        for (const QString& n : dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name))
            walk(dir.filePath(n));
    } else if (info.isFile()) {
        checkOne(path);
    }
}

// 2. Уменьшение при разжатии. У JPEG размер идёт восьмыми долями, поэтому
// требуем «не меньше просимого», а не точное равенство: точную подгонку делает
// вызывающий своей лестницей.
void checkScaledDecode(const QString& dir) {
    QDir d(dir);
    const auto names = d.entryList({QStringLiteral("*.jpg"), QStringLiteral("*.jpeg")},
                                   QDir::Files, QDir::Name);
    if (names.isEmpty()) {
        std::printf("ПРОПУЩЕНО: jpeg для проверки уменьшения не нашлось в %s\n",
                    qPrintable(dir));
        return;
    }
    int done = 0;
    for (const QString& name : names) {
        QFile file(d.filePath(name));
        if (!file.open(QIODevice::ReadOnly)) continue;
        const QByteArray bytes = file.readAll();
        const JpegHeader head = readJpegHeader(bytes);
        if (!head.valid() || head.size.width() < 1600) continue;

        const QImage small = decodeJpeg(bytes, QSize(800, 800), JpegDepth::Eight);
        ZT_TRUE(("уменьшили: " + name).toStdString(), !small.isNull());
        if (small.isNull()) continue;
        // Не ниже просимого ни по одной стороне — иначе картинка стала бы
        // мылом, а место под неё считалось бы по другим числам.
        ZT_TRUE(("не мельче просимого: " + name).toStdString(),
                small.width() >= 800 || small.height() >= 800);
        // И честно мельче исходного: иначе восьмые доли не сработали вовсе.
        ZT_TRUE(("вправду уменьшили: " + name).toStdString(), small.width() < head.size.width());
        if (++done >= 3) break;
    }
    if (done == 0) std::printf("ПРОПУЩЕНО: подходящих по размеру jpeg не нашлось\n");
}

// 3. Поворот. Проверяется на своей картинке, без корпуса: правило поворота
// одно на все форматы, и врать ему негде.
void checkOrientation() {
    QImage src(4, 2, QImage::Format_RGB888);
    src.fill(Qt::black);
    src.setPixelColor(0, 0, Qt::red);

    const QImage same = applyOrientation(src, Orientation::Normal);
    ZT_EQ("без поворота размер не менялся", std::string("4x2"),
          QStringLiteral("%1x%2").arg(same.width()).arg(same.height()).toStdString());

    const QImage turned = applyOrientation(src, Orientation::Rotate90);
    ZT_EQ("поворот на 90 меняет стороны местами", std::string("2x4"),
          QStringLiteral("%1x%2").arg(turned.width()).arg(turned.height()).toStdString());

    const QImage flipped = applyOrientation(src, Orientation::FlipHorizontal);
    ZT_EQ("отражение оставляет размер", std::string("4x2"),
          QStringLiteral("%1x%2").arg(flipped.width()).arg(flipped.height()).toStdString());
    ZT_TRUE("отражение перенесло угол направо",
            flipped.pixelColor(3, 0) == QColor(Qt::red));
}

// 4. Предохранитель по памяти. Отказ обязан прийти ДО разжатия: узнав об этом
// после, мы бы заплатили памятью ровно за то, чего решили не показывать.
void checkMemoryGuard(const QString& dir) {
    QDir d(dir);
    const auto names = d.entryList(QDir::Files, QDir::Name);
    QString big;
    QSize bigSize;
    for (const QString& name : names) {
        const ImageProbe probe = probeImageFile(d.filePath(name));
        if (probe.valid() && probe.decodedBytes() > bigSize.width() * qint64(bigSize.height())) {
            big = d.filePath(name);
            bigSize = probe.size;
        }
    }
    if (big.isEmpty()) {
        std::printf("ПРОПУЩЕНО: картинок для проверки предохранителя не нашлось в %s\n",
                    qPrintable(dir));
        return;
    }
    const ImageProbe probe = probeImageFile(big);
    DecodeRequest tight;
    tight.memoryLimitBytes = probe.decodedBytes() - 1;   // на байт меньше нужного
    ZT_TRUE("потолок на байт ниже нужного отказывает",
            decodeImageFile(big, tight).isNull());

    DecodeRequest roomy;
    roomy.memoryLimitBytes = probe.decodedBytes();
    ZT_TRUE("потолок ровно по размеру пропускает", !decodeImageFile(big, roomy).isNull());
}

}  // namespace

static int ztRunSuite(int argc, char** argv) {
    // Тот же потолок, что у остальных наборов картинок и у самой программы
    // (settings.cpp ставит его из бюджета конфига). Без этого корпусные
    // стомегапиксельные снимки отвергались бы предохранителем, и набор
    // жаловался бы на предохранитель вместо чтения.
    QImageReader::setAllocationLimit(2048);

    const QString root = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QString();
    if (root.isEmpty() || !QFileInfo::exists(root)) {
        std::printf("ПРОПУЩЕНО: корпуса картинок нет рядом (%s)\n",
                    root.isEmpty() ? "путь не задан" : qPrintable(root));
    } else {
        walk(root);
        std::printf("файлов прочитано: %d, из них сверено с судьёй: %d\n", files, judged);
        if (judged == 0)
            std::printf("ПРОПУЩЕНО: плагинов Qt для сверки нет — судьи не было\n");
        checkScaledDecode(root + QStringLiteral("/iphone"));
        checkMemoryGuard(root + QStringLiteral("/formats"));
    }

    checkOrientation();
    return zt::report("image-read");
}

// Набор целиком одним TEST: тело не тронуто, argv ему собран здесь.
// Дробить на отдельные проверки — отдельная работа, по одному набору.
TEST(ImageRead, All) {
    std::vector<QByteArray> ztArgs{QByteArrayLiteral("image_read_test")};
    ztArgs.push_back((zt::TestData::corpus(QStringLiteral("images/originals"))).toLocal8Bit());
    std::vector<char*> ztArgv;
    for (QByteArray& a : ztArgs) ztArgv.push_back(a.data());
    EXPECT_EQ(0, ztRunSuite(int(ztArgv.size()), ztArgv.data()));
}
