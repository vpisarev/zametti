// РАЗМЕР ОБОЛОЧКИ: один кегль на всё, и Ctrl+Alt+± его двигает.
//
// Три вопроса, и все три — про то, чего нельзя увидеть глазами на одном
// снимке:
//
//   1. ВСЁ ВЫВОДИТСЯ ИЗ ОДНОГО КЕГЛЯ. Иконка — две высоты заглавной «A»
//      (правило владельца, замер в zametti-bench ui-metrics), поля кнопок — от
//      иконки, полоса сведений — постоянным отношением от панельного кегля.
//      Ни одного числа в пикселях внутри виджетов не осталось.
//   2. СТУПЕНЬ МАСШТАБА ОБОЛОЧКИ ДВИГАЕТ ВСЁ ЭТО РАЗОМ и ровно во столько, во
//      сколько обещает шкала 2^(k/12).
//   3. ЧЕТЫРЕ МАСШТАБА НЕЗАВИСИМЫ (решение владельца): Ctrl+Alt+− уменьшает
//      оболочку и НЕ трогает текст заметки, Ctrl+= увеличивает заметку и НЕ
//      трогает тулбар. Перемножить их где-нибудь по дороге — ровно та беда,
//      которую на снимке не отличишь от «просто крупновато».

#include "editor_widget.h"
#include "settings.h"
#include "status_bar.h"
#include "toolbar.h"
#include "ui_style.h"
#include "zapp.h"
#include "zoom_scale.h"

#include "settings_hook.h"
#include "test_util.h"
#include "testdata.h"

#include <QApplication>
#include <QFontMetricsF>
#include <QDir>
#include <QFile>
#include <QLabel>
#include <QTest>
#include <QToolButton>

#include <cmath>
#include <string>
#include <vector>

namespace {

std::string s(const QString& q) { return q.toStdString(); }

// Ступень масштаба оболочки живёт в state.json; набор правит её напрямую и
// обязан вернуть как было — наборы идут одним процессом.
struct WithInterfaceZoom {
    int had = zametti::ZApp::instance().state().interfaceZoom();
    explicit WithInterfaceZoom(int steps) {
        zametti::ZApp::instance().state().setInterfaceZoom(steps);
    }
    ~WithInterfaceZoom() { zametti::ZApp::instance().state().setInterfaceZoom(had); }
};

// --- 1. ВСЁ ИЗ ОДНОГО КЕГЛЯ --------------------------------------------------
void checkDerivedFromOneSize() {
    const zametti::ZUiStyle& ui = zametti::ZApp::instance().uiStyle();

    ZT_EQ("шрифт оболочки — кегль из настроек",
          std::to_string(zametti::settings().ui().appPoint()),
          std::to_string(ui.appFont().pointSizeF()));

    // ПРАВИЛО ИКОНКИ ЦЕЛИКОМ: две высоты заглавной «A». Меряется тем же
    // способом, что и в самом расчёте, — иначе набор проверял бы не правило, а
    // своё представление о нём.
    const qreal cap = QFontMetricsF(ui.appFont()).capHeight();
    ZT_EQ("иконка — две высоты заглавной A", std::to_string(int(std::lround(2.0 * cap))),
          std::to_string(ui.iconSize()));

    // И то самое число, ради которого правило заведено: на умолчании оболочки
    // (11 pt) оно даёт 21 — ровно столько владелец подобрал руками, когда
    // размер иконки был отдельной настройкой.
    if (std::fabs(zametti::settings().ui().appPoint() - 11.0) < 0.01)
        ZT_EQ("на умолчании иконка выходит 21 точку", std::to_string(21),
              std::to_string(ui.iconSize()));

    ZT_TRUE("полоса сведений мельче панелей, но не втрое",
            ui.statusFont().pointSizeF() < ui.appFont().pointSizeF() &&
                ui.statusFont().pointSizeF() > ui.appFont().pointSizeF() * 0.75);
    ZT_TRUE("поле поиска крупнее панелей",
            ui.findFont().pointSizeF() > ui.appFont().pointSizeF());
    ZT_TRUE("гарнитура у всех троих одна",
            ui.statusFont().family() == ui.appFont().family() &&
                ui.findFont().family() == ui.appFont().family());

    // Ни один выведенный размер не имеет права выродиться в ноль: кнопка
    // нулевой стороны — это отсутствующая кнопка.
    ZT_TRUE("поля и промежутки положительны",
            ui.buttonPadding() > 0 && ui.groupSpacing() > 0 && ui.toolbarMargin() > 0 &&
                ui.statusPadding() > 0 && ui.statusPaddingTop() > 0 &&
                ui.folderIconSize() > 0);
}

// --- 2. СТУПЕНЬ ДВИГАЕТ ВСЁ РАЗОМ -------------------------------------------
void checkInterfaceZoomScalesEverything() {
    const int iconAtZero = zametti::ZApp::instance().uiStyle().iconSize();
    const qreal pointsAtZero = zametti::ZApp::instance().uiStyle().appFont().pointSizeF();

    {
        WithInterfaceZoom twice(12);   // ровно вдвое
        const zametti::ZUiStyle& ui = zametti::ZApp::instance().uiStyle();
        ZT_TRUE("двенадцать ступеней удвоили кегль: " +
                    std::to_string(ui.appFont().pointSizeF()),
                std::fabs(ui.appFont().pointSizeF() - pointsAtZero * 2.0) < 0.01);
        // Иконка считается по метрикам шрифта, а их квантует хинтинг: точного
        // удвоения требовать нельзя, допуск — две точки.
        ZT_TRUE("и иконку тоже: " + std::to_string(iconAtZero) + " → " +
                    std::to_string(ui.iconSize()),
                std::fabs(ui.iconSize() - 2.0 * iconAtZero) <= 2.0);
        ZT_TRUE("поле кнопки поехало вместе с иконкой",
                ui.buttonPadding() > 0 && ui.buttonPadding() >= iconAtZero / 8);
    }
    {
        WithInterfaceZoom half(-12);
        const zametti::ZUiStyle& ui = zametti::ZApp::instance().uiStyle();
        ZT_TRUE("минус двенадцать — половина кегля",
                std::fabs(ui.appFont().pointSizeF() - pointsAtZero * 0.5) < 0.01);
        ZT_TRUE("иконка не выродилась", ui.iconSize() > 0);
    }
    // Вернулись: расчёт обязан отдать ровно то, с чего начали, — иначе ступени
    // копили бы разницу, ради чего шкала и заводилась.
    ZT_EQ("вернулись к нулевой ступени — иконка та же", std::to_string(iconAtZero),
          std::to_string(zametti::ZApp::instance().uiStyle().iconSize()));
}

// Виджеты обязаны БРАТЬ эти числа, а не считать свои. Спрашивается действием:
// меняем ступень, зовём тот же refreshAppearance, что зовёт окно, и смотрим на
// живую геометрию кнопки и на шрифт полосы сведений.
void checkWidgetsFollow() {
    zametti::Toolbar bar;
    zametti::StatusBar status;
    bar.refreshAppearance();
    status.refreshAppearance();

    const auto anyButton = [&]() -> QToolButton* {
        return bar.findChild<QToolButton*>();
    };
    QToolButton* button = anyButton();
    ZT_TRUE("кнопка тулбара нашлась", button != nullptr);
    if (button == nullptr) return;
    const int sideAtZero = button->width();
    QLabel* label = status.findChild<QLabel*>();
    ZT_TRUE("надпись полосы сведений нашлась", label != nullptr);
    const qreal statusAtZero = label != nullptr ? label->font().pointSizeF() : 0.0;

    WithInterfaceZoom twice(12);
    bar.refreshAppearance();
    status.refreshAppearance();
    ZT_TRUE("кнопка тулбара выросла: " + std::to_string(sideAtZero) + " → " +
                std::to_string(anyButton()->width()),
            anyButton()->width() > sideAtZero * 1.7);
    if (label != nullptr)
        ZT_TRUE("и кегль полосы сведений тоже: " + std::to_string(statusAtZero) + " → " +
                    std::to_string(label->font().pointSizeF()),
                label->font().pointSizeF() > statusAtZero * 1.9);
}

// --- 3. НЕЗАВИСИМОСТЬ --------------------------------------------------------
//
// Матрица из двух клеток, которых на снимке не увидишь: масштаб оболочки не
// входит в кегль заметки, масштаб заметки не входит в размеры оболочки.
void checkScalesDoNotMultiply(const QString& path) {
    zametti::NoteEditor editor;
    editor.resize(800, 600);
    editor.show();
    QTest::qWait(20);
    editor.openFile(path);
    QTest::qWait(20);

    const qreal noteAtZero = editor.document()->defaultFont().pointSizeF();
    const int iconAtZero = zametti::ZApp::instance().uiStyle().iconSize();

    {
        // Оболочка выросла вдвое — заметка не шелохнулась.
        //
        // ДЕЛАЕТСЯ РОВНО ТО, ЧТО ДЕЛАЕТ ОКНО: ступень плюс общий шрифт
        // приложения (QApplication::setFont в applyAppearance). Первая
        // редакция этой клетки ставила только ступень — и была пустышкой:
        // течь шла как раз через общий шрифт. QTextEdit переносит шрифт
        // виджета в документ на FontChange, и кегль оболочки затирал кегль
        // заметки — на снимке живого окна текст рос вместе с тулбаром.
        WithInterfaceZoom twice(12);
        const QFont had = QApplication::font();
        QApplication::setFont(zametti::ZApp::instance().uiStyle().appFont());
        QTest::qWait(20);
        ZT_TRUE("оболочка выросла",
                zametti::ZApp::instance().uiStyle().iconSize() > iconAtZero);
        ZT_TRUE("кегль заметки от масштаба оболочки не зависит: " +
                    std::to_string(noteAtZero) + " → " +
                    std::to_string(editor.document()->defaultFont().pointSizeF()),
                std::fabs(editor.document()->defaultFont().pointSizeF() - noteAtZero) < 0.01);
        QApplication::setFont(had);
        QTest::qWait(20);
    }

    // Заметка выросла вдвое — оболочка не шелохнулась.
    editor.applyZoom(zametti::zoomScale(12));
    QTest::qWait(20);
    ZT_TRUE("заметка выросла",
            editor.document()->defaultFont().pointSizeF() > noteAtZero * 1.9);
    ZT_EQ("а размеры оболочки от масштаба заметки не зависят",
          std::to_string(iconAtZero),
          std::to_string(zametti::ZApp::instance().uiStyle().iconSize()));
    editor.applyZoom(1.0);
}

}  // namespace

static int ztRunSuite(int argc, char** argv) {
    (void)argc;
    (void)argv;
    const QString dir = zt::TestData::outDir(QStringLiteral("ui-style"));
    const QString path = QDir(dir).filePath(QStringLiteral("заметка.md"));
    {
        QFile file(path);
        ZT_TRUE("заметка записана", file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write("# Заголовок\n\nАбзац, чтобы было что мерить.\n");
    }

    checkDerivedFromOneSize();
    checkInterfaceZoomScalesEverything();
    checkWidgetsFollow();
    checkScalesDoNotMultiply(path);
    return zt::report("ui-style");
}

TEST(UiStyle, All) {
    std::vector<QByteArray> ztArgs{QByteArrayLiteral("ui_style_test")};
    std::vector<char*> ztArgv;
    for (QByteArray& a : ztArgs) ztArgv.push_back(a.data());
    EXPECT_EQ(0, ztRunSuite(int(ztArgv.size()), ztArgv.data()));
}
