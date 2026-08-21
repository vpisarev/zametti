#include "config_file.h"

#include "settings.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSaveFile>

namespace zametti {

ZConfigFile::ZConfigFile(QString path) : path_(path.isEmpty() ? configPath() : std::move(path)) {}

bool ZConfigFile::load(QString* error) {
    if (!QFile::exists(path_)) {
        QDir().mkpath(QFileInfo(path_).absolutePath());
        QSaveFile fresh(path_);
        const QByteArray body = configTemplate();
        if (!fresh.open(QIODevice::WriteOnly) || fresh.write(body) != body.size() || !fresh.commit()) {
            if (error != nullptr)
                *error = QStringLiteral("cannot write config template: %1 (%2)")
                             .arg(path_, fresh.errorString());
            return false;
        }
    }
    QFile file(path_);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error != nullptr)
            *error = QStringLiteral("cannot read config: %1 (%2)").arg(path_, file.errorString());
        return false;
    }
    onDisk_ = file.readAll();
    text_ = QString::fromUtf8(onDisk_);
    return true;
}

bool ZConfigFile::dirty() const { return text_.toUtf8() != onDisk_; }

bool ZConfigFile::save(QString* error) {
    const QByteArray bytes = text_.toUtf8();
    QDir().mkpath(QFileInfo(path_).absolutePath());
    QSaveFile file(path_);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        if (error != nullptr)
            *error = QStringLiteral("cannot write config: %1 (%2)").arg(path_, file.errorString());
        return false;
    }
    onDisk_ = bytes;
    return true;
}

ZConfigFile::Check ZConfigFile::check(const QString& text) {
    Check out;
    // Тот же путь, что у загрузчика: стриппер хранит переводы строк, поэтому
    // смещение ошибки в срезанном тексте указывает на ту же строку файла.
    const QByteArray stripped = stripJsonSugar(text.toUtf8());
    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(stripped, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        out.ok = false;
        out.error = parseError.error != QJsonParseError::NoError
                        ? parseError.errorString()
                        : QStringLiteral("config must be a JSON object");
        const int offset = qBound(0, parseError.offset, int(stripped.size()));
        out.line = 1;
        int lineStart = 0;
        for (int i = 0; i < offset; ++i) {
            if (stripped.at(i) == '\n') {
                ++out.line;
                lineStart = i + 1;
            }
        }
        // Колонка — в знаках UTF-8-байтов до ошибки в этой строке, переведённых
        // в знаки: байты строки до смещения разбираем как UTF-8.
        out.column = QString::fromUtf8(stripped.mid(lineStart, offset - lineStart)).size() + 1;
        return out;
    }
    out.unknownKeys = unknownConfigKeys(doc.object());
    return out;
}

}  // namespace zametti
