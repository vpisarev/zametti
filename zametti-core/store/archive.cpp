#include "archive.h"

#include "journal.h"
#include "document.h"
#include "znote.h"
#include "serializer.h"

#include <QDateTime>
#include <QHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

#include <algorithm>
#include <string_view>
#include <utility>

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
            *error = QStringLiteral("cannot open for writing: %1").arg(file.errorString());
        return false;
    }
    file.write(bytes.data(), qint64(bytes.size()));
    if (file.commit()) return true;
    if (error != nullptr) *error = QStringLiteral("write failed: %1").arg(file.errorString());
    return false;
}

QString noteFile(const QString& root, const QString& noteId) {
    return QDir(root).filePath(noteId + QStringLiteral(".md"));
}

}  // namespace

bool isArchivedMeta(const NoteHeader& meta) {
    if (!meta.get(kArchivedKey).empty()) return true;
    // Старый вид: заметка-корзина до этапа 15. Читается как архивная, чтобы
    // хранилище, не прошедшее миграцию, не выглядело поломанным.
    return meta.get("role") == "trash";
}

void setArchivedMeta(NoteHeader& meta, bool archived) {
    meta.setPresent(true);
    if (archived) meta.set(kArchivedKey, kArchivedValue);
    else meta.unset(kArchivedKey);
}

// --- стаб ПО БАЙТАМ, без разбора ---------------------------------------------
//
// Правило владельца, записанное после того, как заметка с формулами оказалась
// испорчена: «пусть .md будет битый-перебитый, случайно или нарочно, — раз
// шапка на месте, программа обязана убрать его в архив, а разбирать дальше
// вообще не должна».
//
// Оно верное и не только про формулы. Архивация — операция НАД ФАЙЛОМ: тело
// целиком уезжает в журнал байт в байт, а на его месте остаётся шапка плюс
// строка заголовка. Ни первое, ни второе разбора не требует, а разбор — это
// лишняя точка отказа ровно там, где человек спасает то, что уже сломалось.
//
// Поэтому здесь ни одного вызова parse. Шапка берётся куском исходных байтов,
// пометка `archived: yes` дописывается строкой, заголовок ищется как первая
// содержательная строка после шапки.

// Границы шапки в байтах: [начало, конец) вместе с закрывающей строкой `-->`.
// Пусто — шапки нет.
std::pair<size_t, size_t> headerRange(std::string_view bytes) {
    static constexpr std::string_view kOpen = "<!-- zametti";
    if (bytes.substr(0, kOpen.size()) != kOpen) return {0, 0};
    const size_t close = bytes.find("\n-->");
    if (close == std::string_view::npos) return {0, 0};
    size_t end = close + 5;   // "\n-->" плюс перевод строки за ним
    if (end <= bytes.size() && bytes.substr(close, 5) != "\n-->\n") end = close + 4;
    return {0, std::min(end, bytes.size())};
}

// Есть ли в шапке пометка архива. Текстом, а не разбором: строка `archived:` со
// значением, либо старый `role: trash`.
bool headerSaysArchived(std::string_view header) {
    for (size_t at = 0; at < header.size();) {
        const size_t eol = std::min(header.find('\n', at), header.size());
        const std::string_view line = header.substr(at, eol - at);
        if (line.rfind("archived:", 0) == 0 && line.find_first_not_of(" \t\r", 9) != std::string_view::npos)
            return true;
        if (line.rfind("role:", 0) == 0 && line.find("trash") != std::string_view::npos) return true;
        at = eol + 1;
    }
    return false;
}

// Шапка с дописанной пометкой. Своя строка перед закрывающей `-->`.
std::string headerWithArchived(std::string_view header) {
    const size_t close = header.rfind("-->");
    if (close == std::string_view::npos) return std::string(header);
    std::string out(header.substr(0, close));
    out += "archived: yes\n";
    out += header.substr(close);
    return out;
}

// Шапка без пометки архива: строка `archived:` выбрасывается целиком, старый
// `role: trash` — тоже. Прочие ключи не трогаются ни один.
std::string headerWithoutArchived(std::string_view header) {
    std::string out;
    for (size_t at = 0; at < header.size();) {
        const size_t eol = std::min(header.find('\n', at), header.size());
        const std::string_view line = header.substr(at, eol - at);
        const bool drop = line.rfind("archived:", 0) == 0 ||
                          (line.rfind("role:", 0) == 0 && line.find("trash") != std::string_view::npos);
        if (!drop) {
            out += line;
            if (eol < header.size()) out += '\n';
        }
        at = eol + 1;
    }
    return out;
}

// Строка заголовка для стаба: первая содержательная строка тела. Уже заголовок
// — берём как есть, иначе делаем заголовком первого уровня. Ничего не нашли —
// стаб останется без тела, и это законно: заметка и была пустой.
std::string titleLine(std::string_view body) {
    for (size_t at = 0; at < body.size();) {
        const size_t eol = std::min(body.find('\n', at), body.size());
        std::string line(body.substr(at, eol - at));
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r'))
            line.pop_back();
        size_t from = 0;
        while (from < line.size() && (line[from] == ' ' || line[from] == '\t')) ++from;
        line = line.substr(from);
        at = eol + 1;
        if (line.empty()) continue;
        // Шапка из HTML-комментария в тело не входит, но заметка могла начаться
        // с чужого комментария — заголовком он не считается.
        if (line.rfind("<!--", 0) == 0) continue;
        if (line.rfind("#", 0) == 0) return line;
        return "# " + line;
    }
    return {};
}

std::string stubFromBytes(std::string_view bytes) {
    const auto [from, to] = headerRange(bytes);
    if (to == 0) return {};   // шапки нет — не наша заметка, трогать нечего
    const std::string_view header = bytes.substr(from, to - from);
    const std::string_view body = bytes.substr(to);

    std::string out = headerSaysArchived(header) ? std::string(header)
                                                 : headerWithArchived(header);
    const std::string title = titleLine(body);
    if (!title.empty()) {
        if (out.empty() || out.back() != '\n') out += '\n';
        out += '\n';
        out += title;
        out += '\n';
    }
    return out;
}

bool archiveNote(const QString& root, const QString& noteId, const history::Rules& rules,
                 QString* error) {
    const QString path = noteFile(root, noteId);
    std::string bytes;
    if (!readFileBytes(path, bytes)) {
        if (error != nullptr) *error = QStringLiteral("cannot read note %1").arg(noteId);
        return false;
    }
    // НИ ОДНОГО РАЗБОРА. Заметка могла быть испорчена чем угодно — правкой в
    // чужом редакторе, сбойным диском, нашей же ошибкой; раз шапка на месте,
    // убрать её в архив программа обязана. Тело уедет в журнал побайтово, стаб
    // соберётся из тех же байтов.
    const auto [headFrom, headTo] = headerRange(bytes);
    if (headTo == 0) {
        if (error != nullptr)
            *error = QStringLiteral("note %1 has no zametti header").arg(noteId);
        return false;
    }
    // ИДЕМПОТЕНТНОСТЬ. Повторная архивация — не ошибка: так выглядит второй
    // заход после падения между шагами. Уже помеченную заметку не трогаем
    // вовсе, иначе её стаб уехал бы в журнал поверх настоящего тела.
    if (headerSaysArchived(std::string_view(bytes).substr(headFrom, headTo - headFrom)))
        return true;

    // ШАГ ПЕРВЫЙ — ЖУРНАЛ. Тело уходит в историю тем же путём, что и живое
    // сохранение: правила отбора решают, ложится ли слепок отдельной записью,
    // заменяет ли последнюю или не пишется вовсе (равен голове).
    journal::History history(root);
    journal::ZJournal read;
    QString why;
    if (!history.read(noteId, &read, &why)) {
        if (error != nullptr) *error = QStringLiteral("cannot read history: %1").arg(why);
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
        history::decideStep(read, snapshotOf, snapshot, journal::Kind::Save, now, rules);
    if (step.keep < read.size() &&
        !history.truncate(noteId, qMax(1, step.keep), &why)) {
        if (error != nullptr) *error = QStringLiteral("journal not trimmed: %1").arg(why);
        return false;
    }
    if (step.writeNew &&
        !history.append(noteId, journal::Kind::Save, now, snapshot, 0, &why)) {
        // ТЕЛО НЕ ЗАПИСАНО — СТАБ НЕ ПИШЕМ. Это и есть инвариант A: потерять
        // тело нельзя, потому что мы не начинаем второй шаг, не сделав первый.
        if (error != nullptr) *error = QStringLiteral("body not written to history: %1").arg(why);
        return false;
    }

    // ШАГ ВТОРОЙ — СТАБ.
    if (!writeFileBytes(path, stubFromBytes(bytes), error)) return false;
    return true;
}

bool restoreNote(const QString& root, const QString& noteId, QString* error) {
    const QString path = noteFile(root, noteId);
    std::string bytes;
    if (!readFileBytes(path, bytes)) {
        if (error != nullptr) *error = QStringLiteral("cannot read note %1").arg(noteId);
        return false;
    }
    ZNote stub;
    stub.load(bytes);
    if (!stub.isArchived()) return true;   // уже дома

    journal::History history(root);
    journal::ZJournal read;
    QString why;
    if (!history.read(noteId, &read, &why)) {
        if (error != nullptr) *error = QStringLiteral("cannot read history: %1").arg(why);
        return false;
    }
    const int head = read.lastSnapshotIndex();
    QByteArray body;
    if (head >= 0 && !history.snapshotAt(noteId, head, &body, &why)) body.clear();
    if (body.isEmpty()) {
        // Тела нет — оставляем стаб как есть и говорим вслух. Молча отдать
        // человеку одну строку вместо заметки нельзя ничем.
        if (error != nullptr)
            *error = QStringLiteral("history of %1 has no body — the note remains a stub: %2")
                         .arg(noteId, why);
        return false;
    }

    // ТЕЛО — БАЙТ В БАЙТ ИЗ ЖУРНАЛА, ШАПКА — ИЗ СТАБА. Ни одного разбора, по той
    // же причине, что и при архивации: заметка могла быть испорчена чем угодно,
    // и вернуть её человек имеет право в любом случае. В шапке стаба живут
    // parent, sort и всё, что человек мог поменять, пока заметка лежала в
    // архиве, — поэтому берётся она, а не журнальная; из тела журнальная шапка
    // просто отрезается.
    const std::string bodyBytes(body.constData(), size_t(body.size()));
    const auto [stubFrom, stubTo] = headerRange(bytes);
    const auto [bodyFrom, bodyTo] = headerRange(bodyBytes);
    std::string header = stubTo > 0 ? bytes.substr(stubFrom, stubTo - stubFrom)
                                    : bodyBytes.substr(bodyFrom, bodyTo - bodyFrom);
    header = headerWithoutArchived(header);
    std::string out = header;
    out += bodyBytes.substr(bodyTo);
    if (!writeFileBytes(path, out, error)) return false;

    // ВЕШКА В ИСТОРИИ. Таймлайн отвечает на вопрос «что с заметкой было», и
    // «вернули из архива» — такой же ответ, как «правили» или «удалили».
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QString ignored;
    history.append(noteId, journal::Kind::Restore, now,
                   QByteArray(out.data(), qsizetype(out.size())), 0, &ignored);
    return true;
}

int migrateTrashToArchive(const QString& root, QString* error) {
    QString trashId;
    QHash<QString, QString> parents;   // id → parent, по всему хранилищу
    QHash<QString, std::shared_ptr<ZNote>> docs;
    for (const QFileInfo& info : QDir(root).entryInfoList({QStringLiteral("*.md")}, QDir::Files)) {
        std::string bytes;
        if (!readFileBytes(info.absoluteFilePath(), bytes)) continue;
        auto doc = std::make_shared<ZNote>();
        doc->load(bytes);
        const QString id = info.completeBaseName();
        parents.insert(id, doc->parentId());
        if (doc->headerValue(QStringLiteral("role")) == QLatin1String("trash")) trashId = id;
        docs.insert(id, doc);
    }
    if (trashId.isEmpty()) return 0;

    int moved = 0;
    for (auto it = docs.begin(); it != docs.end(); ++it) {
        if (it.key() == trashId) continue;
        if (parents.value(it.key()) != trashId) continue;
        ZNote& doc = *it.value();
        // Пустое значение снимает ключ — и «домой в корень» выражается ровно им.
        doc.setParentId(doc.headerValue(QStringLiteral("trash-parent")));
        doc.setHeaderValue(QStringLiteral("trash-parent"), QString());
        doc.setHeaderValue(QStringLiteral("trash-path"), QString());
        doc.setArchived(true);
        if (!writeFileBytes(noteFile(root, it.key()), doc.toMarkdown(), error)) return -1;
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
        if (error != nullptr) *error = QStringLiteral("cannot delete note file %1").arg(noteId);
        return false;
    }

    // ЖУРНАЛ УХОДИТ ВМЕСТЕ С ЗАМЕТКОЙ — и только здесь. Обычное удаление
    // журнал бережёт (по нему заметку можно воскресить), но у архивной тело
    // живёт в журнале и больше нигде: оставить его значило бы оставить и саму
    // заметку, а человек попросил забыть её насовсем.
    const QString log = journal::History(root).pathFor(noteId);
    if (QFile::exists(log) && !QFile::moveToTrash(log) && !QFile::remove(log)) {
        if (error != nullptr) *error = QStringLiteral("cannot delete journal %1").arg(noteId);
        return false;
    }
    if (!hadFile && error != nullptr)
        *error = QStringLiteral("note file %1 did not exist").arg(noteId);
    return true;
}

}  // namespace zametti::store
