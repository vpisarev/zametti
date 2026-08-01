// Пробник кодеков этапа 8: сколько стоит вставить картинку.
//
// Тестом не является — это измерительный прибор. Отвечает на пять вопросов
// брифа сразу, по каждому файлу:
//
//   1. проба на копии ~800 px: отношение lossless/lossy и её цена;
//   2. полный энкод lossy JXL q90 и lossless JXL — время и размер;
//   3. для JPEG: транскод в JXL и обратная реконструкция, с побайтовой сверкой;
//   4. декод JXL против декода WebP (кэш этапа 6 платит его один раз, но число
//      знать надо);
//   5. цена SSIMULACRA2 — арбитра лестницы.
//
// Время: МИНИМУМ серии, а не медиана. Медиана мешает шум машины с ценой кода;
// минимум отвечает на нужный вопрос — сколько это стоит, когда ничто не мешало.
// Рядом с каждым замером крутится ЭТАЛОН — постоянный счётный цикл. Если он
// поехал, значит поехала машина, и числам верить нельзя: на вендоринге zstd
// именно эталон разоблачил «двукратный выигрыш -O3», оказавшийся раскладкой
// процесса.
//
// Запускать под taskset, иначе гибридный процессор гоняет процесс между P- и
// E-ядрами и серии смещаются целиком:
//
//   taskset -c 0 ./image_bench <файл-или-каталог> [ещё пути...]

#include <jxl/decode.h>
#include <jxl/encode.h>
#include <jxl/resizable_parallel_runner.h>
#include <jxl/thread_parallel_runner.h>

#include <QByteArray>
#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QImageReader>
#include <QImageWriter>

#include "tools/ssimulacra2.h"
#include "lib/jxl/color_encoding_internal.h"
#include "lib/jxl/image.h"
#include "lib/jxl/image_bundle.h"
#include "tools/no_memory_manager.h"

#include <sched.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

double msSince(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

// --- эталон ---------------------------------------------------------------
//
// Постоянный счётный цикл: сам по себе не нужен, нужен его РАЗБРОС. Пока
// эталон стоит намертво, разброс в замерах — свойство измеряемого кода;
// поехал эталон — поехала машина.

double g_sink = 0.0;

double yardstick() {
    const auto t0 = Clock::now();
    double s = 0.0;
    for (int i = 1; i <= 2'000'000; ++i) s += 1.0 / double(i);
    g_sink += s;
    return msSince(t0);
}

// Минимум серии + разброс эталона рядом.
struct Timing {
    double best = 0.0;      // мс
    double yardBest = 0.0;  // мс
    double yardWorst = 0.0;
};

template <typename F>
Timing measure(int runs, F&& body) {
    Timing t;
    t.best = 1e18;
    t.yardBest = 1e18;
    for (int i = 0; i < runs; ++i) {
        const double y = yardstick();
        t.yardBest = std::min(t.yardBest, y);
        t.yardWorst = std::max(t.yardWorst, y);
        const auto t0 = Clock::now();
        body();
        t.best = std::min(t.best, msSince(t0));
    }
    return t;
}

// --- мостик Qt → libjxl ---------------------------------------------------

struct Pixels {
    int width = 0;
    int height = 0;
    int channels = 0;   // 3 или 4
    std::vector<uint8_t> data;
    bool ok() const { return width > 0 && height > 0; }
};

Pixels fromImage(const QImage& src) {
    Pixels p;
    if (src.isNull()) return p;
    const bool alpha = src.hasAlphaChannel();
    const QImage img = src.convertToFormat(alpha ? QImage::Format_RGBA8888
                                                 : QImage::Format_RGB888);
    p.width = img.width();
    p.height = img.height();
    p.channels = alpha ? 4 : 3;
    p.data.resize(size_t(p.width) * size_t(p.height) * size_t(p.channels));
    const size_t row = size_t(p.width) * size_t(p.channels);
    for (int y = 0; y < p.height; ++y)
        std::memcpy(p.data.data() + size_t(y) * row, img.constScanLine(y), row);
    return p;
}

// --- целевой размер -------------------------------------------------------
//
// Пиксельный бюджет: цель = min(бюджет S², потолок стороны 3S), пропорции
// сохраняются. Для соотношения a:1 ширина выходит S·√a, пока a ≤ 9; дальше
// режет потолок стороны — это про ленты и скриншоты переписок. Панорама 3:1
// при S=1600 живёт на бюджете и даёт 2771×924.
//
// АПСКЕЙЛА НЕТ НИ НА КАКОМ ПУТИ: картинка меньше бюджета идёт как есть.

constexpr int kMaxImportedImageSize = 1600;  // S из конфига

QSize targetSize(QSize src) {
    if (src.isEmpty()) return src;
    const double s = double(kMaxImportedImageSize);
    const double budget = s * s;
    const double cap = 3.0 * s;
    const double w0 = src.width(), h0 = src.height();

    double k = std::sqrt(budget / (w0 * h0));   // во сколько ужать по площади
    k = std::min(k, cap / std::max(w0, h0));    // но не длиннее потолка стороны
    if (k >= 1.0) return src;                   // меньше бюджета — как есть
    return QSize(std::max(1, int(std::lround(w0 * k))),
                 std::max(1, int(std::lround(h0 * k))));
}

// --- энкод ----------------------------------------------------------------

// runner == nullptr — однопоточно (числа сравнимые между запусками).
//
// effort — «усилие» энкодера libjxl (1 быстро … 9 медленно). Умолчание самой
// libjxl — 7, у нас 5: решение владельца, и замер его держит. На 4.1 Мп,
// шестнадцать ядер: e5 — 23.6 Мп/с, e7 — 14.8, e9 — 0.53 при выигрыше в
// размере в единицы процентов. Ручка главная: лестница делает до шести
// энкодов подряд.
QByteArray encodeJxl(const Pixels& px, float quality, bool lossless, void* runner,
                     int effort = 5) {
    JxlEncoder* enc = JxlEncoderCreate(nullptr);
    if (!enc) return {};
    if (runner)
        JxlEncoderSetParallelRunner(enc, JxlThreadParallelRunner, runner);

    JxlBasicInfo info;
    JxlEncoderInitBasicInfo(&info);
    info.xsize = uint32_t(px.width);
    info.ysize = uint32_t(px.height);
    info.bits_per_sample = 8;
    info.num_color_channels = 3;
    info.num_extra_channels = px.channels == 4 ? 1 : 0;
    info.alpha_bits = px.channels == 4 ? 8 : 0;
    // Для lossless просим не переводить в внутреннее XYB: иначе «без потерь»
    // потеряет — это ровно та ловушка, ради которой флаг и существует.
    info.uses_original_profile = lossless ? JXL_TRUE : JXL_FALSE;
    if (JxlEncoderSetBasicInfo(enc, &info) != JXL_ENC_SUCCESS) {
        JxlEncoderDestroy(enc);
        return {};
    }

    JxlColorEncoding color = {};
    JxlColorEncodingSetToSRGB(&color, JXL_FALSE);
    JxlEncoderSetColorEncoding(enc, &color);

    JxlEncoderFrameSettings* fs = JxlEncoderFrameSettingsCreate(enc, nullptr);
    JxlEncoderFrameSettingsSetOption(fs, JXL_ENC_FRAME_SETTING_EFFORT, effort);
    if (lossless) {
        JxlEncoderSetFrameLossless(fs, JXL_TRUE);
        JxlEncoderSetFrameDistance(fs, 0.0f);
    } else {
        JxlEncoderSetFrameDistance(fs, JxlEncoderDistanceFromQuality(quality));
    }

    const JxlPixelFormat fmt{uint32_t(px.channels), JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0};
    if (JxlEncoderAddImageFrame(fs, &fmt, px.data.data(), px.data.size()) != JXL_ENC_SUCCESS) {
        JxlEncoderDestroy(enc);
        return {};
    }
    JxlEncoderCloseInput(enc);

    QByteArray out;
    out.resize(1 << 16);
    uint8_t* next = reinterpret_cast<uint8_t*>(out.data());
    size_t avail = size_t(out.size());
    JxlEncoderStatus st = JXL_ENC_NEED_MORE_OUTPUT;
    while (st == JXL_ENC_NEED_MORE_OUTPUT) {
        st = JxlEncoderProcessOutput(enc, &next, &avail);
        if (st == JXL_ENC_NEED_MORE_OUTPUT) {
            const qsizetype used = out.size() - qsizetype(avail);
            out.resize(out.size() * 2);
            next = reinterpret_cast<uint8_t*>(out.data()) + used;
            avail = size_t(out.size() - used);
        }
    }
    JxlEncoderDestroy(enc);
    if (st != JXL_ENC_SUCCESS) return {};
    out.resize(out.size() - qsizetype(avail));
    return out;
}

// Транскод JPEG→JXL: байты JPEG кладутся как есть, libjxl хранит всё нужное
// для побайтовой обратной сборки.
QByteArray transcodeJpeg(const QByteArray& jpeg) {
    JxlEncoder* enc = JxlEncoderCreate(nullptr);
    if (!enc) return {};
    JxlEncoderUseContainer(enc, JXL_TRUE);
    if (JxlEncoderStoreJPEGMetadata(enc, JXL_TRUE) != JXL_ENC_SUCCESS) {
        JxlEncoderDestroy(enc);
        return {};
    }
    JxlEncoderFrameSettings* fs = JxlEncoderFrameSettingsCreate(enc, nullptr);
    if (JxlEncoderAddJPEGFrame(fs, reinterpret_cast<const uint8_t*>(jpeg.constData()),
                               size_t(jpeg.size())) != JXL_ENC_SUCCESS) {
        JxlEncoderDestroy(enc);
        return {};
    }
    JxlEncoderCloseInput(enc);

    QByteArray out;
    out.resize(std::max<qsizetype>(1 << 16, jpeg.size()));
    uint8_t* next = reinterpret_cast<uint8_t*>(out.data());
    size_t avail = size_t(out.size());
    JxlEncoderStatus st = JXL_ENC_NEED_MORE_OUTPUT;
    while (st == JXL_ENC_NEED_MORE_OUTPUT) {
        st = JxlEncoderProcessOutput(enc, &next, &avail);
        if (st == JXL_ENC_NEED_MORE_OUTPUT) {
            const qsizetype used = out.size() - qsizetype(avail);
            out.resize(out.size() * 2);
            next = reinterpret_cast<uint8_t*>(out.data()) + used;
            avail = size_t(out.size() - used);
        }
    }
    JxlEncoderDestroy(enc);
    if (st != JXL_ENC_SUCCESS) return {};
    out.resize(out.size() - qsizetype(avail));
    return out;
}

// Обратная сборка исходного JPEG из транскода.
QByteArray reconstructJpeg(const QByteArray& jxl) {
    JxlDecoder* dec = JxlDecoderCreate(nullptr);
    if (!dec) return {};
    if (JxlDecoderSubscribeEvents(dec, JXL_DEC_JPEG_RECONSTRUCTION | JXL_DEC_FULL_IMAGE) !=
        JXL_DEC_SUCCESS) {
        JxlDecoderDestroy(dec);
        return {};
    }
    JxlDecoderSetInput(dec, reinterpret_cast<const uint8_t*>(jxl.constData()), size_t(jxl.size()));
    JxlDecoderCloseInput(dec);

    QByteArray out;
    out.resize(1 << 20);
    bool haveBuffer = false;
    for (;;) {
        const JxlDecoderStatus st = JxlDecoderProcessInput(dec);
        if (st == JXL_DEC_JPEG_RECONSTRUCTION) {
            JxlDecoderSetJPEGBuffer(dec, reinterpret_cast<uint8_t*>(out.data()),
                                    size_t(out.size()));
            haveBuffer = true;
        } else if (st == JXL_DEC_JPEG_NEED_MORE_OUTPUT) {
            const size_t left = JxlDecoderReleaseJPEGBuffer(dec);
            const qsizetype used = out.size() - qsizetype(left);
            out.resize(out.size() * 2);
            JxlDecoderSetJPEGBuffer(dec, reinterpret_cast<uint8_t*>(out.data()) + used,
                                    size_t(out.size() - used));
        } else if (st == JXL_DEC_FULL_IMAGE || st == JXL_DEC_SUCCESS) {
            if (!haveBuffer) { JxlDecoderDestroy(dec); return {}; }
            const size_t left = JxlDecoderReleaseJPEGBuffer(dec);
            out.resize(out.size() - qsizetype(left));
            JxlDecoderDestroy(dec);
            return out;
        } else if (st == JXL_DEC_ERROR || st == JXL_DEC_NEED_MORE_INPUT) {
            JxlDecoderDestroy(dec);
            return {};
        }
    }
}

// Декод JXL в пиксели — то, что будет делать наш плагин.
Pixels decodeJxl(const QByteArray& jxl) {
    Pixels px;
    JxlDecoder* dec = JxlDecoderCreate(nullptr);
    if (!dec) return px;
    JxlDecoderSubscribeEvents(dec, JXL_DEC_BASIC_INFO | JXL_DEC_FULL_IMAGE);
    JxlDecoderSetInput(dec, reinterpret_cast<const uint8_t*>(jxl.constData()), size_t(jxl.size()));
    JxlDecoderCloseInput(dec);

    JxlPixelFormat fmt{3, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0};
    for (;;) {
        const JxlDecoderStatus st = JxlDecoderProcessInput(dec);
        if (st == JXL_DEC_BASIC_INFO) {
            JxlBasicInfo info;
            JxlDecoderGetBasicInfo(dec, &info);
            px.width = int(info.xsize);
            px.height = int(info.ysize);
            px.channels = info.alpha_bits > 0 ? 4 : 3;
            fmt.num_channels = uint32_t(px.channels);
        } else if (st == JXL_DEC_NEED_IMAGE_OUT_BUFFER) {
            px.data.resize(size_t(px.width) * size_t(px.height) * size_t(px.channels));
            JxlDecoderSetImageOutBuffer(dec, &fmt, px.data.data(), px.data.size());
        } else if (st == JXL_DEC_FULL_IMAGE || st == JXL_DEC_SUCCESS) {
            JxlDecoderDestroy(dec);
            return px;
        } else if (st == JXL_DEC_ERROR || st == JXL_DEC_NEED_MORE_INPUT) {
            JxlDecoderDestroy(dec);
            return Pixels{};
        }
    }
}

// --- арбитр ---------------------------------------------------------------

std::shared_ptr<jxl::ImageBundle> toBundle(const Pixels& px) {
    JxlMemoryManager* mm = jpegxl::tools::NoMemoryManager();
    auto created = jxl::Image3F::Create(mm, size_t(px.width), size_t(px.height));
    if (!created.ok()) return nullptr;
    jxl::Image3F img = std::move(created).value_();
    for (int y = 0; y < px.height; ++y) {
        const uint8_t* src = px.data.data() + size_t(y) * size_t(px.width) * size_t(px.channels);
        float* r = img.PlaneRow(0, size_t(y));
        float* g = img.PlaneRow(1, size_t(y));
        float* b = img.PlaneRow(2, size_t(y));
        for (int x = 0; x < px.width; ++x) {
            r[x] = src[x * px.channels + 0] / 255.0f;
            g[x] = src[x * px.channels + 1] / 255.0f;
            b[x] = src[x * px.channels + 2] / 255.0f;
        }
    }
    auto ib = std::make_shared<jxl::ImageBundle>(mm, new jxl::ImageMetadata());
    if (!ib->SetFromImage(std::move(img), jxl::ColorEncoding::SRGB(false))) return nullptr;
    return ib;
}

double ssimulacra2(const jxl::ImageBundle& a, const jxl::ImageBundle& b) {
    auto res = ComputeSSIMULACRA2(a, b);
    if (!res.ok()) return -1000.0;
    return std::move(res).value_().Score();
}

// --- отчёт ----------------------------------------------------------------

std::string kb(qsizetype bytes) {
    char buf[64];
    if (bytes >= 1024 * 1024)
        std::snprintf(buf, sizeof buf, "%.1f МБ", double(bytes) / 1048576.0);
    else
        std::snprintf(buf, sizeof buf, "%.0f КБ", double(bytes) / 1024.0);
    return buf;
}

void row(const char* what, const std::string& value, const Timing& t) {
    std::printf("  %-34s %-22s %8.1f мс   эталон %.2f–%.2f\n", what, value.c_str(), t.best,
                t.yardBest, t.yardWorst);
}

void bench(const QString& path, int runs, void* runner) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return;
    const QByteArray raw = f.readAll();
    f.close();

    QImageReader probe(path);
    probe.setAutoTransform(true);
    const QSize declared = probe.size();

    std::printf("\n### %s\n", QFileInfo(path).fileName().toUtf8().constData());
    std::printf("  файл %s, заголовок обещает %dx%d, формат %s\n", kb(raw.size()).c_str(),
                declared.width(), declared.height(), probe.format().constData());

    // 0. Декод входа. Он же — вход для всего остального.
    QImage img;
    const Timing tDecode = measure(runs, [&] {
        QImageReader r(path);
        r.setAutoTransform(true);
        img = r.read();
    });
    if (img.isNull()) {
        std::printf("  ДЕКОД НЕ УДАЛСЯ: %s\n", probe.errorString().toUtf8().constData());
        return;
    }
    row("декод входа (Qt)", std::to_string(img.width()) + "x" + std::to_string(img.height()),
        tDecode);

    const Pixels full = fromImage(img);
    if (!full.ok()) return;

    // 1. Проба на копии ~800 px: отношение lossless/lossy и её цена.
    const QImage small = img.scaled(800, 800, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    const Pixels probePx = fromImage(small);
    // На всех ядрах: проба — предсказание, и она обязана быть дешевле того,
    // что предсказывает. Однопоточная проба стоила 280–500 мс против 120–140
    // на целевой энкод, то есть предсказание было дороже измерения.
    QByteArray probeLossless, probeLossy;
    const Timing tProbe = measure(runs, [&] {
        probeLossless = encodeJxl(probePx, 0.0f, true, runner);
        probeLossy = encodeJxl(probePx, 90.0f, false, runner);
    });
    char ratio[64] = "—";
    if (!probeLossy.isEmpty())
        std::snprintf(ratio, sizeof ratio, "%.2f (%s / %s)",
                      double(probeLossless.size()) / double(probeLossy.size()),
                      kb(probeLossless.size()).c_str(), kb(probeLossy.size()).c_str());
    row("проба на ~800 px", ratio, tProbe);

    // 2. Энкод в ЦЕЛЕВОЙ размер — то, что конвейер делает на самом деле.
    //
    // Полное разрешение здесь мерить незачем: перед энкодом картинка уводится
    // в пиксельный бюджет S² с потолком стороны 3S. Замер полного разрешения
    // отвечал бы на вопрос, которого в конвейере нет (мой промах в первой
    // редакции пробника — владелец на него и указал).
    const QSize target = targetSize(img.size());
    const QImage scaled = target == img.size()
                              ? img
                              : img.scaled(target, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    const Pixels tgt = fromImage(scaled);
    const double mp = double(tgt.width) * double(tgt.height) / 1e6;
    std::printf("  цель %dx%d = %.2f Мп%s\n", tgt.width, tgt.height, mp,
                target == img.size() ? " (вписалась как есть, апскейла нет)" : "");

    QByteArray lossy;
    {
        const Timing t = measure(runs, [&] { lossy = encodeJxl(tgt, 90.0f, false, nullptr); });
        char v[96];
        std::snprintf(v, sizeof v, "%s, %.2f Мп/с", kb(lossy.size()).c_str(), mp / (t.best / 1000.0));
        row("энкод lossy q90 e5 (1 поток)", v, t);
    }
    {
        QByteArray out;
        const Timing t = measure(runs, [&] { out = encodeJxl(tgt, 90.0f, false, runner); });
        char v[96];
        std::snprintf(v, sizeof v, "%s, %.2f Мп/с", kb(out.size()).c_str(), mp / (t.best / 1000.0));
        row("энкод lossy q90 e5 (все ядра)", v, t);
    }
    {
        QByteArray out;
        const Timing t = measure(runs, [&] { out = encodeJxl(tgt, 0.0f, true, runner); });
        char v[96];
        std::snprintf(v, sizeof v, "%s, x%.1f от lossy", kb(out.size()).c_str(),
                      lossy.isEmpty() ? 0.0 : double(out.size()) / double(lossy.size()));
        row("энкод lossless e5 (все ядра)", v, t);
    }

    // 3. Транскод JPEG и обратная сборка — только для JPEG.
    if (probe.format().toLower() == "jpeg" || probe.format().toLower() == "jpg") {
        QByteArray tr;
        const Timing tTr = measure(runs, [&] { tr = transcodeJpeg(raw); });
        if (tr.isEmpty()) {
            row("транскод JPEG→JXL", "НЕ УДАЛСЯ", tTr);
        } else {
            char v[64];
            std::snprintf(v, sizeof v, "%s (%.0f%% от исходного)", kb(tr.size()).c_str(),
                          100.0 * double(tr.size()) / double(raw.size()));
            row("транскод JPEG→JXL", v, tTr);

            QByteArray back;
            const Timing tBack = measure(runs, [&] { back = reconstructJpeg(tr); });
            const bool same = (back == raw);
            row("реконструкция JPEG", same ? "БАЙТ В БАЙТ" : "РАЗОШЛАСЬ", tBack);
        }
    }

    // 4. Декод JXL против WebP.
    if (!lossy.isEmpty()) {
        Pixels back;
        const Timing tDecJxl = measure(runs, [&] { back = decodeJxl(lossy); });
        row("декод JXL", std::to_string(back.width) + "x" + std::to_string(back.height), tDecJxl);
    }

    QByteArray webp;
    {
        QBuffer buf(&webp);
        buf.open(QIODevice::WriteOnly);
        QImageWriter w(&buf, "webp");
        w.setQuality(90);
        w.write(scaled);   // тот же размер, что и у JXL, иначе сравнивать нечего
    }
    if (!webp.isEmpty()) {
        QImage wimg;
        const Timing tDecWebp = measure(runs, [&] {
            QBuffer buf(&webp);
            buf.open(QIODevice::ReadOnly);
            QImageReader r(&buf, "webp");
            wimg = r.read();
        });
        row("декод WebP q90 (для сравнения)", kb(webp.size()), tDecWebp);
    }

    // 5. Цена арбитра. Сравниваются картинки ОДНОГО размера — уменьшенный
    // оригинал против того, что вышло из энкодера: SSIMULACRA2 иначе не
    // считается, да и лестница сравнивает именно это.
    if (!lossy.isEmpty()) {
        const Pixels decoded = decodeJxl(lossy);
        auto a = toBundle(tgt);
        auto b = toBundle(decoded);
        if (a && b) {
            double score = 0.0;
            const Timing tS = measure(std::max(1, runs / 2), [&] { score = ssimulacra2(*a, *b); });
            char v[64];
            std::snprintf(v, sizeof v, "%.2f", score);
            row("SSIMULACRA2 (q90 против входа)", v, tS);
        }
    }
}

void collect(const QString& path, QStringList* out) {
    QFileInfo info(path);
    if (info.isDir()) {
        QDir dir(path);
        const auto names = dir.entryList(QDir::Files | QDir::NoDotAndDotDot, QDir::Name);
        for (const QString& n : names) collect(dir.filePath(n), out);
        const auto subs = dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
        for (const QString& n : subs) collect(dir.filePath(n), out);
    } else if (info.isFile()) {
        out->append(path);
    }
}

}  // namespace

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    if (argc < 2) {
        std::fprintf(stderr, "  taskset -c 0 image_bench <файл-или-каталог> [ещё...]\n");
        return 2;
    }

    // Потолок Qt на разжатую картинку: пробнику нужны и большие файлы, а
    // умолчание (256 МБ) режет 60-мегапиксельные фотографии владельца.
    QImageReader::setAllocationLimit(2048);

    const int runs = qEnvironmentVariableIntValue("ZAMETTI_BENCH_RUNS") > 0
                         ? qEnvironmentVariableIntValue("ZAMETTI_BENCH_RUNS")
                         : 3;

    QStringList files;
    for (int i = 1; i < argc; ++i) collect(QString::fromLocal8Bit(argv[i]), &files);

    void* runner = JxlThreadParallelRunnerCreate(
        nullptr, std::max(1u, std::thread::hardware_concurrency()));

    std::printf("# Пробник кодеков этапа 8\n");
    std::printf("\nминимум из %d прогонов; рядом разброс эталонного счётного цикла — "
                "если он поехал, числам верить нельзя\n",
                runs);
    // Грабли, на которые я наступил: под taskset -c 0 строка «все ядра» врёт,
    // потому что ядро одно. Однопоточные числа надо брать из прогона ПОД
    // taskset, многопоточное — из прогона без него.
    cpu_set_t mask;
    CPU_ZERO(&mask);
    const int cores = sched_getaffinity(0, sizeof mask, &mask) == 0 ? CPU_COUNT(&mask) : 0;
    std::printf("\nдоступно ядер процессу: %d%s\n", cores,
                cores == 1 ? "  → строка «все ядра» здесь бессмысленна, нужен прогон без taskset"
                           : "  → однопоточные строки надо перемерить под taskset -c 0");

    for (const QString& f : files) bench(f, runs, runner);

    JxlThreadParallelRunnerDestroy(runner);
    std::printf("\n(эталон в сумме: %.3f)\n", g_sink);
    return 0;
}
