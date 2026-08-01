#include "color.h"

#include <jxl/cms.h>
#include <jxl/color_encoding.h>

#include <QColorSpace>

#include <algorithm>

#ifdef _OPENMP
#include <omp.h>
#endif

namespace zametti {

namespace {

// Профиль как его видит CMS: блоб плюс описание, если оно вышло. Описание
// заполняем по мере сил — движку оно подсказка, а не обязанность.
struct Profile {
    JxlColorProfile jxl{};
    QByteArray icc;

    Profile(const JxlCmsInterface& cms, const QByteArray& blob, size_t channels) : icc(blob) {
        jxl.icc.data = reinterpret_cast<const uint8_t*>(icc.constData());
        jxl.icc.size = size_t(icc.size());
        jxl.num_channels = channels;
        JXL_BOOL cmyk = JXL_FALSE;
        cms.set_fields_from_icc(cms.set_fields_data, jxl.icc.data, jxl.icc.size,
                                &jxl.color_encoding, &cmyk);
    }
};

// Формат, в котором удобно переводить: четыре чередующихся составляющих,
// цвет в первых трёх. Оба варианта — восьмибитный и шестнадцатибитный —
// уложены одинаково, поэтому дальше разница только в ширине отсчёта.
QImage::Format workFormat(const QImage& image) {
    const bool deep = image.depth() > 32;
    if (image.hasAlphaChannel())
        return deep ? QImage::Format_RGBA64 : QImage::Format_RGBA8888;
    return deep ? QImage::Format_RGBX64 : QImage::Format_RGBX8888;
}

}  // namespace

bool iccIsCanonical(const QByteArray& icc) {
    if (icc.isEmpty()) return false;
    // СПРАШИВАЕМ Qt, А НЕ skcms, хотя переводит потом именно skcms. Причина
    // замерена: на профиле Apple skcms.set_fields_from_icc отвечает «описал» —
    // он ПРИБЛИЖАЕТ таблицу первичными и кривой. Ответ бесполезен: круг записи
    // и чтения такой профиль всё равно не переживает, и −40 остаются на месте.
    // Qt же честно говорит «первичные свои, кривая своя» — а нам нужен ровно
    // этот признак.
    const QColorSpace cs = QColorSpace::fromIccProfile(icc);
    if (!cs.isValid()) return false;
    return cs.primaries() != QColorSpace::Primaries::Custom &&
           cs.transferFunction() != QColorSpace::TransferFunction::Custom;
}

QByteArray displayP3Icc() {
    static const QByteArray blob = QColorSpace(QColorSpace::DisplayP3).iccProfile();
    return blob;
}

bool convertIcc(QImage& image, const QByteArray& from, const QByteArray& to) {
    if (image.isNull() || from.isEmpty() || to.isEmpty()) return false;

    const QImage::Format want = workFormat(image);
    if (image.format() != want) image = image.convertToFormat(want);
    if (image.isNull()) return false;

    const int width = image.width();
    const int height = image.height();
    const bool deep = image.depth() > 32;

    const JxlCmsInterface cms = *JxlGetDefaultCms();
    const Profile src(cms, from, 3);
    const Profile dst(cms, to, 3);

#ifdef _OPENMP
    const int threads = std::max(1, omp_get_max_threads());
#else
    const int threads = 1;
#endif

    // intensity_target 255 — та же величина, что libjxl подставляет для
    // обычных, не-HDR пространств; здесь она ни на что не влияет, потому что
    // ни PQ, ни HLG в переводе не участвуют.
    void* state = cms.init(cms.init_data, size_t(threads), size_t(width), &src.jxl, &dst.jxl,
                           255.0f);
    if (!state) return false;

    bool ok = true;
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
    for (int y = 0; y < height; ++y) {
#ifdef _OPENMP
        const size_t thread = size_t(omp_get_thread_num());
#else
        const size_t thread = 0;
#endif
        float* in = cms.get_src_buf(state, thread);
        float* out = cms.get_dst_buf(state, thread);
        if (!in || !out) {
            ok = false;
            continue;
        }
        if (deep) {
            const auto* row = reinterpret_cast<const quint16*>(image.constScanLine(y));
            for (int x = 0; x < width; ++x)
                for (int c = 0; c < 3; ++c)
                    in[x * 3 + c] = float(row[size_t(x) * 4 + size_t(c)]) / 65535.0f;
        } else {
            const uchar* row = image.constScanLine(y);
            for (int x = 0; x < width; ++x)
                for (int c = 0; c < 3; ++c)
                    in[x * 3 + c] = float(row[size_t(x) * 4 + size_t(c)]) / 255.0f;
        }
        if (!cms.run(state, thread, in, out, size_t(width))) {
            ok = false;
            continue;
        }
        // Выход за пределы отрезка — обычное дело: охват источника шире цели.
        // Обрезаем, потому что дальше отсчёт целый и другого выхода нет.
        if (deep) {
            auto* row = reinterpret_cast<quint16*>(image.scanLine(y));
            for (int x = 0; x < width; ++x)
                for (int c = 0; c < 3; ++c)
                    row[size_t(x) * 4 + size_t(c)] =
                        quint16(std::clamp(out[x * 3 + c], 0.0f, 1.0f) * 65535.0f + 0.5f);
        } else {
            uchar* row = image.scanLine(y);
            for (int x = 0; x < width; ++x)
                for (int c = 0; c < 3; ++c)
                    row[size_t(x) * 4 + size_t(c)] =
                        uchar(std::clamp(out[x * 3 + c], 0.0f, 1.0f) * 255.0f + 0.5f);
        }
    }
    cms.destroy(state);
    return ok;
}

bool canonicalizeColor(QImage& image, QByteArray& icc) {
    if (image.isNull()) return false;

    // Профиль ищем сначала там, откуда его вынул читатель контейнера, потом в
    // самой картинке: ровно тот же порядок, что у кодировщика, — иначе
    // проверяли бы одно, а записывали другое.
    const QByteArray have = !icc.isEmpty() ? icc : image.colorSpace().iccProfile();
    if (have.isEmpty()) return false;      // профиля нет — и приводить нечего
    if (iccIsCanonical(have)) return false;  // уже выражается описанием

    const QByteArray target = displayP3Icc();
    if (target.isEmpty() || !convertIcc(image, have, target)) return false;

    image.setColorSpace(QColorSpace(QColorSpace::DisplayP3));
    icc = target;
    return true;
}

}  // namespace zametti
