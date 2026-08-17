// Конфиг: срезатель комментариев и сгенерированный шаблон.
//
// Шаблон — это ВЕСЬ список параметров, целиком закомментированный. Две вещи
// обязаны выполняться одновременно, и обе легко сломать по отдельности:
// файл должен разбираться (то есть быть пустым объектом — отклонений нет) и
// должен содержать каждый параметр (то есть служить меню).
//
// Каталог настроек подменяется на временный ДО QCoreApplication: писать в
// настоящий конфиг владельца нельзя ни одной проверкой.

#include "settings.h"

#include "test_util.h"

#include <vector>
#include "testdata.h"

#include <QCoreApplication>
#include <QColor>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <string>

namespace {

std::string s(const QString& q) { return q.toStdString(); }

void checkStrip(const char* what, const QByteArray& source, const QByteArray& expected) {
    ZT_EQ(what, expected.toStdString(), zametti::stripJsonSugar(source).toStdString());
}

void checkStripper() {
    checkStrip("без комментариев ничего не меняется", "{\"a\": 1}", "{\"a\": 1}");
    checkStrip("комментарий в конце строки", "{\n  // всё\n  \"a\": 1\n}",
               "{\n  \n  \"a\": 1\n}");
    checkStrip("комментарий без перевода строки в конце", "{}// хвост", "{}");

    // Главное, ради чего стриппер вообще пишется руками, а не регуляркой:
    // "https://" внутри строки — это не комментарий.
    checkStrip("двойная косая внутри строки цела", "{\"url\": \"https://example.com\"}",
               "{\"url\": \"https://example.com\"}");
    checkStrip("экранированная кавычка не закрывает строку",
               "{\"a\": \"он сказал \\\"//\\\" и всё\"}",
               "{\"a\": \"он сказал \\\"//\\\" и всё\"}");
    // Одиночная косая — не комментарий: она бывает в путях и в цветах.
    checkStrip("одиночная косая цела", "{\"a\": \"/tmp/x\"}", "{\"a\": \"/tmp/x\"}");

    // Висячая запятая перед закрывающей скобкой — вторая и последняя поблажка.
    checkStrip("висячая запятая перед фигурной скобкой", "{\n  \"a\": 1,\n}",
               "{\n  \"a\": 1\n}");
    checkStrip("висячая запятая перед квадратной", "[1, 2, ]", "[1, 2 ]");
    checkStrip("висячая запятая после комментария", "{\n  \"a\": 1, // всё\n}",
               "{\n  \"a\": 1 \n}");
    checkStrip("запятая между полями цела", "{\"a\": 1, \"b\": 2}", "{\"a\": 1, \"b\": 2}");
    checkStrip("запятая внутри строки цела", "{\"a\": \"раз, два\"}", "{\"a\": \"раз, два\"}");
    checkStrip("скобка внутри строки не считается закрывающей",
               "{\"a\": \",}\", \"b\": 2}", "{\"a\": \",}\", \"b\": 2}");
    // Две запятые подряд — это не «висячая», а битый JSON, и он обязан
    // остаться битым: чинить за человека мы не нанимались.
    checkStrip("две запятые подряд не склеиваются", "[1,,]", "[1,]");

    // Номера строк обязаны сойтись с файлом: сообщение об ошибке разбора
    // указывает строку, и если стриппер их съест, оно будет врать.
    const QByteArray source = "// раз\n// два\n{\n  \"a\": 1\n}\n";
    const QByteArray stripped = zametti::stripJsonSugar(source);
    ZT_TRUE("переводы строк не съедены",
            stripped.count('\n') == source.count('\n'));
}

void checkTemplate() {
    QString error;
    ZT_TRUE("шаблон записан: " + s(error), zametti::writeConfigTemplate(&error));

    const QString path = zametti::configPath();
    ZT_TRUE("файл конфига есть: " + s(path), QFile::exists(path));

    QFile file(path);
    ZT_TRUE("файл открывается", file.open(QIODevice::ReadOnly));
    const QByteArray written = file.readAll();
    file.close();

    // 1. Разбирается — и разбирается в ПУСТОЙ объект: отклонений нет.
    QJsonParseError parseError{};
    const QJsonDocument doc =
        QJsonDocument::fromJson(zametti::stripJsonSugar(written), &parseError);
    ZT_TRUE("шаблон разбирается: " + s(parseError.errorString()),
            parseError.error == QJsonParseError::NoError);
    ZT_TRUE("шаблон — объект", doc.isObject());
    ZT_TRUE("отклонений в шаблоне нет", doc.object().isEmpty());

    // 2. При этом в нём есть КАЖДЫЙ параметр: иначе это не меню, а пустышка.
    //    Сверяем по ключам полного списка умолчаний — тому самому, который
    //    печатает --dump-config.
    const QJsonDocument defaults = QJsonDocument::fromJson(zametti::defaultSettingsJson());
    ZT_TRUE("список умолчаний — объект", defaults.isObject());
    int missing = 0;
    const QJsonObject root = defaults.object();
    for (auto section = root.begin(); section != root.end(); ++section) {
        const QByteArray key = QStringLiteral("\"%1\"").arg(section.key()).toUtf8();
        if (!written.contains(key)) {
            ++missing;
            std::printf("  нет в шаблоне: %s\n", section.key().toUtf8().constData());
        }
        if (!section.value().isObject()) continue;
        const QJsonObject inner = section.value().toObject();
        for (auto item = inner.begin(); item != inner.end(); ++item) {
            const QByteArray name = QStringLiteral("\"%1\"").arg(item.key()).toUtf8();
            if (written.contains(name)) continue;
            ++missing;
            std::printf("  нет в шаблоне: %s.%s\n", section.key().toUtf8().constData(),
                        item.key().toUtf8().constData());
        }
    }
    ZT_TRUE("в шаблоне есть каждый параметр", missing == 0);

    // 3. Существующий файл не перезаписывается: там правки человека.
    QFile mine(path);
    ZT_TRUE("подменяем файл", mine.open(QIODevice::WriteOnly | QIODevice::Truncate));
    mine.write("{ \"editor\": { \"fontSize\": 42 } }\n");
    mine.close();
    ZT_TRUE("второй вызов не жалуется", zametti::writeConfigTemplate(&error));
    QFile again(path);
    ZT_TRUE("файл открывается", again.open(QIODevice::ReadOnly));
    ZT_TRUE("правки человека целы", again.readAll().contains("42"));
}

// Конфиг с комментариями обязан читаться настоящей загрузкой, а не только
// стриппером в отрыве от неё.
void checkLoadUnderstandsComments() {
    QFile file(zametti::configPath());
    ZT_TRUE("файл открывается на запись",
            file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write(
        "// мой конфиг\n"
        "{\n"
        "    // через сколько записывать\n"
        "    \"editor\": { \"autosaveDelayMs\": 1700, },   // отклонение, с висячей запятой\n"
        "}\n");
    file.close();

    QString error;
    ZT_TRUE("конфиг с комментариями прочитан: " + s(error),
            zametti::loadSettings(&error));
    ZT_EQ("значение из конфига применено", std::string("1700"),
          std::to_string(zametti::settings().editor().autosaveDelayMs()));

    // Битый конфиг обязан жаловаться, а не молча уезжать на умолчания.
    QFile broken(zametti::configPath());
    ZT_TRUE("файл открывается на запись",
            broken.open(QIODevice::WriteOnly | QIODevice::Truncate));
    broken.write("{ \"editor\": { \"autosaveDelayMs\": }\n");
    broken.close();
    QString complaint;
    ZT_TRUE("битый конфиг не принят", !zametti::loadSettings(&complaint));
    ZT_TRUE("и о нём сказано", !complaint.isEmpty());
}

// Раздел таблиц: умолчания ровно те, о которых договорились с владельцем, и
// все девять ключей читаются из конфига.
//
// Умолчание — таблица БЕЗ СЕТКИ: три чёрных линии поперёк (над заголовком, под
// ним и под последней строкой), всё остальное по нулям и прозрачно. Это не
// придирка к числам, а описание вида: поменяется умолчание — поменяется и
// таблица во всех заметках сразу, и узнать об этом надо здесь.
void checkTablesDefaults() {
    zametti::ZSettings fresh;
    ZT_EQ("цвет линий — чёрный", std::string("#000000"),
          fresh.tables().borderColor().name(QColor::HexRgb).toStdString());
    ZT_EQ("линия над и под таблицей", std::string("2"),
          std::to_string(int(fresh.tables().horizontalBorder())));
    ZT_EQ("линия под заголовком", std::string("2"),
          std::to_string(int(fresh.tables().headerSeparator())));
    ZT_EQ("вертикальных линий нет", std::string("0"),
          std::to_string(int(fresh.tables().verticalBorder())));
    ZT_EQ("разделителей строк нет", std::string("0"),
          std::to_string(int(fresh.tables().rowSeparator())));
    ZT_EQ("разделителей колонок нет", std::string("0"),
          std::to_string(int(fresh.tables().columnSeparator())));
    ZT_TRUE("заливка заголовка прозрачна", fresh.tables().headerColor().alpha() == 0);
    ZT_TRUE("заливка тела прозрачна", fresh.tables().tableColor().alpha() == 0);
    ZT_TRUE("зебры нет", fresh.tables().altTableColor().alpha() == 0);
}

void checkTablesFromConfig() {
    QFile file(zametti::configPath());
    ZT_TRUE("файл открывается на запись",
            file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write(R"cfg({
  "tables": {
    "borderColor": "#3355aa",
    "horizontalBorder": 1,
    "verticalBorder": 3,
    "headerSeparator": 4,
    "rowSeparator": 5,
    "columnSeparator": 6,
    "headerColor": "#eeeeee",
    "tableColor": "#80ffffff",
    "altTableColor": "#11223344"
  }
}
)cfg");
    file.close();

    QString error;
    QStringList unknown;
    ZT_TRUE("конфиг с разделом таблиц прочитан: " + s(error),
            zametti::loadSettings(&error, &unknown));
    // Раздел ЗНАКОМ программе: неизвестный ключ — это опечатка, о которой она
    // обязана сказать, и молчание здесь означало бы, что раздел не заведён.
    ZT_EQ("незнакомых ключей нет", std::string(), unknown.join(QLatin1Char(',')).toStdString());

    const zametti::ZSettings& a = zametti::settings();
    ZT_EQ("цвет линий", std::string("#3355aa"),
          a.tables().borderColor().name(QColor::HexRgb).toStdString());
    ZT_EQ("толщины прочитаны все шесть", std::string("1 3 4 5 6"),
          std::to_string(int(a.tables().horizontalBorder())) + " " +
              std::to_string(int(a.tables().verticalBorder())) + " " +
              std::to_string(int(a.tables().headerSeparator())) + " " +
              std::to_string(int(a.tables().rowSeparator())) + " " +
              std::to_string(int(a.tables().columnSeparator())));
    ZT_EQ("заливка заголовка", std::string("#eeeeee"),
          a.tables().headerColor().name(QColor::HexRgb).toStdString());
    ZT_EQ("полупрозрачная заливка тела", std::string("128"),
          std::to_string(a.tables().tableColor().alpha()));
    ZT_EQ("зебра с прозрачностью", std::string("17"),
          std::to_string(a.tables().altTableColor().alpha()));

    // Опечатка в имени ключа не должна проходить молча.
    QFile typo(zametti::configPath());
    ZT_TRUE("файл открывается на запись",
            typo.open(QIODevice::WriteOnly | QIODevice::Truncate));
    typo.write(R"cfg({ "tables": { "borderColour": "#123456" } })cfg");
    typo.close();
    QStringList complaints;
    zametti::loadSettings(&error, &complaints);
    ZT_EQ("об опечатке в ключе сказано", std::string("tables.borderColour"),
          complaints.join(QLatin1Char(',')).toStdString());
}

}  // namespace

// НАСТРОЙКИ — ПОЖЕЛАНИЯ, РОБАСТНОСТЬ ВЫШЕ (решение владельца): число вне
// допустимого диапазона обрезается сеттером самой настройки (ZM_SETTING в
// settings.h), а не проверяется где-то ещё. Здесь: абсурдный конфиг
// (imageCacheSizeMb = 100000000, отрицательная задержка, кегль в тысячу
// пунктов) не проходит как есть, и умолчания сами лежат в своих границах.
void checkClamping() {
    // Наборы идут одним процессом, а настройки — глобальные: обрезанный до
    // краёв кегль остался бы всем последующим наборам. Возвращаем как было.
    const zametti::ZSettings before = zametti::settings();
    struct Restore {
        const zametti::ZSettings& from;
        ~Restore() { zametti::editSettings() = from; }
    } restore{before};
    QFile file(zametti::configPath());
    ZT_TRUE("файл открывается на запись",
            file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write(
        "{\n"
        "  \"editor\": { \"imageCacheSizeMb\": 100000000, \"autosaveDelayMs\": -5,\n"
        "              \"documentCacheSizeMb\": 0, \"undoLimit\": 7 },\n"
        "  \"font\": { \"pointSize\": 1000 },\n"
        "  \"toolbar\": { \"iconSize\": 3 }\n"
        "}\n");
    file.close();
    QString error;
    ZT_TRUE("абсурдный конфиг читается (обрезается, а не отвергается): " + s(error),
            zametti::loadSettings(&error));
    const zametti::ZSettings& a = zametti::settings();
    ZT_EQ("кэш картинок обрезан до потолка", std::to_string(a.cache().imageCacheSizeMbMax()),
          std::to_string(a.cache().imageCacheSizeMb()));
    ZT_EQ("задержка автосохранения — до пола", std::to_string(a.editor().autosaveDelayMsMin()),
          std::to_string(a.editor().autosaveDelayMs()));
    ZT_EQ("кэш документов — до пола", std::to_string(a.cache().documentCacheSizeMbMin()),
          std::to_string(a.cache().documentCacheSizeMb()));
    ZT_EQ("значение в границах взято как есть", std::string("7"),
          std::to_string(a.editor().undoLimit()));
    ZT_EQ("кегль — до потолка", std::to_string(int(a.look().baseFontPointMax())),
          std::to_string(int(a.look().baseFontPoint())));
    ZT_EQ("иконка тулбара — до пола", std::to_string(a.look().toolbarIconSizeMin()),
          std::to_string(a.look().toolbarIconSize()));

    // Сеттер сам говорит, приняла ли настройка значение как есть.
    zametti::ZSettings own;
    ZT_TRUE("значение в границах принято", own.cache().setImageCacheSizeMb(512));
    ZT_TRUE("вне границ — обрезано и сказано", !own.cache().setImageCacheSizeMb(100000000));
    ZT_EQ("и лежит на потолке", std::to_string(own.cache().imageCacheSizeMbMax()),
          std::to_string(own.cache().imageCacheSizeMb()));

    // Умолчания лежат в своих границах: иначе программа спорила бы сама с собой.
    zametti::ZSettings def;
    ZT_TRUE("умолчание кэша картинок в границах",
            def.cache().setImageCacheSizeMb(def.cache().imageCacheSizeMb()));
    ZT_TRUE("умолчание кегля в границах", def.look().setBaseFontPoint(def.look().baseFontPoint()));
    ZT_TRUE("умолчание автосохранения в границах",
            def.editor().setAutosaveDelayMs(def.editor().autosaveDelayMs()));
    ZT_TRUE("умолчание качества фото в границах",
            def.images().setPhotoQuality(def.images().photoQuality()));
    ZT_TRUE("умолчание полей бумаги в границах", def.pdf().setMarginMm(def.pdf().marginMm()));
}

static int ztRunSuite(int argc, char** argv) {
    (void)argc;
    (void)argv;
    QTemporaryDir home;
    qputenv("XDG_CONFIG_HOME", home.path().toLocal8Bit());

    QCoreApplication::setApplicationName(QStringLiteral("zametti"));

    checkStripper();
    checkTemplate();
    checkLoadUnderstandsComments();
    checkClamping();
    checkTablesDefaults();
    checkTablesFromConfig();

    return zt::report("config");
}

// Набор целиком одним TEST: тело не тронуто, argv ему собран здесь.
// Дробить на отдельные проверки — отдельная работа, по одному набору.
TEST(Config, All) {
    std::vector<QByteArray> ztArgs{QByteArrayLiteral("config_test")};
    std::vector<char*> ztArgv;
    for (QByteArray& a : ztArgs) ztArgv.push_back(a.data());
    EXPECT_EQ(0, ztRunSuite(int(ztArgv.size()), ztArgv.data()));
}
