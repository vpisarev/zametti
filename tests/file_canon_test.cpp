// Канон записи: то, что уходит в файл, читается назад ТЕМ ЖЕ и на втором
// круге не меняется. Каждая проверка здесь — приколотый случай фаззера
// операций (03–04.09.2026): дословный кусок, потерявший причину дословности;
// пункты рядом с дословным списком; поглощение соседей дословным куском в
// читателе; зачёркивание поверх кода; курсив, кончающийся знаком препинания.

#include "document.h"
#include "document_pieces.h"
#include "pieces.h"
#include "serializer.h"

#include "test_util.h"

#include <QString>
#include <QTextBlock>
#include <QTextCursor>

#include <string>
#include <vector>

using namespace zametti;

namespace {

std::string bytesOf(const QString& text) { return text.toStdString(); }

// Два круга: заметка → файл → заметка → файл. Совпасть обязаны и байты обоих
// кругов, и блоки: то, что пошло в файл, и то, что из него прочлось.
void checkStable(const std::string& source, const char* what) {
    ZDocument first = bodyOf(source);
    std::vector<Piece> forFile;
    const QByteArray bytes = first.fileBytes(NoteHeader{}, &forFile);
    std::vector<Piece> reread;
    NoteHeader header;
    parsePieces(normaliseSpaces(QString::fromUtf8(bytes)), reread, header);
    ZT_EQ(std::string(what) + ": блоки записи и чтения", dumpPieces(forFile), dumpPieces(reread));
    ZDocument second = bodyOf(std::string(bytes.constData(), size_t(bytes.size())));
    const QByteArray again = second.fileBytes(NoteHeader{});
    ZT_EQ(std::string(what) + ": второй круг записи",
          std::string(bytes.constData(), size_t(bytes.size())),
          std::string(again.constData(), size_t(again.size())));
}

Piece rawPiece(const char* text, int level = -1) {
    Piece p;
    p.raw = true;
    p.level = level;
    p.text = QString::fromUtf8(text);
    return p;
}

}  // namespace

// СУДЬЯ ДОСЛОВНОГО КУСКА. Кусок, который сам по себе больше не читается
// дословным, идёт в файл тем, во что он распался.
TEST(FileCanon, DecayedRawBecomesParagraph) {
    std::vector<Piece> doc{rawPiece("#include <i хвостeturn 0;\n}\n")};
    const std::vector<Piece> out = documentForFile(doc);
    ASSERT_EQ(1u, out.size());
    EXPECT_FALSE(out[0].raw);
    EXPECT_EQ(int(Kind::Paragraph), int(out[0].kind));
    EXPECT_EQ(QStringLiteral("#include <i хвостeturn 0;\n}"), out[0].text);
}

TEST(FileCanon, RawWithHtmlStaysRaw) {
    std::vector<Piece> doc{rawPiece("#include <iostream>\n")};
    const std::vector<Piece> out = documentForFile(doc);
    ASSERT_EQ(1u, out.size());
    EXPECT_TRUE(out[0].raw);
    EXPECT_EQ(QStringLiteral("#include <iostream>\n"), out[0].text);
}

// Правка внутри дословного куска в живой заметке: набор снимает HTML — файл
// обязан остаться читаемым в тот же документ.
TEST(FileCanon, EditedRawRoundTrips) {
    ZDocument note = bodyOf("#include <iostream>\n\nafter\n");
    QTextCursor at = note.caretAtBlock(0);
    at.setPosition(at.block().position() + 11);   // за «<i»
    ASSERT_TRUE(note.insertText(at, QStringLiteral(" хвост")));
    std::vector<Piece> forFile;
    const QByteArray bytes = note.fileBytes(NoteHeader{}, &forFile);
    std::vector<Piece> reread;
    NoteHeader header;
    parsePieces(QString::fromUtf8(bytes), reread, header);
    EXPECT_EQ(dumpPieces(forFile), dumpPieces(reread));
    checkStable(std::string(bytes.constData(), size_t(bytes.size())), "правленый дословный кусок");
}

// Голый адрес в распавшемся куске — ссылка уже на первой записи.
TEST(FileCanon, DecayedRawUrlIsEnriched) {
    std::vector<Piece> doc{rawPiece(
        "https://github.com/econsystems/opencv_v4l2/blob/master/results/test_results.txt\n"
        "GLFW: https://www.glfw.org\n")};
    const std::vector<Piece> out = documentForFile(doc);
    ASSERT_EQ(1u, out.size());
    EXPECT_FALSE(out[0].raw);
    EXPECT_FALSE(out[0].runs.empty()) << dumpPieces(out);
    const QString text = writePieces(out);
    std::vector<Piece> back;
    NoteHeader header;
    parsePieces(text, back, header);
    EXPECT_EQ(dumpPieces(out), dumpPieces(back));
}

// И голый адрес в ОБЫЧНОМ абзаце (без дословности) — ссылка уже на первой
// записи: разметка чтения богаче, и приведение её принимает.
TEST(FileCanon, PlainParagraphWithUrlIsEnriched) {
    Piece p;
    p.kind = Kind::Paragraph;
    p.text = QStringLiteral(
        "https://github.com/econsystems/opencv_v4l2/blob/master/results/test_results.txt\n"
        "GLFW: https://www.glfw.org");
    const std::vector<Piece> out = documentForFile({p});
    ASSERT_EQ(1u, out.size()) << dumpPieces(out);
    EXPECT_FALSE(out[0].runs.empty()) << dumpPieces(out) << writePieces({p}).toStdString();
    checkStable(p.text.toStdString() + "\n", "голый адрес в абзаце");
}

// Зачёркнутый адрес: чтение даёт зачёркнутую ССЫЛКУ — обогащение, а не потеря
// разметки; зачёркивание остаётся, адрес становится ссылкой.
TEST(FileCanon, StruckUrlKeepsStrikeAndBecomesLink) {
    Piece p;
    p.kind = Kind::Paragraph;
    p.text = QStringLiteral("https://github.com/econsystems/opencv_v4l2/blob/master/results/test_results.txt");
    zametti::Run strike;
    strike.start = 0;
    strike.end = int32_t(p.text.size());
    strike.set(InlineStrike, true);
    p.runs = {strike};
    const std::vector<Piece> out = documentForFile({p});
    ASSERT_EQ(1u, out.size()) << dumpPieces(out);
    ASSERT_FALSE(out[0].runs.empty()) << dumpPieces(out);
    EXPECT_TRUE(out[0].runs[0].strike()) << dumpPieces(out);
    EXPECT_FALSE(out[0].runs[0].href.isEmpty()) << dumpPieces(out);
    checkStable(writePieces(out).toStdString(), "зачёркнутый адрес");
}

// Абзац из двух строк: ссылка на первой, жирный текст и жирная ссылка на
// второй — вся разметка обязана пережить приведение (случай фаззера: строки
// слиплись после Delete на стыке абзацев).
TEST(FileCanon, LinksAndBoldAcrossSoftBreakSurvive) {
    Piece p;
    p.kind = Kind::Paragraph;
    p.text = QStringLiteral(
        "https://github.com/econsystems/opencv_v4l2/blob/master/results/test_results.txt\n"
        "GLFW: https://www.glfw.org");
    zametti::Run link;
    link.start = 0;
    link.end = 79;
    link.href = QStringLiteral("https://github.com/econsystems/opencv_v4l2/blob/master/results/test_results.txt");
    zametti::Run bold;
    bold.start = 80;
    bold.end = 85;
    bold.set(InlineBold, true);
    zametti::Run boldLink;
    boldLink.start = 86;
    boldLink.end = 106;
    boldLink.set(InlineBold, true);
    boldLink.href = QStringLiteral("https://www.glfw.org");
    p.runs = {link, bold, boldLink};
    {
        // Та же ссылка, но с зачёркнутым куском внутри её текста.
        Piece q = p;
        zametti::Run a = boldLink, b = boldLink, c = boldLink;
        a.end = 99;
        b.start = 99; b.end = 105; b.set(InlineStrike, true);
        c.start = 105;
        q.runs = {link, bold, a, b, c};
        const std::vector<Piece> outQ = documentForFile({q});
        std::vector<Piece> backQ;
        NoteHeader h;
        const QString probeQ = writePieces({q});
        parsePieces(probeQ, backQ, h);
        EXPECT_FALSE(outQ[0].runs.empty()) << dumpPieces(outQ) << "\nпроба:\n" << probeQ.toStdString()
                                           << "\nразбор пробы:\n" << dumpPieces(backQ);
        checkStable(writePieces(outQ).toStdString(), "зачёркнутый кусок внутри текста ссылки");
    }
    const std::vector<Piece> out = documentForFile({p});
    ASSERT_EQ(1u, out.size()) << dumpPieces(out);
    std::vector<Piece> back;
    NoteHeader header;
    const QString probe = writePieces({p});
    parsePieces(probe, back, header);
    EXPECT_EQ(3u, out[0].runs.size()) << dumpPieces(out) << "\nпроба:\n" << probe.toStdString()
                                       << "\nразбор пробы:\n" << dumpPieces(back);
    checkStable(writePieces(out).toStdString(), "ссылки и жирный через мягкий перенос");
}

// РАЗВОД МАРКЕРОВ. Пункты заметки рядом с дословным списком того же рода
// выводятся другим знаком — иначе чтение слило бы их в один дословный список.
TEST(FileCanon, ItemsBeforeRawListChangeBullet) {
    checkStable("* [ ] a\n* [ ] b\n\n- [x] c <u>x</u>\n- [x] d\n", "пункты перед дословным списком");
    ZDocument note = bodyOf("* [ ] a\n* [ ] b\n\n- [x] c <u>x</u>\n- [x] d\n");
    const std::string text = note.toMarkdown();
    EXPECT_EQ(0u, text.find("* [ ] a\n* [ ] b\n\n- [x] c <u>x</u>")) << text;
}

TEST(FileCanon, ItemsAfterRawListChangeBullet) {
    checkStable("- [x] c <u>x</u>\n- [x] d\n\n* [ ] a\n* [ ] b\n", "пункты после дословного списка");
}

TEST(FileCanon, OrderedItemsNextToRawOrderedList) {
    checkStable("1) a\n2) b\n\n1. c <u>x</u>\n2. d\n", "номера перед дословным нумерованным");
}

// ЧИТАТЕЛЬ НЕ ПОГЛОЩАЕТ СОСЕДЕЙ. Дословный список и список ДРУГОГО рода за ним —
// два блока; абзац с отступом и жёстким переносом не забирает абзац над собой.
TEST(FileCanon, RawListDoesNotSwallowOtherList) {
    std::vector<Piece> pieces;
    NoteHeader header;
    parsePieces(QStringLiteral("- a <u>x</u>\n\n1. c\n"), pieces, header);
    ASSERT_EQ(3u, pieces.size()) << dumpPieces(pieces);
    EXPECT_TRUE(pieces[0].raw);
    EXPECT_FALSE(pieces[2].raw);
    EXPECT_EQ(int(Kind::ListItem), int(pieces[2].kind));
    checkStable("- a <u>x</u>\n\n1. c\n", "дословный список и нумерованный за ним");
}

TEST(FileCanon, IndentedRawDoesNotSwallowParagraphAbove) {
    std::vector<Piece> pieces;
    NoteHeader header;
    parsePieces(QStringLiteral("b10 = x;\n\n    r11 = y;    \n    g11\n"), pieces, header);
    ASSERT_EQ(3u, pieces.size()) << dumpPieces(pieces);
    EXPECT_FALSE(pieces[0].raw);
    EXPECT_TRUE(pieces[2].raw);
    checkStable("b10 = x;\n\n    r11 = y;    \n    g11\n", "абзац над дословным с отступом");
}

// ЗАЧЁРКИВАНИЕ И КОД. Растяжение зачёркивания до границ слова не заходит в код
// в кавычках, а начертание внутри кода не хранится: код буквален.
TEST(FileCanon, StrikeStopsAtCodeSpan) {
    checkStable("## [[.`./attach/rise_di`~~et~~.png]]\n", "зачёркивание за кодом в вики-вставке");
    checkStable("a `rise_di`~~et~~ b\n", "зачёркивание сразу за кодом");
}

// ФЛАНКИРОВАНИЕ. Курсив, кончающийся знаком препинания перед буквой, markdown
// закрыть не может: край поджимается до выразимого, и чтение возвращает тот же
// кусок, а не курсив на всём тексте.
TEST(FileCanon, EmphasisEdgesAreFlankable) {
    Piece heading;
    heading.kind = Kind::Heading;
    heading.headingLevel = 2;
    // Случай фаззера был с U+2028 между звёздочкой и решёткой; с канона
    // 04.09.2026 разделитель в заголовке — пробел, суть случая (края курсива)
    // от этого не меняется.
    heading.text = QStringLiteral("* #а`");
    zametti::Run first;
    first.start = 0;
    first.end = 3;
    first.set(InlineItalic, true);
    zametti::Run second;
    second.start = 4;
    second.end = 5;
    second.set(InlineItalic, true);
    heading.runs = {first, second};
    const std::vector<Piece> out = documentForFile({heading});
    ASSERT_EQ(1u, out.size());
    const QString text = writePieces(out);
    std::vector<Piece> back;
    NoteHeader header;
    parsePieces(text, back, header);
    ASSERT_EQ(1u, back.size());
    EXPECT_EQ(dumpPieces(out), dumpPieces(back)) << text.toStdString();
    checkStable(bytesOf(text), "курсив на знаке препинания");
}

// Смежные куски, делящие начертание: ограничитель общего начертания на стыке
// не ставится, и край проверяется только там, где ограничитель есть.
TEST(FileCanon, SharedStyleAcrossAdjacentRuns) {
    checkStable("- **It quantifies the Aberrance ~~Retention Quotient (ARQ).~~**\n",
                "жирный поверх зачёркнутого хвоста");
    checkStable("## _Test and profile the ~~implement solutions~~_\n", "курсив поверх зачёркнутого");
    checkStable("## Поправить для автоматическо*го учета ~~разрешения, выставленно~~*~~го в самой сетке~~\n",
                "курсив и зачёркивание внахлёст");
    checkStable("2. ~~rge, https://example.commatte glass walls~~\n", "адрес внутри зачёркнутого");
    checkStable("- **https://example.~~comпроверить сдвиг на (-1, -1)~~**\n",
                "адрес с точкой на стыке начертаний");
    // Ссылка внутри начертания продолжает его: смежность через неё не рвётся.
    checkStable("2. ~~rge, https://example.commatte glass walls. Nice~~\n",
                "адрес внутри зачёркнутого");
    checkStable("~~NoSQL: MongoDB, ~~**Apache Cassandra** (~~**https://www.bigdataschool.ru/wiki/cassandra**~~; "
                "~~**https://docs.datastax.com/en/x.html**~~ ~~**)**\n",
                "зачёркнутое с жирными ссылками");
    checkStable("- _все функции. Например, отсюда:_ **_http://gruntthepeon.free.fr/ssemath/_ _(этот же автор)_**\n",
                "жирное поверх курсива со ссылкой");
}

// АБЗАЦ ЗА КОММЕНТАРИЕМ ВНУТРИ ПУНКТА плотного списка приходит от md4c без
// своего блока; его первая строка — начало строки, и отступ на ней свой.
TEST(FileCanon, ParagraphAfterCommentInTightItemKeepsIndent) {
    std::vector<Piece> pieces;
    NoteHeader header;
    parsePieces(QStringLiteral("- [ ] a\n  <!-- x -->\n    }[ ]\n    return;\n  }\n"), pieces, header);
    ASSERT_EQ(3u, pieces.size()) << dumpPieces(pieces);
    EXPECT_EQ(QStringLiteral("  }[ ]\n  return;\n}"), pieces[2].text);
    checkStable("- [ ] a\n  <!-- x -->\n    }[ ]\n    return;\n  }\n", "абзац за комментарием в пункте");
}

// ЗАБОРЫ У normaliseSpaces — ПО COMMONMARK: черта из тильд внутри блока кода в
// кавычках его не закрывает. Иначе состояние «внутри кода» переворачивалось
// до конца файла, и ведущие неразрывные абзаца после списка становились
// обычными пробелами — абзац въезжал в пункт.
TEST(FileCanon, TildeLineInsideBacktickFenceKeepsSpacesCanon) {
    const std::string source =
        "```\ncode\n~~~~~~~~\nmore\n```\n\n2. a\n   b\n\n\xc2\xa0\xc2\xa0\xc2\xa0(x)\n   y\n";
    ZDocument note = bodyOf(source);
    std::vector<Piece> forFile;
    note.fileBytes(NoteHeader{}, &forFile);
    ASSERT_GE(forFile.size(), 4u) << dumpPieces(forFile);
    const Piece& after = forFile.back();
    EXPECT_EQ(int(Kind::Paragraph), int(after.kind));
    EXPECT_EQ(-1, after.level) << dumpPieces(forFile);
    checkStable(source, "черта из тильд внутри кода в кавычках");
}

// АБЗАЦ С ОТСТУПОМ СРАЗУ ПОСЛЕ СПИСКА остаётся снаружи списка: неразрывные
// в нулевой колонке список закрывают, и на втором круге он не въезжает в пункт.
TEST(FileCanon, IndentedParagraphAfterListStaysOutside) {
    checkStable("2. a\n   b\n\n   (env0)\n   where\n\n   next\n",
                "абзац с отступом после списка");
}

// --- СЛУЖЕБНЫЕ ЗНАКИ В ФАЙЛЕ НЕ ЖИВУТ (просьба владельца 04.09.2026) ---------
//
// Разделители строк юникода (U+2028, U+2029) в файле заметки — не содержимое,
// а перевод строки: так их пишет Apple Notes, так же они получались у нас из
// Qt-шного Shift+Enter мимо пометки. Файл с ними МИГРИРУЕТ при первом
// сохранении (как U+00A0), вставка из буфера приводится той же дверью, а
// непомеченный разделитель, всё же оказавшийся в документе, писатель отдаёт
// переводом строки (в заголовке — пробелом: заголовок однострочен).

namespace {

bool hasLineSeparators(const QByteArray& bytes) {
    const QString text = QString::fromUtf8(bytes);
    return text.contains(QChar::LineSeparator) || text.contains(QChar::ParagraphSeparator);
}

}  // namespace

TEST(FileCanon, LineSeparatorsInFileBecomeNewlines) {
    ZDocument note = bodyOf("раз\xE2\x80\xA8два\n\nтри\xE2\x80\xA9четыре\n");
    const QByteArray bytes = note.fileBytes(NoteHeader{});
    EXPECT_FALSE(hasLineSeparators(bytes));
    EXPECT_EQ("раз\nдва\n\nтри\nчетыре\n", bytes.toStdString());
    checkStable(bytes.toStdString(), "перевод строки вместо разделителя");
}

TEST(FileCanon, PastedLineSeparatorsBecomeNewlines) {
    // Разбором (обычная вставка) — в абзац.
    ZDocument note = bodyOf("абзац\n");
    QTextCursor at = note.caretAtBlock(0);
    at.movePosition(QTextCursor::EndOfBlock);
    ASSERT_TRUE(note.replaceRange(at, QStringLiteral("x\u2028y"), ZDocument::PasteMode::Markdown));
    const QByteArray bytes = note.fileBytes(NoteHeader{});
    EXPECT_FALSE(hasLineSeparators(bytes));
    EXPECT_EQ("абзацx\ny\n", bytes.toStdString());

    // Буквально — в блок кода: строки кода, а не знак внутри строки.
    ZDocument code = bodyOf("```\ncode\n```\n");
    QTextCursor in = code.caretAtBlock(0);
    in.movePosition(QTextCursor::EndOfBlock);
    ASSERT_TRUE(code.replaceRange(in, QStringLiteral("\u2028a\u2029b"),
                                  ZDocument::PasteMode::Literal));
    const QByteArray codeBytes = code.fileBytes(NoteHeader{});
    EXPECT_FALSE(hasLineSeparators(codeBytes));
    EXPECT_EQ("```\ncode\na\nb\n```\n", codeBytes.toStdString());
}

TEST(FileCanon, TypedLineSeparatorIsWrittenAsNewline) {
    ZDocument note = bodyOf("абзац\n\n# Заголовок\n");
    QTextCursor at = note.caretAtBlock(0);
    at.movePosition(QTextCursor::EndOfBlock);
    ASSERT_TRUE(note.insertText(at, QStringLiteral("\u2028хвост")));
    QTextCursor head = note.caretAtBlock(2);
    head.movePosition(QTextCursor::EndOfBlock);
    ASSERT_TRUE(note.insertText(head, QStringLiteral("\u2028хвост")));
    const QByteArray bytes = note.fileBytes(NoteHeader{});
    EXPECT_FALSE(hasLineSeparators(bytes));
    EXPECT_EQ("абзац\nхвост\n\n# Заголовок хвост\n", bytes.toStdString());
}

// ПУСТОЙ ПУНКТ — ОДНО ПОНЯТИЕ НА ВСЕХ (фаззер, случай 4, 03.09.2026). Задача
// без текста «- [ ]» для md4c НЕ пуста: содержимое — сам чекбокс, и абзац с
// отступом через пустую строку после неё читается ВНУТРИ пункта (замерено на
// md4c). Проход отцепления считал её пустой и выносил абзац наружу (уровень
// −1), а файл читался с абзацем внутри — и не устаивался.
TEST(FileCanon, IndentedParagraphAfterEmptyTaskStaysInside) {
    std::vector<Piece> forFile;
    const std::string src = "- [ ]\n\n    ~~ки~~\n";
    const QByteArray bytes = bodyOf(src).fileBytes(NoteHeader{}, &forFile);
    ASSERT_EQ(3u, forFile.size()) << bytes.toStdString();
    EXPECT_EQ(0, forFile.back().level) << bytes.toStdString();
    checkStable(src, "абзац с отступом после пустой задачи");
}

// ТИЛЬДЫ — ПО ПРАВИЛУ md4c, А НЕ ПО ФЛАНКИРОВАНИЮ ЭМФАЗЫ (фаззер, случай 1;
// класс 1 из known_bugs 31.08). md4c спаривает одиночные тильды через пробелы
// и переносы строк: «a ~ b ~ c» читается зачёркиванием « b », «~\n~» —
// зачёркиванием перевода строки. Писатель считал такую тильду не открывающей
// (эмфаза требует не-пробела рядом) и не экранировал.
TEST(FileCanon, LoneTildesThatPairAreEscaped) {
    // Из живого документа: абзац с таким ТЕКСТОМ (без разметки) обязан уйти в
    // файл так, чтобы прочитаться тем же абзацем.
    for (const char* text : {"~\n~", "a ~ b ~ c", "[продал]~\n~\n~"}) {
        Piece p;
        p.kind = Kind::Paragraph;
        p.text = QString::fromUtf8(text);
        std::vector<Piece> forFile = documentForFile({p});
        const QString bytes = writePieces(forFile);
        std::vector<Piece> back;
        NoteHeader header;
        parsePieces(bytes, back, header);
        EXPECT_EQ(dumpPieces(forFile), dumpPieces(back)) << bytes.toStdString();
    }
    checkStable("~\n~\n", "две тильды на своих строках");
    checkStable("a ~ b ~ c\n", "одиночные тильды через пробелы");
}

// И ЧИТАТЕЛЬ: спан из одних пробелов и переносов — не разметка, а буквы.
// Иначе withTrimmedSpans снимал бы пустое зачёркивание, а тильды из файла
// пропадали при следующей записи.
TEST(FileCanon, WhitespaceOnlySpanKeepsItsTildes) {
    const std::string src = "x\n~\n~\ny\n";
    const QByteArray bytes = bodyOf(src).fileBytes(NoteHeader{});
    EXPECT_NE(std::string::npos, bytes.toStdString().find("~")) << bytes.toStdString();
    checkStable(src, "тильды на своих строках между абзацами");
}
