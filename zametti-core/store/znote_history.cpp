#include "znote_history.h"

#include "zstorage.h"

#include <QDateTime>

#include <cstdio>

namespace zametti {

ZNoteHistory::ZNoteHistory(ZStorage* store, QString noteId, journal::ZJournal::Rules rules)
    : store_(store), id_(std::move(noteId)), rules_(rules) {}

void ZNoteHistory::ensureBaseline(const QByteArray& contents, qint64 fileTimeMs) {
    if (!available()) return;
    journal::ZJournal journal;
    QString error;
    if (!store_->readJournal(id_, &journal, &error)) {
        std::fprintf(stderr, "cannot read history: %s\n", error.toUtf8().constData());
        return;
    }
    if (!journal.isEmpty()) return;   // история уже начата
    // Опорной записи отдают время ФАЙЛА: заметка, лежавшая с 2017 года,
    // обязана и в истории начинаться 2017 годом. Поэтому момент назван, а не
    // «сейчас», и страж монотонности его не поднимает.
    const journal::Stamp when = fileTimeMs > 0 ? journal::Stamp::at(fileTimeMs)
                                               : journal::Stamp::now();
    if (!store_->appendToJournal(id_, journal::NewRecord::save(contents, when), &error))
        std::fprintf(stderr, "baseline record not written: %s\n", error.toUtf8().constData());
}

void ZNoteHistory::compressOnce() {
    if (!available() || compressed_) return;   // за один заход в заметку — один раз
    compressed_ = true;
    QString error;
    if (!store_->compressJournal(id_, rules_, false, nullptr, &error))
        std::fprintf(stderr, "history not cleaned: %s\n", error.toUtf8().constData());
    tailKnown_ = false;   // хвост мог переехать
}

void ZNoteHistory::loadTail() {
    if (tailKnown_) return;
    tailKnown_ = true;
    tail_.clear();
    tailTime_ = 0;
    journal::ZJournal read;
    QString error;
    if (store_->readJournal(id_, &read, &error) && !read.isEmpty()) {
        // Голова, а не последняя по файлу: с чем сравнивать свежий слепок,
        // решает ПОРЯДОК записей, а не их укладка. Разойтись эти две вещи
        // могут только у журнала, побывавшего в синхронизации, — и тогда
        // мелкая правка слилась бы не с той записью.
        const int last = read.lastSnapshotIndex();
        if (last >= 0 && store_->journalSnapshot(id_, last, &tail_, &error))
            tailTime_ = read.at(last).time();
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

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QString why;
    QString* err = error != nullptr ? error : &why;

    // ПЕРВАЯ ЗАПИСЬ В ЖУРНАЛ — первый из двух триггеров ленивой миграции.
    // Старый журнал чистится ДО того, как правило отбора начнёт сравнивать
    // новый слепок с хвостом: иначе оно работало бы поверх дубликатов, которых
    // призвано не допускать. Хвост после чистки разжимается заново.
    compressOnce();
    loadTail();

    // РЕШЕНИЕ ПРИНИМАЕТ ОБЩИЙ СВОД ПРАВИЛ (history_rules.h) — тот же, что
    // чистит старую историю. Здесь остаётся механика: прочитать журнал, отдать
    // правилу слепки и сделать, что сказано.
    journal::ZJournal read;
    if (!store_->readJournal(id_, &read, err)) {
        std::fprintf(stderr, "cannot read history: %s\n", err->toUtf8().constData());
        tailKnown_ = false;
        return false;
    }
    // Слепки правило спрашивает по одному и только те, до которых дошло: у
    // хвоста они уже в памяти (ради этого журнал не разжимается), за
    // остальными идём в журнал.
    const int lastIndex = read.size() - 1;
    const auto snapshotOf = [&](int i) -> QByteArray {
        if (i == lastIndex && tailTime_ > 0) return tail_;
        QByteArray older;
        QString ignored;
        if (!store_->journalSnapshot(id_, i, &older, &ignored)) return {};
        return older;
    };
    const journal::ZJournal::Step step = read.planStep(snapshotOf, snapshot, kind, now, rules_);

    // ГАШЕНИЕ ВМЕСТО СТИРАНИЯ. Записи, которые правило объявило лишними,
    // адресуются парой (время, отпечаток) и едут этим адресом в новой записи:
    // их байты выкидываются здесь же, а другое устройство, увидев новую запись,
    // погасит те же у себя. Стирание без адреса не доезжало никуда — уехавшая
    // запись возвращалась объединением и возвращалась бы вечно.
    QVector<journal::EntryRef> voids;
    voids.reserve(step.voided.size());
    for (int i : step.voided) voids.append(journal::EntryRef(read.at(i).time(), read.at(i).digest()));

    bool ok = true;
    if (step.writeNew) {
        journal::NewRecord what = kind == journal::Kind::Restore
                                      ? journal::NewRecord::restore(snapshot, source)
                                      : (kind == journal::Kind::External
                                             ? journal::NewRecord::external(snapshot)
                                             : journal::NewRecord::save(snapshot));
        ok = store_->appendToJournal(id_, what.voiding(voids), err);
    } else if (!voids.isEmpty()) {
        // Человек вернулся к уже записанному состоянию: нового слепка нет, а
        // сказать «того, что между, больше нет» надо.
        ok = store_->appendToJournal(id_, journal::NewRecord::amendment().voiding(voids), err);
    }
    if (!ok) {
        std::fprintf(stderr, "history not written: %s\n", err->toUtf8().constData());
        tailKnown_ = false;   // что там теперь — неизвестно
        return false;
    }
    tail_ = snapshot;
    tailTime_ = now;
    return true;
}

bool ZNoteHistory::read(journal::ZJournal* out, QString* error) {
    if (!available()) {
        if (error != nullptr) *error = QStringLiteral("note has no journal");
        return false;
    }
    compressOnce();
    return store_->readJournal(id_, out, error);
}

bool ZNoteHistory::snapshotAt(int index, QByteArray* out, QString* error) const {
    if (!available()) {
        if (error != nullptr) *error = QStringLiteral("note has no journal");
        return false;
    }
    return store_->journalSnapshot(id_, index, out, error);
}

}  // namespace zametti
