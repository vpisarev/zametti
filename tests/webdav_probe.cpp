// Живая приёмка WebDavCloud против НАСТОЯЩЕГО сервера (m17): digest-логин,
// заливка, листинг с датами, removeTree одним DELETE.
//
// Это пробник, не набор: серверу владельца в общем прогоне делать нечего.
// Пароль — из среды, как у CLI; работает ТОЛЬКО в названной коллекции — и
// стирает только её, нашим же removeTree (который сам не трогает ничего,
// кроме своей коллекции).
//
//   ZAMETTI_WEBDAV_PASSWORD='…' ↵
//   zametti-bench webdav https://host/webdav/zametti-probe user

#include "webdav_cloud.h"

#include <QDateTime>
#include <QUrl>

#include <cstdio>

int ztWebDavProbe(int argc, char** argv) {
    using zametti::CloudStore;
    using zametti::WebDavCloud;
    if (argc < 3) {
        std::printf("нужны адрес коллекции и логин:\n"
                    "  ZAMETTI_WEBDAV_PASSWORD='…' zametti-bench webdav "
                    "https://host/dav/zametti-probe user\n");
        return 2;
    }
    WebDavCloud::Config config;
    QString base = QString::fromLocal8Bit(argv[1]);
    if (!base.endsWith(QLatin1Char('/'))) base += QLatin1Char('/');
    config.base = QUrl(base);
    config.user = QString::fromLocal8Bit(argv[2]);
    config.password = qEnvironmentVariable("ZAMETTI_WEBDAV_PASSWORD");
    QString error;
    if (!WebDavCloud::checkUrl(config, &error)) {
        std::printf("адрес негоден: %s\n", qPrintable(error));
        return 1;
    }
    WebDavCloud cloud(config);
    const auto say = [](const char* what, bool ok, const QString& why) {
        std::printf("%-28s %s%s%s\n", what, ok ? "ок" : "ПРОВАЛ",
                    ok || why.isEmpty() ? "" : ": ", ok ? "" : qPrintable(why));
        return ok;
    };

    if (!say("mkdirOnce", cloud.mkdirOnce(&error), error)) return 1;
    const QByteArray body("живой пробник webdav, байты как байты");
    QString etag;
    if (!say("put №1", cloud.put(QStringLiteral("01aaaaaaaaaaaa.zm"), body, &etag, &error),
             error))
        return 1;
    std::printf("%-28s %s\n", "etag ответа PUT", qPrintable(etag));
    if (!say("put №2",
             cloud.put(QStringLiteral("01bbbbbbbbbbbb_jpg.pic"), body, nullptr, &error),
             error))
        return 1;

    QVector<CloudStore::Entry> listing;
    if (!say("list", cloud.list(&listing, &error), error)) return 1;
    for (const CloudStore::Entry& e : listing)
        std::printf("  %-24s %6lld Б  etag %-34s  %s\n", qPrintable(e.name), e.size,
                    qPrintable(e.etag),
                    e.lastModified.isValid()
                        ? qPrintable(e.lastModified.toString(Qt::ISODate))
                        : "БЕЗ ДАТЫ");
    if (listing.size() != 2)
        std::printf("ОЖИДАЛОСЬ 2 записи, пришло %d\n", int(listing.size()));

    QByteArray back;
    if (!say("get", cloud.get(QStringLiteral("01aaaaaaaaaaaa.zm"), &back, nullptr, &error),
             error))
        return 1;
    say("скачано побайтово то же", back == body, QStringLiteral("байты разошлись"));

    // ГЛАВНОЕ: снос всей коллекции ОДНИМ DELETE (RFC 4918 §9.6.1) и жизнь
    // после него.
    const CloudStore::Traffic before = cloud.traffic();
    if (!say("removeTree", cloud.removeTree(&error), error)) return 1;
    std::printf("%-28s %lld\n", "запросов на снос",
                cloud.traffic().requests - before.requests);
    if (!say("mkdirOnce после сноса", cloud.mkdirOnce(&error), error)) return 1;
    if (!say("list после сноса", cloud.list(&listing, &error), error)) return 1;
    say("и он пуст", listing.isEmpty(),
        QStringLiteral("%1 записей").arg(listing.size()));
    // Прибрать за собой: пустую коллекцию тоже уносим.
    say("уборка (removeTree)", cloud.removeTree(&error), error);
    std::printf("итого: %lld запросов, %lld Б вверх, %lld Б вниз\n",
                cloud.traffic().requests, cloud.traffic().bytesUp,
                cloud.traffic().bytesDown);
    return 0;
}
