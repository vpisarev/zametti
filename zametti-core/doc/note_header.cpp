#include "note_header.h"

#include <cassert>

namespace zametti {
namespace {

std::string_view trimmed(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}

// Ключ строки шапки; пусто — строка не вида «key: value».
std::string_view keyOf(std::string_view line) {
    const size_t colon = line.find(':');
    if (colon == std::string_view::npos) return {};
    return trimmed(line.substr(0, colon));
}

}  // namespace

std::string NoteHeader::get(std::string_view key) const {
    for (const std::string& line : lines_) {
        if (keyOf(line) != key) continue;
        const std::string_view rest = std::string_view(line).substr(line.find(':') + 1);
        return std::string(trimmed(rest));
    }
    return {};
}

void NoteHeader::set(std::string_view key, std::string_view value) {
    // "--" ломает HTML-комментарий, перевод строки — форму «строка на ключ».
    assert(value.find("--") == std::string_view::npos && "'--' has no place in a value");
    assert(value.find('\n') == std::string_view::npos && "a value is a single line");

    if (value.empty()) {
        unset(key);
        return;
    }

    if (!present_) {
        present_ = true;
        blankAfter_ = true;
    }
    std::string line(key);
    line += ": ";
    line += value;
    for (std::string& existing : lines_) {
        if (keyOf(existing) != key) continue;
        existing = std::move(line);
        return;
    }
    lines_.push_back(std::move(line));
}

void NoteHeader::ensureVersion() {
    if (!get(kVersionKey).empty()) return;   // есть — своя или более новая, не трогаем
    if (!present_) {
        present_ = true;
        blankAfter_ = true;
    }
    std::string line(kVersionKey);
    line += ": ";
    line += kFormatVersion;
    lines_.insert(lines_.begin(), std::move(line));
}

void NoteHeader::unset(std::string_view key) {
    for (auto it = lines_.begin(); it != lines_.end();) {
        it = keyOf(*it) == key ? lines_.erase(it) : it + 1;
    }
}

QString NoteHeader::toText() const {
    const std::string bytes = toBytes();
    return QString::fromUtf8(bytes.data(), qsizetype(bytes.size()));
}

std::string NoteHeader::toBytes() const {
    if (!present_) return {};
    std::string out = "<!-- zametti\n";
    for (const std::string& line : lines_) {
        out += line;
        out += '\n';
    }
    out += "-->\n";
    if (blankAfter_) out += '\n';
    return out;
}

}  // namespace zametti
