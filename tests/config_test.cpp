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

#include <QCoreApplication>
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
    const QJsonDocument defaults = QJsonDocument::fromJson(zametti::defaultAppearanceJson());
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
            zametti::loadAppearance(&error));
    ZT_EQ("значение из конфига применено", std::string("1700"),
          std::to_string(zametti::appearance().autosaveDelayMs));

    // Битый конфиг обязан жаловаться, а не молча уезжать на умолчания.
    QFile broken(zametti::configPath());
    ZT_TRUE("файл открывается на запись",
            broken.open(QIODevice::WriteOnly | QIODevice::Truncate));
    broken.write("{ \"editor\": { \"autosaveDelayMs\": }\n");
    broken.close();
    QString complaint;
    ZT_TRUE("битый конфиг не принят", !zametti::loadAppearance(&complaint));
    ZT_TRUE("и о нём сказано", !complaint.isEmpty());
}

}  // namespace

int main(int argc, char** argv) {
    QTemporaryDir home;
    qputenv("XDG_CONFIG_HOME", home.path().toLocal8Bit());

    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("zametti"));

    checkStripper();
    checkTemplate();
    checkLoadUnderstandsComments();

    return zt::report("config");
}
