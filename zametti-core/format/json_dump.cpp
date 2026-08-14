#include "json_dump.h"

#include <cstdio>

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

}  // namespace

std::string toJson(const Document& doc) {
    std::string out;
    // Метаданные — своей секцией и только когда есть: дамп без них читается
    // как раньше, простым списком блоков.
    if (doc.meta.present()) {
        out += "{\"meta\": {\"lines\": [";
        for (size_t i = 0; i < doc.meta.lines().size(); ++i) {
            if (i) out += ", ";
            appendJsonString(out, doc.meta.lines()[i]);
        }
        out += "], \"blankAfter\": ";
        out += doc.meta.blankAfter() ? "true" : "false";
        out += "},\n \"blocks\":\n";
    }
    out += "[\n";
    for (size_t i = 0; i < doc.blocks.size(); ++i) {
        const Block& b = doc.blocks[i];
        out += "  {";
        if (b.raw) {
            out += "\"raw\": ";
            appendJsonString(out, doc.text(b));
        } else {
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
                appendJsonString(out, doc.info(b));
            }
            out += ", \"text\": ";
            appendJsonString(out, doc.text(b));
            if (!b.inlines.empty()) {
                out += ", \"inlines\": [";
                bool firstSpan = true;
                for (const Inline& s : doc.inlines(b)) {
                    if (!firstSpan) out += ", ";
                    firstSpan = false;
                    // Смещение спана относительное — от начала текста блока, —
                    // и в дампе оно таким и было всегда.
                    out += "{\"offset\": ";
                    appendInt(out, s.text.start);
                    out += ", \"length\": ";
                    appendInt(out, s.text.size());
                    if (s.bold()) out += ", \"bold\": true";
                    if (s.italic()) out += ", \"italic\": true";
                    if (s.strike()) out += ", \"strike\": true";
                    if (s.code()) out += ", \"code\": true";
                    if (s.image()) out += ", \"image\": true";
                    if (s.comment()) out += ", \"comment\": true";
                    if (!s.href.empty()) {
                        out += ", \"href\": ";
                        appendJsonString(out, doc.href(s));
                    }
                    if (!s.title.empty()) {
                        out += ", \"title\": ";
                        appendJsonString(out, doc.title(s));
                    }
                    out += "}";
                }
                out += "]";
            }
        }
        out += "}";
        if (i + 1 < doc.blocks.size()) out += ",";
        out += "\n";
    }
    out += "]";
    if (doc.meta.present()) out += "}";
    out += "\n";
    return out;
}

}  // namespace zametti
