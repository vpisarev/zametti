// Состояние сеанса: круг «сохранили — прочитали».
//
// Настройки принадлежат человеку и только читаются, а вот state.json программа
// пишет сама на каждом выходе. Поле, которое забыли положить в запись или
// вычитать при чтении, ведёт себя ХУЖЕ отсутствующего: настройка вроде бы есть,
// работает до перезапуска и молча забывается. Поймать это глазами нельзя —
// нужно закрыть и открыть программу и заметить, что стало не так.
//
// Поэтому проверка сравнивает поле за полем, а не «файл непустой».

#include "settings.h"

#include "test_util.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <string>

namespace {

std::string s(const QString& q) { return q.toStdString(); }
std::string b(bool v) { return v ? "да" : "нет"; }

}  // namespace

int main(int argc, char** argv) {
    // Каталог конфига подменяется ДО QCoreApplication: QStandardPaths смотрит
    // на переменную окружения при каждом обращении, но писать в настоящий
    // конфиг владельца нельзя ни одной проверкой.
    QTemporaryDir home;
    qputenv("XDG_CONFIG_HOME", home.path().toLocal8Bit());

    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("zametti"));

    zametti::Session out;
    out.lastFile = QStringLiteral("/store/00000000000042.md");
    out.storeRoot = QStringLiteral("/store");
    out.treeSort = QStringLiteral("name");
    out.splitterState = QByteArray("сплиттер", 16);
    out.expandedDirs = {QStringLiteral("/store/a"), QStringLiteral("/store/b")};
    out.searchHistory = {QStringLiteral("айвазовский"), QStringLiteral("cmyk")};
    out.scrollRatio = 0.375;
    out.zoom = 1.25;
    out.windowGeometry = QByteArray("геометрия", 18);
    out.panelsHidden = true;
    out.exportDir = QStringLiteral("/tmp/куда-вывозили");
    out.diffPlainView = true;
    out.exportKeepMeta = true;
    zametti::saveSession(out);

    const QString path = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) +
                         QStringLiteral("/state.json");
    ZT_TRUE("state.json написан рядом с конфигом: " + s(path), QFile::exists(path));

    const zametti::Session back = zametti::loadSession();
    ZT_EQ("последний файл", s(out.lastFile), s(back.lastFile));
    ZT_EQ("хранилище", s(out.storeRoot), s(back.storeRoot));
    ZT_EQ("сортировка дерева", s(out.treeSort), s(back.treeSort));
    ZT_EQ("состояние сплиттера", out.splitterState.toBase64().toStdString(),
          back.splitterState.toBase64().toStdString());
    ZT_EQ("раскрытые ветки", s(out.expandedDirs.join(QLatin1Char('|'))),
          s(back.expandedDirs.join(QLatin1Char('|'))));
    ZT_EQ("история поиска", s(out.searchHistory.join(QLatin1Char('|'))),
          s(back.searchHistory.join(QLatin1Char('|'))));
    ZT_EQ("прокрутка", std::to_string(out.scrollRatio), std::to_string(back.scrollRatio));
    ZT_EQ("зум", std::to_string(out.zoom), std::to_string(back.zoom));
    ZT_EQ("геометрия окна", out.windowGeometry.toBase64().toStdString(),
          back.windowGeometry.toBase64().toStdString());
    ZT_EQ("панели убраны", b(out.panelsHidden), b(back.panelsHidden));
    // Каталог вывоза переживает перезапуск: начинать каждый раз с «Документов»
    // — значит каждый раз идти по дереву каталогов заново (замечание владельца).
    ZT_EQ("каталог вывоза", s(out.exportDir), s(back.exportDir));
    // Вид разности — тоже привычка человека, а не свойство заметки: кто читает
    // разность как markdown, читает её так всегда (просьба владельца).
    ZT_EQ("вид разности", b(out.diffPlainView), b(back.diffPlainView));
    // Галочка вывоза «как есть» — тоже привычка человека: кто обменивается
    // заметками с другим хранилищем, делает это постоянно.
    ZT_EQ("галочка вывоза", b(out.exportKeepMeta), b(back.exportKeepMeta));

    // Умолчание важно не меньше: у человека, который запускает программу
    // впервые, файла нет вовсе, и панели обязаны быть на месте.
    QFile::remove(path);
    const zametti::Session fresh = zametti::loadSession();
    ZT_EQ("без файла панели на месте", b(false), b(fresh.panelsHidden));
    ZT_EQ("без файла зум единичный", std::to_string(1.0), std::to_string(fresh.zoom));
    ZT_EQ("без файла разность полосками", b(false), b(fresh.diffPlainView));
    ZT_EQ("без файла вывоз чистый", b(false), b(fresh.exportKeepMeta));

    return zt::report("session");
}
