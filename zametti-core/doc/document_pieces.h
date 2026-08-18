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

#include <QString>
#include <QStringView>

#include <cstddef>

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

class QTextDocument;

namespace zametti {

// Кусок строки с одним начертанием. Смещения — единицы UTF-16 от начала текста
// блока (решение владельца, сессия refactor2: всё, что в памяти, — QString;
// байты — только на границе файла).
struct Run {
    int32_t start = 0;
    int32_t end = 0;
    uint8_t flags = 0;      // биты InlineFlag
    QString href;
    QString title;

    bool bold() const { return (flags & InlineBold) != 0; }
    bool italic() const { return (flags & InlineItalic) != 0; }
    bool strike() const { return (flags & InlineStrike) != 0; }
    bool code() const { return (flags & InlineCode) != 0; }
    bool image() const { return (flags & InlineImage) != 0; }
    bool comment() const { return (flags & InlineComment) != 0; }
    bool math() const { return (flags & InlineMath) != 0; }
    bool mathOpen() const { return (flags & InlineMathOpen) != 0; }
    void set(uint8_t bit, bool on) { flags = uint8_t(on ? (flags | bit) : (flags & ~bit)); }
    bool empty() const { return end <= start; }
};

// Логический блок заметки. С сессии 5 refactor2 — ровно один QTextBlock на
// блок: и код, и дословный кусок лежат в документе одним блоком.
struct Piece {
    Kind kind = Kind::Paragraph;
    Marker marker = Marker::Bullet;
    HtmlKind html = HtmlKind::Comment;
    int level = -1;          // -1 — блок стоит снаружи списка
    int headingLevel = 0;
    bool checked = false;
    bool raw = false;               // выводится дословно
    // Дословный кусок — ТАБЛИЦА GFM (сказал md4c при разборе; у живого блока —
    // objectType == TableObject). Сборщик делает из такого куска объект, а не
    // литеральный текст; писатель на флаг не смотрит — байты те же.
    bool table = false;
    bool trailingNewline = false;   // текст кончался переводом строки
    QString info;                   // язык блока кода
    QString text;
    std::vector<Run> runs;

    QStringView view(const Run& r) const {
        return QStringView(text).mid(r.start, r.end - r.start);
    }

    // Законченный HTML-комментарий: он обрывает себя сам, и сосед начинается
    // заново — замерено на md4c для кода, абзаца, черты, таблицы и второго
    // комментария. Спрашивают об этом и писатель (можно ли ставить соседа
    // вплотную), и стаб архива (заголовком такой кусок не считается), поэтому
    // правило живёт здесь одно.
    bool isClosedHtmlComment() const {
        if (!raw || text.size() < 8) return false;
        return text.startsWith(QLatin1String("<!--")) && text.endsWith(QLatin1String("-->\n"));
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

// Разбор текста markdown в логические блоки и шапку. ТЕКСТА, а не байтов:
// md4c читает UTF-16, смещения — единицы UTF-16, те же, что у QTextDocument.
// Байты файла переводятся в текст один раз, в ZDocument::loadMarkdown.
// Определено в markdown_reader.cpp; ступень внутри ядра, наружу не выходит.
void parsePieces(QStringView markdown, std::vector<Piece>& blocks, NoteHeader& header);

// Обход ЖИВОГО документа теми же логическими блоками — обратная ступень к
// parsePieces. Определено в markdown_writer.cpp.
//
// Блок отдаётся по одному и живёт только внутри вызова: собирать из них список
// незачем, а кто соберёт — заведёт ровно ту вторую копию содержимого, от
// которой мы уходим. Обход общий на всех потребителей: объекты (фото, формула,
// таблица) отдают свой исходник здесь одним правилом, и второго прочтения
// документа, которое однажды прочло бы иначе, не бывает.
//
// ЛОЖЬ ИЗ sink ОБРЫВАЕТ ОБХОД. Это не удобство, а цена вопроса: заголовок
// заметки — её первый содержательный блок, и спрашивать его обходом всей
// заметки значит платить за показ списка размером самой большой заметки.
//
// ДИАПАЗОН — ТОЙ ЖЕ ЦЕНЫ РАДИ. Правка трогает несколько блоков, а пересобрать
// их обязан сборщик (только он знает, как выглядит заголовок), — и обходить
// ради этого всю заметку значило бы платить её размером за каждое нажатие.
// Границы задаются номерами QTextBlock, включая оба конца; toBlock < 0 — до
// конца.
void walkPieces(const QTextDocument& doc, const std::function<bool(const Piece&)>& sink,
                int fromBlock = 0, int toBlock = -1);

// Канонический markdown из логических блоков — ТЕКСТОМ (QString), не байтами:
// байты нужны только файлу, хешу и журналу, и в них текст переводится один раз
// на той границе. Нужна там, где блоки собраны на месте и заметкой ещё не
// стали, — куску в буфере обмена, стороне сравнения. Идёт тем же писателем, что
// и запись на диск: второго писателя не бывает.
QString writePieces(const std::vector<Piece>& blocks, const NoteHeader& header = {},
                    std::vector<BlockLines>* map = nullptr);

// Строение блоков в JSON — односторонне, для золотых наборов и отладки. Та же
// печать, что и у ZDocument::toJson: у дампа один вид, из скольких бы мест его
// ни просили.
std::string dumpPieces(const std::vector<Piece>& blocks, const NoteHeader& header = {});

}  // namespace zametti
