// Сохранение: инвариант B этапа и последний рубеж перед заменой файла.
//
// B. открыть файл и сохранить без правок == serialize(parse(file)),
//    для уже канонических файлов — побайтовая идентичность.
//
// И отдельно — то, ради чего самопроверка вообще есть: если читатель сломан,
// старый файл обязан остаться нетронутым.

#include "document_builder.h"
#include "document_reader.h"
#include "document_saver.h"
#include "parser.h"
#include "serializer.h"
#include "test_util.h"

#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QTextDocument>

#include <string>

namespace {

QString g_dir;

std::string readFile(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    const QByteArray data = file.readAll();
    return std::string(data.constData(), static_cast<size_t>(data.size()));
}

bool writeFile(const QString& path, const std::string& text) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    return file.write(text.data(), static_cast<qsizetype>(text.size())) ==
           qsizetype(text.size());
}

void check(bool ok, const std::string& what) {
    ++zt::g_checks;
    if (ok) return;
    ++zt::g_failures;
    std::printf("провал: %s\n", what.c_str());
}

void checkEqual(const std::string& expected, const std::string& actual,
                const std::string& what) {
    ++zt::g_checks;
    if (expected == actual) return;
    ++zt::g_failures;
    std::printf("провал: %s\n%s", what.c_str(), zt::diff(expected, actual).c_str());
}

QString pathFor(const char* name) { return g_dir + QLatin1Char('/') + QLatin1String(name); }

// Документ, собранный из файла, — ровно то, что видит пользователь после
// открытия заметки.
void buildFrom(const std::string& source, QTextDocument& doc) {
    zametti::buildDocument(zametti::parse(source), doc);
}

// Инвариант B на одном исходнике.
void checkSave(const std::string& source, const char* name) {
    const QString path = pathFor(name);
    check(writeFile(path, source), "не записать исходник");

    QTextDocument doc;
    buildFrom(source, doc);

    const zametti::SaveOutcome first =
        zametti::saveDocument(doc, path, QStringLiteral("test"));
    const std::string canonical = zametti::serialize(zametti::parse(source));

    if (source == canonical) {
        check(first.result == zametti::SaveResult::Unchanged,
              std::string(name) + ": канонический файл не должен переписываться");
    } else {
        check(first.result == zametti::SaveResult::Written,
              std::string(name) + ": файл должен был обновиться");
    }
    checkEqual(canonical, readFile(path), std::string(name) + ": содержимое после записи");

    // Повторное сохранение того же документа не трогает файл вовсе.
    const zametti::SaveOutcome second =
        zametti::saveDocument(doc, path, QStringLiteral("test"));
    check(second.result == zametti::SaveResult::Unchanged,
          std::string(name) + ": повторное сохранение должно быть пустой операцией");
}

const char* const kSources[] = {
    "# заголовок\n\nабзац с **жирным** и `кодом`\n",
    "- буллет\n- второй\n  - вложенный\n",
    "- [ ] не сделано\n- [x] сделано\n",
    "```py\nx = 1\n```\n",
    "| a | b |\n|---|---|\n| 1 | 2 |\n",
    "текст с чужим разделителем\n",
    "текст с разделителем абзацев\n",
    "",
};

// Неканонические исходники: сохранение обязано привести их к канону, но не
// потерять ни байта смысла.
const char* const kNonCanonical[] = {
    "#   заголовок с лишними пробелами\n",
    "*   буллет через много пробелов\n",
    "- [X] отмечено заглавной\n",
    "текст\n\n\n\nчерез три пустых строки\n",
};

// Испорченный читатель: отдаёт заголовок с переводом строки внутри. Такой IR
// сериализуется в две строки, а разбирается обратно в два блока — самопроверка
// обязана это поймать.
zametti::Document brokenReader(const QTextDocument&) {
    zametti::Block b;
    b.kind = zametti::Kind::Heading;
    b.headingLevel = 1;
    b.text = "первая\nвторая";
    return {b};
}

void checkRescue() {
    const QString path = pathFor("rescue.md");
    const std::string source = "# заголовок\n\nтекст\n";
    check(writeFile(path, source), "не записать исходник для аварийного случая");

    QTextDocument doc;
    buildFrom(source, doc);

    const zametti::SaveOutcome outcome =
        zametti::saveDocument(doc, path, QStringLiteral("stamp"), brokenReader);

    check(outcome.result == zametti::SaveResult::Rescued,
          "испорченный читатель должен приводить к аварийному сохранению");
    checkEqual(source, readFile(path), "исходный файл обязан остаться нетронутым");

    const QString rescuePath = path + QStringLiteral(".rescue-stamp");
    check(QFile::exists(rescuePath), "аварийный файл не создан");
    checkEqual("# первая\nвторая\n", readFile(rescuePath), "содержимое аварийного файла");
}

// Записать некуда — старый файл всё равно цел.
void checkFailure() {
    const QString path = g_dir + QStringLiteral("/нет-такого-каталога/файл.md");
    QTextDocument doc;
    buildFrom("текст\n", doc);
    const zametti::SaveOutcome outcome =
        zametti::saveDocument(doc, path, QStringLiteral("stamp"));
    check(outcome.result == zametti::SaveResult::Failed,
          "запись в несуществующий каталог должна проваливаться");
    check(!outcome.message.isEmpty(), "у провала должно быть человеческое объяснение");
}

}  // namespace

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    if (argc < 2) {
        std::printf("использование: save_test <каталог для временных файлов>\n");
        return 2;
    }

    // Не "save_test": по этому имени в каталоге сборки уже лежит сам бинарник.
    g_dir = QString::fromLocal8Bit(argv[1]) + QStringLiteral("/save-data");
    QDir(g_dir).removeRecursively();
    if (!QDir().mkpath(g_dir)) {
        std::printf("не создать каталог %s\n", g_dir.toUtf8().constData());
        return 2;
    }

    int n = 0;
    for (const char* source : kSources)
        checkSave(source, (std::string("case") + std::to_string(n++) + ".md").c_str());
    for (const char* source : kNonCanonical)
        checkSave(source, (std::string("nc") + std::to_string(n++) + ".md").c_str());

    checkRescue();
    checkFailure();

    std::printf("проверок %d, провалов %d\n", zt::g_checks, zt::g_failures);
    return zt::g_failures == 0 ? 0 : 1;
}
