// Утилита хранилища: init, new, импорт синтетического дерева (иерархия,
// времена из манифеста, переписанные и непереписанные ссылки, вложения с
// дедупликацией, отчёт), verify на свежемигрированном — ноль замечаний
// (инвариант B), и verify ловит порчу.

#include "note_id.h"
#include "parser.h"
#include "serializer.h"
#include "journal.h"
#include "store.h"

#include "test_util.h"

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

// id заметки по строке отчёта "rel → id".
QString idFor(const store::Report& report, const QString& rel) {
    for (const QString& line : report.lines) {
        if (!line.startsWith(rel + QStringLiteral(" → "))) continue;
        QString id = line.mid(rel.size() + 3);
        const qsizetype space = id.indexOf(QLatin1Char(' '));
        return space >= 0 ? id.left(space) : id;
    }
    return {};
}

}  // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    g_base = QDir::tempPath() + QStringLiteral("/zametti-store-test");
    QDir(g_base).removeRecursively();
    QDir().mkpath(g_base);

    // --- init ---------------------------------------------------------------
    {
        QString error;
        ZT_TRUE("init на свежем каталоге", store::initStore(g_base + "/пустое", &error));
        ZT_TRUE("появился .zametti", QDir(g_base + "/пустое/.zametti").exists());
        ZT_TRUE("появился .rescue", QDir(g_base + "/пустое/.rescue").exists());
        ZT_TRUE("появился history", QDir(g_base + "/пустое/history").exists());
        write(QStringLiteral("занятое/мусор.txt"), "x");
        ZT_TRUE("init в непустом отказывает",
                !store::initStore(g_base + "/занятое", &error) && !error.isEmpty());
    }

    // --- удаление заметки: надгробие остаётся навсегда ----------------------
    {
        const QString root = g_base + QStringLiteral("/удаление");
        QString error;
        ZT_TRUE("хранилище заведено", store::initStore(root, &error));
        const QString path = store::newNote(root, QString(), &error);
        const QString noteId = QFileInfo(path).completeBaseName();

        // История заметки: пара сохранений, как в жизни.
        journal::History history(root);
        ZT_TRUE("первый слепок",
                history.append(noteId, journal::Kind::Save, 1'700'000'000'000LL,
                               QByteArray("# заметка\n\nраз\n"), 0, &error));
        ZT_TRUE("второй слепок",
                history.append(noteId, journal::Kind::Save, 1'700'000'060'000LL,
                               QByteArray("# заметка\n\nраз\nдва\n"), 0, &error));

        ZT_TRUE("заметка удалена", store::deleteNoteFile(root, noteId, &error));
        ZT_EQ("и без жалоб", std::string(), error.toStdString());
        ZT_TRUE("файла заметки больше нет", !QFileInfo::exists(path));

        journal::Journal journal;
        ZT_TRUE("журнал на месте", history.read(noteId, &journal, &error));
        ZT_EQ("и в нём три записи", std::string("3"),
              std::to_string(journal.entries.size()));
        // Пустой журнал здесь — не «не сошлось число», а «журнал удалили
        // вместе с заметкой»; спрашивать у него последнюю запись нельзя.
        const bool haveRecords = !journal.entries.isEmpty();
        ZT_TRUE("журнал не удалён вместе с заметкой", haveRecords);
        ZT_TRUE("последняя — надгробие",
                haveRecords && journal.entries.last().kind == journal::Kind::Tombstone);
        ZT_TRUE("у надгробия своего слепка нет",
                haveRecords && !journal.entries.last().hasSnapshot());

        // Главное обещание: по журналу удалённую заметку можно воскресить.
        QByteArray last;
        ZT_TRUE("предпоследний слепок достаётся",
                journal.entries.size() >= 2 &&
                    history.snapshotAt(noteId, int(journal.entries.size()) - 2, &last, &error));
        ZT_EQ("и это её последнее содержимое", std::string("# заметка\n\nраз\nдва\n"),
              std::string(last.constData(), size_t(last.size())));

        ZT_TRUE("удалять несуществующую нельзя",
                !store::deleteNoteFile(root, QStringLiteral("нет-такой"), &error));
    }

    // --- verify: журналы ----------------------------------------------------
    //
    // Журнал не восстановим из файлов, поэтому проверяется всерьёз: каждый
    // слепок обязан собираться и сходиться с отпечатком. И отдельно —
    // достижимость вложений через прошлое: картинка, видная хоть в одной
    // версии истории, не сирота.
    {
        const QString root = g_base + QStringLiteral("/проверка-журналов");
        QString error;
        ZT_TRUE("хранилище заведено", store::initStore(root, &error));
        const QString path = store::newNote(root, QString(), &error);
        const QString noteId = QFileInfo(path).completeBaseName();

        // Вложение, на которое ссылается ТОЛЬКО прошлое: в живой заметке
        // картинки уже нет.
        const QString picture = QStringLiteral("01n6cqevh7bbfr.webp");
        write(QStringLiteral("проверка-журналов/") + picture, "не картинка, но файл");

        journal::History history(root);
        const QByteArray withPicture =
            QStringLiteral("<!-- zametti\ncreated: 2023-01-01T00:00:00Z\n-->\n\n"
                           "# было\n\n![вид](%1)\n").arg(picture).toUtf8();
        const QByteArray withoutPicture =
            QStringLiteral("<!-- zametti\ncreated: 2023-01-01T00:00:00Z\n-->\n\n"
                           "# стало\n\nбез картинки\n").toUtf8();
        ZT_TRUE("слепок с картинкой",
                history.append(noteId, journal::Kind::Save, 1'700'000'000'000LL, withPicture, 0,
                               &error));
        ZT_TRUE("слепок без картинки",
                history.append(noteId, journal::Kind::Save, 1'700'000'060'000LL, withoutPicture,
                               0, &error));
        {
            QFile f(path);
            ZT_TRUE("живая заметка — без картинки", f.open(QIODevice::WriteOnly));
            f.write(withoutPicture);
        }

        store::Report report;
        const bool ok = store::verifyStore(root, report);
        ZT_TRUE("проверка проходит", ok);
        const QString all = report.lines.join(QLatin1Char('\n'));
        ZT_TRUE("журналы посчитаны", all.contains(QStringLiteral("журналов: 1")));
        ZT_TRUE("вложение, видное только в прошлом, не сирота",
                !all.contains(QStringLiteral("осиротевшее вложение")));

        // Испорченный слепок обязан всплыть бедой, а не молчанием.
        {
            QFile f(history.pathFor(noteId));
            ZT_TRUE("журнал открылся", f.open(QIODevice::ReadWrite));
            QByteArray blob = f.readAll();
            blob[blob.size() - 4] = char(blob[blob.size() - 4] ^ 0x5a);
            f.seek(0);
            f.write(blob);
        }
        store::Report broken;
        ZT_TRUE("проверка видит порчу в журнале", !store::verifyStore(root, broken));
        ZT_TRUE("и называет журнал",
                broken.lines.join(QLatin1Char('\n')).contains(QStringLiteral("журнал")));
    }

    // --- verify: журнал без заметки -----------------------------------------
    {
        const QString root = g_base + QStringLiteral("/журнал-без-заметки");
        QString error;
        ZT_TRUE("хранилище заведено", store::initStore(root, &error));
        const QString path = store::newNote(root, QString(), &error);
        const QString noteId = QFileInfo(path).completeBaseName();
        journal::History history(root);
        history.append(noteId, journal::Kind::Save, 1'700'000'000'000LL,
                       QByteArray("<!-- zametti\ncreated: 2023-01-01T00:00:00Z\n-->\n\n"
                                  "# была\n"),
                       0, &error);

        // Удалили как положено — надгробие есть, история осталась намеренно.
        ZT_TRUE("заметка удалена", store::deleteNoteFile(root, noteId, &error));
        store::Report buried;
        ZT_TRUE("проверка проходит", store::verifyStore(root, buried));
        ZT_TRUE("и надгробие названо нормой",
                buried.lines.join(QLatin1Char('\n'))
                    .contains(QStringLiteral("с надгробием")));

        // А теперь заметку унесли мимо программы: журнал есть, надгробия нет.
        const QString second = store::newNote(root, QString(), &error);
        const QString secondId = QFileInfo(second).completeBaseName();
        history.append(secondId, journal::Kind::Save, 1'700'000'000'000LL,
                       QByteArray("<!-- zametti\ncreated: 2023-01-01T00:00:00Z\n-->\n\n"
                                  "# унесли\n"),
                       0, &error);
        ZT_TRUE("файл унесён мимо программы", QFile::remove(second));
        store::Report orphan;
        ZT_TRUE("проверка всё ещё проходит", store::verifyStore(root, orphan));
        ZT_TRUE("но про унесённую сказано",
                orphan.lines.join(QLatin1Char('\n'))
                    .contains(QStringLiteral("мимо программы")));
    }

    // --- new ----------------------------------------------------------------
    {
        QString error;
        const QString path = store::newNote(g_base + "/пустое", QString(), &error);
        ZT_TRUE("new создал заметку", !path.isEmpty() && QFileInfo::exists(path));
        const std::string bytes = readAll(path);
        ZT_TRUE("в заметке каркас метаданных — канон без хвостовой пустой",
                bytes.rfind("<!-- zametti\ncreated: ", 0) == 0 &&
                    bytes.compare(bytes.size() - 4, 4, "-->\n") == 0);
        const QString id = QFileInfo(path).completeBaseName();
        ZT_TRUE("имя — корректный id", isValidNoteId(id.toStdString()));

        const QString child =
            store::newNote(g_base + "/пустое", id, &error);
        ZT_TRUE("new с родителем", !child.isEmpty());
        ZT_TRUE("parent записан",
                readAll(child).find("parent: " + id.toStdString()) != std::string::npos);
        ZT_TRUE("new с несуществующим родителем отказывает",
                store::newNote(g_base + "/пустое", QStringLiteral("00000000000000"),
                               &error)
                    .isEmpty());
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

    store::ImportOptions options;
    options.root = g_base + QStringLiteral("/хранилище");
    options.from = g_base + QStringLiteral("/src");
    options.appleManifest = manifestPath;

    // Сухой прогон ничего не создаёт.
    {
        store::ImportOptions dry = options;
        dry.dryRun = true;
        store::Report report;
        store::importTree(dry, report);
        ZT_TRUE("dry-run не создал хранилище", !QDir(options.root).exists());
    }

    store::Report report;
    const bool imported = store::importTree(options, report);

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

    const std::string top = readAll(options.root + "/" + topId + ".md");
    const std::string inner = readAll(options.root + "/" + innerId + ".md");
    const std::string dirNote = readAll(options.root + "/" + dirId + ".md");

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
    for (const QFileInfo& info : QDir(options.root).entryInfoList(QDir::Files))
        if (!info.fileName().endsWith(QStringLiteral(".md"))) files.append(info.fileName());
    ZT_TRUE("вложение ровно одно и плоско", files.size() == 1);
    if (files.size() == 1) {
        const std::string name = files.first().toStdString();
        ZT_TRUE("имя — id и .webp после пережатия",
                files.first().endsWith(QStringLiteral(".webp")) && name.size() == 19 &&
                    isValidNoteId(name.substr(0, 14)));
        ZT_TRUE("пережатое — действительно WebP",
                readAll(options.root + "/" + files.first()).compare(0, 4, "RIFF") == 0);
        ZT_TRUE("Верх ссылается на вложение",
                top.find("(" + name + ")") != std::string::npos);
        ZT_TRUE("Внутри ссылается на то же вложение",
                inner.find("(" + name + ")") != std::string::npos);
    }
    ZT_TRUE("битая ссылка осталась как есть",
            inner.find("(нет-такой.png)") != std::string::npos);
    ZT_TRUE("отчёт лежит рядом с хранилищем",
            QFileInfo::exists(options.root + QStringLiteral(".import-report.txt")));

    // --- verify: инвариант B — ноль замечаний, ноль дрейфа --------------------
    {
        store::Report v;
        ZT_TRUE("verify зелёный на свежем импорте", store::verifyStore(options.root, v));
        ZT_TRUE("сироты не найдены",
                v.lines.filter(QStringLiteral("осиротев")).isEmpty());
    }

    // verify ловит порчу: чужой файл, битый parent, вложение не по хешу, сироту.
    {
        write(QStringLiteral("хранилище/чужак.txt"), "мимо");
        store::Report v;
        ZT_TRUE("чужой файл — беда", !store::verifyStore(options.root, v));
        QFile::remove(options.root + QStringLiteral("/чужак.txt"));
    }
    {
        // Сирота с валидным id-именем — замечание, не беда.
        const QString orphan =
            options.root + QStringLiteral("/00000000000009.webp");
        write(QStringLiteral("хранилище/00000000000009.webp"), "RIFFxxxx");
        store::Report v;
        ZT_TRUE("сирота не беда", store::verifyStore(options.root, v));
        ZT_TRUE("но в отчёте", v.lines.filter(QStringLiteral("осиротев")).size() == 1);
        QFile::remove(orphan);
    }

    // --- импорт одиночных .md ------------------------------------------------
    {
        const QString root = g_base + QStringLiteral("/импорт");
        QString error;
        ZT_TRUE("хранилище под импорт заведено", store::initStore(root, &error));

        // Папка, в которую импортируем.
        const QString folderPath = store::newNote(root, QString(), &error);
        ZT_TRUE("папка заведена", !folderPath.isEmpty());
        const QString folderId = QFileInfo(folderPath).completeBaseName();

        // Чужой файл: без шапки, с неканоническим markdown.
        const QString source =
            write(QStringLiteral("чужие/Заметка.md"),
                  "Заголовок\n=========\n\n*  пункт\n*  второй\n\nтекст с __жирным__\n");
        const QString made = store::importNote(root, folderId, source, &error);
        ZT_TRUE("импорт прошёл", !made.isEmpty());
        ZT_TRUE("источник на месте и не тронут",
                readAll(source) ==
                    "Заголовок\n=========\n\n*  пункт\n*  второй\n\nтекст с __жирным__\n");
        ZT_TRUE("имя файла — свежий id",
                isValidNoteId(QFileInfo(made).completeBaseName().toStdString()) &&
                    QFileInfo(made).completeBaseName() != folderId);

        const std::string written = readAll(made);
        const Document doc = parse(written);
        ZT_TRUE("шапка на месте", doc.meta.present);
        ZT_EQ("родитель проставлен", folderId.toStdString(), doc.meta.get("parent"));
        ZT_TRUE("времена проставлены",
                !doc.meta.get("created").empty() && !doc.meta.get("modified").empty());
        ZT_TRUE("role не появился", doc.meta.get("role").empty());
        // Канон: setext-заголовок стал ATX, звёздочки — дефисами, __ — **.
        ZT_TRUE("содержимое канонизировано",
                written.find("# Заголовок") != std::string::npos &&
                    written.find("- пункт") != std::string::npos &&
                    written.find("**жирным**") != std::string::npos);
        ZT_EQ("и дрейфа нет", written, serialize(parse(written)));

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
        const QString second = store::importNote(root, QString(), exported, &error);
        ZT_TRUE("второй импорт прошёл", !second.isEmpty());
        const Document back = parse(readAll(second));
        ZT_TRUE("чужой id не унаследован", back.meta.get("id").empty());
        ZT_TRUE("чужой role снят", back.meta.get("role").empty());
        ZT_TRUE("в корень — родителя нет", back.meta.get("parent").empty());
        ZT_EQ("время создания взято из шапки", "2019-03-14T09:26:53Z", back.meta.get("created"));
        ZT_TRUE("а время правки — сегодняшнее, а не из шапки",
                back.meta.get("modified") != "2020-01-02T03:04:05Z" &&
                    back.meta.get("modified").rfind(
                        QDateTime::currentDateTimeUtc()
                            .toString(QStringLiteral("yyyy-MM-dd"))
                            .toStdString(),
                        0) == 0);
        ZT_EQ("неизвестный ключ уцелел", "беречь", back.meta.get("x-своё"));

        // Пустой файл: пустая строка после "-->" дала бы дрейф.
        const QString empty = write(QStringLiteral("чужие/Пустая.md"), "");
        const QString third = store::importNote(root, QString(), empty, &error);
        ZT_TRUE("пустой файл импортируется", !third.isEmpty());
        ZT_EQ("и без дрейфа", readAll(third), serialize(parse(readAll(third))));

        // Отказы.
        ZT_TRUE("несуществующий источник — отказ",
                store::importNote(root, QString(), g_base + QStringLiteral("/нет.md"), &error)
                        .isEmpty() &&
                    !error.isEmpty());
        ZT_TRUE("несуществующая папка — отказ",
                store::importNote(root, QStringLiteral("00000000000001"), source, &error)
                    .isEmpty());

        store::Report v;
        ZT_TRUE("хранилище после импорта проходит проверку", store::verifyStore(root, v));
    }

    QDir(g_base).removeRecursively();
    return zt::report("хранилище");
}
