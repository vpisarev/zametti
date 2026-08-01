#include "heif_plugin.h"

#include "heif_handler.h"

namespace zametti {

namespace {
// Один обработчик на четыре имени: внутри у них общий контейнер, и libheif
// различает содержимое сама.
bool knownName(const QByteArray& format) {
    return format == "avif" || format == "heic" || format == "heif" || format == "avifs";
}
}  // namespace

QImageIOPlugin::Capabilities HeifPlugin::capabilities(QIODevice* device,
                                                      const QByteArray& format) const {
    if (knownName(format)) return Capabilities(CanRead);
    if (!format.isEmpty()) return {};
    if (!device || !device->isReadable()) return {};
    return HeifHandler::peek(device) ? Capabilities(CanRead) : Capabilities();
}

QImageIOHandler* HeifPlugin::create(QIODevice* device, const QByteArray& format) const {
    auto* handler = new HeifHandler;
    handler->setDevice(device);
    handler->setFormat(knownName(format) ? format : QByteArray("heif"));
    return handler;
}

}  // namespace zametti
