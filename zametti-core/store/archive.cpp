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



bool archiveNote(const QString& root, const QString& noteId, const history::Rules& rules,
                 QString* error) {
    const QString path = noteFile(root, noteId);
    std::string bytes;
    if (!readFileBytes(path, bytes)) {
        if (error != nullptr) *error = QStringLiteral("cannot read note %1").arg(noteId);
        return false;
    }
    // НИ ОДНОГО РАЗБОРА, и это правило владельца, а не экономия: заметка могла
    // быть испорчена чем угодно — правкой в чужом редакторе, сбойным диском,
    // нашей же ошибкой; раз шапка на месте, убрать её в архив программа
    // обязана. Отсюда же запрет сводить архивацию к ZStorage::rewriteNote: тот
    // заметку разбирает.
    const auto [headFrom, headTo] = headerRange(bytes);
    if (headTo == 0) {
        if (error != nullptr)
            *error = QStringLiteral("note %1 has no zametti header").arg(noteId);
        return false;
    }
    // ИДЕМПОТЕНТНОСТЬ. Повторная архивация — не ошибка: так выглядит второй
    // заход после падения между шагами. Уже помеченную заметку не трогаем
    // вовсе, чтобы не заводить в журнале запись, ничего не меняющую.
    if (headerSaysArchived(std::string_view(bytes).substr(headFrom, headTo - headFrom)))
        return true;

    // АРХИВ — ЭТО ОДНА СТРОКА В ШАПКЕ. Тело остаётся в файле: вместе с ним
    // остаются и ссылки на вложения, по которым считается, чему уходить при
    // удалении насовсем, и текст, который находит поиск по хранилищу.
    std::string out = headerWithArchived(std::string_view(bytes).substr(headFrom, headTo - headFrom));
    out += bytes.substr(headTo);
    if (!writeFileBytes(path, out, error)) return false;

    // ЖУРНАЛ ПОСЛЕ ФАЙЛА, а не до. Прежний порядок (сперва журнал) держал
    // инвариант «тело не должно исчезнуть отовсюду» — теперь тело никуда не
    // девается, зато появляется другое требование: не объявлять заметку
    // архивной раньше, чем она такой стала.
    //
    // Запись ложится по общим правилам отбора: пометка мелкая, значит гасит
    // прошлую запись, а не встаёт рядом. Голова журнала обязана сойтись с
    // файлом — на этом стоит вся синхронизация.
    journal::History history(root);
    journal::ZJournal read;
    QString why;
    if (!history.read(noteId, &read, &why)) {
        if (error != nullptr) *error = QStringLiteral("cannot read history: %1").arg(why);
        return true;   // файл уже помечен: архивация состоялась
    }
    const QByteArray snapshot(out.data(), qsizetype(out.size()));
    const auto snapshotOf = [&](int at) {
        QByteArray older;
        QString ignored;
        if (!history.snapshotAt(noteId, at, &older, &ignored)) return QByteArray();
        return older;
    };
    const history::Step step = history::decideStep(read, snapshotOf, snapshot, journal::Kind::Save,
                                                   QDateTime::currentMSecsSinceEpoch(), rules);
    QVector<journal::EntryRef> voids;
    voids.reserve(step.voided.size());
    for (int at : step.voided)
        voids.append(journal::EntryRef(read.at(at).time(), read.at(at).digest()));
    if (step.writeNew &&
        !history.append(noteId, journal::NewRecord::save(snapshot).voiding(voids), &why)) {
        // Файл уже помечен — архивация состоялась; но расхождение головы с
        // файлом надо назвать вслух, а не проглотить.
        if (error != nullptr) *error = QStringLiteral("mark not written to history: %1").arg(why);
    }
    return true;
}

bool restoreNote(const QString& root, const QString& noteId, QString* error) {
    const QString path = noteFile(root, noteId);
    std::string bytes;
    if (!readFileBytes(path, bytes)) {
        if (error != nullptr) *error = QStringLiteral("cannot read note %1").arg(noteId);
        return false;
    }
    // Разбора нет и здесь, по той же причине: вернуть заметку человек вправе,
    // какой бы она ни была.
    const auto [headFrom, headTo] = headerRange(bytes);
    if (headTo == 0) {
        if (error != nullptr)
            *error = QStringLiteral("note %1 has no zametti header").arg(noteId);
        return false;
    }
    const std::string_view header = std::string_view(bytes).substr(headFrom, headTo - headFrom);
    if (!headerSaysArchived(header)) return true;   // уже дома

    // ВОЗВРАТ — СНЯТЬ СТРОКУ, и больше ничего. Журнал для этого не нужен вовсе:
    // тело всё это время лежало в файле. Раньше оно бралось из головы журнала,
    // и архивная заметка без истории — приехавшая с чужой машины, потерявшая
    // журнал — возвращалась стабом, то есть не возвращалась.
    std::string out = headerWithoutArchived(header);
    out += bytes.substr(headTo);
    if (!writeFileBytes(path, out, error)) return false;

    // ВЕШКА В ИСТОРИИ. Таймлайн отвечает на вопрос «что с заметкой было», и
    // «вернули из архива» — такой же ответ, как «правили» или «удалили».
    journal::History history(root);
    QString ignored;
    history.append(noteId,
                   journal::NewRecord::restore(QByteArray(out.data(), qsizetype(out.size())), 0),
                   &ignored);
    return true;
}

namespace {

// Похоже ли тело на стаб: не больше одной непустой строки, и та — заголовок.
// Консервативно: любое сомнение трактуется как «не стаб».
bool looksLikeStub(std::string_view body) {
    int lines = 0;
    size_t at = 0;
    while (at < body.size()) {
        size_t end = body.find('\n', at);
        if (end == std::string_view::npos) end = body.size();
        std::string_view line = body.substr(at, end - at);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.remove_suffix(1);
        if (!line.empty()) {
            if (++lines > 1) return false;
            if (line.front() != '#') return false;
        }
        at = end + 1;
    }
    return lines == 1;
}

}  // namespace

int unfoldArchivedStubs(const QString& root, QStringList* leftAlone, QString* error) {
    journal::History history(root);
    int unfolded = 0;
    for (const QFileInfo& info : QDir(root).entryInfoList({QStringLiteral("*.md")}, QDir::Files)) {
        const QString id = info.completeBaseName();
        std::string bytes;
        if (!readFileBytes(info.absoluteFilePath(), bytes)) continue;
        const auto [headFrom, headTo] = headerRange(bytes);
        if (headTo == 0) continue;   // не наша заметка
        const std::string_view header = std::string_view(bytes).substr(headFrom, headTo - headFrom);
        if (!headerSaysArchived(header)) continue;
        if (!looksLikeStub(std::string_view(bytes).substr(headTo))) continue;

        journal::ZJournal read;
        QString why;
        if (!history.read(id, &read, &why)) {
            if (leftAlone != nullptr)
                leftAlone->append(QStringLiteral("%1: history unreadable (%2)").arg(id, why));
            continue;
        }

        // ИДЁМ ПО ЖУРНАЛУ НАЗАД, А НЕ БЕРЁМ ГОЛОВУ. У заметки, которую после
        // архивации ещё раз сохранили (прежняя беда: человек выходил из режима
        // истории и получал стаб, доступный для правки), головой журнала тоже
        // стаб — и по голове тело не нашлось бы вовсе. Настоящее тело лежит
        // глубже; ищем ПОСЛЕДНИЙ слепок, который стабом не является.
        std::string snapshot;
        size_t snapTo = 0;
        for (int at = read.lastSnapshotIndex(); at >= 0; at = read.previousSnapshotIndex(at)) {
            QByteArray body;
            if (!history.snapshotAt(id, at, &body, &why) || body.isEmpty()) continue;
            const std::string candidate(body.constData(), size_t(body.size()));
            const auto [from, to] = headerRange(candidate);
            (void)from;
            if (looksLikeStub(std::string_view(candidate).substr(to))) continue;
            snapshot = candidate;
            snapTo = to;
            break;
        }
        if (snapshot.empty()) {
            if (leftAlone != nullptr)
                leftAlone->append(QStringLiteral("%1: archived stub without a body in the history")
                                      .arg(id));
            continue;
        }

        // Шапка — из ФАЙЛА (в ней всё, что человек менял, пока заметка лежала в
        // архиве), тело — из журнала, журнальная шапка отрезается.
        std::string out(header);
        out += snapshot.substr(snapTo);
        if (out == bytes) continue;   // и так уже развёрнута
        QString writeError;
        if (!writeFileBytes(info.absoluteFilePath(), out, &writeError)) {
            if (error != nullptr) *error = writeError;
            return -1;
        }
        ++unfolded;
    }
    return unfolded;
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
