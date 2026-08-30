// Состояние сеанса: круг «сохранили — прочитали».
//
// Настройки принадлежат человеку и только читаются, а вот state.json программа
// пишет сама на каждом выходе. Поле, которое забыли положить в запись или
// вычитать при чтении, ведёт себя ХУЖЕ отсутствующего: настройка вроде бы есть,
// работает до перезапуска и молча забывается. Поймать это глазами нельзя —
// нужно закрыть и открыть программу и заметить, что стало не так.
//
// Поэтому проверка сравнивает поле за полем, а не «файл непустой».

#include "app_state.h"
#include "settings.h"
#include "zstorage_manager.h"

#include "test_util.h"

#include <vector>
#include "testdata.h"
#include "scratch_files.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QTemporaryDir>

#include <string>

namespace {

std::string s(const QString& q) { return q.toStdString(); }
std::string b(bool v) { return v ? "да" : "нет"; }


// ЛЕНИВАЯ МИГРАЦИЯ МАСШТАБА (refactor3): прежде исходник и правка настроек
// держали по своему числу (markdownZoom, settingsZoom) и открывались разного
// размера. Ключ теперь один — plainZoom; старый читается, пока не переписан.
void checkPlainZoomMigrates() {
    // Пишем СТАРОЕ состояние на штатное место (каталог настроек уже подменён
    // обвязкой набора) и читаем штатной загрузкой: проверяется путь, которым
    // ходит программа, а не отдельная функция.
    const QString path = zametti::ZAppState::path();
    QDir().mkpath(QFileInfo(path).absolutePath());
    {
        QFile file(path);
        ZT_TRUE("старое состояние записано", file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write("{ \"markdownZoom\": 1.5, \"settingsZoom\": 1.0 }\n");
    }
    const zametti::ZAppState old = zametti::ZAppState::load();
    ZT_TRUE("масштаб исходника стал общим для обоих плоских видов",
            qFuzzyCompare(old.plainZoom(), 1.5));
}

}  // namespace

static int ztRunSuite(int argc, char** argv) {
    (void)argc;
    (void)argv;
    // Каталог конфига подменяется ДО QCoreApplication: configDir() смотрит
    // переменную при каждом обращении, а писать в настоящий конфиг владельца
    // нельзя ни одной проверкой. Свой ключ, а не XDG_CONFIG_HOME: тот под
    // Windows не значит ничего (см. settings.h у configDir).
    QTemporaryDir home;
    qputenv(zametti::kConfigDirVar, home.path().toLocal8Bit());

    QCoreApplication::setApplicationName(QStringLiteral("zametti"));

    zametti::ZAppState out;
    out.setLastFile(QStringLiteral("/store/00000000000042.md"));
    out.setStoreRoot(QStringLiteral("/store"));
    out.setTreeSort(QStringLiteral("name"));
    out.setSplitterState(QByteArray("сплиттер", 16));
    out.setExpandedDirs({QStringLiteral("/store/a"), QStringLiteral("/store/b")});
    out.setSearchHistory({QStringLiteral("айвазовский"), QStringLiteral("cmyk")});
    out.setCaret(4321);
    out.setAnchor(4300);
    out.setZoom(1.25);
    out.setWindowGeometry(QByteArray("геометрия", 18));
    out.setPanelsHidden(true);
    out.setMarkdownMode(true);
    out.setPlainZoom(1.75);   // масштаб плоских видов — одно число на оба
    out.setHistoryListWidth(233);
    out.setExportDir(QStringLiteral("/tmp/куда-вывозили"));
    out.setExportKeepMeta(true);
    // Каретки по заметкам — там же, по id, без дублей.
    out.rememberCaret(QStringLiteral("00000000000042"), {10, 5, 3});
    out.rememberCaret(QStringLiteral("00000000000007"), {1, 1, 0});
    out.rememberCaret(QStringLiteral("00000000000042"), {20, 20, 7});   // та же — заменяет
    // Список хранилищ устройства ведёт МЕНЕДЖЕР — единственная дорога к нему;
    // массив в ZAppState не живёт вовсе, секция существует один миг записи.
    zametti::ZStorageManager stores;
    {
        zametti::ZStorage::Config first;
        first.root = QStringLiteral("/дом/заметки");
        first.name = QStringLiteral("Заметки");
        first.cloudUrl = QStringLiteral("https://host/dav/");
        first.cloudUser = QStringLiteral("вадим");
        zametti::ZStorage::Config second;
        second.root = QStringLiteral("/дом/работа");
        stores.remember(first);
        stores.remember(second);
        // Та же папка с хвостовым слэшем — та же строка (канонизация), адрес
        // обновляется на месте, а пустое имя прежнего не затирает.
        zametti::ZStorage::Config again;
        again.root = QStringLiteral("/дом/заметки/");
        again.cloudUrl = QStringLiteral("https://host2/dav/");
        stores.remember(again);
    }
    out.save(stores);

    const QString path = zametti::configDir() + QStringLiteral("/state.json");
    ZT_TRUE("state.json написан рядом с конфигом: " + s(path), QFile::exists(path));

    zametti::ZStorageManager returned;
    const zametti::ZAppState back = zametti::ZAppState::load(&returned);
    ZT_EQ("последний файл", s(out.lastFile()), s(back.lastFile()));
    ZT_EQ("хранилище", s(out.storeRoot()), s(back.storeRoot()));
    ZT_EQ("сортировка дерева", s(out.treeSort()), s(back.treeSort()));
    ZT_EQ("состояние сплиттера", out.splitterState().toBase64().toStdString(),
          back.splitterState().toBase64().toStdString());
    ZT_EQ("раскрытые ветки", s(out.expandedDirs().join(QLatin1Char('|'))),
          s(back.expandedDirs().join(QLatin1Char('|'))));
    ZT_EQ("история поиска", s(out.searchHistory().join(QLatin1Char('|'))),
          s(back.searchHistory().join(QLatin1Char('|'))));
    ZT_EQ("каретка", std::to_string(out.caret()), std::to_string(back.caret()));
    ZT_EQ("якорь выделения", std::to_string(out.anchor()), std::to_string(back.anchor()));

    ZT_EQ("зум", std::to_string(out.zoom()), std::to_string(back.zoom()));
    ZT_EQ("геометрия окна", out.windowGeometry().toBase64().toStdString(),
          back.windowGeometry().toBase64().toStdString());
    ZT_EQ("панели убраны", b(out.panelsHidden()), b(back.panelsHidden()));
    ZT_EQ("режим исходника", b(out.markdownMode()), b(back.markdownMode()));
    ZT_TRUE("масштаб исходника — свой и переживает запись",
            qFuzzyCompare(out.plainZoom(), back.plainZoom()));
    // Каталог вывоза переживает перезапуск: начинать каждый раз с «Документов»
    // — значит каждый раз идти по дереву каталогов заново (замечание владельца).
    ZT_EQ("каталог вывоза", s(out.exportDir()), s(back.exportDir()));
    // Галочка вывоза «как есть» — тоже привычка человека: кто обменивается
    // заметками с другим хранилищем, делает это постоянно.
    ZT_EQ("галочка вывоза", b(out.exportKeepMeta()), b(back.exportKeepMeta()));
    // Ширина списка записей режима истории — привычка человека, переживает
    // перезапуск (просьба владельца: список крал место у разности).
    ZT_EQ("ширина списка истории", std::to_string(out.historyListWidth()),
          std::to_string(back.historyListWidth()));
    ZT_EQ("каретки по заметкам: две записи, без дублей", std::string("2"),
          std::to_string(back.carets().size()));
    ZT_TRUE("повтор заменил запись, а не добавил",
            back.caretOf(QStringLiteral("00000000000042")).cursor == 20 &&
                back.caretOf(QStringLiteral("00000000000042")).scroll == 7);
    ZT_TRUE("свежая — впереди", back.carets().first().noteId == QStringLiteral("00000000000042"));
    ZT_TRUE("неизвестная заметка — начало документа",
            !back.knowsCaret(QStringLiteral("нет-такой")) &&
                back.caretOf(QStringLiteral("нет-такой")).cursor == 0 &&
                back.caretOf(QStringLiteral("нет-такой")).anchor == 0);
    // Список хранилищ: секция вернулась из файла ПРЯМО в менеджер — две
    // строки, порядок стабилен, повтор обновил на месте.
    ZT_EQ("хранилищ две строки, без дублей", std::string("2"),
          std::to_string(returned.stores().size()));
    ZT_EQ("порядок стабилен: первая — первой", std::string("/дом/заметки"),
          s(returned.stores().first().root));
    ZT_EQ("адрес обновился на месте", std::string("https://host2/dav/"),
          s(returned.stores().first().cloudUrl));
    ZT_EQ("пустое имя не затёрло прежнего", std::string("Заметки"),
          s(returned.stores().first().name));
    ZT_TRUE("строка ищется по корню с любым хвостом",
            returned.storeFor(QStringLiteral("/дом/работа/")).root ==
                QStringLiteral("/дом/работа"));
    {
        zametti::ZStorageManager edit;
        zametti::ZAppState::load(&edit);
        edit.forget(QStringLiteral("/дом/работа/"));
        ZT_EQ("«−» забыл ровно одну строку", std::string("1"),
              std::to_string(edit.stores().size()));
        ZT_TRUE("осталась другая",
                edit.stores().first().root == QStringLiteral("/дом/заметки"));
    }

    // Умолчание важно не меньше: у человека, который запускает программу
    // впервые, файла нет вовсе, и панели обязаны быть на месте.
    zt::dropFile(QFileInfo(path).absolutePath(), path);
    zametti::ZStorageManager freshStores;
    const zametti::ZAppState fresh = zametti::ZAppState::load(&freshStores);
    ZT_EQ("без файла панели на месте", b(false), b(fresh.panelsHidden()));
    ZT_EQ("без файла режим исходника выключен", b(false), b(fresh.markdownMode()));
    ZT_EQ("без файла зум единичный", std::to_string(1.0), std::to_string(fresh.zoom()));
    ZT_EQ("без файла вывоз чистый", b(false), b(fresh.exportKeepMeta()));
    ZT_EQ("без файла ширина списка истории не задана", std::string("0"),
          std::to_string(fresh.historyListWidth()));
    ZT_EQ("без файла список хранилищ пуст", std::string("0"),
          std::to_string(freshStores.size()));

    checkPlainZoomMigrates();


    return zt::report("session");
}

// Набор целиком одним TEST: тело не тронуто, argv ему собран здесь.
// Дробить на отдельные проверки — отдельная работа, по одному набору.
TEST(Session, All) {
    std::vector<QByteArray> ztArgs{QByteArrayLiteral("session_test")};
    std::vector<char*> ztArgv;
    for (QByteArray& a : ztArgs) ztArgv.push_back(a.data());
    EXPECT_EQ(0, ztRunSuite(int(ztArgv.size()), ztArgv.data()));
}
