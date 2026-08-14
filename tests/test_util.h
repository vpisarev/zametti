// Мостик от прежней обвязки к gtest.
//
// Проверок в дереве несколько тысяч, и написаны они двумя макросами — ZT_EQ и
// ZT_TRUE. Переписывать их все на EXPECT_EQ разом значило бы сделать
// многотысячную правку, в которой ошибку не разглядишь; поэтому макросы
// остались, а под ними теперь gtest.
//
// Что изменилось по существу:
//
//   * счётчики и zt::report ушли — итог подводит gtest;
//   * провал теперь ЗАВАЛИВАЕТ набор. Прежняя обвязка складывала провалы в
//     глобальный счётчик, и внутри одного бинарника они не мешали идти дальше;
//   * пояснение к проверке (первый довод макроса) уезжает в сообщение gtest —
//     оно и было главной ценностью прежней обвязки. Формулировки вроде
//     «усечённое не совпадает с целым» стоят больше, чем имя функции.
//
// Построчный дифф оставлен: при расхождении длинных текстов «первый
// различающийся байт» не говорит человеку ничего, а список строк говорит.

#ifndef ZAMETTI_TEST_UTIL_H
#define ZAMETTI_TEST_UTIL_H

#include <gtest/gtest.h>

#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace zt {

// Счётчик провалов остался, хотя итог теперь подводит gtest. Он нужен не для
// отчёта: фаззеры смотрят на него, чтобы остановиться на первом же расхождении
// и не завалить человека тысячей одинаковых жалоб.
inline int g_failures = 0;
// И счётчик самих проверок: наборы печатают им прогресс («проверено 4182 блока»)
// и по нему же убеждаются, что проверка вообще что-то проверила.
inline int g_checks = 0;

inline std::string visible(std::string_view s) {
    std::string out;
    for (char c : s) {
        if (c == '\n') out += "\\n\n";
        else if (c == '\t') out += "\\t";
        else if (c == '\r') out += "\\r";
        else out.push_back(c);
    }
    return out;
}

inline std::vector<std::string> splitLines(std::string_view s) {
    std::vector<std::string> lines;
    size_t pos = 0;
    while (pos <= s.size()) {
        size_t e = s.find('\n', pos);
        if (e == std::string_view::npos) {
            if (pos < s.size()) lines.push_back(std::string(s.substr(pos)));
            break;
        }
        lines.push_back(std::string(s.substr(pos, e - pos)));
        pos = e + 1;
        if (pos == s.size()) break;
    }
    return lines;
}

inline std::string diff(std::string_view expected, std::string_view actual) {
    std::vector<std::string> a = splitLines(expected);
    std::vector<std::string> b = splitLines(actual);
    std::string out;
    size_t n = a.size() > b.size() ? a.size() : b.size();
    for (size_t i = 0; i < n; ++i) {
        const std::string* ea = i < a.size() ? &a[i] : nullptr;
        const std::string* eb = i < b.size() ? &b[i] : nullptr;
        if (ea && eb && *ea == *eb) continue;
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%5zu", i + 1);
        if (ea) out += std::string(buf) + " - " + *ea + "\n";
        if (eb) out += std::string(buf) + " + " + *eb + "\n";
    }
    return out;
}

inline void checkEq(std::string_view what, std::string_view expected,
                    std::string_view actual, const char* file, int line) {
    ++g_checks;
    if (expected == actual) {
        SUCCEED();
        return;
    }
    std::string message = std::string(what) + "\n--- ожидалось\n" + visible(expected) +
                          "\n+++ получено\n" + visible(actual) + "\n";
    const std::string d = diff(expected, actual);
    if (!d.empty()) message += "дифф:\n" + d;
    ++g_failures;
    ADD_FAILURE_AT(file, line) << message;
}

inline void checkTrue(std::string_view what, bool cond, const char* file, int line) {
    ++g_checks;
    if (cond) {
        SUCCEED();
        return;
    }
    ++g_failures;
    ADD_FAILURE_AT(file, line) << what;
}

// Прежний итог набора. Теперь его подводит gtest, а функция осталась, чтобы не
// править восемьдесят два тела разом. Всегда ноль: провалы уже посчитаны — их
// отметила каждая проверка сама, в тот же миг, когда споткнулась.
inline int report(const char*) { return 0; }

}  // namespace zt

#define ZT_EQ(what, expected, actual) ::zt::checkEq((what), (expected), (actual), __FILE__, __LINE__)
#define ZT_TRUE(what, cond) ::zt::checkTrue((what), (cond), __FILE__, __LINE__)

#endif  // ZAMETTI_TEST_UTIL_H
