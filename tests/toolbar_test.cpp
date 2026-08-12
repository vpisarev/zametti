// Тулбар и рисование иконок.
//
// Проверять глазами тут особенно нечего: кнопка с чужой иконкой, кнопка без
// тултипа и кнопка, погашенная без объяснения, выглядят ровно как исправные.
// Поэтому набор спрашивает у виджета то, что человек спросить не может: из
// какого файла нарисована каждая кнопка, какого цвета вышел растр и в каком
// порядке кнопки встали в раскладке.
//
// Заодно кладёт снимок приёмки.

#include "icons.h"
#include "resources.h"
#include "settings.h"
#include "toolbar.h"

#include "test_util.h"

#include <QApplication>
#include <QDir>
#include <QImage>
#include <QLayout>
#include <QSet>
#include <QToolButton>

#include <string>

using zametti::Toolbar;
using Button = zametti::Toolbar::Button;

namespace {

std::string s(const QString& q) { return q.toStdString(); }

// Каждая кнопка списка описана полностью: иконка есть в ресурсах, подпись не
// пуста, номер группы не отрицателен. Список — единственный источник правды о
// составе тулбара, и дырка в нём иначе всплыла бы только на экране.
void checkSpecsAreComplete() {
    QSet<int> seen;
    QSet<QString> icons;
    for (const Toolbar::Spec& spec : Toolbar::specs()) {
        const QString icon = QString::fromLatin1(spec.icon);
        ZT_TRUE("иконка " + s(icon) + " есть в ресурсах",
                QFile::exists(zametti::iconPath(spec.icon)));
        ZT_TRUE("у кнопки " + s(icon) + " есть подпись",
                spec.tip != nullptr && *spec.tip != '\0');
        ZT_TRUE("кнопка " + s(icon) + " встречается один раз",
                !seen.contains(int(spec.id)));
        seen.insert(int(spec.id));
        icons.insert(icon);
    }
    // Две кнопки с одной иконкой человек не различит. Если такое понадобится —
    // это осознанное решение, и тогда проверка правится вместе с ним.
    ZT_EQ("иконки не повторяются", std::to_string(seen.size()),
          std::to_string(icons.size()));
}

// Виджет построил кнопку на каждую строку списка, и ни одной лишней.
void checkEveryButtonExists(const Toolbar& bar) {
    int made = 0;
    for (const Toolbar::Spec& spec : Toolbar::specs()) {
        QToolButton* button = bar.buttonFor(spec.id);
        ZT_TRUE("кнопка " + std::string(spec.icon) + " построена", button != nullptr);
        if (!button) continue;
        ++made;
        ZT_EQ("переключаемость у " + std::string(spec.icon),
              std::string(spec.checkable ? "да" : "нет"),
              std::string(button->isCheckable() ? "да" : "нет"));
        ZT_TRUE("тултип у " + std::string(spec.icon) + " не пуст",
                !button->toolTip().isEmpty());
    }
    ZT_EQ("кнопок построено ровно по списку", std::to_string(Toolbar::specs().size()),
          std::to_string(made));
}

// Шорткат обязан быть в тултипе: иначе о нём никто не узнает, а он и есть
// причина, по которой к тулбару перестают тянуться мышкой.
void checkShortcutsAreShown(const Toolbar& bar) {
    for (const Toolbar::Spec& spec : Toolbar::specs()) {
        const QString shortcut = QString::fromLatin1(spec.shortcut);
        if (shortcut.isEmpty()) continue;
        QToolButton* button = bar.buttonFor(spec.id);
        if (!button) continue;
        ZT_TRUE("тултип " + std::string(spec.icon) + " называет " + s(shortcut),
                button->toolTip().contains(shortcut));
    }
}

// Два поиска: по заметке и по всему хранилищу. Стоят рядом, каждый называет
// своё сочетание клавиш, и порядок именно такой — сперва поиск по заметке,
// правее поиск по хранилищу (просьба владельца). Порядок спрашивается у
// РАСКЛАДКИ, а не у списка: список задаёт его, но перепутать местами их может
// и раскладка, и увидеть это иначе нечем.
void checkSearchPair(Toolbar& bar) {
    bar.resize(1100, bar.sizeHint().height());
    if (bar.layout() != nullptr) bar.layout()->activate();

    QToolButton* inNote = bar.buttonFor(Button::Search);
    QToolButton* inStore = bar.buttonFor(Button::SearchInStore);
    ZT_TRUE("кнопка поиска по заметке есть", inNote != nullptr);
    ZT_TRUE("кнопка поиска по хранилищу есть", inStore != nullptr);
    if (inNote == nullptr || inStore == nullptr) return;
    ZT_TRUE("поиск по заметке называет Ctrl+F",
            inNote->toolTip().contains(QStringLiteral("Ctrl+F")));
    ZT_TRUE("поиск по хранилищу называет Ctrl+Shift+F",
            inStore->toolTip().contains(QStringLiteral("Ctrl+Shift+F")));
    ZT_TRUE("поиск по заметке стоит левее", inNote->x() < inStore->x());
    // Рядом, а не в разных концах полосы: между ними не должно быть ни
    // промежутка между группами, ни распорки.
    ZT_TRUE("и вплотную к нему",
            inStore->x() - (inNote->x() + inNote->width()) < 4);

    // Кнопка истории вернулась — одна, слева от пары поиска и вплотную к ней.
    // Порядок спрашивается у РАСКЛАДКИ, а не у списка: список его задаёт, но
    // перепутать местами может и раскладка.
    QToolButton* history = bar.buttonFor(Button::History);
    ZT_TRUE("кнопка истории есть", history != nullptr);
    if (history == nullptr) return;
    ZT_TRUE("она про историю заметки",
            history->toolTip().contains(QStringLiteral("История")));
    ZT_TRUE("и стоит левее поиска", history->x() < inNote->x());
    ZT_TRUE("вплотную к нему", inNote->x() - (history->x() + history->width()) < 4);
}

// Обещание гасит кнопку И объясняет причину. Половина этого — хуже, чем ничего:
// молча погашенная кнопка читается как поломка.
void checkPromiseExplainsItself(Toolbar& bar) {
    const QString why = QStringLiteral("появится вместе с синхронизацией");
    bar.setPromise(Button::Cloud, why);
    ZT_TRUE("обещанная кнопка погашена", !bar.isEnabled(Button::Cloud));
    QToolButton* button = bar.buttonFor(Button::Cloud);
    ZT_TRUE("причина написана в тултипе",
            button && button->toolTip().contains(why));

    bar.setPromise(Button::Cloud, QString());
    ZT_TRUE("снятое обещание зажигает кнопку", bar.isEnabled(Button::Cloud));
    ZT_TRUE("и убирает причину из тултипа",
            button && !button->toolTip().contains(why));
}

// Иконка нарисована, непуста и покрашена в тот цвет, который просили. Проверка
// цвета не придирка: штрих у Lucide объявлен currentColor, и без перекраски
// иконки выходят чёрными — на тёмной палитре их не стало бы вовсе.
void checkIconsAreDrawnAndTinted() {
    const QColor want(0xcc, 0x22, 0x44);
    for (const Toolbar::Spec& spec : Toolbar::specs()) {
        const QPixmap pixmap =
            zametti::toolbarIcon(QString::fromLatin1(spec.icon), 24, want, 1.0);
        ZT_TRUE("иконка " + std::string(spec.icon) + " нарисована", !pixmap.isNull());
        if (pixmap.isNull()) continue;

        const QImage image = pixmap.toImage();
        int painted = 0;
        bool foreign = false;
        for (int y = 0; y < image.height(); ++y)
            for (int x = 0; x < image.width(); ++x) {
                const QColor c = image.pixelColor(x, y);
                if (c.alpha() < 200) continue;
                ++painted;
                // Домноженный на альфу цвет по краям штриха слегка отличается,
                // поэтому смотрим только на плотные точки и с допуском.
                if (qAbs(c.red() - want.red()) > 6 || qAbs(c.green() - want.green()) > 6 ||
                    qAbs(c.blue() - want.blue()) > 6)
                    foreign = true;
            }
        ZT_TRUE("у " + std::string(spec.icon) + " есть закрашенные точки", painted > 20);
        ZT_TRUE("у " + std::string(spec.icon) + " нет чужого цвета", !foreign);
    }
}

// Плотность экрана попадает в растр, а не теряется. Иначе на плотном мониторе
// иконка вышла бы вдвое крупнее задуманного — Qt считала бы пиксели точками.
void checkDensityIsHonoured() {
    const QPixmap one = zametti::toolbarIcon(QStringLiteral("search"), 20, Qt::black, 1.0);
    const QPixmap two = zametti::toolbarIcon(QStringLiteral("search"), 20, Qt::black, 2.0);
    ZT_EQ("при dpr 1 растр 20 точек", std::string("20x20"),
          std::to_string(one.width()) + "x" + std::to_string(one.height()));
    ZT_EQ("при dpr 2 растр 40 точек", std::string("40x40"),
          std::to_string(two.width()) + "x" + std::to_string(two.height()));
    ZT_EQ("и logical-размер у обоих одинаков", std::string("20"),
          std::to_string(int(two.deviceIndependentSize().width())));
}

// Кэш отвечает тем же растром, а не рисует заново. Проверяется счётчиком, а не
// временем: время шумит, а счётчик отвечает на вопрос прямо.
void checkCacheHolds() {
    zametti::clearIconCache();
    ZT_EQ("после сброса кэш пуст", std::string("0"),
          std::to_string(zametti::iconCacheSize()));
    zametti::toolbarIcon(QStringLiteral("search"), 20, Qt::black, 1.0);
    ZT_EQ("первый вызов положил растр", std::string("1"),
          std::to_string(zametti::iconCacheSize()));
    zametti::toolbarIcon(QStringLiteral("search"), 20, Qt::black, 1.0);
    ZT_EQ("повтор не добавил второго", std::string("1"),
          std::to_string(zametti::iconCacheSize()));
    zametti::toolbarIcon(QStringLiteral("search"), 20, Qt::red, 1.0);
    ZT_EQ("другой цвет — другой растр", std::string("2"),
          std::to_string(zametti::iconCacheSize()));
}

void writeShots(Toolbar& bar, const QString& dir) {
    bar.resize(1100, bar.sizeHint().height());
    bar.grab().save(QDir(dir).filePath(QStringLiteral("toolbar.png")));
}

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);

    checkSpecsAreComplete();
    checkIconsAreDrawnAndTinted();
    checkDensityIsHonoured();
    checkCacheHolds();

    Toolbar bar;
    checkEveryButtonExists(bar);
    checkShortcutsAreShown(bar);
    checkSearchPair(bar);
    checkPromiseExplainsItself(bar);

    if (argc > 1) writeShots(bar, QString::fromLocal8Bit(argv[1]));

    return zt::report("toolbar");
}
