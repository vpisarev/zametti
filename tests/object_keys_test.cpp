// КЛАВИШИ НА ОБЪЕКТЕ: картинка, таблица, выключная формула — ОДНИМ ПРАВИЛОМ.
//
// Беда владельца (26.08.2026): выделить картинку и нажать любую букву — весь
// интерфейс становится серым, в полосе сведений совет про подпись, и выйти из
// этого нельзя иначе как закрыв программу. То же с таблицей и с выключной
// формулой.
//
// Причина была не в буквах, а в проводах: совет уезжал сигналом `importStatus`,
// которым ВВОЗ КАРТИНОК запирает окно (дерево, список, тулбар гаснут, пока
// текст непуст). Ввоз в конце шлёт пустую строку и отпирает — а совет не шлёт
// ничего, и окно оставалось запертым навсегда.
//
// Поэтому здесь проверяется ДВОЕ, и второе важнее первого:
//
//   * буква на объекте не делает НИЧЕГО (решение владельца): документ цел, в
//     исходник объекта ничего не попадает;
//   * НАЖАТИЕ КЛАВИШИ НИКОГДА НЕ ШЛЁТ `importStatus` — то есть не может запереть
//     окно. Это инвариант провода, и он проверяется на каждом нажатии матрицы.
//
// Матрица: три рода объектов × набор клавиш. Новый род объекта обязан попасть
// в этот же набор, а не завести себе своё правило рядом.

#include "doc_model.h"
#include "caption_editor.h"
#include "editor_widget.h"
#include "test_util.h"
#include "testdata.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QTemporaryDir>
#include <QTest>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>

#include <string>
#include <vector>

namespace {

QString g_dir;

QString noteWith(const QString& name, const QString& body) {
    const QString path = QDir(g_dir).filePath(name);
    QFile file(path);
    if (file.open(QIODevice::WriteOnly)) file.write(body.toUtf8());
    return path;
}

// Блок-объект: картинка, таблица или формула — по тем же вопросам, что задаёт
// сам редактор. -1 — объекта в документе нет.
int firstObjectBlock(zametti::NoteEditor& editor) {
    for (QTextBlock b = editor.document()->firstBlock(); b.isValid(); b = b.next()) {
        if (zametti::blockImageRef(b).valid) return b.blockNumber();
        if (editor.formulaAt(b.blockNumber()) != nullptr) return b.blockNumber();
        if (b.text().contains(QChar::ObjectReplacementCharacter)) return b.blockNumber();
    }
    return -1;
}

}  // namespace

static int ztRunSuite(int, char**) {
    QTemporaryDir dir;
    g_dir = dir.path();

    // Картинка настоящая: без файла блок не станет фотографией.
    QImage pixels(8, 8, QImage::Format_RGB32);
    pixels.fill(Qt::darkCyan);
    const QString shot = QDir(g_dir).filePath(QStringLiteral("снимок.png"));
    pixels.save(shot);

    struct Case {
        const char* what;
        QString body;
    };
    const std::vector<Case> cases{
        {"картинка", QStringLiteral("# Заметка\n\nАбзац.\n\n![подпись](снимок.png)\n\nХвост.\n")},
        {"таблица", QStringLiteral("# Заметка\n\nАбзац.\n\n| имя | цена |\n|---|---|\n"
                                   "| болт | 10 |\n\nХвост.\n")},
        {"формула", QStringLiteral("# Заметка\n\nАбзац.\n\n$$\\frac{a}{b}$$\n\nХвост.\n")},
    };

    // Клавиши, которых у слоя объекта нет: они обязаны не делать ничего.
    struct Key {
        const char* what;
        Qt::Key key;
        Qt::KeyboardModifiers mods;
    };
    const std::vector<Key> keys{
        {"буква", Qt::Key_A, Qt::NoModifier},
        {"заглавная", Qt::Key_B, Qt::ShiftModifier},
        {"цифра", Qt::Key_5, Qt::NoModifier},
        {"пробел", Qt::Key_Space, Qt::NoModifier},
        {"решётка", Qt::Key_NumberSign, Qt::NoModifier},
        {"звёздочка", Qt::Key_Asterisk, Qt::NoModifier},
        {"доллар", Qt::Key_Dollar, Qt::NoModifier},
        {"труба", Qt::Key_Bar, Qt::NoModifier},
    };

    for (const Case& one : cases) {
        zametti::NoteEditor editor;
        editor.setAttribute(Qt::WA_DontShowOnScreen);
        editor.resize(900, 700);
        editor.show();
        const QString path = noteWith(QString::fromUtf8(one.what) + QStringLiteral(".md"), one.body);
        ZT_TRUE(std::string("заметка открылась: ") + one.what, editor.openFile(path));
        QCoreApplication::processEvents();

        // ЗАПОР ОКНА — ЭТО СИГНАЛ, и мы ловим его на каждом нажатии: именно он
        // и гасил интерфейс. Пустая строка тоже считается: её шлёт только ввоз,
        // и от нажатия клавиши не должно прилетать ни того, ни другого.
        int locks = 0;
        QObject::connect(&editor, &zametti::NoteEditor::importStatus, &editor,
                         [&locks](const QString&) { ++locks; });
        // Подсказки — ДРУГИМ проводом, который ничего не запирает. Считаем их
        // отдельно: разделение сигналов и есть починка, и оно проверяется.
        int hints = 0;
        QObject::connect(&editor, &zametti::NoteEditor::hint, &editor,
                         [&hints](const QString&) { ++hints; });

        const int block = firstObjectBlock(editor);
        ZT_TRUE(std::string("объект в заметке нашёлся: ") + one.what, block >= 0);
        if (block < 0) continue;

        const std::string before = editor.note().toMarkdown();

        // ДВА СОСТОЯНИЯ, И ОБА НАСТОЯЩИЕ. Владелец сказал прямо: «выделяем
        // картинку курсором ИЛИ кликом» — это разные состояния каретки, и слой
        // объекта их различает (выделение он объявляет не своим делом). Значит
        // и спрашивать надо оба: беда была в обоих.
        for (int state = 0; state < 2; ++state) {
            const bool selected = state == 1;
            QTextCursor at(editor.document()->findBlockByNumber(block));
            at.movePosition(QTextCursor::StartOfBlock);
            if (selected) at.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
            editor.setTextCursor(at);
            QCoreApplication::processEvents();
            const std::string where = selected ? " (выделен)" : " (каретка)";

            for (const Key& key : keys) {
                QTest::keyClick(&editor, key.key, key.mods);
                QCoreApplication::processEvents();
                const std::string now = editor.note().toMarkdown();
                ZT_EQ(std::string("«") + key.what + "» на объекте " + one.what + where +
                          " ничего не изменила",
                      before, now);
                ZT_EQ(std::string("«") + key.what + "» на объекте " + one.what + where +
                          " не заперла окно",
                      "0", std::to_string(locks));
            }
        }

        // Каретка на объекте без выделения — состояние, в котором слой объекта
        // и живёт: Enter ниже спрашивается именно в нём.
        {
            QTextCursor at(editor.document()->findBlockByNumber(block));
            at.movePosition(QTextCursor::StartOfBlock);
            editor.setTextCursor(at);
            QCoreApplication::processEvents();
        }

        // Ctrl+D (сочетание переключения подписи) у ТАБЛИЦЫ и ФОРМУЛЫ подписи не
        // находит и обязан сказать это ПОДСКАЗКОЙ — не запирающим сигналом.
        // Здесь и проверяется, что развилка проводов настоящая: подсказка
        // приходит, замок не щёлкает.
        if (std::string(one.what) != "картинка") {
            const int hintsBefore = hints;
            QTest::keyClick(&editor, Qt::Key_D, Qt::ControlModifier);
            QCoreApplication::processEvents();
            ZT_TRUE(std::string("Ctrl+D на объекте ") + one.what + " сказал подсказкой",
                    hints > hintsBefore);
            ZT_EQ(std::string("и окна не запер: ") + one.what, "0", std::to_string(locks));
        }

        // И ЭТО НЕ ЗАПРЕТ НА ВСЁ: клавиши слоя объекта работают. Enter у
        // картинки открывает поле подписи, у таблицы и формулы — исходник.
        QTest::keyClick(&editor, Qt::Key_Return);
        QCoreApplication::processEvents();
        const bool opened =
            editor.findChild<zametti::CaptionEditor*>() != nullptr ||
            editor.note().toMarkdown() != before ||
            firstObjectBlock(editor) < 0;
        ZT_TRUE(std::string("Enter на объекте открывает правку: ") + one.what, opened);
        ZT_EQ(std::string("и Enter окна не запер: ") + one.what, "0", std::to_string(locks));

    }

    return zt::report("клавиши на объектах");
}

TEST(ObjectKeys, All) {
    std::vector<QByteArray> ztArgs{QByteArrayLiteral("object_keys_test")};
    std::vector<char*> ztArgv;
    for (QByteArray& a : ztArgs) ztArgv.push_back(a.data());
    EXPECT_EQ(0, ztRunSuite(int(ztArgv.size()), ztArgv.data()));
}
