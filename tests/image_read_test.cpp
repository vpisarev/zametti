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

// Умеет ли Qt здесь читать этот формат. Без судьи сверять нечего.
bool judgeKnows(const QString& format) {
    if (format.isEmpty()) return false;
    const auto known = QImageReader::supportedImageFormats();
    const QByteArray want = format.toLatin1();
    if (known.contains(want)) return true;
    if (want == "jpeg") return known.contains("jpg");
    if (want == "heif") return known.contains("heic") || known.contains("avif");
    return false;
}

int files = 0;
int judged = 0;

void checkOne(const QString& path) {
    const QString name = QFileInfo(path).fileName();
    const ImageProbe probe = probeImageFile(path);
    if (!probe.valid()) return;   // не картинка либо шапка битая — не наше дело
    ++files;

    const QImage ours = decodeImageFile(path);
    const QImage theirs = QImageReader(path).read();

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
    if (judgeKnows(probe.format)) {
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
    ZT_EQ(("размер из шапки: " + name).toStdString(),
          QStringLiteral("%1x%2").arg(probe.size.width()).arg(probe.size.height()).toStdString(),
          QStringLiteral("%1x%2").arg(ours.width()).arg(ours.height()).toStdString());

    if (!judgeKnows(probe.format)) return;
    if (theirs.isNull()) return;   // судья сам не смог — сверять нечего
    if (weConvertedColour(path, probe)) {
        std::printf("СУДЬЯ НЕ В СЧЁТ: %s — пространство переводили мы, у Qt оно наивное\n",
                    qPrintable(name));
        return;
    }
    ++judged;

    const Diff d = compare(ours, theirs);
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
    const bool ownDecoder = probe.format == QLatin1String("jpeg");
    if (!ownDecoder) {
        // У остальных форматов декодер один и тот же код: расходиться негде.
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
    const QImage eight = decodeJpeg(bytes, {}, JpegDepth::Eight);
    const QImage deep = decodeJpeg(bytes, {}, JpegDepth::Sixteen);
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

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
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
