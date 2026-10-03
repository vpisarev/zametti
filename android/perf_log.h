// [perf] lines for logcat — the brief's measurements (§9) are read off these:
//
//   adb logcat -s zametti | grep '\[perf\]'
//
// One logging category for the whole phone shell: on Android the logcat TAG
// is the category name, and `-s zametti` filters exactly our lines. Plain
// qInfo() would land under the tag "default", next to everything else.
#ifndef ZAMETTI_ANDROID_PERF_LOG_H
#define ZAMETTI_ANDROID_PERF_LOG_H

#include <QByteArray>
#include <QLoggingCategory>

Q_DECLARE_LOGGING_CATEGORY(lcZametti)

namespace zametti {

// "[perf] <what> <ms> ms" — the same shape for every mark, so that one grep
// and one awk column give the table.
void perfLog(const QByteArray& what, qint64 ms);

}  // namespace zametti

#endif  // ZAMETTI_ANDROID_PERF_LOG_H
