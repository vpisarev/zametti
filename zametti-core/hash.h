// Хеш содержимого — один на весь проект.
//
// Задача у него одна: ответить «те же это байты или другие», не сравнивая
// сами байты. Отсюда три применения, и все три об одном:
//   - не писать файл, если содержимое не изменилось;
//   - при возврате в закэшированную заметку понять, не правил ли её кто-то
//     снаружи, не перечитывая и не разбирая файл;
//   - позже — целостность журнала и синхронизации.
//
// BLAKE3 (вендоринг, см. 3rdparty/blake3): криптостойкий, поэтому границы
// применимости помнить не надо — «совпало» значит совпало. Быстрых, но хрупких
// хешей вроде FNV или CRC здесь сознательно нет: цена ошибки — молча показанная
// человеку не та заметка.
//
// Ядро без Qt (за этим следит набор core-without-qt), поэтому и здесь только
// стандартная библиотека.

#ifndef ZAMETTI_HASH_H
#define ZAMETTI_HASH_H

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace zametti {

// 32 байта BLAKE3. Пустой отпечаток (все нули) означает «не считали»: так его
// заводят те, кому ещё нечего хешировать, и по нему же видно, что сверять не с
// чем.
struct Digest {
    std::array<uint8_t, 32> bytes{};

    bool empty() const;
    // Шестнадцатеричный вид, 64 знака в нижнем регистре — для файлов и глаз.
    std::string hex() const;

    friend bool operator==(const Digest& a, const Digest& b) { return a.bytes == b.bytes; }
    friend bool operator!=(const Digest& a, const Digest& b) { return !(a == b); }
};

Digest hashOf(std::string_view content);

}  // namespace zametti

#endif  // ZAMETTI_HASH_H
