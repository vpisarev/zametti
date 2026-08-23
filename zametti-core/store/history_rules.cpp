#include "history_rules.h"

#include <QtGlobal>

#include <utility>

namespace zametti {

// Шапка заметки — HTML-комментарий в начале файла. Строки modified и version
// ищем только в ней: слово «modified:» в тексте заметки трогать нельзя.
//
// `version` — тоже не содержимое (refactor3): версия формата встаёт в шапку
// лениво, при первой записи правленой заметки, и без этой оговорки возврат
// отменой к состоянию, записанному ДО неё, считался бы новой записью журнала
// (набор HistoryWrite это и поймал), а заметка, вернувшаяся к исходному
// тексту, — изменённой.
bool sameApartFromModified(const QByteArray& a, const QByteArray& b) {
    const auto stripped = [](const QByteArray& text) {
        const qsizetype head = text.indexOf("-->");
        if (head < 0) return text;
        QByteArray out = text;
        for (const char* key : {"\nmodified:", "\nversion:"}) {
            const qsizetype at = out.indexOf(key);
            if (at < 0 || at > out.indexOf("-->")) continue;
            const qsizetype eol = out.indexOf('\n', at + 1);
            if (eol < 0) continue;
            out.remove(at, eol - at);
        }
        return out;
    };
    if (a.size() == b.size() && a == b) return true;
    return stripped(a) == stripped(b);
}

int changedChars(const QByteArray& a, const QByteArray& b) {
    const qsizetype shared = qMin(a.size(), b.size());
    qsizetype prefix = 0;
    while (prefix < shared && a[prefix] == b[prefix]) ++prefix;
    qsizetype suffix = 0;
    while (suffix < shared - prefix && a[a.size() - 1 - suffix] == b[b.size() - 1 - suffix])
        ++suffix;
    return int(qMax(a.size(), b.size()) - prefix - suffix);
}

namespace history {

}  // namespace history
}  // namespace zametti
