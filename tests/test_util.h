// Минимальный каркас для тестов ядра. Внешних зависимостей нет намеренно:
// новых зависимостей в проект без согласования не добавляем, а всё, что здесь
// нужно, — это «сравни и покажи разницу».

#ifndef ZAMETTI_TEST_UTIL_H
#define ZAMETTI_TEST_UTIL_H

#include <cstdio>
#include <string>
#include <vector>

namespace zt {

inline int g_failures = 0;
inline int g_checks = 0;

inline std::string visible(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '\n') out += "\\n\n";
        else if (c == '\t') out += "\\t";
        else if (c == '\r') out += "\\r";
        else out.push_back(c);
    }
    return out;
}

// Построчный дифф. Нужен и тестам, и режиму --check просмотрщика, поэтому
// формат держим человекочитаемым, а не «первый различающийся байт».
inline std::vector<std::string> splitLines(const std::string& s) {
    std::vector<std::string> lines;
    size_t pos = 0;
    while (pos <= s.size()) {
        size_t e = s.find('\n', pos);
        if (e == std::string::npos) {
            if (pos < s.size()) lines.push_back(s.substr(pos));
            break;
        }
        lines.push_back(s.substr(pos, e - pos));
        pos = e + 1;
        if (pos == s.size()) break;
    }
    return lines;
}

inline std::string diff(const std::string& expected, const std::string& actual) {
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

inline void checkEq(const std::string& what, const std::string& expected,
                    const std::string& actual, const char* file, int line) {
    ++g_checks;
    if (expected == actual) return;
    ++g_failures;
    std::fprintf(stderr, "FAIL %s:%d  %s\n", file, line, what.c_str());
    std::fprintf(stderr, "--- ожидалось\n%s\n+++ получено\n%s\n", visible(expected).c_str(),
                 visible(actual).c_str());
    std::string d = diff(expected, actual);
    if (!d.empty()) std::fprintf(stderr, "дифф:\n%s", d.c_str());
    std::fprintf(stderr, "\n");
}

inline void checkTrue(const std::string& what, bool cond, const char* file, int line) {
    ++g_checks;
    if (cond) return;
    ++g_failures;
    std::fprintf(stderr, "FAIL %s:%d  %s\n\n", file, line, what.c_str());
}

inline int report(const char* suite) {
    if (g_failures == 0) {
        std::fprintf(stderr, "%s: %d проверок, всё зелено\n", suite, g_checks);
        return 0;
    }
    std::fprintf(stderr, "%s: %d проверок, %d провалов\n", suite, g_checks, g_failures);
    return 1;
}

}  // namespace zt

#define ZT_EQ(what, expected, actual) ::zt::checkEq((what), (expected), (actual), __FILE__, __LINE__)
#define ZT_TRUE(what, cond) ::zt::checkTrue((what), (cond), __FILE__, __LINE__)

#endif  // ZAMETTI_TEST_UTIL_H
