// Разбор таблицы для показа.
//
// Половина набора — придуманные случаи (края, кривые ряды, выравнивания,
// экранированная палка), вторая половина — СВЕРКА С ЧУЖОЙ РЕАЛИЗАЦИЕЙ. md4c
// зовётся здесь по-настоящему, с флагом таблиц, и его ответ («это таблица,
// столько-то колонок, столько-то рядов, вот текст ячеек») сравнивается с
// нашим на всех таблицах корпусов.
//
// Так и положено проверять свой разбор чужого формата: собственные примеры
// показывают только то, что я успел придумать, а корпус — то, что люди пишут
// на самом деле.

#include "table.h"
#include "test_util.h"
#include "testdata.h"

#include "md4c.h"

#include <QString>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::string s(const std::string& v) { return v; }
std::string n(int v) { return std::to_string(v); }

// Разбор берёт текст (QStringView, решение владельца: всё в памяти — QString);
// придуманные случаи набора удобнее держать байтами.
zametti::Table parse(const std::string& markdown) {
    return zametti::parseTable(QString::fromStdString(markdown));
}
std::string cellText(const zametti::Table& t, int row, int col) {
    return t.cell(row, col).toString().toStdString();
}

// --- md4c как независимый судья ---------------------------------------------
//
// Собираем из колбэков ровно то, что нужно для сверки: сколько таблиц, сколько
// в каждой колонок и рядов и какой в ячейках ПРОСТОЙ текст (без разметки —
// сравнивать с нашим сырым markdown нельзя, поэтому наш прогоняем через ту же
// чистку).
struct MdTable {
    int columns = 0;
    int rows = 0;                            // включая шапку
    std::vector<std::string> cells;          // подряд, рядами
};

struct Judge {
    std::vector<MdTable> tables;
    int depth = 0;          // глубина внутри таблицы
    bool inCell = false;
    std::string cell;
};

int judgeEnterBlock(MD_BLOCKTYPE type, void* detail, void* userdata) {
    Judge& j = *static_cast<Judge*>(userdata);
    if (type == MD_BLOCK_TABLE) {
        const auto* d = static_cast<const MD_BLOCK_TABLE_DETAIL*>(detail);
        MdTable t;
        t.columns = int(d->col_count);
        t.rows = int(d->head_row_count) + int(d->body_row_count);
        j.tables.push_back(t);
        ++j.depth;
    } else if (j.depth > 0 && (type == MD_BLOCK_TH || type == MD_BLOCK_TD)) {
        j.inCell = true;
        j.cell.clear();
    }
    return 0;
}

int judgeLeaveBlock(MD_BLOCKTYPE type, void*, void* userdata) {
    Judge& j = *static_cast<Judge*>(userdata);
    if (type == MD_BLOCK_TABLE) {
        --j.depth;
    } else if (j.depth > 0 && (type == MD_BLOCK_TH || type == MD_BLOCK_TD)) {
        j.tables.back().cells.push_back(j.cell);
        j.inCell = false;
    }
    return 0;
}

int judgeText(MD_TEXTTYPE type, const MD_CHAR* text, MD_SIZE size, void* userdata) {
    Judge& j = *static_cast<Judge*>(userdata);
    if (!j.inCell) return 0;
    if (type == MD_TEXT_BR || type == MD_TEXT_SOFTBR) j.cell += ' ';
    else j.cell += QString::fromUtf16(text, size).toStdString();   // md4c читает UTF-16
    return 0;
}

std::vector<MdTable> tablesByMd4c(const std::string& markdown) {
    Judge judge;
    MD_PARSER parser{};
    parser.flags = MD_FLAG_TABLES | MD_FLAG_STRIKETHROUGH | MD_FLAG_TASKLISTS |
                   MD_FLAG_PERMISSIVEAUTOLINKS;
    parser.enter_block = judgeEnterBlock;
    parser.leave_block = judgeLeaveBlock;
    parser.enter_span = [](MD_SPANTYPE, void*, void*) { return 0; };
    parser.leave_span = [](MD_SPANTYPE, void*, void*) { return 0; };
    parser.text = judgeText;
    // md4c (вендоренная копия) читает UTF-16: судье подаём тот же текст в нём.
    const QString text = QString::fromStdString(markdown);
    md_parse(QStringView(text).utf16(), MD_SIZE(text.size()), &parser, &judge);
    return judge.tables;
}

// Наш сырой markdown ячейки — к тому же простому тексту, каким его отдаёт
// md4c: снимаем разметку тем же md4c, чтобы сравнивать сравнимое.
std::string plainOf(const std::string& markdown) {
    struct Sink {
        std::string out;
    } sink;
    MD_PARSER parser{};
    parser.flags = MD_FLAG_TABLES | MD_FLAG_STRIKETHROUGH | MD_FLAG_TASKLISTS |
                   MD_FLAG_PERMISSIVEAUTOLINKS;
    parser.enter_block = [](MD_BLOCKTYPE, void*, void*) { return 0; };
    parser.leave_block = [](MD_BLOCKTYPE, void*, void*) { return 0; };
    parser.enter_span = [](MD_SPANTYPE, void*, void*) { return 0; };
    parser.leave_span = [](MD_SPANTYPE, void*, void*) { return 0; };
    parser.text = [](MD_TEXTTYPE type, const MD_CHAR* text, MD_SIZE size, void* user) {
        auto& out = static_cast<Sink*>(user)->out;
        if (type == MD_TEXT_BR || type == MD_TEXT_SOFTBR) out += ' ';
        else out += QString::fromUtf16(text, size).toStdString();
        return 0;
    };
    // Ячейку разбираем В КОНТЕКСТЕ ЯЧЕЙКИ: заворачиваем в таблицу из одной
    // колонки. Иначе "+ раз" внутри ячейки становится списком и теряет
    // маркер, а "> два" — цитатой; md4c внутри ячейки разбирает только
    // строчную разметку, и сравнивать надо с тем же.
    const QString wrapped = QString::fromStdString("| " + markdown + " |\n|---|\n");
    md_parse(QStringView(wrapped).utf16(), MD_SIZE(wrapped.size()), &parser, &sink);
    // Одиночный абзац кончается переводом строки — у md4c в ячейке его нет.
    while (!sink.out.empty() && (sink.out.back() == '\n' || sink.out.back() == ' '))
        sink.out.pop_back();
    return sink.out;
}

// --- придуманные случаи -----------------------------------------------------

void checkSimple() {
    const zametti::Table t = parse("| имя | цена |\n"
        "|---|---:|\n"
        "| болт | 10 |\n"
        "| гайка | 5 |\n");
    ZT_TRUE("простая таблица разобрана", t.valid);
    ZT_EQ("колонок", n(2), n(t.columns));
    ZT_EQ("рядов вместе с шапкой", n(3), n(int(t.rows.size())));
    ZT_EQ("строк исходника", n(4), n(t.lines));
    ZT_EQ("шапка", s("имя"), cellText(t, 0, 0));
    ZT_EQ("ячейка тела", s("гайка"), cellText(t, 2, 0));
    ZT_TRUE("вторая колонка выровнена вправо",
            t.align.at(1) == zametti::TableAlign::Right);
    ZT_TRUE("первая — как придётся", t.align.at(0) == zametti::TableAlign::Default);
}

void checkAligns() {
    const zametti::Table t = parse("| л | ц | п | н |\n"
        "|:---|:---:|---:|---|\n"
        "| 1 | 2 | 3 | 4 |\n");
    ZT_TRUE("разобрана", t.valid);
    ZT_TRUE("слева", t.align.at(0) == zametti::TableAlign::Left);
    ZT_TRUE("по центру", t.align.at(1) == zametti::TableAlign::Center);
    ZT_TRUE("справа", t.align.at(2) == zametti::TableAlign::Right);
    ZT_TRUE("не задано", t.align.at(3) == zametti::TableAlign::Default);
}

// Кривые ряды: лишние ячейки отрезаются, недостающие — пустые. В корпусе таких
// таблиц нет ни одной (замер), поэтому случай тут придуманный — и потому он
// тут и нужен.
void checkRagged() {
    const zametti::Table t = parse("| a | b | c |\n"
        "|---|---|---|\n"
        "| 1 | 2 |\n"
        "| 1 | 2 | 3 | 4 |\n");
    ZT_TRUE("разобрана", t.valid);
    ZT_EQ("колонок по шапке", n(3), n(t.columns));
    ZT_EQ("недостающая ячейка пуста", s(""), cellText(t, 1, 2));
    ZT_EQ("лишняя отрезана", n(3), n(int(t.rows.at(2).size())));
    ZT_EQ("а что было — на месте", s("3"), cellText(t, 2, 2));
}

void checkEdges() {
    ZT_TRUE("без разделителя — не таблица",
            !parse("| a | b |\n| 1 | 2 |\n").valid);
    ZT_TRUE("без палок — не таблица",
            !parse("просто текст\n---\nещё текст\n").valid);
    // Разделитель короче шапки — таблица ВСЁ РАВНО, и колонок в ней столько,
    // сколько в разделителе. Спецификация GFM говорит «не таблица», md4c —
    // «таблица в одну колонку», и решает здесь он: это его ответ становится
    // дословным куском в хранилище.
    const zametti::Table narrow = parse("| a | b |\n|---|\n| 1 | 2 |\n");
    ZT_TRUE("разделитель короче шапки — всё равно таблица", narrow.valid);
    ZT_EQ("колонок по разделителю", n(1), n(narrow.columns));
    ZT_EQ("лишняя ячейка шапки отрезана", s("a"), cellText(narrow, 0, 0));
    ZT_TRUE("пустая ячейка в разделителе — не таблица",
            !parse("| a | b |\n|---||\n").valid);

    // Таблица без тела законна — одна шапка.
    const zametti::Table head = parse("| a | b |\n|---|---|\n");
    ZT_TRUE("одна шапка — уже таблица", head.valid);
    ZT_EQ("рядов тела нет", n(0), n(head.bodyRows()));

    // Палки по краям необязательны.
    const zametti::Table bare = parse("a | b\n--- | ---\n1 | 2\n");
    ZT_TRUE("без внешних палок — таблица", bare.valid);
    ZT_EQ("колонок", n(2), n(bare.columns));
    ZT_EQ("ячейка", s("1"), cellText(bare, 1, 0));

    // Пустые ячейки — законны и должны остаться пустыми, а не пропасть.
    const zametti::Table empty = parse("| a | b |\n|---|---|\n|  |  |\n");
    ZT_EQ("пустых ячеек две", n(2), n(int(empty.rows.at(1).size())));
    ZT_EQ("и они пусты", s(""), cellText(empty, 1, 1));

    // Хвост после таблицы в тот же дословный кусок не входит.
    const zametti::Table tail = parse("| a |\n|---|\n| 1 |\n\nабзац после\n");
    ZT_EQ("таблица кончилась пустой строкой", n(3), n(tail.lines));
    ZT_EQ("рядов вместе с шапкой", n(2), n(int(tail.rows.size())));

    // А строка без палок таблицу НЕ кончает: это ряд из одной ячейки.
    const zametti::Table bare2 = parse("| a | b |\n|---|---|\n| 1 | 2 |\nхвост\n");
    ZT_EQ("рядов вместе с шапкой", n(3), n(int(bare2.rows.size())));
    ZT_EQ("строка без палок — ряд", s("хвост"), cellText(bare2, 2, 0));
    ZT_EQ("и вторая ячейка в нём пуста", s(""), cellText(bare2, 2, 1));
}

void checkEscapedPipe() {
    const zametti::Table t = parse("| код | что |\n"
        "|---|---|\n"
        "| `a \\| b` | или |\n");
    ZT_EQ("экранированная палка ряд не рвёт", n(2), n(int(t.rows.at(1).size())));
    // Обратная косая ОСТАЁТСЯ: снимать её — работа разметки, а внутри кода
    // она и не снимается вовсе (так показывает GitHub, так отвечает md4c).
    ZT_EQ("и экранирование осталось в тексте", s("`a \\| b`"), cellText(t, 1, 0));
}

// --- сверка с md4c ----------------------------------------------------------

void checkAgainstMd4c(const std::string& markdown, const std::string& where) {
    const std::vector<MdTable> theirs = tablesByMd4c(markdown);
    if (theirs.empty()) return;

    // Наш разбор идёт по кускам: находим начала таблиц тем же признаком, что и
    // показ, — построчно. Текст один раз переводится в QString: разбор ходит по
    // QStringView, смещения — единицы UTF-16.
    const QString text = QString::fromStdString(markdown);
    const QStringView view(text);
    std::vector<zametti::Table> mine;
    qsizetype at = 0;
    while (at <= view.size()) {
        const QStringView rest = view.mid(at);
        if (zametti::looksLikeTable(rest)) {
            zametti::Table t = zametti::parseTable(rest);
            if (t.valid) {
                // Смещения ячеек указывают на их же текст в исходнике.
                for (const auto& row : t.rows)
                    for (const zametti::TableCell& cell : row)
                        ZT_EQ(where + ": смещение ячейки", cell.text.toStdString(),
                              rest.mid(cell.start, cell.end - cell.start).toString().toStdString());
                mine.push_back(t);
                // Пропускаем строки этой таблицы: внутри неё начал нет.
                qsizetype skip = at;
                for (int line = 0; line < t.lines && skip < view.size(); ++line) {
                    const qsizetype next = view.indexOf(u'\n', skip);
                    if (next < 0) { skip = view.size(); break; }
                    skip = next + 1;
                }
                at = skip;
                continue;
            }
        }
        const qsizetype next = view.indexOf(u'\n', at);
        if (next < 0) break;
        at = next + 1;
    }

    ZT_EQ(where + ": число таблиц", n(int(theirs.size())), n(int(mine.size())));
    const size_t count = std::min(theirs.size(), mine.size());
    for (size_t i = 0; i < count; ++i) {
        ZT_EQ(where + ": колонок в таблице " + n(int(i)), n(theirs[i].columns),
              n(mine[i].columns));
        ZT_EQ(where + ": рядов в таблице " + n(int(i)), n(theirs[i].rows),
              n(int(mine[i].rows.size())));
        // Текст ячеек: наш сырой markdown приводим к простому тексту тем же
        // md4c, иначе сравнивали бы разное.
        const size_t cells = std::min(theirs[i].cells.size(),
                                      size_t(mine[i].columns) * mine[i].rows.size());
        for (size_t k = 0; k < cells; ++k) {
            const int row = int(k) / mine[i].columns;
            const int col = int(k) % mine[i].columns;
            ZT_EQ(where + ": ячейка " + n(row) + "," + n(col),
                  theirs[i].cells[k], plainOf(cellText(mine[i], row, col)));
        }
    }
}

void checkCorpus(const std::filesystem::path& root) {
    if (!std::filesystem::exists(root)) {
        std::printf("корпуса нет, пропускаем: %s\n", root.string().c_str());
        return;
    }
    int files = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".md") continue;
        std::ifstream in(entry.path(), std::ios::binary);
        std::stringstream buffer;
        buffer << in.rdbuf();
        ++files;
        checkAgainstMd4c(buffer.str(),
                         std::filesystem::relative(entry.path(), root).string());
    }
    std::printf("сверено с md4c: %d файлов в %s\n", files, root.string().c_str());
}

}  // namespace

static int ztRunSuite(int argc, char** argv) {
    checkSimple();
    checkAligns();
    checkRagged();
    checkEdges();
    checkEscapedPipe();

    for (int i = 1; i < argc; ++i) checkCorpus(std::filesystem::path(argv[i]));

    return zt::report("таблицы");
}

// Набор целиком одним TEST: тело не тронуто, argv ему собран здесь.
// Дробить на отдельные проверки — отдельная работа, по одному набору.
TEST(Table, All) {
    // Корпуса — судье md4c: docs/ репозитория (всегда есть) и внешние, если
    // подключены. Без них половина набора не запускалась вовсе — доводы
    // никто не передавал.
    std::vector<QByteArray> ztArgs{QByteArrayLiteral("table_test")};
    ztArgs.push_back((QStringLiteral(ZAMETTI_SOURCE_DIR) + QStringLiteral("/docs")).toLocal8Bit());
    for (const char* name : {"corpus", "commonmark", "gfm"}) {
        const QString dir = zt::TestData::corpus(QString::fromLatin1(name));
        if (!dir.isEmpty()) ztArgs.push_back(dir.toLocal8Bit());
    }
    std::vector<char*> ztArgv;
    for (QByteArray& a : ztArgs) ztArgv.push_back(a.data());
    EXPECT_EQ(0, ztRunSuite(int(ztArgv.size()), ztArgv.data()));
}
