// WebDavRemote: пять операций поверх HTTP. См. шапку webdav_remote.h.

#include "webdav_remote.h"

#include <QEventLoop>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrlQuery>
#include <QXmlStreamReader>

namespace zametti {
namespace {

// Имя блоба плоское: всё, что похоже на путь, отвергается здесь — иначе
// сервер (или подделанный листинг) мог бы увести запись в чужой каталог.
bool badName(const QString& name) {
    return name.isEmpty() || name.contains(QLatin1Char('/')) ||
           name.contains(QLatin1Char('\\')) || name.startsWith(QLatin1Char('.'));
}

QString httpTrouble(const QString& what, QNetworkReply* reply) {
    // Оборвал наш сторож бездействия — так и говорим: безликое «Operation
    // canceled» первым же живым прогоном прочиталось как загадка.
    const QVariant stalled = reply->property("zamettiStalledMs");
    if (stalled.isValid())
        return QStringLiteral("%1: no data for %2 s — the connection stalled")
            .arg(what)
            .arg(stalled.toInt() / 1000);
    const int code =
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (code == 401 || code == 403)
        return QStringLiteral("%1: the server refused the login (HTTP %2)")
            .arg(what).arg(code);
    if (code != 0)
        return QStringLiteral("%1: HTTP %2 %3").arg(what).arg(code).arg(
            reply->attribute(QNetworkRequest::HttpReasonPhraseAttribute).toString());
    return QStringLiteral("%1: %2").arg(what, reply->errorString());
}

// Кавычки и слабый префикс W/ у etag — оформление HTTP, а не часть метки.
// Сравнивать их побайтово значило бы объявлять «изменилось» при смене
// оформления на сервере.
QString tidyEtag(const QString& raw) {
    QString etag = raw.trimmed();
    if (etag.startsWith(QLatin1String("W/"))) etag = etag.mid(2);
    if (etag.startsWith(QLatin1Char('"')) && etag.endsWith(QLatin1Char('"')) &&
        etag.size() >= 2)
        etag = etag.mid(1, etag.size() - 2);
    return etag;
}

}  // namespace

struct WebDavRemote::Impl {
    QNetworkAccessManager net;
};

bool WebDavRemote::checkUrl(const Config& config, QString* error) {
    const QUrl& url = config.base;
    if (!url.isValid() || url.host().isEmpty()) {
        if (error != nullptr)
            *error = QStringLiteral("sync.url is not a valid address: %1")
                         .arg(url.toString());
        return false;
    }
    const QString scheme = url.scheme().toLower();
    if (scheme == QLatin1String("https")) return true;
    if (scheme != QLatin1String("http")) {
        if (error != nullptr)
            *error = QStringLiteral("sync.url: only http and https are supported, "
                                    "got %1").arg(scheme);
        return false;
    }
    // Голый http разрешён только САМОМУ СЕБЕ (localhost — наборы и замеры):
    // на внешнем адресе пароль сервера ехал бы открытым текстом. Прежний
    // ключ-лазейка sync.allowInsecureHttp убран решением владельца 28.08.2026.
    const QString host = url.host().toLower();
    const bool local = host == QLatin1String("localhost") ||
                       host == QLatin1String("127.0.0.1") ||
                       host == QLatin1String("::1");
    if (local) return true;
    if (error != nullptr)
        *error = QStringLiteral(
            "the address uses plain http, and the server password would travel "
            "in the clear — use https.");
    return false;
}

WebDavRemote::WebDavRemote(const Config& config)
    : config_(config), impl_(std::make_shared<Impl>()) {}

// ЛОГИН ШЛЁМ САМИ, а не через QAuthenticator, по двум причинам, и обе
// замерены пробником против живого wsgidav:
//
//   1. КОДИРОВКА. Qt кодирует пароль для basic auth не в UTF-8 (basic родом
//      из времён latin1), и пароль с кириллицей сервер отвергает — 401 на
//      каждой операции. Пароли у людей всякие, и терять их из-за буквы «ё»
//      нельзя. Здесь — честный UTF-8, как шлёт curl.
//   2. ЛИШНИЙ КРУГ. Через QAuthenticator каждый запрос идёт дважды: первый
//      получает 401, второй — с логином. Синк мерится счётчиками запросов,
//      и удваивать их незачем.
void WebDavRemote::authorize(::QNetworkRequest* request) const {
    if (config_.user.isEmpty()) return;
    const QByteArray pair =
        config_.user.toUtf8() + ':' + config_.password.toUtf8();
    request->setRawHeader("Authorization", "Basic " + pair.toBase64());
}

WebDavRemote::~WebDavRemote() = default;

namespace {

// Дождаться ответа, не давая программе висеть вечно, — но и не рубя живую
// передачу: сторож следит за БЕЗДЕЙСТВИЕМ, а не за длительностью. Большое
// вложение на медленном канале едет минутами, и это нормально; беда — когда
// байты перестали ходить вовсе. Каждый ушедший или пришедший кусок
// перезапускает таймер. Прежний дедлайн на всю операцию оборвал первую же
// живую заливку крупной картинки ровно на 30-й секунде («Operation
// canceled») — большие файлы были обречены по построению.
void waitFor(QNetworkReply* reply, int timeoutMs) {
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(reply, &QNetworkReply::uploadProgress, &timer,
                     [&timer, timeoutMs](qint64, qint64) { timer.start(timeoutMs); });
    QObject::connect(reply, &QNetworkReply::downloadProgress, &timer,
                     [&timer, timeoutMs](qint64, qint64) { timer.start(timeoutMs); });

    QObject::connect(&timer, &QTimer::timeout, [reply, timeoutMs] {
        // Пометка «оборвал сторож»: httpTrouble скажет про молчание канала,
        // а не отдаст безликое «Operation canceled» от Qt.
        reply->setProperty("zamettiStalledMs", timeoutMs);
        reply->abort();          // abort сам приведёт к finished
    });
    timer.start(timeoutMs);
    if (!reply->isFinished()) loop.exec();
}

}  // namespace

bool WebDavRemote::list(QVector<Entry>* out, QString* error) {
    Q_ASSERT(out != nullptr);
    ++traffic_.requests;
    QNetworkRequest request(config_.base);
    authorize(&request);
    request.setRawHeader("Depth", "1");
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/xml; charset=utf-8"));
    // Спрашиваем ровно три свойства: имя, размер, etag. Allprop заставил бы
    // сервер считать и слать лишнее на каждый блоб.
    const QByteArray body =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        "<d:propfind xmlns:d=\"DAV:\"><d:prop>"
        "<d:getcontentlength/><d:getetag/><d:resourcetype/>"
        "</d:prop></d:propfind>";
    std::unique_ptr<QNetworkReply, void (*)(QNetworkReply*)> reply(
        impl_->net.sendCustomRequest(request, "PROPFIND", body),
        [](QNetworkReply* r) { r->deleteLater(); });
    traffic_.bytesUp += body.size();
    waitFor(reply.get(), config_.timeoutMs);

    const int code =
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (reply->error() != QNetworkReply::NoError && code != 207) {
        if (error != nullptr) *error = httpTrouble(QStringLiteral("PROPFIND"), reply.get());
        return false;
    }
    const QByteArray xml = reply->readAll();
    traffic_.bytesDown += xml.size();

    // Разбор multistatus. Свой каталог (первый ответ, он же collection)
    // пропускаем: нас интересуют только блобы внутри.
    out->clear();
    QXmlStreamReader reader(xml);
    Entry current;
    bool inResponse = false, isCollection = false;
    while (!reader.atEnd()) {
        reader.readNext();
        if (reader.isStartElement()) {
            const QStringView name = reader.name();
            if (name == QLatin1String("response")) {
                inResponse = true;
                isCollection = false;
                current = Entry();
            } else if (!inResponse) {
                continue;
            } else if (name == QLatin1String("href")) {
                // Имя блоба — последний сегмент пути, декодированный.
                const QString href = reader.readElementText();
                QString path = QUrl(href).path();
                while (path.endsWith(QLatin1Char('/'))) {
                    path.chop(1);
                    isCollection = true;   // каталог: href оканчивается слэшем
                }
                current.name = QUrl::fromPercentEncoding(
                    path.section(QLatin1Char('/'), -1).toUtf8());
            } else if (name == QLatin1String("getcontentlength")) {
                current.size = reader.readElementText().toLongLong();
            } else if (name == QLatin1String("getetag")) {
                current.etag = tidyEtag(reader.readElementText());
            } else if (name == QLatin1String("collection")) {
                isCollection = true;
            }
        } else if (reader.isEndElement() &&
                   reader.name() == QLatin1String("response")) {
            inResponse = false;
            if (!isCollection && !current.name.isEmpty()) out->append(current);
        }
    }
    if (reader.hasError()) {
        if (error != nullptr)
            *error = QStringLiteral("PROPFIND: the answer is not valid XML: %1")
                         .arg(reader.errorString());
        return false;
    }
    return true;
}

bool WebDavRemote::get(const QString& name, QByteArray* bytes, QString* etag,
                       QString* error) {
    Q_ASSERT(bytes != nullptr);
    ++traffic_.requests;
    if (badName(name)) {
        if (error != nullptr)
            *error = QStringLiteral("webdav: bad blob name %1").arg(name);
        return false;
    }
    QNetworkRequest request(config_.base.resolved(QUrl(name)));
    authorize(&request);
    std::unique_ptr<QNetworkReply, void (*)(QNetworkReply*)> reply(
        impl_->net.get(request), [](QNetworkReply* r) { r->deleteLater(); });
    waitFor(reply.get(), config_.timeoutMs);
    if (reply->error() != QNetworkReply::NoError) {
        if (error != nullptr)
            *error = httpTrouble(QStringLiteral("GET %1").arg(name), reply.get());
        return false;
    }
    *bytes = reply->readAll();
    traffic_.bytesDown += bytes->size();
    if (etag != nullptr)
        *etag = tidyEtag(QString::fromUtf8(reply->rawHeader("ETag")));
    return true;
}

bool WebDavRemote::put(const QString& name, const QByteArray& bytes, QString* etag,
                       QString* error) {
    return putIfMatch(name, bytes, QString(), etag, nullptr, error);
}

bool WebDavRemote::putIfMatch(const QString& name, const QByteArray& bytes,
                              const QString& expectedEtag, QString* etag,
                              bool* preconditionFailed, QString* error) {
    ++traffic_.requests;
    if (preconditionFailed != nullptr) *preconditionFailed = false;
    if (badName(name)) {
        if (error != nullptr)
            *error = QStringLiteral("webdav: bad blob name %1").arg(name);
        return false;
    }
    QNetworkRequest request(config_.base.resolved(QUrl(name)));
    authorize(&request);
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/octet-stream"));
    if (!expectedEtag.isEmpty())
        request.setRawHeader("If-Match", ('"' + expectedEtag + '"').toUtf8());

    std::unique_ptr<QNetworkReply, void (*)(QNetworkReply*)> reply(
        impl_->net.put(request, bytes), [](QNetworkReply* r) { r->deleteLater(); });
    waitFor(reply.get(), config_.timeoutMs);
    const int code =
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (code == 412) {
        // Сервер сказал «там уже другое». Это не беда связи, а ответ на
        // вопрос, и вызывающий обязан его различать.
        if (preconditionFailed != nullptr) *preconditionFailed = true;
        if (error != nullptr)
            *error = QStringLiteral("%1 changed on the server").arg(name);
        return false;
    }
    if (reply->error() != QNetworkReply::NoError) {
        if (error != nullptr)
            *error = httpTrouble(QStringLiteral("PUT %1").arg(name), reply.get());
        return false;
    }
    traffic_.bytesUp += bytes.size();
    // ETag на ответ PUT дают не все серверы; пусто — просто нет подсказки.
    if (etag != nullptr)
        *etag = tidyEtag(QString::fromUtf8(reply->rawHeader("ETag")));
    return true;
}

bool WebDavRemote::del(const QString& name, QString* error) {
    ++traffic_.requests;
    if (badName(name)) {
        if (error != nullptr)
            *error = QStringLiteral("webdav: bad blob name %1").arg(name);
        return false;
    }
    QNetworkRequest request(config_.base.resolved(QUrl(name)));
    authorize(&request);
    std::unique_ptr<QNetworkReply, void (*)(QNetworkReply*)> reply(
        impl_->net.deleteResource(request),
        [](QNetworkReply* r) { r->deleteLater(); });
    waitFor(reply.get(), config_.timeoutMs);
    const int code =
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    // Удаление идемпотентно: чего нет — того нет, и это не беда.
    if (code == 404) return true;
    if (reply->error() != QNetworkReply::NoError) {
        if (error != nullptr)
            *error = httpTrouble(QStringLiteral("DELETE %1").arg(name), reply.get());
        return false;
    }
    return true;
}

bool WebDavRemote::mkdirOnce(QString* error) {
    ++traffic_.requests;
    QNetworkRequest request(config_.base);
    authorize(&request);
    std::unique_ptr<QNetworkReply, void (*)(QNetworkReply*)> reply(
        impl_->net.sendCustomRequest(request, "MKCOL"),
        [](QNetworkReply* r) { r->deleteLater(); });
    waitFor(reply.get(), config_.timeoutMs);
    const int code =
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    // 405 — «уже есть»: MKCOL по существующей коллекции. Ровно то, чего мы и
    // хотели, потому не беда.
    if (code == 405) return true;
    if (reply->error() != QNetworkReply::NoError) {
        if (error != nullptr) *error = httpTrouble(QStringLiteral("MKCOL"), reply.get());
        return false;
    }
    return true;
}

}  // namespace zametti
