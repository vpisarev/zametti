#include "history_rules.h"

#include <QtGlobal>

#include <utility>

namespace zametti {

// Шапка заметки — HTML-комментарий в начале файла. Строки modified и version
// ищем только в ней: слово «modified:» в тексте заметки трогать нельзя.
//
// `version` — тоже не содержимое (refactor3): версия формата встаёт в шапку
// лениво, при первой записи правленой заметки, и без этой оговорки возврат
// отменой к состоянию, записанному ДО неё, считался бы новой записью журнала
// (набор HistoryWrite это и поймал), а заметка, вернувшаяся к исходному
// тексту, — изменённой.
bool sameApartFromModified(const QByteArray& a, const QByteArray& b) {
    const auto stripped = [](const QByteArray& text) {
        const qsizetype head = text.indexOf("-->");
        if (head < 0) return text;
        QByteArray out = text;
        for (const char* key : {"\nmodified:", "\nversion:"}) {
            const qsizetype at = out.indexOf(key);
            if (at < 0 || at > out.indexOf("-->")) continue;
            const qsizetype eol = out.indexOf('\n', at + 1);
            if (eol < 0) continue;
            out.remove(at, eol - at);
        }
        return out;
    };
    if (a.size() == b.size() && a == b) return true;
    return stripped(a) == stripped(b);
}

int changedChars(const QByteArray& a, const QByteArray& b) {
    const qsizetype shared = qMin(a.size(), b.size());
    qsizetype prefix = 0;
    while (prefix < shared && a[prefix] == b[prefix]) ++prefix;
    qsizetype suffix = 0;
    while (suffix < shared - prefix && a[a.size() - 1 - suffix] == b[b.size() - 1 - suffix])
        ++suffix;
    return int(qMax(a.size(), b.size()) - prefix - suffix);
}

namespace history {

Step decideStep(const journal::ZJournal& journal, const SnapshotOf& snapshotOf,
                const QByteArray& fresh, journal::Kind kind, qint64 now, const Rules& rules) {
    using journal::Kind;
    Step step;
    step.keep = journal.size();

    // Вот он, сторож свежести, и он тут ровно один — на оба правила сразу.
    const qint64 window = qint64(qMax(1, rules.mergeHours)) * 3600 * 1000;
    const auto stale = [&](qint64 time) { return !rules.ignoreAge && now - time > window; };

    // ВОЗВРАТ К УЖЕ ЗАПИСАННОМУ СОСТОЯНИЮ. Ищем в хвосте самую старую запись,
    // равную новому слепку. Нашли — всё, что после неё, было работой, которую
    // человек сам же и отменил: она уходит, а новая запись не пишется вовсе.
    //
    //   …, M0   →   …, M0, M0'   →   …, M0
    //
    // Одного сравнения с последней записью мало: набрал человек текст (M0'),
    // отменил его (M0" = M0) — и одинаковые записи оказывались ЧЕРЕЗ ОДНУ. Так
    // и вышло у владельца в «Пробуем Obsidian».
    //
    // Схлопывание — ТОЛЬКО для обычного сохранения, и чужую вешку оно не
    // перепрыгивает: восстановление из истории и приход правки снаружи —
    // вешки, поставленные не набором, и стирать их нельзя ничем.
    int sameAs = -1;
    for (int i = kind == Kind::Save ? journal.size() - 1 : -1; i >= 0; --i) {
        const journal::Entry& entry = journal.at(i);
        if (stale(entry.time())) break;      // дальше история старая, её не трогаем
        if (!entry.hasSnapshot()) break;   // надгробие: за него не заглядываем
        const QByteArray older = snapshotOf(i);
        if (older.isNull()) break;         // слепок не собрался — дальше не идём
        if (sameApartFromModified(older, fresh)) {
            sameAs = i;   // нашли; но, может, ещё старее лежит такая же
            continue;
        }
        if (sameAs >= 0) break;                        // старее — уже другое состояние
        if (entry.kind() != Kind::Save) break;           // чужую вешку не перепрыгиваем
    }
    if (sameAs >= 0) {
        step.keep = sameAs + 1;
        step.writeNew = false;
        step.dropped = journal.size() - step.keep + 1;   // хвост и сама новая
        return step;
    }

    // ЗАМЕНА ВМЕСТО ДОБАВЛЕНИЯ: мелкая правка встаёт на место прошлой записи.
    // Условий три, и все обязаны сойтись — прошлая запись свежая (её ещё не
    // поздно переписать), она тоже обычное сохранение, и версии разошлись на
    // мелочь.
    //
    // Меньше двух записей — это защита опорной: после замены в журнале
    // обязана остаться хотя бы одна, а первая — то, с чего заметка начиналась,
    // и стереть её нельзя ничем.
    if (kind != Kind::Save || journal.size() < 2) return step;
    const journal::Entry& back = journal.at(journal.size() - 1);
    if (back.kind() != Kind::Save || !back.hasSnapshot() || stale(back.time())) return step;
    const QByteArray tail = snapshotOf(journal.size() - 1);
    if (tail.isNull() || tail.isEmpty()) return step;
    if (changedChars(tail, fresh) > qMax(0, rules.mergeChars)) return step;
    --step.keep;
    step.merged = 1;
    return step;
}

Plan planFor(const journal::ZJournal& journal, const QVector<QByteArray>& snapshots,
             const Rules& rules) {
    Plan plan;
    for (int i = 0; i < journal.size(); ++i) plan.keep.append(i);
    if (journal.isEmpty()) return plan;

    // ПОЧЕМУ ПРОХОДОВ МОЖЕТ БЫТЬ НЕСКОЛЬКО. Один проход — это «как если бы
    // записи дописывали по одной», и неподвижной точкой он сам по себе не
    // является. Пример: A, B, C, где A и B разошлись на 150 знаков (не
    // сливаются), B и C — на 80 (сливаются). Проход даёт A, C — а между ними
    // может оказаться и 90 знаков, то есть следующий проход слил бы и их.
    // Гоняем до неподвижности; на этом стоит обещание идемпотентности —
    // повторная миграция форсом не находит уже ничего.
    //
    // Каждый проход, который что-то меняет, укорачивает список хотя бы на
    // одну запись, так что проходов не больше, чем записей.
    for (int pass = 0; pass <= journal.size(); ++pass) {
        QVector<int> accepted;      // номера принятых записей
        QVector<journal::Entry> acc;  // их рамки — их и видит правило
        const SnapshotOf snapshotOf = [&](int i) { return snapshots[accepted[i]]; };
        int duplicates = 0;
        int merged = 0;

        for (int idx : std::as_const(plan.keep)) {
            const journal::Entry& entry = journal.at(idx);
            // «Сейчас» для записи — время её самой: пересборка проигрывает
            // историю заново, и свежесть в ней меряется от момента записи, а не
            // от сегодняшнего дня. Миграции это безразлично (она на возраст не
            // глядит), а вот показу чистой истории корпусным читателем — нет.
            // Правило видит НАКОПЛЕННЫЙ журнал: пересборка проигрывает
            // историю заново, запись за записью.
            const Step step = decideStep(journal::ZJournal(acc), snapshotOf, snapshots[idx],
                                         entry.kind(), entry.time(), rules);
            duplicates += step.dropped;
            merged += step.merged;
            acc.resize(step.keep);
            accepted.resize(step.keep);
            if (!step.writeNew) continue;
            acc.append(entry);
            accepted.append(idx);
        }
        ++plan.passes;
        const bool settled = accepted.size() == plan.keep.size();
        plan.keep = accepted;
        plan.duplicates += duplicates;
        plan.merged += merged;
        if (settled) break;
    }
    return plan;
}

bool compressJournal(journal::History& history, const QString& noteId, const Rules& rules,
                     bool force, Report* report, QString* error) {
    // ВОТ ЗДЕСЬ МИГРАЦИЯ И РАСХОДИТСЯ С ЖИВОЙ ЗАПИСЬЮ, и больше нигде: она
    // чистит ретроактивно. Ставится сторож здесь, а не вызывающим, чтобы
    // «забыть выключить возраст» было негде.
    Rules retro = rules;
    retro.ignoreAge = true;

    Plan plan;
    journal::CompressOutcome outcome;
    const bool ok = history.compress(
        noteId,
        [&](const journal::ZJournal& journal, const QVector<QByteArray>& snapshots) {
            plan = planFor(journal, snapshots, retro);
            return plan.keep;
        },
        force, &outcome, error);

    if (report != nullptr) {
        report->versionBefore = outcome.versionBefore;
        report->versionAfter = outcome.versionAfter;
        report->recordsBefore = outcome.recordsBefore;
        report->recordsAfter = outcome.recordsAfter;
        report->rewritten = outcome.rewritten;
        // Считанное правилом годится, только если правило вообще спрашивали:
        // у чищеного журнала механика до плана не доходит.
        report->duplicates = outcome.rewritten ? plan.duplicates : 0;
        report->merged = outcome.rewritten ? plan.merged : 0;
    }
    return ok;
}

}  // namespace history
}  // namespace zametti
