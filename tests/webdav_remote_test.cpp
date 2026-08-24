// WebDavRemote против НАСТОЯЩЕГО сервера (m17, сессия 3).
//
// Приёмка: тот же контракт, что у фиктивных адаптеров, плюс то, что бывает
// только по проводу — неверный пароль, правило TLS, If-Match.
//
// Сервер поднимается обвязкой (`uvx wsgidav`) на время набора. Нет uvx или
// сети — ГРОМКИЙ пропуск.

#include "webdav_remote.h"

#include "fake_remote.h"
#include "testdata.h"
#include "test_util.h"
#include "webdav_harness.h"

namespace {

using zametti::WebDavRemote;

WebDavRemote::Config configFor(const zt::WebDavStand& stand) {
    WebDavRemote::Config config;
    config.base = stand.url();
    config.user = QString::fromUtf8(zt::WebDavStand::kUser);
    config.password = QString::fromUtf8(zt::WebDavStand::kPassword);
    config.timeoutMs = 20000;
    return config;
}

// Правило TLS — БЕЗ СЕТИ ВОВСЕ: это проверка адреса, а не сервера.
void checkUrlRule() {
    WebDavRemote::Config config;
    QString error;

    config.base = QUrl(QStringLiteral("https://dav.example.org/зам/"));
    ZT_TRUE("https разрешён", WebDavRemote::checkUrl(config, &error));

    config.base = QUrl(QStringLiteral("http://127.0.0.1:8080/зам/"));
    ZT_TRUE("http на localhost разрешён", WebDavRemote::checkUrl(config, &error));
    config.base = QUrl(QStringLiteral("http://localhost:8080/зам/"));
    ZT_TRUE("и по имени localhost", WebDavRemote::checkUrl(config, &error));

    config.base = QUrl(QStringLiteral("http://dav.example.org/зам/"));
    ZT_TRUE("внешний http запрещён по умолчанию",
            !WebDavRemote::checkUrl(config, &error));
    ZT_TRUE("и объяснено про пароль открытым текстом",
            error.contains(QLatin1String("clear")));
    ZT_TRUE("и названа настройка, которой это снимается",
            error.contains(QLatin1String("allowInsecureHttp")));

    config.allowInsecureHttp = true;
    ZT_TRUE("с явным разрешением внешний http проходит",
            WebDavRemote::checkUrl(config, &error));

    config.allowInsecureHttp = false;
    config.base = QUrl(QStringLiteral("ftp://dav.example.org/зам/"));
    ZT_TRUE("чужая схема запрещена", !WebDavRemote::checkUrl(config, &error));
    config.base = QUrl(QStringLiteral("вообще не адрес"));
    ZT_TRUE("негодный адрес запрещён", !WebDavRemote::checkUrl(config, &error));
}

}  // namespace

TEST(WebDavRemote, All) {
    checkUrlRule();

    zt::WebDavStand stand(zt::TestData::outDir(QStringLiteral("webdav")));
    ZT_SKIP_NO_WEBDAV(stand);

    // Тот же контракт, что у фиктивных адаптеров: обещания у всех одни.
    WebDavRemote remote(configFor(stand));
    QString error;
    ZT_TRUE("адрес стенда годен", WebDavRemote::checkUrl(remote.config(), &error));
    zt::checkRemoteContract(remote, "webdav");

    // Неверный пароль — внятный отказ, а не молчаливая пустота (пустота
    // означала бы «на сервере ничего нет» и стоила бы синку заливки всего).
    WebDavRemote::Config bad = configFor(stand);
    bad.password = QStringLiteral("не тот пароль");
    WebDavRemote refused(bad);
    QVector<zametti::RemoteStore::Entry> listing;
    ZT_TRUE("с неверным паролем листинг не удаётся",
            !refused.list(&listing, &error));
    ZT_TRUE("и сказано, что дело в логине",
            error.contains(QLatin1String("refused the login")));

    // If-Match против настоящего сервера. Тела РАЗНОЙ ДЛИНЫ нарочно: wsgidav
    // считает etag по размеру и времени файла, и три тела по шесть букв
    // подряд оставили бы метку прежней — проверка прошла бы, ничего не
    // проверив (первая редакция именно так и была написана).
    const QByteArray first("первое"), second = QByteArray("второе, подлиннее").repeated(3),
                     third = QByteArray("третье");
    QString etag;
    ZT_TRUE("put", remote.put(QStringLiteral("01ifmatch.log"), first, &etag, &error));
    const auto etagOf = [&](const char* what) {
        QVector<zametti::RemoteStore::Entry> listing;
        ZT_TRUE(what, remote.list(&listing, &error));
        for (const auto& entry : listing)
            if (entry.name == QLatin1String("01ifmatch.log")) return entry.etag;
        return QString();
    };
    const QString beforeEtag = etagOf("листинг до правки");
    ZT_TRUE("etag виден в листинге", !beforeEtag.isEmpty());

    QString newEtag;
    bool clash = false;
    ZT_TRUE("putIfMatch со свежим etag проходит",
            remote.putIfMatch(QStringLiteral("01ifmatch.log"), second, beforeEtag,
                              &newEtag, &clash, &error));
    ZT_TRUE("столкновения не было", !clash);

    const QString afterEtag = etagOf("листинг после правки");
    // Если сервер метку не сменил, спрашивать с него отказ по условию не за
    // что: проверять надо то, что он обещает, а не то, чего мы ждём.
    ZT_TRUE("сервер сменил etag после правки", afterEtag != beforeEtag);
    if (afterEtag != beforeEtag) {
        QByteArray body;
        ZT_TRUE("putIfMatch с устаревшим etag отказан",
                !remote.putIfMatch(QStringLiteral("01ifmatch.log"), third,
                                   beforeEtag, &newEtag, &clash, &error));
        ZT_TRUE("и назван столкновением", clash);
        ZT_TRUE("содержимое на сервере не тронуто",
                remote.get(QStringLiteral("01ifmatch.log"), &body, nullptr, &error) &&
                    body == second);
    }
    ZT_TRUE("прибрано", remote.del(QStringLiteral("01ifmatch.log"), &error));

    EXPECT_EQ(0, zt::freshFailures());
}
