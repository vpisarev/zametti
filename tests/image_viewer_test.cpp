// ПОЛНОЭКРАННЫЙ ПРОСМОТР КАРТИНОК (решение владельца): снимок на весь экран,
// стрелки листают снимки ЭТОЙ заметки, снизу подпись, фон свой, мелкая
// картинка увеличивается не больше потолка.
//
// Что спрашивается: список снимков заметки (порядок — как в тексте, подписи —
// показанные, каретка узнаётся), листание по кругу и клавишами, потолок
// увеличения ДЕЙСТВИЕМ (пиксели снимка на фоне), фон из настроек.

#include "doc_model.h"
#include "editor_widget.h"
#include "image_viewer.h"
#include "settings.h"
#include "settings_hook.h"

#include "test_util.h"

#include <QDir>
#include <QFile>
#include <QImage>
#include <QTemporaryDir>
#include <QTest>
#include <QTextBlock>
#include <QTextCursor>

#include <string>
#include <vector>

namespace {

QString g_dir;

// Картинка ровного цвета: её край на тёмном фоне просмотра видно точно.
QString writeImage(const QString& name, int width, int height, QColor colour) {
    QImage picture(width, height, QImage::Format_RGB32);
    picture.fill(colour);
    const QString path = QDir(g_dir).filePath(name);
    return picture.save(path) ? path : QString();
}

QString writeNote(const QString& name, const QString& body) {
    const QString path = QDir(g_dir).filePath(name);
    QFile file(path);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) file.write(body.toUtf8());
    return path;
}

// Ширина полосы пикселей, отличных от фона, по средней строке снимка вида.
int shotWidthOnScreen(zametti::ImageViewer& viewer, const QColor& background) {
    const QImage shot = viewer.grab().toImage();
    const int y = shot.height() / 2;
    int run = 0;
    int best = 0;
    for (int x = 0; x < shot.width(); ++x) {
        if (shot.pixelColor(x, y) != background) {
            ++run;
            best = qMax(best, run);
        } else {
            run = 0;
        }
    }
    return best;
}

void checkShotList() {
    ZT_TRUE("картинки записаны",
            !writeImage(QStringLiteral("один.png"), 80, 60, Qt::red).isEmpty() &&
                !writeImage(QStringLiteral("два.png"), 80, 60, Qt::green).isEmpty() &&
                !writeImage(QStringLiteral("три.png"), 80, 60, Qt::blue).isEmpty());
    const QString note = writeNote(
        QStringLiteral("галерея.md"),
        QStringLiteral("# Галерея\n\n![первая](один.png)\n\nтекст\n\n![](два.png)\n\n"
                       "![третья](три.png)\n"));

    zametti::NoteEditor editor;
    editor.resize(700, 500);
    editor.show();
    QTest::qWait(20);
    editor.openFile(note);
    QTest::qWait(40);

    zametti::NoteEditor::ShotList shots = editor.noteShots();
    ZT_EQ("снимков в заметке три", std::string("3"), std::to_string(shots.paths.size()));
    ZT_TRUE("порядок — как в тексте",
            shots.paths.at(0).endsWith(QStringLiteral("один.png")) &&
                shots.paths.at(1).endsWith(QStringLiteral("два.png")) &&
                shots.paths.at(2).endsWith(QStringLiteral("три.png")));
    ZT_TRUE("пути абсолютные", QDir::isAbsolutePath(shots.paths.at(0)));
    ZT_EQ("подпись первой", std::string("первая"), shots.captions.at(0).toStdString());
    ZT_EQ("у безымянной подписи нет", std::string(), shots.captions.at(1).toStdString());
    ZT_EQ("каретка не на картинке — номера нет", std::string("-1"),
          std::to_string(shots.atCaret));

    // Каретка на второй картинке — просмотр начнётся с неё.
    for (QTextBlock b = editor.document()->begin(); b.isValid(); b = b.next()) {
        if (!zametti::blockImageRef(b).valid) continue;
        if (!zametti::blockImageRef(b).path.endsWith(QStringLiteral("два.png"))) continue;
        QTextCursor at(b);
        editor.setTextCursor(at);
        break;
    }
    shots = editor.noteShots();
    ZT_EQ("каретка на второй — её номер", std::string("1"), std::to_string(shots.atCaret));
}

void checkViewerWalks() {
    const QString note = writeNote(
        QStringLiteral("листание.md"),
        QStringLiteral("![a](один.png)\n\n![b](два.png)\n\n![c](три.png)\n"));
    zametti::NoteEditor editor;
    editor.resize(700, 500);
    editor.show();
    QTest::qWait(20);
    editor.openFile(note);
    QTest::qWait(40);
    const zametti::NoteEditor::ShotList shots = editor.noteShots();

    std::vector<zametti::ImageViewer::Shot> list;
    for (int i = 0; i < shots.paths.size(); ++i)
        list.push_back({shots.paths.at(i), shots.captions.value(i)});

    zametti::ImageViewer viewer;
    viewer.resize(800, 600);
    ZT_TRUE("просмотр открылся с первой", viewer.show(list, 0));
    ZT_EQ("их три", std::string("3"), std::to_string(viewer.count()));
    QTest::qWait(30);

    QTest::keyClick(&viewer, Qt::Key_Right);
    ZT_EQ("стрелка вправо — вторая", std::string("1"), std::to_string(viewer.current()));
    QTest::keyClick(&viewer, Qt::Key_Right);
    QTest::keyClick(&viewer, Qt::Key_Right);
    ZT_EQ("за последней идёт первая — по кругу", std::string("0"),
          std::to_string(viewer.current()));
    QTest::keyClick(&viewer, Qt::Key_Left);
    ZT_EQ("влево с первой — последняя", std::string("2"), std::to_string(viewer.current()));
    QTest::keyClick(&viewer, Qt::Key_Home);
    ZT_EQ("Home — первая", std::string("0"), std::to_string(viewer.current()));
    QTest::keyClick(&viewer, Qt::Key_End);
    ZT_EQ("End — последняя", std::string("2"), std::to_string(viewer.current()));

    int closed = 0;
    QObject::connect(&viewer, &zametti::ImageViewer::closed, &viewer, [&closed] { ++closed; });
    QTest::keyClick(&viewer, Qt::Key_Escape);
    QTest::qWait(20);
    ZT_EQ("Esc закрывает", std::string("1"), std::to_string(closed));
    ZT_TRUE("и окно спрятано", !viewer.isVisible());

    // Пустой список смотреть нечего — и просмотр об этом честно говорит.
    zametti::ImageViewer empty;
    ZT_TRUE("пустой список не показывается", !empty.show({}, 0));
}

// ПОТОЛОК УВЕЛИЧЕНИЯ (imageViewer.maxZoomPercent): мелкая картинка растягивается
// не больше, чем позволено, — растянутый на весь экран значок это каша.
// Спрашивается ДЕЙСТВИЕМ: меряем ширину картинки на снимке вида.
void checkZoomCeiling() {
    const QString small = writeImage(QStringLiteral("мелкая.png"), 100, 50, Qt::yellow);
    ZT_TRUE("мелкая картинка записана", !small.isEmpty());

    zametti::ZSettings& live = zametti::mutableSettingsForTests();
    const int wasCeiling = live.imageViewer().maxZoomPercent();
    const QColor background = live.imageViewer().background();

    zametti::ImageViewer viewer;
    viewer.resize(900, 700);

    live.imageViewer().setMaxZoomPercent(200);
    ZT_TRUE("показан", viewer.show({{small, QString()}}, 0));
    QTest::qWait(50);
    const int atTwo = shotWidthOnScreen(viewer, background);
    ZT_TRUE("вдвое — это около 200 пикселей (" + std::to_string(atTwo) + ")",
            atTwo >= 190 && atTwo <= 210);

    live.imageViewer().setMaxZoomPercent(100);
    viewer.refreshAppearance();
    QTest::qWait(50);
    const int atOne = shotWidthOnScreen(viewer, background);
    ZT_TRUE("со потолком 100 — своя ширина (" + std::to_string(atOne) + ")",
            atOne >= 95 && atOne <= 105);

    live.imageViewer().setMaxZoomPercent(wasCeiling);
    viewer.close();
}

}  // namespace

TEST(ImageViewer, All) {
    QTemporaryDir tmp;
    g_dir = tmp.path();
    checkShotList();
    checkViewerWalks();
    checkZoomCeiling();
}
