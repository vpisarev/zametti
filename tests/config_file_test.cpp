// ZConfigFile — модель конфига для редактора настроек: шаблон на пустом месте,
// круг записи, проверка с номером строки и неизвестными ключами, отказ записи.

#include "config_file.h"
#include "settings.h"
#include "test_util.h"
#include "testdata.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>

#include <string>

namespace {

std::string s(const QString& q) { return q.toStdString(); }

void checkTemplateOnLoad() {
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("sub/config.json"));
    zametti::ZConfigFile model(path);
    QString error;
    ZT_TRUE("нет файла — load пишет шаблон: " + s(error), model.load(&error));
    ZT_TRUE("файл появился", QFile::exists(path));
    ZT_EQ("и это ровно шаблон", zametti::configTemplate().toStdString(), model.text().toStdString());
    ZT_TRUE("шаблон не грязный", !model.dirty());
    // Шаблон разбирается в пустой объект — отклонений нет.
    const zametti::ZConfigFile::Check check = zametti::ZConfigFile::check(model.text());
    ZT_TRUE("шаблон разбирается: " + s(check.error), check.ok);
    ZT_TRUE("и без неизвестных ключей", check.unknownKeys.isEmpty());

    // Круг записи.
    model.setText(QStringLiteral("{\n  \"fonts\": { \"noteSize\": 13 }\n}\n"));
    ZT_TRUE("грязный после правки", model.dirty());
    ZT_TRUE("записалось: " + s(error), model.save(&error));
    ZT_TRUE("чистый после записи", !model.dirty());
    zametti::ZConfigFile again(path);
    ZT_TRUE("перечитан", again.load(&error));
    ZT_EQ("текст тот же", std::string("{\n  \"fonts\": { \"noteSize\": 13 }\n}\n"), s(again.text()));
}

void checkCheck() {
    using zametti::ZConfigFile;
    const ZConfigFile::Check broken =
        ZConfigFile::check(QStringLiteral("// шапка\n{\n  \"a\": 1,\n  \"b\" }\n"));
    ZT_TRUE("битый не проходит", !broken.ok);
    ZT_EQ("строка беды — четвёртая", std::string("4"), std::to_string(broken.line));
    ZT_TRUE("колонка названа", broken.column > 0);
    ZT_TRUE("причина названа", !broken.error.isEmpty());

    const ZConfigFile::Check unknown =
        ZConfigFile::check(QStringLiteral("{ \"fonts\": { \"foo\": 1 }, \"bar\": {} }"));
    ZT_TRUE("разбирается", unknown.ok);
    ZT_EQ("неизвестные ключи названы (в порядке ключей JSON)", std::string("bar, fonts.foo"),
          s(unknown.unknownKeys.join(QStringLiteral(", "))));
    ZT_TRUE("известный ключ — не неизвестный",
            ZConfigFile::check(QStringLiteral("{ \"editor\": { \"tabWidth\": 2 } }"))
                .unknownKeys.isEmpty());
    ZT_TRUE("комментарии и висячие запятые — не беда",
            ZConfigFile::check(QStringLiteral("{ // c\n \"fonts\": { \"noteSize\": 12, }, }")).ok);
    ZT_TRUE("массив вместо объекта — беда", !ZConfigFile::check(QStringLiteral("[1, 2]")).ok);
}

void checkSaveRefused() {
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("ro/config.json"));
    QDir().mkpath(dir.filePath(QStringLiteral("ro")));
    zametti::ZConfigFile model(path);
    ZT_TRUE("создан", model.load());
    // Каталог без права записи: QSaveFile не сможет завести временный файл.
    QFile::setPermissions(dir.filePath(QStringLiteral("ro")), QFileDevice::ReadOwner | QFileDevice::ExeOwner);
    model.setText(QStringLiteral("{}"));
    QString error;
    const bool saved = model.save(&error);
    QFile::setPermissions(dir.filePath(QStringLiteral("ro")),
                          QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    // Под root запись проходит и так; тогда проверять нечего — не врём.
    if (!saved) {
        ZT_TRUE("причина названа", !error.isEmpty());
        zametti::ZConfigFile again(path);
        ZT_TRUE("перечитан", again.load());
        ZT_EQ("файл нетронут", zametti::configTemplate().toStdString(), again.text().toStdString());
    }
}

}  // namespace

TEST(ConfigFile, All) {
    checkTemplateOnLoad();
    checkCheck();
    checkSaveRefused();
}
