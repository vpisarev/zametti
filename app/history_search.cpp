#include "history_search.h"

#include "document.h"

#include "history_panel.h"

#include <QElapsedTimer>

namespace zametti {

HistorySearchReport searchNoteHistory(const journal::History& history, const QString& noteId,
                                      const Query& query, int limit) {
    HistorySearchReport report;
    if (query.isEmpty() || query.tooShort()) return report;

    QElapsedTimer clock;
    clock.start();

    journal::Journal journal;
    QString error;
    if (!history.read(noteId, &journal, &error)) {
        report.elapsedMs = clock.elapsed();
        return report;
    }

    // СВЕЖИЕ СЛЕПКИ ПЕРВЫМИ: человек ищет «где это было», и ближайшее прошлое
    // ему нужнее давнего. Список поиска по хранилищу устроен так же — сперва
    // то, что вероятнее нужно.
    for (int i = int(journal.entries.size()) - 1; i >= 0; --i) {
        const journal::Entry& entry = journal.entries[i];
        if (!entry.hasSnapshot()) continue;   // у надгробия смотреть нечего
        QByteArray bytes;
        if (!history.snapshotAt(noteId, i, &bytes, &error)) continue;
        ++report.snapshots;

        // Разбираем тем же ядром, что и заметку: поиск обязан видеть ровно то,
        // что видит человек, — текст блоков, без меты и без разметки.
        ZDocument doc;
        doc.loadMarkdown(std::string_view(bytes.constData(), size_t(bytes.size())));
        const std::vector<Hit> hits = doc.find(query);
        if (hits.empty()) continue;
        ++report.withHits;

        // Заголовок группы — дата слепка: заголовок заметки у всех слепков
        // один и тот же, и по нему их не различить.
        const QString title = historyMoment(entry.time());
        for (const Hit& hit : hits) {
            if (report.hits.size() >= limit) {
                report.truncated = true;
                break;
            }
            const HitLine line = doc.hitLine(hit);
            SearchResult result;
            result.noteId = noteId + QStringLiteral("@%1").arg(entry.time());   // ключ группы
            result.title = title;
            result.line = line.text;
            result.lineOffset = line.offset;
            result.lineLength = line.length;
            result.ordinal = hit.ordinal;
            result.snapshotTime = entry.time();
            result.snapshotDigest = entry.digest();
            report.hits.append(result);
        }
        if (report.truncated) break;
    }
    report.elapsedMs = clock.elapsed();
    return report;
}

}  // namespace zametti
