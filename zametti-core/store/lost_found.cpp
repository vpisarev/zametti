#include "lost_found.h"

#include "note_id.h"
#include "parser.h"
#include "serializer.h"
#include "store.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSaveFile>

namespace zametti::store {
namespace {

bool readBytes(const QString& path, std::string& out) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    const QByteArray bytes = f.readAll();
    out.assign(bytes.constData(), size_t(bytes.size()));
    return true;
}

bool writeBytes(const QString& path, const std::string& bytes, QString* error) {
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error != nullptr)
            *error = QStringLiteral("не открыть на запись: %1").arg(file.errorString());
        return false;
    }
    file.write(bytes.data(), qint64(bytes.size()));
    if (file.commit()) return true;
    if (error != nullptr) *error = QStringLiteral("запись не удалась: %1").arg(file.errorString());
    return false;
}

}  // namespace

int fileOrphans(const QString& root, QString* error) {
    // Один проход по каталогу: что за заметки есть и на кого они ссылаются.
    QHash<QString, QString> parents;
    QHash<QString, Document> docs;
    QString bureau;
    for (const QFileInfo& info : QDir(root).entryInfoList({QStringLiteral("*.md")}, QDir::Files)) {
        const QString id = info.completeBaseName();
        if (!isValidNoteId(id.toStdString())) continue;
        std::string bytes;
        if (!readBytes(info.absoluteFilePath(), bytes)) continue;
        Document doc = parse(bytes);
        parents.insert(id, QString::fromStdString(doc.meta.get("parent")));
        if (doc.meta.get("role") == kLostRole) bureau = id;
        docs.insert(id, std::move(doc));
    }

    // Сироты: parent стоит, а заметки с таким id нет. Сама папка-бюро сиротой
    // быть не может — она в корне.
    QStringList lost;
    for (auto it = parents.constBegin(); it != parents.constEnd(); ++it) {
        if (it.value().isEmpty()) continue;
        if (docs.contains(it.value())) continue;
        lost << it.key();
    }
    if (lost.isEmpty()) return 0;
    lost.sort();   // порядок обхода каталога не определён, а поведение обязано быть

    // Бюро заводится ТОЛЬКО когда есть первая находка: пустой спецпапки в
    // дереве человек не заказывал.
    if (bureau.isEmpty()) {
        QString why;
        const QString made = newNote(root, QString(), &why);
        if (made.isEmpty()) {
            if (error != nullptr) *error = why;
            return -1;
        }
        bureau = QFileInfo(made).completeBaseName();
        std::string bytes;
        if (!readBytes(made, bytes)) {
            if (error != nullptr) *error = QStringLiteral("папка бюро не читается");
            return -1;
        }
        Document doc = parse(bytes);
        doc.meta.present = true;
        doc.meta.set("role", kLostRole);
        Block heading = doc.newBlock(Kind::Heading, "Бюро находок");
        heading.headingLevel = 1;
        doc.blocks.push_back(heading);
        doc.meta.blankAfter = true;
        if (!writeBytes(made, serialize(doc), error)) return -1;
    }

    int filed = 0;
    for (const QString& id : lost) {
        Document& doc = docs[id];
        // ДВА ПОРТА: текущий — бюро, оригинал — то, что было. По второму видно,
        // откуда заметка пришла, и он же переживёт приезд настоящего родителя
        // синхронизацией.
        const std::string was = doc.meta.get("parent");
        if (!was.empty() && doc.meta.get(kLostParentKey).empty())
            doc.meta.set(kLostParentKey, was);
        doc.meta.set("parent", bureau.toStdString());
        // `modified` НЕ трогаем: правка организационная, как перенос. Здесь это
        // держится тем, что мы пишем ровно те байты, что прочитали, поменяв
        // две строки шапки, — штампов в этом пути нет вовсе.
        if (!writeBytes(QDir(root).filePath(id + QStringLiteral(".md")), serialize(doc), error))
            return -1;
        ++filed;
    }
    return filed;
}

}  // namespace zametti::store
