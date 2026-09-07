// ZStorage: архив и миграции прежних видов хранилища.
//
// Архив — ПОМЕТКА В ШАПКЕ (NoteHeader::kArchivedKey), и ничего кроме. Пришёл
// на место корзины (этап 15): корзина была ПАПКОЙ, куда заметку переносили, а
// архив — пометка; `parent` не трогается, «Архив» в дереве собирается сам из
// помеченных, восстановление — снять пометку.
//
// ДО ЭТАПА 17 АРХИВАЦИЯ СРЕЗАЛА ТЕЛО в журнал, оставляя стаб. Платили за это
// четырежды и молча: из файла исчезали ссылки на вложения (по ним считается,
// чему уходить при удалении насовсем); картинку, которую держала только
// архивная заметка, уносило удаление соседней; поиск переставал видеть
// архивные тела; человек, вышедший из режима истории, получал стаб для правки.
// Теперь тело остаётся в файле, а стабы прежних сборок разворачивает
// unfoldArchivedStubs при открытии.
//
// Всё здесь — ПО БАЙТАМ, без разбора (правило владельца, записанное после
// того, как заметка с формулами оказалась испорчена: «пусть .md будет
// битый-перебитый — раз шапка на месте, программа обязана убрать его в
// архив, а разбирать дальше вообще не должна»). Шапка берётся куском
// исходных байтов, пометка дописывается строкой. Исключение одно —
// migrateTrashToArchive: там правятся три ключа, и заметка разбирается.

#include "zstorage.h"

#include "journal.h"
#include "znote.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>

#include <algorithm>
#include <string_view>
#include <utility>

namespace zametti {
namespace {

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

bool ZStorage::archiveOne(const QString& id, const ZJournal::Rules& rules, QString* error) {
    const QString path = pathOf(id);
    std::string bytes;
    if (!readFileBytes(path, bytes)) {
        if (error != nullptr) *error = QStringLiteral("cannot read note %1").arg(id);
        return false;
    }
    const auto [headFrom, headTo] = headerRange(bytes);
    if (headTo == 0) {
        if (error != nullptr) *error = QStringLiteral("note %1 has no zametti header").arg(id);
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
    ZJournal read;
    QString why;
    if (!readJournal(id, &read, &why)) {
        if (error != nullptr) *error = QStringLiteral("cannot read history: %1").arg(why);
        return true;   // файл уже помечен: архивация состоялась
    }
    const QByteArray snapshot(out.data(), qsizetype(out.size()));
    const auto snapshotOf = [&](int at) {
        QByteArray older;
        QString ignored;
        if (!journalSnapshot(id, at, &older, &ignored)) return QByteArray();
        return older;
    };
    const ZJournal::Step step = read.planStep(
        snapshotOf, snapshot, ZJournal::Kind::Save, QDateTime::currentMSecsSinceEpoch(), rules);
    QVector<ZJournal::RecordRef> voids;
    voids.reserve(step.voided.size());
    for (int at : step.voided)
        voids.append(ZJournal::RecordRef(read.at(at).time(), read.at(at).digest()));
    if (step.writeNew &&
        !appendToJournal(id, ZJournal::NewRecord::save(snapshot).voiding(voids), &why)) {
        // Файл уже помечен — архивация состоялась; но расхождение головы с
        // файлом надо назвать вслух, а не проглотить.
        if (error != nullptr) *error = QStringLiteral("mark not written to history: %1").arg(why);
    }
    return true;
}

bool ZStorage::restoreOne(const QString& id, QString* error) {
    const QString path = pathOf(id);
    std::string bytes;
    if (!readFileBytes(path, bytes)) {
        if (error != nullptr) *error = QStringLiteral("cannot read note %1").arg(id);
        return false;
    }
    // Разбора нет и здесь, по той же причине: вернуть заметку человек вправе,
    // какой бы она ни была.
    const auto [headFrom, headTo] = headerRange(bytes);
    if (headTo == 0) {
        if (error != nullptr) *error = QStringLiteral("note %1 has no zametti header").arg(id);
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
    QString ignored;
    appendToJournal(id, ZJournal::NewRecord::restore(QByteArray(out.data(), qsizetype(out.size())), 0),
                    &ignored);
    return true;
}

int ZStorage::unfoldArchivedStubs(QStringList* leftAlone, QString* error) {
    int unfolded = 0;
    // BY THE CATALOGUE, NOT BY THE DISK: only an archived note can be a stub,
    // and the catalogue knows which notes are archived without reading a byte
    // more. A start must not grow with the size of the store (CLAUDE.md, O(1)):
    // before 07.09.2026 this read every note whole on every start.
    if (!loaded_) reload();
    QStringList candidates;
    for (auto it = notes_.constBegin(); it != notes_.constEnd(); ++it)
        if (it->archived()) candidates << it.key();
    candidates.sort();   // the catalogue's order is not defined; the behaviour must be
    for (const QString& id : candidates) {
        const QString path = pathOf(id);
        std::string bytes;
        if (!readFileBytes(path, bytes)) continue;
        const auto [headFrom, headTo] = headerRange(bytes);
        if (headTo == 0) continue;   // не наша заметка
        const std::string_view header = std::string_view(bytes).substr(headFrom, headTo - headFrom);
        if (!headerSaysArchived(header)) continue;
        if (!looksLikeStub(std::string_view(bytes).substr(headTo))) continue;

        ZJournal read;
        QString why;
        if (!readJournal(id, &read, &why)) {
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
            if (!journalSnapshot(id, at, &body, &why) || body.isEmpty()) continue;
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
        if (!writeFileBytes(path, out, &writeError)) {
            if (error != nullptr) *error = writeError;
            return -1;
        }
        ++unfolded;
    }
    return unfolded;
}

int ZStorage::migrateTrashToArchive(QString* error) {
    // The trash and its children are found in the catalogue: a store without
    // an old trash costs this step nothing (before 07.09.2026 every note was
    // read and built here, on every start). Only the children are read — they
    // are the ones rewritten.
    if (!loaded_) reload();
    QString trashId;
    for (auto it = notes_.constBegin(); it != notes_.constEnd(); ++it)
        if (it->role() == QLatin1String("trash")) trashId = it.key();
    if (trashId.isEmpty()) return 0;

    QStringList children;
    for (auto it = notes_.constBegin(); it != notes_.constEnd(); ++it)
        if (it.key() != trashId && it->parent() == trashId) children << it.key();
    children.sort();

    int moved = 0;
    for (const QString& id : children) {
        std::string bytes;
        if (!readFileBytes(pathOf(id), bytes)) continue;
        ZNote doc;
        doc.load(bytes);
        // Пустое значение снимает ключ — и «домой в корень» выражается ровно им.
        doc.setParentId(doc.headerValue(QStringLiteral("trash-parent")));
        doc.setHeaderValue(QStringLiteral("trash-parent"), QString());
        doc.setHeaderValue(QStringLiteral("trash-path"), QString());
        doc.setArchived(true);
        if (!writeFileBytes(pathOf(id), doc.toMarkdown(), error)) return -1;
        ++moved;
    }

    // Опустевшая корзина уходит вместе со своим журналом: заметкой она не была
    // никогда, и место в Архиве ей ни к чему.
    QString ignored;
    forgetNote(trashId, &ignored);
    return moved;
}

bool ZStorage::forgetNote(const QString& id, QString* error) {
    const QString path = pathOf(id);
    const bool hadFile = QFile::exists(path);
    QString why;
    if (!files().remove(path, &why)) {
        if (error != nullptr)
            *error = QStringLiteral("cannot delete note file %1: %2").arg(id, why);
        return false;
    }

    // ЖУРНАЛ УХОДИТ ВМЕСТЕ С ЗАМЕТКОЙ — и только здесь. Обычное удаление
    // журнал бережёт (по нему заметку можно воскресить), но у заметки-корзины
    // истории нет по смыслу, и её журналу место в мусорке.
    if (!removeJournal(id, error)) return false;
    if (!hadFile && error != nullptr)
        *error = QStringLiteral("note file %1 did not exist").arg(id);
    return true;
}

}  // namespace zametti
