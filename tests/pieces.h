// Словарь наборов после сноса промежуточного представления.
//
// Раньше наборы разговаривали с ядром через zametti::Document — арену блоков и
// спанов, — и три её глагола (parse, serialize, readDocument) стояли в двух
// с половиной сотнях мест. Представления больше нет; на его месте логический
// блок (Piece), который живёт внутри одного вызова и ничем не владеет, кроме
// собственного текста.
//
// Здесь ровно те же три вопроса, только к новому словарю. Ничего своего эти
// обёртки не считают: каждая — один вызов ядра, и переехать на них наборам
// стоило одной замены имени.
//
// Заметку целиком спрашивают у ZDocument, а не отсюда: эти четыре нужны там,
// где предмет проверки — сами блоки (что даёт разбор, что уходит в файл, что
// помнит цепочка отмены).

#pragma once

#include "document.h"
#include "document_pieces.h"

#include <QTextDocument>

#include <string>
#include <string_view>
#include <vector>

// Байты markdown → логические блоки. Шапка отбрасывается: её спрашивают у
// заметки.
inline std::vector<zametti::Piece> pieces(std::string_view markdown) {
    std::vector<zametti::Piece> out;
    zametti::NoteHeader header;
    zametti::parsePieces(markdown, out, header);
    return out;
}

// То же, но с шапкой: она нужна наборам хранилища.
inline std::vector<zametti::Piece> pieces(std::string_view markdown,
                                          zametti::NoteHeader& header) {
    std::vector<zametti::Piece> out;
    zametti::parsePieces(markdown, out, header);
    return out;
}

// Логические блоки → канонические байты. Писатель отдаёт текст (QString);
// наборы сравнивают с байтовыми литералами, и граница переводится здесь.
inline std::string markdownOf(const std::vector<zametti::Piece>& blocks,
                              const zametti::NoteHeader& header = {}) {
    return zametti::writePieces(blocks, header).toStdString();
}

// Текст блока байтами UTF-8 — для сравнения с литералами наборов: сам текст в
// памяти QString (решение владельца, refactor2).
inline std::string utf8(const QString& text) { return text.toStdString(); }

// Живой документ → логические блоки.
inline std::vector<zametti::Piece> blocksOf(const QTextDocument& doc) {
    std::vector<zametti::Piece> out;
    zametti::walkPieces(doc, [&](const zametti::Piece& piece) {
        out.push_back(piece);
        return true;
    });
    return out;
}

// Строение блоков в JSON — тот же дамп, что сверяют золотые наборы.
inline std::string dumpOf(const std::vector<zametti::Piece>& blocks,
                          const zametti::NoteHeader& header = {}) {
    return zametti::dumpPieces(blocks, header);
}

// Заметка из байтов — там, где предмет проверки сама заметка, а не блоки.
inline zametti::ZDocument noteOf(std::string_view markdown) {
    zametti::ZDocument out;
    out.loadMarkdown(markdown);
    return out;
}

// Ключ шапки в UTF-8: наборы сравнивают со std::string-литералами.
inline std::string head(const zametti::ZDocument& note, const char* key) {
    return note.headerValue(QString::fromUtf8(key)).toStdString();
}

inline void setHead(zametti::ZDocument& note, const char* key, const std::string& value) {
    note.setHeaderValue(QString::fromUtf8(key), QString::fromStdString(value));
}
