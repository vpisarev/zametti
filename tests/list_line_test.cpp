// Разбор строки пункта списка (list_line.h): одно правило на подсветчик и на
// клавиши режима исходника. Таблица краёв: отступ пробелами и табами, три
// буллета, номера с `.` и `)`, задачи в длинной и краткой записи, и то, что
// пунктом НЕ является (черта, жирный, буллет без пробела, десять цифр).

#include "list_line.h"
#include "test_util.h"

#include <QString>

#include <string>

namespace {

struct Case {
    const char* line;
    bool item;
    int indent;          // колонок
    int indentChars;
    const char* marker;  // "bullet" / "ordered" / "task" / ""
    int markerEnd;
    int contentStart;
    int contentColumn;
    bool emptyBody;
    const char* next;    // nextMarker
    const char* what;
};

const Case kCases[] = {
    {"- пункт", true, 0, 0, "bullet", 1, 2, 2, false, "- ", "буллет «-»"},
    {"* пункт", true, 0, 0, "bullet", 1, 2, 2, false, "* ", "буллет «*»"},
    {"+ пункт", true, 0, 0, "bullet", 1, 2, 2, false, "+ ", "буллет «+»"},
    {"  - пункт", true, 2, 2, "bullet", 3, 4, 4, false, "- ", "отступ двумя пробелами"},
    {"\t- пункт", true, 4, 1, "bullet", 2, 3, 6, false, "- ", "отступ табом — колонка 4, знак 1"},
    {" \t- пункт", true, 4, 2, "bullet", 3, 4, 6, false, "- ", "пробел и таб — до стопа"},
    {"- ", true, 0, 0, "bullet", 1, 2, 2, true, "- ", "пустой пункт с пробелом"},
    {"-   ", true, 0, 0, "bullet", 1, 2, 2, true, "- ", "пустой пункт с хвостом пробелов"},
    {"-\tпункт", true, 0, 0, "bullet", 1, 2, 2, false, "- ", "таб за буллетом — тоже пробельный"},
    {"1. номер", true, 0, 0, "ordered", 2, 3, 3, false, "2. ", "номер с точкой"},
    {"7) номер", true, 0, 0, "ordered", 2, 3, 3, false, "8) ", "номер со скобкой"},
    {"10. номер", true, 0, 0, "ordered", 3, 4, 4, false, "11. ", "двузначный номер"},
    {"   007. номер", true, 3, 3, "ordered", 7, 8, 8, false, "8. ", "номер с нулями — значение 7"},
    {"123456789. x", true, 0, 0, "ordered", 10, 11, 11, false, "123456790. ", "девять цифр — ещё номер"},
    {"- [ ] задача", true, 0, 0, "task", 5, 6, 2, false, "- [ ] ", "задача незакрытая"},
    {"- [x] задача", true, 0, 0, "task", 5, 6, 2, false, "- [ ] ", "задача закрытая"},
    {"- [X] задача", true, 0, 0, "task", 5, 6, 2, false, "- [ ] ", "задача закрытая заглавной"},
    {"-[ ] задача", true, 0, 0, "task", 4, 5, 2, false, "- [ ] ", "краткая запись"},
    {"-[x]", true, 0, 0, "task", 4, 4, 2, true, "- [ ] ", "краткая закрытая в конце строки — пустая"},
    {"* [ ] задача", true, 0, 0, "task", 5, 6, 2, false, "* [ ] ", "задача со звёздочкой"},
    {"  - [ ]", true, 2, 2, "task", 7, 7, 4, true, "- [ ] ", "пустая задача с отступом"},
    {"1. [ ] не задача", true, 0, 0, "ordered", 2, 3, 3, false, "2. ", "чекбокс у номера — текст"},
    {"- [y] не задача", true, 0, 0, "bullet", 1, 2, 2, false, "- ", "чужая буква в скобках — буллет"},
    {"- [ ]x", true, 0, 0, "bullet", 1, 2, 2, false, "- ", "за скобками буква — не чекбокс, буллет"},
    {"", false, 0, 0, "", 0, 0, 0, false, "", "пустая строка"},
    {"    ", false, 4, 4, "", 0, 0, 0, false, "", "одни пробелы"},
    {"абзац", false, 0, 0, "", 0, 0, 0, false, "", "абзац"},
    {"-пункт", false, 0, 0, "", 0, 0, 0, false, "", "буллет без пробела — не пункт"},
    {"-", false, 0, 0, "", 0, 0, 0, false, "", "одинокий дефис"},
    {"---", false, 0, 0, "", 0, 0, 0, false, "", "черта"},
    {"**жирный**", false, 0, 0, "", 0, 0, 0, false, "", "жирный со звёздочек"},
    {"1234567890. x", false, 0, 0, "", 0, 0, 0, false, "", "десять цифр — не номер"},
    {"1.5 число", false, 0, 0, "", 0, 0, 0, false, "", "точка без пробела — не номер"},
    {"1", false, 0, 0, "", 0, 0, 0, false, "", "одна цифра"},
    {"# заголовок", false, 0, 0, "", 0, 0, 0, false, "", "заголовок"},
    {"> - цитата", false, 0, 0, "", 0, 0, 0, false, "", "пункт в цитате — не наша забота"},
};

std::string markerName(const zametti::ListLine& l) {
    if (!l.item) return "";
    switch (l.marker) {
        case zametti::Marker::Bullet: return "bullet";
        case zametti::Marker::Ordered: return "ordered";
        case zametti::Marker::Task: return "task";
    }
    return "?";
}

void checkTable() {
    for (const Case& c : kCases) {
        const QString line = QString::fromUtf8(c.line);
        const zametti::ListLine got = zametti::parseListLine(line, 4);
        const std::string what = std::string(c.what) + " [" + c.line + "]";
        ZT_EQ(what + ": пункт?", std::to_string(c.item), std::to_string(got.item));
        ZT_EQ(what + ": отступ колонок", std::to_string(c.indent), std::to_string(got.indent));
        ZT_EQ(what + ": отступ знаков", std::to_string(c.indentChars), std::to_string(got.indentChars));
        ZT_EQ(what + ": маркер", std::string(c.marker), markerName(got));
        ZT_EQ(what + ": следующий маркер", std::string(c.next),
              zametti::nextMarker(got).toStdString());
        if (!c.item) continue;
        ZT_EQ(what + ": конец маркера", std::to_string(c.markerEnd), std::to_string(got.markerEnd));
        ZT_EQ(what + ": начало содержимого", std::to_string(c.contentStart),
              std::to_string(got.contentStart));
        ZT_EQ(what + ": колонка содержимого", std::to_string(c.contentColumn),
              std::to_string(got.contentColumn));
        ZT_EQ(what + ": пустое тело?", std::to_string(c.emptyBody), std::to_string(got.emptyBody));
    }
}

void checkDetails() {
    const zametti::ListLine task = zametti::parseListLine(QStringLiteral("- [x] сделано"));
    ZT_TRUE("задача отмечена", task.checked);
    ZT_EQ("знак буллета у задачи", std::string("-"), QString(task.bullet).toStdString());
    const zametti::ListLine open = zametti::parseListLine(QStringLiteral("* [ ] нет"));
    ZT_TRUE("задача не отмечена", !open.checked);
    const zametti::ListLine num = zametti::parseListLine(QStringLiteral("12) x"));
    ZT_EQ("значение номера", std::string("12"), std::to_string(num.ordinal));
    ZT_EQ("разделитель номера", std::string(")"), QString(num.delimiter).toStdString());

    ZT_EQ("ширина отступа с табом при стопе 8", std::string("8"),
          std::to_string(zametti::columnOf(QStringLiteral("\tx"), 1, 8)));
    ZT_EQ("ширина «  \\t» при стопе 4 — 4", std::string("4"),
          std::to_string(zametti::columnOf(QStringLiteral("  \tx"), 3, 4)));
    ZT_EQ("ведущие пробельные", std::string("3"),
          std::to_string(zametti::leadingWhitespace(QStringLiteral(" \t x"))));
    ZT_TRUE("пустая строка — пустая", zametti::isBlankLine(QStringLiteral("")));
    ZT_TRUE("табы и пробелы — пустая", zametti::isBlankLine(QStringLiteral(" \t ")));
    ZT_TRUE("буква — не пустая", !zametti::isBlankLine(QStringLiteral(" a")));
}

}  // namespace

TEST(ListLine, All) {
    checkTable();
    checkDetails();
}
