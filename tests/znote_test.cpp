// ZNote и ZNoteHistory: заметка как объект и её журнал одним полем.
//
// Тесты как критерий дизайна (zametti-refactor2-design.md §4): класс обязан
// держать свои инварианты сам, без редактора. Здесь — журнал заметки без
// единого виджета: опорная запись кладётся один раз временем файла; шаг
// истории пишется по правилам отбора; «ближайшая запись — восстановление»
// гасится записью; заметка без хранилища молчит и не падает.

#include "znote.h"
#include "test_util.h"

#include <QDir>
#include <QTemporaryDir>

#include <string>

namespace {

using zametti::ZNote;
using zametti::ZNoteHistory;
namespace journal = zametti::journal;

std::string n(long long v) { return std::to_string(v); }

zametti::history::Rules rules() {
    zametti::history::Rules r;
    r.mergeChars = 100;
    r.mergeHours = 24;
    return r;
}

void checkHistory() {
    QTemporaryDir root;
    ZT_TRUE("временное хранилище", root.isValid());
    ZNoteHistory history(root.path(), QStringLiteral("01test000000000"), rules());
    ZT_TRUE("журнал доступен", history.available());

    // Опорная запись — один раз, временем файла.
    history.ensureBaseline("первое\n", 1000);
    history.ensureBaseline("второе\n", 2000);   // журнал уже начат — молчит
    journal::ZJournal read;
    QString error;
    ZT_TRUE("журнал читается", history.read(&read, &error));
    ZT_EQ("опорная запись одна", n(1), n(read.size()));
    if (!read.isEmpty()) {
        ZT_EQ("временем файла, а не «сейчас»", n(1000), n(read.at(0).time()));
        QByteArray bytes;
        ZT_TRUE("слепок собирается", history.snapshotAt(0, &bytes, &error));
        ZT_EQ("и это то, с чем открыли", "первое\n", bytes.toStdString());
    }

    // Далёкая правка ложится рядом; та же — не пишется вовсе (правило отбора).
    const QByteArray far(2000, 'x');
    ZT_TRUE("далёкая правка записана", history.record(journal::Kind::Save, far, &error));
    ZT_TRUE("тот же слепок записывается без ошибки", history.record(journal::Kind::Save, far, &error));
    ZT_TRUE("журнал читается снова", history.read(&read, &error));
    ZT_EQ("две записи: опорная и далёкая; повтор не лёг рядом", n(2), n(read.size()));

    // Восстановление: следующая Save становится Restore с источником; признак
    // гасится записью.
    history.markNextSaveAsRestore(1000);
    ZT_EQ("признак поставлен", n(1000), n(history.pendingRestoreSource()));
    ZT_TRUE("запись восстановления", history.record(journal::Kind::Save, "первое\n", &error));
    ZT_EQ("признак погашен записью", n(0), n(history.pendingRestoreSource()));
    ZT_TRUE("журнал читается в третий раз", history.read(&read, &error));
    if (!read.isEmpty()) {
        const journal::ZJournal::Entry& last = read.entries().back();
        ZT_TRUE("последняя запись — восстановление", last.kind() == journal::Kind::Restore);
        ZT_EQ("со временем источника", n(1000), n(last.source()));
    }
    // Внешняя правка — своим родом, признак восстановления не трогает.
    history.markNextSaveAsRestore(2000);
    ZT_TRUE("внешняя правка записана", history.record(journal::Kind::External, "чужое\n", &error));
    ZT_EQ("внешняя запись признак не гасит", n(2000), n(history.pendingRestoreSource()));
    history.clearPendingRestore();
    ZT_EQ("погашен явно", n(0), n(history.pendingRestoreSource()));

    // Без хранилища — всё «нет», и ничего не падает.
    ZNoteHistory none;
    ZT_TRUE("без хранилища журнала нет", !none.available());
    none.ensureBaseline("x", 1);
    ZT_TRUE("запись без хранилища — ложь", !none.record(journal::Kind::Save, "x", &error));
    ZT_TRUE("чтение без хранилища — ложь", !none.read(&read, &error));
    QByteArray bytes;
    ZT_TRUE("слепок без хранилища — ложь", !none.snapshotAt(0, &bytes, &error));
    none.compressOnce();
}

void checkNote() {
    ZNote empty;
    ZT_TRUE("пустая заметка без пути", !empty.hasPath());
    ZT_TRUE("и без журнала", !empty.history().available());
    ZT_TRUE("статистика не свежа", !empty.statsFresh());

    const QByteArray bytes("# Заголовок\n\nтекст\n");
    ZNote note(QStringLiteral("/tmp/store/01abcdefghijkl.md"), bytes, zametti::hashOf(bytes),
               ZNoteHistory());
    ZT_EQ("id — имя файла без расширения", "01abcdefghijkl", note.id().toStdString());
    ZT_EQ("последняя записанная копия — байты файла", bytes.toStdString(),
          note.lastSaved().toStdString());
    ZT_TRUE("отпечаток — их же", note.digest() == zametti::hashOf(bytes));

    // Место человека — три числа разом.
    note.rememberCaret(7, 3, 120);
    ZT_EQ("каретка", n(7), n(note.caret().cursor));
    ZT_EQ("второй конец", n(3), n(note.caret().anchor));
    ZT_EQ("прокрутка", n(120), n(note.caret().scroll));

    // Самопроверка: отвечает, изменился ли признак.
    ZT_TRUE("признак поднят — изменение", note.setSelfCheckFailed(true));
    ZT_TRUE("тот же — не изменение", !note.setSelfCheckFailed(true));
    ZT_TRUE("снят — изменение", note.setSelfCheckFailed(false));

    // Потерянная мета: есть, забирается один раз.
    zametti::NoteHeader lost;
    lost.set("created", "2020-01-01T00:00:00Z");
    note.rememberLostMeta(lost);
    ZT_TRUE("потерянная мета есть", note.hasLostMeta());
    ZT_EQ("и забирается", "2020-01-01T00:00:00Z", note.takeLostMeta().get("created"));
    ZT_TRUE("а второй раз её нет", !note.hasLostMeta());

    // Запись: отпечаток и копия меняются вместе.
    const QByteArray written("# Заголовок\n\nтекст правленый\n");
    note.markWritten(zametti::hashOf(written), written);
    ZT_EQ("копия — записанное", written.toStdString(), note.lastSaved().toStdString());
    ZT_TRUE("отпечаток — записанного", note.digest() == zametti::hashOf(written));

    // Подмена документа обесценивает заплатку и статистику.
    note.setBuiltBlocks({});
    ZT_TRUE("блоки для заплатки есть", note.builtBlocks() != nullptr);
    zametti::NoteStats stats;
    stats.words = 3;
    note.setStats(stats);
    ZT_TRUE("статистика свежа", note.statsFresh());
    note.replaceDoc(zametti::ZDocument());
    ZT_TRUE("после подмены документа заплатке не за что зацепиться",
            note.builtBlocks() == nullptr);
    ZT_TRUE("и статистика не свежа", !note.statsFresh());
}

// КРУГ ФАЙЛА У ЗАМЕТКИ: шапка + тело склеиваются и разбираются в ZNote,
// байты те же; метаданные — из шапки и тела; глаголы пишут в шапку; служебный
// ZDocument шапки не имеет.
void checkFileRound() {
    const std::string source =
        "<!-- zametti\nparent: 01aaaaaaaaaaaaa\ncreated: 2024-05-01T10:00:00+02:00\n"
        "modified: 2024-06-01T12:00:00Z\nsort: name-asc\nchужой: ключ\n-->\n\n"
        "# Заголовок\n\nтело заметки\n";
    ZNote note(QStringLiteral("/store/01bbbbbbbbbbbbb.md"), QByteArray::fromStdString(source),
               zametti::hashOf(source), ZNoteHistory());
    ZT_TRUE("файл разобран", note.load(source));
    ZT_TRUE("шапка у заметки", note.hasHeader());
    ZT_EQ("круг файл→заметка→файл побайтовый", source, note.toMarkdown());
    ZT_EQ("тело документа — без шапки", "# Заголовок\n\nтело заметки\n", note.doc().toMarkdown(zametti::NoteHeader{}));
    ZT_EQ("чужой ключ шапки цел", "ключ", note.headerValue(QStringLiteral("chужой")).toStdString());

    const ZNote::Metadata m = note.metadata();
    ZT_EQ("id из пути", "01bbbbbbbbbbbbb", m.id().toStdString());
    ZT_EQ("родитель из шапки", "01aaaaaaaaaaaaa", m.parent().toStdString());
    ZT_EQ("заголовок из тела", "Заголовок", m.title().toStdString());
    ZT_EQ("сниппет из тела", "тело заметки", m.snippet().toStdString());
    ZT_EQ("modified в сравнимой форме (UTC)", "2024-06-01T12:00:00Z", m.modified().toStdString());
    ZT_EQ("created приведено к UTC", "2024-05-01T08:00:00Z", m.created().toStdString());
    ZT_TRUE("метка сортировки прочитана", m.sortMark().has_value());
    ZT_TRUE("не архив, не папка", !m.archived() && !m.folder());

    // Глаголы пишут в шапку — и в файл.
    note.setParentId(QString());
    note.setArchived(true);
    note.setRole(QStringLiteral("folder"));
    ZT_TRUE("родитель снят", note.parentId().isEmpty());
    ZT_TRUE("архив и папка — из шапки", note.metadata().archived() && note.metadata().folder());
    ZT_TRUE("archived в байтах файла", note.toMarkdown().find("archived: yes") != std::string::npos);
    ZT_TRUE("role в байтах файла", note.toMarkdown().find("role: folder") != std::string::npos);
    // Заметка без шапки — тоже заметка (файл вне хранилища).
    ZNote plain;
    ZT_TRUE("тело без шапки разобрано", plain.load("просто текст\n"));
    ZT_TRUE("шапки нет", !plain.hasHeader());
    ZT_EQ("круг без шапки", "просто текст\n", plain.toMarkdown());
    ZT_EQ("заголовок — первый содержательный блок", "просто текст", plain.title().toStdString());
    ZNote empty;
    ZT_TRUE("пустая разобрана", empty.load(""));
    ZT_EQ("у пустой заголовок по умолчанию", "Untitled", empty.title().toStdString());
}

// ВЕРСИЯ ФОРМАТА В ШАПКЕ — ЛЕНИВО (решение владельца, refactor3): `version: 1`
// получает только та заметка, которую программа ПИШЕТ: ставится записью
// (NoteEditor::save, набор Save), а не штампом modified — штамп ставится и
// откатывается ещё до решения «сохранять нечего». Здесь — сама операция шапки:
// первой строкой, более новую версию не понижает и не удваивает.
void checkVersionStamp() {
    ZNote note;
    ZT_TRUE("разобрана", note.load("<!-- zametti\ncreated: 2024-05-01T10:00:00+02:00\n-->\n\nтело\n"));
    ZT_TRUE("версии в старой заметке нет", note.headerValue(QStringLiteral("version")).isEmpty());
    note.stampModified();
    ZT_TRUE("штамп modified версию НЕ ставит", note.headerValue(QStringLiteral("version")).isEmpty());
    note.header().ensureVersion();
    ZT_EQ("ensureVersion ставит version: 1", "1", note.headerValue(QStringLiteral("version")).toStdString());
    ZT_TRUE("и первой строкой шапки",
            note.toMarkdown().rfind("<!-- zametti\nversion: 1\ncreated: ", 0) == 0);

    ZNote newer;
    ZT_TRUE("разобрана", newer.load("<!-- zametti\nversion: 2\n-->\n\nтело\n"));
    newer.header().ensureVersion();
    ZT_EQ("более новая версия не понижается", "2", newer.headerValue(QStringLiteral("version")).toStdString());
    ZT_TRUE("и не удваивается", newer.toMarkdown().find("version: 2\nversion") == std::string::npos);

    ZNote bare;
    ZT_TRUE("без шапки", bare.load("просто текст\n") && !bare.hasHeader());
    bare.header().ensureVersion();
    ZT_TRUE("у безшапочной версия заводит шапку", bare.hasHeader());
    ZT_EQ("шапка из одной строки и пустой строки за ней",
          "<!-- zametti\nversion: 1\n-->\n\nпросто текст\n", bare.toMarkdown());
}

}  // namespace

TEST(ZNote, All) {
    checkHistory();
    checkNote();
    checkFileRound();
    checkVersionStamp();
    EXPECT_EQ(0, zt::freshFailures());
}
