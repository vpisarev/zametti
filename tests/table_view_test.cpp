// Раскладка таблицы: ширины, выравнивания, вписывание в место.
//
// Здесь же замер, которого требует бриф: разбор и раскладка таблицы 50×8 не
// должны стать вторым «затыком прокрутки». Ключ --bench.
//
// Инвариант D из брифа («рендер не искажает содержимое») спрашивается прямо:
// текст, попавший в разметку ячейки, сверяется с текстом разбора.

#include "parser.h"
#include "table.h"
#include "table_view.h"
#include "settings.h"
#include "test_util.h"
#include "testdata.h"

#include <QElapsedTimer>
#include <QTextLayout>

#include <memory>
#include <vector>
#include <QGuiApplication>

#include <string>

namespace {

std::string n(int v) { return std::to_string(v); }
std::string num(qreal v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.1f", double(v));
    return buf;
}

const char* kSmall =
    "| имя | цена | наличие |\n"
    "|:---|---:|:---:|\n"
    "| болт | 10 | да |\n"
    "| очень длинное имя детали | 1000 | нет |\n";

zametti::TableSpace roomFor(qreal column, qreal full) {
    zametti::TableSpace space;
    space.columnWidth = column;
    space.fullWidth = full;
    space.zoom = 1.0;
    return space;
}

void checkWidths() {
    const zametti::Table table = zametti::parseTable(kSmall);
    const zametti::TableLayout out = zametti::layoutTable(table, roomFor(2000, 2000));

    ZT_EQ("колонок", n(3), n(out.columns));
    ZT_EQ("рядов вместе с шапкой", n(3), n(out.rows));
    ZT_EQ("ячеек", n(9), n(int(out.cells.size())));
    ZT_TRUE("в просторном месте не ужимали", out.scale == 1.0);
    ZT_TRUE("и переноса не было", !out.wrapped);

    // Ширина колонки — от самой длинной ячейки. Первая колонка содержит
    // «очень длинное имя детали» и обязана быть шире остальных.
    ZT_TRUE("первая колонка шире второй: " + num(out.columnWidth.at(0)) + " против " +
                num(out.columnWidth.at(1)),
            out.columnWidth.at(0) > out.columnWidth.at(1));
    ZT_TRUE("ширина таблицы — сумма колонок",
            qAbs(out.width - (out.columnWidth.at(0) + out.columnWidth.at(1) +
                              out.columnWidth.at(2))) < 0.5);

    // Выравнивания доехали от :---: до раскладки.
    ZT_TRUE("первая — влево", out.at(1, 0)->align == zametti::TableAlign::Left);
    ZT_TRUE("вторая — вправо", out.at(1, 1)->align == zametti::TableAlign::Right);
    ZT_TRUE("третья — по центру", out.at(1, 2)->align == zametti::TableAlign::Center);

    // Ряды стоят один под другим, колонки — одна за другой, без нахлёстов.
    ZT_TRUE("второй ряд ниже первого", out.at(1, 0)->rect.top() >= out.at(0, 0)->rect.bottom() - 0.5);
    ZT_TRUE("вторая колонка правее первой",
            out.at(0, 1)->rect.left() >= out.at(0, 0)->rect.right() - 0.5);
}

// Инвариант D: что попало в разметку, то и было в разборе.
void checkContentKept() {
    const zametti::Table table = zametti::parseTable(kSmall);
    const zametti::TableLayout out = zametti::layoutTable(table, roomFor(2000, 2000));
    for (int row = 0; row < out.rows; ++row) {
        for (int column = 0; column < out.columns; ++column) {
            const zametti::TableCellBox* box = out.at(row, column);
            ZT_TRUE("ячейка на месте", box != nullptr && box->text != nullptr);
            if (box == nullptr || box->text == nullptr) continue;
            ZT_EQ("текст ячейки " + n(row) + "," + n(column),
                  std::string(table.cell(row, column)), box->text->text().toStdString());
        }
    }

    // Разметка внутри ячейки разбирается, а не показывается звёздочками.
    const zametti::Table rich = zametti::parseTable(
        "| что |\n|---|\n| **жир** и `код` |\n");
    const zametti::TableLayout marked = zametti::layoutTable(rich, roomFor(2000, 2000));
    const zametti::TableCellBox* cell = marked.at(1, 0);
    ZT_TRUE("ячейка с разметкой разложена", cell != nullptr && cell->text != nullptr);
    if (cell != nullptr && cell->text != nullptr) {
        ZT_EQ("звёздочек в тексте нет", std::string("жир и код"),
              cell->text->text().toStdString());
        ZT_TRUE("а форматы кусков есть", !cell->text->formats().isEmpty());
    }
}

// Вписывание. Порядок такой (правило владельца, уточнённое по ходу этапа):
// сперва поля, потом ПЕРЕНОС ПО СЛОВАМ, и только потом усадка шрифта.
//
// Первая редакция переносила лишь на полу усадки, и одна длинная ячейка
// ужимала шрифт всей таблицы — не то лекарство от не той болезни.
void checkFitting() {
    const zametti::Table table = zametti::parseTable(kSmall);

    // Колонка узкая, а окно широкое — таблица занимает поля и не ужимается.
    const zametti::TableLayout roomy = zametti::layoutTable(table, roomFor(200, 2000));
    ZT_TRUE("в полях помещается без усадки: " + num(roomy.scale), roomy.scale == 1.0);
    ZT_TRUE("и шире колонки текста", roomy.width > 200);
    ZT_TRUE("переноса не понадобилось", !roomy.wrapped);

    // Места мало — переносим по словам, но шрифт НЕ трогаем.
    const zametti::TableLayout tight = zametti::layoutTable(table, roomFor(260, 260));
    ZT_TRUE("шрифт цел: " + num(tight.scale), tight.scale == 1.0);
    ZT_TRUE("зато включился перенос", tight.wrapped);
    ZT_TRUE("и таблица влезла: " + num(tight.width), tight.width <= 260.5);
    // Перенос делает ряды выше — это и есть его цена.
    ZT_TRUE("ряд с длинной ячейкой стал выше шапки",
            tight.rowHeight.at(2) > tight.rowHeight.at(0));

    // Места совсем нет — переноса не хватает, ужимается шрифт.
    const zametti::TableLayout floored = zametti::layoutTable(table, roomFor(90, 90));
    ZT_TRUE("шрифт ужат: " + num(floored.scale), floored.scale < 1.0);
    ZT_TRUE("усадка остановилась на полу: " + num(floored.scale), floored.scale >= 0.55);
    ZT_TRUE("таблица всё равно вписалась: " + num(floored.width), floored.width <= 90.5);
}

// Замечание владельца: одна очень длинная ячейка не должна раздувать свою
// колонку — её надо переносить по словам, а не ужимать всю таблицу.
void checkLongCellWraps() {
    const zametti::Table table = zametti::parseTable(
        "| что | описание |\n"
        "|---|---|\n"
        "| болт | очень длинное описание детали, которое ни в какую колонку "
        "целиком не поместится и обязано перенестись по словам |\n");

    const zametti::TableLayout out = zametti::layoutTable(table, roomFor(600, 600));
    ZT_TRUE("шрифт не тронут: " + num(out.scale), out.scale == 1.0);
    ZT_TRUE("перенос включился", out.wrapped);
    ZT_TRUE("таблица вписалась: " + num(out.width), out.width <= 600.5);
    // Узкая колонка от переноса почти не пострадала, широкая ужалась.
    ZT_TRUE("узкая колонка осталась узкой: " + num(out.columnWidth.at(0)),
            out.columnWidth.at(0) < out.columnWidth.at(1));
    // Ряд стал многострочным — это и значит «перенеслось». Спрашиваем саму
    // разметку, а не высоту: высота зависит ещё и от полей, и сравнение с
    // «вдвое выше шапки» ломалось на границе (54 против 27×2).
    const zametti::TableCellBox* longCell = out.at(1, 1);
    ZT_TRUE("длинная ячейка разложена", longCell != nullptr && longCell->text != nullptr);
    if (longCell != nullptr && longCell->text != nullptr)
        ZT_TRUE("и заняла несколько строк: " + n(longCell->text->lineCount()),
                longCell->text->lineCount() > 1);
    // Слова целы: колонка не уже самого длинного слова.
    ZT_TRUE("колонка не уже самого длинного слова", out.columnWidth.at(1) > 40);
}

void checkEdges() {
    // Таблица из одной шапки.
    const zametti::TableLayout head =
        zametti::layoutTable(zametti::parseTable("| a | b |\n|---|---|\n"), roomFor(500, 500));
    ZT_EQ("рядов один", n(1), n(head.rows));
    ZT_TRUE("высота положительна", head.height > 0);

    // Пустые ячейки: место занимают, разметка пустая.
    const zametti::TableLayout empty = zametti::layoutTable(
        zametti::parseTable("| a | b |\n|---|---|\n|  |  |\n"), roomFor(500, 500));
    ZT_TRUE("пустая ячейка на месте", empty.at(1, 0) != nullptr);
    ZT_EQ("и текст в ней пуст", std::string(), empty.at(1, 0)->text->text().toStdString());
    ZT_TRUE("а ширина колонки не нулевая", empty.columnWidth.at(0) > 0);

    // Не таблица — пустая раскладка, без падений.
    const zametti::TableLayout none =
        zametti::layoutTable(zametti::parseTable("просто текст\n"), roomFor(500, 500));
    ZT_EQ("не таблица — пустая раскладка", n(0), n(none.rows));
}

// --- замер ------------------------------------------------------------------
void bench() {
    std::string big = "| c1 | c2 | c3 | c4 | c5 | c6 | c7 | c8 |\n"
                      "|---|---|---|---|---|---|---|---|\n";
    for (int row = 0; row < 50; ++row) {
        big += "|";
        for (int column = 0; column < 8; ++column)
            big += " ячейка " + std::to_string(row) + "." + std::to_string(column) + " |";
        big += "\n";
    }

    // Эталон рядом с замером: постоянный счётный цикл. Поехал он — поехала
    // машина, и числам верить нельзя (правило проекта).
    qint64 yard = 0;
    {
        QElapsedTimer timer;
        timer.start();
        volatile double acc = 0;
        for (int i = 0; i < 3000000; ++i) acc += i * 0.5;
        yard = timer.nsecsElapsed() / 1000;
    }

    // РАЗБИВКА ПО ЧАСТЯМ: где именно уходит время. Первая догадка («почти всё
    // — разбор ячеек») замером не подтвердилась, и вписывать её в код без
    // проверки было бы ровно тем, от чего предостерегает CLAUDE.md.
    qint64 shapeBest = -1;
    for (int round = 0; round < 5; ++round) {
        const zametti::Table table = zametti::parseTable(big);
        QElapsedTimer timer;
        timer.start();
        // Только разметка ячеек, без раскладки: столько стоит parse() по всем
        // четырёмстам ячейкам.
        for (int row = 0; row < int(table.rows.size()); ++row)
            for (int column = 0; column < table.columns; ++column)
                (void)zametti::parse(std::string(table.cell(row, column)));
        const qint64 spent = timer.nsecsElapsed() / 1000;
        if (shapeBest < 0 || spent < shapeBest) shapeBest = spent;
    }

    // Из чего складывается раскладка: сколько стоит завести QTextLayout на
    // ячейку и сколько — разложить её. Догадками тут делать нечего.
    qint64 buildBest = -1;
    qint64 shapeOnceBest = -1;
    {
        const zametti::Table table = zametti::parseTable(big);
        const QFont font = zametti::tableFont(1.0);
        QVector<QString> texts;
        for (int row = 0; row < int(table.rows.size()); ++row)
            for (int column = 0; column < table.columns; ++column)
                texts.push_back(QString::fromStdString(std::string(table.cell(row, column))));

        for (int round = 0; round < 5; ++round) {
            QElapsedTimer timer;
            timer.start();
            std::vector<std::unique_ptr<QTextLayout>> made;
            made.reserve(size_t(texts.size()));
            for (const QString& text : texts)
                made.push_back(std::make_unique<QTextLayout>(text, font));
            const qint64 built = timer.nsecsElapsed() / 1000;
            for (const std::unique_ptr<QTextLayout>& made_layout : made) {
                QTextLayout& layout = *made_layout;
                layout.beginLayout();
                for (QTextLine line = layout.createLine(); line.isValid();
                     line = layout.createLine()) {
                    line.setLineWidth(1e6);
                    line.setPosition(QPointF(0, 0));
                }
                layout.endLayout();
            }
            const qint64 shaped = timer.nsecsElapsed() / 1000;
            if (buildBest < 0 || built < buildBest) buildBest = built;
            if (shapeOnceBest < 0 || shaped < shapeOnceBest) shapeOnceBest = shaped;
        }
    }

    qint64 parseBest = -1;
    qint64 wholeBest = -1;
    for (int round = 0; round < 7; ++round) {
        QElapsedTimer timer;
        timer.start();
        const zametti::Table table = zametti::parseTable(big);
        const qint64 parsed = timer.nsecsElapsed() / 1000;
        const zametti::TableLayout out = zametti::layoutTable(table, roomFor(900, 1400));
        const qint64 whole = timer.nsecsElapsed() / 1000;
        if (parseBest < 0 || parsed < parseBest) parseBest = parsed;
        if (wholeBest < 0 || whole < wholeBest) wholeBest = whole;
        (void)out;
    }
    std::printf("таблица 50×8 (400 ячеек): разбор таблицы %lld, разбор ячеек %lld, "
                "завести QTextLayout %lld, +разложить один раз %lld, вся раскладка %lld мкс "
                "(эталон %lld)\n",
                (long long)parseBest, (long long)shapeBest, (long long)buildBest,
                (long long)shapeOnceBest, (long long)wholeBest, (long long)yard);
}

}  // namespace

static int ztRunSuite(int argc, char** argv) {
    bool wantBench = false;
    for (int i = 1; i < argc; ++i)
        if (std::string(argv[i]) == "--bench") wantBench = true;

    checkWidths();
    checkContentKept();
    checkFitting();
    checkLongCellWraps();
    checkEdges();
    if (wantBench) bench();

    return zt::report("раскладка таблиц");
}

// Набор целиком одним TEST: тело не тронуто, argv ему собран здесь.
// Дробить на отдельные проверки — отдельная работа, по одному набору.
TEST(TableView, All) {
    std::vector<QByteArray> ztArgs{QByteArrayLiteral("table_view_test")};
    std::vector<char*> ztArgv;
    for (QByteArray& a : ztArgs) ztArgv.push_back(a.data());
    EXPECT_EQ(0, ztRunSuite(int(ztArgv.size()), ztArgv.data()));
}
