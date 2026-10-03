#include "perf_log.h"

Q_LOGGING_CATEGORY(lcZametti, "zametti")

namespace zametti {

void perfLog(const QByteArray& what, qint64 ms) {
    qCInfo(lcZametti).noquote() << QStringLiteral("[perf] %1 %2 ms")
                                       .arg(QString::fromUtf8(what))
                                       .arg(ms);
}

}  // namespace zametti
