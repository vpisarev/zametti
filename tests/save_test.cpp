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
#include "editor_ops.h"
#include "parser.h"
#include "serializer.h"
#include "test_util.h"

#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QTextBlock>
#include <QTextCursor>
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
    // Пробел на краю не терпит только начертание. Внутри жирного он не край, а
    // середина — жирный кусок со встроенным кодом лежит у нас двумя спанами, и
    // поджатие стыка разрывало его надвое: открытие файла переписывало его без
    // единой правки. Встроенный код и ссылка пробел по краям терпят сами.
    "**Ядро `zametti-core`** — ядро.\n",
    "текст `[x] ` ещё\n",
    "текст [ так ](/url) ещё\n",
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

// Испорченный читатель: выдаёт за дословный кусок то, что дословным куском не
// является. Строка с палочками без строки-разделителя — не таблица, разбор
// вернёт обычный абзац, и самопроверка обязана это поймать.
//
// Прежние поломки перестали годиться: заголовок с переводом строки внутри
// сводится в одну строку перед записью, заголовок девятого уровня ядро считает
// недопустимым IR и падает на проверке, не дойдя до самопроверки, а название
// языка с обратной кавычкой сериализатор сам выводит забором из волнистых
// черт — и круг сходится.
// Литералы IR строятся билдером: блок без своей арены — набор смещений в
// никуда. Строитель держит документ и раздаёт ссылки на блоки в нём.
struct Builder {
    zametti::Document ir;

    zametti::Block& add(zametti::Kind kind, std::string_view text) {
        ir.blocks.push_back(ir.newBlock(kind, text));
        return ir.blocks.back();
    }
    zametti::Block& addRaw(std::string_view bytes) {
        ir.blocks.push_back(ir.newRaw(bytes));
        return ir.blocks.back();
    }
    // Спаны блока обязаны лежать в spans подряд, поэтому размечать блок надо
    // до того, как заведён следующий.
    void mark(zametti::Block& block, int from, int length, zametti::InlineFlag flag) {
        zametti::Inline span;
        span.text = {from, from + length};
        span.set(flag, true);
        const int32_t at = static_cast<int32_t>(ir.spans.size());
        ir.spans.push_back(span);
        if (block.inlines.empty()) block.inlines = {at, at + 1};
        else block.inlines.end = at + 1;
    }
};

zametti::Document brokenReader(const QTextDocument&) {
    Builder b;
    b.addRaw("| это не таблица |\n");
    return std::move(b.ir);
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
    checkEqual("| это не таблица |\n", readFile(rescuePath),
               "содержимое аварийного файла");
}

// Пустой абзац markdown выразить нечем, а Enter его заводит. Без уборки
// самопроверка ловила бы расхождение при каждом сохранении, и вместо записи
// появлялся бы аварийный файл — ровно это и случилось на живой заметке.
void checkEmptyParagraphs() {
    const QString path = pathFor("пустые.md");
    check(writeFile(path, "текст\n"), "не записать исходник");

    QTextDocument doc;
    buildFrom("текст\n", doc);

    // Enter в конце: в документе появляется пустой абзац.
    QTextCursor cursor(&doc);
    cursor.movePosition(QTextCursor::End);
    cursor.insertBlock();

    const zametti::SaveOutcome outcome =
        zametti::saveDocument(doc, path, QStringLiteral("test"));
    check(outcome.result == zametti::SaveResult::Unchanged ||
              outcome.result == zametti::SaveResult::Written,
          "сохранение с пустым абзацем не должно уходить в аварийный файл");
    checkEqual("текст\n", readFile(path), "пустой абзац в файл не попадает");
    check(!QFile::exists(path + QStringLiteral(".rescue-test")), "аварийный файл не создан");

    // Пустой пункт списка, наоборот, записывается: "-" файл выражает прекрасно.
    const QString listPath = pathFor("пустой-пункт.md");
    check(writeFile(listPath, "- пункт\n"), "не записать исходник списка");
    QTextDocument list;
    buildFrom("- пункт\n- \n", list);
    zametti::saveDocument(list, listPath, QStringLiteral("test"));
    checkEqual("- пункт\n-\n", readFile(listPath), "пустой пункт списка сохраняется");
}

// Пробелы по краям строк markdown съедает при разборе. Оставить их — значит
// проваливать самопроверку при каждом сохранении: ровно от этого на живой
// заметке накопился десяток аварийных файлов.
void checkEdgeSpaces() {
    struct Case {
        const char* source;      // что лежит в заметке
        const char* typed;       // что дописали в конец первого блока
        const char* expected;    // что должно оказаться в файле
        const char* what;
    };
    const Case cases[] = {
        {"- пункт\n", " ", "- пункт\n", "концевой пробел в пункте"},
        {"текст\n", "  ", "текст\n", "два концевых пробела в абзаце"},
        {"текст\n", "\t", "текст\n", "концевая табуляция"},
        {"# заголовок\n", " ", "# заголовок\n", "концевой пробел в заголовке"},
        {"> цитата\n", " ", "> цитата\n", "концевой пробел в цитате"},
        {"абзац с **жирным**\n", "  ", "абзац с **жирным**\n",
         "пробелы не сдвигают начертание"},
    };

    int index = 0;
    for (const Case& c : cases) {
        const QString path = pathFor((std::string("пробелы") + std::to_string(index++) +
                                      ".md").c_str());
        check(writeFile(path, c.source), "не записать исходник");

        QTextDocument doc;
        buildFrom(c.source, doc);
        QTextCursor cursor(&doc);
        cursor.movePosition(QTextCursor::EndOfBlock);
        cursor.insertText(QString::fromUtf8(c.typed));

        const zametti::SaveOutcome outcome =
            zametti::saveDocument(doc, path, QStringLiteral("test"));
        check(outcome.result != zametti::SaveResult::Rescued,
              std::string(c.what) + ": сохранение не должно уходить в аварийный файл");
        checkEqual(c.expected, readFile(path), c.what);
    }

    // Ведущие пробелы сохраняются неразрывными: отступ значим, им рисуют схемы
    // и лесенки. Обычный пробел в начале строки markdown съедает, неразрывный —
    // нет.
    //
    // Отступ приходит от набора, а не из файла: в файле его съедает уже разбор,
    // и вернуть оттуда нечего.
    struct Indent {
        const char* source;
        int block;
        int offset;
        const char* typed;
        const char* expected;
        const char* what;
    };
    const Indent indents[] = {
        {"отступ\n", 0, 0, "  ", "\xC2\xA0\xC2\xA0отступ\n",
         "два набранных ведущих пробела сохранены"},
        {"первая\nвторая\n", 0, 7, "  ", "первая\n\xC2\xA0\xC2\xA0вторая\n",
         "отступ второй строки абзаца"},
        {"- пункт\n", 0, 0, "  ", "- \xC2\xA0\xC2\xA0пункт\n",
         "отступ внутри пункта списка"},
        {"текст\n", 0, 5, "   ", "текст\n", "концевые пробелы по-прежнему выброшены"},
    };
    int k = 0;
    for (const Indent& c : indents) {
        const QString path = pathFor((std::string("отступ") + std::to_string(k++) +
                                      ".md").c_str());
        check(writeFile(path, c.source), "не записать исходник");
        QTextDocument doc;
        buildFrom(c.source, doc);
        QTextCursor cursor(&doc);
        cursor.setPosition(doc.findBlockByNumber(c.block).position() + c.offset);
        cursor.insertText(QString::fromUtf8(c.typed));

        const zametti::SaveOutcome outcome =
            zametti::saveDocument(doc, path, QStringLiteral("test"));
        check(outcome.result != zametti::SaveResult::Rescued,
              std::string(c.what) + ": не должно уводить в аварийный файл");
        checkEqual(c.expected, readFile(path), c.what);
    }

    // Пустая строка внутри абзаца — содержимое: в заметках ею отбивают куски
    // текста. В файле пустой она быть не может, там пустая строка блок
    // заканчивает, — поэтому в неё ставится неразрывный пробел.
    {
        const QString path = pathFor("пустая-внутри.md");
        check(writeFile(path, "первая\nвторая\n"), "не записать исходник");
        Builder builder;
        builder.add(zametti::Kind::Paragraph, "первая\n   \nвторая");
        QTextDocument doc;
        zametti::buildDocument(builder.ir, doc);

        const zametti::SaveOutcome outcome =
            zametti::saveDocument(doc, path, QStringLiteral("test"));
        check(outcome.result != zametti::SaveResult::Rescued,
              "пустая строка внутри блока не должна уводить в аварийный файл");
        checkEqual("первая\n\nвторая\n", readFile(path),
                   "пустая строка внутри абзаца стала настоящей пустой строкой");
    }

    // А в конце документа пустые строки не нужны: хвост из них набирается
    // случайно и ничего не отбивает.
    {
        const QString path = pathFor("хвост-пустых.md");
        check(writeFile(path, "текст\n"), "не записать исходник");
        Builder builder;
        builder.add(zametti::Kind::Paragraph, "текст\n\n\n");
        QTextDocument doc;
        zametti::buildDocument(builder.ir, doc);

        const zametti::SaveOutcome outcome =
            zametti::saveDocument(doc, path, QStringLiteral("test"));
        check(outcome.result != zametti::SaveResult::Rescued,
              "хвост пустых строк не должен уводить в аварийный файл");
        checkEqual("текст\n", readFile(path), "хвост пустых строк выброшен");
    }

    // Разметка не может начинаться или кончаться пробелом: markdown такое не
    // выражает. Проверено на ядре — курсив по слову круг проходит, курсив с
    // пробелом на краю нет. Выделить курсивом стих вместе с отступами человек
    // может, значит края надо поджимать.
    {
        struct SpanCase {
            const char* text;
            const char* expected;
            const char* what;
        };
        const SpanCase cases[] = {
            // Ведущие пробелы по дороге становятся неразрывными — это отступ.
            {"  слово", "\xC2\xA0\xC2\xA0*слово*\n",
             "ведущие пробелы уходят из курсива"},
            {"слово  ", "_слово_\n", "концевые тоже"},
            {"\xC2\xA0\xC2\xA0слово", "\xC2\xA0\xC2\xA0*слово*\n",
             "и неразрывные, которыми держится отступ"},
            // Знак начертания выбирает сериализатор: рядом с пробелом
            // подчёркивание не открыло бы курсив, и он берёт звёздочку.
            {"раз\nдва", "_раз\nдва_\n", "а перенос строки внутри разметки живёт"},
            {"   ", "", "разметка из одних пробелов исчезает вовсе"},
        };
        int n = 0;
        for (const SpanCase& c : cases) {
            const QString path =
                pathFor((std::string("курсив") + std::to_string(n++) + ".md").c_str());
            check(writeFile(path, "заглушка\n"), "не записать исходник");

            Builder builder;
            zametti::Block& block = builder.add(zametti::Kind::Paragraph, c.text);
            builder.mark(block, 0, block.text.size(), zametti::InlineItalic);
            QTextDocument doc;
            zametti::buildDocument(builder.ir, doc);

            const zametti::SaveOutcome outcome =
                zametti::saveDocument(doc, path, QStringLiteral("test"));
            check(outcome.result != zametti::SaveResult::Rescued,
                  std::string(c.what) + ": не должно уводить в аварийный файл");
            checkEqual(c.expected, readFile(path), c.what);
        }
    }

    // Перенос строки внутри заголовка и внутри встроенного кода markdown не
    // выражает. Оба случая достижимы: заголовок — вставкой, код — Ctrl+E,
    // дотянутым до соседней строки.
    {
        const QString heading = pathFor("заголовок-в-две-строки.md");
        check(writeFile(heading, "заглушка\n"), "не записать исходник");
        Builder builder;
        builder.add(zametti::Kind::Heading, "первая\nвторая").headingLevel = 2;
        QTextDocument doc;
        zametti::buildDocument(builder.ir, doc);
        const zametti::SaveOutcome outcome =
            zametti::saveDocument(doc, heading, QStringLiteral("test"));
        check(outcome.result != zametti::SaveResult::Rescued,
              "заголовок в две строки не должен уводить в аварийный файл");
        checkEqual("## первая вторая\n", readFile(heading),
                   "заголовок сводится в одну строку");
    }
    {
        const QString path = pathFor("код-через-строку.md");
        check(writeFile(path, "заглушка\n"), "не записать исходник");
        Builder builder;
        zametti::Block& block = builder.add(zametti::Kind::Paragraph, "раз\nдва");
        builder.mark(block, 0, block.text.size(), zametti::InlineCode);
        QTextDocument doc;
        zametti::buildDocument(builder.ir, doc);
        const zametti::SaveOutcome outcome =
            zametti::saveDocument(doc, path, QStringLiteral("test"));
        check(outcome.result != zametti::SaveResult::Rescued,
              "код через перенос не должен уводить в аварийный файл");
        checkEqual("`раз`\n`два`\n", readFile(path),
                   "код через перенос режется построчно");
    }

    // Пустой вложенный пункт markdown не выражает: одинокий "-" под текстом
    // родителя читается подчёркиванием заголовка, и весь список уезжает в
    // дословный кусок. В документе такой пункт заводится каждым Enter.
    {
        struct NestedCase {
            const char* source;
            const char* expected;
            const char* what;
        };
        const NestedCase cases[] = {
            {"- раз\n  - вложенный\n", "- раз\n  - вложенный\n",
             "пустой вложенный пункт в файл не идёт"},
            {"- раз\n", "- раз\n-\n", "а пустой пункт верхнего уровня записывается"},
        };
        int n = 0;
        for (const NestedCase& c : cases) {
            const QString path = pathFor(
                (std::string("пустой-вложенный") + std::to_string(n) + ".md").c_str());
            check(writeFile(path, c.source), "не записать исходник");

            QTextDocument doc;
            buildFrom(c.source, doc);
            // Enter в конце последнего пункта заводит пустой пункт того же
            // уровня — так это и выходит при живом наборе.
            QTextCursor cursor(&doc);
            cursor.movePosition(QTextCursor::End);
            zametti::splitBlockAtCursor(doc, cursor);

            const zametti::SaveOutcome outcome =
                zametti::saveDocument(doc, path, QStringLiteral("test"));
            check(outcome.result != zametti::SaveResult::Rescued,
                  std::string(c.what) + ": не должно уводить в аварийный файл");
            checkEqual(c.expected, readFile(path), c.what);
            ++n;
        }
    }

    // Потомки выброшенного пункта поднимаются на уровень: без родителя им
    // остаться нельзя.
    {
        const QString path = pathFor("потомки-пустого.md");
        check(writeFile(path, "заглушка\n"), "не записать исходник");
        Builder builder;
        const auto item = [&builder](zametti::Marker marker, int level, const char* text) {
            zametti::Block& block = builder.add(zametti::Kind::ListItem, text);
            block.marker = marker;
            block.level = static_cast<int16_t>(level);
        };
        item(zametti::Marker::Bullet, 0, "раз");
        item(zametti::Marker::Bullet, 1, "");
        item(zametti::Marker::Bullet, 2, "внук");
        QTextDocument doc;
        zametti::buildDocument(builder.ir, doc);
        const zametti::SaveOutcome outcome =
            zametti::saveDocument(doc, path, QStringLiteral("test"));
        check(outcome.result != zametti::SaveResult::Rescued,
              "пустой пункт с потомком не должен уводить в аварийный файл");
        checkEqual("- раз\n  - внук\n", readFile(path),
                   "потомок поднялся на уровень выброшенного");
    }

    // Зачёркивание живёт только на целых словах: тильды внутри слова markdown
    // разбирает буквально. Кусок раздаётся наружу до границ слова — обрезать
    // внутрь хуже, человек остался бы вовсе без зачёркивания.
    {
        const QString path = pathFor("зачёркнуто-полслова.md");
        check(writeFile(path, "заглушка\n"), "не записать исходник");
        Builder builder;
        zametti::Block& block = builder.add(zametti::Kind::Paragraph, "фрукты");
        builder.mark(block, 6, 6, zametti::InlineStrike);   // "кты" — вторая половина слова
        QTextDocument doc;
        zametti::buildDocument(builder.ir, doc);
        const zametti::SaveOutcome outcome =
            zametti::saveDocument(doc, path, QStringLiteral("test"));
        check(outcome.result != zametti::SaveResult::Rescued,
              "зачёркнутая половина слова не должна уводить в аварийный файл");
        checkEqual("~~фрукты~~\n", readFile(path), "зачёркивание раздалось до целого слова");
    }

    // Текст свят, разметка — по возможности. Правил о том, где знаки начертания
    // открывают кусок, много; если блок с разметкой обратно не читается,
    // разметка снимается, а текст остаётся до знака.
    {
        const QString path = pathFor("неживучая-разметка.md");
        check(writeFile(path, "заглушка\n"), "не записать исходник");
        Builder builder;
        zametti::Block& block = builder.add(zametti::Kind::Paragraph, "штуки 2-5.");
        builder.mark(block, 9, 1, zametti::InlineBold);   // одна точка, и та в конце
        QTextDocument doc;
        zametti::buildDocument(builder.ir, doc);
        const zametti::SaveOutcome outcome =
            zametti::saveDocument(doc, path, QStringLiteral("test"));
        check(outcome.result != zametti::SaveResult::Rescued,
              "невыразимая разметка не должна уводить в аварийный файл");
        checkEqual("штуки 2-5.\n", readFile(path), "текст остался, разметка снята");
    }

    // Записанное с неразрывными отступами устойчиво: второй проход ничего не
    // меняет, иначе файл переписывался бы при каждом сохранении.
    {
        const QString path = pathFor("схема.md");
        const std::string source =
            "\xC2\xA0\xC2\xA0ромб\n\xC2\xA0/    \\\n<      >\n";
        check(writeFile(path, source), "не записать исходник схемы");
        QTextDocument doc;
        buildFrom(source, doc);
        const zametti::SaveOutcome outcome =
            zametti::saveDocument(doc, path, QStringLiteral("test"));
        check(outcome.result == zametti::SaveResult::Unchanged,
              "схема с неразрывными отступами устойчива");
        checkEqual(source, readFile(path), "и не переписывается");
    }

    // В коде пробел значим, и трогать его нельзя.
    const QString path = pathFor("код-с-отступом.md");
    const std::string source = "```\n    отступ\n```\n";
    check(writeFile(path, source), "не записать исходник кода");
    QTextDocument doc;
    buildFrom(source, doc);
    zametti::saveDocument(doc, path, QStringLiteral("test"));
    checkEqual(source, readFile(path), "отступы в коде сохраняются как есть");
}

// Голую ссылку человек набирает текстом, а файл читает её ссылкой. Тексты при
// этом совпадают до знака, и запрещать такую запись значило бы запретить писать
// ссылки — ровно от этого на живой заметке накопились аварийные файлы.
void checkBareLinks() {
    const QString path = pathFor("ссылка.md");
    const std::string source = "смотри тут\n";
    check(writeFile(path, source), "не записать исходник");

    QTextDocument doc;
    buildFrom(source, doc);
    QTextCursor cursor(&doc);
    cursor.movePosition(QTextCursor::EndOfBlock);
    cursor.insertText(QStringLiteral(": https://apple.com."));

    const zametti::SaveOutcome outcome =
        zametti::saveDocument(doc, path, QStringLiteral("test"));
    check(outcome.result == zametti::SaveResult::Written,
          "набранная ссылка не должна мешать сохранению");
    checkEqual("смотри тут: https://apple.com.\n", readFile(path), "ссылка записана");
    check(outcome.differsFromDocument,
          "прочитанное обратно богаче документа: появилась ссылка");

    // А вот подмена текста обязана ловиться по-прежнему: строение и содержимое
    // сверяются строго.
    check(zametti::saveDocument(doc, pathFor("сломанный.md"), QStringLiteral("stamp"),
                                brokenReader)
                  .result == zametti::SaveResult::Rescued,
          "испорченный читатель ловится и с новой сверкой");
}

// Enter в конце абзаца оставляет висящий перенос. В файле он даёт пустую
// строку, а пустая строка абзац заканчивает — без уборки самопроверка не дала
// бы записать.
void checkTrailingSoftBreak() {
    const QString path = pathFor("висящий-перенос.md");
    check(writeFile(path, "текст\n"), "не записать исходник");

    QTextDocument doc;
    buildFrom("текст\n", doc);
    // Через саму операцию, а не вставкой разделителя: у настоящего переноса есть
    // пометка, по которой читатель узнаёт в нём перевод строки.
    QTextCursor cursor(&doc);
    cursor.movePosition(QTextCursor::EndOfBlock);
    zametti::splitBlockAtCursor(doc, cursor);

    const zametti::SaveOutcome outcome =
        zametti::saveDocument(doc, path, QStringLiteral("test"));
    check(outcome.result != zametti::SaveResult::Rescued,
          "висящий перенос не должен уводить в аварийный файл");
    checkEqual("текст\n", readFile(path), "в файл висящий перенос не идёт");

    // А настоящий перенос между строками остаётся.
    const QString kept = pathFor("перенос-между.md");
    check(writeFile(kept, "первая\nвторая\n"), "не записать исходник");
    QTextDocument two;
    buildFrom("первая\nвторая\n", two);
    zametti::saveDocument(two, kept, QStringLiteral("test"));
    checkEqual("первая\nвторая\n", readFile(kept), "перенос между строками сохраняется");
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

    checkEmptyParagraphs();
    checkEdgeSpaces();
    checkTrailingSoftBreak();
    checkBareLinks();
    checkRescue();
    checkFailure();

    std::printf("проверок %d, провалов %d\n", zt::g_checks, zt::g_failures);
    return zt::g_failures == 0 ? 0 : 1;
}
