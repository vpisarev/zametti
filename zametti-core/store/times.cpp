#include "times.h"

#include <QTime>

namespace zametti::store {

QString isoWithOffset(const QDateTime& when) {
    if (!when.isValid()) return {};
    QDateTime moment = when;
    // Доли секунды прочь: в шапке они шум, а «21:40:00.500» и «21:40:00»
    // одного момента дали бы разные строки при каждом сохранении.
    moment.setTime(QTime(moment.time().hour(), moment.time().minute(), moment.time().second()));
    // Приведение к собственному офсету — ради ПЕЧАТИ. У местного времени spec =
    // LocalTime, и Qt печатает его вовсе без зоны; офсет появляется только у
    // OffsetFromUTC и TimeZone. Момент от приведения не меняется.
    return moment.toOffsetFromUtc(moment.offsetFromUtc()).toString(Qt::ISODate);
}

QString isoNow() { return isoWithOffset(QDateTime::currentDateTime()); }

QDateTime parseNoteTime(const QString& text) {
    // Qt сама разбирает все три вида: «…Z» — UTC, «…+02:00» — офсет, без зоны —
    // местное время читающей машины (проверено пробником). Нам остаётся не
    // мешать ей своим форматом.
    return QDateTime::fromString(text.trimmed(), Qt::ISODate);
}

QDateTime parseNoteTime(const std::string& text) {
    return parseNoteTime(QString::fromStdString(text));
}

QString comparableTime(const QString& text) {
    const QDateTime moment = parseNoteTime(text);
    if (!moment.isValid()) return {};
    return moment.toUTC().toString(Qt::ISODate);
}

QString comparableTime(const std::string& text) {
    return comparableTime(QString::fromStdString(text));
}

}  // namespace zametti::store
