// Края фотографии: инвариант «блок-картинка ни с чем не сливается, и по обе
// стороны от него всегда есть куда встать».
//
// Три беды владельца оказались одной: после вставки за последней картинкой не
// оставалось соседа; Enter на картинке делил её блок; Backspace на пустой
// строке над ней склеивал её с этой строкой. Во всех трёх случаях блок
// переставал быть «целиком одна картинка» — и разметка "![alt](путь)"
// показывалась ссылкой, потому что фотографией рисуется только тот абзац,
// который состоит из image-спана целиком.
//
// Чинится это ИНВАРИАНТОМ, а не тремя ветками в трёх операциях: четвёртая
// операция принесла бы ту же беду заново. Поэтому здесь МАТРИЦА — каждая
// операция против каждой стороны картинки, — а не проверка на найденный случай.

#include "doc_model.h"
#include "editor_widget.h"
#include "test_util.h"

#include <vector>
#include "testdata.h"

#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QTemporaryDir>
#include <QTest>
#include <QTextBlock>
#include <QTextCursor>

#include <string>

namespace {

QString g_dir;

// Заметка с картинкой и пустыми строками вокруг: тот самый вид, который
// получается после вставки.
QString noteWith(const QString& name, const QString& body) {
    const QString path = QDir(g_dir).filePath(name);
    QFile file(path);
    if (file.open(QIODevice::WriteOnly)) file.write(body.toUtf8());
    return path;
}

// Осталась ли картинка картинкой. Это и есть предмет проверки: разъехавшийся
// блок выглядит как ссылка с подписью, и внешне отличается только этим.
int photoCount(const QTextDocument& doc) {
    int photos = 0;
    for (QTextBlock block = doc.begin(); block.isValid(); block = block.next())
        if (zametti::blockImageRef(block).valid) ++photos;
    return photos;
}

int blockOfPhoto(const QTextDocument& doc) {
    for (QTextBlock block = doc.begin(); block.isValid(); block = block.next())
        if (zametti::blockImageRef(block).valid) return block.blockNumber();
    return -1;
}

// Один заход матрицы: поставить каретку куда сказано, нажать что сказано и
// спросить, цела ли картинка.
void probe(const std::string& what, const QString& markdown, int caretBlock,
           bool atBlockEnd, Qt::Key key) {
    static int serial = 0;
    const QString path =
        noteWith(QStringLiteral("0000000000000%1.md").arg(serial++, 1, 36), markdown);

    zametti::NoteEditor editor;
    editor.resize(600, 400);
    editor.show();
    editor.openFile(path);

    const int before = photoCount(*editor.document());
    ZT_TRUE(what + ": картинка на месте до правки", before == 1);

    QTextBlock block = editor.document()->findBlockByNumber(caretBlock);
    ZT_TRUE(what + ": блок для каретки существует", block.isValid());
    if (!block.isValid()) return;

    QTextCursor caret = editor.textCursor();
    caret.setPosition(block.position() + (atBlockEnd ? block.length() - 1 : 0));
    editor.setTextCursor(caret);
    QTest::keyClick(&editor, key);
    QTest::qWait(20);

    const bool intact = photoCount(*editor.document()) == before;
    ZT_TRUE(what + ": картинка осталась картинкой", intact);
    // Без текста «после» провал не говорит НИЧЕГО: разъехаться блок может
    // по-разному, и чинить надо то, что случилось, а не то, что придумалось.
    if (!intact)
        std::fprintf(stderr, "  документ после: [%s]\n",
                    editor.document()->toPlainText().toUtf8().constData());
}

// Картинка обязана уйти ЦЕЛИКОМ: ни ссылки, ни подписи, ни осколка разметки.
void probeGone(const std::string& what, const QString& markdown, int caretBlock, Qt::Key key,
               Qt::KeyboardModifiers mods) {
    static int serial = 100;
    const QString path =
        noteWith(QStringLiteral("000000000000%1.md").arg(serial++, 2, 36), markdown);
    zametti::NoteEditor editor;
    editor.resize(600, 400);
    editor.show();
    editor.openFile(path);

    QTextBlock block = editor.document()->findBlockByNumber(caretBlock);
    if (!block.isValid()) {
        ZT_TRUE(what + ": блок для каретки существует", false);
        return;
    }
    QTextCursor caret = editor.textCursor();
    caret.setPosition(block.position());
    editor.setTextCursor(caret);
    QTest::keyClick(&editor, key, mods);
    QTest::qWait(20);

    ZT_TRUE(what + ": картинки в документе больше нет",
            photoCount(*editor.document()) == 0);
    const QString text = editor.document()->toPlainText();
    ZT_TRUE(what + ": и осколка разметки тоже",
            !text.contains(QStringLiteral("Кадр")) && !text.contains(QStringLiteral("kadr")));
}

// В буфер попадает ССЫЛКА, а не пиксели: вставка в другую заметку сошлётся на
// тот же файл вложения, а не заведёт вторую его копию.
void probeClipboard(const std::string& what, const QString& markdown, int caretBlock,
                    Qt::Key key, Qt::KeyboardModifiers mods, bool keeps) {
    static int serial = 200;
    const QString path =
        noteWith(QStringLiteral("000000000000%1.md").arg(serial++, 2, 36), markdown);
    QApplication::clipboard()->clear();

    zametti::NoteEditor editor;
    editor.resize(600, 400);
    editor.show();
    editor.openFile(path);

    QTextBlock block = editor.document()->findBlockByNumber(caretBlock);
    if (!block.isValid()) {
        ZT_TRUE(what + ": блок для каретки существует", false);
        return;
    }
    QTextCursor caret = editor.textCursor();
    caret.setPosition(block.position());
    editor.setTextCursor(caret);
    QTest::keyClick(&editor, key, mods);
    QTest::qWait(20);

    const QString buffer = QApplication::clipboard()->text();
    ZT_TRUE(what + ": в буфере ссылка на вложение",
            buffer.contains(QStringLiteral("kadr.png")) &&
                buffer.contains(QStringLiteral("![")));
    ZT_TRUE(what + (keeps ? ": картинка осталась" : ": картинка убрана"),
            (photoCount(*editor.document()) == 1) == keeps);
}

}  // namespace

static int ztRunSuite(int argc, char** argv) {
    (void)argc;
    (void)argv;
    QTemporaryDir tmp;
    if (!tmp.isValid()) {
        std::printf("не завёлся временный каталог\n");
        return 2;
    }
    g_dir = tmp.path();

    // Картинка нужна настоящая: без файла вид рисует рамку, и половина
    // проверок проверяла бы не то.
    QImage picture(120, 80, QImage::Format_RGB32);
    picture.fill(Qt::darkCyan);
    picture.save(QDir(g_dir).filePath(QStringLiteral("kadr.png")));

    // "# Заголовок", пустая, картинка, пустая, "хвост" — блоки 0..4.
    const QString around = QStringLiteral(
        "# Заголовок\n\n![Кадр](kadr.png)\n\nхвост\n");
    // Картинка последним блоком — так выглядит заметка сразу после вставки.
    const QString last = QStringLiteral("# Заголовок\n\n![Кадр](kadr.png)\n");

    // МАТРИЦА: подход к картинке × клавиша.
    probe("Enter на самой картинке", around, 2, false, Qt::Key_Return);
    probe("Enter в конце строки картинки", around, 2, true, Qt::Key_Return);
    probe("Backspace на пустой строке над картинкой", around, 1, true, Qt::Key_Backspace);
    // Delete на пустой строке НАД картинкой съедает её целиком: справа от
    // каретки именно она, а фотография — атом. Это не поломка, а правило —
    // «либо цела, либо удалена целиком», и промежуточного огрызка нет.
    probeGone("Delete на пустой строке над картинкой", around, 1, Qt::Key_Delete,
              Qt::NoModifier);
    // Симметрично: Backspace на пустой строке ПОД картинкой съедает её целиком.
    probeGone("Backspace на пустой строке под картинкой", around, 3, Qt::Key_Backspace,
              Qt::NoModifier);
    probe("Delete на пустой строке под картинкой", around, 3, false, Qt::Key_Delete);
    probe("Backspace в начале хвоста", around, 4, false, Qt::Key_Backspace);
    probe("Enter на картинке, стоящей последней", last, 2, false, Qt::Key_Return);
    probe("Backspace над картинкой, стоящей последней", last, 1, true, Qt::Key_Backspace);

    // Каретка НА картинке — значит объект действия она сама: удаляется целиком
    // (обеими клавишами), копируется и вырезается без выделения мышью.
    // Промежуточного состояния «осталась подпись-ссылка» не существует.
    probeGone("Backspace на картинке удаляет её целиком", around, 2, Qt::Key_Backspace,
              Qt::NoModifier);
    probeGone("Delete на картинке удаляет её целиком", around, 2, Qt::Key_Delete,
              Qt::NoModifier);
    probeGone("Ctrl+X на картинке вырезает её целиком", around, 2, Qt::Key_X,
              Qt::ControlModifier);
    probeClipboard("Ctrl+C на картинке кладёт в буфер ссылку", around, 2, Qt::Key_C,
                   Qt::ControlModifier, true);
    probeClipboard("Ctrl+X на картинке кладёт в буфер ссылку", around, 2, Qt::Key_X,
                   Qt::ControlModifier, false);

    // После картинки, стоящей ПОСЛЕДНЕЙ, обязано быть КУДА ВСТАТЬ. Дописывать
    // пустую строку при открытии нельзя — «открытие заметки не должно её
    // менять», это железное правило, и его стережёт свой набор. Значит
    // достижимость даёт Enter: нажал — получил строку и каретку в ней.
    {
        const QString path = noteWith(QStringLiteral("0000000000000z.md"), last);
        zametti::NoteEditor editor;
        editor.resize(600, 400);
        editor.show();
        editor.openFile(path);

        const int photo = blockOfPhoto(*editor.document());
        ZT_TRUE("картинка в заметке найдена", photo >= 0);
        ZT_TRUE("открытие заметки её не изменило", !editor.document()->isModified());
        if (photo >= 0) {
            QTextCursor caret = editor.textCursor();
            caret.setPosition(editor.document()->findBlockByNumber(photo).position());
            editor.setTextCursor(caret);
            // ЖЕСТ ПЕРЕЕХАЛ НА Ctrl+Enter (решение владельца, этап 12).
            //
            // Раньше строку за картинкой заводил обычный Enter. Теперь у всех
            // объектов — картинки, таблицы, формулы — правила одни: Enter
            // ПРАВИТ объект, а «встань на параграф после него» это Ctrl+Enter,
            // тот же жест, что и у блока кода. Инвариант, ради которого писана
            // эта проверка, не изменился: после последней картинки обязано
            // быть куда встать.
            const int before = editor.document()->blockCount();
            QTest::keyClick(&editor, Qt::Key_Return);
            QTest::qWait(20);
            ZT_TRUE("обычный Enter картинку не делит и строк не заводит",
                    editor.document()->blockCount() == before);

            QTest::keyClick(&editor, Qt::Key_Return, Qt::ControlModifier);
            QTest::qWait(20);

            ZT_TRUE("Ctrl+Enter завёл строку за последней картинкой",
                    editor.document()->blockCount() > photo + 1);
            ZT_TRUE("каретка встала в неё, а не осталась на картинке",
                    editor.textCursor().blockNumber() > photo);
            ZT_TRUE("сама картинка цела", photoCount(*editor.document()) == 1);
        }
    }

    return zt::report("края фотографии");
}

// Набор целиком одним TEST: тело не тронуто, argv ему собран здесь.
// Дробить на отдельные проверки — отдельная работа, по одному набору.
TEST(ImageEdge, All) {
    std::vector<QByteArray> ztArgs{QByteArrayLiteral("image_edge_test")};
    std::vector<char*> ztArgv;
    for (QByteArray& a : ztArgs) ztArgv.push_back(a.data());
    EXPECT_EQ(0, ztRunSuite(int(ztArgv.size()), ztArgv.data()));
}
