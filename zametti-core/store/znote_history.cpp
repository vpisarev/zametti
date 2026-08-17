#include "znote_history.h"

#include <QDateTime>

#include <cstdio>

namespace zametti {

ZNoteHistory::ZNoteHistory(QString storeRoot, QString noteId, history::Rules rules)
    : root_(std::move(storeRoot)), id_(std::move(noteId)), rules_(rules) {}

void ZNoteHistory::ensureBaseline(const QByteArray& contents, qint64 fileTimeMs) {
    if (!available()) return;
    journal::History history(root_);
    journal::Journal journal;
    QString error;
    if (!history.read(id_, &journal, &error)) {
        std::fprintf(stderr, "история не читается: %s\n", error.toUtf8().constData());
        return;
    }
    if (!journal.entries.isEmpty()) return;   // история уже начата
    const qint64 when = fileTimeMs > 0 ? fileTimeMs : QDateTime::currentMSecsSinceEpoch();
    if (!history.append(id_, journal::Kind::Save, when, contents, 0, &error))
        std::fprintf(stderr, "опорная запись не записана: %s\n", error.toUtf8().constData());
}

void ZNoteHistory::compressOnce() {
    if (!available() || compressed_) return;   // за один заход в заметку — один раз
    compressed_ = true;
    journal::History history(root_);
    QString error;
    if (!history::compressJournal(history, id_, rules_, false, nullptr, &error))
        std::fprintf(stderr, "история не вычищена: %s\n", error.toUtf8().constData());
    tailKnown_ = false;   // хвост мог переехать
}

void ZNoteHistory::loadTail(journal::History& history) {
    if (tailKnown_) return;
    tailKnown_ = true;
    tail_.clear();
    tailTime_ = 0;
    journal::Journal read;
    QString error;
    if (history.read(id_, &read, &error) && !read.entries.isEmpty()) {
        const int last = int(read.entries.size()) - 1;
        if (read.entries[last].hasSnapshot() && history.snapshotAt(id_, last, &tail_, &error))
            tailTime_ = read.entries[last].time;
    }
}

bool ZNoteHistory::record(journal::Kind kind, const QByteArray& snapshot, QString* error) {
    qint64 source = 0;
    if (kind == journal::Kind::Save && restoreSource_ != 0) {
        kind = journal::Kind::Restore;
        source = restoreSource_;
    }
    if (kind == journal::Kind::Save || kind == journal::Kind::Restore) restoreSource_ = 0;
    if (!available()) return false;

    journal::History history(root_);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QString why;
    QString* err = error != nullptr ? error : &why;

    // ПЕРВАЯ ЗАПИСЬ В ЖУРНАЛ — первый из двух триггеров ленивой миграции.
    // Старый журнал чистится ДО того, как правило отбора начнёт сравнивать
    // новый слепок с хвостом: иначе оно работало бы поверх дубликатов, которых
    // призвано не допускать. Хвост после чистки разжимается заново.
    compressOnce();
    loadTail(history);

    // РЕШЕНИЕ ПРИНИМАЕТ ОБЩИЙ СВОД ПРАВИЛ (history_rules.h) — тот же, что
    // чистит старую историю. Здесь остаётся механика: прочитать журнал, отдать
    // правилу слепки и сделать, что сказано.
    journal::Journal read;
    if (!history.read(id_, &read, err)) {
        std::fprintf(stderr, "история не читается: %s\n", err->toUtf8().constData());
        tailKnown_ = false;
        return false;
    }
    // Слепки правило спрашивает по одному и только те, до которых дошло: у
    // хвоста они уже в памяти (ради этого журнал не разжимается), за
    // остальными идём в журнал.
    const int lastIndex = int(read.entries.size()) - 1;
    const auto snapshotOf = [&](int i) -> QByteArray {
        if (i == lastIndex && tailTime_ > 0) return tail_;
        QByteArray older;
        QString ignored;
        if (!history.snapshotAt(id_, i, &older, &ignored)) return {};
        return older;
    };
    const history::Step step =
        history::decideStep(read.entries, snapshotOf, snapshot, kind, now, rules_);

    bool ok = true;
    if (step.keep < int(read.entries.size())) ok = history.truncate(id_, step.keep, err);
    if (ok && step.writeNew) ok = history.append(id_, kind, now, snapshot, source, err);
    if (!ok) {
        std::fprintf(stderr, "история не записана: %s\n", err->toUtf8().constData());
        tailKnown_ = false;   // что там теперь — неизвестно
        return false;
    }
    tail_ = snapshot;
    tailTime_ = now;
    return true;
}

bool ZNoteHistory::read(journal::Journal* out, QString* error) {
    if (!available()) {
        if (error != nullptr) *error = QStringLiteral("у заметки нет журнала");
        return false;
    }
    compressOnce();
    return journal::History(root_).read(id_, out, error);
}

bool ZNoteHistory::snapshotAt(int index, QByteArray* out, QString* error) const {
    if (!available()) {
        if (error != nullptr) *error = QStringLiteral("у заметки нет журнала");
        return false;
    }
    return journal::History(root_).snapshotAt(id_, index, out, error);
}

}  // namespace zametti
