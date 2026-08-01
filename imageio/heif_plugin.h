// Оболочка плагина для AVIF и HEIC. Статический, как и JXL-овый: см.
// imageio/CMakeLists.txt о том, почему не динамический.

#pragma once

#include <QImageIOPlugin>

namespace zametti {

class HeifPlugin : public QImageIOPlugin {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "org.qt-project.Qt.QImageIOHandlerFactoryInterface" FILE "heif.json")

public:
    Capabilities capabilities(QIODevice* device, const QByteArray& format) const override;
    QImageIOHandler* create(QIODevice* device, const QByteArray& format) const override;
};

}  // namespace zametti
