#include "archive.h"

#include "journal.h"
#include "parser.h"
#include "serializer.h"

#include <QDateTime>
#include <QHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

namespace zametti::store {
namespace {

bool readFileBytes(const QString& path, std::string& out) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    const QByteArray bytes = f.readAll();
    out.assign(bytes.constData(), size_t(bytes.size()));
    return true;
}

// Запись файла заметки целиком и разом. QSaveFile пишет во временный файл рядом
// и переименовывает его поверх — на месте старого файла либо прежние байты,
// либо новые, и никогда половина. Тем же способом пишет и редактор.
bool writeFileBytes(const QString& path, const std::string& bytes, QString* error) {
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

QString noteFile(const QString& root, const QString& noteId) {
    return QDir(root).filePath(noteId + QStringLiteral(".md"));
}

}  // namespace

bool isArchivedMeta(const NoteMeta& meta) {
    if (!meta.get(kArchivedKey).empty()) return true;
    // Старый вид: заметка-корзина до этапа 15. Читается как архивная, чтобы
    // хранилище, не прошедшее миграцию, не выглядело поломанным.
    return meta.get("role") == "trash";
}

void setArchivedMeta(NoteMeta& meta, bool archived) {
    meta.present = true;
    if (archived) meta.set(kArchivedKey, kArchivedValue);
    else meta.unset(kArchivedKey);
}

std::string stubBytes(const Document& doc) {
    Document stub;
    stub.meta = doc.meta;
    setArchivedMeta(stub.meta, true);

    // Заголовок ищем так же, как его видит средняя колонка: первый
    // содержательный блок. Не нашли — стаб остаётся без тела, и это законно:
    // заметка без единой строки текста и была пустой.
    for (const Block& b : doc.blocks) {
        if (b.raw) {
            if (doc.isClosedHtmlComment(b)) continue;
            break;   // дословный кусок заголовком не считаем
        }
        if (b.kind == Kind::VSpace || b.kind == Kind::Html) continue;
        const std::string_view text = doc.text(b);
        if (text.empty()) continue;
        // Первая строка: заголовок стаба однострочный, а блок может нести
        // мягкие переносы.
        std::string line(text.substr(0, text.find('\n')));
        while (!line.empty() && (line.back() == ' ' || line.back() == '\r')) line.pop_back();
        if (line.empty()) continue;
        Block heading = stub.newBlock(Kind::Heading, line);
        heading.headingLevel = b.kind == Kind::Heading && b.headingLevel > 0 ? b.headingLevel : 1;
        stub.blocks.push_back(heading);
        break;
    }
    stub.meta.blankAfter = !stub.blocks.empty();
    return serialize(stub);
}

bool archiveNote(const QString& root, const QString& noteId, const history::Rules& rules,
                 QString* error) {
    const QString path = noteFile(root, noteId);
    std::string bytes;
    if (!readFileBytes(path, bytes)) {
        if (error != nullptr) *error = QStringLiteral("заметка %1 не читается").arg(noteId);
        return false;
    }
    Document doc = parse(bytes);
    // ИДЕМПОТЕНТНОСТЬ. Повторная архивация — не ошибка: так выглядит второй
    // заход после падения между шагами. Уже помеченную заметку не трогаем
    // вовсе, иначе её стаб уехал бы в журнал поверх настоящего тела.
    if (isArchivedMeta(doc.meta)) return true;

    // ШАГ ПЕРВЫЙ — ЖУРНАЛ. Тело уходит в историю тем же путём, что и живое
    // сохранение: правила отбора решают, ложится ли слепок отдельной записью,
    // заменяет ли последнюю или не пишется вовсе (равен голове).
    journal::History history(root);
    journal::Journal read;
    QString why;
    if (!history.read(noteId, &read, &why)) {
        if (error != nullptr) *error = QStringLiteral("история не читается: %1").arg(why);
        return false;
    }
    const QByteArray snapshot(bytes.data(), qsizetype(bytes.size()));
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const auto snapshotOf = [&](int i) {
        QByteArray older;
        QString ignored;
        if (!history.snapshotAt(noteId, i, &older, &ignored)) return QByteArray();
        return older;
    };
    const history::Step step =
        history::decideStep(read.entries, snapshotOf, snapshot, journal::Kind::Save, now, rules);
    if (step.keep < int(read.entries.size()) &&
        !history.truncate(noteId, qMax(1, step.keep), &why)) {
        if (error != nullptr) *error = QStringLiteral("журнал не подрезан: %1").arg(why);
        return false;
    }
    if (step.writeNew &&
        !history.append(noteId, journal::Kind::Save, now, snapshot, 0, &why)) {
        // ТЕЛО НЕ ЗАПИСАНО — СТАБ НЕ ПИШЕМ. Это и есть инвариант A: потерять
        // тело нельзя, потому что мы не начинаем второй шаг, не сделав первый.
        if (error != nullptr) *error = QStringLiteral("тело не записано в историю: %1").arg(why);
        return false;
    }

    // ШАГ ВТОРОЙ — СТАБ.
    if (!writeFileBytes(path, stubBytes(doc), error)) return false;
    return true;
}

bool restoreNote(const QString& root, const QString& noteId, QString* error) {
    const QString path = noteFile(root, noteId);
    std::string bytes;
    if (!readFileBytes(path, bytes)) {
        if (error != nullptr) *error = QStringLiteral("заметка %1 не читается").arg(noteId);
        return false;
    }
    Document stub = parse(bytes);
    if (!isArchivedMeta(stub.meta)) return true;   // уже дома

    journal::History history(root);
    journal::Journal read;
    QString why;
    if (!history.read(noteId, &read, &why)) {
        if (error != nullptr) *error = QStringLiteral("история не читается: %1").arg(why);
        return false;
    }
    int head = int(read.entries.size()) - 1;
    while (head >= 0 && !read.entries[head].hasSnapshot()) --head;
    QByteArray body;
    if (head >= 0 && !history.snapshotAt(noteId, head, &body, &why)) body.clear();
    if (body.isEmpty()) {
        // Тела нет — оставляем стаб как есть и говорим вслух. Молча отдать
        // человеку одну строку вместо заметки нельзя ничем.
        if (error != nullptr)
            *error = QStringLiteral("в истории %1 нет тела — заметка осталась стабом: %2")
                         .arg(noteId, why);
        return false;
    }

    // Тело берётся из журнала, а ШАПКА — из стаба: в ней живут parent, sort и
    // прочее, что человек мог поменять, пока заметка лежала в архиве. Из
    // журнальной шапки не берём ничего, кроме собственно тела.
    Document restored = parse(std::string(body.constData(), size_t(body.size())));
    restored.meta = stub.meta;
    setArchivedMeta(restored.meta, false);
    // Старый вид архивности (`role: trash`) снимаем заодно: иначе заметка
    // вернулась бы домой и тут же уехала обратно в Архив.
    if (restored.meta.get("role") == "trash") restored.meta.unset("role");
    restored.meta.blankAfter = !restored.blocks.empty();
    if (!writeFileBytes(path, serialize(restored), error)) return false;

    // Возврат — вешка в истории, а не тихая подмена файла: журнал отвечает на
    // вопрос «что с заметкой было», и «вернули из архива» такой же ответ, как
    // «правили» или «удалили».
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QString ignored;
    history.append(noteId, journal::Kind::Restore, now, body,
                   head >= 0 ? read.entries[head].time : 0, &ignored);
    return true;
}

int migrateTrashToArchive(const QString& root, QString* error) {
    QString trashId;
    QHash<QString, QString> parents;   // id → parent, по всему хранилищу
    QHash<QString, Document> docs;
    for (const QFileInfo& info : QDir(root).entryInfoList({QStringLiteral("*.md")}, QDir::Files)) {
        std::string bytes;
        if (!readFileBytes(info.absoluteFilePath(), bytes)) continue;
        Document doc = parse(bytes);
        const QString id = info.completeBaseName();
        parents.insert(id, QString::fromStdString(doc.meta.get("parent")));
        if (doc.meta.get("role") == "trash") trashId = id;
        docs.insert(id, std::move(doc));
    }
    if (trashId.isEmpty()) return 0;

    int moved = 0;
    for (auto it = docs.begin(); it != docs.end(); ++it) {
        if (it.key() == trashId) continue;
        if (parents.value(it.key()) != trashId) continue;
        Document& doc = it.value();
        const std::string home = doc.meta.get("trash-parent");
        if (home.empty()) doc.meta.unset("parent");
        else doc.meta.set("parent", home);
        doc.meta.unset("trash-parent");
        doc.meta.unset("trash-path");
        setArchivedMeta(doc.meta, true);
        if (!writeFileBytes(noteFile(root, it.key()), serialize(doc), error)) return -1;
        ++moved;
    }

    // Опустевшая корзина уходит вместе со своим журналом: заметкой она не была
    // никогда, и место в Архиве ей ни к чему.
    QString ignored;
    forgetNote(root, trashId, &ignored);
    return moved;
}

bool forgetNote(const QString& root, const QString& noteId, QString* error) {
    const QString path = noteFile(root, noteId);
    const bool hadFile = QFile::exists(path);
    if (hadFile && !QFile::moveToTrash(path) && !QFile::remove(path)) {
        if (error != nullptr) *error = QStringLiteral("файл заметки %1 не удалить").arg(noteId);
        return false;
    }

    // ЖУРНАЛ УХОДИТ ВМЕСТЕ С ЗАМЕТКОЙ — и только здесь. Обычное удаление
    // журнал бережёт (по нему заметку можно воскресить), но у архивной тело
    // живёт в журнале и больше нигде: оставить его значило бы оставить и саму
    // заметку, а человек попросил забыть её насовсем.
    const QString log = journal::History(root).pathFor(noteId);
    if (QFile::exists(log) && !QFile::moveToTrash(log) && !QFile::remove(log)) {
        if (error != nullptr) *error = QStringLiteral("журнал %1 не удалить").arg(noteId);
        return false;
    }
    if (!hadFile && error != nullptr)
        *error = QStringLiteral("файла заметки %1 не было").arg(noteId);
    return true;
}

}  // namespace zametti::store
