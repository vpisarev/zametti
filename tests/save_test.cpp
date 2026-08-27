// Сохранение: инвариант B этапа и последний рубеж перед заменой файла.
//
// B. открыть файл и сохранить без правок == noteOf(file).toMarkdown(),
//    для уже канонических файлов — побайтовая идентичность.
//
// И отдельно — то, ради чего самопроверка вообще есть: если читатель сломан,
// старый файл обязан остаться нетронутым.

#include "document_builder.h"
#include "pieces.h"
#include "document_saver.h"
#include "hash.h"
#include "editor_ops.h"
#include "test_util.h"

#include <vector>
#include "testdata.h"

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

// Инвариант B на одном исходнике.
void checkSave(const std::string& source, const char* name) {
    const QString path = pathFor(name);
    check(writeFile(path, source), "не записать исходник");

    // ЗАМЕТКА ЦЕЛИКОМ, а не голый документ: инвариант B говорит про
    // noteOf(file), и проверять его надо тем же путём, каким пишет программа.
    zametti::ZNote note = noteOf(source);

    const zametti::SaveOutcome first = note.save(path, QStringLiteral("test"));
    const std::string canonical = noteOf(source).toMarkdown();

    if (source == canonical) {
        check(first.result == zametti::SaveResult::Unchanged,
              std::string(name) + ": канонический файл не должен переписываться");
    } else {
        check(first.result == zametti::SaveResult::Written,
              std::string(name) + ": файл должен был обновиться");
    }
    checkEqual(canonical, readFile(path), std::string(name) + ": содержимое после записи");

    // Повторное сохранение того же документа не трогает файл вовсе.
    const zametti::SaveOutcome second = note.save(path, QStringLiteral("test"));
    check(second.result == zametti::SaveResult::Unchanged,
          std::string(name) + ": повторное сохранение должно быть пустой операцией");

    // Отпечаток из записи обязан совпасть с отпечатком того, что лежит в
    // файле: на нём стоит вся сверка «правил ли кто-то файл снаружи», и
    // перечитывать файл ради неё больше никто не будет.
    const zametti::Digest onDisk = zametti::hashOf(readFile(path));
    check(first.digest == onDisk,
          std::string(name) + ": отпечаток записи совпадает с файлом");
    check(second.digest == onDisk,
          std::string(name) + ": отпечаток при «не изменилось» тоже совпадает");
    check(!first.digest.empty(), std::string(name) + ": отпечаток не пуст");

    // С известным отпечатком «не изменилось» решается без чтения файла. Чтобы
    // это было ВИДНО, а не только быстро, подменим файл мусором: старый путь
    // прочитал бы его и переписал, новый даже не заглянет.
    check(writeFile(path, std::string("мусор, которого тут быть не должно\n")),
          std::string(name) + ": мусор записан");
    const zametti::SaveOutcome byDigest = note.save(path, QStringLiteral("test"), onDisk);
    check(byDigest.result == zametti::SaveResult::Unchanged,
          std::string(name) + ": с известным отпечатком файл не читается");
    checkEqual(std::string("мусор, которого тут быть не должно\n"), readFile(path),
               std::string(name) + ": и не переписывается");

    // А без отпечатка — прежний путь: прочитает, увидит расхождение, перепишет.
    const zametti::SaveOutcome byBytes = note.save(path, QStringLiteral("test"));
    check(byBytes.result == zametti::SaveResult::Written,
          std::string(name) + ": без отпечатка расхождение видно и файл переписан");
    checkEqual(canonical, readFile(path), std::string(name) + ": и содержимое вернулось");
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
// Литералы блоков строятся билдером: он держит список и раздаёт ссылки на
// блоки в нём. Блок владеет своим текстом и своими кусками, поэтому размечать
// его можно когда угодно.
struct Builder {
    std::vector<zametti::Piece> ir;

    zametti::Piece& add(zametti::Kind kind, std::string_view text) {
        zametti::Piece block;
        block.kind = kind;
        block.text = QString::fromUtf8(text.data(), qsizetype(text.size()));
        block.trailingNewline = block.text.endsWith(QLatin1Char('\n'));
        ir.push_back(std::move(block));
        return ir.back();
    }
    zametti::Piece& addRaw(std::string_view bytes) {
        zametti::Piece block;
        block.raw = true;
        block.text = QString::fromUtf8(bytes.data(), qsizetype(bytes.size()));
        block.trailingNewline = block.text.endsWith(QLatin1Char('\n'));
        ir.push_back(std::move(block));
        return ir.back();
    }
    void mark(zametti::Piece& block, int from, int length, zametti::InlineFlag flag) {
        zametti::Run run;
        run.start = from;
        run.end = from + length;
        run.set(flag, true);
        block.runs.push_back(run);
    }
};

std::vector<zametti::Piece> brokenReader(const QTextDocument&) {
    Builder b;
    b.addRaw("| это не таблица |\n");
    return std::move(b.ir);
}

void checkRescue() {
    const QString path = pathFor("rescue.md");
    const std::string source = "# заголовок\n\nтекст\n";
    check(writeFile(path, source), "не записать исходник для аварийного случая");

    zametti::ZDocument note = zametti::ZDocument::fromPieces(pieces(source));

    const zametti::SaveOutcome outcome =
        note.saveTo(path, QStringLiteral("stamp"), brokenReader);

    // ДОГОВОР ИЗМЕНИЛСЯ. Прежде расхождение ЗАПРЕЩАЛО запись: файл оставался
    // прежним, а буфер уезжал в .rescue. Задумано это было как последний рубеж
    // против потери данных, а обернулось способом её устроить: у владельца
    // отказ повторялся на каждом автосохранении, он выключил предупреждение,
    // доработал заметку, вышел — и не нашёл ни одной своей правки.
    //
    // Теперь пишем всегда, а расхождение остаётся диагностикой. Это стало
    // возможно потому, что у заметки есть полная история: неудачная запись
    // отменима, а потерянная работа — нет.
    check(outcome.result == zametti::SaveResult::Written,
          "расхождение самопроверки больше не отменяет запись");
    check(!outcome.message.isEmpty(), "но о нём сказано словами");
    checkEqual("| это не таблица |\n", readFile(path),
               "в файле то, что дал читатель");

    const QString rescuePath = path + QStringLiteral(".rescue-stamp");
    check(QFile::exists(rescuePath), "аварийная копия всё равно сделана");
    checkEqual("| это не таблица |\n", readFile(rescuePath),
               "содержимое аварийной копии");
}

// Пустой абзац markdown выразить нечем, а Enter его заводит. Без уборки
// самопроверка ловила бы расхождение при каждом сохранении, и вместо записи
// появлялся бы аварийный файл — ровно это и случилось на живой заметке.
void checkEmptyParagraphs() {
    const QString path = pathFor("пустые.md");
    check(writeFile(path, "текст\n"), "не записать исходник");

    // Пустой абзац в конце — состояние, markdown не выражающее: строим его
    // блоками, а не правкой мимо заметки.
    std::vector<zametti::Piece> blocks = pieces("текст\n");
    blocks.push_back(zametti::Piece{});
    zametti::ZDocument note = zametti::ZDocument::fromPieces(blocks);

    const zametti::SaveOutcome outcome = note.saveTo(path, QStringLiteral("test"));
    check(outcome.result == zametti::SaveResult::Unchanged ||
              outcome.result == zametti::SaveResult::Written,
          "сохранение с пустым абзацем не должно уходить в аварийный файл");
    checkEqual("текст\n", readFile(path), "пустой абзац в файл не попадает");
    check(!QFile::exists(path + QStringLiteral(".rescue-test")), "аварийный файл не создан");

    // Пустой пункт списка, наоборот, записывается: "-" файл выражает прекрасно.
    const QString listPath = pathFor("пустой-пункт.md");
    check(writeFile(listPath, "- пункт\n"), "не записать исходник списка");
    zametti::ZDocument list = zametti::ZDocument::fromPieces(pieces("- пункт\n- \n"));
    list.saveTo(listPath, QStringLiteral("test"));
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

        zametti::ZDocument note = zametti::ZDocument::fromPieces(pieces(c.source));
        QTextCursor cursor = note.caretAtBlock(0);
        cursor.movePosition(QTextCursor::EndOfBlock);
        note.insertText(cursor, QString::fromUtf8(c.typed));

        const zametti::SaveOutcome outcome = note.saveTo(path, QStringLiteral("test"));
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
        zametti::ZDocument note = zametti::ZDocument::fromPieces(pieces(c.source));
        QTextCursor cursor = note.caretAtBlock(c.block);
        cursor.setPosition(cursor.position() + c.offset);
        note.insertText(cursor, QString::fromUtf8(c.typed));

        const zametti::SaveOutcome outcome = note.saveTo(path, QStringLiteral("test"));
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
        zametti::ZDocument note = zametti::ZDocument::fromPieces(builder.ir);

        const zametti::SaveOutcome outcome = note.saveTo(path, QStringLiteral("test"));
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
        zametti::ZDocument note = zametti::ZDocument::fromPieces(builder.ir);

        const zametti::SaveOutcome outcome = note.saveTo(path, QStringLiteral("test"));
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
            zametti::Piece& block = builder.add(zametti::Kind::Paragraph, c.text);
            builder.mark(block, 0, block.text.size(), zametti::InlineItalic);
            zametti::ZDocument note = zametti::ZDocument::fromPieces(builder.ir);

            const zametti::SaveOutcome outcome = note.saveTo(path, QStringLiteral("test"));
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
        zametti::ZDocument note = zametti::ZDocument::fromPieces(builder.ir);
        const zametti::SaveOutcome outcome = note.saveTo(heading, QStringLiteral("test"));
        check(outcome.result != zametti::SaveResult::Rescued,
              "заголовок в две строки не должен уводить в аварийный файл");
        checkEqual("## первая вторая\n", readFile(heading),
                   "заголовок сводится в одну строку");
    }
    {
        const QString path = pathFor("код-через-строку.md");
        check(writeFile(path, "заглушка\n"), "не записать исходник");
        Builder builder;
        zametti::Piece& block = builder.add(zametti::Kind::Paragraph, "раз\nдва");
        builder.mark(block, 0, block.text.size(), zametti::InlineCode);
        zametti::ZDocument note = zametti::ZDocument::fromPieces(builder.ir);
        const zametti::SaveOutcome outcome = note.saveTo(path, QStringLiteral("test"));
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

            // Enter в конце последнего пункта заводит пустой пункт того же
            // уровня — так это и выходит при живом наборе. Через глагол
            // заметки: другого входа в правку нет.
            zametti::ZDocument note = bodyOf(c.source);
            QTextCursor cursor = note.caretAtBlock(note.blockCount() - 1);
            cursor.movePosition(QTextCursor::End);
            note.breakBlock(cursor, zametti::ZDocument::BreakKind::Plain);

            const zametti::SaveOutcome outcome = note.saveTo(path, QStringLiteral("test"));
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
            zametti::Piece& block = builder.add(zametti::Kind::ListItem, text);
            block.marker = marker;
            block.level = static_cast<int16_t>(level);
        };
        item(zametti::Marker::Bullet, 0, "раз");
        item(zametti::Marker::Bullet, 1, "");
        item(zametti::Marker::Bullet, 2, "внук");
        zametti::ZDocument note = zametti::ZDocument::fromPieces(builder.ir);
        const zametti::SaveOutcome outcome = note.saveTo(path, QStringLiteral("test"));
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
        zametti::Piece& block = builder.add(zametti::Kind::Paragraph, "фрукты");
        builder.mark(block, 3, 3, zametti::InlineStrike);   // "кты" — вторая половина слова (единицы UTF-16)
        zametti::ZDocument note = zametti::ZDocument::fromPieces(builder.ir);
        const zametti::SaveOutcome outcome = note.saveTo(path, QStringLiteral("test"));
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
        zametti::Piece& block = builder.add(zametti::Kind::Paragraph, "штуки 2-5.");
        builder.mark(block, 9, 1, zametti::InlineBold);   // одна точка, и та в конце
        zametti::ZDocument note = zametti::ZDocument::fromPieces(builder.ir);
        const zametti::SaveOutcome outcome = note.saveTo(path, QStringLiteral("test"));
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
        zametti::ZDocument note = zametti::ZDocument::fromPieces(pieces(source));
        const zametti::SaveOutcome outcome = note.saveTo(path, QStringLiteral("test"));
        check(outcome.result == zametti::SaveResult::Unchanged,
              "схема с неразрывными отступами устойчива");
        checkEqual(source, readFile(path), "и не переписывается");
    }

    // В коде пробел значим, и трогать его нельзя.
    const QString path = pathFor("код-с-отступом.md");
    const std::string source = "```\n    отступ\n```\n";
    check(writeFile(path, source), "не записать исходник кода");
    zametti::ZDocument note = zametti::ZDocument::fromPieces(pieces(source));
    note.saveTo(path, QStringLiteral("test"));
    checkEqual(source, readFile(path), "отступы в коде сохраняются как есть");
}

// Голую ссылку человек набирает текстом, а файл читает её ссылкой. Тексты при
// этом совпадают до знака, и запрещать такую запись значило бы запретить писать
// ссылки — ровно от этого на живой заметке накопились аварийные файлы.
void checkBareLinks() {
    const QString path = pathFor("ссылка.md");
    const std::string source = "смотри тут\n";
    check(writeFile(path, source), "не записать исходник");

    zametti::ZDocument note = zametti::ZDocument::fromPieces(pieces(source));
    QTextCursor cursor = note.caretAtBlock(0);
    cursor.movePosition(QTextCursor::EndOfBlock);
    note.insertText(cursor, QStringLiteral(": https://apple.com."));

    const zametti::SaveOutcome outcome = note.saveTo(path, QStringLiteral("test"));
    check(outcome.result == zametti::SaveResult::Written,
          "набранная ссылка не должна мешать сохранению");
    checkEqual("смотри тут: https://apple.com.\n", readFile(path), "ссылка записана");
    check(outcome.differsFromDocument,
          "прочитанное обратно богаче документа: появилась ссылка");

    // Подмена текста по-прежнему ЗАМЕЧАЕТСЯ — но теперь она не отменяет запись,
    // а объясняется словами и копией буфера (см. checkRescue).
    {
        const zametti::SaveOutcome broken =
            note.saveTo(pathFor("сломанный.md"), QStringLiteral("stamp"), brokenReader);
        check(broken.result == zametti::SaveResult::Written,
              "испорченный читатель записи не отменяет");
        check(!broken.message.isEmpty(), "испорченный читатель ловится и с новой сверкой");
        check(!broken.rescuePath.isEmpty(), "и копия буфера сделана");
    }
}

// Enter в конце абзаца оставляет висящий перенос. В файле он даёт пустую
// строку, а пустая строка абзац заканчивает — без уборки самопроверка не дала
// бы записать.
void checkTrailingSoftBreak() {
    const QString path = pathFor("висящий-перенос.md");
    check(writeFile(path, "текст\n"), "не записать исходник");

    // Через саму операцию, а не вставкой разделителя: у настоящего переноса есть
    // пометка, по которой читатель узнаёт в нём перевод строки.
    zametti::ZDocument note = bodyOf("текст\n");
    QTextCursor cursor = note.caretAtBlock(0);
    cursor.movePosition(QTextCursor::EndOfBlock);
    note.breakBlock(cursor, zametti::ZDocument::BreakKind::Plain);

    const zametti::SaveOutcome outcome = note.saveTo(path, QStringLiteral("test"));
    check(outcome.result != zametti::SaveResult::Rescued,
          "висящий перенос не должен уводить в аварийный файл");
    checkEqual("текст\n", readFile(path), "в файл висящий перенос не идёт");

    // А настоящий перенос между строками остаётся.
    const QString kept = pathFor("перенос-между.md");
    check(writeFile(kept, "первая\nвторая\n"), "не записать исходник");
    zametti::ZDocument two = zametti::ZDocument::fromPieces(pieces("первая\nвторая\n"));
    two.saveTo(kept, QStringLiteral("test"));
    checkEqual("первая\nвторая\n", readFile(kept), "перенос между строками сохраняется");
}

// Записать некуда — старый файл всё равно цел.
// Картинка без подписи. У неё нет текста — есть только сам снимок, и ни «пустой
// абзац в конце файла не нужен», ни «схлопнувшийся кусок разметки выбрасываем»
// к ней не относятся: приведение к файлу обязано различать пустоту и картинку с
// пустой подписью. Ловилось только на записи: канон toMarkdown() картинку
// держал, а файл — нет.
//
// Картинка без подписи ПОСРЕДИ ТЕКСТА («до ![](x.png) после») — ниже, отдельно:
// её теряла уже сборка документа (знаков, на которые лечь формату, у неё нет),
// и теперь сборка даёт ей безымянное имя «image N» — в файл уходит
// «![image 1](x.png)», картинка цела (решение владельца).
void checkTrailingBareImage() {
    for (const char* source : {"---\ntitle: t\n---\n\nтекст\n\n![](x.png)\n",
                               "---\ntitle: t\n---\n\n![](x.png)\n",
                               "---\ntitle: t\n---\n\n![](x.png)\n\nтекст\n",
                               "---\ntitle: t\n---\n\n- пункт\n\n  ![](x.png)\n"}) {
        const QString path = pathFor("bare-image.md");
        check(writeFile(path, source), "картинка без подписи: не записать исходник");
        // Шапка тут есть, и писать надо ЗАМЕТКОЙ: конверт — её дело.
        zametti::ZNote note = noteOf(source);
        note.save(path, QStringLiteral("test"));
        const std::string onDisk = readFile(path);
        check(onDisk.find("![](x.png)") != std::string::npos,
              std::string("картинка без подписи пережила запись: ") + source);
        checkEqual(noteOf(source).toMarkdown(), onDisk,
                   "картинка без подписи: канон и файл — одно");
    }
}

// Строчная картинка без подписи: не теряется, а получает имя «image N» — оно
// безымянное (под снимком не показалось бы), а картинка в файле цела. Две
// картинки в одном абзаце получают разные номера; настоящая подпись соседки
// не трогается; разметка вокруг не съезжает.
void checkInlineBareImage() {
    struct Case {
        const char* source;
        const char* expected;
    };
    const Case cases[] = {
        {"до ![](x.png) после\n", "до ![image 1](x.png) после\n"},
        {"![](a.png) и ![](b.png)\n", "![image 1](a.png) и ![image 2](b.png)\n"},
        {"текст, *курсив* и ![](x.png), потом **жирный** хвост\n",
         "текст, _курсив_ и ![image 1](x.png), потом **жирный** хвост\n"},
        {"- пункт с ![](x.png) внутри\n", "- пункт с ![image 1](x.png) внутри\n"},
        // Картинка внутри выделения — дословный кусок (так решил разбор), и
        // дословное не трогается: в файл уходит ровно то, что было.
        {"*курсив ![](x.png) до конца*\n", "*курсив ![](x.png) до конца*\n"},
    };
    for (const Case& c : cases) {
        const QString path = pathFor("inline-bare-image.md");
        check(writeFile(path, c.source), "строчная картинка без подписи: не записать исходник");
        zametti::ZNote note = noteOf(c.source);
        note.save(path, QStringLiteral("test"));
        const std::string onDisk = readFile(path);
        checkEqual(std::string(c.expected), onDisk,
                   std::string("строчная картинка без подписи пережила запись: ") + c.source);
        checkEqual(noteOf(c.source).toMarkdown(), onDisk,
                   "строчная картинка без подписи: канон и файл — одно");
    }
}

// ЖИВОЕ, ЧЕГО MARKDOWN НЕ ХРАНИТ, ОБЯЗАНО УЙТИ В ФАЙЛ ТАК, ЧТОБЫ ФАЙЛ ПРОЧЁЛСЯ
// ТЕМ ЖЕ. Из файла такие состояния не подать — их заводит правка: одиночный
// неразрывный пробел посреди строки (склеили строки, и наш отступ оказался в
// середине), отступ у текста комментария, таб внутри блока кода. Чтение всё это
// приводит к своему виду; значит и записывать надо приведённым, иначе первое же
// перечитывание сдвинет текст, отпечаток запляшет, а «drift» покажется на ровном
// месте. Ловится это только через живой документ — потому и здесь, а не в
// круговом наборе.
void checkLiveExtras() {
    // 1. Ведущий неразрывный пробел, ставший серединой строки.
    {
        zametti::ZDocument note = zametti::ZDocument::fromPieces(pieces("первая\n\u00a0вторая\n"));
        QTextCursor cursor = note.caretAtBlock(0);
        cursor.movePosition(QTextCursor::EndOfBlock);
        // Каретка — перед неразрывным, то есть сразу за переносом строки:
        // Backspace здесь склеивает строки, а не съедает сам отступ.
        cursor.setPosition(cursor.position() - int(QStringLiteral("вторая").size()) - 1);
        note.deleteBack(cursor);   // склеили строки: неразрывный уехал в середину
        const std::string canon = note.toMarkdown();
        check(canon.find("\u00a0") == std::string::npos,
              "одиночный неразрывный посреди строки в файл не уходит: " + canon);
        checkEqual(canon, noteOf(canon).toMarkdown(),
                   "склеенная строка: канон — неподвижная точка");
    }
    // 2. Отступ у текста комментария: за «<!-- » отступу взяться неоткуда.
    {
        zametti::ZDocument note = zametti::ZDocument::fromPieces(pieces("текст\n"));
        QTextCursor cursor = note.caretAtBlock(0);
        note.insertText(cursor, QStringLiteral("  "));
        QTextCursor at = note.caretAtBlock(0);
        note.toggleComment(at);
        const std::string canon = note.toMarkdown();
        checkEqual(std::string("<!-- текст -->\n"), canon, "отступ у комментария не пишется");
        checkEqual(canon, noteOf(canon).toMarkdown(),
                   "комментарий с отступом: канон — неподвижная точка");
    }
    // 3. Пустой пункт не держит содержимого через пустую строку: замерено на
    // md4c — "-\n\n  текст" читается пунктом, пустой строкой и АБЗАЦЕМ СНАРУЖИ,
    // а два пробела становятся отступом автора. Записав абзац внутрь пустого
    // пункта, мы получили бы файл с двумя лишними знаками в тексте.
    {
        zametti::ZDocument note =
            zametti::ZDocument::fromPieces(pieces("- п\n\n  текст\n"));
        QTextCursor cursor = note.caretAtBlock(0);
        cursor.movePosition(QTextCursor::EndOfBlock);
        note.deleteBack(cursor);   // пункт опустел, абзац под ним остался
        const std::string canon = note.toMarkdown();
        checkEqual(canon, noteOf(canon).toMarkdown(),
                   "абзац под опустевшим пунктом: канон — неподвижная точка");
        check(canon.find("\u00a0") == std::string::npos,
              "и отступ не превратился в неразрывные: " + canon);
    }
    // 4. Пустой пункт сразу за абзацем файл съедает: "текст\n-" читается
    // setext-заголовком второго уровня, "текст\n1." — просто текстом абзаца
    // (замерено на md4c). В живом документе это законное промежуточное
    // состояние набора — человек напечатал "- " и сейчас напечатает текст, — а
    // в файле между ними обязана встать пустая строка.
    for (zametti::Marker marker : {zametti::Marker::Bullet, zametti::Marker::Ordered}) {
        std::vector<zametti::Piece> blocks;
        zametti::Piece text;
        text.kind = zametti::Kind::Paragraph;
        text.text = QStringLiteral("текст");
        zametti::Piece item;
        item.kind = zametti::Kind::ListItem;
        item.marker = marker;
        item.level = 0;
        blocks.push_back(text);
        blocks.push_back(item);

        zametti::ZDocument note = zametti::ZDocument::fromPieces(blocks);
        const std::string canon = note.toMarkdown();
        check(canon.find("\n\n") != std::string::npos,
              "пустой пункт за абзацем отбит пустой строкой: " + canon);
        checkEqual(canon, noteOf(canon).toMarkdown(),
                   "пустой пункт за абзацем: канон — неподвижная точка");
        const std::vector<zametti::Piece> back = pieces(canon);
        check(back.size() == 3 && back[2].kind == zametti::Kind::ListItem,
              "и читается обратно пунктом, а не заголовком: " + canon);
    }
    // 5. Таб, набранный в блоке кода: чтение развернёт его по стопам.
    {
        zametti::ZDocument note = zametti::ZDocument::fromPieces(pieces("```\nраз\n```\n"));
        QTextCursor cursor = note.caretAtBlock(0);
        cursor.movePosition(QTextCursor::StartOfBlock);
        note.insertText(cursor, QStringLiteral("\t"));
        const std::string canon = note.toMarkdown();
        checkEqual(std::string("```\n    раз\n```\n"), canon, "таб в отступе кода развёрнут");
        checkEqual(canon, noteOf(canon).toMarkdown(),
                   "таб в коде: канон — неподвижная точка");
    }
}

void checkFailure() {
    const QString path = g_dir + QStringLiteral("/нет-такого-каталога/файл.md");
    zametti::ZDocument note = zametti::ZDocument::fromPieces(pieces("текст\n"));
    const zametti::SaveOutcome outcome = note.saveTo(path, QStringLiteral("stamp"));
    check(outcome.result == zametti::SaveResult::Failed,
          "запись в несуществующий каталог должна проваливаться");
    check(!outcome.message.isEmpty(), "у провала должно быть человеческое объяснение");
}

}  // namespace

// СТОРОЖ НА ЖИВЫХ ЗАМЕТКАХ: пройти весь путь записи и посмотреть, на каких
// заметках самопроверка не даёт записать. Не проверка, а прибор: зовётся
// вторым параметром — каталогом с .md.
//
// Путь — ровно тот, каким пишет программа: ZNote::load → ZNote::save во
// временный файл. Отказ виден по исходу: Rescued означает, что самопроверка
// не сошлась, и рядом лежит копия буфера. Именно это и отправляло правки
// владельца в .rescue вместо файла.
void surveyGuard(const QString& root) {
    QDir dir(root);
    const QStringList files = dir.entryList({QStringLiteral("*.md")}, QDir::Files);
    const QString scratch = QDir::tempPath() + QStringLiteral("/zametti-survey");
    QDir(scratch).removeRecursively();
    if (!QDir().mkpath(scratch)) {
        std::printf("не создать каталог %s\n", scratch.toUtf8().constData());
        return;
    }
    int checked = 0;
    int refused = 0;
    for (const QString& name : files) {
        QFile file(dir.filePath(name));
        if (!file.open(QIODevice::ReadOnly)) continue;
        const QByteArray bytes = file.readAll();
        file.close();

        zametti::ZNote note;
        if (!note.load(std::string_view(bytes.constData(), size_t(bytes.size())))) continue;
        ++checked;
        const zametti::SaveOutcome outcome =
            note.save(scratch + QLatin1Char('/') + name, QStringLiteral("survey"));
        if (outcome.result != zametti::SaveResult::Rescued) continue;
        ++refused;
        if (refused <= 5)
            std::printf("сторож не даёт записать: %s\n  %s\n", name.toUtf8().constData(),
                        outcome.message.toUtf8().constData());
    }
    std::printf("сторож: проверено %d, отказов %d\n", checked, refused);
}

static int ztRunSuite(int argc, char** argv) {
    if (argc < 2) {
        std::printf("использование: save_test <каталог для временных файлов>\n");
        return 2;
    }

    if (argc > 2) {
        surveyGuard(QString::fromLocal8Bit(argv[2]));
        return 0;
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
    checkTrailingBareImage();
    checkInlineBareImage();
    checkLiveExtras();
    checkRescue();
    checkFailure();

    std::printf("проверок %d, провалов %d\n", zt::g_checks, zt::g_failures);
    return zt::freshFailures();
}

// Набор целиком одним TEST: тело не тронуто, argv ему собран здесь.
// Дробить на отдельные проверки — отдельная работа, по одному набору.
TEST(Save, All) {
    std::vector<QByteArray> ztArgs{QByteArrayLiteral("save_test")};
    ztArgs.push_back((zt::TestData::outDir(QStringLiteral("save"))).toLocal8Bit());
    std::vector<char*> ztArgv;
    for (QByteArray& a : ztArgs) ztArgv.push_back(a.data());
    EXPECT_EQ(0, ztRunSuite(int(ztArgv.size()), ztArgv.data()));
}
