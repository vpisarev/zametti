#include "document_saver.h"

#include "document_reader.h"
#include "json_dump.h"
#include "parser.h"
#include "serializer.h"

#include <QDateTime>
#include <QFile>
#include <QSaveFile>

namespace zametti {
namespace {

QByteArray toBytes(const std::string& text) {
    return QByteArray(text.data(), static_cast<qsizetype>(text.size()));
}

// Содержимое файла целиком. Заметки маленькие, и побайтовое сравнение и точнее
// хеша, и короче: не надо рассуждать о коллизиях. Читаем именно файл, а не
// помним последнюю запись, — тогда правка снаружи не приводит к «уже сохранено».
QByteArray fileContents(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return file.readAll();
}

bool writeFile(const QString& path, const QByteArray& data, QString* error) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error != nullptr) *error = file.errorString();
        return false;
    }
    const bool ok = file.write(data) == data.size() && file.flush();
    if (!ok && error != nullptr) *error = file.errorString();
    file.close();
    return ok;
}

}  // namespace

QString rescueTimestamp() {
    return QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
}

SaveOutcome saveDocument(const QTextDocument& doc, const QString& path,
                         const QString& timestamp, DocumentReaderFn reader) {
    const Document ir = reader ? reader(doc) : readDocument(doc);
    const QByteArray text = toBytes(serialize(ir));

    if (QFile::exists(path) && fileContents(path) == text)
        return {SaveResult::Unchanged, {}, {}};

    // Последний рубеж: то, что мы собрались записать, должно читаться обратно в
    // тот же документ. Сравнение по дампу — по всем полям, а не по тексту, и
    // расхождение сразу видно глазами.
    const std::string expected = toJson(ir);
    const std::string actual = toJson(parse(std::string(text.constData(),
                                                        static_cast<size_t>(text.size()))));
    if (expected != actual) {
        const QString rescuePath = path + QStringLiteral(".rescue-") + timestamp;
        QString error;
        if (!writeFile(rescuePath, text, &error)) {
            return {SaveResult::Failed,
                    QStringLiteral("самопроверка не прошла, и аварийный файл не записан: ") +
                        error,
                    {}};
        }
        return {SaveResult::Rescued,
                QStringLiteral("самопроверка перед записью не прошла: разобранное обратно "
                               "не совпало с документом. Файл не тронут, буфер сохранён в ") +
                    rescuePath,
                rescuePath};
    }

    // Замена файла целиком и разом: QSaveFile пишет во временный файл рядом и
    // переименовывает его на commit. Оборванная запись не оставит половину
    // заметки на месте целой.
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return {SaveResult::Failed,
                QStringLiteral("не открыть на запись: ") + file.errorString(), {}};
    }
    file.write(text);
    if (!file.commit()) {
        return {SaveResult::Failed, QStringLiteral("не записать: ") + file.errorString(), {}};
    }
    return {SaveResult::Written, {}, {}};
}

}  // namespace zametti
