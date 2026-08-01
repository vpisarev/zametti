// Откуда берутся числа конфига: кривая «качество против байтов».
//
// Мысль владельца: единственная настройка, которую за человека никто не
// выберет, — это S, максимальное линейное разрешение (1920, 2400, 3360).
// Всё остальное выводится из замера, в том числе уровень качества: надо найти
// точку, за которой мы платим хранилищем всё больше, а качества получаем всё
// меньше.
//
// Стенд, а не проверка: add_test для него нет. Гоняется руками и долго.
//
//   quality_study <список файлов> <куда положить csv> [quality|size]
//
// Список — по одному пути в строке (см. .testdata/images/выборка/список.txt).
// Файлы владельца читаются и никак не меняются.
//
// ЧТО СЧИТАЕТСЯ МЕРОЙ ПОТЕРИ. Два разных вопроса, и стенд отвечает на оба:
//
//   режим quality — «сколько теряет сжатие»: SSIMULACRA2 против уменьшенного
//     оригинала. Размер у обоих один, значит разница только о сжатии;
//
//   режим restore — «сколько теряет ХРАНЕНИЕ»: картинка уменьшается, жмётся,
//     разжимается и ВОССТАНАВЛИВАЕТСЯ до исходного размера, а сравнивается с
//     самим оригиналом. Это честный ответ на вопрос владельца «что мы отдали,
//     положив её маленькой»: для мелких фотографий потери нет вовсе, а для
//     крупных она тем больше, чем мельче текстуры.
//
// Уменьшение — усреднением по площади, восстановление — Lanczos (указание
// владельца): у Lanczos отрицательные лепестки ядра, и на уменьшении они дают
// ореолы вокруг границ.

#include "color.h"
#include "import_limits.h"
#include "jxl_encoder.h"
#include "resample.h"

#include "tools/ssimulacra2.h"
#include "lib/jxl/color_encoding_internal.h"
#include "lib/jxl/image.h"
#include "lib/jxl/image_bundle.h"
#include "tools/no_memory_manager.h"

#include <jxl/decode.h>
#include <jxl/decode_cxx.h>

#include <QBuffer>
#include <QColorSpace>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QImageReader>
#include <QTextStream>

#include <cstdio>
#include <memory>
#include <vector>

using namespace zametti;

namespace {

QImage decodeJxl(const QByteArray& jxl) {
    QBuffer buf;
    buf.setData(jxl);
    buf.open(QIODevice::ReadOnly);
    QImageReader r(&buf, "jxl");
    return r.read();
}

std::shared_ptr<jxl::ImageBundle> toBundle(const QImage& src) {
    const QImage img = src.convertToFormat(QImage::Format_RGBX64);
    JxlMemoryManager* mm = jpegxl::tools::NoMemoryManager();
    auto created = jxl::Image3F::Create(mm, size_t(img.width()), size_t(img.height()));
    if (!created.ok()) return nullptr;
    jxl::Image3F out = std::move(created).value_();
    for (int y = 0; y < img.height(); ++y) {
        const auto* s = reinterpret_cast<const uint16_t*>(img.constScanLine(y));
        float* r = out.PlaneRow(0, size_t(y));
        float* g = out.PlaneRow(1, size_t(y));
        float* b = out.PlaneRow(2, size_t(y));
        for (int x = 0; x < img.width(); ++x) {
            r[x] = s[x * 4 + 0] / 65535.0f;
            g[x] = s[x * 4 + 1] / 65535.0f;
            b[x] = s[x * 4 + 2] / 65535.0f;
        }
    }
    auto ib = std::make_shared<jxl::ImageBundle>(mm, new jxl::ImageMetadata());
    if (!ib->SetFromImage(std::move(out), jxl::ColorEncoding::SRGB(false))) return nullptr;
    return ib;
}

double score(const QImage& reference, const QImage& got) {
    if (reference.size() != got.size()) return -1000;
    auto a = toBundle(reference);
    auto b = toBundle(got);
    if (!a || !b) return -1000;
    auto res = ComputeSSIMULACRA2(*a, *b);
    if (!res.ok()) return -1000;
    return std::move(res).value_().Score();
}

QString sourceOf(const QString& path) {
    // Имя каталога сразу под ~/Pictures: по нему в отчёте видно, что за род
    // картинок, — фотографии, сканы плёнки, работы художника.
    const QString marker = QStringLiteral("/Pictures/");
    const int at = path.indexOf(marker);
    if (at < 0) return QStringLiteral("?");
    const QString rest = path.mid(at + marker.size());
    const int slash = rest.indexOf(QLatin1Char('/'));
    return slash < 0 ? rest : rest.left(slash);
}

}  // namespace

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    if (argc < 3) {
        std::fprintf(stderr, "quality_study <список> <csv> [quality|restore]\n");
        return 2;
    }
    // Крупные снимки в выборке доходят до 60 Мп; умолчание Qt их отвергло бы.
    QImageReader::setAllocationLimit(2048);

    const QString mode = argc > 3 ? QString::fromLocal8Bit(argv[3]) : QStringLiteral("quality");
    QFile listFile(QString::fromLocal8Bit(argv[1]));
    if (!listFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        std::fprintf(stderr, "не открылся список\n");
        return 2;
    }
    QStringList files;
    while (!listFile.atEnd()) {
        const QString line = QString::fromUtf8(listFile.readLine()).trimmed();
        if (!line.isEmpty()) files << line;
    }
    listFile.close();

    QFile out(QString::fromLocal8Bit(argv[2]));
    if (!out.open(QIODevice::WriteOnly | QIODevice::Text)) {
        std::fprintf(stderr, "не открылся csv\n");
        return 2;
    }
    QTextStream csv(&out);
    csv << "источник,файл,исхШирина,исхВысота,S,качество,цельШирина,цельВысота,байт,ssimu2,мс,"
           "уменьшали\n";

    // Что перебираем. В режиме quality S закреплено, качество гуляет — так
    // ищется точка перелома. В режиме size наоборот.
    const bool restore = mode == QStringLiteral("restore");
    const QList<int> qualities = mode == QStringLiteral("quality")
                                     ? QList<int>{70, 75, 80, 85, 88, 90, 92, 95}
                                     : QList<int>{90};
    // Сетка раздвинута в обе стороны от прежней: надо увидеть, где кривая
    // выполаживается, а не подтвердить заранее выбранное число.
    const QList<int> sizes = mode == QStringLiteral("quality")
                                 ? QList<int>{1920}
                                 : QList<int>{1680, 1920, 2400, 3360, 3840};

    int done = 0;
    int skipped = 0;
    for (const QString& path : files) {
        QImageReader reader(path);
        reader.setAutoTransform(true);
        QImage src = reader.read();
        if (src.isNull()) {
            ++skipped;
            continue;
        }
        // Стенд обязан повторять боевой тракт, иначе он мерит не то, что
        // работает. Приведение цвета в конвейере стоит сразу после разжатия —
        // значит и здесь.
        QByteArray icc;
        canonicalizeColor(src, icc);
        const QString source = sourceOf(path);
        const QString name = QFileInfo(path).fileName();

        for (int s : sizes) {
            ImportLimits limits;
            limits.maxSize = s;
            const Size target = targetSize({src.width(), src.height()}, limits);
            const QImage scaled =
                (target.width == src.width() && target.height == src.height())
                    ? src
                    : resampleArea(src, target.width, target.height);
            const bool shrunk = target.width != src.width() || target.height != src.height();

            for (int q : qualities) {
                EncodeOptions opt;
                opt.quality = q;
                opt.effort = 5;
                QString err;
                QElapsedTimer timer;
                timer.start();
                const QByteArray jxl = encodeJxl(scaled, opt, EncodeMeta{}, &err);
                const qint64 ms = timer.elapsed();
                if (jxl.isEmpty()) continue;
                const QImage back = decodeJxl(jxl);
                // В режиме restore возвращаем картинку к исходному размеру и
                // сверяем с оригиналом. Если уменьшения не было, восстанавливать
                // нечего — сравниваем как есть.
                const double sc =
                    restore ? (shrunk ? score(src, resampleLanczos(back, src.width(),
                                                                   src.height()))
                                      : score(src, back))
                            : score(scaled, back);

                csv << source << ',' << name << ',' << src.width() << ',' << src.height() << ','
                    << s << ',' << q << ',' << target.width << ',' << target.height << ','
                    << jxl.size() << ',' << QString::number(sc, 'f', 3) << ',' << ms << ','
                    << (shrunk ? 1 : 0) << '\n';
            }
        }
        csv.flush();
        if (++done % 10 == 0)
            std::fprintf(stderr, "  %d из %lld (пропущено %d)\n", done,
                         static_cast<long long>(files.size()), skipped);
    }
    std::fprintf(stderr, "готово: %d файлов, пропущено %d\n", done, skipped);
    return 0;
}
