#include "note_header.h"

#include <QtGlobal>

#include <cassert>
#include <cstdio>

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

// Шапка заметки — HTML-комментарий в начале файла. Строки modified и version
// ищем только в ней: слово «modified:» в тексте заметки трогать нельзя.
//
// `version` — тоже не содержимое (refactor3): версия формата встаёт в шапку
// лениво, при первой записи правленой заметки, и без этой оговорки возврат
// отменой к состоянию, записанному ДО неё, считался бы новой записью журнала
// (набор HistoryWrite это и поймал), а заметка, вернувшаяся к исходному
// тексту, — изменённой.
bool NoteHeader::sameFileApartFromStamps(const QByteArray& a, const QByteArray& b) {
    const auto stripped = [](const QByteArray& text) {
        const qsizetype head = text.indexOf("-->");
        if (head < 0) return text;
        QByteArray out = text;
        for (const char* key : {"\nmodified:", "\nversion:"}) {
            const qsizetype at = out.indexOf(key);
            if (at < 0 || at > out.indexOf("-->")) continue;
            const qsizetype eol = out.indexOf('\n', at + 1);
            if (eol < 0) continue;
            out.remove(at, eol - at);
        }
        return out;
    };
    if (a.size() == b.size() && a == b) return true;
    return stripped(a) == stripped(b);
}

bool NoteHeader::archived() const {
    if (!get(kArchivedKey).empty()) return true;
    // Старый вид: заметка-корзина до этапа 15. Читается как архивная, чтобы
    // хранилище, не прошедшее миграцию, не выглядело поломанным.
    return get("role") == "trash";
}

void NoteHeader::setArchived(bool archived) {
    present_ = true;
    if (archived) set(kArchivedKey, kArchivedValue);
    else unset(kArchivedKey);
}

bool NoteHeader::readOnly() const {
    const std::string value = get(kAccessKey);
    if (value.empty()) return false;
    if (value == kReadOnlyValue) return true;
    // Понятных значений пока два: `read-only` и отсутствие ключа. Всё
    // остальное — чужая или будущая запись: заметку считаем правимой и
    // говорим вслух, что не поняли. Ключ при этом остаётся в файле нетронутым
    // (побайтовый круг шапки), и разбираться с ним будет тот, кто его написал.
    std::fprintf(stderr, "unknown access in header: %s\n", value.c_str());
    return false;
}

void NoteHeader::setReadOnly(bool readOnly) {
    present_ = true;
    if (readOnly) set(kAccessKey, kReadOnlyValue);
    else unset(kAccessKey);
}

}  // namespace zametti
