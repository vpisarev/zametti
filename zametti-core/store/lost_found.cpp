// ZStorage::fileOrphans — бюро находок: куда попадает заметка с оборванным
// родителем.
//
// Раньше такая заметка просто показывалась в корне с пометкой «сирота» — то
// есть чинилась В ПАМЯТИ, при каждой сборке дерева заново. Теперь она
// прописывается в спецпапку, и это одно из САНКЦИОНИРОВАННЫХ ИСКЛЮЧЕНИЙ из
// правила «загрузка ничего не пишет» (решение владельца; список — у
// ZStorage::migrate). Рамки исключения жёсткие, и они же — предмет проверок:
//
//   * пишем ТОЛЬКО заметкам с неразрешимым parent;
//   * запись двухпортовая: текущий порт (`parent`) := бюро, оригинал
//     (`lost-parent`) := что было;
//   * идемпотентно: заметка, уже лежащая в бюро, не трогается вовсе;
//   * `modified` НЕ поднимается — правка организационная, как перенос;
//   * само бюро заводится, только когда есть первая находка.
//
// Коллизии id между хранилищами объявлены владельцем пренебрежимыми: id — это
// 8 знаков времени и 6 случайных, и совпадение означало бы, что две заметки
// заведены в одну секунду на двух машинах, да ещё с одинаковым броском CSPRNG.

#include "zstorage.h"

#include "note_id.h"
#include "znote.h"

#include <QDir>
#include <QFileInfo>
#include <QHash>

namespace zametti {

int ZStorage::fileOrphans(QString* error) {
    // Один проход по каталогу: что за заметки есть и на кого они ссылаются.
    QHash<QString, QString> parents;
    QHash<QString, std::shared_ptr<ZNote>> docs;
    QString bureau;
    for (const QFileInfo& info : QDir(root_).entryInfoList({QStringLiteral("*.md")}, QDir::Files)) {
        const QString id = info.completeBaseName();
        if (!isValidNoteId(id.toStdString())) continue;
        std::string bytes;
        if (!readFileBytes(info.absoluteFilePath(), bytes)) continue;
        auto doc = std::make_shared<ZNote>();
        doc->load(bytes);
        parents.insert(id, doc->parentId());
        if (doc->isLost()) bureau = id;
        docs.insert(id, doc);
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
        const QString made = newNoteFile(QString(), &why);
        if (made.isEmpty()) {
            if (error != nullptr) *error = why;
            return -1;
        }
        bureau = QFileInfo(made).completeBaseName();
        std::string bytes;
        if (!readFileBytes(made, bytes)) {
            if (error != nullptr) *error = QStringLiteral("cannot read the lost & found folder");
            return -1;
        }
        ZNote doc;
        doc.load("# Lost & found\n");
        NoteHeader head;
        {
            ZNote was;
            was.load(bytes);
            head = was.header();
        }
        head.setPresent(true);
        head.ensureVersion();
        head.set("role", kLostRole);
        head.setBlankAfter(true);
        doc.setHeader(head);
        if (!writeFileBytes(made, doc.toMarkdown(), error)) return -1;
    }

    int filed = 0;
    for (const QString& id : lost) {
        ZNote& doc = *docs[id];
        // ДВА ПОРТА: текущий — бюро, оригинал — то, что было. По второму видно,
        // откуда заметка пришла, и он же переживёт приезд настоящего родителя
        // синхронизацией.
        const QString was = doc.parentId();
        if (!was.isEmpty() && doc.headerValue(QString::fromLatin1(kLostParentKey)).isEmpty())
            doc.setHeaderValue(QString::fromLatin1(kLostParentKey), was);
        doc.setParentId(bureau);
        // `modified` НЕ трогаем: правка организационная, как перенос. Здесь это
        // держится тем, что мы пишем ровно те байты, что прочитали, поменяв
        // две строки шапки, — штампов в этом пути нет вовсе.
        if (!writeFileBytes(pathOf(id), doc.toMarkdown(), error)) return -1;
        ++filed;
    }
    return filed;
}

}  // namespace zametti
