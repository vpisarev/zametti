// Каретка на дробном масштабе экрана — в обычном виде и в виде исходника.
//
// Штатную каретку мы гасим шириной 0, но при дробном QT_SCALE_FACTOR Qt
// прижимает ширину курсора к целым физическим пикселям — ноль становится
// единицей, и рядом с нашей кареткой мигает чужая чёрная черта. Красить её
// нечем: она рисуется инверсией пикселей (замерено: палитра Text перекрашивает
// буквы, черту — нет). NoteView лечит это перерисовкой колонки каретки после
// штатной отрисовки (repaintOverNativeCaret); этот набор держит, что чужая
// черта не вернётся с обновлением Qt, — и спрашивает то же у вида исходника.
//
// Черта отличается от ножки буквы ростом: она во всю высоту строки, ножки
// глифов — нет.
//
// ДОЧЕРНИМ ПРОЦЕССОМ. Масштаб читается при создании QApplication, а в общем
// бинарнике наборов он создан в main — задним числом QT_SCALE_FACTOR ничего не
// меняет, и прежде набор молча шёл на масштабе 1.0 (нашлось в сессии 9).
// Теперь набор запускает сам себя с переменной в окружении, а ребёнок
// проверяет, что масштаб ДЕЙСТВИТЕЛЬНО дробный, — иначе проверка пустая.

#include "editor_widget.h"
#include "markdown_edit_view.h"
#include "settings.h"

#include <QApplication>
#include <QCoreApplication>
#include <QImage>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTest>
#include <QTextBlock>
#include <QTextCursor>

#include "test_util.h"

#include <vector>
#include "testdata.h"

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace {

constexpr const char* kChildFlag = "ZAMETTI_CARET_SCALE_CHILD";

// Кадры с тёмной вертикалью во весь рост строки в колонке каретки. Ножка
// буквы столько не занимает — это может быть только чужая каретка.
template <class View>
int alienFrames(View& view) {
    const QRect at = view.cursorRect();
    const qreal dpr = view.devicePixelRatioF();
    const QColor caret = zametti::settings().style().caretColor();
    int frames = 0;
    for (int frame = 0; frame < 20; ++frame) {
        QImage shot = view.viewport()->grab().toImage();
        shot.setDevicePixelRatio(1.0);
        const int left = int((at.left() - 2) * dpr);
        const int right = int((at.left() + 4) * dpr);
        const int top = int(at.top() * dpr);
        const int bottom = int(at.bottom() * dpr);
        bool alien = false;
        for (int x = qMax(0, left); x <= right && x < shot.width(); ++x) {
            int darkRun = 0;
            int best = 0;
            for (int y = qMax(0, top); y <= bottom && y < shot.height(); ++y) {
                const QColor c = shot.pixelColor(x, y);
                const bool dark = c.lightness() < 70 && c != caret;
                darkRun = dark ? darkRun + 1 : 0;
                best = qMax(best, darkRun);
            }
            if (best > (bottom - top) * 85 / 100) alien = true;
        }
        if (alien) ++frames;
        QTest::qWait(45);
    }
    return frames;
}

// Штатную каретку будят по-разному: фокус, стрелки, мышь, набор. Каждый
// источник проверяется отдельно — фокусом её усыпить удавалось, а клавиши
// будили снова.
template <class View>
void checkView(View& view, const std::string& name) {
    ZT_TRUE(name + ": масштаб показа дробный (" + std::to_string(view.devicePixelRatioF()) + ")",
            view.devicePixelRatioF() > 1.2);
    ZT_TRUE(name + ": после установки курсора", alienFrames(view) == 0);

    for (int i = 0; i < 3; ++i) QTest::keyClick(&view, Qt::Key_Right);
    QTest::qWait(30);
    ZT_TRUE(name + ": после стрелок", alienFrames(view) == 0);

    const QRect r = view.cursorRect();
    QTest::mouseClick(view.viewport(), Qt::LeftButton, Qt::NoModifier,
                      QPoint(r.left() - 30, r.center().y()));
    QTest::qWait(30);
    ZT_TRUE(name + ": после щелчка мыши", alienFrames(view) == 0);

    view.insertPlainText(QStringLiteral("x"));
    QTest::qWait(30);
    ZT_TRUE(name + ": после набора", alienFrames(view) == 0);
}

int runChild() {
    zametti::loadSettings(nullptr);

    const fs::path dir = fs::temp_directory_path() / "zametti-caret-scale-test";
    fs::create_directories(dir);
    const fs::path note = dir / "каретка.md";
    std::ofstream(note, std::ios::binary) << "давайте напишем немного текста\n";

    {
        zametti::NoteEditor editor;
        editor.resize(900, 700);
        editor.show();
        QTest::qWait(30);
        editor.setFocus();
        zametti::applyPalette(editor);
        editor.openFile(QString::fromStdString(note.string()));
        QTest::qWait(50);

        QTextCursor cursor(editor.document());
        cursor.setPosition(editor.document()->firstBlock().position() + 5);
        editor.setTextCursor(cursor);
        QTest::qWait(30);
        checkView(editor, "обычный вид");
    }
    {
        zametti::MarkdownEditView view;
        view.resize(900, 700);
        view.show();
        QTest::qWait(30);
        view.setFocus();
        view.showSource(QStringLiteral("давайте напишем немного текста\n"), {0, 5, true});
        QTest::qWait(50);
        checkView(view, "вид исходника");
    }

    return zt::report("каретка на дробном масштабе");
}

}  // namespace

TEST(CaretScale, All) {
    if (qEnvironmentVariableIsSet(kChildFlag)) {
        EXPECT_EQ(0, runChild());
        return;
    }
    // Родитель: тот же бинарник, тот же набор, но с масштабом в окружении ДО
    // создания QApplication.
    QProcess child;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QString::fromLatin1(kChildFlag), QStringLiteral("1"));
    env.insert(QStringLiteral("QT_SCALE_FACTOR"), QStringLiteral("1.25"));
    child.setProcessEnvironment(env);
    child.setProcessChannelMode(QProcess::ForwardedChannels);
    child.start(QCoreApplication::applicationFilePath(),
                {QStringLiteral("--gtest_filter=CaretScale.All")});
    ASSERT_TRUE(child.waitForStarted(10000));
    ASSERT_TRUE(child.waitForFinished(120000));
    EXPECT_EQ(QProcess::NormalExit, child.exitStatus());
    EXPECT_EQ(0, child.exitCode());
}
