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
//   zametti-bench switch        переключение заметок в настоящем хранилище, по фазам
//   zametti-bench write-probe   цена одной пробы разметки: шрифт, документ, md4c
//   zametti-bench setdoc-probe  что стоит QTextEdit::setDocument на свёрстанном документе
//   zametti-bench zoom          ВОРОТА к ZDocument: что делает Ctrl+= с документом
//   zametti-bench md-spaces     обычные ведущие пробелы против md4c (NOINDENTEDCODEBLOCKS)
//   zametti-bench ui-metrics    из чего выводить размер иконки и строки
//   zametti-bench undo          штатный стек отмены: чем именно мы его теряем
//   zametti-bench paste         цена вставки против размера заметки
//   zametti-bench inline        строчная формула-объект: посадка, дедупликация
//   zametti-bench source        цена выхода из режима правки исходника
//   zametti-bench argon2        калибровка Argon2id на этой машине (m17)
//   zametti-bench keyring       ручная приёмка Secret Service (m17)
//   zametti-bench webdav        живая приёмка адаптера против настоящего сервера

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
int ztStartBench(int argc, char** argv);
int ztFontProbe(int argc, char** argv);
int ztZoomProbe(int argc, char** argv);
int ztMdSpacesProbe(int argc, char** argv);
int ztUiMetricsProbe(int argc, char** argv);
int ztUndoProbe(int argc, char** argv);
int ztPasteBench(int argc, char** argv);
int ztBigBench(int argc, char** argv);
int ztSwitchBench(int argc, char** argv);
int ztWriteProbe(int argc, char** argv);
int ztSetDocProbe(int argc, char** argv);
int ztInlineFormulaProbe(int argc, char** argv);
int ztSourceBench(int argc, char** argv);
int ztArgon2Probe(int argc, char** argv);
int ztKeyringProbe(int argc, char** argv);
int ztWebDavProbe(int argc, char** argv);

namespace {

struct Bench {
    const char* name;
    int (*run)(int, char**);
};

const Bench kBenches[] = {
    {"ir", ztIrBench},
    {"source", ztSourceBench},
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
    {"start", ztStartBench},
    {"fonts", ztFontProbe},
    {"zoom", ztZoomProbe},
    {"md-spaces", ztMdSpacesProbe},
    {"ui-metrics", ztUiMetricsProbe},
    {"undo", ztUndoProbe},
    {"paste", ztPasteBench},
    {"big", ztBigBench},
    {"switch", ztSwitchBench},
    {"write-probe", ztWriteProbe},
    {"setdoc-probe", ztSetDocProbe},
    {"inline", ztInlineFormulaProbe},
    {"argon2", ztArgon2Probe},
    {"keyring", ztKeyringProbe},
    {"webdav", ztWebDavProbe},
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
