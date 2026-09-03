// ЧИСТОЕ ПЕРЕКЛЮЧЕНИЕ ЗАМЕТКИ НЕ ПИШЕТ И НЕ СЕРИАЛИЗУЕТ.
//
// Владелец, 03.09.2026: «переключение между заметками без правок перестало
// быть мгновенным». Стенд switch показал две полные сериализации на каждый
// уход (save(force) и сверка кэша) и, хуже, МАШИНУ ХАОСА: заметка, чей файл
// не в каноне, переписывалась на каждом уходе — штамп modified, слепок в
// журнал, всплытие в списке — без единой правки человека.
//
// Правило: не тронул — не сериализуем и не пишем; тронул — пишем один раз, и
// записанное становится неподвижной точкой (второй уход ничего не пишет).

#include "editor_widget.h"
#include "journal.h"
#include "zstorage.h"

#include "test_util.h"
#include "testdata.h"

#include <QDir>
#include <QFile>
#include <QTest>

#include <string>

namespace {

std::string bytesOf(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return f.readAll().toStdString();
}

int recordsOf(zametti::ZStorage& storage, const QString& id) {
    zametti::ZJournal jrn;
    QString error;
    if (!storage.readJournal(id, &jrn, &error)) return -1;
    return jrn.size();
}

}  // namespace

TEST(SwitchClean, All) {
    using zametti::ZStorage;
    const QString root = zt::TestData::outDir(QStringLiteral("switch-clean")) + QStringLiteral("/store");
    QString error;
    ZT_TRUE("хранилище заведено", ZStorage(root).init(&error));
    auto storage = std::make_shared<ZStorage>(root);
    storage->reload();
    const QString a = storage->createNote(QString(), false, &error);
    const QString b = storage->createNote(QString(), false, &error);
    ZT_TRUE("две заметки", !a.isEmpty() && !b.isEmpty());
    // Заметка A — руками, НЕ в каноне: `\~` писатель сегодня отдаёт голой
    // тильдой (класс A фаззера), и сверка строения при открытии отступает —
    // ровно тот случай, что переписывался на каждом уходе.
    {
        QFile f(storage->pathOf(a));
        ZT_TRUE("A записана", f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write("<!-- zametti\ncreated: 2020-01-01T00:00:00Z\nmodified: 2020-01-01T00:00:00Z\n-->\n\n"
                "# А\n\nстрока с \\~|-|+ знаками\n\nхвост\n\n");
    }
    {
        QFile f(storage->pathOf(b));
        ZT_TRUE("B записана", f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write("<!-- zametti\ncreated: 2020-01-01T00:00:00Z\nmodified: 2020-01-01T00:00:00Z\n-->\n\n"
                "# Б\n\nобычный абзац\n");
    }
    const std::string a0 = bytesOf(storage->pathOf(a));
    const std::string b0 = bytesOf(storage->pathOf(b));

    zametti::NoteEditor editor;
    editor.setStorage(storage);
    editor.resize(700, 500);
    editor.show();
    QTest::qWait(20);

    // 1. Открыли A. Открытие вправе ВЫЛЕЧИТЬ файл (canonicaliseFile: `\~` →
    //    `~`, лишняя пустая строка) — без штампа и без записи в журнал: это
    //    причёсывание, не правка. Ушли в B: A не сериализуется и не пишется,
    //    файл после лечения — байт в байт, журнал — одна опорная запись.
    ZT_TRUE("A открылась", editor.openFile(storage->pathOf(a)));
    QTest::qWait(20);
    const std::string aHealed = bytesOf(storage->pathOf(a));
    ZT_TRUE("лечение при открытии не ставит штамп",
            aHealed.find("modified: 2020-01-01T00:00:00Z") != std::string::npos);
    ZT_TRUE("B открылась", editor.openFile(storage->pathOf(b)));
    ZT_EQ("уход с нетронутой A — без сериализации (5)", std::string("5"),
          std::to_string(editor.lastOpenTrace().saveOutcome));
    ZT_EQ("файл A после ухода тот же, что после открытия", aHealed, bytesOf(storage->pathOf(a)));
    ZT_EQ("журнал A: одна опорная запись", std::string("1"), std::to_string(recordsOf(*storage, a)));

    // 2. Обратно в A (из кэша) и снова в B: то же самое, и из кэша тоже.
    ZT_TRUE("A снова", editor.openFile(storage->pathOf(a)));
    ZT_TRUE("A из кэша", editor.lastOpenTrace().fromCache);
    ZT_TRUE("B снова", editor.openFile(storage->pathOf(b)));
    ZT_EQ("уход с A из кэша — без сериализации", std::string("5"),
          std::to_string(editor.lastOpenTrace().saveOutcome));
    ZT_EQ("файл A по-прежнему не тронут", aHealed, bytesOf(storage->pathOf(a)));
    ZT_EQ("файл B не тронут", b0, bytesOf(storage->pathOf(b)));

    // 3. Правка в A: уход пишет — один раз, и файл становится неподвижной точкой.
    ZT_TRUE("A для правки", editor.openFile(storage->pathOf(a)));
    editor.moveCursor(QTextCursor::End);
    QTest::keyClicks(&editor, QStringLiteral("z"));
    QTest::qWait(20);
    ZT_TRUE("в B после правки", editor.openFile(storage->pathOf(b)));
    ZT_EQ("уход с правленой A — запись (3)", std::string("3"),
          std::to_string(editor.lastOpenTrace().saveOutcome));
    const std::string a1 = bytesOf(storage->pathOf(a));
    ZT_TRUE("файл A изменился", a1 != aHealed);
    (void)a0;
    ZT_TRUE("правка в файле", a1.find("хвостz") != std::string::npos || a1.find("z\n") != std::string::npos);
    ZT_EQ("журнал A: две записи", std::string("2"), std::to_string(recordsOf(*storage, a)));

    ZT_TRUE("A после записи", editor.openFile(storage->pathOf(a)));
    ZT_TRUE("в B ещё раз", editor.openFile(storage->pathOf(b)));
    ZT_EQ("второй уход с записанной A ничего не пишет", std::string("5"),
          std::to_string(editor.lastOpenTrace().saveOutcome));
    ZT_EQ("файл A — неподвижная точка", a1, bytesOf(storage->pathOf(a)));
    ZT_EQ("журнал A не вырос", std::string("2"), std::to_string(recordsOf(*storage, a)));

    // 4. Смена облика (зум) правкой не считается: уход после неё не пишет.
    ZT_TRUE("A для зума", editor.openFile(storage->pathOf(a)));
    editor.setZoom(editor.zoom() * 1.5);
    QTest::qWait(20);
    ZT_TRUE("в B после зума", editor.openFile(storage->pathOf(b)));
    ZT_EQ("зум — не правка: без сериализации", std::string("5"),
          std::to_string(editor.lastOpenTrace().saveOutcome));
    ZT_EQ("файл A после зума тот же", a1, bytesOf(storage->pathOf(a)));
}
