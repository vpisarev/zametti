// Стенд отрисовки: сколько стоит нарисовать кадр тяжёлой заметки.
//
// В обязательный набор НЕ входит и ctest его не запускает: это не проверка, а
// замер, и смотрят на него тогда, когда есть подозрение, что правка замедлила
// отрисовку. Числа сравниваются с числами предыдущей сборки — своего эталона у
// стенда нет и быть не может, они зависят от машины.
//
//   taskset -c 0 ./tests/render_bench                — на синтетике
//   taskset -c 0 ./tests/render_bench заметка.md ...  — на настоящих заметках
//
// taskset обязателен, а не для красоты. У гибридных процессоров планировщик
// гоняет процесс между P- и E-ядрами, и без закрепления числа скачут вдвое:
// на первом же прогоне этого стенда без него «до» и «после» поменялись
// местами, и я успел поверить, что правка всё замедлила. С закреплением на том
// же материале — 2.66 против 1.30 мс, повторяемо от круга к кругу.
//
// Мерить надо ОФФСКРИН, и стенд сам ставит offscreen, если платформу не задали.
// На живом экране доставка кадра стоит 4–6 мс со всплесками до 90 (замер на
// окне 2560x1400), и на этом фоне работа самой отрисовки не видна вовсе: вынос
// метрик шрифта из циклов дал 2.86 -> 1.45 мс, и на экране этой разницы не
// было заметно ни в одном из трёх кругов.
//
// Тяжесть для отрисовки — это НЕ картинки. Дорого обходится текст: разные
// гарнитуры и кегли, маркеры списков, подложка блоков кода, черты. Синтетика
// ниже собрана из них.

#include "editor_widget.h"
#include "settings.h"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QScrollBar>
#include <QTest>
#include <QTextDocument>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

namespace {

// Страница, тяжёлая ровно тем, чем тяжелы настоящие заметки: заголовки всех
// уровней (у каждого свой кегль), вложенные списки трёх видов с маркерами,
// блоки кода (своя гарнитура, свой кегль, подложка), цитаты, черты и разметка
// внутри строки.
std::string heavyNote(int sections) {
    std::string out = "# Стенд отрисовки\n\n";
    for (int i = 0; i < sections; ++i) {
        const std::string n = std::to_string(i);
        out += "## Раздел " + n + "\n\n";
        out += "Абзац с **жирным**, *курсивом*, ~~зачёркнутым~~ и `кодом в строке`,\n";
        out += "и ещё одна строка того же абзаца, чтобы он переносился.\n\n";
        out += "### Подзаголовок " + n + "\n\n";
        out += "- буллет верхнего уровня\n";
        out += "  - вложенный буллет\n";
        out += "    - и ещё глубже\n";
        out += "- [ ] невыполненная задача\n";
        out += "- [x] выполненная задача\n\n";
        out += "1. первый нумерованный\n";
        out += "2. второй нумерованный\n";
        out += "   1. вложенный нумерованный\n\n";
        out += "> цитата в две строки,\n> вторая строка цитаты\n\n";
        out += "```cpp\n";
        for (int k = 0; k < 8; ++k)
            out += "    const int value" + std::to_string(k) + " = " + std::to_string(k) + ";\n";
        out += "```\n\n";
        out += "---\n\n";
    }
    return out;
}

struct Sample {
    QString name;
    int blocks = 0;
    double median = 0;
    double p90 = 0;
    double p99 = 0;
    double worst = 0;
    int overBudget = 0;   // кадров дороже 16 мс — это промах кадра при 60 Гц
};

Sample measure(zametti::NoteEditor& editor, const QString& path) {
    editor.openFile(path);
    QTest::qWait(40);

    Sample sample;
    sample.name = QFileInfo(path).fileName();
    sample.blocks = editor.document()->blockCount();

    std::vector<double> times;
    const int max = editor.verticalScrollBar()->maximum();
    // Шаг мелкий: так кадров много и медиана устойчива.
    for (int at = 0; at <= max; at += 40) {
        editor.verticalScrollBar()->setValue(at);
        QElapsedTimer timer;
        timer.start();
        // Полная перерисовка нарочно: меряем работу отрисовки, а не то, сколько
        // Qt сумел сблитить.
        editor.viewport()->repaint();
        times.push_back(double(timer.nsecsElapsed()) / 1e6);
    }
    if (times.empty()) return sample;

    std::sort(times.begin(), times.end());
    sample.median = times[times.size() / 2];
    sample.p90 = times[times.size() * 9 / 10];
    sample.p99 = times[times.size() * 99 / 100];
    sample.worst = times.back();
    sample.overBudget =
        int(std::count_if(times.begin(), times.end(), [](double ms) { return ms > 16.0; }));
    return sample;
}

}  // namespace

int main(int argc, char** argv) {
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    zametti::loadAppearance(nullptr);

    zametti::NoteEditor editor;
    editor.resize(1000, 900);
    editor.show();
    QTest::qWait(40);

    QStringList notes;
    for (int i = 1; i < argc; ++i) notes << QString::fromLocal8Bit(argv[i]);
    if (notes.isEmpty()) {
        const QString dir = QDir::tempPath() + QStringLiteral("/zametti-render-bench");
        QDir().mkpath(dir);
        for (int sections : {20, 80}) {
            const QString path =
                dir + QStringLiteral("/синтетика-%1.md").arg(sections);
            std::ofstream out(path.toStdString(), std::ios::binary);
            const std::string text = heavyNote(sections);
            out.write(text.data(), qint64(text.size()));
            notes << path;
        }
    }

    std::printf("%-28s %7s %8s %8s %8s %8s %9s\n", "заметка", "блоков", "медиана", "90%", "99%",
                "худший", ">16 мс");
    for (const QString& path : notes) {
        const Sample s = measure(editor, path);
        std::printf("%-28s %7d %8.2f %8.2f %8.2f %8.2f %9d\n",
                    s.name.toUtf8().constData(), s.blocks, s.median, s.p90, s.p99, s.worst,
                    s.overBudget);
        std::fflush(stdout);
    }
    return 0;
}
