// Стенды и пробники: одна программа, подкоманда первым доводом.
//
// Это НЕ наборы. Они меряют, а не проверяют, и в общий процесс проверок не
// идут: у набора ответ «да или нет», у стенда — число, на которое надо
// смотреть. Гоняются руками, когда есть подозрение, что правка что-то
// замедлила или испортила.
//
// Отдельным исполняемым файлом на каждый — как было раньше — плодить незачем:
// одиннадцать целей ради одиннадцати main это ровно та россыпь, от которой
// уборка и затевалась.
//
//   zametti-bench ir            представление IR: аллокации, время, память
//   zametti-bench history       журнал: чтение поколений, восстановление
//   zametti-bench image         кодеки: качество против байтов и времени
//   zametti-bench render        отрисовка кадра
//   zametti-bench quality       подбор параметров записи картинок
//   zametti-bench formula       вёрстка формул
//   zametti-bench formula-sheet лист формул целиком
//   zametti-bench cmyk          путь CMYK через профиль печати
//   zametti-bench display-scale плотность экрана и масштаб
//   zametti-bench matrix        матрица правок
//   zametti-bench open          открытие заметки

#include <QApplication>

#include <cstdio>
#include <cstring>

int ztIrBench(int argc, char** argv);
int ztHistoryBench(int argc, char** argv);
int ztImageBench(int argc, char** argv);
int ztRenderBench(int argc, char** argv);
int ztQualityStudy(int argc, char** argv);
int ztFormulaProbe(int argc, char** argv);
int ztFormulaSheetProbe(int argc, char** argv);
int ztCmykProbe(int argc, char** argv);
int ztDisplayScaleProbe(int argc, char** argv);
int ztMatrixProbe(int argc, char** argv);
int ztOpenProbe(int argc, char** argv);

namespace {

struct Bench {
    const char* name;
    int (*run)(int, char**);
};

const Bench kBenches[] = {
    {"ir", ztIrBench},
    {"history", ztHistoryBench},
    {"image", ztImageBench},
    {"render", ztRenderBench},
    {"quality", ztQualityStudy},
    {"formula", ztFormulaProbe},
    {"formula-sheet", ztFormulaSheetProbe},
    {"cmyk", ztCmykProbe},
    {"display-scale", ztDisplayScaleProbe},
    {"matrix", ztMatrixProbe},
    {"open", ztOpenProbe},
};

int usage() {
    std::printf("стенды и пробники. Подкоманда первым доводом:\n\n");
    for (const Bench& b : kBenches) std::printf("  %s\n", b.name);
    std::printf("\nОстальные доводы уходят стенду как есть.\n");
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    // Платформа — до создания приложения, как и у наборов: стенды гоняют и на
    // машинах без экрана. Заданную снаружи не перебиваем — снимки под Xvfb
    // делаются именно так.
    if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    if (argc < 2) return usage();
    for (const Bench& b : kBenches) {
        if (std::strcmp(argv[1], b.name) == 0) {
            // Стенду отдаём доводы так, будто его звали напрямую: имя
            // программы плюс всё, что после подкоманды.
            argv[1] = argv[0];
            return b.run(argc - 1, argv + 1);
        }
    }
    std::printf("неизвестный стенд: %s\n\n", argv[1]);
    return usage();
}
