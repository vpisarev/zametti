// Лестница качества: каждая ступень отдельно.
//
// Настоящих файлов тут почти нет, и намеренно: чтобы попасть ровно в третью
// ступень, нужен вход, который перелетает бюджет ровно настолько. Такие входы
// сочиняются, а не ищутся. Настоящие файлы проверяют таблицу решений — это
// другой набор.
//
// Ступени задаются не кодом лестницы, а бюджетом: одна и та же картинка при
// разных бюджетах обязана проходить разное число ступеней. Это и проверяется.

#include "ladder.h"
#include "resample.h"

#include "test_util.h"

#include <QGuiApplication>
#include <QImageReader>
#include <QImage>
#include <QPainter>

#include <algorithm>
#include <cmath>
#include <string>

using namespace zametti;

namespace {

std::string num(long long v) { return std::to_string(v); }

// Шум сжимается плохо: на нём легко получить любой перелёт бюджета.
QImage noise(int w, int h, unsigned seed = 7) {
    QImage img(w, h, QImage::Format_RGBX8888);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            seed = seed * 1664525u + 1013904223u;
            auto* p = img.scanLine(y) + size_t(x) * 4;
            p[0] = uint8_t(seed >> 24);
            p[1] = uint8_t(seed >> 16);
            p[2] = uint8_t(seed >> 8);
            p[3] = 255;
        }
    return img;
}

// Плавный переход сжимается прекрасно — на нём лестница не должна начинаться.
QImage smooth(int w, int h) {
    QImage img(w, h, QImage::Format_RGBX8888);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            auto* p = img.scanLine(y) + size_t(x) * 4;
            p[0] = uint8_t(255 * x / w);
            p[1] = uint8_t(255 * y / h);
            p[2] = 128;
            p[3] = 255;
        }
    return img;
}

void checkNoStepsWhenFits() {
    // Качество задаётся ЯВНО: набор проверяет саму лестницу, а не умолчание
    // конфига. За умолчание отвечает import_limits_test — иначе его правка
    // красила бы здесь всё подряд и прятала настоящую поломку.
    ImportLimits limits;
    limits.quality = 90;
    limits.maxFileSizeMb = 4;   // щедрый бюджет
    const LadderResult r = runLadder(smooth(800, 600), EncodeMeta{}, limits);
    ZT_TRUE("влезло: " + r.error.toStdString(), r.ok());
    ZT_EQ("ступеней не потребовалось", num(0), num(r.steps));
    ZT_EQ("энкод ровно один", num(1), num(r.encodes));
    ZT_EQ("качество не понижали", num(90), num(r.quality));
    // Арбитр стоит как четыре энкода — звать его, когда всё влезло, расточительно.
    ZT_EQ("арбитра не звали", num(0), num(static_cast<long long>(r.ssimulacra2)));
    ZT_TRUE("размер не тронут", r.width == 800 && r.height == 600);
}

// Ступени качества и разрешения проверяются на НАСТОЯЩЕЙ фотографии.
// Синтетика тут не годится: любая рукотворная фактура ведёт себя как шум —
// понижение качества обваливает оценку до полусотни, и арбитр честно
// откатывает всё назад, то есть ступень не проверяется вовсе. На настоящих
// снимках q80 даёт больше восьмидесяти (замерено на выборке в 311 файлов).
QImage realPhoto(const QString& root) {
    if (root.isEmpty()) return {};
    QImageReader r(root + "/photo/wallpaper-4mp.jpg");
    r.setAutoTransform(true);
    const QImage full = r.read();
    if (full.isNull()) return {};
    // Уменьшаем до целевого размера — лестница получает именно такое.
    return resampleArea(full, 1600, full.height() * 1600 / full.width());
}

void checkQualityStep(const QString& root) {
    ImportLimits limits;
    limits.quality = 90;   // явно: набор про лестницу, не про умолчание
    const QImage src = realPhoto(root);
    if (src.isNull()) return;   // без корпуса эту ступень не проверить
    // Сколько весит при обычном качестве — от этого и пляшем.
    EncodeOptions opt;
    QString err;
    const qint64 full = encodeJxl(src, opt, EncodeMeta{}, &err).size();
    ZT_TRUE("исходный энкод получился", full > 0);

    // Ставим бюджет так, чтобы первая ступень НЕ влезла даже с запасом в
    // полтора раза, а следующая влезла. Бюджет дробный — иначе в эту щель не
    // попасть вовсе.
    limits.maxFileSizeMb = double(full) / (1024.0 * 1024.0) / kBudgetSlack * 0.9;

    const LadderResult r = runLadder(src, EncodeMeta{}, limits);
    ZT_TRUE("лестница отработала: " + r.error.toStdString(), r.ok());
    ZT_TRUE("ступеней хотя бы одна (" + num(r.steps) + ")", r.steps >= 1);
    ZT_TRUE("качество понижено (" + num(r.quality) + ")", r.quality < 90);
    ZT_TRUE("ниже пола не опускались", r.quality >= kQualityFloor);
    ZT_TRUE("арбитра позвали, раз отступали", r.ssimulacra2 != 0);
    ZT_TRUE("на фактуре арбитр не отказал (" + std::to_string(int(r.ssimulacra2)) + ")",
            !r.arbiterRolledBack);
}

void checkResolutionStepAndFloor(const QString& root) {
    // Совсем тесный бюджет: обязаны пройти и качество, и разрешение, и пол.
    ImportLimits limits;
    limits.quality = 90;   // явно: набор про лестницу, не про умолчание
    limits.maxFileSizeMb = 0.05;   // 50 КБ: сюда фотография не влезет никак
    const QImage src = realPhoto(root);
    if (src.isNull()) return;

    const LadderResult r = runLadder(src, EncodeMeta{}, limits);
    ZT_TRUE("лестница отработала: " + r.error.toStdString(), r.ok());
    ZT_TRUE("прошли не одну ступень (" + num(r.steps) + ")", r.steps >= 2);
    ZT_TRUE("картинка уменьшена (" + num(r.width) + "x" + num(r.height) + ")",
            r.width < src.width() || r.height < src.height());
    ZT_TRUE("ниже пола качества не опускались (" + num(r.quality) + ")",
            r.quality >= kQualityFloor);
    ZT_TRUE("энкодов не больше шести (" + num(r.encodes) + ")", r.encodes <= 6);
}

// Арбитр не украшение: на входе, который от сжатия рассыпается, он обязан
// вернуть ступень назад и принять перелёт бюджета. Чистый шум — как раз такой
// вход: любое понижение качества обваливает оценку.
void checkArbiterRollsBack() {
    ImportLimits limits;
    limits.quality = 90;   // явно: набор про лестницу, не про умолчание
    const QImage src = noise(700, 500);
    EncodeOptions opt;
    QString err;
    const qint64 full = encodeJxl(src, opt, EncodeMeta{}, &err).size();
    limits.maxFileSizeMb = double(full) / (1024.0 * 1024.0) / kBudgetSlack * 0.9;

    const LadderResult r = runLadder(src, EncodeMeta{}, limits);
    ZT_TRUE("лестница отработала", r.ok());
    ZT_TRUE("на шуме арбитр вернул ступень назад", r.arbiterRolledBack);
    ZT_TRUE("и принял перелёт бюджета", r.overBudget);
    ZT_EQ("вернулись к исходному качеству", num(90), num(r.quality));
    ZT_TRUE("оценка была ниже нижнего порога (" + std::to_string(int(r.ssimulacra2)) + ")",
            r.ssimulacra2 <= kArbiterReject);
}

void checkArbiterOnAlpha() {
    // САМОЕ ВАЖНОЕ ЗДЕСЬ. На картинке с прозрачностью арбитр без наложения на
    // фон выдаёт бессмыслицу: под прозрачными областями цвета нет. Замер на
    // настоящем прозрачном PNG дал -403.84. Проверяем, что наложение на фон
    // спасает — оценка обязана быть в разумных пределах.
    QImage withAlpha(400, 300, QImage::Format_RGBA8888);
    withAlpha.fill(QColor(0, 0, 0, 0));
    QPainter p(&withAlpha);
    p.fillRect(50, 50, 200, 150, QColor(200, 60, 40, 255));
    p.fillRect(120, 100, 200, 150, QColor(40, 120, 200, 128));
    p.end();

    const double same = compareSsimulacra2(withAlpha, withAlpha);
    ZT_TRUE("сама с собой — сто (" + std::to_string(int(same)) + ")", same > 99.0);

    // Слегка испорченная копия обязана дать НЕ бессмыслицу, а число в шкале.
    QImage worse = withAlpha;
    QPainter p2(&worse);
    p2.fillRect(0, 0, 20, 20, QColor(255, 255, 255, 255));
    p2.end();
    const double score = compareSsimulacra2(withAlpha, worse);
    ZT_TRUE("с альфой оценка осмысленна, а не -400 (" + std::to_string(int(score)) + ")",
            score > -50.0 && score < 100.0);
}

void checkComparisonSanity() {
    const QImage a = smooth(200, 150);
    ZT_TRUE("одинаковые дают сотню", compareSsimulacra2(a, a) > 99.0);
    ZT_TRUE("разные размеры — не сравниваем",
            compareSsimulacra2(a, smooth(100, 75)) < -900.0);
    ZT_TRUE("пустая — не сравниваем", compareSsimulacra2(a, QImage()) < -900.0);

    // Шум против гладкого — заведомо плохо, и оценка обязана это показать.
    ZT_TRUE("непохожие дают низкую оценку",
            compareSsimulacra2(a, noise(200, 150)) < 30.0);
}

}  // namespace

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    QImageReader::setAllocationLimit(2048);
    const QString root = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QString();
    checkNoStepsWhenFits();
    checkQualityStep(root);
    checkResolutionStepAndFloor(root);
    checkArbiterRollsBack();
    checkArbiterOnAlpha();
    checkComparisonSanity();
    return zt::report("лестница качества");
}
