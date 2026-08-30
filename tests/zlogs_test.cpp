// ЛОГИ: err.log и sync.log — предел размером, проверка при первой записи.
//
// Что проверяется:
//   - выключено = файла не появляется вовсе (дефолт — выключено);
//   - строка получает метку времени UTC;
//   - переполнение чинится ПРИ ПЕРВОЙ записи сессии: остаются свежие 80%
//     предела, рез по границе строки; вторая запись уже не подрезает;
//   - configure заново взводит проверку;
//   - getLogStream — поток-строка в ZLogs::instance().

#include "zlogs.h"

#include "test_util.h"
#include "scratch_files.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <string>
#include <vector>

using namespace zametti;

namespace {

QByteArray readAll(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return f.readAll();
}

void checkDisabledWritesNothing() {
    QTemporaryDir home;
    ZLogs logs(home.path());
    logs.configure({false, false, 1 << 20});
    logs.err(QStringLiteral("беда"));
    logs.sync(QStringLiteral("ход"));
    ZT_TRUE("err.log не появился", !QFile::exists(logs.errPath()));
    ZT_TRUE("sync.log не появился", !QFile::exists(logs.syncPath()));
}

void checkLineCarriesUtcStamp() {
    QTemporaryDir home;
    ZLogs logs(home.path());
    logs.configure({true, true, 1 << 20});
    logs.sync(QStringLiteral("первый прогон"));
    const QByteArray all = readAll(logs.syncPath());
    ZT_TRUE("строка записана", all.contains("первый прогон"));
    // Метка UTC: ISO-время с суффиксом Z перед текстом.
    ZT_TRUE("метка времени в UTC", all.contains("Z первый прогон"));
    ZT_TRUE("err отдельно и пуст", !QFile::exists(logs.errPath()));
}

void checkTrimHappensOnFirstWriteOnly() {
    QTemporaryDir home;
    const qint64 limit = 1000;
    ZLogs logs(home.path());
    logs.configure({false, true, limit});

    // Готовый распухший лог «с прошлой сессии»: строки по 50 байт.
    {
        QFile f(logs.syncPath());
        ZT_TRUE("файл завёлся", f.open(QIODevice::WriteOnly));
        for (int i = 0; i < 100; ++i)
            f.write(QStringLiteral("2026-08-24T00:00:00Z старая строка %1\n")
                        .arg(i, 4, 10, QLatin1Char('0'))
                        .toUtf8());
    }
    const qint64 before = QFile(logs.syncPath()).size();
    ZT_TRUE("заготовка больше предела", before > limit);

    logs.sync(QStringLiteral("свежая после подрезки"));
    const QByteArray all = readAll(logs.syncPath());
    ZT_TRUE("файл подрезан к 80% предела",
            all.size() <= limit * 8 / 10 + 64);  // + свежая строка
    ZT_TRUE("рез по границе строки", all.startsWith("2026-"));
    ZT_TRUE("свежие строки выжили", all.contains("старая строка 0099"));
    ZT_TRUE("старейшие ушли", !all.contains("старая строка 0000"));
    ZT_TRUE("новая строка на месте", all.contains("свежая после подрезки"));

    // Вторая запись сессии НЕ подрезает, даже если файл снова за пределом.
    {
        QFile f(logs.syncPath());
        ZT_TRUE("дозапись мусора", f.open(QIODevice::WriteOnly | QIODevice::Append));
        f.write(QByteArray(2000, 'x'));
        f.write("\n");
    }
    logs.sync(QStringLiteral("вторая запись"));
    ZT_TRUE("второй раз не подрезали", QFile(logs.syncPath()).size() > limit);

    // Перечитанный конфиг заново взводит проверку — первая запись подрежет.
    logs.configure({false, true, limit});
    logs.sync(QStringLiteral("после переезда конфига"));
    ZT_TRUE("после configure подрезано снова",
            QFile(logs.syncPath()).size() <= limit * 8 / 10 + 64);
}

void checkLogStreamGoesToInstance() {
    // Единственный экземпляр программы; наборы живут в подменённом
    // ZAMETTI_CONFIG_DIR (tests/main.cpp), так что пишем в песочницу.
    ZLogs::instance().configure({false, true, 1 << 20});
    getLogStream(LogKind::Sync) << "поток-строка " << 42;
    const QByteArray all = readAll(ZLogs::instance().syncPath());
    ZT_TRUE("строка дошла через глобальную дверь", all.contains("поток-строка 42"));
    // Прибрать за собой: другим наборам логи не нужны.
    ZLogs::instance().configure({});
    const QString log = ZLogs::instance().syncPath();
    zt::dropFile(QFileInfo(log).absolutePath(), log);
}

}  // namespace

static int ztRunSuite(int argc, char** argv) {
    (void)argc;
    (void)argv;
    checkDisabledWritesNothing();
    checkLineCarriesUtcStamp();
    checkTrimHappensOnFirstWriteOnly();
    checkLogStreamGoesToInstance();
    return zt::report("zlogs");
}

TEST(ZLogs, All) {
    std::vector<QByteArray> ztArgs{QByteArrayLiteral("zlogs_test")};
    std::vector<char*> ztArgv;
    for (QByteArray& a : ztArgs) ztArgv.push_back(a.data());
    EXPECT_EQ(0, ztRunSuite(int(ztArgv.size()), ztArgv.data()));
}
