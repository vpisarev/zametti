// ZNoteTimeline: заметка, прочитанная из истории, — без единого виджета.
//
// Что обязан держать сам объект (критерий дизайна refactor2 §4):
//   * открывается на последнем слепке, надгробия пропускает; шаги назад и
//     вперёд идут по слепкам; вперёд с последнего — «дальше живая версия»;
//   * две базы: предыдущая запись и свежая версия; смена базы меняет
//     сравнение, а слепок — нет; у первой записи базы нет — всё добавлено;
//   * документ разности: для той же пары — тот же документ, для другой —
//     другой; убранные строки в нём видны своим текстом, сводки «удалено:» нет;
//   * тело слепка — байт в байт то, что писали в журнал (восстановлению);
//   * карта блок ↔ строка слепка согласована со сравнением;
//   * журнал разделяется с заметкой (тот же объект).

#include "znote.h"
#include "zstorage.h"
#include "znote_timeline.h"
#include "test_util.h"

#include <QDir>
#include <QTemporaryDir>

#include <string>

namespace {

using zametti::ZNoteTimeline;
namespace diff = zametti::diff;

std::string n(long long v) { return std::to_string(v); }

zametti::history::Rules rules() {
    zametti::history::Rules r;
    r.mergeChars = 100;
    r.mergeHours = 24;
    return r;
}

QByteArray note(const char* body, const char* stamp) {
    return QByteArray("<!-- zametti\ncreated: 2026-01-01T00:00:00Z\nmodified: ") + stamp +
           "\n-->\n\n" + body;
}

const qint64 kNow = 1'700'000'000'000LL;
const qint64 kMinute = 60'000;

// Три версии в журнале: v0 (опорная), v1, v2 — и «свежая» v3 в буфере.
const char* kV0 = "# Заголовок\n\nПервый абзац.\n\nВторой абзац.\n\n- пункт\n- ещё пункт\n";
const char* kV1 = "# Заголовок\n\nПервый абзац поправленный.\n\n- пункт\n- ещё пункт\n- третий пункт\n";
const char* kV2 = "# Заголовок\n\nПервый абзац поправленный.\n\n- пункт\n- третий пункт\n";
const char* kV3 = "# Заголовок\n\nСовсем новый абзац.\n\n- пункт\n- третий пункт\n";

struct Fixture {
    QTemporaryDir root;
    QString id = QStringLiteral("01timeline00001");
    // Хранилище живёт дольше журнала заметки: журнал держит на него указатель.
    std::shared_ptr<zametti::ZStorage> store;
    std::shared_ptr<zametti::ZJournal> journal;

    Fixture() {
        QDir().mkpath(root.path() + QStringLiteral("/.zametti"));
        QDir().mkpath(root.path() + QStringLiteral("/history"));
        store = std::make_shared<zametti::ZStorage>(root.path());
        zametti::ZStorage& file = *store;
        QString error;
        file.appendToJournal(id, zametti::ZJournal::NewRecord::save(note(kV0, "a"), zametti::ZJournal::Stamp::at(kNow)), &error);
        file.appendToJournal(id, zametti::ZJournal::NewRecord::save(note(kV1, "b"), zametti::ZJournal::Stamp::at(kNow + kMinute)), &error);
        file.appendToJournal(id, zametti::ZJournal::NewRecord::save(note(kV2, "c"), zametti::ZJournal::Stamp::at(kNow + 2 * kMinute)), &error);
        journal = std::make_shared<zametti::ZJournal>(store.get(), id, rules());
    }
};

QStringList linesOf(ZNoteTimeline& tl) {
    QStringList out;
    const zametti::ZDocument& doc = tl.document();
    for (int i = 0; i < doc.blockCount(); ++i) out.append(doc.blockAt(i).text);
    return out;
}

void checkOpenAndSteps() {
    Fixture f;
    ZNoteTimeline tl(f.journal, note(kV3, "d"), nullptr);
    ZT_TRUE("до open не открыт", !tl.isOpen());
    QString error;
    ZT_TRUE("открылся", tl.open(-1, &error));
    ZT_EQ("три записи", n(3), n(tl.count()));
    ZT_EQ("встали на последний слепок", n(2), n(tl.index()));
    ZT_EQ("время слепка — время записи", n(kNow + 2 * kMinute), n(tl.snapshotTime()));
    ZT_EQ("тело слепка — байт в байт то, что писали",
          std::string(kV2), tl.snapshotBody());

    ZT_TRUE("шаг назад", tl.stepBack());
    ZT_EQ("показан предыдущий", n(1), n(tl.index()));
    ZT_EQ("и тело его", std::string(kV1), tl.snapshotBody());
    ZT_TRUE("ещё шаг назад — к опорной", tl.stepBack());
    ZT_EQ("опорная запись", n(0), n(tl.index()));
    ZT_TRUE("дальше опорной ходу нет", !tl.stepBack());
    ZT_EQ("и мы остались на ней", n(0), n(tl.index()));
    ZT_TRUE("шаг вперёд", tl.stepForward());
    ZT_TRUE("ещё шаг вперёд", tl.stepForward());
    ZT_EQ("снова последний", n(2), n(tl.index()));
    ZT_TRUE("дальше последнего — только живая версия", !tl.stepForward());
    ZT_EQ("индекс не тронут", n(2), n(tl.index()));

    // Прямой выбор: вне диапазона — ложь и ничего не меняется.
    ZT_TRUE("выбор вне диапазона отвергнут", !tl.select(7));
    ZT_EQ("показанное прежнее", n(2), n(tl.index()));
    ZT_TRUE("выбор первой", tl.select(0));
    ZT_EQ("первая показана", n(0), n(tl.index()));
}

void checkBases() {
    Fixture f;
    ZNoteTimeline tl(f.journal, note(kV3, "d"), nullptr);
    ZT_TRUE("открылся", tl.open());
    // По умолчанию — с предыдущей записью.
    ZT_TRUE("база по умолчанию — предыдущая", !tl.baseIsFresh());
    ZT_EQ("время базы — время предыдущей записи", n(kNow + kMinute), n(tl.baseTime()));
    const int withPrevious = tl.changedLines();
    ZT_TRUE("с предыдущей разница есть: " + n(withPrevious), withPrevious > 0);
    // «ещё пункт» ушёл между v1 и v2 — в документе он виден своим текстом.
    QStringList lines = linesOf(tl);
    ZT_TRUE("убранная строка показана своим текстом",
            lines.contains(QStringLiteral("- ещё пункт")));
    ZT_TRUE("сводки «удалено:» нет", !lines.filter(QStringLiteral("removed:")).size());
    // Метки: убранная — Removed.
    const int gone = int(lines.indexOf(QStringLiteral("- ещё пункт")));
    ZT_TRUE("убранная помечена Removed",
            gone >= 0 && tl.markOfBlock(gone) == diff::Mark::Removed);

    // Со свежей: базой становится буфер, время базы 0.
    tl.setBase(ZNoteTimeline::Base::Fresh);
    ZT_TRUE("база — свежая", tl.baseIsFresh());
    ZT_EQ("времени у свежей базы нет", n(0), n(tl.baseTime()));
    const int withFresh = tl.changedLines();
    ZT_TRUE("со свежей разница есть: " + n(withFresh), withFresh > 0);
    lines = linesOf(tl);
    // v2 → v3: «Первый абзац поправленный.» заменён на «Совсем новый абзац.» —
    // но сравнение идёт база(свежая) → слепок: в слепке ЕСТЬ «поправленный»,
    // а «Совсем новый» — убран относительно слепка (есть в базе, нет здесь).
    ZT_TRUE("строка свежей версии видна как убранная",
            lines.contains(QStringLiteral("Совсем новый абзац.")));
    const int fresh = int(lines.indexOf(QStringLiteral("Совсем новый абзац.")));
    ZT_TRUE("...и помечена Removed", fresh >= 0 && tl.markOfBlock(fresh) == diff::Mark::Removed);
    const int mine = int(lines.indexOf(QStringLiteral("Первый абзац поправленный.")));
    ZT_TRUE("строка слепка помечена Added",
            mine >= 0 && tl.markOfBlock(mine) == diff::Mark::Added);
    ZT_TRUE("пара идёт «− старая / + новая»", fresh >= 0 && mine == fresh + 1);
    // Слепок при смене базы не меняется.
    ZT_EQ("слепок тот же", std::string(kV2), tl.snapshotBody());

    // Первая запись: предыдущей нет — всё добавлено.
    tl.setBase(ZNoteTimeline::Base::Previous);
    ZT_TRUE("на первую", tl.select(0));
    ZT_EQ("базы нет — время 0", n(0), n(tl.baseTime()));
    bool allAdded = true;
    for (int i = 0; i < tl.document().blockCount(); ++i)
        allAdded = allAdded && tl.markOfBlock(i) == diff::Mark::Added;
    ZT_TRUE("у первой записи всё добавлено", allAdded && tl.document().blockCount() > 0);
}

void checkDocumentCache() {
    Fixture f;
    ZNoteTimeline tl(f.journal, note(kV3, "d"), nullptr);
    ZT_TRUE("открылся", tl.open());
    const zametti::ZDocument a = tl.document();
    const zametti::ZDocument b = tl.document();
    ZT_TRUE("та же пара — тот же документ", a.sameHandle(b));
    tl.setBase(ZNoteTimeline::Base::Fresh);
    const zametti::ZDocument c = tl.document();
    ZT_TRUE("другая база — другой документ", !a.sameHandle(c));
    tl.setBase(ZNoteTimeline::Base::Previous);
    ZT_TRUE("вернулись к базе — вернулся и документ, без пересборки",
            a.sameHandle(tl.document()));
    ZT_TRUE("на другой слепок", tl.stepBack());
    ZT_TRUE("другой слепок — другой документ", !a.sameHandle(tl.document()));
    // Смена облика выбрасывает документы.
    const zametti::ZDocument d = tl.document();
    tl.setStyle(nullptr);
    ZT_TRUE("после смены облика документ собран заново", !d.sameHandle(tl.document()));
}

void checkBlockLineMap() {
    Fixture f;
    ZNoteTimeline tl(f.journal, note(kV3, "d"), nullptr);
    ZT_TRUE("открылся", tl.open());
    const diff::Result& result = tl.result();
    const int blocks = tl.document().blockCount();
    // Каждый блок ведёт к строке сравнения, а сравнение — обратно к блоку.
    bool consistent = true;
    for (int b = 0; b < blocks; ++b) {
        const int row = tl.rowOfBlock(b);
        if (row < 0 || row >= result.rows.size()) { consistent = false; continue; }
        const int after = result.rows[row].after;
        if (after >= 0) {
            // Строка слепка ведёт к блоку не позже этого (у убранной строки
            // тот же after у соседа).
            const int back = tl.blockOfAfterLine(after);
            if (back < 0 || back > b) consistent = false;
            if (tl.afterLineOfBlock(b) != after) consistent = false;
        }
    }
    ZT_TRUE("карта блок ↔ строка согласована со сравнением", consistent);
    ZT_EQ("вне диапазона — -1", n(-1), n(tl.rowOfBlock(blocks + 5)));
    ZT_EQ("отрицательная строка — -1", n(-1), n(tl.blockOfAfterLine(-1)));
    // Число блоков = строки сравнения + по одному на каждую изменённую.
    int changedRows = 0;
    for (const diff::Row& row : result.rows)
        if (row.mark == diff::Mark::Changed) ++changedRows;
    ZT_EQ("блоков = строк сравнения + изменённые", n(result.rows.size() + changedRows), n(blocks));
}

void checkSharedJournalAndNoHistory() {
    Fixture f;
    // Журнал разделяется с заметкой: чистка и хвост — одни на двоих.
    zametti::ZNote note(f.root.path() + QLatin1Char('/') + f.id + QStringLiteral(".md"),
                        ::note(kV2, "c"), zametti::Digest{},
                        std::make_shared<zametti::ZJournal>(f.store.get(), f.id, rules()));
    ZNoteTimeline tl(note.journalPtr(), note.fileBytes(), nullptr);
    ZT_TRUE("тот же объект журнала", tl.journalPtr().get() == &note.journal());
    ZT_TRUE("открылся по журналу заметки", tl.open());

    // Без хранилища — «истории нет», и ничего не падает.
    ZNoteTimeline none(std::make_shared<zametti::ZJournal>(), QByteArray(), nullptr);
    QString error;
    ZT_TRUE("без журнала open ложь", !none.open(-1, &error));
    ZT_TRUE("и объяснение есть", !error.isEmpty());
    ZT_TRUE("не открыт", !none.isOpen());
    ZT_TRUE("шаги без слепка молчат", !none.stepBack() && !none.stepForward());
    ZT_EQ("тела нет", std::string(), none.snapshotBody());
    // Нулевой указатель на журнал — тоже не падение.
    ZNoteTimeline null(nullptr, QByteArray(), nullptr);
    ZT_TRUE("нулевой журнал — истории нет", !null.open());
}

}  // namespace

TEST(ZNoteTimeline, All) {
    checkOpenAndSteps();
    checkBases();
    checkDocumentCache();
    checkBlockLineMap();
    checkSharedJournalAndNoHistory();
    EXPECT_EQ(0, zt::freshFailures());
}
