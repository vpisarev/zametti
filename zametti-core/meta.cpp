// Метаданные заметки: чтение и правка ключей поверх дословных строк.
//
// Строки хранятся как есть — round-trip обязан быть побайтовым, включая
// неизвестные ключи и их порядок. Известный ключ правится заменой своей строки
// на каноническую "key: value"; чужие строки не трогаются никогда.

#include "ir.h"

#include <cassert>

namespace zametti {
namespace {

std::string_view trimmed(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}

// Ключ строки метаданных; пусто — строка не вида "key: value".
std::string_view keyOf(std::string_view line) {
    const size_t colon = line.find(':');
    if (colon == std::string_view::npos) return {};
    return trimmed(line.substr(0, colon));
}

}  // namespace

std::string NoteMeta::get(std::string_view key) const {
    for (const std::string& line : lines) {
        if (keyOf(line) != key) continue;
        const std::string_view rest = std::string_view(line).substr(line.find(':') + 1);
        return std::string(trimmed(rest));
    }
    return {};
}

void NoteMeta::set(std::string_view key, std::string_view value) {
    // "--" ломает HTML-комментарий, перевод строки — форму «строка на ключ».
    assert(value.find("--") == std::string_view::npos && "в значении не место '--'");
    assert(value.find('\n') == std::string_view::npos && "значение — одна строка");

    // Пустое значение снимает ключ: отсутствующий parent и значит «в корне».
    if (value.empty()) {
        for (size_t i = 0; i < lines.size(); ++i) {
            if (keyOf(lines[i]) != key) continue;
            lines.erase(lines.begin() + static_cast<long>(i));
            return;
        }
        return;
    }

    if (!present) {
        present = true;
        blankAfter = true;
    }
    std::string line(key);
    line += ": ";
    line += value;
    for (std::string& existing : lines) {
        if (keyOf(existing) != key) continue;
        existing = std::move(line);
        return;
    }
    lines.push_back(std::move(line));
}

void NoteMeta::unset(std::string_view key) {
    for (auto it = lines.begin(); it != lines.end();) {
        const std::string& line = *it;
        const size_t colon = line.find(':');
        bool match = false;
        if (colon != std::string::npos) {
            std::string_view name(line.data(), colon);
            while (!name.empty() && (name.back() == ' ' || name.back() == '\t'))
                name.remove_suffix(1);
            while (!name.empty() && (name.front() == ' ' || name.front() == '\t'))
                name.remove_prefix(1);
            match = name == key;
        }
        it = match ? lines.erase(it) : it + 1;
    }
}

}  // namespace zametti
