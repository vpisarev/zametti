// ИНЕРЦИОННАЯ ПРОКРУТКА ТАЧПАДОМ (scroll.smooth).
//
// Просьба владельца: в браузере и в Sublime текст после отрыва пальцев ещё
// летит и затухает, а у нас вставал колом. Колеса это не касается — там
// владельца всё устраивает.
//
// Проверяется ровно то отличие, ради которого всё делалось: после отрыва
// пальцев текст ПРОДОЛЖАЕТ ехать. И обратная сторона: с выключенной настройкой
// он встаёт там же, где отпустили. Ключ заведён затем, чтобы его можно было
// выключить, и проверка держит именно это обещание.
//
// Тачпад приходит не касанием, а тем же QWheelEvent — точными пикселями и
// фазами; на этом набор и стоит.

#include "editor_widget.h"
#include "doc_model.h"
#include "formula.h"
#include "object_frame.h"
#include "settings.h"
#include "settings_hook.h"

#include "test_util.h"
#include "testdata.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QPainter>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTest>
#include <QWheelEvent>

#include <string>
#include <vector>

namespace {

QString writeNote(const QString& dir, const QString& name, const QString& text) {
    QDir().mkpath(dir);
    const QString path = dir + QLatin1Char('/') + name;
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return {};
    file.write(text.toUtf8());
    file.close();
    return path;
}

// ПРОВЕДЕНИЕ ПАЛЬЦАМИ ПО ТАЧПАДУ, как его присылает Wayland: точные пиксели
// (pixelDelta) и фазы — начало, движение, конец. Колесо приходит иначе
// (рывками по «щелчку»), и инерции ему не нужно: она у него своя.
void sendScroll(zametti::NoteEditor& editor, Qt::ScrollPhase phase, int pixels) {
    QWheelEvent event(QPointF(10, 10), editor.mapToGlobal(QPoint(10, 10)), QPoint(0, pixels),
                      QPoint(0, pixels), Qt::NoButton, Qt::NoModifier, phase, false);
    QApplication::sendEvent(editor.viewport(), &event);
}

// Бросок: пальцы ведут вниз и отрываются на ходу.
void swipe(zametti::NoteEditor& editor) {
    sendScroll(editor, Qt::ScrollBegin, 0);
    for (int i = 0; i < 6; ++i) {
        QTest::qWait(10);   // между событиями обязана пройти жизнь: по ней и скорость
        sendScroll(editor, Qt::ScrollUpdate, -40);
    }
    sendScroll(editor, Qt::ScrollEnd, 0);
}

}  // namespace

static int ztRunSuite(int argc, char** argv) {
    (void)argc;
    (void)argv;
    zametti::loadSettings(nullptr);

    const QString dir = zt::TestData::outDir(QStringLiteral("scroll"));
    QString big = QStringLiteral("# Длинная\n\n");
    for (int i = 0; i < 300; ++i)
        big += QStringLiteral("Абзац номер %1, в нём достаточно слов.\n\n").arg(i);
    const QString path = writeNote(dir, QStringLiteral("длинная.md"), big);

    zametti::NoteEditor editor;
    editor.resize(800, 600);
    editor.show();
    QTest::qWait(20);
    editor.openFile(path);
    QTest::qWait(30);
    ZT_TRUE("заметка длиннее окна: есть что прокручивать",
            editor.verticalScrollBar()->maximum() > 100);

    const bool savedSmooth = zametti::settings().ui().smoothScroll();

    // --- с инерцией: после отрыва пальцев текст ещё едет ------------------
    zametti::mutableSettingsForTests().ui().setSmoothScroll(true);
    editor.verticalScrollBar()->setValue(0);
    QTest::qWait(20);

    swipe(editor);
    const int atRelease = editor.verticalScrollBar()->value();
    QTest::qWait(zametti::settings().ui().smoothScrollMs() * 6);
    const int settled = editor.verticalScrollBar()->value();

    ZT_TRUE("пальцы увели текст: " + std::to_string(atRelease), atRelease > 0);
    ZT_TRUE("после отрыва текст ПРОДОЛЖИЛ ехать (" + std::to_string(atRelease) + " → " +
                std::to_string(settled) + ")",
            settled > atRelease);

    // --- без инерции: встал там, где отпустили ---------------------------
    zametti::mutableSettingsForTests().ui().setSmoothScroll(false);
    editor.verticalScrollBar()->setValue(0);
    QTest::qWait(20);

    swipe(editor);
    const int stopped = editor.verticalScrollBar()->value();
    QTest::qWait(zametti::settings().ui().smoothScrollMs() * 6);
    const int later = editor.verticalScrollBar()->value();

    ZT_TRUE("без инерции пальцы тоже увели текст: " + std::to_string(stopped), stopped > 0);
    ZT_TRUE("и он встал там же, где отпустили", later == stopped);

    // --- ПРОКРУТКА НЕ ДВИГАЕТ КАРЕТКУ ------------------------------------
    //
    // Жалоба владельца: прокрутка вниз уводит курсор вниз, он уезжает за окно, и
    // прокрутка перестаёт работать. Вверх при этом всё хорошо — значит дело не
    // в самой прокрутке, а в чём-то, что срабатывает только при движении вниз.
    zametti::mutableSettingsForTests().ui().setSmoothScroll(true);
    editor.verticalScrollBar()->setValue(0);
    QTest::qWait(20);
    {
        QTextCursor at(editor.document());
        at.setPosition(editor.document()->firstBlock().position());
        editor.setTextCursor(at);
    }
    const int caretBefore = editor.textCursor().position();

    for (int i = 0; i < 4; ++i) {
        swipe(editor);
        QTest::qWait(zametti::settings().ui().smoothScrollMs() * 4);
    }
    const int caretAfter = editor.textCursor().position();
    const int scrolledTo = editor.verticalScrollBar()->value();

    ZT_TRUE("прокрутка вниз уехала: " + std::to_string(scrolledTo), scrolledTo > 0);
    ZT_TRUE("каретка от прокрутки НЕ двигается: было " + std::to_string(caretBefore) +
                ", стало " + std::to_string(caretAfter),
            caretAfter == caretBefore);

    // --- РАМКА ВЫБРАННОГО ОБЪЕКТА ПЕРЕЖИВАЕТ ЧАСТИЧНУЮ ПЕРЕРИСОВКУ ----------
    //
    // Дефект владельца: при прокрутке рамка вокруг формулы частично затиралась.
    // Qt перерисовывает только открывшуюся полосу, а уголки рамки нарисованы ЗА
    // прямоугольником блока — без запаса отсечения (ObjectFrame::sweep) полоса,
    // попавшая на вынос, закрашивалась текстом без рамки. Воспроизводим тем же
    // механизмом, что и прокрутка: render с узкой областью-исходником идёт
    // через paintEvent с этим клипом.
    {
        // Рамка рисуется вокруг ВЁРСТКИ — без движка формул проверять нечего.
        QString engineError;
        ZT_TRUE("движок формул поднялся: " + engineError.toStdString(),
                zametti::Formulas::init(&engineError));
        const QString mathPath =
            writeNote(dir, QStringLiteral("формула.md"),
                      QStringLiteral("Абзац до формулы, довольно обычный.\n\n"
                                     "$$\\frac{a}{b} + \\sqrt{x + 1}$$\n\n"
                                     "Абзац после формулы.\n"));
        editor.openFile(mathPath);
        QTest::qWait(30);
        int number = -1;
        for (QTextBlock b = editor.document()->begin(); b.isValid(); b = b.next())
            if (!zametti::isRawBlock(b) && zametti::kindOf(b) == zametti::Kind::Math) {
                number = b.blockNumber();
                break;
            }
        ZT_TRUE("формула в документе есть", number >= 0);
        // Каретка на блок формулы: объект выбран, рамка рисуется.
        QTextCursor at(editor.document());
        at.setPosition(editor.document()->findBlockByNumber(number).position());
        editor.setTextCursor(at);
        QTest::qWait(30);

        QImage whole(editor.viewport()->size(), QImage::Format_RGB32);
        whole.fill(Qt::white);
        editor.viewport()->render(&whole);

        const QRectF box = editor.formulaRect(number);
        ZT_TRUE("вёрстка формулы посчитана", !box.isEmpty());
        // Полоса над вёрсткой — ровно там, где живёт верхняя перекладина рамки.
        const int sweep = int(zametti::ObjectFrame::sweep()) + 2;
        const QRect strip(0, int(box.top()) - editor.verticalScrollBar()->value() - sweep,
                          editor.viewport()->width(), sweep);
        QImage partial = whole;
        {
            QPainter eraser(&partial);
            eraser.fillRect(strip, Qt::white);
        }
        editor.viewport()->render(&partial, strip.topLeft(), QRegion(strip));

        int mismatched = 0;
        for (int y = qMax(0, strip.top()); y <= qMin(whole.height() - 1, strip.bottom()); ++y)
            for (int x = strip.left(); x <= qMin(whole.width() - 1, strip.right()); ++x)
                if (whole.pixel(x, y) != partial.pixel(x, y)) ++mismatched;
        ZT_EQ("узкая полоса перерисована один в один с целым кадром (рамка цела)", "0",
              std::to_string(mismatched));
    }

    zametti::mutableSettingsForTests().ui().setSmoothScroll(savedSmooth);
    return zt::report("scroll");
}

TEST(Scroll, All) {
    std::vector<QByteArray> ztArgs{QByteArrayLiteral("scroll_test")};
    std::vector<char*> ztArgv;
    for (QByteArray& a : ztArgs) ztArgv.push_back(a.data());
    EXPECT_EQ(0, ztRunSuite(int(ztArgv.size()), ztArgv.data()));
}
