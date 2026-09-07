// Стиль документа — параметр сборки, а не глобальные настройки.
//
// Решение владельца: ZSettings только для чтения; нужен другой облик —
// копия ZDocStyle передаётся явно. Здесь: документ, собранный со своим
// стилем, собран ИМ (шрифт, кегль), настройки программы при этом не тронуты,
// стиль прикреплён к документу и виден всем, у кого документ в руках
// (styleOf), а пересборка живого документа стиль не теряет.

#include "document.h"
#include "document_builder.h"
#include "pieces.h"
#include "settings.h"
#include "settings_hook.h"
#include "znote.h"
#include "test_util.h"

#include <QTextBlock>
#include <QTextBlockFormat>
#include <QTextCursor>
#include <QTextDocument>

#include <cmath>
#include <memory>
#include <string>
#include <string>

namespace {

std::string s(const QString& q) { return q.toStdString(); }

void checkOwnStyle() {
    const QString before = zametti::settings().style().fontFamily();
    const qreal beforePoint = zametti::settings().style().baseFontPoint();

    auto own = std::make_shared<zametti::ZDocStyle>(zametti::settings().style());
    own->setFontFamily(QStringLiteral("DejaVu Serif"));
    own->setBaseFontPoint(17.0);

    // Сборка со своим стилем — через прикрепление к документу.
    QTextDocument doc;
    zametti::attachStyle(doc, own);
    zametti::buildDocument(pieces("# Заголовок\n\nабзац\n"), doc);
    ZT_EQ("документ собран своим шрифтом", "DejaVu Serif", s(doc.defaultFont().family()));
    ZT_TRUE("и своим кеглем", qFuzzyCompare(doc.defaultFont().pointSizeF(), 17.0));
    ZT_EQ("стиль виден у документа", "DejaVu Serif", s(zametti::styleOf(doc).fontFamily()));
    ZT_TRUE("настройки программы не тронуты",
            zametti::settings().style().fontFamily() == before &&
                qFuzzyCompare(zametti::settings().style().baseFontPoint(), beforePoint));

    // Пересборка того же документа без явного стиля стиль не теряет.
    zametti::buildDocument(pieces("другой текст\n"), doc);
    ZT_EQ("после пересборки стиль тот же", "DejaVu Serif", s(doc.defaultFont().family()));

    // Документ без своего стиля — стиль настроек.
    QTextDocument plain;
    zametti::buildDocument(pieces("абзац\n"), plain);
    ZT_EQ("без своего стиля — шрифт настроек", s(before), s(plain.defaultFont().family()));
    ZT_TRUE("и прикреплённого стиля нет", zametti::attachedStyle(plain) == nullptr);

    // Заметка: setStyle до загрузки — и её документ собран этим стилем.
    zametti::ZDocument note;
    note.setStyle(own);
    note.loadMarkdown("# a\n\nb\n");
    ZT_EQ("стиль заметки — свой", "DejaVu Serif", s(note.style().fontFamily()));
    ZT_TRUE("и он же отдаётся указателем", note.stylePtr() == own);
}

// --- THE READING LOOK BY ROLE (brief 18) --------------------------------------
// A note with role: book is built with settings().readingStyle(): the red line,
// justified paragraphs, centred headings, the collapsed empty line, the reading
// line height. An ordinary note is built with the editor's style and shows none
// of it. Take the role away — the book turns into an ordinary note (the check
// that catches a removed chooser in ZNote::load).
void checkReadingLookByRole() {
    const std::string body =
        "# Title\n\nFirst paragraph.\n\nSecond paragraph.\n\n## Chapter\n\nText.\n\n```\ncode\n```\n\n"
        "> A quote.\n\nAfter the quote.\n";
    zametti::ZNote book;
    const std::string header = "<!-- zametti\nrole: book\n-->\n\n";
    book.load(header + body);
    zametti::ZNote plain;
    plain.load(body);

    const zametti::ZDocStyle& reading = book.doc().style();
    const zametti::ZDocStyle& ordinary = plain.doc().style();
    const auto expected = zametti::settings().readingStyle();
    ZT_EQ("книга собрана шрифтом чтения", s(expected->fontFamily()), s(reading.fontFamily()));
    ZT_TRUE("и с интерлиньяжем чтения",
            qFuzzyCompare(reading.lineHeightFactor(), zametti::settings().reading().lineHeightFactor()));
    // Code in a book (books2): its own step under the prose and the editor's
    // line height, not the book's airy one.
    ZT_EQ("ступень кода в книге — из reading.codeStep",
          std::to_string(zametti::settings().reading().codeStep()), std::to_string(reading.codeStep()));
    ZT_TRUE("интерлиньяж кода в книге — редакторский",
            qFuzzyCompare(reading.codeLineHeight(), zametti::settings().style().lineHeightFactor()));
    ZT_TRUE("у стиля редактора кода свой интерлиньяж не задан",
            ordinary.codeLineHeightFactor() < 0.0 &&
                qFuzzyCompare(ordinary.codeLineHeight(), ordinary.lineHeightFactor()));
    ZT_TRUE("обычная заметка — стиль настроек",
            ordinary.fontFamily() == zametti::settings().style().fontFamily() &&
                plain.doc().stylePtr() == nullptr);
    ZT_TRUE("у стиля чтения есть красная строка", reading.firstLineIndent() > 0.0);
    ZT_TRUE("у стиля редактора её нет", qFuzzyIsNull(ordinary.firstLineIndent()));

    // Baked into the block formats, not just remembered by the style.
    const auto fmt = [](zametti::ZNote& note, int block) {
        return note.doc().caretAtBlock(block).blockFormat();
    };
    // blocks: 0 title, 1 empty, 2 first paragraph, 3 empty, 4 second, 5 empty, 6 heading, 7 empty, 8 text
    const QTextBlockFormat para = fmt(book, 2);
    ZT_TRUE("абзац книги — с красной строкой", para.textIndent() > 0.0);
    ZT_TRUE("абзац книги — по ширине", para.alignment() == Qt::AlignJustify);
    ZT_TRUE("заголовок книги — по центру", fmt(book, 6).alignment() == Qt::AlignHCenter);
    // A changed config chooses the style again (books2, owner's report:
    // reading.codeStep "does nothing"): the document carried the copy from
    // its load. After refreshStyle() the book's style is the new reading one.
    {
        const int was = zametti::settings().reading().codeStep();
        zametti::mutableSettingsForTests().reading().setCodeStep(was - 1);
        book.refreshStyle();
        ZT_EQ("после смены облика книга берёт новую ступень кода", std::to_string(was - 1),
              std::to_string(book.doc().style().codeStep()));
        zametti::mutableSettingsForTests().reading().setCodeStep(was);
        book.refreshStyle();
        ZT_EQ("и обратно", std::to_string(was), std::to_string(book.doc().style().codeStep()));
    }
    // Air around a heading (books2): every level, the editor's style has none.
    ZT_TRUE("заголовок книги — с воздухом сверху и снизу",
            fmt(book, 6).topMargin() > 0.0 && fmt(book, 6).bottomMargin() > 0.0);
    ZT_TRUE("заголовок заметки — без воздуха",
            qFuzzyIsNull(fmt(plain, 6).topMargin()) && qFuzzyIsNull(fmt(plain, 6).bottomMargin()));
    // blocks 9 empty, 10 code: the step and the line height are baked.
    const QTextBlockFormat code = fmt(book, 10);
    ZT_EQ("блок кода книги — интерлиньяж редактора, %",
          std::to_string(int(std::lround(zametti::settings().style().lineHeightFactor() * 100.0))),
          std::to_string(int(std::lround(code.lineHeight()))));
    ZT_EQ("блок кода книги — на ступени reading.codeStep",
          std::to_string(zametti::settings().reading().codeStep()),
          std::to_string(book.doc().caretAtBlock(10).blockCharFormat().intProperty(
              QTextFormat::FontSizeAdjustment)));
    // blocks 11 empty, 12 quote, 13 empty, 14 paragraph: the quote of a book
    // is italic, indented on the right too, with air before and after the
    // run (the blank line after it carries the air); the editor's is not.
    {
        const QTextBlockFormat quote = fmt(book, 12);
        ZT_TRUE("цитата книги — курсивом",
                book.doc().caretAtBlock(12).blockCharFormat().fontItalic());
        ZT_TRUE("цитата книги — с правым полем", quote.rightMargin() > 0.0);
        ZT_TRUE("перед цитатой книги — воздух", quote.topMargin() > 0.0);
        ZT_TRUE("после цитаты книги — воздух", fmt(book, 13).topMargin() > 0.0);
        ZT_TRUE("а перед обычным абзацем книги воздуха нет",
                qFuzzyIsNull(fmt(book, 4).topMargin()));
        ZT_TRUE("цитата заметки — не курсивом, без правого поля и воздуха",
                !plain.doc().caretAtBlock(12).blockCharFormat().fontItalic() &&
                    qFuzzyIsNull(fmt(plain, 12).rightMargin()) &&
                    qFuzzyIsNull(fmt(plain, 12).topMargin()));
    }
    // The code families of a book (07.09.2026): blocks in reading.codeFamily
    // (Source Code Pro, the serif's companion), inline code in the book font.
    ZT_EQ("блок кода книги — семейством reading.codeFamily",
          s(zametti::settings().reading().codeFamily()),
          s(book.doc().caretAtBlock(10).blockCharFormat().fontFamilies().toStringList().value(0)));
    ZT_EQ("блок кода заметки — семейством fonts.monospaceFamily",
          s(zametti::settings().style().codeFamily()),
          s(plain.doc().caretAtBlock(10).blockCharFormat().fontFamilies().toStringList().value(0)));
    ZT_EQ("блок кода заметки — вровень с текстом",
          std::to_string(zametti::settings().style().codeStep()),
          std::to_string(plain.doc().caretAtBlock(10).blockCharFormat().intProperty(
              QTextFormat::FontSizeAdjustment)));
    const QTextBlockFormat gap = fmt(book, 3);
    ZT_TRUE("пустая строка книги схлопнута до пикселя",
            gap.lineHeightType() == QTextBlockFormat::FixedHeight && gap.lineHeight() <= 1.0);
    const QTextBlockFormat plainPara = fmt(plain, 2);
    ZT_TRUE("абзац обычной заметки — без красной строки", qFuzzyIsNull(plainPara.textIndent()));
    ZT_TRUE("и не по ширине", plainPara.alignment() != Qt::AlignJustify);
    ZT_TRUE("пустая строка обычной заметки — обычной высоты",
            fmt(plain, 3).lineHeightType() != QTextBlockFormat::FixedHeight);
    // The file is untouched by the look: both round-trip to the same body.
    ZT_EQ("облик не меняет файл книги", header + body, book.toMarkdown());
}

}  // namespace

// --- INLINE CODE: ONE RULE FOR THE BUILDER, THE CARET AND THE CELLS ----------
// Without the plate the span is set in the text's family, at the text's step,
// heavier and in its own colour; a link inside code keeps the link's colour;
// bold and a heading are never lightened. The plate look is the old one:
// the monospace family, codeStep, the grey background.
void checkInlineCodeLook() {
    const auto fragmentAt = [](zametti::ZDocument& doc, int block, const QString& text) {
        QTextCharFormat out;
        const QTextBlock b = doc.caretAtBlock(block).block();
        for (auto it = b.begin(); !it.atEnd(); ++it)
            if (it.fragment().text() == text) out = it.fragment().charFormat();
        return out;
    };
    auto look = std::make_shared<zametti::ZDocStyle>(zametti::settings().style());
    look->setInlineCodeFamily(QString());
    look->setInlineCodePlate(false);
    look->setInlineCodeWeight(600);
    look->setInlineCodeColor(QColor(0x40, 0x40, 0x80));
    look->setCodeStep(-1);
    const auto build = [&](const std::shared_ptr<zametti::ZDocStyle>& style) {
        auto doc = std::make_shared<zametti::ZDocument>();
        doc->setStyle(style);
        doc->loadMarkdown("a `b` c\n\n**a `b`**\n\n# t `c`\n\n[`x`](u)\n");
        return doc;
    };
    {
        auto doc = build(look);
        const QTextCharFormat plain = fragmentAt(*doc, 0, QStringLiteral("b"));
        ZT_TRUE("без плашки: семейство — текста", plain.fontFamilies().toStringList().isEmpty());
        ZT_EQ("без плашки: вес 600", "600", std::to_string(plain.fontWeight()));
        ZT_EQ("без плашки: свой цвет", "#404080", s(plain.foreground().color().name()));
        ZT_EQ("без плашки: ступень текста", "0",
              std::to_string(plain.intProperty(QTextFormat::FontSizeAdjustment)));
        ZT_TRUE("без плашки: фона нет", plain.background().style() == Qt::NoBrush);
        ZT_EQ("жирный код — жирный", std::to_string(int(QFont::Bold)),
              std::to_string(fragmentAt(*doc, 2, QStringLiteral("b")).fontWeight()));
        ZT_EQ("код в заголовке — не легче заголовка", std::to_string(int(QFont::Bold)),
              std::to_string(fragmentAt(*doc, 4, QStringLiteral("c")).fontWeight()));
        ZT_EQ("ссылка в коде — цвета ссылки", s(look->linkColor().name()),
              s(fragmentAt(*doc, 6, QStringLiteral("x")).foreground().color().name()));
    }
    {
        // The old look, knob by knob.
        auto plate = std::make_shared<zametti::ZDocStyle>(*look);
        plate->setInlineCodeFamily(look->codeFamily());
        plate->setInlineCodePlate(true);
        plate->setInlineCodeWeight(400);
        plate->setInlineCodeColor(QColor(0, 0, 0, 0));
        auto doc = build(plate);
        const QTextCharFormat coded = fragmentAt(*doc, 0, QStringLiteral("b"));
        ZT_EQ("с плашкой: семейство кода", s(look->codeFamily()),
              s(coded.fontFamilies().toStringList().value(0)));
        ZT_EQ("с плашкой: ступень кода", std::to_string(look->codeStep()),
              std::to_string(coded.intProperty(QTextFormat::FontSizeAdjustment)));
        ZT_TRUE("с плашкой: фон кода", coded.background().color() == look->codeBackground());
        ZT_EQ("с плашкой: вес текста", std::to_string(int(QFont::Normal)), std::to_string(coded.fontWeight()));
        ZT_TRUE("с плашкой: цвет текста", coded.foreground().style() == Qt::NoBrush);
        ZT_EQ("с плашкой жирный код — жирный", std::to_string(int(QFont::Bold)),
              std::to_string(fragmentAt(*doc, 2, QStringLiteral("b")).fontWeight()));
    }
    // The reading style carries the same knobs and its own families.
    const auto reading = zametti::settings().readingStyle();
    ZT_EQ("чтение: вес inline-кода — из fonts", std::to_string(zametti::settings().style().inlineCodeWeight()),
          std::to_string(reading->inlineCodeWeight()));
    ZT_EQ("чтение: семейство inline-кода — из reading", s(zametti::settings().reading().inlineCodeFamily()),
          s(reading->inlineCodeFamily()));
    ZT_EQ("чтение: семейство блоков — из reading", s(zametti::settings().reading().codeFamily()),
          s(reading->codeFamily()));
}

// THE WEIGHT OF THE TEXT (07.09.2026): fonts.noteWeight is the base font's
// weight, the book may have its own (reading.fontWeight, 0 = the note's), and
// the document built with a style carries it as its default font.
void checkFontWeight() {
    auto look = std::make_shared<zametti::ZDocStyle>(zametti::settings().style());
    ZT_EQ("вес по умолчанию — обычный", "400", std::to_string(look->baseFont().weight()));
    look->setFontWeight(500);
    ZT_EQ("baseFont несёт вес", "500", std::to_string(look->baseFont().weight()));
    ZT_TRUE("и кегль с зумом", qFuzzyCompare(look->baseFont(2.0).pointSizeF(), look->baseFontPoint() * 2.0));
    zametti::ZDocument doc;
    doc.setStyle(look);
    doc.loadMarkdown("# a\n\nb `c`\n");
    ZT_EQ("документ собран шрифтом этого веса", "500",
          std::to_string(doc.caretAtBlock(0).block().document()->defaultFont().weight()));
    // Inline code is never lighter than the text: a heavy text lifts it.
    look->setFontWeight(700);
    look->setInlineCodeWeight(600);
    zametti::ZDocument heavy;
    heavy.setStyle(look);
    heavy.loadMarkdown("b `c`\n");
    const QTextBlock b = heavy.caretAtBlock(0).block();
    int codeWeight = 0;
    for (auto it = b.begin(); !it.atEnd(); ++it)
        if (it.fragment().text() == QStringLiteral("c")) codeWeight = it.fragment().charFormat().fontWeight();
    ZT_EQ("код не легче жирного текста", "700", std::to_string(codeWeight));
    // The book: its own weight, or the note's.
    const int noteWeight = zametti::settings().style().fontWeight();
    ZT_EQ("чтение без своего веса — вес заметки", std::to_string(noteWeight),
          std::to_string(zametti::settings().readingStyle()->fontWeight()));
    zametti::mutableSettingsForTests().reading().setFontWeight(600);
    ZT_EQ("чтение со своим весом", "600", std::to_string(zametti::settings().readingStyle()->fontWeight()));
    zametti::mutableSettingsForTests().reading().setFontWeight(0);
}

TEST(DocStyle, All) {
    checkFontWeight();
    checkInlineCodeLook();
    checkOwnStyle();
    checkReadingLookByRole();
    EXPECT_EQ(0, zt::freshFailures());
}
