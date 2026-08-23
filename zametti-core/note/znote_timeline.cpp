#include "znote_timeline.h"

#include <cstdio>

namespace zametti {

ZNoteTimeline::ZNoteTimeline(std::shared_ptr<ZJournal> journal, QByteArray fresh,
                             std::shared_ptr<const ZDocStyle> style)
    : journal_(std::move(journal)), fresh_(std::move(fresh)), style_(std::move(style)) {
    if (journal_ == nullptr) journal_ = std::make_shared<ZJournal>();
}

bool ZNoteTimeline::open(int index, QString* error) {
    if (!journal_->available()) {
        if (error != nullptr) *error = QStringLiteral("note has no journal");
        return false;
    }
    // Чтение рамок — второй триггер ленивой чистки: человек пошёл в прошлое,
    // и прошлое обязано быть уже чистым (дубликаты, которые всё равно уйдут при
    // первой правке, показывать незачем).
    if (!journal_->refresh(error)) return false;
    index_ = -1;
    resetSlots();
    const int at = index < 0 ? lastSnapshotIndex() : index;
    if (at < 0) {
        if (error != nullptr) *error = QStringLiteral("journal has no snapshots");
        return false;
    }
    return select(at, error);
}

int ZNoteTimeline::lastSnapshotIndex() const {
    // Формула головы одна на всю программу и живёт в журнале: хвост заметки,
    // таймлайн и возврат из архива обязаны согласиться, какая запись позже.
    return journal_->lastSnapshotIndex();
}

int ZNoteTimeline::previousSnapshotIndex(int from) const {
    return journal_->previousSnapshotIndex(from);
}

bool ZNoteTimeline::select(int index, QString* error) {
    if (index < 0 || index >= journal_->size()) return false;
    if (!journal_->entries()[index].hasSnapshot()) return false;
    QByteArray bytes;
    QString why;
    if (!journal_->snapshotAt(index, &bytes, &why)) {
        std::fprintf(stderr, "cannot rebuild snapshot: %s\n", why.toUtf8().constData());
        if (error != nullptr) *error = why;
        return false;
    }
    index_ = index;
    snapshotBytes_ = std::move(bytes);
    snapshotLines_ = diff::linesOf(
        std::string_view(snapshotBytes_.constData(), size_t(snapshotBytes_.size())));
    // Слепок другой — всё собранное относилось к прежнему.
    resetSlots();
    return true;
}

bool ZNoteTimeline::stepBack() {
    if (!isOpen()) return false;
    const int at = previousSnapshotIndex(index_);
    if (at < 0) return false;
    return select(at);
}

bool ZNoteTimeline::stepForward() {
    if (!isOpen()) return false;
    int at = index_ + 1;
    while (at < journal_->size() && !journal_->at(at).hasSnapshot()) ++at;
    if (at >= journal_->size()) return false;
    return select(at);
}

void ZNoteTimeline::setBase(Base base) { base_ = base; }

qint64 ZNoteTimeline::baseTime() const {
    if (!isOpen() || base_ == Base::Fresh) return 0;
    const int at = previousSnapshotIndex(index_);
    return at >= 0 ? journal_->at(at).time() : 0;
}

// --- разность ---------------------------------------------------------------

ZNoteTimeline::Slot& ZNoteTimeline::slot() {
    Slot& s = slots_[base_ == Base::Fresh ? 1 : 0];
    if (!s.ready && isOpen()) computeSlot(s, base_);
    return s;
}

void ZNoteTimeline::computeSlot(Slot& slot, Base base) {
    slot.lines.clear();
    slot.time = 0;
    if (base == Base::Fresh) {
        // Со свежей версией: она пришла с заметкой при открытии режима, файл
        // ради неё не читается.
        slot.lines = diff::linesOf(std::string_view(fresh_.constData(), size_t(fresh_.size())));
    } else {
        // С предыдущей записью; надгробия пропускаем — слепка у них нет. Базы
        // нет вовсе (первая запись) — сравниваем с пустотой: вся заметка
        // окажется добавленной, и это правда.
        const int at = previousSnapshotIndex(index_);
        if (at >= 0) {
            QByteArray bytes;
            QString why;
            if (journal_->snapshotAt(at, &bytes, &why)) {
                slot.time = journal_->at(at).time();
                slot.lines = diff::linesOf(std::string_view(bytes.constData(), size_t(bytes.size())));
            } else {
                std::fprintf(stderr, "cannot rebuild snapshot for comparison: %s\n",
                             why.toUtf8().constData());
            }
        }
    }
    slot.result = diff::compare(slot.lines, snapshotLines_);
    slot.doc.reset();
    slot.rowOfBlock.clear();
    slot.ready = true;
}

void ZNoteTimeline::ensureDocument(Slot& s) {
    if (s.doc.has_value()) return;
    s.doc = ZDocument::fromDiff(s.result, style_, &s.rowOfBlock);
}

void ZNoteTimeline::resetSlots() {
    for (Slot& s : slots_) s = Slot{};
    // Найденное держало курсоры в прежнем документе разности.
    search_.clear();
}

const diff::Result& ZNoteTimeline::result() { return slot().result; }

int ZNoteTimeline::changedLines() { return slot().result.changed; }

ZDocument& ZNoteTimeline::document() {
    Slot& s = slot();
    ensureDocument(s);
    return *s.doc;
}

int ZNoteTimeline::rowOfBlock(int block) {
    Slot& s = slot();
    ensureDocument(s);
    return block >= 0 && block < s.rowOfBlock.size() ? s.rowOfBlock[block] : -1;
}

int ZNoteTimeline::afterLineOfBlock(int block) {
    Slot& s = slot();
    ensureDocument(s);
    if (block < 0 || block >= s.rowOfBlock.size()) return -1;
    // У убранной строки стороны after нет — берём ближайшую строку слепка
    // снизу (следующую строку, которая в слепке есть): место всё равно то же.
    for (int b = block; b < s.rowOfBlock.size(); ++b) {
        const int row = s.rowOfBlock[b];
        if (row < 0 || row >= s.result.rows.size()) continue;
        if (s.result.rows[row].after >= 0) return s.result.rows[row].after;
    }
    return -1;
}

int ZNoteTimeline::blockOfAfterLine(int line) {
    if (line < 0) return -1;
    Slot& s = slot();
    ensureDocument(s);
    int best = -1;
    for (int b = 0; b < s.rowOfBlock.size(); ++b) {
        const int row = s.rowOfBlock[b];
        if (row < 0 || row >= s.result.rows.size()) continue;
        const int after = s.result.rows[row].after;
        if (after >= 0 && after <= line) best = b;
        if (after > line) break;
    }
    return best;
}

diff::Mark ZNoteTimeline::markOfBlock(int block) {
    const int mark = document().diffMarkAt(block);
    return mark < 0 ? diff::Mark::Same : diff::Mark(mark);
}

// --- слепок -----------------------------------------------------------------

std::string ZNoteTimeline::snapshotBody() const {
    return diff::bodyOf(std::string_view(snapshotBytes_.constData(), size_t(snapshotBytes_.size())));
}

qint64 ZNoteTimeline::snapshotTime() const {
    return isOpen() ? journal_->at(index_).time() : 0;
}

ZJournal::Kind ZNoteTimeline::snapshotKind() const {
    return isOpen() ? journal_->at(index_).kind() : ZJournal::Kind::Save;
}

// --- облик ------------------------------------------------------------------

void ZNoteTimeline::setStyle(std::shared_ptr<const ZDocStyle> style) {
    style_ = std::move(style);
    dropDocuments();
}

void ZNoteTimeline::dropDocuments() {
    for (Slot& s : slots_) {
        s.doc.reset();
        s.rowOfBlock.clear();
    }
    search_.clear();
}

}  // namespace zametti
