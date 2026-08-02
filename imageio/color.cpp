#include "color.h"

#include <jxl/cms.h>
#include <jxl/color_encoding.h>

#include <QColorSpace>

#include <algorithm>

#ifdef _OPENMP
#include <omp.h>
#endif

namespace zametti {

// Профиль как его видит CMS: блоб плюс описание, если оно вышло. Описание
// заполняем по мере сил — движку оно подсказка, а не обязанность.
//
// Лежит НЕ в безымянном пространстве имён нарочно: он поле у
// RowIccConverter::Impl, а тот объявлен в заголовке и связывание имеет внешнее.
// Поле с внутренним связыванием у такого класса — предупреждение компилятора,
// и справедливое.
struct Profile {
    JxlColorProfile jxl{};
    QByteArray icc;
    // Объявляет ли профиль своим пространством данных CMYK. Это НЕ то же, что
    // «каналов четыре»: по этому полю skcms решает, переворачивать ли краску,
    // и наш переворот обязан висеть на том же условии, а не на числе каналов.
    bool cmyk = false;

    Profile(const JxlCmsInterface& cms, const QByteArray& blob, size_t channels) : icc(blob) {
        jxl.icc.data = reinterpret_cast<const uint8_t*>(icc.constData());
        jxl.icc.size = size_t(icc.size());
        jxl.num_channels = channels;
        JXL_BOOL isCmyk = JXL_FALSE;
        cms.set_fields_from_icc(cms.set_fields_data, jxl.icc.data, jxl.icc.size,
                                &jxl.color_encoding, &isCmyk);
        cmyk = isCmyk != 0;
    }
};

namespace {

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

bool iccDescribesRgb(const QByteArray& icc) {
    if (icc.isEmpty()) return false;
    const JxlCmsInterface cms = *JxlGetDefaultCms();
    const Profile p(cms, icc, 3);
    if (p.cmyk) return false;
    // Серый профиль на трёхканальных пикселях — та же беда с другой стороны:
    // одна кривая на три составляющих не натягивается.
    return p.jxl.color_encoding.color_space != JXL_COLOR_SPACE_GRAY;
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

struct RowIccConverter::Impl {
    // Порядок объявления существен: cms должен быть готов до профилей, а
    // профили — до init. Список инициализации идёт по объявлению, не по записи.
    JxlCmsInterface cms;
    Profile src;
    Profile dst;
    int channels;
    int width;
    void* state = nullptr;

    Impl(const QByteArray& from, int ch, const QByteArray& to, int w)
        : cms(*JxlGetDefaultCms()),
          src(cms, from, size_t(ch)),
          dst(cms, to, 3),
          channels(ch),
          width(w) {
        // Четыре канала имеют смысл ТОЛЬКО с профилем, который сам объявил себя
        // CMYK. Иначе выйдет молчаливая чепуха: мы бы переворачивали краску, а
        // skcms — нет, и наоборот. Пусть лучше зовущий узнает отказ и уйдёт на
        // свой запасной путь.
        if (ch == 4 && !src.cmyk) return;
        if (w > 0) state = cms.init(cms.init_data, 1, size_t(w), &src.jxl, &dst.jxl, 255.0f);
    }
    ~Impl() {
        if (state) cms.destroy(state);
    }
};

RowIccConverter::RowIccConverter(const QByteArray& from, int channels, const QByteArray& to,
                                 int width)
    : impl_(from.isEmpty() || to.isEmpty() || channels < 1 || channels > 4 || width < 1
                ? nullptr
                : std::make_unique<Impl>(from, channels, to, width)) {}

RowIccConverter::~RowIccConverter() = default;

bool RowIccConverter::valid() const { return impl_ && impl_->state != nullptr; }

float* RowIccConverter::input() {
    return valid() ? impl_->cms.get_src_buf(impl_->state, 0) : nullptr;
}

const float* RowIccConverter::output() const {
    return valid() ? impl_->cms.get_dst_buf(impl_->state, 0) : nullptr;
}

bool RowIccConverter::run() {
    if (!valid()) return false;
    float* in = impl_->cms.get_src_buf(impl_->state, 0);
    float* out = impl_->cms.get_dst_buf(impl_->state, 0);
    if (!in || !out) return false;
    // ВОТ ТОТ САМЫЙ ПЕРЕВОРОТ, про который написано в заголовке. Снаружи
    // единица — полная краска, у skcms единица — чистая бумага. Условие ровно
    // то же, по которому переворачивает skcms: не «каналов четыре», а «профиль
    // объявил себя CMYK».
    if (impl_->src.cmyk) {
        const int n = impl_->width * 4;
        for (int i = 0; i < n; ++i) in[i] = 1.0f - in[i];
    }
    return impl_->cms.run(impl_->state, 0, in, out, size_t(impl_->width)) != 0;
}

bool canonicalizeColor(QImage& image, QByteArray& icc) {
    if (image.isNull()) return false;

    // Профиль ищем сначала там, откуда его вынул читатель контейнера, потом в
    // самой картинке: ровно тот же порядок, что у кодировщика, — иначе
    // проверяли бы одно, а записывали другое.
    const QByteArray have = !icc.isEmpty() ? icc : image.colorSpace().iccProfile();
    if (have.isEmpty()) return false;      // профиля нет — и приводить нечего

    // ПРОФИЛЬ ОБЯЗАН ОПИСЫВАТЬ ТЕ ПИКСЕЛИ, ЧТО НАМ ДАЛИ. У CMYK-JPEG от Adobe
    // в файле печатный CMYK-профиль, а Qt отдаёт уже переведённый им RGB —
    // профиль и пиксели после этого про разное. Такой профиль выбрасываем
    // совсем: и переводить по нему нельзя, и в хранилище класть тоже —
    // пиксели он не описывает.
    if (!iccDescribesRgb(have)) {
        icc.clear();
        image.setColorSpace(QColorSpace());
        return false;
    }

    if (iccIsCanonical(have)) return false;  // уже выражается описанием

    const QByteArray target = displayP3Icc();
    if (target.isEmpty() || !convertIcc(image, have, target)) return false;

    image.setColorSpace(QColorSpace(QColorSpace::DisplayP3));
    icc = target;
    return true;
}

}  // namespace zametti
