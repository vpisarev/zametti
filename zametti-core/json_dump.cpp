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

void appendJsonString(std::string& out, const std::string& s) {
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
    std::string out = "[\n";
    for (size_t i = 0; i < doc.size(); ++i) {
        const Block& b = doc[i];
        out += "  {";
        if (!b.rawSource.empty()) {
            out += "\"raw\": ";
            appendJsonString(out, b.rawSource);
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
                out += ", \"level\": ";
                appendInt(out, b.level);
            }
            if (b.kind == Kind::Code && !b.info.empty()) {
                out += ", \"info\": ";
                appendJsonString(out, b.info);
            }
            out += ", \"text\": ";
            appendJsonString(out, b.text);
            if (!b.inlines.empty()) {
                out += ", \"inlines\": [";
                for (size_t j = 0; j < b.inlines.size(); ++j) {
                    const Span& s = b.inlines[j];
                    if (j) out += ", ";
                    out += "{\"offset\": ";
                    appendInt(out, s.offset);
                    out += ", \"length\": ";
                    appendInt(out, s.length);
                    if (s.bold) out += ", \"bold\": true";
                    if (s.italic) out += ", \"italic\": true";
                    if (s.strike) out += ", \"strike\": true";
                    if (s.code) out += ", \"code\": true";
                    if (!s.href.empty()) {
                        out += ", \"href\": ";
                        appendJsonString(out, s.href);
                    }
                    out += "}";
                }
                out += "]";
            }
        }
        out += "}";
        if (i + 1 < doc.size()) out += ",";
        out += "\n";
    }
    out += "]\n";
    return out;
}

}  // namespace zametti
