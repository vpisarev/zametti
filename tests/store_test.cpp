// Утилита хранилища: init, new, импорт синтетического дерева (иерархия,
// времена из манифеста, переписанные и непереписанные ссылки, вложения с
// дедупликацией, отчёт), verify на свежемигрированном — ноль замечаний
// (инвариант B), и verify ловит порчу.

#include "note_id.h"
#include "zstorage.h"
#include "pieces.h"
#include "journal.h"

#include "test_util.h"

#include <vector>
#include "testdata.h"
#include "scratch_files.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <string>

using namespace zametti;

namespace {

QString g_base;

QString write(const QString& rel, const QByteArray& bytes) {
    const QString path = g_base + QLatin1Char('/') + rel;
    QDir().mkpath(QFileInfo(path).path());
    QFile f(path);
    if (f.open(QIODevice::WriteOnly)) f.write(bytes);
    f.close();
    return path;
}

std::string readAll(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    const QByteArray b = f.readAll();
    return std::string(b.constData(), size_t(b.size()));
}

// Набор ходит теми же глаголами, что программа и утилита; помощники ниже
// только переводят их в форму «корень + путь файла», в которой написан набор.
bool initStore(const QString& dir, QString* error) { return ZStorage(dir).init(error); }

QString newNote(const QString& root, const QString& parentId, QString* error) {
    ZStorage storage(root);
    const QString id = storage.createNote(parentId, false, error);
    return id.isEmpty() ? QString() : storage.pathOf(id);
}

QString importNote(const QString& root, const QString& parentId, const QString& source,
                   QString* error) {
    ZStorage storage(root);
    const QString id = storage.importNote(parentId, source, error);
    return id.isEmpty() ? QString() : storage.pathOf(id);
}

bool removeNote(const QString& root, const QString& id, QString* error) {
    ZStorage storage(root);
    storage.reload();
    return storage.remove(id, zametti::ImportLimits{}, error);
}

bool verifyStore(const QString& root, ZStorage::Report& report) {
    return ZStorage(root).verify(report);
}

// id заметки по строке отчёта "rel → id".
QString idFor(const ZStorage::Report& report, const QString& rel) {
    for (const QString& line : report.lines) {
        if (!line.startsWith(rel + QStringLiteral(" → "))) continue;
        QString id = line.mid(rel.size() + 3);
        const qsizetype space = id.indexOf(QLatin1Char(' '));
        return space >= 0 ? id.left(space) : id;
    }
    return {};
}

}  // namespace

static int ztRunSuite(int argc, char** argv) {
    (void)argc;
    (void)argv;
    g_base = QDir::tempPath() + QStringLiteral("/zametti-store-test");
    zt::dropTree(g_base);
    QDir().mkpath(g_base);

    // --- init ---------------------------------------------------------------
    {
        QString error;
        ZT_TRUE("init на свежем каталоге", initStore(g_base + "/пустое", &error));
        ZT_TRUE("появился .zametti", QDir(g_base + "/пустое/.zametti").exists());
        ZT_TRUE("появился .rescue", QDir(g_base + "/пустое/.rescue").exists());
        ZT_TRUE("появился history", QDir(g_base + "/пустое/history").exists());
        write(QStringLiteral("занятое/мусор.txt"), "x");
        ZT_TRUE("init в непустом отказывает",
                !initStore(g_base + "/занятое", &error) && !error.isEmpty());
    }

    // --- удаление заметки: надгробие остаётся навсегда ----------------------
    {
        const QString root = g_base + QStringLiteral("/удаление");
        QString error;
        ZT_TRUE("хранилище заведено", initStore(root, &error));
        const QString path = newNote(root, QString(), &error);
        const QString noteId = QFileInfo(path).completeBaseName();
        {
            // Свежая заметка — наша с первого байта: версия формата стоит
            // сразу, первой строкой шапки (refactor3).
            QFile fresh(path);
            ZT_TRUE("свежая заметка читается", fresh.open(QIODevice::ReadOnly));
            ZT_TRUE("и начинается с version: 1",
                    fresh.readAll().startsWith("<!-- zametti\nversion: 1\ncreated: "));
        }

        // История заметки: пара сохранений, как в жизни.
        ZStorage history(root);
        ZT_TRUE("первый слепок",
                history.appendToJournal(noteId, zametti::ZJournal::NewRecord::save(QByteArray("# заметка\n\nраз\n"), ZJournal::Stamp::at(1'700'000'000'000LL)), &error));
        ZT_TRUE("второй слепок",
                history.appendToJournal(noteId, zametti::ZJournal::NewRecord::save(QByteArray("# заметка\n\nраз\nдва\n"), ZJournal::Stamp::at(1'700'000'060'000LL)), &error));

        ZT_TRUE("заметка удалена", removeNote(root, noteId, &error));
        ZT_EQ("и без жалоб", std::string(), error.toStdString());
        ZT_TRUE("файла заметки больше нет", !QFileInfo::exists(path));

        ZJournal journal;
        ZT_TRUE("журнал на месте", history.readJournal(noteId, &journal, &error));
        // ДВЕ, а не три: надгробие гасит всё, кроме последнего слепка. Полная
        // история после удаления — мёртвый груз, а последнего состояния хватает,
        // чтобы заметку поднять.
        ZT_EQ("и в нём две записи: последний слепок и надгробие", std::string("2"),
              std::to_string(journal.size()));
        // Пустой журнал здесь — не «не сошлось число», а «журнал удалили
        // вместе с заметкой»; спрашивать у него последнюю запись нельзя.
        const bool haveRecords = !journal.isEmpty();
        ZT_TRUE("журнал не удалён вместе с заметкой", haveRecords);
        ZT_TRUE("последняя — надгробие",
                haveRecords && journal.at(journal.size() - 1).kind() == ZJournal::Kind::Tombstone);
        ZT_TRUE("у надгробия своего слепка нет",
                haveRecords && !journal.at(journal.size() - 1).hasSnapshot());

        // Главное обещание: по журналу удалённую заметку можно воскресить.
        QByteArray last;
        ZT_TRUE("предпоследний слепок достаётся",
                journal.size() >= 2 &&
                    history.journalSnapshot(noteId, int(journal.size()) - 2, &last, &error));
        ZT_EQ("и это её последнее содержимое", std::string("# заметка\n\nраз\nдва\n"),
              std::string(last.constData(), size_t(last.size())));

        ZT_TRUE("удалять несуществующую нельзя",
                !removeNote(root, QStringLiteral("нет-такой"), &error));
    }

    // --- verify: журналы ----------------------------------------------------
    //
    // Журнал не восстановим из файлов, поэтому проверяется всерьёз: каждый
    // слепок обязан собираться и сходиться с отпечатком.
    //
    // А вот достижимость вложений через прошлое с этапа 10 ОТМЕНЕНА: картинка,
    // которую видит только история, — сирота (решение владельца; иначе ни одна
    // картинка не покинула бы хранилище никогда, её видит прошлое).
    {
        const QString root = g_base + QStringLiteral("/проверка-журналов");
        QString error;
        ZT_TRUE("хранилище заведено", initStore(root, &error));
        const QString path = newNote(root, QString(), &error);
        const QString noteId = QFileInfo(path).completeBaseName();

        // Вложение, на которое ссылается ТОЛЬКО прошлое: в живой заметке
        // картинки уже нет.
        const QString picture = QStringLiteral("01n6cqevh7bbfr.webp");
        write(QStringLiteral("проверка-журналов/") + picture, "не картинка, но файл");

        ZStorage history(root);
        const QByteArray withPicture =
            QStringLiteral("<!-- zametti\ncreated: 2023-01-01T00:00:00Z\n-->\n\n"
                           "# было\n\n![вид](%1)\n").arg(picture).toUtf8();
        const QByteArray withoutPicture =
            QStringLiteral("<!-- zametti\ncreated: 2023-01-01T00:00:00Z\n-->\n\n"
                           "# стало\n\nбез картинки\n").toUtf8();
        ZT_TRUE("слепок с картинкой",
                history.appendToJournal(noteId, zametti::ZJournal::NewRecord::save(withPicture, ZJournal::Stamp::at(1'700'000'000'000LL)), &error));
        ZT_TRUE("слепок без картинки",
                history.appendToJournal(noteId, zametti::ZJournal::NewRecord::save(withoutPicture, ZJournal::Stamp::at(1'700'000'060'000LL)), &error));
        {
            QFile f(path);
            ZT_TRUE("живая заметка — без картинки", f.open(QIODevice::WriteOnly));
            f.write(withoutPicture);
        }

        ZStorage::Report report;
        const bool ok = verifyStore(root, report);
        ZT_TRUE("проверка проходит", ok);
        const QString all = report.lines.join(QLatin1Char('\n'));
        ZT_TRUE("журналы посчитаны", all.contains(QStringLiteral("journals: 1")));
        ZT_TRUE("вложение, видное только в прошлом, — сирота",
                all.contains(QStringLiteral("orphan attachment")));

        // Испорченный слепок обязан всплыть бедой, а не молчанием.
        {
            QFile f(history.journalPath(noteId));
            ZT_TRUE("журнал открылся", f.open(QIODevice::ReadWrite));
            QByteArray blob = f.readAll();
            blob[blob.size() - 4] = char(blob[blob.size() - 4] ^ 0x5a);
            f.seek(0);
            f.write(blob);
        }
        ZStorage::Report broken;
        ZT_TRUE("проверка видит порчу в журнале", !verifyStore(root, broken));
        ZT_TRUE("и называет журнал",
                broken.lines.join(QLatin1Char('\n')).contains(QStringLiteral("journal")));
    }

    // --- каскад картинок ------------------------------------------------------
    //
    // Корзинность вложения ВЫВОДИТСЯ: «в корзине» ⇔ все ссылающиеся заметки в
    // корзине. Поэтому перенос в корзину и восстановление над файлами не делают
    // ничего (инвариант D), а физическое расставание — ровно один момент,
    // очистка корзины (инвариант C).
    {
        const QString root = g_base + QStringLiteral("/каскад");
        QString error;
        ZT_TRUE("хранилище заведено", initStore(root, &error));

        const QString shared = QStringLiteral("01n6cqevh7bbf1.webp");
        const QString lonely = QStringLiteral("01n6cqevh7bbf2.webp");
        const QString mentioned = QStringLiteral("01n6cqevh7bbf3.webp");
        for (const QString& picture : {shared, lonely, mentioned})
            write(QStringLiteral("каскад/") + picture, "не картинка, но файл");

        const auto makeNote = [&](const QString& id, const QString& body) {
            write(QStringLiteral("каскад/") + id + QStringLiteral(".md"),
                  QStringLiteral("<!-- zametti\ncreated: 2023-01-01T00:00:00Z\n-->\n\n%1")
                      .arg(body)
                      .toUtf8());
        };
        // Первая и вторая делят одну картинку; у первой есть ещё своя.
        makeNote(QStringLiteral("01n6cqevaaaa01"),
                 QStringLiteral("# первая\n\n![вид](%1)\n\n![своя](%2)\n").arg(shared, lonely));
        makeNote(QStringLiteral("01n6cqevaaaa02"),
                 QStringLiteral("# вторая\n\n![вид](%1)\n").arg(shared));
        // Третья только УПОМИНАЕТ id в блоке кода — ложное срабатывание, и оно
        // обязано ошибаться в безопасную сторону.
        makeNote(QStringLiteral("01n6cqevaaaa03"),
                 QStringLiteral("# третья\n\n```\n%1\n```\n")
                     .arg(mentioned.left(mentioned.lastIndexOf(QLatin1Char('.')))));
        // Четвёртая ссылается на ту же «упомянутую» картинку по-настоящему.
        makeNote(QStringLiteral("01n6cqevaaaa04"),
                 QStringLiteral("# четвёртая\n\n![вид](%1)\n").arg(mentioned));

        // Очищаем ТОЛЬКО первую: общая картинка остаётся (её держит вторая), а
        // одинокая уходит.
        const ZStorage storage(root);
        QStringList doomed = storage.attachmentsLeavingWith({QStringLiteral("01n6cqevaaaa01")});
        ZT_EQ("с первой заметкой уходит одна картинка", std::string("1"),
              std::to_string(doomed.size()));
        ZT_TRUE("и это её собственная", doomed.contains(lonely));
        ZT_TRUE("общая остаётся: её держит вторая заметка", !doomed.contains(shared));

        // Очищаем обе — общая уходит следом.
        doomed = storage.attachmentsLeavingWith(
            {QStringLiteral("01n6cqevaaaa01"), QStringLiteral("01n6cqevaaaa02")});
        ZT_TRUE("вместе с обеими уходит и общая", doomed.contains(shared));
        ZT_TRUE("и одинокая", doomed.contains(lonely));

        // Ложное срабатывание: id текстом в кодовом блоке ЗАЩИЩАЕТ файл.
        doomed = storage.attachmentsLeavingWith({QStringLiteral("01n6cqevaaaa04")});
        ZT_TRUE("id, упомянутый текстом в чужой заметке, спасает картинку",
                !doomed.contains(mentioned));

        // Удаление файла вложения — в мусорку ОС, как и заметки.
        ZStorage remover(root);
        ZT_TRUE("вложение удаляется", remover.deleteAttachmentFile(lonely, &error));
        ZT_TRUE("и файла больше нет",
                !QFileInfo::exists(root + QLatin1Char('/') + lonely));
        ZT_TRUE("повторное удаление не беда", remover.deleteAttachmentFile(lonely, &error));
    }

    // --- verify: три категории вложений ---------------------------------------
    {
        const QString root = g_base + QStringLiteral("/категории");
        QString error;
        ZT_TRUE("хранилище заведено", initStore(root, &error));
        const QString alive = QStringLiteral("01n6cqevh7bbc1.webp");
        const QString inTrash = QStringLiteral("01n6cqevh7bbc2.webp");
        const QString orphan = QStringLiteral("01n6cqevh7bbc3.webp");
        for (const QString& picture : {alive, inTrash, orphan})
            write(QStringLiteral("категории/") + picture, "не картинка, но файл");

        const QString trashId = QStringLiteral("01n6cqevbbbb00");
        write(QStringLiteral("категории/") + trashId + QStringLiteral(".md"),
              QStringLiteral("<!-- zametti\ncreated: 2023-01-01T00:00:00Z\nrole: trash\n-->\n\n"
                             "# Корзина\n")
                  .toUtf8());
        write(QStringLiteral("категории/01n6cqevbbbb01.md"),
              QStringLiteral("<!-- zametti\ncreated: 2023-01-01T00:00:00Z\n-->\n\n"
                             "# живая\n\n![вид](%1)\n")
                  .arg(alive)
                  .toUtf8());
        write(QStringLiteral("категории/01n6cqevbbbb02.md"),
              QStringLiteral("<!-- zametti\ncreated: 2023-01-01T00:00:00Z\nparent: %1\n-->\n\n"
                             "# выброшенная\n\n![вид](%2)\n")
                  .arg(trashId, inTrash)
                  .toUtf8());

        ZStorage::Report v;
        ZT_TRUE("проверка проходит", verifyStore(root, v));
        const QString all = v.lines.join(QLatin1Char('\n'));
        ZT_TRUE("живая картинка молчит",
                !all.contains(QStringLiteral("orphan attachment: ") + alive) &&
                    !all.contains(QStringLiteral("attachment only in archive: ") + alive));
        ZT_TRUE("корзинная названа расписанием, а не бедой",
                all.contains(QStringLiteral("attachment only in archive: ") + inTrash));
        ZT_TRUE("сирота названа сиротой",
                all.contains(QStringLiteral("orphan attachment: ") + orphan));
    }

    // --- ИНВАРИАНТ D: картинки следуют за заметкой сами ------------------------
    //
    // Заметку отправили в корзину и вернули обратно — над файлами вложений не
    // сделано НИ ОДНОЙ операции, а «в корзине она или нет» каждый раз выводится
    // заново. Проверяем оба ответа и неприкосновенность файла между ними.
    {
        const QString root = g_base + QStringLiteral("/следуют");
        QString error;
        ZT_TRUE("хранилище заведено", initStore(root, &error));
        const QString picture = QStringLiteral("01n6cqevh7bbd1.webp");
        const QString file = write(QStringLiteral("следуют/") + picture, "не картинка, но файл");
        const QString trashId = QStringLiteral("01n6cqevcccc00");
        write(QStringLiteral("следуют/") + trashId + QStringLiteral(".md"),
              QStringLiteral("<!-- zametti\ncreated: 2023-01-01T00:00:00Z\nrole: trash\n-->\n\n"
                             "# Корзина\n")
                  .toUtf8());
        const QString notePath = QStringLiteral("следуют/01n6cqevcccc01.md");
        const auto writeNote = [&](const QString& parent) {
            write(notePath,
                  QStringLiteral("<!-- zametti\ncreated: 2023-01-01T00:00:00Z\n%1-->\n\n"
                                 "# заметка\n\n![вид](%2)\n")
                      .arg(parent.isEmpty() ? QString()
                                            : QStringLiteral("parent: %1\n").arg(parent),
                           picture)
                      .toUtf8());
        };

        writeNote(QString());
        const QDateTime touched = QFileInfo(file).lastModified();
        const qint64 size = QFileInfo(file).size();

        ZStorage::Report live;
        ZT_TRUE("проверка проходит", verifyStore(root, live));
        ZT_TRUE("у живой заметки картинка живая",
                !live.lines.join(QLatin1Char('\n')).contains(QStringLiteral("only in archive")));

        writeNote(trashId);   // «в корзину» — это правка одной строки меты
        ZStorage::Report trashed;
        ZT_TRUE("проверка проходит и с корзиной", verifyStore(root, trashed));
        ZT_TRUE("картинка уехала в корзину вместе с заметкой — сама",
                trashed.lines.join(QLatin1Char('\n'))
                    .contains(QStringLiteral("attachment only in archive: ") + picture));

        writeNote(QString());   // «восстановить» — та же правка обратно
        ZStorage::Report back;
        ZT_TRUE("проверка проходит после возврата", verifyStore(root, back));
        ZT_TRUE("и картинка вернулась вместе с заметкой",
                !back.lines.join(QLatin1Char('\n')).contains(QStringLiteral("only in archive")));

        ZT_TRUE("а файла вложения никто не касался",
                QFileInfo(file).lastModified() == touched && QFileInfo(file).size() == size);
    }

    // --- verify журналы НЕ ПЕРЕПИСЫВАЕТ ---------------------------------------
    //
    // Чистка истории пер-заметочная и ленивая; корпусный обход не имеет права
    // мигрировать журналы, иначе первый же verify стал бы той самой глобальной
    // утилитой через чёрный ход (решение владельца). Проверяем по ВСЕМ
    // журналам хранилища сразу: и по чищеному, и по старому.
    {
        const QString root = g_base + QStringLiteral("/verify-не-пишет");
        QString error;
        ZT_TRUE("хранилище заведено", initStore(root, &error));
        ZStorage history(root);
        const QByteArray text = "<!-- zametti\ncreated: 2023-01-01T00:00:00Z\n-->\n\n# раз\n";
        const QByteArray same = "<!-- zametti\ncreated: 2023-01-01T00:00:00Z\n"
                                "modified: 2023-01-02T00:00:00Z\n-->\n\n# раз\n";

        QStringList ids;
        for (int i = 0; i < 2; ++i) {
            const QString path = newNote(root, QString(), &error);
            const QString id = QFileInfo(path).completeBaseName();
            ids << id;
            history.appendToJournal(id, zametti::ZJournal::NewRecord::save(text, zametti::ZJournal::Stamp::at(1'700'000'000'000LL)), &error);
            history.appendToJournal(id, zametti::ZJournal::NewRecord::save(same, zametti::ZJournal::Stamp::at(1'700'000'060'000LL)), &error);
        }
        // Первый журнал делаем старым (v0): именно такому чистка и полагается —
        // но не от verify.
        {
            const QString path = history.journalPath(ids[0]);
            QFile file(path);
            ZT_TRUE("журнал открыт", file.open(QIODevice::ReadOnly));
            QByteArray blob = file.readAll();
            file.close();
            const QByteArray clean =
                ZJournal::headerBytes(QString::fromLatin1(ZJournal::kCleanVersion));
            blob = ZJournal::headerBytes(QString()) + blob.mid(clean.size());
            QFile out(path);
            ZT_TRUE("журнал переписан на старый лад",
                    out.open(QIODevice::WriteOnly | QIODevice::Truncate));
            out.write(blob);
        }

        struct Seen { QByteArray bytes; QDateTime when; };
        QHash<QString, Seen> before;
        for (const QString& id : ids) {
            QFile file(history.journalPath(id));
            ZT_TRUE("журнал читается", file.open(QIODevice::ReadOnly));
            before.insert(id, Seen{file.readAll(), QFileInfo(history.journalPath(id)).lastModified()});
        }

        ZStorage::Report v;
        ZT_TRUE("проверка проходит", verifyStore(root, v));

        bool untouched = true;
        for (const QString& id : ids) {
            QFile file(history.journalPath(id));
            if (!file.open(QIODevice::ReadOnly)) { untouched = false; continue; }
            untouched = untouched && file.readAll() == before[id].bytes &&
                        QFileInfo(history.journalPath(id)).lastModified() == before[id].when;
        }
        ZT_TRUE("ни один журнал не тронут: ни байтом, ни временем", untouched);

        ZJournal still;
        ZT_TRUE("старый журнал читается", history.readJournal(ids[0], &still, &error));
        ZT_EQ("и остался старым", std::string(), still.cleanVersion().toStdString());
        ZT_TRUE("с дубликатом внутри", still.size() == 2);
    }

    // --- verify: журнал без заметки -----------------------------------------
    {
        const QString root = g_base + QStringLiteral("/журнал-без-заметки");
        QString error;
        ZT_TRUE("хранилище заведено", initStore(root, &error));
        const QString path = newNote(root, QString(), &error);
        const QString noteId = QFileInfo(path).completeBaseName();
        ZStorage history(root);
        history.appendToJournal(noteId, zametti::ZJournal::NewRecord::save(QByteArray("<!-- zametti\ncreated: 2023-01-01T00:00:00Z\n-->\n\n"
                                  "# была\n"), ZJournal::Stamp::at(1'700'000'000'000LL)), &error);

        // Удалили как положено — надгробие есть, история осталась намеренно.
        ZT_TRUE("заметка удалена", removeNote(root, noteId, &error));
        ZStorage::Report buried;
        ZT_TRUE("проверка проходит", verifyStore(root, buried));
        ZT_TRUE("и надгробие названо нормой",
                buried.lines.join(QLatin1Char('\n'))
                    .contains(QStringLiteral("with tombstone")));

        // А теперь заметку унесли мимо программы: журнал есть, надгробия нет.
        const QString second = newNote(root, QString(), &error);
        const QString secondId = QFileInfo(second).completeBaseName();
        history.appendToJournal(secondId, zametti::ZJournal::NewRecord::save(QByteArray("<!-- zametti\ncreated: 2023-01-01T00:00:00Z\n-->\n\n"
                                  "# унесли\n"), zametti::ZJournal::Stamp::at(1'700'000'000'000LL)), &error);
        ZT_TRUE("файл унесён мимо программы", zt::dropFile(root, second));
        ZStorage::Report orphan;
        ZT_TRUE("проверка всё ещё проходит", verifyStore(root, orphan));
        ZT_TRUE("но про унесённую сказано",
                orphan.lines.join(QLatin1Char('\n'))
                    .contains(QStringLiteral("outside the app")));
    }

    // --- new ----------------------------------------------------------------
    {
        QString error;
        const QString path = newNote(g_base + "/пустое", QString(), &error);
        ZT_TRUE("new создал заметку", !path.isEmpty() && QFileInfo::exists(path));
        const std::string bytes = readAll(path);
        ZT_TRUE("в заметке каркас метаданных — канон без хвостовой пустой",
                bytes.rfind("<!-- zametti\nversion: 1\ncreated: ", 0) == 0 &&
                    bytes.compare(bytes.size() - 4, 4, "-->\n") == 0);
        const QString id = QFileInfo(path).completeBaseName();
        ZT_TRUE("имя — корректный id", isValidNoteId(id.toStdString()));

        const QString child =
            newNote(g_base + "/пустое", id, &error);
        ZT_TRUE("new с родителем", !child.isEmpty());
        ZT_TRUE("parent записан",
                readAll(child).find("parent: " + id.toStdString()) != std::string::npos);
        // Несуществующий родитель — В КОРЕНЬ, а не отказ (правило владельца для
        // Ctrl+N: «в архиве и в никуда ничего не создаётся — на глобальный
        // уровень»); утилита с явным --parent проверяет родителя сама, у двери.
        const QString stray =
            newNote(g_base + "/пустое", QStringLiteral("00000000000000"), &error);
        ZT_TRUE("new с несуществующим родителем кладёт в корень",
                !stray.isEmpty() && readAll(stray).find("parent:") == std::string::npos);
    }

    // --- импорт: синтетическое дерево ----------------------------------------
    // src/
    //   Верх.md           — заголовок, ссылка на Дом/Внутри.md, картинка, wikilink
    //   img/пик.png       — вложение (см. дедупликацию из Внутри.md)
    //   Дом/Внутри.md     — та же картинка другим путём, битая картинка
    // Настоящий однопиксельный PNG: пережатие требует настоящих байтов.
    const QByteArray pixel = QByteArray::fromBase64(
        "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAIAAACQd1PeAAAADElEQVR4nGO4"
        "IycHAALyARlzRAvLAAAAAElFTkSuQmCC");
    write(QStringLiteral("src/img/пик.png"), pixel);
    write(QStringLiteral("src/Верх.md"),
          QByteArray("# Верх\n\nСм. [внутри](Дом/Внутри.md) и ![пик](img/пик.png).\n\n"
                     "![[wiki-вложение.png|315]]\n"));
    write(QStringLiteral("src/Дом/Внутри.md"),
          QByteArray("Текст с ![пиком](../img/пик.png) и ![битой](нет-такой.png).\n"));

    const QByteArray manifest(
        "[{\"folder\": \"\", \"title\": \"Верх\","
        "  \"created\": \"2019-03-14T09:26:53Z\", \"modified\": \"2024-11-02T08:12:40Z\"},"
        " {\"folder\": \"Дом\", \"title\": \"Призрак\","
        "  \"created\": \"2020-01-01T00:00:00Z\", \"modified\": \"2020-01-01T00:00:00Z\"}]");
    const QString manifestPath = write(QStringLiteral("manifest.json"), manifest);

    const QString storeRoot = g_base + QStringLiteral("/хранилище");
    ZStorage::ImportOptions options;
    options.from = g_base + QStringLiteral("/src");
    options.appleManifest = manifestPath;

    // Сухой прогон ничего не создаёт.
    {
        ZStorage::ImportOptions dry = options;
        dry.dryRun = true;
        ZStorage::Report report;
        ZStorage(storeRoot).importTree(dry, report);
        ZT_TRUE("dry-run не создал хранилище", !QDir(storeRoot).exists());
    }

    ZStorage::Report report;
    const bool imported = ZStorage(storeRoot).importTree(options, report);

    const QString topId = idFor(report, QStringLiteral("Верх.md"));
    const QString dirId = idFor(report, QStringLiteral("Дом"));
    const QString innerId = idFor(report, QStringLiteral("Дом/Внутри.md"));
    ZT_TRUE("все трое получили id",
            !topId.isEmpty() && !dirId.isEmpty() && !innerId.isEmpty());

    // Битая картинка и запись манифеста без файла — беды в отчёте, поэтому
    // сам импорт не «зелёный»; но обе беды ожидаемые.
    ZT_TRUE("импорт с ожидаемыми бедами", !imported && report.problems == 2);
    ZT_TRUE("битая картинка в отчёте",
            report.lines.filter(QStringLiteral("нет-такой.png")).size() == 1);
    ZT_TRUE("манифест без файла в отчёте",
            report.lines.filter(QStringLiteral("Призрак")).size() == 1);
    ZT_TRUE("wikilink посчитан",
            report.lines.filter(QStringLiteral("wikilink")).size() == 1);

    // Времена из манифеста кодируются в id: 2019-03-14T09:26:53Z → 01e8m7jx.
    ZT_TRUE("created из манифеста в префиксе id",
            topId.startsWith(QStringLiteral("01e8m7jx")));

    const std::string top = readAll(storeRoot + "/" + topId + ".md");
    const std::string inner = readAll(storeRoot + "/" + innerId + ".md");
    const std::string dirNote = readAll(storeRoot + "/" + dirId + ".md");

    // Заголовки возвращаются всем; не дублируется только точное совпадение.
    // У Верха первый блок "# Верх" и title "Верх" — совпали, дубля нет.
    ZT_TRUE("совпавший заголовок не задублирован",
            top.find("# Верх\n") != std::string::npos &&
                top.find("# Верх\n\n# Верх") == std::string::npos);
    ZT_TRUE("заголовок возвращён из имени файла",
            inner.find("# Внутри\n") != std::string::npos);
    ZT_TRUE("метаданные Верха из манифеста",
            top.find("created: 2019-03-14T09:26:53Z") != std::string::npos &&
                top.find("modified: 2024-11-02T08:12:40Z") != std::string::npos);
    ZT_TRUE("Верх в корне — без parent", top.find("parent:") == std::string::npos);
    ZT_TRUE("Внутри — ребёнок Дома",
            inner.find("parent: " + dirId.toStdString()) != std::string::npos);
    ZT_TRUE("заметка-каталог с заголовком",
            dirNote.find("# Дом") != std::string::npos);

    ZT_TRUE("ссылка на .md переписана на id",
            top.find("(" + innerId.toStdString() + ".md)") != std::string::npos);
    ZT_TRUE("wikilink не переписан",
            top.find("![[wiki-вложение.png|315]]") != std::string::npos);

    // Вложение одно на двоих (дедупликация), лежит плоско под своим id и —
    // раз кодек стоит — пережато в webp без потерь.
    QStringList files;
    for (const QFileInfo& info : QDir(storeRoot).entryInfoList(QDir::Files)) {
        const QString name = info.fileName();
        // zametti.json — идентичность хранилища, а не вложение: она появляется
        // при заведении хранилища и к ввозу отношения не имеет.
        if (name == QLatin1String(ZStorage::Identity::kFile)) continue;
        if (!name.endsWith(QStringLiteral(".md"))) files.append(name);
    }
    ZT_TRUE("вложение ровно одно и плоско", files.size() == 1);
    if (files.size() == 1) {
        const std::string name = files.first().toStdString();
        ZT_TRUE("имя — id и .webp после пережатия",
                files.first().endsWith(QStringLiteral(".webp")) && name.size() == 19 &&
                    isValidNoteId(name.substr(0, 14)));
        ZT_TRUE("пережатое — действительно WebP",
                readAll(storeRoot + "/" + files.first()).compare(0, 4, "RIFF") == 0);
        ZT_TRUE("Верх ссылается на вложение",
                top.find("(" + name + ")") != std::string::npos);
        ZT_TRUE("Внутри ссылается на то же вложение",
                inner.find("(" + name + ")") != std::string::npos);
    }
    ZT_TRUE("битая ссылка осталась как есть",
            inner.find("(нет-такой.png)") != std::string::npos);
    ZT_TRUE("отчёт лежит рядом с хранилищем",
            QFileInfo::exists(storeRoot + QStringLiteral(".import-report.txt")));

    // --- verify: инвариант B — ноль замечаний, ноль дрейфа --------------------
    {
        ZStorage::Report v;
        ZT_TRUE("verify зелёный на свежем импорте", verifyStore(storeRoot, v));
        ZT_TRUE("сироты не найдены",
                v.lines.filter(QStringLiteral("orphan")).isEmpty());
    }

    // verify ловит порчу: чужой файл, битый parent, вложение не по хешу, сироту.
    {
        write(QStringLiteral("хранилище/чужак.txt"), "мимо");
        ZStorage::Report v;
        ZT_TRUE("чужой файл — беда", !verifyStore(storeRoot, v));
        zt::dropFile(storeRoot, storeRoot + QStringLiteral("/чужак.txt"));
    }
    {
        // Сирота с валидным id-именем — замечание, не беда.
        const QString orphan =
            storeRoot + QStringLiteral("/00000000000009.webp");
        write(QStringLiteral("хранилище/00000000000009.webp"), "RIFFxxxx");
        ZStorage::Report v;
        ZT_TRUE("сирота не беда", verifyStore(storeRoot, v));
        ZT_TRUE("но в отчёте", v.lines.filter(QStringLiteral("orphan")).size() == 1);
        zt::dropFile(storeRoot, orphan);
    }

    // --- импорт одиночных .md ------------------------------------------------
    {
        const QString root = g_base + QStringLiteral("/импорт");
        QString error;
        ZT_TRUE("хранилище под импорт заведено", initStore(root, &error));

        // Папка, в которую импортируем.
        const QString folderPath = newNote(root, QString(), &error);
        ZT_TRUE("папка заведена", !folderPath.isEmpty());
        const QString folderId = QFileInfo(folderPath).completeBaseName();

        // Чужой файл: без шапки, с неканоническим markdown.
        const QString source =
            write(QStringLiteral("чужие/Заметка.md"),
                  "Заголовок\n=========\n\n*  пункт\n*  второй\n\nтекст с __жирным__\n");
        const QString made = importNote(root, folderId, source, &error);
        ZT_TRUE("импорт прошёл", !made.isEmpty());
        ZT_TRUE("источник на месте и не тронут",
                readAll(source) ==
                    "Заголовок\n=========\n\n*  пункт\n*  второй\n\nтекст с __жирным__\n");
        ZT_TRUE("имя файла — свежий id",
                isValidNoteId(QFileInfo(made).completeBaseName().toStdString()) &&
                    QFileInfo(made).completeBaseName() != folderId);

        const std::string written = readAll(made);
        const zametti::ZNote doc = noteOf(written);
        ZT_TRUE("шапка на месте", doc.hasHeader());
        ZT_EQ("родитель проставлен", folderId.toStdString(), head(doc, "parent"));
        ZT_TRUE("времена проставлены",
                !head(doc, "created").empty() && !head(doc, "modified").empty());
        ZT_TRUE("role не появился", head(doc, "role").empty());
        // Канон: setext-заголовок стал ATX, звёздочки — дефисами, __ — **.
        ZT_TRUE("содержимое канонизировано",
                written.find("# Заголовок") != std::string::npos &&
                    written.find("- пункт") != std::string::npos &&
                    written.find("**жирным**") != std::string::npos);
        ZT_EQ("и дрейфа нет", written, noteOf(written).toMarkdown());

        // Файл из другого хранилища: id и role не наследуются, время СОЗДАНИЯ
        // берётся из шапки, а время правки — сегодняшнее.
        //
        // Раньше из шапки брались оба, и проверка ниже стерегла именно это.
        // Владелец наткнулся на цену такого решения: привезённая заметка со
        // старой датой уходит в самый низ средней колонки, отсортированной по
        // дате правки, и найти её нельзя — «непонятно куда она импортируется».
        // Привоз и есть правка ЭТОГО хранилища; хронология источника при этом
        // цела, она в created.
        const QString exported =
            write(QStringLiteral("чужие/Вывезенная.md"),
                  "<!-- zametti\nid: 00000000000042\nrole: folder\n"
                  "parent: 0000000000000z\ncreated: 2019-03-14T09:26:53Z\n"
                  "modified: 2020-01-02T03:04:05Z\nx-своё: беречь\n-->\n\n# Вывезенная\n");
        const QString second = importNote(root, QString(), exported, &error);
        ZT_TRUE("второй импорт прошёл", !second.isEmpty());
        const zametti::ZNote back = noteOf(readAll(second));
        ZT_TRUE("чужой id не унаследован", head(back, "id").empty());
        ZT_TRUE("чужой role снят", head(back, "role").empty());
        ZT_TRUE("в корень — родителя нет", head(back, "parent").empty());
        ZT_EQ("время создания взято из шапки", "2019-03-14T09:26:53Z", head(back, "created"));
        ZT_TRUE("а время правки — сегодняшнее, а не из шапки",
                head(back, "modified") != "2020-01-02T03:04:05Z" &&
                    head(back, "modified").rfind(
                        // МЕСТНАЯ дата, а не UTC: времена мы пишем с офсетом
                        // (этап 15), и сразу после полуночи по местному времени
                        // UTC-дата ещё вчерашняя. Проверка краснела ровно в
                        // этот час — не от кода, а от собственной ошибки.
                        QDateTime::currentDateTime()
                            .toString(QStringLiteral("yyyy-MM-dd"))
                            .toStdString(),
                        0) == 0);
        ZT_EQ("неизвестный ключ уцелел", "беречь", head(back, "x-своё"));

        // Пустой файл: пустая строка после "-->" дала бы дрейф.
        const QString empty = write(QStringLiteral("чужие/Пустая.md"), "");
        const QString third = importNote(root, QString(), empty, &error);
        ZT_TRUE("пустой файл импортируется", !third.isEmpty());
        ZT_EQ("и без дрейфа", readAll(third), noteOf(readAll(third)).toMarkdown());

        // Отказы.
        ZT_TRUE("несуществующий источник — отказ",
                importNote(root, QString(), g_base + QStringLiteral("/нет.md"), &error)
                        .isEmpty() &&
                    !error.isEmpty());
        ZT_TRUE("несуществующая папка — отказ",
                importNote(root, QStringLiteral("00000000000001"), source, &error)
                    .isEmpty());

        ZStorage::Report v;
        ZT_TRUE("хранилище после импорта проходит проверку", verifyStore(root, v));
    }

    zt::dropTree(g_base);
    return zt::report("хранилище");
}

// Набор целиком одним TEST: тело не тронуто, argv ему собран здесь.
// Дробить на отдельные проверки — отдельная работа, по одному набору.
TEST(Store, All) {
    std::vector<QByteArray> ztArgs{QByteArrayLiteral("store_test")};
    std::vector<char*> ztArgv;
    for (QByteArray& a : ztArgs) ztArgv.push_back(a.data());
    EXPECT_EQ(0, ztRunSuite(int(ztArgv.size()), ztArgv.data()));
}
