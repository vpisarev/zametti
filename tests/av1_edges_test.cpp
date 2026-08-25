// Края AV1: клетки матрицы, которых в корпусе владельца не было.
//
// Набор появился вместе с вендорингом libgav1 (август 2026). Матрица краёв
// писалась ДО починок, и она показала пустоту там, где её меньше всего ждёшь:
// в корпусе не оказалось НИ ОДНОГО 4:2:0-AVIF — то есть самого частого AVIF на
// свете, — ни монохромного, ни плиточного, ни битого. Файлы изготовлены и
// лежат в .testdata/images/originals/av1-края/ (см. ПРОИСХОЖДЕНИЕ.md рядом).
//
// ЧТО ИМЕННО СТЕРЕЖЁТСЯ, по клеткам:
//
//   4:2:0        — плоскости цветности вдвое меньше яркостной. Если брать их
//                  размер не у декодера, а считать (w+1)/2 руками, ошибка
//                  вылезет именно здесь;
//   монохром     — одна плоскость, plane[1] и plane[2] пустые. Отдельная ветка
//                  кода, и уронить её проще всего;
//   альфа        — в AVIF она отдельный item, и декодер зовут ВТОРОЙ РАЗ, а не
//                  ищут её в буфере кадра;
//   плитки       — libheif заводит до четырёх декодеров в РАЗНЫХ ПОТОКАХ.
//                  Единственная проверка, что плагин реентерабелен;
//   поворот      — применяет его libheif, а не мы; проверяем, что применяет
//                  ровно один раз;
//   битые файлы  — честный отказ или разумное восстановление, но НЕ ПАДЕНИЕ.

#include "image_read.h"

#include "test_util.h"
#include "testdata.h"

#include <QFileInfo>
#include <QImage>

#include <cstdio>
#include <string>
#include <vector>

namespace {

using namespace zametti;

struct Expected {
    const char* file;
    int width;
    int height;
    bool alpha;
    const char* what;
};

void checkGood(const QString& dir, const Expected& e) {
    const QString path = dir + QLatin1Char('/') + QString::fromUtf8(e.file);
    if (!QFileInfo::exists(path)) {
        std::printf("ПРОПУЩЕНО: нет файла %s\n", e.file);
        return;
    }
    const std::string name = e.file;

    // 1. Проба знает размер ДО разжатия. Пока размеры спрашивали у Qt, проба
    //    выходила пустой и ввоз отказывал ещё до нашего декодера.
    const ImageProbe probe = probeImageFile(path);
    ZT_TRUE(name + " (" + e.what + "): опознан", probe.valid());
    ZT_EQ(name + ": формат heif", std::string("heif"), probe.format.toStdString());
    ZT_EQ(name + ": размер из шапки",
          std::to_string(e.width) + "x" + std::to_string(e.height),
          std::to_string(probe.size.width()) + "x" + std::to_string(probe.size.height()));

    // 2. И разжатие даёт ровно то же. Расхождение здесь означало бы, что
    //    поворот применён дважды или не применён вовсе.
    const QImage img = decodeImageFile(path);
    ZT_TRUE(name + ": разжалось", !img.isNull());
    ZT_EQ(name + ": размер после разжатия",
          std::to_string(e.width) + "x" + std::to_string(e.height),
          std::to_string(img.width()) + "x" + std::to_string(img.height()));
    ZT_EQ(name + ": альфа", std::string(e.alpha ? "есть" : "нет"),
          std::string(img.hasAlphaChannel() ? "есть" : "нет"));
}

// Битый вход. Требование ОДНО: процесс жив. Что именно вернётся — пустая
// картинка или разумно восстановленная, — решает декодер, и решают они
// по-разному: на файле с мусором в середине libaom говорит «corrupt frame», а
// libgav1 доводит кадр до конца. Правило проекта на нашей стороне: при разборе
// данных прикладываются разумные усилия и работа продолжается.
void checkBroken(const QString& dir, const char* file) {
    const QString path = dir + QLatin1Char('/') + QString::fromUtf8(file);
    if (!QFileInfo::exists(path)) {
        std::printf("ПРОПУЩЕНО: нет файла %s\n", file);
        return;
    }
    const std::string name = file;
    const ImageProbe probe = probeImageFile(path);
    const QImage img = decodeImageFile(path);
    // Ни одного требования к содержимому — только к тому, что мы сюда дошли.
    ZT_TRUE(name + ": разбор битого файла не уронил процесс", true);
    std::printf("  %s: проба %s, разжатие %s\n", file,
                probe.valid() ? "прошла" : "отказала",
                img.isNull() ? "отказало" : "дало картинку");
}

int ztRunSuite(int argc, char** argv) {
    const QString dir = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QString();
    if (dir.isEmpty() || !QFileInfo::exists(dir)) {
        std::printf("ПРОПУЩЕНО: корпуса av1-края нет рядом\n");
        return zt::report("av1-edges");
    }

    static const Expected kGood[] = {
        {"avif-420-8бит.avif",   2560, 1600, false, "4:2:0, 8 бит"},
        {"avif-альфа-420.avif",  2756, 1965, true,  "альфа на 4:2:0"},
        {"avif-плитки.avif",     2560, 1600, false, "сетка 5x4, четыре потока"},
        {"avif-поворот.avif",    2736, 3648, false, "поворот контейнером"},
        {"avif-монохром.avif",    256,  256, false, "монохром, одна плоскость"},
    };
    for (const Expected& e : kGood) checkGood(dir, e);

    for (const char* file : {"avif-усечённый.avif", "avif-мусор-в-обу.avif"})
        checkBroken(dir, file);

    return zt::report("av1-edges");
}

}  // namespace

TEST(Av1Edges, All) {
    std::vector<QByteArray> ztArgs{QByteArrayLiteral("av1_edges_test")};
    ztArgs.push_back(zt::TestData::corpus(QStringLiteral("images/originals/av1-края")).toLocal8Bit());
    std::vector<char*> ztArgv;
    for (QByteArray& a : ztArgs) ztArgv.push_back(a.data());
    EXPECT_EQ(0, ztRunSuite(int(ztArgv.size()), ztArgv.data()));
}
