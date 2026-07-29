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

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

int main(int argc, char** argv) {
    // До создания QApplication, иначе масштаб не применится.
    qputenv("QT_SCALE_FACTOR", "1.25");
    QApplication app(argc, argv);
    zametti::loadAppearance(nullptr);

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

    const QRect at = editor.cursorRect();
    const qreal dpr = editor.devicePixelRatioF();
    const QColor caret = zametti::appearance().caretColor;

    // Полный цикл мигания обеих кареток с запасом.
    int alienFrames = 0;
    for (int frame = 0; frame < 24; ++frame) {
        QImage shot = editor.viewport()->grab().toImage();
        shot.setDevicePixelRatio(1.0);
        const int left = int((at.left() - 2) * dpr);
        const int right = int((at.left() + 4) * dpr);
        const int top = int(at.top() * dpr);
        const int bottom = int(at.bottom() * dpr);
        for (int x = left; x <= right && x < shot.width(); ++x) {
            if (x < 0) continue;
            int darkRun = 0;
            int best = 0;
            for (int y = qMax(0, top); y <= bottom && y < shot.height(); ++y) {
                const QColor c = shot.pixelColor(x, y);
                const bool dark = c.lightness() < 70 && c != caret;
                darkRun = dark ? darkRun + 1 : 0;
                best = qMax(best, darkRun);
            }
            // Во всю высоту строки — ножка буквы столько не занимает.
            if (best > (bottom - top) * 85 / 100) ++alienFrames;
        }
        QTest::qWait(45);
    }
    ZT_TRUE("чужая каретка: тёмная вертикаль во весь рост строки, кадров " +
                std::to_string(alienFrames),
            alienFrames == 0);

    return zt::report("каретка на дробном масштабе");
}
