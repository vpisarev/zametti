#include "note_id.h"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <random>

namespace zametti {
namespace {

// Crockford base32: без i l o u — их путают с 1 и 0.
constexpr char kAlphabet[] = "0123456789abcdefghjkmnpqrstvwxyz";

void appendBase32(std::string& out, std::uint64_t value, int digits) {
    for (int i = digits - 1; i >= 0; --i)
        out.push_back(kAlphabet[(value >> (5 * i)) & 31]);
}

std::uint64_t systemRandom() {
    // random_device на Linux и маке — энтропия ОС. 64 бита из двух вызовов:
    // сам по себе он отдаёт 32.
    std::random_device rd;
    return (std::uint64_t(rd()) << 32) | rd();
}

}  // namespace

std::string makeNoteId(std::uint64_t unixSeconds, std::uint64_t random) {
    std::string id;
    id.reserve(kNoteIdLength);
    appendBase32(id, unixSeconds, 8);   // сдвиги режут по модулю 32^8 сами
    appendBase32(id, random, 6);
    return id;
}

std::string newNoteId() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto seconds =
        std::chrono::duration_cast<std::chrono::seconds>(now).count();
    return makeNoteId(std::uint64_t(seconds), systemRandom());
}

bool isValidNoteId(std::string_view id) {
    if (id.size() != kNoteIdLength) return false;
    for (char c : id) {
        const bool digit = c >= '0' && c <= '9';
        const bool letter = c >= 'a' && c <= 'z' && c != 'i' && c != 'l' &&
                            c != 'o' && c != 'u';
        if (!digit && !letter) return false;
    }
    return true;
}

std::string createNoteFile(const std::string& dir, const std::string& content,
                           std::string* pathOut,
                           const std::function<std::string()>& generator) {
    // Потолок попыток — от сумасшедшего генератора в тестах; в жизни вторая
    // попытка уже почти невозможна (32^6 вариантов в ту же секунду).
    for (int attempt = 0; attempt < 64; ++attempt) {
        const std::string id = generator ? generator() : newNoteId();
        // Завершающая косая черта в каталоге дала бы "хранилище//<id>.md".
        // Открылось бы и записалось нормально — и именно поэтому опасно: путь
        // рабочий, но по строке не равен тому, которым ту же заметку зовёт
        // остальная программа. Один такой лишний знак стоил бага с заголовком
        // в средней колонке.
        std::string base = dir;
        while (base.size() > 1 && base.back() == '/') base.pop_back();
        const std::string path = base + "/" + id + ".md";

        // O_EXCL — единственная честная защита от гонки: проверка "файла нет"
        // и создание — одно действие ядра ОС.
        const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
        if (fd < 0) {
            if (errno == EEXIST) continue;   // коллизия: другая случайная часть
            return {};
        }
        bool ok = true;
        size_t at = 0;
        while (at < content.size()) {
            const ssize_t n = ::write(fd, content.data() + at, content.size() - at);
            if (n < 0) {
                ok = false;
                break;
            }
            at += size_t(n);
        }
        if (::close(fd) != 0) ok = false;
        if (!ok) return {};
        if (pathOut != nullptr) *pathOut = path;
        return id;
    }
    return {};
}

}  // namespace zametti
