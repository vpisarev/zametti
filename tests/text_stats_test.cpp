// Счёт слов и строк: что считается словом и сколько стоит пересчёт.
//
// Проверка и прибор в одном файле. Ключ --bench включает замер (в ctest не
// попадает: замер — не проверка), путь к каталогу с .md — счёт слов по живым
// заметкам для сверки с чужими счётчиками.

#include "document_builder.h"
#include "pieces.h"
#include "test_util.h"
#include "testdata.h"
#include "text_stats.h"

#include <QElapsedTimer>
#include <QGuiApplication>
#include <QString>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

void check(bool ok, const std::string& what) {
    ++zt::g_checks;
    if (ok) return;
    ++zt::g_failures;
    std::printf("провал: %s\n", what.c_str());
}

void checkWords(int expected, const QString& text, const std::string& what) {
    const int actual = zametti::countWords(text);
    ++zt::g_checks;
    if (actual == expected) return;
    ++zt::g_failures;
    std::printf("провал: %s\n  ждали:  %d\n  вышло:  %d\n  текст:  [%s]\n", what.c_str(),
                expected, actual, text.toUtf8().constData());
}

std::string readFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::vector<std::filesystem::path> markdownFiles(const std::filesystem::path& root) {
    std::vector<std::filesystem::path> files;
    std::error_code ec;
    for (std::filesystem::recursive_directory_iterator it(root, ec), end; it != end;
         it.increment(ec)) {
        if (ec) break;
        if (it->is_regular_file() && it->path().extension() == ".md") files.push_back(it->path());
    }
    std::sort(files.begin(), files.end());
    return files;
}

// --- что считается словом ---------------------------------------------------

void checkWordRules() {
    checkWords(0, QString(), "пусто");
    checkWords(0, QStringLiteral("   \t  "), "одни пробелы");
    checkWords(1, QStringLiteral("слово"), "одно слово");
    checkWords(1, QStringLiteral("  слово  "), "пробелы по краям");
    checkWords(2, QStringLiteral("два слова"), "два слова");
    checkWords(2, QStringLiteral("два слова"), "через перевод строки внутри блока");
    checkWords(3, QStringLiteral("раз, два; три!"), "знаки препинания");
    checkWords(1, QStringLiteral("1984"), "цифры — слово");
    checkWords(1, QStringLiteral("файл2"), "буквы с цифрами — одно слово");

    // Правило без исключений: разделяет всё, что не буква и не цифра.
    checkWords(2, QStringLiteral("3.14"), "точка разделяет");
    checkWords(2, QStringLiteral("по-русски"), "дефис разделяет");
    checkWords(2, QStringLiteral("don't"), "апостроф разделяет");

    // Комбинирующие метки слово не рвут: "и" + U+0306 — это "й".
    checkWords(1, QStringLiteral("й"), "краткая");
    checkWords(1, QStringLiteral("café"), "аксан");
    checkWords(1, QStringLiteral("мяг­кий"), "мягкий перенос внутри слова");

    // За пределами BMP: суррогатная пара — одна буква, а не два не-знака.
    checkWords(1, QString::fromUcs4(U"\U0001D400\U0001D401"), "математические заглавные");
    checkWords(0, QStringLiteral("🎉"), "эмодзи словом не считается");
    checkWords(2, QStringLiteral("раз 🎉 два"), "эмодзи разделяет и не считается");

    // Проверка снятием быстрого пути: разбор суррогатной пары обязан работать
    // и когда пара стоит последней.
    checkWords(1, QString::fromUcs4(U"\U0001D400"), "суррогатная пара в конце текста");
}

// Быстрая таблица заполняется из Qt, но разбор категорий написан руками:
// проверяем его по всему Юникоду против готового ответа самого Qt.
void checkRolesAgainstQt() {
    int mismatches = 0;
    for (char32_t cp = 0; cp <= 0x10FFFF; ++cp) {
        if (cp >= 0xD800 && cp <= 0xDFFF) continue;   // суррогаты знаками не бывают
        const QChar::Category cat = QChar::category(cp);
        const bool mark = cat == QChar::Mark_NonSpacing || cat == QChar::Mark_SpacingCombining ||
                          cat == QChar::Mark_Enclosing || cat == QChar::Other_Format;
        if (mark) continue;   // метки в слово не входят и слово не рвут
        // Слово из одного знака: получилось единицей — значит знак считается
        // буквой, нулём — разделителем.
        const bool letter = zametti::countWords(QString::fromUcs4(&cp, 1)) == 1;
        if (letter == QChar::isLetterOrNumber(cp)) continue;
        if (++mismatches <= 5)
            std::printf("провал: знак U+%04X: буква по Qt %d, по нашему счёту %d\n",
                        unsigned(cp), int(QChar::isLetterOrNumber(cp)), int(letter));
    }
    ++zt::g_checks;
    if (mismatches > 0) {
        ++zt::g_failures;
        std::printf("провал: разбор категорий разошёлся с Qt на %d знаках\n", mismatches);
    }
}

void checkLineRules() {
    check(zametti::countLineBreaks(QStringLiteral("одна строка")) == 0, "без переводов");
    check(zametti::countLineBreaks(QStringLiteral("раз два три")) == 2, "два перевода");
}

// --- сводка по документу ----------------------------------------------------

std::shared_ptr<QTextDocument> build(const std::string& markdown) {
    auto doc = std::make_shared<QTextDocument>();
    zametti::buildDocument(pieces(markdown), *doc);
    return doc;
}

// Оба счёта разом: числа обязаны совпасть, иначе один из них врёт.
zametti::NoteStats bothCounts(const std::string& markdown, const std::string& what) {
    const std::vector<zametti::Piece> ir = pieces(markdown);
    QTextDocument doc;
    zametti::buildDocument(ir, doc);

    const zametti::NoteStats byIr = zametti::pieceStats(ir);
    const zametti::NoteStats byDoc = zametti::documentStats(doc);
    ++zt::g_checks;
    if (byIr.words != byDoc.words || byIr.lines != byDoc.lines ||
        byIr.blocks != byDoc.blocks || byIr.blocks != doc.blockCount() ||
        byIr.images != byDoc.images) {
        ++zt::g_failures;
        std::printf("провал: счёт по IR разошёлся с обходом документа (%s)\n"
                    "  по IR:      слов %d, строк %d, блоков %d\n"
                    "  по документу: слов %d, строк %d, блоков %d (blockCount %d)\n",
                    what.c_str(), byIr.words, byIr.lines, byIr.blocks, byDoc.words,
                    byDoc.lines, byDoc.blocks, doc.blockCount());
    }
    // Метки переносов — тоже: по ним считается строка каретки.
    ++zt::g_checks;
    if (byIr.marks.size() != byDoc.marks.size() ||
        !std::equal(byIr.marks.begin(), byIr.marks.end(), byDoc.marks.begin(),
                    [](const zametti::NoteStats::Mark& a, const zametti::NoteStats::Mark& b) {
                        return a.block == b.block && a.before == b.before;
                    })) {
        ++zt::g_failures;
        std::printf("провал: указатель строк разошёлся (%s): меток по IR %zu, по документу %zu\n",
                    what.c_str(), byIr.marks.size(), byDoc.marks.size());
    }
    return byIr;
}

void checkDocumentStats() {
    {
        const zametti::NoteStats s = bothCounts("", "пустой документ");
        check(s.words == 0, "пустой документ: ноль слов");
        check(s.lines == 1, "пустой документ: одна строка, а не ноль");
    }
    {
        // Абзац, пустая строка, абзац из двух строк исходника.
        const zametti::NoteStats s = bothCounts("Раз два три\n\nчетыре пять\nшесть\n", "три абзаца");
        check(s.words == 6, "шесть слов в трёх абзацах");
        // Строки: "Раз два три", пустая, "четыре пять", "шесть".
        check(s.lines == 4, "четыре строки, мягкий перенос считается строкой");
        check(s.blocks == 3, "блоков три: мягкий перенос блока не заводит");
    }
    {
        const zametti::NoteStats s = bothCounts("# Заголовок\n\n- пункт раз\n- пункт два\n", "список");
        // Ни решётки заголовка, ни дефисов пунктов в тексте документа нет.
        check(s.words == 5, "разметка словами не считается");
    }
    {
        const zametti::NoteStats s = bothCounts("![[img/foo-bar.jpg]]\n", "вики-вложение");
        check(s.words == 0, "путь к фотографии словами не считается");
    }
    {
        const zametti::NoteStats s = bothCounts("![подпись](img/foo-bar.jpg)\n", "image-спан");
        check(s.words == 0, "подпись фотографии словами не считается");
    }
    {
        // Код режется построчно: блоков столько же, сколько строк.
        const zametti::NoteStats s = bothCounts("```\nint main() {\n    return 0;\n}\n```\n", "код");
        check(s.lines == 3 && s.blocks == 3, "три строки кода — три блока");
    }
    {
        const zametti::NoteStats s = bothCounts("| a | b |\n|---|---|\n| 1 | 2 |\n", "таблица");
        check(s.lines == 3 && s.blocks == 3, "дословная таблица: строка на блок");
    }
    {
        // Каретка в середине второй строки второго абзаца.
        const std::string source = "Раз\n\nдва\nтри четыре\n";
        const zametti::NoteStats s = bothCounts(source, "каретка");
        auto doc = build(source);
        QTextCursor caret(doc.get());
        caret.movePosition(QTextCursor::End);
        const zametti::CaretPlace place = zametti::caretPlace(s, caret);
        check(s.lines == 4, "четыре строки");
        check(place.line == 4, "каретка на четвёртой строке");
        check(place.column == 11, "каретка в конце строки из десяти знаков");
    }
    {
        // Каретка перед мягким переносом и сразу после него.
        const std::string source = "раз\nдва\n";
        const zametti::NoteStats s = bothCounts(source, "каретка у переноса");
        auto doc = build(source);
        QTextCursor caret(doc.get());
        caret.setPosition(3);
        check(zametti::caretPlace(s, caret).line == 1, "перед переносом — первая строка");
        check(zametti::caretPlace(s, caret).column == 4, "перед переносом — четвёртый знак");
        caret.setPosition(4);
        check(zametti::caretPlace(s, caret).line == 2, "после переноса — вторая строка");
        check(zametti::caretPlace(s, caret).column == 1, "после переноса — первый знак");
    }
}

// Номер строки каретки по указателю обязан совпасть с прямым счётом по всему
// документу — в КАЖДОМ блоке, а не в одном выбранном.
void checkCaretEverywhere(const std::vector<zametti::Piece>& ir, QTextDocument& doc,
                          const std::string& what) {
    const zametti::NoteStats s = zametti::pieceStats(ir);
    int line = 1;
    for (QTextBlock block = doc.begin(); block.isValid(); block = block.next()) {
        QTextCursor caret(block);
        const zametti::CaretPlace place = zametti::caretPlace(s, caret);
        ++zt::g_checks;
        if (place.line != line) {
            ++zt::g_failures;
            std::printf("провал: строка каретки (%s), блок %d: ждали %d, вышло %d\n",
                        what.c_str(), block.blockNumber(), line, place.line);
            break;
        }
        line += zametti::countLineBreaks(block.text()) + 1;
    }
}

// --- прибор -----------------------------------------------------------------

void benchSource(const std::string& source, const std::string& label) {
    auto doc = build(source);
    QTextCursor caret(doc.get());
    caret.movePosition(QTextCursor::End);

    // Эталонный счётный цикл рядом: пока он стоит намертво, разброс в замере —
    // свойство измеряемого кода, а не машины.
    QElapsedTimer yard;
    yard.start();
    volatile double acc = 0;
    for (int i = 0; i < 20'000'000; ++i) acc += i * 1e-9;
    const qint64 yardUs = yard.nsecsElapsed() / 1000;

    const int rounds = 20;
    zametti::NoteStats stats;
    QElapsedTimer t;
    t.start();
    for (int i = 0; i < rounds; ++i) stats = zametti::documentStats(*doc);
    const qint64 fullUs = t.nsecsElapsed() / 1000;

    // Рабочий путь: тот же счёт по IR.
    const std::vector<zametti::Piece> parsed = pieces(source);
    t.restart();
    for (int i = 0; i < rounds; ++i) stats = zametti::pieceStats(parsed);
    const qint64 irUs = t.nsecsElapsed() / 1000;

    // Сам счёт слов в отрыве от всего прочего: по нему видно цену таблицы.
    long long onlyWords = 0;
    t.restart();
    for (int i = 0; i < rounds; ++i) onlyWords += zametti::countWords(std::string_view(source));
    const qint64 onlyUs = t.nsecsElapsed() / 1000;
    std::printf("  только счёт слов по UTF-8: %.0f мкс (%lld)\n", double(onlyUs) / rounds,
                onlyWords / rounds);

    // И место каретки по указателю строк — оно считается на каждое движение.
    const int moves = 100'000;
    t.restart();
    zametti::CaretPlace place;
    for (int i = 0; i < moves; ++i) place = zametti::caretPlace(stats, caret);
    const qint64 placeNs = t.nsecsElapsed();

    // Из чего складывается: обход блоков вообще, обход с чтением текста, счёт
    // слов, распознавание фотографии. Разбирается вычитанием.
    long long sink = 0;
    t.restart();
    for (int i = 0; i < rounds; ++i)
        for (QTextBlock b = doc->begin(); b.isValid(); b = b.next()) sink += b.length();
    const qint64 walkUs = t.nsecsElapsed() / 1000;

    t.restart();
    for (int i = 0; i < rounds; ++i)
        for (QTextBlock b = doc->begin(); b.isValid(); b = b.next()) sink += b.text().size();
    const qint64 textUs = t.nsecsElapsed() / 1000;

    t.restart();
    for (int i = 0; i < rounds; ++i)
        for (QTextBlock b = doc->begin(); b.isValid(); b = b.next())
            sink += zametti::countWords(b.text());
    const qint64 wordsUs = t.nsecsElapsed() / 1000;

    // Самый большой блок — худший случай пересчёта после одного нажатия.
    QTextBlock worst = doc->begin();
    for (QTextBlock b = doc->begin(); b.isValid(); b = b.next())
        if (b.length() > worst.length()) worst = b;
    const int taps = 100'000;
    t.restart();
    zametti::BlockStats one;
    for (int i = 0; i < taps; ++i) one = zametti::blockStats(worst);
    const qint64 oneNs = t.nsecsElapsed();

    // С чем сравнивать: во что обходится сама пересборка документа из IR и
    // сама подготовка автосохранения (чтение документа в IR и сериализация).
    // Счёт слов имеет право стоить лишь долю от них.
    t.restart();
    std::vector<zametti::Piece> ir;
    for (int i = 0; i < 3; ++i) ir = blocksOf(*doc);
    const qint64 readUs = t.nsecsElapsed() / 1000 / 3;

    t.restart();
    std::string bytes;
    for (int i = 0; i < 3; ++i) bytes = markdownOf(ir);
    const qint64 serializeUs = t.nsecsElapsed() / 1000 / 3;

    t.restart();
    QTextDocument fresh;
    for (int i = 0; i < 3; ++i) zametti::buildDocument(ir, fresh);
    const qint64 buildUs = t.nsecsElapsed() / 1000 / 3;

    std::printf("  для сравнения: документ→IR %lld мкс, IR→файл %lld мкс, "
                "IR→документ %lld мкс (байт %zu)\n",
                (long long)readUs, (long long)serializeUs, (long long)buildUs, bytes.size());

    std::printf("%s: %lld байт, %d блоков, %d слов, %d строк (переносов внутри блоков %d)\n"
                "  ПО IR: %.0f мкс; место каретки %.2f мкс (строка %d, знак %d)\n"
                "  обходом документа: %.0f мкс\n"
                "    обход блоков      %.0f мкс\n"
                "    + чтение текста   %.0f мкс\n"
                "    + счёт слов       %.0f мкс\n"
                "    + распознать фото %.0f мкс\n"
                "  худший блок (%d знаков): %.2f мкс на пересчёт, слов %d\n"
                "  эталон %lld мкс, acc %.0f, sink %lld\n",
                label.c_str(), (long long)source.size(), doc->blockCount(),
                stats.words, stats.lines, stats.lines - doc->blockCount(),
                double(irUs) / rounds, double(placeNs) / moves / 1000.0, place.line, place.column,
                double(fullUs) / rounds, double(walkUs) / rounds, double(textUs) / rounds,
                double(wordsUs) / rounds, double(fullUs - wordsUs) / rounds,
                int(worst.text().size()), double(oneNs) / taps / 1000.0, one.words,
                (long long)yardUs, double(acc), sink);
}

void bench(const std::filesystem::path& file) {
    benchSource(readFile(file), file.filename().string());
}

// Счёт по живым заметкам: сколько слов, сколько строк и насколько строки
// расходятся с блоками — от этого зависит, можно ли считать строки даром.
void survey(const std::filesystem::path& root) {
    const std::vector<std::filesystem::path> files = markdownFiles(root);
    long long words = 0, lines = 0, blocks = 0, bytes = 0;
    int notesWithBreaks = 0;
    double worstShare = 0.0;
    std::string worstNote;
    std::string all;      // весь корпус одной заметкой — потолок нагрузки
    std::filesystem::path biggest;
    size_t biggestSize = 0;

    for (const std::filesystem::path& file : files) {
        const std::string source = readFile(file);
        auto doc = build(source);
        const zametti::NoteStats s = zametti::documentStats(*doc);
        words += s.words;
        lines += s.lines;
        blocks += doc->blockCount();
        bytes += (long long)source.size();
        const int breaks = s.lines - doc->blockCount();
        if (breaks > 0) {
            ++notesWithBreaks;
            const double share = double(breaks) / s.lines;
            if (share > worstShare) { worstShare = share; worstNote = file.filename().string(); }
        }
        if (source.size() > biggestSize) { biggestSize = source.size(); biggest = file; }
        all += source;
        all += "\n\n";
    }

    std::printf("%s: заметок %zu, %lld байт, слов %lld, строк %lld, блоков %lld\n"
                "  мягких переносов внутри блоков: %lld (%.1f%% строк), в %d заметках из %zu\n"
                "  худшая доля переносов: %.0f%% (%s)\n",
                root.string().c_str(), files.size(), bytes, words, lines, blocks,
                lines - blocks, 100.0 * double(lines - blocks) / double(lines), notesWithBreaks,
                files.size(), 100.0 * worstShare, worstNote.c_str());

    if (!biggest.empty()) bench(biggest);
    benchSource(all, "весь корпус одной заметкой");
}

}  // namespace

static int ztRunSuite(int argc, char** argv) {

    std::vector<std::string> paths;
    bool benchmark = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--bench") benchmark = true;
        else paths.push_back(arg);
    }

    if (benchmark) {
        for (const std::string& path : paths) {
            const std::filesystem::path p = path;
            if (std::filesystem::is_directory(p)) survey(p);
            else bench(p);
        }
        return 0;
    }

    checkWordRules();
    checkRolesAgainstQt();
    checkLineRules();
    checkDocumentStats();

    // Сверка двух счётов по живым заметкам: там разметка, картинки, таблицы,
    // код и мягкие переносы вперемешку — того, что придумаешь руками, мало.
    for (const std::string& path : paths) {
        const std::vector<std::filesystem::path> files = markdownFiles(path);
        if (files.empty()) {
            std::printf("в каталоге %s нет .md\n", path.c_str());
            return 1;
        }
        for (const std::filesystem::path& file : files) {
            const std::string source = readFile(file);
            const std::string what = file.filename().string();
            bothCounts(source, what);
            const std::vector<zametti::Piece> ir = pieces(source);
            QTextDocument doc;
            zametti::buildDocument(ir, doc);
            checkCaretEverywhere(ir, doc, what);
        }
        std::printf("%s: заметок %zu\n", path.c_str(), files.size());
    }

    return zt::report("text-stats");
}

// Набор целиком одним TEST: тело не тронуто, argv ему собран здесь.
// Дробить на отдельные проверки — отдельная работа, по одному набору.
TEST(TextStats, All) {
    std::vector<QByteArray> ztArgs{QByteArrayLiteral("text_stats_test")};
    std::vector<char*> ztArgv;
    for (QByteArray& a : ztArgs) ztArgv.push_back(a.data());
    EXPECT_EQ(0, ztRunSuite(int(ztArgv.size()), ztArgv.data()));
}
