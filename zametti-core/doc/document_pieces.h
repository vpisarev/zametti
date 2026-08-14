// Логический блок заметки и куски его строки — рабочие структуры разбора и
// записи.
//
// ЭТО НЕ ПРЕДСТАВЛЕНИЕ ЗАМЕТКИ. Живая модель одна — QTextDocument внутри
// ZDocument. Здешние значения живут внутри одного вызова (разобрать байты,
// записать байты), покрывают один блок и наружу из zametti-core не выходят.
// Второй живой моделью они не становятся ровно потому, что не живут.
//
// Нужны они затем, что и разбор, и запись работают ПОБЛОЧНО: нельзя записать
// блок кода, не увидев все его строки, и нельзя положить блок в документ, не
// собрав его текст целиком.

#pragma once

#include "block_kind.h"

#include <cstddef>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace zametti {

// Кусок строки с одним начертанием. Смещения — байты от начала текста блока.
struct Run {
    int32_t start = 0;
    int32_t end = 0;
    uint8_t flags = 0;      // биты InlineFlag
    std::string href;
    std::string title;

    bool bold() const { return (flags & InlineBold) != 0; }
    bool italic() const { return (flags & InlineItalic) != 0; }
    bool strike() const { return (flags & InlineStrike) != 0; }
    bool code() const { return (flags & InlineCode) != 0; }
    bool image() const { return (flags & InlineImage) != 0; }
    bool comment() const { return (flags & InlineComment) != 0; }
    bool math() const { return (flags & InlineMath) != 0; }
    void set(uint8_t bit, bool on) { flags = uint8_t(on ? (flags | bit) : (flags & ~bit)); }
    bool empty() const { return end <= start; }
};

// Логический блок заметки. Не то же, что QTextBlock: литеральные куски (код,
// дословное) лежат в документе построчно, по блоку на строку, а здесь они
// целые.
struct Piece {
    Kind kind = Kind::Paragraph;
    Marker marker = Marker::Bullet;
    HtmlKind html = HtmlKind::Comment;
    int level = -1;          // -1 — блок стоит снаружи списка
    int headingLevel = 0;
    bool checked = false;
    bool raw = false;               // выводится дословно
    bool trailingNewline = false;   // текст кончался переводом строки
    std::string info;               // язык блока кода
    std::string text;
    std::vector<Run> runs;

    std::string_view view(const Run& r) const {
        return std::string_view(text).substr(size_t(r.start), size_t(r.end - r.start));
    }
};

// Сколько байт черновика резервирует разбор под источник этой длины. В черновик
// уезжают текст блоков, дословные куски, info-строки и адреса — всё это куски
// исходника, и суммарно они его не превосходят (замер на корпусах: k ≈ 0.9).
// Запас нужен на переезды при правке текста внутри самого разбора.
//
// Значение открыто наружу ради проверки инварианта «ноль перекладываний
// черновика»: иначе набору не с чем сравнивать.
constexpr size_t draftReserveFor(size_t sourceLength) {
    return sourceLength + sourceLength / 8 + 1024;
}

class NoteHeader;

// Разбор байтов markdown в логические блоки и шапку. Определено в
// markdown_reader.cpp; ступень внутри ZDocument::loadMarkdown, наружу из ядра
// не выходит.
void parsePieces(std::string_view markdown, std::vector<Piece>& blocks, NoteHeader& header);

}  // namespace zametti
