#include "znote.h"

#include <QFileInfo>

namespace zametti {

ZNote::ZNote(QString path, QByteArray fileBytes, Digest digest, ZNoteHistory history)
    : path_(std::move(path)),
      digest_(digest),
      lastSaved_(std::move(fileBytes)),
      history_(std::move(history)) {}

QString ZNote::id() const { return QFileInfo(path_).completeBaseName(); }

ZDocument ZNote::replaceDoc(ZDocument fresh) {
    ZDocument previous = doc_;
    doc_ = std::move(fresh);
    // Другой документ — всё производное от прежнего вслух устарело: ревизия
    // у нового может совпасть случайно.
    built_.invalidate();
    stats_.invalidate();
    search_.clear();
    return previous;
}

NoteHeader ZNote::takeLostMeta() {
    NoteHeader lost = lostMeta_;
    lostMeta_ = NoteHeader();
    return lost;
}

void ZNote::markWritten(const Digest& digest, QByteArray written) {
    digest_ = digest;
    lastSaved_ = std::move(written);
}

bool ZNote::setSelfCheckFailed(bool failed) {
    if (failed == selfCheckFailed_) return false;
    selfCheckFailed_ = failed;
    return true;
}


}  // namespace zametti
