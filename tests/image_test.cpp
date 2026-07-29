// Показ картинок в просмотре.
//
// Модель текста картинок не знает: строка остаётся строкой, а фотография
// рисуется в нижнем поле блока — место резервирует syncImageSpace. Здесь
// проверяются обе формы (image-спан целым абзацем и вики-вложение
// "![[путь|ширина]]"), отсутствие файла, снятие резерва правкой и то, что
// фотография действительно попадает в кадр (по пикселям отрисовки).

#include "doc_model.h"
#include "editor_widget.h"
#include "settings.h"

#include "test_util.h"

#include <QApplication>
#include <QImage>
#include <QPainter>
#include <QTest>
#include <QTextBlock>
#include <QTextCursor>

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace {

int countReddish(const QImage& shot) {
    int hits = 0;
    for (int y = 0; y < shot.height(); ++y)
        for (int x = 0; x < shot.width(); ++x) {
            const QColor c = shot.pixelColor(x, y);
            if (c.red() > 180 && c.green() < 90 && c.blue() < 90) ++hits;
        }
    return hits;
}

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    zametti::loadAppearance(nullptr);

    const fs::path dir = fs::temp_directory_path() / "zametti-image-test";
    fs::create_directories(dir);

    // Красный квадрат 64×64 — его пиксели потом ищутся в кадре.
    QImage square(64, 64, QImage::Format_RGB32);
    square.fill(QColor(220, 30, 30));
    ZT_TRUE("картинка записана",
            square.save(QString::fromStdString((dir / "img.png").string())));

    const char* source =
        "![фото](img.png)\n"
        "\n"
        "![[img.png|40]]\n"
        "\n"
        "![нет такой](пропала.png)\n"
        "\n"
        "просто текст\n";
    {
        std::ofstream out(dir / "н.md", std::ios::binary);
        out << source;
    }

    zametti::NoteEditor editor;
    editor.resize(600, 500);
    editor.show();
    QTest::qWait(20);
    editor.openFile(QString::fromStdString((dir / "н.md").string()));
    QTest::qWait(20);

    const auto marginOf = [&](int blockNumber) {
        return editor.document()->findBlockByNumber(blockNumber).blockFormat().bottomMargin();
    };

    // Спан целым абзацем: место под фото своей ширины (64 логических пикселя).
    ZT_TRUE("под image-спан зарезервировано место", marginOf(0) > 64.0);
    // Вики-вложение с шириной 40: квадрат — значит, и высота 40 плюс отбивки.
    ZT_TRUE("под вики-вложение зарезервировано место", marginOf(2) > 40.0);
    ZT_TRUE("ширина из вики-вложения уважена: место меньше своего размера",
            marginOf(2) < marginOf(0));
    // Файла нет — и места нет.
    ZT_TRUE("под пропавший файл места нет", marginOf(4) == 0.0);
    ZT_TRUE("под обычный текст места нет", marginOf(6) == 0.0);

    // Фотографии действительно в кадре: красных пикселей не меньше, чем в
    // самих картинках (64×64 + 40×40), с запасом на сглаживание краёв.
    QImage shot(editor.viewport()->size(), QImage::Format_RGB32);
    shot.fill(Qt::white);
    {
        QPainter painter(&shot);
        editor.viewport()->render(&painter);
    }
    const int reds = countReddish(shot);
    ZT_TRUE("фотографии нарисованы", reds > 64 * 64 + 40 * 40 - 600);

    // Правка ломает путь вики-вложения — резерв обязан сняться.
    QTextCursor cursor(editor.document()->findBlockByNumber(2));
    cursor.movePosition(QTextCursor::EndOfBlock);
    editor.setTextCursor(cursor);
    editor.insertPlainText(QStringLiteral("х"));
    QTest::qWait(10);
    ZT_TRUE("правка сняла резерв: вложение испорчено", marginOf(2) == 0.0);

    // И возврат правкой же — резерв возвращается.
    QTest::keyClick(&editor, Qt::Key_Backspace);
    QTest::qWait(10);
    ZT_TRUE("резерв вернулся после починки строки", marginOf(2) > 40.0);

    return zt::report("картинки в просмотре");
}
