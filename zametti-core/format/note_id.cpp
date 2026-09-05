#include "note_id.h"

#include <fcntl.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <random>

// Заведение файла «строго на свежее имя» — единственное место во всей
// программе, где мы ходим в файловую систему мимо Qt. Причина одна и названа
// ниже у самого open: у QFile нет O_EXCL, а «проверить и создать» двумя
// действиями — это гонка. Плата — две ветки на две системы, и обе живут ЗДЕСЬ,
// в одной функции.
#ifdef _WIN32
#include <io.h>
#include <sys/stat.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

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

NewFileResult writeNewFile(const std::string& path, const void* data, std::size_t size) {
    // O_EXCL — единственная честная защита от гонки: проверка «файла нет» и
    // создание — одно действие ядра ОС. Ради него мы и не берём QSaveFile.
    //
    // O_BINARY обязателен, а не желателен: у mingw поток по умолчанию
    // ТЕКСТОВЫЙ, и всякий '\n' на записи превращается в CRLF. Канон на диске —
    // markdown с LF, а вложения вообще двоичные: без этого флага и заметки, и
    // картинки уезжали бы на диск порчеными, причём молча.
#ifdef _WIN32
    // Путь у нас UTF-8, а узкий open() на Windows читает его в кодировке ANSI
    // текущей системы — то есть хранилище в каталоге с кириллицей в имени не
    // открылось бы вовсе. Поэтому переводим в UTF-16 и зовём широкий вариант:
    // он от кодовой страницы не зависит.
    const int wide = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    if (wide <= 0) return NewFileResult::Failed;
    std::wstring wpath(std::size_t(wide - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wpath.data(), wide);
    const int fd = ::_wopen(wpath.c_str(), _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY,
                            _S_IREAD | _S_IWRITE);
#else
#ifndef O_BINARY
#define O_BINARY 0
#endif
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_BINARY, 0644);
#endif
    if (fd < 0) return errno == EEXIST ? NewFileResult::Exists : NewFileResult::Failed;

    bool ok = true;
    std::size_t at = 0;
    const char* bytes = static_cast<const char*>(data);
    while (at < size) {
        // Кусок режем: у Windows ::_write берёт unsigned int, а не size_t.
        const unsigned chunk = unsigned(std::min<std::size_t>(size - at, 1u << 20));
#ifdef _WIN32
        const int n = ::_write(fd, bytes + at, chunk);
#else
        const auto n = ::write(fd, bytes + at, chunk);
#endif
        if (n <= 0) {
            ok = false;
            break;
        }
        at += std::size_t(n);
    }
#ifdef _WIN32
    if (::_close(fd) != 0) ok = false;
#else
    if (::close(fd) != 0) ok = false;
#endif
    if (ok) return NewFileResult::Created;

    // Недописанное убираем: половина заметки или половина картинки в хранилище
    // хуже, чем их отсутствие, — и покажется сломанной, и место займёт.
#ifdef _WIN32
    ::_wunlink(wpath.c_str());
#else
    ::unlink(path.c_str());
#endif
    return NewFileResult::Failed;
}

std::string createNoteFile(const std::string& dir, const std::string& content,
                           std::string* pathOut,
                           const std::function<std::string()>& generator) {
    const std::string name = createAttachmentFile(dir, "md", content.data(), content.size(),
                                                  generator);
    if (name.empty()) return {};
    if (pathOut != nullptr) {
        std::string base = dir;
        while (base.size() > 1 && base.back() == '/') base.pop_back();
        *pathOut = base + "/" + name;
    }
    return name.substr(0, name.size() - 3);
}

std::string createAttachmentFile(const std::string& dir, const std::string& suffix,
                                 const void* data, std::size_t size,
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
        const std::string name = id + "." + suffix;
        const std::string path = base + "/" + name;

        switch (writeNewFile(path, data, size)) {
            case NewFileResult::Exists:
                continue;   // коллизия: берём другую случайную часть
            case NewFileResult::Failed:
                return {};
            case NewFileResult::Created:
                return name;
        }
    }
    return {};
}

}  // namespace zametti
