// Каретка и набор — по пикселям, и на ретине тоже.
//
// Жалоба владельца (03.09.2026, мак, dpr 2): «очень криво рисующийся курсор,
// когда я просто печатаю текст; на linux такого не замечаю». Оффскрин-наборы
// отрисовку каретки прежде не видели вовсе: QWidget::grab рисует виджет ЗАНОВО
// целиком, а человек видит ЗАДНИЙ БУФЕР ОКНА — итог частичных перерисовок
// (мигание обновляет только прямоугольник каретки, набор — область правки).
// Здесь сверяются оба:
//
//   1) задний буфер окна (QScreen::grabWindow — у оффскрин-платформы это и
//      есть буфер, а не свежая отрисовка) против отрисовки целиком после
//      каждого нажатия: расхождение вне полосы каретки — след, огрызок, не
//      стёртый хвост;
//   2) каретка добавляет к кадру РОВНО свою полосу: кадры с кареткой в двух
//      местах одной строки расходятся только в двух полосах — колонка
//      перерисовки под штатной кареткой (repaintOverNativeCaret) не вправе
//      менять ни буквы, ни подложку рядом.
//
// Масштаб 2.0 — дочерним процессом: масштаб читается при создании
// QApplication (см. caret_scale_test.cpp).

#include "caret_blink.h"
#include "editor_widget.h"
#include "settings.h"

#include "test_util.h"

#include <QApplication>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QImage>
#include <QProcess>
#include <QProcessEnvironment>
#include <QScreen>
#include <QTest>
#include <QTextBlock>
#include <QTextCursor>
#include <QWindow>

#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace {

constexpr const char* kChildFlag = "ZAMETTI_CARET_PAINT_CHILD";

// Зона каретки в пикселях кадра: полоса с запасом по бокам и по высоте.
QRect caretZone(zametti::NoteEditor& editor) {
    const QRect at = editor.cursorRect();
    const QPoint origin = editor.viewport()->mapTo(editor.window(), at.topLeft());
    const qreal dpr = editor.devicePixelRatioF();
    const int width = zametti::caretPixelWidth(zametti::settings().style().caretWidth(), 1.0);
    return QRect(int((origin.x() - 4) * dpr), int((origin.y() - 3) * dpr),
                 int((width + 9) * dpr), int((at.height() + 6) * dpr));
}

QImage frameOf(const QPixmap& pixmap) {
    QImage image = pixmap.toImage().convertToFormat(QImage::Format_RGB32);
    image.setDevicePixelRatio(1.0);
    return image;
}

// Задний буфер окна — то, что на экране.
QImage backingStore(zametti::NoteEditor& editor) {
    QScreen* screen = editor.window()->windowHandle() != nullptr
                          ? editor.window()->windowHandle()->screen()
                          : QGuiApplication::primaryScreen();
    return frameOf(screen->grabWindow(editor.window()->winId()));
}

// Свежая отрисовка целиком.
QImage freshRender(zametti::NoteEditor& editor) { return frameOf(editor.window()->grab()); }

struct Mismatch {
    int pixels = 0;
    QRect bounds;
};

// Сколько пикселей расходится вне разрешённых зон, и где.
Mismatch compare(const QImage& a, const QImage& b, const QList<QRect>& allowed) {
    Mismatch m;
    if (a.size() != b.size()) {
        m.pixels = -1;
        return m;
    }
    for (int y = 0; y < a.height(); ++y) {
        const QRgb* pa = reinterpret_cast<const QRgb*>(a.constScanLine(y));
        const QRgb* pb = reinterpret_cast<const QRgb*>(b.constScanLine(y));
        for (int x = 0; x < a.width(); ++x) {
            if (pa[x] == pb[x]) continue;
            bool inside = false;
            for (const QRect& zone : allowed)
                if (zone.contains(x, y)) { inside = true; break; }
            if (inside) continue;
            ++m.pixels;
            m.bounds = m.bounds.isNull() ? QRect(x, y, 1, 1) : m.bounds.united(QRect(x, y, 1, 1));
        }
    }
    return m;
}

std::string describe(const Mismatch& m) {
    return std::to_string(m.pixels) + " пикс. в (" + std::to_string(m.bounds.x()) + "," +
           std::to_string(m.bounds.y()) + " " + std::to_string(m.bounds.width()) + "x" +
           std::to_string(m.bounds.height()) + ")";
}

// Задний буфер после нажатия обязан совпасть со свежей отрисовкой — вне полосы
// каретки (фаза мигания вправе отличаться).
int g_dumped = 0;

// Кадры на диск — глазами (ZAMETTI_CARET_PAINT_DUMP=<каталог>).
void dumpFrames(const QImage& store, const QImage& fresh) {
    const QByteArray dir = qgetenv("ZAMETTI_CARET_PAINT_DUMP");
    if (dir.isEmpty()) return;
    const QString base = QString::fromLocal8Bit(dir) + QStringLiteral("/frame-%1-").arg(++g_dumped);
    store.save(base + QStringLiteral("store.png"));
    fresh.save(base + QStringLiteral("fresh.png"));
}

void checkNoTrails(zametti::NoteEditor& editor, const std::string& name) {
    const QImage store = backingStore(editor);
    QImage fresh = freshRender(editor);
    dumpFrames(store, fresh);
    // На масштабе 2 оффскрин-платформа держит задний буфер в «родных»
    // пикселях окна, то есть логического размера, и рисует в него в 2×:
    // виден левый верхний угол. Сверяем то, что видно, — каретка стоит там же
    // (см. runChecks).
    ZT_TRUE(name + ": задний буфер не пуст (" + std::to_string(store.width()) + "x" +
                std::to_string(store.height()) + ", отрисовка " + std::to_string(fresh.width()) +
                "x" + std::to_string(fresh.height()) + ")",
            !store.isNull() && store.width() <= fresh.width() && store.height() <= fresh.height());
    if (store.size() != fresh.size()) fresh = fresh.copy(0, 0, store.width(), store.height());
    const Mismatch m = compare(store, fresh, {caretZone(editor)});
    ZT_TRUE(name + ": задний буфер совпадает со свежей отрисовкой вне каретки (" + describe(m) + ")",
            m.pixels == 0);
}

void runChecks(zametti::NoteEditor& editor, const std::string& scale) {
    editor.setFocus();
    QTest::qWait(30);
    // Каретка — в начале второго абзаца (левый верхний угол окна: на
    // масштабе 2 задний буфер оффскрина показывает только его).
    QTextCursor cursor(editor.document());
    cursor.setPosition(editor.document()->findBlockByNumber(2).position() + 6);
    editor.setTextCursor(cursor);
    QTest::qWait(40);
    checkNoTrails(editor, scale + ": до набора");

    // Набор по букве, с ожиданием между нажатиями: каждое обновление
    // успевает дойти до заднего буфера, и след, если он есть, остаётся там.
    const char* typed = "abc def ghij";
    int step = 0;
    for (const char* p = typed; *p != '\0'; ++p, ++step) {
        QTest::keyClick(&editor, QString(QLatin1Char(*p)).at(0).unicode() == u' ' ? Qt::Key_Space
                                                                                    : Qt::Key(QChar::toUpper(uint(*p))),
                        Qt::NoModifier, 40);
        checkNoTrails(editor, scale + ": после буквы №" + std::to_string(step + 1));
    }

    // Enter, набор на новой строке, и правка в середине строки — перекладка.
    QTest::keyClick(&editor, Qt::Key_Return, Qt::NoModifier, 40);
    checkNoTrails(editor, scale + ": после Enter");
    for (int i = 0; i < 3; ++i) QTest::keyClick(&editor, Qt::Key_X, Qt::NoModifier, 40);
    checkNoTrails(editor, scale + ": после набора на новой строке");
    QTest::keyClick(&editor, Qt::Key_Up, Qt::NoModifier, 40);
    for (int i = 0; i < 4; ++i) QTest::keyClick(&editor, Qt::Key_Left, Qt::NoModifier, 40);
    checkNoTrails(editor, scale + ": после стрелок");
    QTest::keyClick(&editor, Qt::Key_Y, Qt::NoModifier, 40);
    checkNoTrails(editor, scale + ": после буквы в середине строки");

    // Каретка добавляет к кадру ровно свою полосу.
    const QImage here = freshRender(editor);
    const QRect zoneHere = caretZone(editor);
    for (int i = 0; i < 6; ++i) QTest::keyClick(&editor, Qt::Key_Right, Qt::NoModifier, 20);
    QTest::qWait(40);
    const QImage there = freshRender(editor);
    const QRect zoneThere = caretZone(editor);
    const Mismatch m = compare(here, there, {zoneHere, zoneThere});
    ZT_TRUE(scale + ": кадры с кареткой в двух местах расходятся только полосами каретки (" +
                describe(m) + ")",
            m.pixels == 0);
}

int runSuite(const std::string& scale) {
    zametti::loadSettings(nullptr);
    const fs::path dir = fs::temp_directory_path() / "zametti-caret-paint-test";
    fs::create_directories(dir);
    const fs::path note = dir / "набор.md";
    std::ofstream(note, std::ios::binary)
        << "давайте напишем немного текста и посмотрим на каретку\n\n"
           "второй абзац, чтобы под первым было что перерисовывать\n"
           "и вторая строка того же абзаца\n\n"
           "- пункт списка\n- ещё пункт\n";

    zametti::NoteEditor editor;
    editor.resize(640, 440);
    editor.show();
    QTest::qWait(30);
    zametti::applyPalette(editor);
    editor.openFile(QString::fromStdString(note.string()));
    QTest::qWait(60);
    runChecks(editor, scale);
    return zt::report(("каретка и набор по пикселям, масштаб " + scale).c_str());
}

}  // namespace

TEST(CaretPaint, Scale1) { EXPECT_EQ(0, runSuite("1.0")); }

TEST(CaretPaint, Scale2) {
    if (qEnvironmentVariableIsSet(kChildFlag)) {
        zametti::NoteEditor probe;
        ZT_TRUE("масштаб показа ретины (" + std::to_string(probe.devicePixelRatioF()) + ")",
                probe.devicePixelRatioF() > 1.9);
        EXPECT_EQ(0, runSuite("2.0"));
        return;
    }
    QProcess child;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QString::fromLatin1(kChildFlag), QStringLiteral("1"));
    env.insert(QStringLiteral("QT_SCALE_FACTOR"), QStringLiteral("2"));
    child.setProcessEnvironment(env);
    child.setProcessChannelMode(QProcess::ForwardedChannels);
    child.start(QCoreApplication::applicationFilePath(),
                {QStringLiteral("--gtest_filter=CaretPaint.Scale2")});
    ASSERT_TRUE(child.waitForStarted(10000));
    ASSERT_TRUE(child.waitForFinished(120000));
    EXPECT_EQ(QProcess::NormalExit, child.exitStatus());
    EXPECT_EQ(0, child.exitCode());
}
