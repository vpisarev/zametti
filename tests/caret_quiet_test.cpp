// ХОДЬБА КАРЕТКИ ДОКУМЕНТ НЕ ТРОГАЕТ (требование владельца: «при любых
// движениях курсора по документу contentsChange не должен вызываться»).
//
// Почему это отдельный набор, а не строчка в другом: молчание документа —
// свойство, от которого зависит ВСЁ производное. По ревизии документа судят
// счёт слов, кэш найденного и кэш собранных блоков; стоит ходьбе каретки
// поднять ревизию — и они протухают на каждом шаге стрелкой. Владелец увидел
// это как «даже при движениях курсора счётчик слов инвалидируется».
//
// Как ломалось: уходя из блока, редактор предлагал трём судьям свернуть то,
// что человек мог раскрыть (строчная формула, выключная формула, таблица).
// Судьи почти всегда отвечали «нечего», но правка к тому времени была уже
// открыта — а ПУСТАЯ СКОБКА ПРАВКИ У Qt ПОДНИМАЕТ РЕВИЗИЮ (замер:
// beginEditBlock + endEditBlock без изменений дают +1). Теперь редактор
// сперва спрашивает заметку (mayHaveOpenObject) и скобку не открывает.
//
// Спрашивается на заметке СО ВСЕМИ РОДАМИ БЛОКОВ: у каждого своя разметка, и
// та, что «показывает место», как раз и норовит переписать блок под кареткой.

#include "editor_widget.h"

#include "test_util.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>
#include <QTextDocument>

#include <string>

namespace {

const char* const kNote =
    "# Заголовок\n"
    "\n"
    "Абзац с **жирным**, *косым* и `кодом`, а также ссылкой [сюда](https://z).\n"
    "\n"
    "- пункт один\n"
    "- пункт два\n"
    "  - вложенный\n"
    "\n"
    "1. нумерованный\n"
    "2. второй\n"
    "\n"
    "> цитата\n"
    "\n"
    "```python\n"
    "code = 1\n"
    "```\n"
    "\n"
    "| a | b |\n"
    "|---|---|\n"
    "| 1 | 2 |\n"
    "\n"
    "Строчная формула $x^2$ внутри текста и цена 5$ рядом.\n"
    "\n"
    "$$\n"
    "\\frac{a}{b}\n"
    "$$\n"
    "\n"
    "- [ ] задача\n"
    "- [x] сделана\n"
    "\n"
    "Последний абзац.\n";

}  // namespace

TEST(CaretQuiet, All) {
    QTemporaryDir tmp;
    const QString path = QDir(tmp.path()).filePath(QStringLiteral("ходьба.md"));
    {
        QFile file(path);
        ZT_TRUE("заметка записана", file.open(QIODevice::WriteOnly));
        file.write(kNote);
    }

    zametti::NoteEditor editor;
    editor.setStoreRoot(tmp.path());
    editor.resize(800, 600);
    editor.show();
    QTest::qWait(30);
    ZT_TRUE("заметка открыта", editor.openFile(path));
    QTest::qWait(80);

    int edits = 0;
    QObject::connect(editor.document(), &QTextDocument::contentsChange, &editor,
                     [&edits](int, int removed, int added) {
                         if (removed != 0 || added != 0) ++edits;
                     });
    const int revisionBefore = editor.document()->revision();

    // Все ходы, какими человек гуляет по заметке: стрелки во все стороны,
    // края строки и документа, страницами и словами, с выделением и без.
    const std::vector<QKeySequence> walk = {
        QKeySequence(Qt::Key_Down),      QKeySequence(Qt::Key_Down),
        QKeySequence(Qt::Key_Down),      QKeySequence(Qt::Key_Down),
        QKeySequence(Qt::Key_Down),      QKeySequence(Qt::Key_Down),
        QKeySequence(Qt::Key_Down),      QKeySequence(Qt::Key_Down),
        QKeySequence(Qt::Key_Down),      QKeySequence(Qt::Key_Down),
        QKeySequence(Qt::Key_Down),      QKeySequence(Qt::Key_Down),
        QKeySequence(Qt::Key_Right),     QKeySequence(Qt::Key_Right),
        QKeySequence(Qt::Key_End),       QKeySequence(Qt::Key_Home),
        QKeySequence(Qt::Key_Up),        QKeySequence(Qt::Key_Up),
        QKeySequence(Qt::Key_Left),      QKeySequence(Qt::Key_PageDown),
        QKeySequence(Qt::Key_PageUp),    QKeySequence(Qt::CTRL | Qt::Key_End),
        QKeySequence(Qt::CTRL | Qt::Key_Home),
        QKeySequence(Qt::CTRL | Qt::Key_Right),
        QKeySequence(Qt::CTRL | Qt::Key_Left),
        QKeySequence(Qt::SHIFT | Qt::Key_Down),
        QKeySequence(Qt::SHIFT | Qt::Key_Right),
        QKeySequence(Qt::Key_Down),      QKeySequence(Qt::Key_Down),
        QKeySequence(Qt::Key_Down),      QKeySequence(Qt::Key_Down),
    };
    for (const QKeySequence& keys : walk) {
        QTest::keySequence(&editor, keys);
        QTest::qWait(12);
    }
    QTest::qWait(60);

    ZT_EQ("ходьба каретки не правит текст", std::string("0"), std::to_string(edits));
    ZT_EQ("и не поднимает ревизию документа (пустых скобок правки нет)",
          std::to_string(revisionBefore), std::to_string(editor.document()->revision()));

    // И самое главное — числа остались свежими: производное от документа от
    // ходьбы каретки не протухает.
    ZT_TRUE("счёт слов остался свежим", editor.statsFresh());
}
