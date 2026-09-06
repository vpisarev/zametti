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
#include "znote.h"
#include "test_util.h"

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
        "# Title\n\nFirst paragraph.\n\nSecond paragraph.\n\n## Chapter\n\nText.\n\n```\ncode\n```\n";
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
    ZT_EQ("блок кода книги — на ступень ниже текста",
          std::to_string(zametti::settings().reading().codeStep()),
          std::to_string(book.doc().caretAtBlock(10).blockCharFormat().intProperty(
              QTextFormat::FontSizeAdjustment)));
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

TEST(DocStyle, All) {
    checkOwnStyle();
    checkReadingLookByRole();
    EXPECT_EQ(0, zt::freshFailures());
}
