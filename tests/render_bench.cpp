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
// ПРО ПИКИ ПРИ ПРОКРУТКЕ (замер 10.08.2026, Ficus Tutorial: 4182 блока, 37000
// слов, одна картинка, окно 2560x1400). Владелец видел рывки при плавной
// прокрутке. Что установлено и что закрыто:
//
//   медиана 4.3 мс, 99% 5.5, второй-худший кадр всегда ~6.4 — и раз в
//   несколько запусков ОДИН кадр на 16-22 мс;
//   пик не наш: под временным ключом, отключавшим всю нашу дорисовку, пики
//   ровно те же (наша дорисовка — 1.3 мс из 4.25, то есть 30% ровного времени,
//   но ни разу не источник рывка);
//   пик не от ленивой разметки Qt: documentLayout()->documentSize() при
//   открытии возвращается за 0.0 мс (ничего не размечает), и пики с ним и без
//   него одинаковы — 8 запусков на сторону. Три запуска на сторону показывали
//   «разницу», которой нет: на этом я успел объявить эффект и ошибиться;
//   пик не от машины: ЭТАЛОН (счётный цикл ниже) на кадре в 19.22 мс показывает
//   те же 0.175 мс, что и на спокойных. Время потрачено внутри отрисовки;
//   место каждый раз новое (блоки 117, 1116, 2379, 3529, 3916 в разных
//   запусках) и с содержимым не связано;
//   а вот от чего он зависит — не найдено. Одно наблюдение без объяснения:
//   стоило поставить между кадрами эталонный цикл, как весь хвост потяжелел
//   (запусков с пиком стало 6 из 6 вместо 2 из 6, второй-худший кадр вырос с
//   6.4 до 9-13 мс). Списать это на вытеснение кэша нельзя: цикл считает в
//   одном регистре и памяти не трогает. Механизм неизвестен.
//
// Дальше своими силами не продвинуться: пик редкий, одиночный, ни к месту в
// документе, ни к нашему коду, ни к состоянию машины не привязан. Решение
// владельца — вернуться к вопросу на слабой машине, где рывок, если он
// настоящий, будет виден крупнее.
//
// Тяжесть для отрисовки — это НЕ картинки. Дорого обходится текст: разные
// гарнитуры и кегли, маркеры списков, подложка блоков кода, черты. Синтетика
// ниже собрана из них.

#include "editor_widget.h"
#include "settings.h"

#include <QAbstractTextDocumentLayout>
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

// selected — мерить С ВЫДЕЛЕНИЕМ всего документа. Нужно потому, что часть
// отрисовки работает только при выделении (подложка кода поверх него), и без
// этого её цена не видна вовсе.
Sample measure(zametti::NoteEditor& editor, const QString& path, bool selected = false) {
    editor.openFile(path);
    QTest::qWait(40);
    // Ленивая разметка Qt: блоки размечаются, когда впервые попадают в кадр, и
    // это ровно то, что даёт рывки при прокрутке. Спрашиваем размер документа —
    // Qt вынуждена разметить его целиком; ключом можно померить и без этого.

    if (selected) {
        QTextCursor all = editor.textCursor();
        all.select(QTextCursor::Document);
        editor.setTextCursor(all);
        QTest::qWait(20);
    }

    Sample sample;
    sample.name = QFileInfo(path).fileName() + (selected ? QStringLiteral(" [выделено]") : QString());
    sample.blocks = editor.document()->blockCount();

    std::vector<double> times;
    // Пики важнее среднего: рывок на одном кадре из ста человек видит, а
    // ровные 2.6 мс — нет. Поэтому запоминаем не только время, но и МЕСТО.
    std::vector<std::pair<double, int>> byPlace;
    // ЭТАЛОН рядом с каждым кадром: постоянный счётный цикл. Сам по себе он не
    // нужен, нужен его разброс. Если на «плохом» кадре подскочил и эталон —
    // подскочила машина, а не отрисовка, и объяснять тут нечего.
    std::vector<double> yard;
    double sink = 0;
    const int max = editor.verticalScrollBar()->maximum();
    // Шаг мелкий: так кадров много и медиана устойчива.
    for (int at = 0; at <= max; at += 40) {
        editor.verticalScrollBar()->setValue(at);
        QElapsedTimer timer;
        timer.start();
        // Полная перерисовка нарочно: меряем работу отрисовки, а не то, сколько
        // Qt сумел сблитить.
        editor.viewport()->repaint();
        const double ms = double(timer.nsecsElapsed()) / 1e6;
        times.push_back(ms);
        byPlace.emplace_back(ms, at);

        QElapsedTimer bar;
        bar.start();
        double acc = 0;
        for (int i = 1; i < 200000; ++i) acc += 1.0 / double(i);
        sink = acc;
        yard.push_back(double(bar.nsecsElapsed()) / 1e6);
    }

    // Итог эталонного цикла нужен только затем, чтобы оптимизатор его не выбросил.
    if (sink < 0) std::printf("этого не бывает\n");

    // Пять худших кадров: где стояла прокрутка и что там было видно.
    std::sort(byPlace.begin(), byPlace.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });
    std::printf("  худшие кадры (%s):\n", sample.name.toUtf8().constData());
    std::vector<double> calm = yard;
    std::sort(calm.begin(), calm.end());
    const double yardTypical = calm.empty() ? 0.0 : calm[calm.size() / 2];
    std::printf("    эталон: медиана %.3f мс, худший %.3f мс\n", yardTypical,
                calm.empty() ? 0.0 : calm.back());
    for (size_t i = 0; i < byPlace.size() && i < 5; ++i) {
        editor.verticalScrollBar()->setValue(byPlace[i].second);
        const QAbstractTextDocumentLayout* layout = editor.document()->documentLayout();
        const int top = layout->hitTest(QPointF(0, byPlace[i].second), Qt::FuzzyHit);
        const QTextBlock block = editor.document()->findBlock(top);
        const size_t at = size_t(byPlace[i].second / 40);
        std::printf("    %6.2f мс  эталон %5.3f  прокрутка %6d  блок %5d  [%s]\n",
                    byPlace[i].first, at < yard.size() ? yard[at] : 0.0, byPlace[i].second,
                    block.blockNumber(), block.text().left(40).toUtf8().constData());
    }
    std::fflush(stdout);
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

int ztRenderBench(int argc, char** argv) {
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "offscreen");
    zametti::loadAppearance(nullptr);

    zametti::NoteEditor editor;
    // Размер окна — переменной среды: цена кадра растёт вместе с ним, а у
    // владельца экран 2560x1400, и мерить на игрушечном окне значит мерить не
    // его беду.
    const int width = qEnvironmentVariableIntValue("ZAMETTI_BENCH_W") > 0
                          ? qEnvironmentVariableIntValue("ZAMETTI_BENCH_W")
                          : 1000;
    const int height = qEnvironmentVariableIntValue("ZAMETTI_BENCH_H") > 0
                           ? qEnvironmentVariableIntValue("ZAMETTI_BENCH_H")
                           : 900;
    editor.resize(width, height);
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
        const auto show = [](const Sample& s) {
            std::printf("%-28s %7d %8.2f %8.2f %8.2f %8.2f %9d\n",
                        s.name.toUtf8().constData(), s.blocks, s.median, s.p90, s.p99,
                        s.worst, s.overBudget);
            std::fflush(stdout);
        };
        show(measure(editor, path));
        // И то же самое с выделением всего документа: подложка кода поверх
        // выделения рисуется только тогда, и её цена иначе не видна.
        show(measure(editor, path, true));
    }
    return 0;
}
