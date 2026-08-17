// Каретка на дробном масштабе экрана.
//
// Штатную каретку мы гасим шириной 0, но при дробном QT_SCALE_FACTOR Qt
// прижимает ширину курсора к целым физическим пикселям — ноль становится
// единицей, и рядом с нашей кареткой мигает чужая чёрная черта. Красить её
// нечем: она рисуется инверсией пикселей (замерено: палитра Text перекрашивает
// буквы, черту — нет). Лечение — не отдавать фокус QTextEdit (см.
// NoteView::focusInEvent); этот тест держит именно то, что чужая черта не
// вернётся с обновлением Qt.
//
// Черта отличается от ножки буквы ростом: она во всю высоту строки, ножки
// глифов — нет.

#include "editor_widget.h"
#include "settings.h"

#include <QApplication>
#include <QImage>
#include <QTest>
#include <QTextBlock>
#include <QTextCursor>

#include "test_util.h"

#include <vector>
#include "testdata.h"

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

static int ztRunSuite(int argc, char** argv) {
    (void)argc;
    (void)argv;
    // До создания QApplication, иначе масштаб не применится.
    qputenv("QT_SCALE_FACTOR", "1.25");
    zametti::loadSettings(nullptr);

    const fs::path dir = fs::temp_directory_path() / "zametti-caret-scale-test";
    fs::create_directories(dir);
    const fs::path note = dir / "каретка.md";
    std::ofstream(note, std::ios::binary) << "давайте напишем немного текста\n";

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

    // Кадры с тёмной вертикалью во весь рост строки в колонке каретки.
    // Ножка буквы столько не занимает — это может быть только чужая каретка.
    const auto alienFrames = [&editor] {
        const QRect at = editor.cursorRect();
        const qreal dpr = editor.devicePixelRatioF();
        const QColor caret = zametti::settings().style().caretColor();
        int frames = 0;
        for (int frame = 0; frame < 20; ++frame) {
            QImage shot = editor.viewport()->grab().toImage();
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
    };

    // Штатную каретку будят по-разному: фокус, стрелки, мышь, набор. Каждый
    // источник проверяется отдельно — фокусом её усыпить удавалось, а клавиши
    // будили снова.
    ZT_TRUE("после установки курсора", alienFrames() == 0);

    for (int i = 0; i < 3; ++i) QTest::keyClick(&editor, Qt::Key_Right);
    QTest::qWait(30);
    ZT_TRUE("после стрелок", alienFrames() == 0);

    const QRect r = editor.cursorRect();
    QTest::mouseClick(editor.viewport(), Qt::LeftButton, Qt::NoModifier,
                      QPoint(r.left() - 30, r.center().y()));
    QTest::qWait(30);
    ZT_TRUE("после щелчка мыши", alienFrames() == 0);

    editor.insertPlainText(QStringLiteral("x"));
    QTest::qWait(30);
    ZT_TRUE("после набора", alienFrames() == 0);

    return zt::report("каретка на дробном масштабе");
}

// Набор целиком одним TEST: тело не тронуто, argv ему собран здесь.
// Дробить на отдельные проверки — отдельная работа, по одному набору.
TEST(CaretScale, All) {
    std::vector<QByteArray> ztArgs{QByteArrayLiteral("caret_scale_test")};
    std::vector<char*> ztArgv;
    for (QByteArray& a : ztArgs) ztArgv.push_back(a.data());
    EXPECT_EQ(0, ztRunSuite(int(ztArgv.size()), ztArgv.data()));
}
