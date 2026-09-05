// Сборщик .ico из нашей png-иконки. Запускается РУКАМИ, как compress.sh рядом:
// результат (zametti.ico) лежит в репозитории готовым, а не собирается каждой
// сборкой. Причина та же, что у zametti1024x1024.png — кросс-сборке под Windows
// негде взять хостовую программу, а тащить в круг сборки ImageMagick ради
// одного файла мы не станем.
//
//   c++ -std=c++17 -fPIC $(pkg-config --cflags --libs Qt6Gui) \
//       -o /tmp/make-ico resources/make-ico.cpp
//   /tmp/make-ico resources/zametti_alt_lossless_512.png resources/zametti_alt.ico
//
// ЗАЧЕМ ВООБЩЕ. QGuiApplication::setWindowIcon() красит заголовок окна и панель
// задач на ИСПОЛНЕНИИ, а значок самого .exe в проводнике, на рабочем столе и в
// диалоге «Открыть с помощью» Windows берёт совсем не оттуда — из ресурса
// RT_GROUP_ICON внутри файла. Без него программа выглядит безликой ровно там,
// где человек её впервые и видит.
//
// ПОЧЕМУ НЕ QImageWriter("ico"). Qt писать .ico умеет (qicohandler.cpp), но
// одним изображением: у QImageWriter нет понятия «несколько картинок в файле».
// А значок обязан нести НЕСКОЛЬКО размеров — Windows берёт из набора точный, а
// не жмёт большой: 512 → 16 системным ужатием превращается в кашу. Поэтому
// контейнер собираем сами, благо он простой.
//
// ФОРМАТ. ICONDIR (6 байт) + по ICONDIRENTRY (16 байт) на размер + сами данные.
// Данные бывают двух родов, и оба нужны:
//   * 256×256 — PNG как есть (понимает Windows Vista и новее, а старее нам и
//     не нужно: планка сборки — Windows 10);
//   * всё, что меньше, — BMP без заголовка файла, с ДВОЙНОЙ высотой в
//     BITMAPINFOHEADER и маской прозрачности следом. Двойная высота — не
//     опечатка: так формат отличает «картинка + маска» от просто картинки, и
//     проводник на этом спотыкается первым, если написать обычную.

#include <QBuffer>
#include <QFile>
#include <QGuiApplication>
#include <QImage>

#include <cstdio>
#include <cstring>
#include <vector>

namespace {

// Размеры, которые Windows спрашивает на самом деле: 16 — список в проводнике и
// заголовок окна, 32 — рабочий стол и Alt+Tab, 48 — «крупные значки»,
// 256 — «огромные» и предпросмотр. 64 и 128 идут между ними, чтобы системе не
// приходилось жать через два шага.
constexpr int kSizes[] = {16, 24, 32, 48, 64, 128, 256};

void put16(std::vector<char>& out, quint16 v) {
    out.push_back(char(v & 0xFF));
    out.push_back(char((v >> 8) & 0xFF));
}

void put32(std::vector<char>& out, quint32 v) {
    for (int i = 0; i < 4; ++i) out.push_back(char((v >> (8 * i)) & 0xFF));
}

// PNG-байты картинки — ровно то, что уйдёт внутрь .ico для 256×256.
QByteArray pngBytes(const QImage& image) {
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return bytes;
}

// BMP-представление с маской: заголовок BITMAPINFOHEADER, потом строки цвета
// СНИЗУ ВВЕРХ, потом однобитная маска (тоже снизу вверх, строки выровнены на 4
// байта). Маску Windows на 32-битных значках не использует для прозрачности —
// её берут из альфы, — но пропустить её нельзя: размер данных считается с ней,
// и без неё проводник читает мусор.
QByteArray bmpBytes(const QImage& image) {
    const int w = image.width();
    const int h = image.height();
    std::vector<char> out;

    put32(out, 40);                 // biSize
    put32(out, quint32(w));         // biWidth
    put32(out, quint32(h * 2));     // biHeight — ДВОЙНАЯ: цвет + маска
    put16(out, 1);                  // biPlanes
    put16(out, 32);                 // biBitCount
    put32(out, 0);                  // biCompression = BI_RGB
    put32(out, quint32(w * h * 4)); // biSizeImage
    put32(out, 0);                  // biXPelsPerMeter
    put32(out, 0);                  // biYPelsPerMeter
    put32(out, 0);                  // biClrUsed
    put32(out, 0);                  // biClrImportant

    // Цвет: BGRA, снизу вверх.
    for (int y = h - 1; y >= 0; --y) {
        const QRgb* row = reinterpret_cast<const QRgb*>(image.constScanLine(y));
        for (int x = 0; x < w; ++x) {
            const QRgb p = row[x];
            out.push_back(char(qBlue(p)));
            out.push_back(char(qGreen(p)));
            out.push_back(char(qRed(p)));
            out.push_back(char(qAlpha(p)));
        }
    }

    // Маска: бит на точку, единица — «прозрачно». Строки выровнены на 4 байта.
    const int maskStride = ((w + 31) / 32) * 4;
    for (int y = h - 1; y >= 0; --y) {
        const QRgb* row = reinterpret_cast<const QRgb*>(image.constScanLine(y));
        std::vector<unsigned char> line(size_t(maskStride), 0);
        for (int x = 0; x < w; ++x)
            if (qAlpha(row[x]) == 0) line[size_t(x / 8)] |= (0x80 >> (x % 8));
        out.insert(out.end(), line.begin(), line.end());
    }

    return QByteArray(out.data(), qsizetype(out.size()));
}

}  // namespace

int main(int argc, char** argv) {
    // Платформа offscreen: программа рисует не на экран, а в память, и без
    // дисплея (в конвейере, в контейнере) обязана работать так же.
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);

    if (argc != 3) {
        std::fprintf(stderr, "  make-ico <исходник.png> <результат.ico>\n");
        return 2;
    }

    QImage source(QString::fromLocal8Bit(argv[1]));
    if (source.isNull()) {
        std::fprintf(stderr, "не читается: %s\n", argv[1]);
        return 1;
    }
    if (source.width() != source.height())
        std::fprintf(stderr, "предупреждение: исходник не квадратный (%dx%d)\n",
                     source.width(), source.height());
    source = source.convertToFormat(QImage::Format_ARGB32);

    std::vector<QByteArray> blobs;
    for (int size : kSizes) {
        // SmoothTransformation обязателен: без него 512 → 16 идёт ближайшей
        // точкой и даёт рваные края — ровно то, ради чего значок и делают.
        const QImage scaled = source.scaled(size, size, Qt::IgnoreAspectRatio,
                                            Qt::SmoothTransformation);
        blobs.push_back(size == 256 ? pngBytes(scaled) : bmpBytes(scaled));
    }

    std::vector<char> out;
    put16(out, 0);   // зарезервировано
    put16(out, 1);   // тип: 1 — значок
    put16(out, quint16(std::size(kSizes)));

    // Смещения считаются после всех записей каталога — он фиксированной длины.
    quint32 offset = quint32(6 + 16 * std::size(kSizes));
    for (size_t i = 0; i < std::size(kSizes); ++i) {
        const int size = kSizes[i];
        // Ширина и высота — ОДИН БАЙТ каждая, и 256 в него не влезает: формат
        // договорился писать вместо неё ноль. Здесь спотыкаются все, кто пишет
        // .ico впервые.
        out.push_back(char(size == 256 ? 0 : size));
        out.push_back(char(size == 256 ? 0 : size));
        out.push_back(0);            // цветов в палитре: 0 — не палитра
        out.push_back(0);            // зарезервировано
        put16(out, 1);               // плоскостей
        put16(out, 32);              // бит на точку
        put32(out, quint32(blobs[i].size()));
        put32(out, offset);
        offset += quint32(blobs[i].size());
    }
    for (const QByteArray& blob : blobs) out.insert(out.end(), blob.begin(), blob.end());

    QFile file(QString::fromLocal8Bit(argv[2]));
    if (!file.open(QIODevice::WriteOnly)) {
        std::fprintf(stderr, "не пишется: %s\n", argv[2]);
        return 1;
    }
    if (file.write(out.data(), qsizetype(out.size())) != qsizetype(out.size())) {
        std::fprintf(stderr, "не дописался: %s\n", argv[2]);
        return 1;
    }
    std::printf("%s: %zu размеров, %zu байт\n", argv[2], std::size(kSizes), out.size());
    return 0;
}
