// WebDavRemote — облако по WebDAV (m17, сессия 3).
//
// Пять операций брифа поверх пяти методов HTTP: PROPFIND Depth:1 (листинг),
// GET, PUT, DELETE, MKCOL (один раз). Basic auth поверх TLS; If-Match — если
// сервер честен, иначе молча деградируем (дефолт RemoteStore).
//
// СИНХРОННЫЙ по построению: у каждой операции свой QEventLoop. Синк живёт в
// фоновом потоке, и асинхронность внутри адаптера только запутала бы порядок
// шагов, который в этом этапе — инвариант.
//
// TLS: содержимое едет зашифрованным всегда, поэтому голый http рискует
// одним — паролем СЕРВЕРА. Голый http разрешён только localhost (наборы и
// замеры); ключа-лазейки для внешних адресов нет (решение владельца
// 28.08.2026, прежний sync.allowInsecureHttp убран).
//
// В ЗАГОЛОВКЕ НЕТ НИ ОДНОГО ТИПА QtNetwork — линковка Qt6::Network остаётся
// PRIVATE у ядра, вся сеть живёт в .cpp.

#ifndef ZAMETTI_SYNC_WEBDAV_REMOTE_H
#define ZAMETTI_SYNC_WEBDAV_REMOTE_H

#include "remote_store.h"

#include <QString>
#include <QUrl>

#include <memory>

// Forward-объявление ЗДЕСЬ, а не в подписи метода: «class QNetworkRequest*»
// внутри namespace zametti объявил бы zametti::QNetworkRequest — свой,
// пустой и чужому не равный (компилятор поймал сразу).
class QNetworkRequest;

namespace zametti {

class WebDavRemote : public RemoteStore {
public:
    struct Config {
        QUrl base;                    // адрес каталога, напр. https://dav/зам/<storeId>
        QString user;
        QString password;
        // Сторож БЕЗДЕЙСТВИЯ: обрыв, когда байты не ходят дольше этого. Не
        // дедлайн операции — большой файл едет столько, сколько едет.
        int timeoutMs = 30000;
    };

    // Годен ли адрес к работе: схема, хост, правило TLS. Ложь — объяснение в
    // error; спрашивается ДО первой операции, чтобы пароль не уехал в сеть
    // открытым текстом даже один раз.
    static bool checkUrl(const Config& config, QString* error);

    explicit WebDavRemote(const Config& config);
    ~WebDavRemote() override;

    bool list(QVector<Entry>* out, QString* error = nullptr) override;
    bool get(const QString& name, QByteArray* bytes, QString* etag,
             QString* error = nullptr) override;
    // До шести реплаев в полёте — см. .cpp; контракт как у дефолта.
    void getMany(const QStringList& names, QHash<QString, Fetched>* out) override;
    bool put(const QString& name, const QByteArray& bytes, QString* etag,
             QString* error = nullptr) override;
    bool putIfMatch(const QString& name, const QByteArray& bytes,
                    const QString& expectedEtag, QString* etag,
                    bool* preconditionFailed, QString* error = nullptr) override;
    bool del(const QString& name, QString* error = nullptr) override;
    bool mkdirOnce(QString* error = nullptr) override;

    const Config& config() const { return config_; }

protected:
    // Заголовок Authorization ставим сами (UTF-8, преждевременно) — см. .cpp:
    // Qt кодирует basic auth не в UTF-8, и пароль с кириллицей не проходит.
    void authorize(::QNetworkRequest* request) const;

    struct Impl;                  // QNetworkAccessManager и всё сетевое — в .cpp
    Config config_;
    std::shared_ptr<Impl> impl_;
};

}  // namespace zametti

#endif  // ZAMETTI_SYNC_WEBDAV_REMOTE_H
