// СТРОЧНАЯ ФОРМУЛА — ОБЪЕКТ НА КАЖДОЕ ВХОЖДЕНИЕ (сессия 6 refactor2, бриф
// zametti-brief-inline-formulas.md).
//
// Модельная сторона: `$…$` внутри абзаца сборщик превращает в один знак
// U+FFFC с исходником в свойстве формата, писатель возвращает исходник, и круг
// файла проходит байт в байт. Приёмка — на КОПИИ настоящей заметки владельца
// «Typesetting Math in Markdown» (.testdata/typesetting-math.md): именно её он
// и просил отрендерить, и именно на ней считаются вхождения.
//
// Набранные руками доллары формулой НЕ становятся (семантика писателя:
// dollarOpensMath экранирует, math_ir_test это стережёт) — объекты рождают
// только файл, вставка и жест математики; здесь проверяется, что цены и
// экранированные доллары объектами не стали.

#include "doc_model.h"
#include "settings.h"
#include "formula_object.h"
#include "formula.h"
#include "math_scan.h"
#include "note_search.h"
#include "search.h"
#include "text_stats.h"
#include "note_view.h"
#include "pieces.h"
#include "test_util.h"
#include "testdata.h"

#include <QTextBlock>

#include <memory>
#include <QTextCharFormat>
#include <QTextDocument>
#include <QTextFragment>

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using zametti::InlineFormulaObject;
using zametti::Piece;
using zametti::Run;

namespace {

std::string readFile(const QString& path) {
    std::ifstream in(path.toLocal8Bit().constData(), std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Все строчные формулы-объекты документа: исходники по порядку, через «|».
struct ObjectSweep {
    int objects = 0;
    QString sources;                 // через |
    bool allBaseline = true;         // у всех — посадка AlignBaseline
};

ObjectSweep sweepInlineObjects(const QTextDocument& doc) {
    ObjectSweep sweep;
    for (QTextBlock block = doc.begin(); block.isValid(); block = block.next()) {
        for (QTextBlock::iterator it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            if (!fragment.isValid()) continue;
            const QTextCharFormat format = fragment.charFormat();
            if (format.objectType() != InlineFormulaObject) continue;
            for (int n = 0; n < fragment.length(); ++n) {
                ++sweep.objects;
                if (!sweep.sources.isEmpty()) sweep.sources += QLatin1Char('|');
                sweep.sources += format.property(zametti::ObjectSourceProperty).toString();
            }
            if (format.verticalAlignment() != QTextCharFormat::AlignBaseline)
                sweep.allBaseline = false;
        }
    }
    return sweep;
}

// Сколько закрытых цельных math-спанов дал бы разбор этого текста — столько
// объектов обязан дать сборщик. Считается по тем же кускам, что видит он.
int expectedInlineObjects(const std::vector<Piece>& blocks) {
    int count = 0;
    for (const Piece& b : blocks) {
        if (b.raw || (b.kind != zametti::Kind::Paragraph && b.kind != zametti::Kind::ListItem &&
                      b.kind != zametti::Kind::Quote))
            continue;
        for (const Run& s : b.runs)
            if (s.math() && !s.mathOpen() && zametti::wholeMath(b.view(s))) ++count;
    }
    return count;
}

// Документ заметки — через вид: живой QTextDocument отдаётся наружу только
// затем, чтобы его показать (правило getDocument).
struct Shown {
    zametti::ZNote note;
    zametti::NoteView view;
    QTextDocument* doc = nullptr;
};

void show(Shown& s, std::string_view markdown) {
    s.note.load(markdown);
    s.view.setDocument(s.note.doc().getDocument());
    s.doc = s.view.document();
}

// --- сборка: спан → объект ----------------------------------------------------

void checkBuildsObjects() {
    Shown s;
    show(s, "Если $a$ целое, то $2a+1$ нечётно, и снова $a$ рядом.\n");
    const ObjectSweep sweep = sweepInlineObjects(*s.doc);
    ZT_EQ("объект на каждое вхождение", "3", std::to_string(sweep.objects));
    ZT_EQ("исходники с долларами, по порядку", "$a$|$2a+1$|$a$",
          sweep.sources.toStdString());
    ZT_TRUE("посадка каждой — AlignBaseline", sweep.allBaseline);

    // Текст блока: на месте формул — знаки объекта, буквы целы.
    const QTextBlock block = s.doc->firstBlock();
    ZT_EQ("текст блока — буквы и знаки объектов",
          utf8(QStringLiteral("Если ￼ целое, то ￼ нечётно, и снова ￼ рядом.")),
          utf8(block.text()));
    // Исходник блока для файла, буфера и счёта — с формулами на местах.
    ZT_EQ("sourceTextOf возвращает исходник",
          "Если $a$ целое, то $2a+1$ нечётно, и снова $a$ рядом.",
          utf8(zametti::sourceTextOf(block)));

    // Круг файла.
    ZT_EQ("круг байт в байт", "Если $a$ целое, то $2a+1$ нечётно, и снова $a$ рядом.\n",
          s.note.doc().toMarkdown());
}

// Формула, перенесённая через строку исходника, — законна (канон), и перенос
// живёт в исходнике объекта, а не в тексте блока.
void checkMultilineSpan() {
    Shown s;
    show(s, "Список: $N\n\\approx M$, дальше текст.\n");
    const ObjectSweep sweep = sweepInlineObjects(*s.doc);
    ZT_EQ("одна формула", "1", std::to_string(sweep.objects));
    ZT_EQ("перенос — внутри исходника", "$N\n\\approx M$", sweep.sources.toStdString());
    ZT_EQ("круг байт в байт", "Список: $N\n\\approx M$, дальше текст.\n",
          s.note.doc().toMarkdown());
}

// Не математика объектом не становится: цены, экранированные доллары,
// формула в заголовке (решение сессии: заголовок — литерально).
void checkNonMathStaysText() {
    {
        // Цены — не математика (канон: закрывающий доллар не перед цифрой), и
        // экранировать их писателю незачем: круг проходит без косых.
        Shown s;
        show(s, "Цена $5 и $10 без формул.\n");
        ZT_EQ("цены — текст, не объекты", "0",
              std::to_string(sweepInlineObjects(*s.doc).objects));
        ZT_EQ("и круг цел", "Цена $5 и $10 без формул.\n", s.note.doc().toMarkdown());
    }
    {
        // А вот `\$x$` — доллар, который БЕЗ косой открыл бы формулу: писатель
        // обязан удержать косую (экранируется только открывающий — канон, тот
        // же край в math_ir_test), а сборщик — не сделать объект.
        Shown s;
        show(s, "Литерально \\$x$ тут.\n");
        ZT_EQ("экранированная пара — текст", "0",
              std::to_string(sweepInlineObjects(*s.doc).objects));
        ZT_EQ("косая удержана", "Литерально \\$x$ тут.\n", s.note.doc().toMarkdown());
    }
    {
        Shown s;
        show(s, "# Заголовок с $x^2$ внутри\n\nАбзац с $x^2$ внутри.\n");
        const ObjectSweep sweep = sweepInlineObjects(*s.doc);
        ZT_EQ("в заголовке — литерально, в абзаце — объект", "1",
              std::to_string(sweep.objects));
        ZT_EQ("круг цел", "# Заголовок с $x^2$ внутри\n\nАбзац с $x^2$ внутри.\n",
              s.note.doc().toMarkdown());
    }
    {
        // Пункт списка и цитата — как абзац.
        Shown s;
        show(s, "- пункт с $\\alpha$\n\n> цитата с $\\beta$\n");
        ZT_EQ("пункт и цитата берут объекты", "2",
              std::to_string(sweepInlineObjects(*s.doc).objects));
    }
}

// ФОРМУЛЫ, ПРИКЛЕЕННЫЕ К БУКВАМ, — законная математика канона (pandoc):
// «$\Pi$иф$\alpha$гор» — две формулы, «слово$\gamma$слово» — одна. Нашёл
// владелец: md4c мерил границы `$` по флангам эмфазиса (не после буквы / не
// перед буквой), склеивал первый случай в один неверный спан «$\Pi$иф$», а
// второй не видел вовсе — и писатель, судящий нашим каноном, оборачивал такой
// текст косыми при каждой записи. Вендоренному md4c поправлены границы
// ОДИНОЧНОГО `$` под канон (math_scan.h); `$$` и `~` не тронуты.
void checkGluedFormulas() {
    {
        Shown s;
        show(s, "$\\Pi$иф$\\alpha$гор — теорема.\n");
        const ObjectSweep sweep = sweepInlineObjects(*s.doc);
        ZT_EQ("две формулы, приклеенные к буквам", "$\\Pi$|$\\alpha$",
              sweep.sources.toStdString());
        ZT_EQ("круг байт в байт", "$\\Pi$иф$\\alpha$гор — теорема.\n",
              s.note.doc().toMarkdown());
    }
    {
        Shown s;
        show(s, "слово$\\gamma$слово\n");
        ZT_EQ("формула в середине слова", "$\\gamma$",
              sweepInlineObjects(*s.doc).sources.toStdString());
        ZT_EQ("круг байт в байт — косые не растут", "слово$\\gamma$слово\n",
              s.note.doc().toMarkdown());
    }
    {
        Shown s;
        show(s, "$a$b и хвост.\n");
        ZT_EQ("буква сразу за закрывающим долларом законна", "$a$",
              sweepInlineObjects(*s.doc).sources.toStdString());
        ZT_EQ("круг цел", "$a$b и хвост.\n", s.note.doc().toMarkdown());
    }
}

// Выключная — по-прежнему блочный объект: одиночные доллара своей строкой
// показываются выключной (liftMath), строчным объектом они не становятся.
void checkDisplayStaysBlock() {
    Shown s;
    show(s, "До.\n\n$e = mc^2$\n\nПосле.\n");
    ZT_EQ("строчных объектов нет", "0", std::to_string(sweepInlineObjects(*s.doc).objects));
    int mathBlocks = 0;
    for (QTextBlock b = s.doc->begin(); b.isValid(); b = b.next())
        if (!zametti::isRawBlock(b) && zametti::kindOf(b) == zametti::Kind::Math) ++mathBlocks;
    ZT_EQ("формула-блок одна", "1", std::to_string(mathBlocks));
    ZT_EQ("круг цел", "До.\n\n$e = mc^2$\n\nПосле.\n", s.note.doc().toMarkdown());
}

// --- флип: объект ⇄ исходник на месте -------------------------------------------

void checkFlip() {
    Shown s;
    show(s, "До $x^2$ после.\n");
    // Так делает редактор, взяв документ себе: отмена — штатная, Qt.
    s.doc->setUndoRedoEnabled(true);
    zametti::ZDocument& d = s.note.doc();

    const QTextBlock block = s.doc->firstBlock();
    const int knob = block.text().indexOf(QChar::ObjectReplacementCharacter);
    ZT_TRUE("знак объекта на месте", knob >= 0);

    QTextCursor at(s.doc);
    at.setPosition(block.position() + knob);
    ZT_TRUE("раскрылась", d.openInlineFormula(at));
    ZT_EQ("исходник на месте, блок тот же", "До $x^2$ после.",
          utf8(s.doc->firstBlock().text()));
    ZT_TRUE("кусок помечен раскрытым", zametti::hasOpenInlineFormula(s.doc->firstBlock()));
    ZT_EQ("каретка в начале исходника", std::to_string(block.position() + int(knob)),
          std::to_string(at.position()));
    ZT_EQ("объектов нет, пока раскрыта", "0",
          std::to_string(sweepInlineObjects(*s.doc).objects));
    // Запись в раскрытом виде — литеральная: косые не растут (math() истинен).
    ZT_EQ("запись раскрытой литеральна", "До $x^2$ после.\n", d.toMarkdown());

    // Правка исходника на месте: «$x^2$» → «$x^21$». Формат — как у редактора:
    // currentCharFormat, то есть формат знака слева (несёт SpanMath|Open —
    // набранное продолжает раскрытый кусок, а не рвёт его).
    at.setPosition(block.position() + knob + 4);   // за «2», перед закрывающим $
    QTextCharFormat typing;
    {
        QTextCursor probe(s.doc);
        probe.setPosition(at.position());
        typing = probe.charFormat();
    }
    ZT_TRUE("буква вошла в исходник", d.insertText(at, QStringLiteral("1"), typing));
    ZT_TRUE("кусок всё ещё раскрыт", zametti::hasOpenInlineFormula(s.doc->firstBlock()));

    // Судья закрытия: всё ещё формула — объект.
    ZT_TRUE("свернулась", d.closeInlineFormula(at));
    const ObjectSweep closed = sweepInlineObjects(*s.doc);
    ZT_EQ("объект вернулся с правкой", "1", std::to_string(closed.objects));
    ZT_EQ("исходник поправлен", "$x^21$", closed.sources.toStdString());
    ZT_EQ("круг цел", "До $x^21$ после.\n", d.toMarkdown());

    // Ctrl+Z после сворачивания — раскрытая вернулась (бит Open в undo).
    s.doc->undo();
    ZT_TRUE("отмена вернула раскрытую", zametti::hasOpenInlineFormula(s.doc->firstBlock()));
    while (s.doc->isUndoAvailable()) s.doc->undo();
    ZT_EQ("отмена до конца возвращает объект", "$x^2$",
          sweepInlineObjects(*s.doc).sources.toStdString());
}

// Судья закрытия: разорванная формула — законный текст.
void checkBrokenBecomesText() {
    Shown s;
    show(s, "До $x^2$ после.\n");
    zametti::ZDocument& d = s.note.doc();
    const QTextBlock block = s.doc->firstBlock();
    const int knob = block.text().indexOf(QChar::ObjectReplacementCharacter);

    QTextCursor at(s.doc);
    at.setPosition(block.position() + knob);
    ZT_TRUE("раскрылась", d.openInlineFormula(at));
    // Стереть закрывающий доллар: «$x^2$» → «$x^2».
    at.setPosition(block.position() + knob + 4);
    ZT_TRUE("доллар стёрт", d.deleteForward(at));
    ZT_TRUE("свернулась в текст", d.closeInlineFormula(at));
    ZT_EQ("объектов нет", "0", std::to_string(sweepInlineObjects(*s.doc).objects));
    ZT_TRUE("пометка раскрытости снята", !zametti::hasOpenInlineFormula(s.doc->firstBlock()));
    // В файл уходит то, что написано; одинокий доллар формулу не открывает и
    // косой не обрастает.
    ZT_EQ("запись — законный текст", "До $x^2 после.\n", d.toMarkdown());
}

// Абзац, ставший целиком одной формулой, после закрытия — выключная (liftMath).
void checkCloseLiftsWholeParagraph() {
    Shown s;
    show(s, "$x^2$ хвост.\n");
    zametti::ZDocument& d = s.note.doc();
    const QTextBlock block = s.doc->firstBlock();
    const int knob = block.text().indexOf(QChar::ObjectReplacementCharacter);

    QTextCursor at(s.doc);
    at.setPosition(block.position() + knob);
    ZT_TRUE("раскрылась", d.openInlineFormula(at));
    // Стереть хвост: остаётся «$x^2$» целым абзацем.
    at.setPosition(block.position() + 5);
    at.setPosition(block.position() + int(s.doc->firstBlock().text().size()),
                   QTextCursor::KeepAnchor);
    ZT_TRUE("хвост стёрт", d.deleteForward(at));
    ZT_TRUE("свернулась", d.closeInlineFormula(at));
    ZT_TRUE("стала выключной (liftMath)",
            !zametti::isRawBlock(s.doc->firstBlock()) &&
                zametti::kindOf(s.doc->firstBlock()) == zametti::Kind::Math);
    ZT_EQ("круг цел", "$x^2$\n", d.toMarkdown());
}

// Жест математики: обёрнутое долларами помечено раскрытым, судья делает объект.
void checkToggleGesture() {
    Shown s;
    show(s, "Просто икс квадрат тут.\n");
    zametti::ZDocument& d = s.note.doc();
    const QTextBlock block = s.doc->firstBlock();

    // Выделить «икс» и обернуть.
    QTextCursor at(s.doc);
    at.setPosition(block.position() + 7);
    at.setPosition(block.position() + 10, QTextCursor::KeepAnchor);
    ZT_TRUE("жест обернул", d.toggleInlineMath(at));
    ZT_TRUE("обёрнутое помечено раскрытым",
            zametti::hasOpenInlineFormula(s.doc->firstBlock()));
    ZT_TRUE("свернулась судьёй", d.closeInlineFormula(at));
    ZT_EQ("объект появился", "$икс$", sweepInlineObjects(*s.doc).sources.toStdString());
    ZT_EQ("круг цел", "Просто $икс$ квадрат тут.\n", d.toMarkdown());
}

// --- поиск, счёт, буфер ---------------------------------------------------------

void checkSearchAndFriends() {
    Shown s;
    show(s, "Буква $\\gamma$ и снова $\\gamma$, а ещё слово gamma тут.\n");
    zametti::ZDocument& d = s.note.doc();

    // Поиск ищет по исходнику: «gamma» находит обе формулы и слово.
    const std::vector<zametti::Hit> hits = d.find(zametti::makeQuery(QStringLiteral("gamma")));
    ZT_EQ("три вхождения", "3", std::to_string(hits.size()));
    ZT_TRUE("первые два — в объектах, третье — в тексте",
            hits.size() == 3 && hits[0].inObject && hits[1].inObject && !hits[2].inObject);

    // Живой поиск: карта смещений даёт верные курсоры.
    zametti::NoteSearch search;
    ZT_EQ("живой поиск считает так же", "3",
          std::to_string(search.find(*s.doc, QStringLiteral("gamma"), false)));
    const zametti::SearchHit& first = search.hitAt(0);
    ZT_TRUE("вхождение в формуле — курсор над её знаком",
            first.inObject() &&
                zametti::isInlineFormulaChar(*s.doc, first.cursor.selectionStart()));
    ZT_EQ("смещение — в исходнике объекта", "2", std::to_string(first.innerOffset));
    const zametti::SearchHit& word = search.hitAt(2);
    ZT_TRUE("вхождение в тексте — обычное выделение", !word.inObject());
    ZT_EQ("и стоит на своём слове", "gamma", utf8(word.cursor.selectedText()));

    // Замена в исходнике одной формулы — судьёй.
    QTextCursor scratch(s.doc);
    ZT_TRUE("замена внутри формулы",
            d.rewriteInlineFormula(scratch, first.cursor.selectionStart(),
                                   QStringLiteral("$\\Gamma$")));
    ZT_EQ("файл поменялся ровно в ней",
          "Буква $\\Gamma$ и снова $\\gamma$, а ещё слово gamma тут.\n", d.toMarkdown());

    // Счёт слов: по исходнику, двумя независимыми счётами одинаково.
    const zametti::NoteStats byDoc = d.getStats();
    Shown fresh;
    show(fresh, d.toMarkdown());
    ZT_EQ("счёт слов не зависит от пути", std::to_string(fresh.note.doc().getStats().words),
          std::to_string(byDoc.words));

    // Буфер: кусок строки с формулой внутри уносит исходник.
    QTextCursor range(s.doc);
    range.setPosition(s.doc->firstBlock().position());
    range.setPosition(s.doc->firstBlock().position() + 9, QTextCursor::KeepAnchor);
    ZT_EQ("Ctrl+C куска строки с формулой", "Буква $\\Gamma$ и",
          utf8(d.markdownOf(range)));
}

// --- приёмка: заметка владельца ------------------------------------------------

void checkOwnersNote() {
    const QString path = zt::TestData::file(QStringLiteral("typesetting-math.md"));
    if (path.isEmpty()) {
        std::printf("ПРОПУСК: нет .testdata/typesetting-math.md — копии заметки владельца "
                    "«Typesetting Math in Markdown»\n");
        return;
    }
    const std::string bytes = readFile(path);
    ZT_TRUE("копия не пуста", !bytes.empty());

    Shown s;
    show(s, bytes);
    ZT_EQ("КРУГ ФАЙЛА БАЙТ В БАЙТ на заметке владельца", bytes, s.note.toMarkdown());

    const ObjectSweep sweep = sweepInlineObjects(*s.doc);
    const int expected = expectedInlineObjects(pieces(bytes));
    std::printf("«Typesetting Math in Markdown»: строчных объектов %d (ожидалось %d)\n",
                sweep.objects, expected);
    ZT_EQ("объект на каждое вхождение", std::to_string(expected),
          std::to_string(sweep.objects));
    ZT_TRUE("вхождений десятки — копия настоящая", sweep.objects >= 50);
    ZT_TRUE("посадка каждой — AlignBaseline", sweep.allBaseline);
}

}  // namespace

TEST(InlineFormula, All) {
    checkBuildsObjects();
    checkMultilineSpan();
    checkNonMathStaysText();
    checkGluedFormulas();
    checkDisplayStaysBlock();
    checkFlip();
    checkBrokenBecomesText();
    checkCloseLiftsWholeParagraph();
    checkToggleGesture();
    checkSearchAndFriends();
    checkOwnersNote();
    zt::report("строчные формулы: модель");
}
