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
#include "test_util.h"

#include <QTextDocument>

#include <memory>
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

}  // namespace

TEST(DocStyle, All) {
    checkOwnStyle();
    EXPECT_EQ(0, zt::freshFailures());
}
