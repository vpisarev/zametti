// Пробник: осилит ли наш CMS перевод CMYK по вложенному профилю.
//
// Повод. Владелец заметил, что «Девятый вал» Айвазовского в нашем JXL заметно
// насыщеннее и контрастнее, чем в GIMP, в просмотрщике GNOME и на macOS. Файл
// оказался не Lab, а CMYK (PHOTOMETRIC_SEPARATED) с профилем 3M Matchprint в
// 424 КБ, а читатель переводил его наивно: (1-C)*(1-K).
//
// Пробник отвечает на два вопроса, от которых зависит вид починки:
//
//   1. берёт ли Qt такой профиль (это решает, не подхватит ли его
//      canonicalizeColor через image.colorSpace() и не переведёт ли пиксели
//      второй раз);
//   2. переводит ли skcms четырёхканальный вход и СХОДИТСЯ ЛИ он с lcms —
//      с тем самым движком, которым считают GIMP и macOS.
//
// Эталон приготовлен отдельно: пиксели картинки уменьшены до 582x384 и
// выгружены в wave_cmyk.raw, а рядом лежит wave_ref_srgb.raw — та же выборка,
// переведённая lcms относительно-колориметрически в sRGB.
//
// Третий вопрос, если дать вторым доводом путь к TIFF: что в итоге ЛОЖИТСЯ В
// ХРАНИЛИЩЕ. Сверка движков ничего не стоит, если между ней и файлом ещё
// уменьшение, энкодер и пометка пространства.
//
// Запуск:
//   ./cmyk_probe <каталог с matchprint.icc, wave_cmyk.raw, wave_ref_srgb.raw>
//                [путь к CMYK-TIFF]

#include <jxl/cms.h>
#include <jxl/color_encoding.h>

#include "import.h"

#include <QBuffer>
#include <QByteArray>
#include <QColorSpace>
#include <QFile>
#include <QColor>
#include <QGuiApplication>
#include <QImage>
#include <QImageReader>

#include <cmath>
#include <cstdio>

namespace {

QByteArray slurp(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        std::fprintf(stderr, "не открылся %s\n", qPrintable(path));
        return {};
    }
    return f.readAll();
}

}  // namespace

using namespace zametti;

int ztCmykProbe(int argc, char** argv) {
    QImageReader::setAllocationLimit(2048);
    if (argc < 2) {
        std::fprintf(stderr, "нужен каталог с приготовленными файлами\n");
        return 2;
    }
    const QString dir = QString::fromLocal8Bit(argv[1]);
    const QByteArray icc = slurp(dir + QStringLiteral("/matchprint.icc"));
    const QByteArray cmyk = slurp(dir + QStringLiteral("/wave_cmyk.raw"));
    const QByteArray ref = slurp(dir + QStringLiteral("/wave_ref_srgb.raw"));
    if (icc.isEmpty() || cmyk.isEmpty() || ref.isEmpty()) return 2;

    const int width = 582;
    const int height = 384;
    if (cmyk.size() != width * height * 4 || ref.size() != width * height * 3) {
        std::fprintf(stderr, "размеры выборки не сходятся\n");
        return 2;
    }

    // Заодно выкладываем профиль Display P3 таким, каким его отдаёт Qt: эталон
    // для сверки должен считаться в ТОЧНО ТО пространство, в которое переводим
    // мы, иначе сравнивались бы разные вещи.
    {
        QFile f(dir + QStringLiteral("/displayp3-from-qt.icc"));
        if (f.open(QIODevice::WriteOnly)) f.write(QColorSpace(QColorSpace::DisplayP3).iccProfile());
    }

    // Вопрос 1: что скажет Qt.
    const QColorSpace qcs = QColorSpace::fromIccProfile(icc);
    std::printf("Qt о профиле: %s\n", qcs.isValid() ? "ПРИНЯЛ (это опасно)" : "отверг");

    // Вопрос 2: что скажет skcms.
    const JxlCmsInterface cms = *JxlGetDefaultCms();

    JxlColorProfile src{};
    src.icc.data = reinterpret_cast<const uint8_t*>(icc.constData());
    src.icc.size = size_t(icc.size());
    src.num_channels = 4;
    JXL_BOOL isCmyk = JXL_FALSE;
    const JXL_BOOL described = cms.set_fields_from_icc(cms.set_fields_data, src.icc.data,
                                                       src.icc.size, &src.color_encoding, &isCmyk);
    std::printf("set_fields_from_icc: описал=%s cmyk=%s\n", described ? "да" : "нет",
                isCmyk ? "да" : "нет");

    const QByteArray dstIcc = QColorSpace(QColorSpace::SRgb).iccProfile();
    JxlColorProfile dst{};
    dst.icc.data = reinterpret_cast<const uint8_t*>(dstIcc.constData());
    dst.icc.size = size_t(dstIcc.size());
    dst.num_channels = 3;
    JXL_BOOL dstCmyk = JXL_FALSE;
    cms.set_fields_from_icc(cms.set_fields_data, dst.icc.data, dst.icc.size, &dst.color_encoding,
                            &dstCmyk);

    void* state = cms.init(cms.init_data, 1, size_t(width), &src, &dst, 255.0f);
    if (!state) {
        std::printf("cms.init ОТКАЗАЛ — четырёхканальный вход не взят\n");
        return 1;
    }

    // Опорные отсчёты: бумага без краски обязана выйти белой, сплошной чёрный —
    // чёрным. Если нет, спор не про качество перевода, а про соглашение о том,
    // что такое единица: краска или её отсутствие.
    {
        const float probes[5][4] = {{0, 0, 0, 0},        // бумага
                                    {0, 0, 0, 1},        // сплошной K
                                    {1, 0, 0, 0},        // голубой
                                    {0, 1, 0, 0},        // пурпурный
                                    {0, 0, 1, 0}};       // жёлтый
        const char* names[5] = {"бумага (0,0,0,0)", "чёрный (0,0,0,1)", "голубой",
                                "пурпурный", "жёлтый"};
        void* st = cms.init(cms.init_data, 1, 5, &src, &dst, 255.0f);
        if (st) {
            float* in = cms.get_src_buf(st, 0);
            float* out = cms.get_dst_buf(st, 0);
            for (int i = 0; i < 5; ++i)
                for (int c = 0; c < 4; ++c) in[i * 4 + c] = 1.0f - probes[i][c];
            if (cms.run(st, 0, in, out, 5)) {
                for (int i = 0; i < 5; ++i)
                    std::printf("  %-18s -> %6.1f %6.1f %6.1f\n", names[i], out[i * 3] * 255.0f,
                                out[i * 3 + 1] * 255.0f, out[i * 3 + 2] * 255.0f);
            }
            cms.destroy(st);
        }
    }

    const auto* cmykBytes = reinterpret_cast<const uint8_t*>(cmyk.constData());
    const auto* refBytes = reinterpret_cast<const uint8_t*>(ref.constData());
    double sumAbs = 0;
    double maxAbs = 0;
    double meanOurs[3] = {0, 0, 0};
    double meanRef[3] = {0, 0, 0};
    long count = 0;

    for (int y = 0; y < height; ++y) {
        float* in = cms.get_src_buf(state, 0);
        float* out = cms.get_dst_buf(state, 0);
        if (!in || !out) {
            std::printf("буферы не выданы\n");
            cms.destroy(state);
            return 1;
        }
        for (int x = 0; x < width * 4; ++x)
            in[x] = 1.0f - float(cmykBytes[size_t(y) * size_t(width) * 4 + size_t(x)]) / 255.0f;
        if (!cms.run(state, 0, in, out, size_t(width))) {
            std::printf("cms.run ОТКАЗАЛ на строке %d\n", y);
            cms.destroy(state);
            return 1;
        }
        for (int x = 0; x < width; ++x) {
            for (int c = 0; c < 3; ++c) {
                const double ours = std::fmin(std::fmax(double(out[x * 3 + c]), 0.0), 1.0) * 255.0;
                const double want =
                    double(refBytes[(size_t(y) * size_t(width) + size_t(x)) * 3 + size_t(c)]);
                const double d = std::fabs(ours - want);
                sumAbs += d;
                if (d > maxAbs) maxAbs = d;
                meanOurs[c] += ours;
                meanRef[c] += want;
            }
            ++count;
        }
    }
    cms.destroy(state);

    std::printf("skcms  средн RGB %.1f %.1f %.1f\n", meanOurs[0] / double(count),
                meanOurs[1] / double(count), meanOurs[2] / double(count));
    std::printf("lcms   средн RGB %.1f %.1f %.1f\n", meanRef[0] / double(count),
                meanRef[1] / double(count), meanRef[2] / double(count));
    std::printf("расхождение skcms против lcms: средн %.2f, наибольшее %.0f (из 255)\n",
                sumAbs / (double(count) * 3.0), maxAbs);

    // --- весь конвейер целиком ---------------------------------------------
    if (argc > 2) {
        const QString tiffPath = QString::fromLocal8Bit(argv[2]);

        // Что видит Qt в САМОМ исходнике. Нужно, чтобы отделить «мы неверно
        // перевели» от «нам неверно отдали пиксели».
        {
            QImageReader r(tiffPath);
            const QImage got = r.read();
            if (got.isNull()) {
                std::printf("\nQt исходник не прочитал: %s\n", qPrintable(r.errorString()));
            } else {
                double m[3] = {0, 0, 0};
                for (int y = 0; y < got.height(); ++y)
                    for (int x = 0; x < got.width(); ++x) {
                        const QColor c = got.pixelColor(x, y);
                        m[0] += c.red();
                        m[1] += c.green();
                        m[2] += c.blue();
                    }
                const double n = double(got.width()) * double(got.height());
                std::printf("\nQt из исходника: формат «%s», %dx%d, пространство «%s», "
                            "средний RGB %.1f %.1f %.1f\n",
                            r.format().constData(), got.width(), got.height(),
                            qPrintable(got.colorSpace().description()), m[0] / n, m[1] / n,
                            m[2] / n);
            }
        }

        ImportLimits limits;
        const ImportResult r = importImage(tiffPath, limits);
        std::printf("\nконвейер: путь %s, %dx%d, %d бит, качество %d, %lld байт\n",
                    routeName(r.route), r.size.width, r.size.height, r.bitsPerSample, r.quality,
                    static_cast<long long>(r.bytes.size()));
        if (!r.ok()) {
            std::printf("отказ: %s\n", qPrintable(r.message));
            return 1;
        }
        // Читаем обратно ровно тем плагином, которым читает программа.
        QByteArray blob = r.bytes;
        QBuffer buf(&blob);
        buf.open(QIODevice::ReadOnly);
        QImageReader reader(&buf, r.extension.toLatin1());
        const QImage back = reader.read();
        if (back.isNull()) {
            std::printf("обратно не прочиталось: %s\n", qPrintable(reader.errorString()));
            return 1;
        }
        double mean[3] = {0, 0, 0};
        for (int y = 0; y < back.height(); ++y)
            for (int x = 0; x < back.width(); ++x) {
                const QColor c = back.pixelColor(x, y);
                mean[0] += c.red();
                mean[1] += c.green();
                mean[2] += c.blue();
            }
        const double n = double(back.width()) * double(back.height());
        back.save(dir + QStringLiteral("/from-storage.png"));
        std::printf("из хранилища: %dx%d, пространство «%s», средний RGB %.1f %.1f %.1f\n",
                    back.width(), back.height(),
                    qPrintable(back.colorSpace().description()), mean[0] / n, mean[1] / n,
                    mean[2] / n);
    }
    return 0;
}
