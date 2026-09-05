// Идентификатор заметки — имя файла в плоском хранилище: "<id>.md".
//
// 14 знаков Crockford base32 в нижнем регистре (0-9 и a-z без i l o u):
// 8 знаков — unix-секунды создания, big-endian, с ведущими нулями; 6 знаков —
// случайные (CSPRNG). До ближайшего тысячелетия все id начинаются с '0' — это
// ожидаемо и служит де-факто признаком формата.
//
// В логике id непрозрачен: время из него не разбирается никогда, настоящая
// дата создания живёт в метаданных. Временной префикс — только для сортировки
// ls и глаз при отладке.

#ifndef ZAMETTI_NOTE_ID_H
#define ZAMETTI_NOTE_ID_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace zametti {

inline constexpr int kNoteIdLength = 14;

// Детерминированное ядро: время и случайная часть заданы снаружи.
// Секунды берутся по модулю 32^8 (год ~36812), случайная часть — по модулю
// 32^6.
std::string makeNoteId(std::uint64_t unixSeconds, std::uint64_t random);

// Время сейчас + CSPRNG.
std::string newNoteId();

// ^[0-9a-hjkmnp-tv-z]{14}$
bool isValidNoteId(std::string_view id);

// Заводит файл СТРОГО НА СВЕЖЕЕ ИМЯ и пишет в него все байты. Единственное
// место в программе, где файл открывается мимо Qt, и причина названа прямо: у
// QFile/QSaveFile нет O_EXCL, а «проверить, что имени нет, и создать» двумя
// действиями — это гонка. Здесь же спрятана и вся разница между системами
// (двоичный режим, широкий путь под Windows) — у зовущих её быть не должно.
//
// path — UTF-8. Недописанный файл функция убирает за собой сама.
enum class NewFileResult {
    Created,   // заведён и записан целиком
    Exists,    // имя занято — звать заново с другим id
    Failed,    // беда ввода-вывода; следов на диске не осталось
};
NewFileResult writeNewFile(const std::string& path, const void* data, std::size_t size);

// Создаёт файл "<id>.md" в каталоге строго на свежее имя (O_EXCL) и пишет в
// него content. При совпадении имени перегенерирует id — случайная часть
// другая, время успело утечь или нет, неважно. Возвращает id; пусто — ошибка
// ввода-вывода (не коллизия). pathOut, если задан, получает полный путь.
//
// generator подставной ради тестов: боевой — newNoteId.
std::string createNoteFile(const std::string& dir, const std::string& content,
                           std::string* pathOut = nullptr,
                           const std::function<std::string()>& generator = {});

// The same for an ATTACHMENT, "<id>.<suffix>": a fresh name under O_EXCL, a
// new id on a collision, the bytes written whole. Returns the file NAME
// ("<id>.<suffix>", not the path); empty — an I/O failure. One loop for the
// notes, the pictures the editor inserts (image_insert.cpp) and the pictures
// a book brings (importBook): a second copy of it once lived in the app.
std::string createAttachmentFile(const std::string& dir, const std::string& suffix,
                                 const void* data, std::size_t size,
                                 const std::function<std::string()>& generator = {});

}  // namespace zametti

#endif  // ZAMETTI_NOTE_ID_H
