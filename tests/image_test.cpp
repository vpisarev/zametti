// Показ картинок в просмотре.
//
// Модель текста картинок не знает: строка остаётся строкой (подпись
// image-спана или дословное вики-вложение "![[путь|ширина]]"), но рисуется
// на её месте сама фотография — строка хитро-отрисованная, как черта. Текст
// показывается, только когда строку задевает выделение. Место резервирует
// syncImageSpace нижним полем блока: скрытая строка — фото минус высота
// строки, показанная — фото целиком под текстом.
//
// Ресайз: угол фотографии тянется мышью, отпускание записывает ширину
// операцией — вики-вложению в "|ширину", image-спану в "#w=" пути.

#include "doc_model.h"
#include "document_reader.h"
#include "editor_ops.h"
#include "editor_widget.h"
#include "serializer.h"
#include "settings.h"

#include "test_util.h"

#include <QApplication>
#include <QClipboard>
#include <QGuiApplication>
#include <QImage>
#include <QMouseEvent>
#include <QPainter>
#include <QTest>
#include <QTextBlock>
#include <QTextCursor>

#include <cmath>
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

// Та же отбивка, что у вида (imageGap при масштабе 1).
const qreal kGap = 6.0;

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

    const auto blockAt = [&](int n) { return editor.document()->findBlockByNumber(n); };
    const auto marginOf = [&](int n) { return blockAt(n).blockFormat().bottomMargin(); };
    const auto lineOf = [&](int n) { return blockAt(n).blockFormat().lineHeight(); };

    // Скрытая строка: фото стоит на месте текста и торчит из него вниз.
    ZT_TRUE("резерв image-спана: фото плюс отбивка минус строка",
            std::fabs(marginOf(0) - (64.0 + kGap - lineOf(0))) < 1.5);
    ZT_TRUE("резерв вики-вложения: ширина 40 уважена",
            std::fabs(marginOf(2) - qMax(0.0, 40.0 + kGap - lineOf(2))) < 1.5);
    // Файла нет — и места нет.
    ZT_TRUE("под пропавший файл места нет", marginOf(4) == 0.0);
    ZT_TRUE("под обычный текст места нет", marginOf(6) == 0.0);

    // Цвет в центре фотографии блока — тонировку видно по нему.
    const auto shadeOf = [&](int n) {
        QImage frame(editor.viewport()->size(), QImage::Format_RGB32);
        frame.fill(Qt::white);
        QPainter painter(&frame);
        editor.viewport()->render(&painter);
        const QRectF photo = editor.imageRectInViewport(blockAt(n));
        return frame.pixelColor(photo.center().toPoint());
    };
    const auto sameShade = [](const QColor& a, const QColor& b) {
        return a.red() == b.red() && a.green() == b.green() && a.blue() == b.blue();
    };
    const auto caretTo = [&](int n) {
        editor.setTextCursor(QTextCursor(blockAt(n)));
        QTest::qWait(10);
    };

    // Выделение — это выделенная фотография, а не вскрытая разметка: текст
    // не показывается, резерв не дёргается, поверх фото ложится тонировка.
    {
        caretTo(6);   // каретка в стороне: она тонирует фото сама по себе
        const QColor plain = shadeOf(2);

        QTextCursor cursor(blockAt(2));
        cursor.movePosition(QTextCursor::Right, QTextCursor::KeepAnchor, 3);
        editor.setTextCursor(cursor);
        QTest::qWait(10);
        ZT_TRUE("выделение не тронуло резерв",
                std::fabs(marginOf(2) - qMax(0.0, 40.0 + kGap - lineOf(2))) < 1.5);
        ZT_TRUE("выделенная фотография тонирована", !sameShade(plain, shadeOf(2)));

        caretTo(6);
        ZT_TRUE("тонировка снята вместе с выделением", sameShade(plain, shadeOf(2)));

        // Каретка, вставшая на строку-фотографию, — та же выбранная
        // фотография: тонировка без всякого выделения.
        caretTo(2);
        ZT_TRUE("каретка на фотографии тонирует её", !sameShade(plain, shadeOf(2)));
        caretTo(6);
        ZT_TRUE("каретка ушла — тонировка снята", sameShade(plain, shadeOf(2)));
    }

    // Фотографии действительно в кадре: красных пикселей не меньше, чем в
    // самих картинках (64×64 + 40×40), с запасом на сглаживание краёв.
    QImage shot(editor.viewport()->size(), QImage::Format_RGB32);
    shot.fill(Qt::white);
    {
        QPainter painter(&shot);
        editor.viewport()->render(&painter);
    }
    ZT_TRUE("фотографии нарисованы", countReddish(shot) > 64 * 64 + 40 * 40 - 600);

    // Ресайз мышью: взяться за нижний правый угол, потянуть вправо, отпустить
    // — ширина записывается в "|ширину" вики-вложения, отдельным шагом истории.
    {
        const QRectF photo = editor.imageRectInViewport(blockAt(2));
        ZT_TRUE("фото вики-вложения имеет прямоугольник", !photo.isEmpty());
        const QPointF grip(photo.right() - 4.0, photo.bottom() - 4.0);
        const QPointF pulled = grip + QPointF(26.0, 9.0);
        const int expected = qRound(pulled.x() - photo.left());

        QTest::mousePress(editor.viewport(), Qt::LeftButton, {}, grip.toPoint());
        QMouseEvent drag(QEvent::MouseMove, pulled, editor.viewport()->mapToGlobal(pulled),
                         Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(editor.viewport(), &drag);
        QTest::mouseRelease(editor.viewport(), Qt::LeftButton, {}, pulled.toPoint());
        QTest::qWait(10);

        ZT_EQ("ширина записана в вики-вложение",
              QStringLiteral("![[img.png|%1]]").arg(expected).toStdString(),
              blockAt(2).text().toStdString());
        ZT_TRUE("резерв пересчитан под новую ширину",
                std::fabs(marginOf(2) - qMax(0.0, expected + kGap - lineOf(2))) < 1.5);

        QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
        QTest::qWait(10);
        ZT_EQ("Ctrl-Z вернул прежнюю ширину", std::string("![[img.png|40]]"),
              blockAt(2).text().toStdString());
    }

    // Запись ширины image-спана — во фрагмент пути "#w=". Операция напрямую:
    // мышь уже проверена на вики-форме, путь тот же.
    {
        QTextCursor cursor(blockAt(0));
        ZT_TRUE("ширина image-спана записана",
                zametti::setImageWidthAtCursor(*editor.document(), cursor, 50));
        QTest::qWait(10);
        const zametti::Document ir = zametti::readDocument(*editor.document());
        const std::string out = zametti::serialize(ir);
        ZT_TRUE("в файл уходит путь с #w=50",
                out.find("![фото](img.png#w=50)") != std::string::npos);
        ZT_TRUE("резерв ужался до 50",
                std::fabs(marginOf(0) - qMax(0.0, 50.0 + kGap - lineOf(0))) < 1.5);
    }

    // Правка ломает путь вики-вложения — резерв обязан сняться.
    QTextCursor cursor(blockAt(2));
    cursor.movePosition(QTextCursor::EndOfBlock);
    editor.setTextCursor(cursor);
    editor.insertPlainText(QStringLiteral("х"));
    QTest::qWait(10);
    ZT_TRUE("правка сняла резерв: вложение испорчено", marginOf(2) == 0.0);

    // И возврат правкой же — резерв возвращается.
    QTest::keyClick(&editor, Qt::Key_Backspace);
    QTest::qWait(10);
    ZT_TRUE("резерв вернулся после починки строки",
            std::fabs(marginOf(2) - qMax(0.0, 40.0 + kGap - lineOf(2))) < 1.5);

    // Ctrl+C/Ctrl+X/Ctrl+V: каретка на картинке — выбранная картинка. В
    // клипборд идёт текстовое представление строки, вставка идёт через полный
    // парсер ядра и встаёт своей строкой.
    {
        QClipboard* clipboard = QGuiApplication::clipboard();
        caretTo(2);
        QTest::keyClick(&editor, Qt::Key_C, Qt::ControlModifier);
        ZT_EQ("Ctrl+C кладёт текст вложения", std::string("![[img.png|40]]"),
              clipboard->text().toStdString());

        const int blocksBefore = editor.document()->blockCount();
        QTest::keyClick(&editor, Qt::Key_X, Qt::ControlModifier);
        QTest::qWait(10);
        ZT_TRUE("Ctrl+X убрал строку целиком",
                editor.document()->blockCount() == blocksBefore - 1 &&
                    !editor.toPlainText().contains(QStringLiteral("img.png|40")));
        ZT_EQ("клипборд после Ctrl+X цел", std::string("![[img.png|40]]"),
              clipboard->text().toStdString());

        // Вставка в конец: фотография встаёт своей строкой, не вклеивается.
        QTextCursor end(editor.document());
        end.movePosition(QTextCursor::End);
        editor.setTextCursor(end);
        QTest::keyClick(&editor, Qt::Key_V, Qt::ControlModifier);
        QTest::qWait(10);
        int pasted = -1;
        for (QTextBlock b = editor.document()->begin(); b.isValid(); b = b.next())
            if (b.text() == QStringLiteral("![[img.png|40]]")) pasted = b.blockNumber();
        ZT_TRUE("Ctrl+V вернул строку-фотографию", pasted >= 0);
        if (pasted >= 0) {
            ZT_TRUE("вставленная строка — своя, не вклейка",
                    blockAt(pasted).text() == QStringLiteral("![[img.png|40]]"));
            ZT_TRUE("резерв места у вставленной есть",
                    std::fabs(marginOf(pasted) -
                              qMax(0.0, 40.0 + kGap - lineOf(pasted))) < 1.5);
        }

        // Откат: вставка и вырезание — по своему шагу истории.
        QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
        QTest::qWait(10);
        ZT_TRUE("Ctrl-Z убрал вставленную",
                !editor.toPlainText().contains(QStringLiteral("img.png|40")));
        QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
        QTest::qWait(10);
        ZT_EQ("Ctrl-Z вернул вырезанную на место", std::string("![[img.png|40]]"),
              blockAt(2).text().toStdString());
    }

    // Канон image-спана из клипборда тоже встаёт фотографией: полный парсер
    // ядра превращает текст в спан, ширина — из "#w=".
    {
        QGuiApplication::clipboard()->setText(QStringLiteral("![пейзаж](img.png#w=33)"));
        QTextCursor end(editor.document());
        end.movePosition(QTextCursor::End);
        editor.setTextCursor(end);
        QTest::keyClick(&editor, Qt::Key_V, Qt::ControlModifier);
        QTest::qWait(10);
        int pasted = -1;
        for (QTextBlock b = editor.document()->begin(); b.isValid(); b = b.next())
            if (b.text() == QStringLiteral("пейзаж")) pasted = b.blockNumber();
        ZT_TRUE("канон вставился image-спаном", pasted >= 0);
        if (pasted >= 0) {
            const zametti::BlockImageRef ref = zametti::blockImageRef(blockAt(pasted));
            ZT_TRUE("вставленный спан — фотография с шириной из #w=",
                    ref.valid && !ref.wiki && qRound(ref.widthHint) == 33);
        }
    }

    return zt::report("картинки в просмотре");
}
