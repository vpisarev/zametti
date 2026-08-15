// Дамп заметки в JSON — односторонне, только для золотых наборов и отладки.
//
// Обратного пути нет и не будет: иначе дамп незаметно станет вторым форматом
// хранения. Единственный формат на диске — markdown.
//
// СУДЬЯ КОРПУСОВ. Золотые наборы сверяют этот дамп с файлами на диске, поэтому
// он и обязан быть методом заметки: раньше дампилось промежуточное
// представление, и сверялось, стало быть, оно, а не то, что человек видит и
// правит. Теперь дамп идёт тем же обходом живого документа (walkPieces), что и
// запись в файл, — а значит золотые файлы стерегут ровно ту границу
// «документ → файл», через которую проходят все данные владельца.

#include "document_impl.h"

#include "document_pieces.h"

#include <cstdio>
#include <string>
#include <string_view>

namespace zametti {
namespace {

const char* kindName(Kind k) {
    switch (k) {
        case Kind::Paragraph:     return "paragraph";
        case Kind::Heading:       return "heading";
        case Kind::Code:          return "code";
        case Kind::Quote:         return "quote";
        case Kind::VSpace:        return "vspace";
        case Kind::ListItem:      return "list-item";
        case Kind::Divider:       return "divider";
        case Kind::Math:          return "math";
        case Kind::Html:          return "html";
    }
    return "?";
}

const char* markerName(Marker m) {
    switch (m) {
        case Marker::Bullet:  return "bullet";
        case Marker::Ordered: return "ordered";
        case Marker::Task:    return "task";
    }
    return "?";
}

void appendJsonString(std::string& out, std::string_view s) {
    out.push_back('"');
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    // UTF-8 отдаём как есть: дамп читают глазами.
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    out.push_back('"');
}

void appendInt(std::string& out, int v) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%d", v);
    out += buf;
}

void appendPiece(std::string& out, const Piece& b) {
    out += "  {";
    if (b.raw) {
        out += "\"raw\": ";
        appendJsonString(out, b.text);
        out += "}";
        return;
    }
    out += "\"kind\": ";
    out += '"';
    out += kindName(b.kind);
    out += '"';
    if (b.kind == Kind::Heading) {
        out += ", \"headingLevel\": ";
        appendInt(out, b.headingLevel);
    }
    if (isList(b.kind)) {
        out += ", \"marker\": \"";
        out += markerName(b.marker);
        out += '"';
        if (b.marker == Marker::Task)
            out += b.checked ? ", \"checked\": true" : ", \"checked\": false";
    }
    if (b.level >= 0) {
        out += ", \"level\": ";
        appendInt(out, b.level);
    }
    if (b.kind == Kind::Code && !b.info.empty()) {
        out += ", \"info\": ";
        appendJsonString(out, b.info);
    }
    out += ", \"text\": ";
    appendJsonString(out, b.text);
    if (!b.runs.empty()) {
        out += ", \"inlines\": [";
        bool firstRun = true;
        for (const Run& s : b.runs) {
            if (!firstRun) out += ", ";
            firstRun = false;
            // Смещение куска относительное — от начала текста блока, — и в
            // дампе оно таким и было всегда.
            out += "{\"offset\": ";
            appendInt(out, s.start);
            out += ", \"length\": ";
            appendInt(out, s.end - s.start);
            if (s.bold()) out += ", \"bold\": true";
            if (s.italic()) out += ", \"italic\": true";
            if (s.strike()) out += ", \"strike\": true";
            if (s.code()) out += ", \"code\": true";
            if (s.image()) out += ", \"image\": true";
            if (s.comment()) out += ", \"comment\": true";
            if (!s.href.empty()) {
                out += ", \"href\": ";
                appendJsonString(out, s.href);
            }
            if (!s.title.empty()) {
                out += ", \"title\": ";
                appendJsonString(out, s.title);
            }
            out += "}";
        }
        out += "]";
    }
    out += "}";
}

// Шапка своей секцией и только когда есть: дамп без неё читается как раньше,
// простым списком блоков.
void appendHead(std::string& out, const NoteHeader& header) {
    out += "{\"meta\": {\"lines\": [";
    for (size_t i = 0; i < header.lines().size(); ++i) {
        if (i) out += ", ";
        appendJsonString(out, header.lines()[i]);
    }
    out += "], \"blankAfter\": ";
    out += header.blankAfter() ? "true" : "false";
    out += "},\n \"blocks\":\n";
}

}  // namespace

std::string ZDocument::toJson() const {
    std::string out;
    const bool head = d_->header.present();
    if (head) appendHead(out, d_->header);
    out += "[\n";

    // Запятая ставится ПЕРЕД следующим блоком, а не после предыдущего: сколько
    // блоков отдаст обход, заранее неизвестно — он идёт по живому документу и
    // склеивает литеральные строки по дороге.
    bool first = true;
    walkPieces(d_->text, [&](const Piece& piece) {
        if (!first) out += ",\n";
        first = false;
        appendPiece(out, piece);
    });
    if (!first) out += "\n";

    out += "]";
    if (head) out += "}";
    out += "\n";
    return out;
}

std::string dumpPieces(const std::vector<Piece>& blocks, const NoteHeader& header) {
    std::string out;
    if (header.present()) appendHead(out, header);
    out += "[\n";
    for (size_t i = 0; i < blocks.size(); ++i) {
        appendPiece(out, blocks[i]);
        if (i + 1 < blocks.size()) out += ",";
        out += "\n";
    }
    out += "]";
    if (header.present()) out += "}";
    out += "\n";
    return out;
}

}  // namespace zametti
