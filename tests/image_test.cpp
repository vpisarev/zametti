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
#include <QImageReader>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollBar>
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
        // Математика дельтой: насколько уехала мышь, настолько выросла ширина.
        const int expected = qRound(photo.width() + 26.0);

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

    // Esc посреди жеста — отмена без записи: ни текста, ни шага истории.
    {
        const std::string before = blockAt(2).text().toStdString();
        const qreal roomBefore = marginOf(2);
        const QRectF photo = editor.imageRectInViewport(blockAt(2));
        const QPointF grip(photo.right() - 4.0, photo.bottom() - 4.0);
        const QPointF pulled = grip + QPointF(40.0, 14.0);

        QTest::mousePress(editor.viewport(), Qt::LeftButton, {}, grip.toPoint());
        QMouseEvent drag(QEvent::MouseMove, pulled, editor.viewport()->mapToGlobal(pulled),
                         Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(editor.viewport(), &drag);
        QTest::qWait(10);
        ZT_TRUE("во время жеста фото и правда тянется",
                editor.imageRectInViewport(blockAt(2)).width() > photo.width() + 20.0);

        QTest::keyClick(&editor, Qt::Key_Escape);
        QTest::qWait(10);
        ZT_TRUE("Esc вернул примерочную ширину на место",
                std::fabs(editor.imageRectInViewport(blockAt(2)).width() - photo.width()) < 1.5);

        QTest::mouseRelease(editor.viewport(), Qt::LeftButton, {}, pulled.toPoint());
        QTest::qWait(10);
        ZT_EQ("после Esc отпускание ничего не записывает", before,
              blockAt(2).text().toStdString());
        ZT_TRUE("и резерв остался прежним", std::fabs(marginOf(2) - roomBefore) < 1.5);
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

    // Прокрутка: верх фотографии ушёл за кадр — остальная часть обязана
    // остаться на экране (hitTest по верхней кромке отдаёт следующий блок,
    // без шага назад картинка пропадала целиком).
    {
        QTextCursor cursor(blockAt(0));
        editor.setTextCursor(cursor);
        zametti::setImageWidthAtCursor(*editor.document(), cursor, 400);
        QTest::qWait(20);
        caretTo(6);   // тонировка от каретки не должна мешать замеру красного

        QRectF photo = editor.imageRectInViewport(blockAt(0));
        ZT_TRUE("фотография раздута до 400", photo.height() > 300.0);
        // Прокрутить так, чтобы верх фото был выше кадра на треть высоты.
        editor.verticalScrollBar()->setValue(
            editor.verticalScrollBar()->value() + int(photo.top() + photo.height() / 3));
        QTest::qWait(20);
        photo = editor.imageRectInViewport(blockAt(0));
        ZT_TRUE("верх фото действительно за кадром",
                photo.top() < 0.0 && photo.bottom() > 0.0);

        QImage frame(editor.viewport()->size(), QImage::Format_RGB32);
        frame.fill(Qt::white);
        {
            QPainter painter(&frame);
            editor.viewport()->render(&painter);
        }
        ZT_TRUE("хвост фотографии в кадре дорисован",
                countReddish(frame) > int(photo.width() * photo.bottom() * 0.8));
    }

    // Левый верхний угол: тянем ВЛЕВО — фотография растёт (дельта от центра).
    {
        caretTo(6);
        const QRectF photo = editor.imageRectInViewport(blockAt(2));
        const QPointF grip(photo.left() + 3.0, photo.top() + 3.0);
        const QPointF pulled = grip + QPointF(-15.0, -5.0);
        const int expected = qRound(photo.width() + 15.0);

        QTest::mousePress(editor.viewport(), Qt::LeftButton, {}, grip.toPoint());
        QMouseEvent drag(QEvent::MouseMove, pulled, editor.viewport()->mapToGlobal(pulled),
                         Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(editor.viewport(), &drag);
        QTest::mouseRelease(editor.viewport(), Qt::LeftButton, {}, pulled.toPoint());
        QTest::qWait(10);
        ZT_EQ("левый угол растит ширину",
              QStringLiteral("![[img.png|%1]]").arg(expected).toStdString(),
              blockAt(2).text().toStdString());
        QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
        QTest::qWait(10);
        ZT_EQ("Ctrl-Z вернул ширину после левого угла", std::string("![[img.png|40]]"),
              blockAt(2).text().toStdString());
    }

    // Фотография — атом: свежая заметка, чтобы соседство было предсказуемым.
    {
        const char* atomSource =
            "строка\n"
            "\n"
            "![[img.png|40]]\n"
            "\n"
            "хвост\n";
        {
            std::ofstream out(dir / "а.md", std::ios::binary);
            out << atomSource;
        }
        editor.document()->setModified(false);
        editor.openFile(QString::fromStdString((dir / "а.md").string()));
        QTest::qWait(20);

        // Каретка залетела в середину скрытого текста — её сносит к началу.
        {
            QTextCursor inside(editor.document());
            inside.setPosition(blockAt(2).position() + 5);
            editor.setTextCursor(inside);
            QTest::qWait(10);
            ZT_TRUE("каретка внутри фото сведена к началу строки",
                    editor.textCursor().position() == blockAt(2).position());
        }
        // Вправо с начала — прыжок через всю строку; влево обратно — к началу.
        QTest::keyClick(&editor, Qt::Key_Right);
        ZT_TRUE("шаг вправо перепрыгивает фото",
                editor.textCursor().position() == blockAt(3).position());
        QTest::keyClick(&editor, Qt::Key_Left);
        ZT_TRUE("шаг влево возвращает к началу фото",
                editor.textCursor().position() == blockAt(2).position());

        // Backspace на самой строке: атом гибнет целиком, огрызков не остаётся.
        QTest::keyClick(&editor, Qt::Key_Backspace);
        QTest::qWait(10);
        ZT_TRUE("Backspace на фото убрал строку без огрызков",
                !editor.toPlainText().contains(QStringLiteral("img.png")) &&
                    !editor.toPlainText().contains(QStringLiteral("[[")));
        QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
        QTest::qWait(10);
        ZT_EQ("Ctrl-Z вернул фото-атом", std::string("![[img.png|40]]"),
              blockAt(2).text().toStdString());

        // Backspace из-под фотографии: пустую строку под фото удалить нельзя
        // (склеила бы текст со скрытой подписью) — отказ и шаг: каретка встаёт
        // на фотографию, следующий Backspace убирает её целиком.
        {
            QTextCursor below(editor.document());
            below.setPosition(blockAt(4).position());
            editor.setTextCursor(below);
        }
        QTest::keyClick(&editor, Qt::Key_Backspace);
        ZT_TRUE("отказ и шаг: каретка встала на фото, документ цел",
                editor.textCursor().position() == blockAt(2).position() &&
                    blockAt(2).text() == QStringLiteral("![[img.png|40]]"));
        QTest::keyClick(&editor, Qt::Key_Backspace);
        QTest::qWait(10);
        ZT_TRUE("Backspace снизу: фото ушло атомом, соседи целы",
                !editor.toPlainText().contains(QStringLiteral("img.png")) &&
                    editor.toPlainText().contains(QStringLiteral("хвост")) &&
                    editor.toPlainText().contains(QStringLiteral("строка")));
        QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
        QTest::qWait(10);
        ZT_EQ("история вернула фото", std::string("![[img.png|40]]"),
              blockAt(2).text().toStdString());

        // Delete сверху: то же правило зеркально — отказ и шаг, затем атом.
        {
            QTextCursor above(editor.document());
            above.setPosition(blockAt(0).position() + blockAt(0).length() - 1);
            editor.setTextCursor(above);
        }
        QTest::keyClick(&editor, Qt::Key_Delete);
        ZT_TRUE("Delete сверху: отказ и шаг на фото",
                editor.textCursor().position() == blockAt(2).position());
        QTest::keyClick(&editor, Qt::Key_Delete);
        QTest::qWait(10);
        ZT_TRUE("Delete сверху: фото ушло атомом",
                !editor.toPlainText().contains(QStringLiteral("img.png")) &&
                    editor.toPlainText().contains(QStringLiteral("строка")));

        // Щелчок по фотографии выбирает её: каретка в начале строки.
        QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
        QTest::qWait(10);
        // Щелчок по фотографии выбирает её: каретка в начале строки.
        QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
        QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
        QTest::qWait(10);
        const QRectF photo = editor.imageRectInViewport(blockAt(2));
        ZT_TRUE("фото на месте перед щелчком", !photo.isEmpty());
        QTest::mouseClick(editor.viewport(), Qt::LeftButton, {},
                          photo.center().toPoint());
        QTest::qWait(10);
        ZT_TRUE("щелчок по фото поставил каретку в начало его строки",
                editor.textCursor().position() == blockAt(2).position() &&
                    !editor.textCursor().hasSelection());
    }

    // --- кэш картинок: потолок, вытеснение, защита от бомбы ---
    //
    // Кэш живёт всю сессию, а не заметку, поэтому у него есть бюджет. Правила
    // владельца: сначала добавляем, потом убираем; картинки ОТКРЫТОЙ заметки
    // не вытесняем никогда.
    //
    // Ниже восьми мегабайт бюджет не опускается: кэш на одну картинку смысла
    // не имеет. Числа ниже подобраны под это, а не взяты с потолка.
    {
        const fs::path cacheDir = dir / "кэш";
        fs::create_directories(cacheDir);
        // Восемнадцать картинок по мегабайту разжатыми: 512x512 RGB32.
        const int kSide = 512;
        const qint64 kOne = qint64(kSide) * kSide * 4;
        for (int i = 0; i < 20; ++i) {
            QImage tile(kSide, kSide, QImage::Format_RGB32);
            tile.fill(QColor(10 * i, 40, 200));
            ZT_TRUE("картинка кэша записана",
                    tile.save(QString::fromStdString(
                        (cacheDir / ("к" + std::to_string(i) + ".png")).string())));
        }
        // Десять заметок по две картинки.
        for (int n = 0; n < 10; ++n) {
            std::ofstream out(cacheDir / ("з" + std::to_string(n) + ".md"), std::ios::binary);
            out << "![[к" << (n * 2) << ".png]]\n\n![[к" << (n * 2 + 1) << ".png]]\n";
        }

        // Свой редактор: кэш у каждого вида собственный, и картинки проверок
        // выше сбивали бы счёт.
        zametti::NoteEditor cacheEditor;
        cacheEditor.resize(600, 500);
        cacheEditor.show();
        QTest::qWait(20);
        const auto openNote = [&](int n) {
            cacheEditor.openFile(QString::fromStdString(
                (cacheDir / ("з" + std::to_string(n) + ".md")).string()));
            QTest::qWait(20);
        };

        const int savedBudget = zametti::appearance().imageCacheSizeMb;
        // Бюджет 16 МБ: в кэш влезает шестнадцать мегабайтных картинок,
        // а всего их двадцать.
        zametti::appearance().imageCacheSizeMb = 16;
        zametti::applyImageAllocationLimit();

        zametti::NoteView::resetImageDecodeCounters();
        openNote(0);
        ZT_EQ("первое открытие разжало обе картинки", std::to_string(2),
              std::to_string(zametti::NoteView::imageDecodes()));
        ZT_EQ("в кэше две записи", std::to_string(2),
              std::to_string(cacheEditor.cachedImageCount()));
        ZT_EQ("вес кэша — две картинки", std::to_string(2 * kOne),
              std::to_string(cacheEditor.imageCacheBytes()));

        // Повторное открытие той же заметки не разжимает ничего.
        zametti::NoteView::resetImageDecodeCounters();
        openNote(0);
        ZT_EQ("повторное открытие не разжимает ничего", std::to_string(0),
              std::to_string(zametti::NoteView::imageDecodes()));

        for (int n = 1; n < 9; ++n) openNote(n);
        ZT_TRUE("кэш уложился в бюджет",
                cacheEditor.imageCacheBytes() <= 16 * 1024 * 1024);
        ZT_TRUE("вытеснение случилось: записей меньше, чем картинок",
                cacheEditor.cachedImageCount() < 18);
        zametti::NoteView::resetImageDecodeCounters();
        cacheEditor.repaint();
        QTest::qWait(0);
        ZT_EQ("картинки открытой заметки не вытеснены", std::to_string(0),
              std::to_string(zametti::NoteView::imageDecodes()));

        // Возврат в самую старую заметку: её картинки успели вытесниться.
        zametti::NoteView::resetImageDecodeCounters();
        openNote(0);
        ZT_EQ("вытесняется самое старое", std::to_string(2),
              std::to_string(zametti::NoteView::imageDecodes()));

        // Заметка тяжелее всего бюджета: её картинки всё равно все на месте.
        // Ниже восьми мегабайт бюджет не опускается — кэш на одну картинку
        // смысла не имеет, — поэтому заметка берётся из десяти.
        {
            std::ofstream out(cacheDir / "тяжёлая.md", std::ios::binary);
            for (int i = 10; i < 20; ++i) out << "![[к" << i << ".png]]\n\n";
        }
        zametti::appearance().imageCacheSizeMb = 1;   // упрётся в нижние 8 МБ
        zametti::NoteEditor heavy;
        heavy.resize(600, 500);
        heavy.show();
        QTest::qWait(20);
        heavy.openFile(QString::fromStdString((cacheDir / "тяжёлая.md").string()));
        QTest::qWait(20);
        ZT_EQ("на одну заметку кэша хватает всегда: все десять на месте",
              std::to_string(10), std::to_string(heavy.cachedImageCount()));
        ZT_TRUE("и кэш при этом заведомо больше бюджета",
                heavy.imageCacheBytes() > 8 * 1024 * 1024);
        zametti::NoteView::resetImageDecodeCounters();
        heavy.repaint();
        QTest::qWait(0);
        ZT_EQ("и ни одна не разжимается заново", std::to_string(0),
              std::to_string(zametti::NoteView::imageDecodes()));

        zametti::appearance().imageCacheSizeMb = savedBudget;
        zametti::applyImageAllocationLimit();
    }

    // Картинка больше потолка: Qt её не разжимает, вместо неё рамка с
    // надписью. Место она занимает ровно то же, что заняла бы сама картинка —
    // иначе вёрстка прыгала бы от правки потолка в конфиге. Это и проверяем:
    // один и тот же файл при высоком потолке и при низком.
    {
        const fs::path bombDir = dir / "бомба";
        fs::create_directories(bombDir);
        QImage huge(1024, 768, QImage::Format_RGB32);
        huge.fill(QColor(30, 200, 30));
        ZT_TRUE("большая картинка записана",
                huge.save(QString::fromStdString((bombDir / "б.png").string())));
        {
            std::ofstream out(bombDir / "з.md", std::ios::binary);
            out << "![[б.png]]\n";
        }
        const QString note = QString::fromStdString((bombDir / "з.md").string());
        const int savedBudget = zametti::appearance().imageCacheSizeMb;
        // Порог высокий: картинка разжимается, место меряется по ней.
        zametti::appearance().imageCacheSizeMb = 512;
        zametti::applyImageAllocationLimit();
        zametti::NoteEditor shown;
        shown.resize(600, 500);
        shown.show();
        QTest::qWait(20);
        shown.openFile(note);
        QTest::qWait(20);
        const qreal roomForImage =
            shown.document()->findBlockByNumber(0).blockFormat().bottomMargin();
        ZT_TRUE("картинка разжалась и заняла место", roomForImage > 0.0);
        ZT_TRUE("и заняла вес в кэше", shown.imageCacheBytes() > 0);

        // Порог низкий: та же картинка отвергнута, место то же самое.
        // Четверть от восьми мегабайт — два, а картинка весит три.
        zametti::appearance().imageCacheSizeMb = 8;
        zametti::applyImageAllocationLimit();
        zametti::NoteEditor refused;
        refused.resize(600, 500);
        refused.show();
        QTest::qWait(20);
        zametti::NoteView::resetImageDecodeCounters();
        refused.openFile(note);
        QTest::qWait(20);
        const qreal roomForFrame =
            refused.document()->findBlockByNumber(0).blockFormat().bottomMargin();
        // Рамка — не картинка: пропорций оригинала она не повторяет, а занимает
        // ровно столько, сколько нужно надписи. Показывать в ней всё равно
        // нечего, а 1x1000000 растянуло бы её на миллион пикселей.
        ZT_TRUE("рамка меньше самой картинки", roomForFrame < roomForImage);
        ZT_TRUE("но не вырождается: надпись в неё помещается",
                roomForFrame > 2 * refused.document()
                                       ->findBlockByNumber(0)
                                       .blockFormat()
                                       .lineHeight());
        ZT_EQ("веса в кэше она не занимает", std::to_string(0),
              std::to_string(refused.imageCacheBytes()));

        const int decodes = zametti::NoteView::imageDecodes();
        refused.repaint();
        QTest::qWait(0);
        ZT_EQ("отказ не повторяется на каждом кадре", std::to_string(decodes),
              std::to_string(zametti::NoteView::imageDecodes()));

        zametti::appearance().imageCacheSizeMb = savedBudget;
        zametti::applyImageAllocationLimit();
    }

    // Вырожденные пропорции. Предел стороны держит ОБЕ стороны, поэтому худший
    // случай в кэше — квадрат limit x limit, а не лента: 1x1000000 занимает
    // столько же, сколько 1x1024. И место в документе тоже ограничено — без
    // этого замер на 1x20000 давал 19984 px поля под одну строку и документ
    // высотой в двадцать тысяч пикселей.
    {
        const fs::path thinDir = dir / "вырожденные";
        fs::create_directories(thinDir);
        QImage thin(1, 20000, QImage::Format_RGB32);
        thin.fill(QColor(200, 40, 40));
        ZT_TRUE("тонкая картинка записана",
                thin.save(QString::fromStdString((thinDir / "тонкая.png").string())));
        QImage wide(20000, 1, QImage::Format_RGB32);
        wide.fill(QColor(40, 200, 40));
        ZT_TRUE("широкая картинка записана",
                wide.save(QString::fromStdString((thinDir / "широкая.png").string())));
        {
            std::ofstream out(thinDir / "з.md", std::ios::binary);
            out << "![[тонкая.png]]\n\n![[широкая.png]]\n";
        }

        const int savedLimit = zametti::appearance().maxLoadedImageSize;
        zametti::appearance().maxLoadedImageSize = 1024;
        zametti::NoteEditor thinEditor;
        thinEditor.resize(600, 500);
        thinEditor.show();
        QTest::qWait(20);
        thinEditor.openFile(QString::fromStdString((thinDir / "з.md").string()));
        QTest::qWait(20);

        const qreal thinRoom =
            thinEditor.document()->findBlockByNumber(0).blockFormat().bottomMargin();
        ZT_TRUE("под ленту 1x20000 отведено место, а не двадцать тысяч пикселей",
                thinRoom > 0.0 && thinRoom < 4.0 * 500 + 50);
        // Худший случай в кэше — квадрат стороной в предел, и обе вырожденные
        // вместе до него не дотягивают.
        ZT_TRUE("вырожденные картинки в кэше — не лента, а мелочь",
                thinEditor.imageCacheBytes() < qint64(1024) * 1024 * 4);

        zametti::appearance().maxLoadedImageSize = savedLimit;
    }

    // Выравнивание фотографии в колонке. Умолчание — по центру, и в файл ради
    // него не пишется ничего: заметка не обязана хранить незаданное.
    {
        const fs::path alignDir = dir / "выравнивание";
        fs::create_directories(alignDir);
        QImage small(80, 60, QImage::Format_RGB32);
        small.fill(QColor(40, 40, 220));
        ZT_TRUE("картинка выравнивания записана",
                small.save(QString::fromStdString((alignDir / "м.png").string())));
        {
            std::ofstream out(alignDir / "з.md", std::ios::binary);
            out << "![[м.png]]\n\n![подпись](м.png)\n";
        }

        zametti::NoteEditor aligned;
        aligned.resize(700, 500);
        aligned.show();
        QTest::qWait(20);
        aligned.openFile(QString::fromStdString((alignDir / "з.md").string()));
        QTest::qWait(20);
        const auto blockOf = [&](int n) { return aligned.document()->findBlockByNumber(n); };
        const auto photoOf = [&](int n) { return aligned.imageRectInViewport(blockOf(n)); };
        const auto setAlign = [&](int n, zametti::ImageAlign to) {
            QTextCursor cursor(blockOf(n));
            const bool changed = zametti::setImageAlignAtCursor(*aligned.document(), cursor, to);
            QTest::qWait(10);
            return changed;
        };

        // Умолчание: фотография стоит посередине колонки, а не у левого края.
        const QRectF centred = photoOf(0);
        ZT_TRUE("по умолчанию фотография по центру", centred.left() > 40.0);
        ZT_EQ("и в тексте про выравнивание ничего не написано",
              std::string("![[м.png]]"), blockOf(0).text().toStdString());

        ZT_TRUE("выравнивание влево принято", setAlign(0, zametti::ImageAlign::Left));
        const QRectF left = photoOf(0);
        ZT_TRUE("слева фотография уехала левее центра", left.left() < centred.left() - 20.0);
        ZT_EQ("выравнивание записано рядом с шириной",
              std::string("![[м.png|align=left]]"), blockOf(0).text().toStdString());

        ZT_TRUE("выравнивание вправо принято", setAlign(0, zametti::ImageAlign::Right));
        const QRectF right = photoOf(0);
        ZT_TRUE("справа фотография уехала правее центра", right.left() > centred.left() + 20.0);
        ZT_EQ("и записано так же", std::string("![[м.png|align=right]]"),
              blockOf(0).text().toStdString());

        // Возврат к умолчанию стирает запись, а не пишет "align=center".
        ZT_TRUE("возврат к центру принят", setAlign(0, zametti::ImageAlign::Center));
        ZT_EQ("умолчание в файл не пишется", std::string("![[м.png]]"),
              blockOf(0).text().toStdString());
        ZT_TRUE("и фотография вернулась на середину",
                std::fabs(photoOf(0).left() - centred.left()) < 1.5);

        // Ширина и выравнивание уживаются рядом и не сбивают друг друга.
        {
            QTextCursor cursor(blockOf(0));
            ZT_TRUE("ширина записана", zametti::setImageWidthAtCursor(*aligned.document(),
                                                                     cursor, 50));
        }
        QTest::qWait(10);
        setAlign(0, zametti::ImageAlign::Right);
        ZT_EQ("ширина и выравнивание стоят рядом",
              std::string("![[м.png|50|align=right]]"), blockOf(0).text().toStdString());
        {
            QTextCursor cursor(blockOf(0));
            ZT_TRUE("ширина меняется, выравнивание цело",
                    zametti::setImageWidthAtCursor(*aligned.document(), cursor, 60));
        }
        QTest::qWait(10);
        ZT_EQ("обе величины на месте", std::string("![[м.png|60|align=right]]"),
              blockOf(0).text().toStdString());

        // Image-спан: то же самое, но во фрагменте пути.
        setAlign(2, zametti::ImageAlign::Left);
        {
            const zametti::Document ir = zametti::readDocument(*aligned.document());
            const std::string out = zametti::serialize(ir);
            ZT_TRUE("у image-спана выравнивание уходит во фрагмент пути",
                    out.find("![подпись](м.png#align=left)") != std::string::npos);
        }
        {
            QTextCursor cursor(blockOf(2));
            zametti::setImageWidthAtCursor(*aligned.document(), cursor, 70);
        }
        QTest::qWait(10);
        {
            const zametti::Document ir = zametti::readDocument(*aligned.document());
            const std::string out = zametti::serialize(ir);
            ZT_TRUE("ширина встаёт рядом с выравниванием",
                    out.find("![подпись](м.png#w=70&align=left)") != std::string::npos);
        }
    }

    return zt::report("картинки в просмотре");
}
