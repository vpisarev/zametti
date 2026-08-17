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
    journal::Journal read;
    QString error;
    ZT_TRUE("журнал читается", history.read(&read, &error));
    ZT_EQ("опорная запись одна", n(1), n(read.entries.size()));
    if (!read.entries.isEmpty()) {
        ZT_EQ("временем файла, а не «сейчас»", n(1000), n(read.entries[0].time));
        QByteArray bytes;
        ZT_TRUE("слепок собирается", history.snapshotAt(0, &bytes, &error));
        ZT_EQ("и это то, с чем открыли", "первое\n", bytes.toStdString());
    }

    // Далёкая правка ложится рядом; та же — не пишется вовсе (правило отбора).
    const QByteArray far(2000, 'x');
    ZT_TRUE("далёкая правка записана", history.record(journal::Kind::Save, far, &error));
    ZT_TRUE("тот же слепок записывается без ошибки", history.record(journal::Kind::Save, far, &error));
    ZT_TRUE("журнал читается снова", history.read(&read, &error));
    ZT_EQ("две записи: опорная и далёкая; повтор не лёг рядом", n(2), n(read.entries.size()));

    // Восстановление: следующая Save становится Restore с источником; признак
    // гасится записью.
    history.markNextSaveAsRestore(1000);
    ZT_EQ("признак поставлен", n(1000), n(history.pendingRestoreSource()));
    ZT_TRUE("запись восстановления", history.record(journal::Kind::Save, "первое\n", &error));
    ZT_EQ("признак погашен записью", n(0), n(history.pendingRestoreSource()));
    ZT_TRUE("журнал читается в третий раз", history.read(&read, &error));
    if (!read.entries.isEmpty()) {
        const journal::Entry& last = read.entries.back();
        ZT_TRUE("последняя запись — восстановление", last.kind == journal::Kind::Restore);
        ZT_EQ("со временем источника", n(1000), n(last.source));
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

}  // namespace

TEST(ZNote, All) {
    checkHistory();
    checkNote();
    EXPECT_EQ(0, zt::freshFailures());
}
