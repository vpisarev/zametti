// Утилита хранилища: init, new, импорт синтетического дерева (иерархия,
// времена из манифеста, переписанные и непереписанные ссылки, вложения с
// дедупликацией, отчёт), verify на свежемигрированном — ноль замечаний
// (инвариант B), и verify ловит порчу.

#include "note_id.h"
#include "parser.h"
#include "store.h"

#include "test_util.h"

#include <QCoreApplication>
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

    QDir(g_base).removeRecursively();
    return zt::report("хранилище");
}
