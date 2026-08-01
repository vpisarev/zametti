#include "jxl_plugin.h"

#include "jxl_handler.h"

namespace zametti {

QImageIOPlugin::Capabilities JxlPlugin::capabilities(QIODevice* device,
                                                     const QByteArray& format) const {
    // По имени формата отвечаем и без устройства: так QImageReader выбирает
    // обработчик, когда формат задан явно (например, по расширению файла).
    if (format == "jxl") return Capabilities(CanRead);
    if (!format.isEmpty()) return {};
    if (!device || !device->isReadable()) return {};
    return JxlHandler::peek(device) ? Capabilities(CanRead) : Capabilities();
}

QImageIOHandler* JxlPlugin::create(QIODevice* device, const QByteArray& format) const {
    auto* handler = new JxlHandler;
    handler->setDevice(device);
    handler->setFormat(format.isEmpty() ? QByteArray("jxl") : format);
    return handler;
}

}  // namespace zametti
