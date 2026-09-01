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
#include "settings_hook.h"

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

    // 2. В нём есть КАЖДЫЙ ПУБЛИЧНЫЙ ключ — иначе это не меню, а пустышка, — и
    //    у каждого своя подпись: имя без объяснения человеку ничего не даёт,
    //    ради этого шаблон и перестал быть просто дампом умолчаний.
    int missing = 0;
    int mute = 0;
    for (const zametti::ZSettings::Key& key : zametti::ZSettings::registry()) {
        const QByteArray name = QStringLiteral("\"%1\"").arg(QString::fromLatin1(key.name)).toUtf8();
        if (!written.contains(name)) {
            ++missing;
            std::printf("  нет в шаблоне: %s.%s\n", key.section, key.name);
        }
        if (key.note == nullptr || *key.note == '\0') {
            ++mute;
            std::printf("  без подписи: %s.%s\n", key.section, key.name);
        }
    }
    ZT_TRUE("в шаблоне есть каждый публичный ключ", missing == 0);
    ZT_TRUE("и у каждого своя подпись", mute == 0);

    // 3. А внутренних коэффициентов отрисовки в нём НЕТ (решение владельца:
    //    убрать из JSON совсем). Три имени наугад из тех, что жили в конфиге
    //    до этой уборки, — если хоть одно вернулось, значит вернулась и
    //    россыпь, ради ухода от которой всё затевалось.
    for (const char* internal : {"bulletRise", "codeCornerRadius", "cornerOffset"})
        ZT_TRUE(std::string("внутренней ручки в шаблоне нет: ") + internal,
                !written.contains(internal));

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
    ZT_EQ("и прежнее значение осталось", std::string("1700"),
          std::to_string(zametti::settings().editor().autosaveDelayMs()));

    // КОНФИГ — ОТКЛОНЕНИЯ ОТ УМОЛЧАНИЙ, И ЧИТАЕТСЯ С ЧИСТОГО ЛИСТА: убрали ключ
    // — значение вернулось к умолчанию, а не зависло (в редакторе внутри
    // программы это обычное дело).
    QFile empty(zametti::configPath());
    ZT_TRUE("файл открывается на запись", empty.open(QIODevice::WriteOnly | QIODevice::Truncate));
    empty.write("{}\n");
    empty.close();
    ZT_TRUE("пустой конфиг прочитан", zametti::loadSettings(&error));
    ZT_EQ("убранный ключ вернулся к умолчанию",
          std::to_string(zametti::ZSettings{}.editor().autosaveDelayMs()),
          std::to_string(zametti::settings().editor().autosaveDelayMs()));

    // Опечатка названа поимённо, а верный ключ рядом — нет. Здесь же ловится
    // и целая секция, которой у программы нет.
    const QJsonObject probe =
        QJsonDocument::fromJson("{\"editor\": {\"tabWidth\": 2, \"tabWdth\": 3},"
                                " \"colours\": {\"caret\": \"#fff\"}}").object();
    const QStringList unknown = zametti::unknownConfigKeys(probe);
    ZT_EQ("опечатка и чужая секция названы, верный ключ — нет",
          std::string("colours, editor.tabWdth"), s(unknown.join(QStringLiteral(", "))));
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
    ZT_EQ("линия над и под таблицей", std::string("1"),
          std::to_string(int(fresh.tables().horizontalBorder())));
    ZT_EQ("линия под заголовком", std::string("1"),
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

// КРУГ ПО РЕЕСТРУ: то, что программа напечатала как свои умолчания, она
// обязана прочитать обратно и получить ровно то же. Проверка ОБЩАЯ — по всем
// ключам сразу, а не по списку избранных: раньше здесь стояли девять ключей
// раздела таблиц, и о ключе, который печатался, но не читался, набор не знал
// бы ничего. Ровно эта беда и была возможна, пока запись и чтение конфига были
// двумя рукописными функциями.
void checkRegistryRoundTrip() {
    const QByteArray dump = zametti::defaultSettingsJson();
    {
        QFile file(zametti::configPath());
        ZT_TRUE("умолчания записаны конфигом",
                file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write(dump);
    }
    QString error;
    QStringList unknown;
    ZT_TRUE("конфиг из умолчаний прочитан: " + s(error), zametti::loadSettings(&error, &unknown));
    ZT_EQ("и ни одного незнакомого ключа в нём нет", std::string(),
          unknown.join(QLatin1Char(',')).toStdString());
    // Печатаем ещё раз — и сверяем побайтово: разойтись эти два дампа могут
    // только если какой-то ключ читается не туда, куда пишется.
    ZT_EQ("круг «напечатали — прочитали — напечатали» сходится", dump.toStdString(),
          zametti::defaultSettingsJson().toStdString());

    // ОТКЛОНЕНИЕ ПО КАЖДОМУ КЛЮЧУ: значение, заведомо отличное от умолчания,
    // обязано доехать до настроек. Числа сдвигаем на единицу, строки —
    // приписыванием, признаки — отрицанием; списки и выражения этот проход
    // пропускает: у них своя проверка выше.
    for (const zametti::ZSettings::Key& key : zametti::ZSettings::registry()) {
        const zametti::ZSettings fresh;
        const QJsonValue was = key.get(fresh);
        QJsonValue want;
        if (was.isBool()) want = !was.toBool();
        else if (was.isDouble()) want = was.toDouble() + 1.0;
        else if (was.isString()) want = was.toString() + QStringLiteral("x");
        else continue;

        zametti::ZSettings mine;
        const std::string where = std::string(key.section) + "." + key.name;
        // Значение может не влезть в диапазон (кегль у потолка) — тогда set
        // честно отвечает ложью, и проверять нечего, кроме самой честности.
        if (!key.set(mine, want)) continue;
        ZT_TRUE(where + ": отклонение доехало",
                zametti::ZSettings::registry().empty() || key.get(mine) != was);
    }
}

void checkClamping() {
    // Наборы идут одним процессом, а настройки — глобальные: обрезанный до
    // краёв кегль остался бы всем последующим наборам. Возвращаем как было.
    const zametti::ZSettings before = zametti::settings();
    struct Restore {
        const zametti::ZSettings& from;
        ~Restore() { zametti::mutableSettingsForTests() = from; }
    } restore{before};
    QFile file(zametti::configPath());
    ZT_TRUE("файл открывается на запись",
            file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write(
        "{\n"
        "  \"editor\": { \"autosaveDelayMs\": -5, \"tabWidth\": 7,\n"
        "              \"historyMergeHours\": 100000 },\n"
        "  \"fonts\": { \"noteSize\": 1000, \"appSize\": 1000 }\n"
        "}\n");
    file.close();
    QString error;
    ZT_TRUE("абсурдный конфиг читается (обрезается, а не отвергается): " + s(error),
            zametti::loadSettings(&error));
    const zametti::ZSettings& a = zametti::settings();
    ZT_EQ("задержка автосохранения — до пола", std::to_string(a.editor().autosaveDelayMsMin()),
          std::to_string(a.editor().autosaveDelayMs()));
    ZT_EQ("часы слияния истории — до потолка",
          std::to_string(a.history().historyMergeHoursMax()),
          std::to_string(a.history().historyMergeHours()));
    ZT_EQ("значение в границах взято как есть", std::string("7"),
          std::to_string(a.editor().tabWidth()));
    ZT_EQ("кегль — до потолка", std::to_string(int(a.style().baseFontPointMax())),
          std::to_string(int(a.style().baseFontPoint())));
    ZT_EQ("кегль оболочки — до потолка", std::to_string(int(a.ui().appPointMax())),
          std::to_string(int(a.ui().appPoint())));

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
    ZT_TRUE("умолчание кегля в границах", def.style().setBaseFontPoint(def.style().baseFontPoint()));
    ZT_TRUE("умолчание автосохранения в границах",
            def.editor().setAutosaveDelayMs(def.editor().autosaveDelayMs()));
    ZT_TRUE("умолчание качества фото в границах",
            def.images().setPhotoQuality(def.images().photoQuality()));
    ZT_TRUE("умолчание полей бумаги в границах", def.pdf().setMarginMm(def.pdf().marginMm()));
}

}  // namespace

static int ztRunSuite(int argc, char** argv) {
    (void)argc;
    (void)argv;
    // СВОЙ каталог настроек, и прежний ВОЗВРАЩАЕТСЯ на выходе. Общий временный
    // каталог наборам уже дал main (конфиг владельца не виден никому), но этот
    // набор — единственный, кто настоящие конфиги ПИШЕТ: с отклонениями, с
    // опечатками, битые. Оставить их соседям нельзя — наборы идут одним
    // процессом, и loadSettings(nullptr) у четверых читал бы написанное здесь.
    QTemporaryDir home;
    const QByteArray previousHome = qgetenv(zametti::kConfigDirVar);
    qputenv(zametti::kConfigDirVar, home.path().toLocal8Bit());

    QCoreApplication::setApplicationName(QStringLiteral("zametti"));

    checkStripper();
    checkTemplate();
    checkLoadUnderstandsComments();
    checkClamping();
    checkTablesDefaults();
    checkRegistryRoundTrip();

    qputenv(zametti::kConfigDirVar, previousHome);
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
