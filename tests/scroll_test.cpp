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
#include "settings.h"

#include "test_util.h"
#include "testdata.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QScrollBar>
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
    zametti::loadAppearance(nullptr);

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

    const bool savedSmooth = zametti::appearance().smoothScroll;

    // --- с инерцией: после отрыва пальцев текст ещё едет ------------------
    zametti::appearance().smoothScroll = true;
    editor.verticalScrollBar()->setValue(0);
    QTest::qWait(20);

    swipe(editor);
    const int atRelease = editor.verticalScrollBar()->value();
    QTest::qWait(zametti::appearance().smoothScrollMs * 6);
    const int settled = editor.verticalScrollBar()->value();

    ZT_TRUE("пальцы увели текст: " + std::to_string(atRelease), atRelease > 0);
    ZT_TRUE("после отрыва текст ПРОДОЛЖИЛ ехать (" + std::to_string(atRelease) + " → " +
                std::to_string(settled) + ")",
            settled > atRelease);

    // --- без инерции: встал там, где отпустили ---------------------------
    zametti::appearance().smoothScroll = false;
    editor.verticalScrollBar()->setValue(0);
    QTest::qWait(20);

    swipe(editor);
    const int stopped = editor.verticalScrollBar()->value();
    QTest::qWait(zametti::appearance().smoothScrollMs * 6);
    const int later = editor.verticalScrollBar()->value();

    ZT_TRUE("без инерции пальцы тоже увели текст: " + std::to_string(stopped), stopped > 0);
    ZT_TRUE("и он встал там же, где отпустили", later == stopped);

    zametti::appearance().smoothScroll = savedSmooth;
    return zt::report("scroll");
}

TEST(Scroll, All) {
    std::vector<QByteArray> ztArgs{QByteArrayLiteral("scroll_test")};
    std::vector<char*> ztArgv;
    for (QByteArray& a : ztArgs) ztArgv.push_back(a.data());
    EXPECT_EQ(0, ztRunSuite(int(ztArgv.size()), ztArgv.data()));
}
