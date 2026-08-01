// Оболочка плагина: по ней Qt находит наш обработчик JXL.
//
// Плагин СТАТИЧЕСКИЙ и вкомпилирован в программу, а не лежит отдельным .so.
// Так и задумано: свой декод нужен именно затем, чтобы не зависеть от того,
// что установлено в системе. Динамический плагин пришлось бы куда-то класть и
// надеяться, что Qt его найдёт.

#pragma once

#include <QImageIOPlugin>

namespace zametti {

class JxlPlugin : public QImageIOPlugin {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "org.qt-project.Qt.QImageIOHandlerFactoryInterface" FILE "jxl.json")

public:
    Capabilities capabilities(QIODevice* device, const QByteArray& format) const override;
    QImageIOHandler* create(QIODevice* device, const QByteArray& format) const override;
};

}  // namespace zametti
