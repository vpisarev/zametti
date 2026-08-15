// Логический блок заметки и куски его строки — рабочие структуры разбора и
// записи.
//
// ЭТО НЕ ПРЕДСТАВЛЕНИЕ ЗАМЕТКИ. Живая модель одна — QTextDocument внутри
// ZDocument. Здешние значения живут внутри одного вызова (разобрать байты,
// записать байты) и покрывают один блок. Второй живой моделью они не
// становятся ровно потому, что НЕ ЖИВУТ: собрал, отдал, забыл.
//
// Держать их в поле — повод остановиться и спросить себя, не должен ли этот
// вопрос быть глаголом заметки. Наборы включают этот заголовок нарочно
// (tests/pieces.h): предмет их проверки — сами блоки.
//
// Нужны они затем, что и разбор, и запись работают ПОБЛОЧНО: нельзя записать
// блок кода, не увидев все его строки, и нельзя положить блок в документ, не
// собрав его текст целиком.

#pragma once

#include "block_kind.h"
#include "note_header.h"

#include <cstddef>

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

class QTextDocument;

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

    // Законченный HTML-комментарий: он обрывает себя сам, и сосед начинается
    // заново — замерено на md4c для кода, абзаца, черты, таблицы и второго
    // комментария. Спрашивают об этом и писатель (можно ли ставить соседа
    // вплотную), и стаб архива (заголовком такой кусок не считается), поэтому
    // правило живёт здесь одно.
    bool isClosedHtmlComment() const {
        if (!raw || text.size() < 8) return false;
        return text.compare(0, 4, "<!--") == 0 && text.compare(text.size() - 4, 4, "-->\n") == 0;
    }
};

// Сколько байт черновика резервирует разбор под источник этой длины. В черновик
// уезжают текст блоков, дословные куски, info-строки и адреса — всё это куски
// исходника, и суммарно они его не превосходят (замер на корпусах: k ≈ 0.9).
// Запас нужен на переезды при правке текста внутри самого разбора.
constexpr size_t draftReserveFor(size_t sourceLength) {
    return sourceLength + sourceLength / 8 + 1024;
}

// Где какой блок оказался в выводе: номер первой строки (с нуля) и сколько
// строк занял. Блок, не давший ни строки, получает count == 0. Нужна разности
// версий: единица сравнения — строка, а полоски на поле рисуются по блокам.
struct BlockLines {
    int first = 0;
    int count = 0;
};

// Разбор байтов markdown в логические блоки и шапку. Определено в
// markdown_reader.cpp; ступень внутри ZDocument::loadMarkdown, наружу из ядра
// не выходит.
void parsePieces(std::string_view markdown, std::vector<Piece>& blocks, NoteHeader& header);

// Обход ЖИВОГО документа теми же логическими блоками — обратная ступень к
// parsePieces. Определено в markdown_writer.cpp.
//
// Блок отдаётся по одному и живёт только внутри вызова: собирать из них список
// незачем, а кто соберёт — заведёт ровно ту вторую копию содержимого, от
// которой мы уходим. Литеральные куски (код, дословное) лежат в документе
// построчно и склеиваются здесь обратно, поэтому обход и нужен общий: два
// потребителя, склеивающих строки каждый по-своему, однажды склеят по-разному.
//
// ЛОЖЬ ИЗ sink ОБРЫВАЕТ ОБХОД. Это не удобство, а цена вопроса: заголовок
// заметки — её первый содержательный блок, и спрашивать его обходом всей
// заметки значит платить за показ списка размером самой большой заметки.
void walkPieces(const QTextDocument& doc, const std::function<bool(const Piece&)>& sink);

// Байты канонического markdown из логических блоков. Нужна там, где блоки
// собраны на месте и заметкой ещё не стали, — куску в буфере обмена, стороне
// сравнения. Идёт тем же писателем, что и запись на диск: второго писателя не
// бывает.
std::string writePieces(const std::vector<Piece>& blocks, const NoteHeader& header = {},
                        std::vector<BlockLines>* map = nullptr);

// Строение блоков в JSON — односторонне, для золотых наборов и отладки. Та же
// печать, что и у ZDocument::toJson: у дампа один вид, из скольких бы мест его
// ни просили.
std::string dumpPieces(const std::vector<Piece>& blocks, const NoteHeader& header = {});

}  // namespace zametti
