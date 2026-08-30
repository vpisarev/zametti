// WebDavCloud: пять операций поверх HTTP. См. шапку webdav_cloud.h.

#include "webdav_cloud.h"

#include <QAuthenticator>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrlQuery>
#include <QXmlStreamReader>

#include <functional>

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

struct WebDavCloud::Impl {
    QNetworkAccessManager net;
    // Сервер уже бросал challenge (Digest): наш упреждающий Basic ему не
    // нужен и ВРЕДЕН — сырой заголовок Authorization затирает digest-ответ,
    // который Qt считает из кэша для следующих запросов (первый живой прогон
    // на na4u: GET прошёл, PUT упёрся в 401 ровно из-за этого).
    bool challenged = false;
};

bool WebDavCloud::checkUrl(const Config& config, QString* error) {
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

WebDavCloud::WebDavCloud(const Config& config)
    : config_(config), impl_(std::make_shared<Impl>()) {
    // DIGEST И ПРОЧИЕ CHALLENGE-СХЕМЫ. Basic мы шлём сами и заранее (см.
    // authorize), но не всякий сервер его принимает: сервер владельца
    // (na4u.ru, Apache) отвечает 401 c WWW-Authenticate: Digest MD5 — и
    // прежний адаптер вечно упирался в «the server refused the login».
    // На challenge Qt спрашивает реквизиты здесь и сам считает Digest-ответ;
    // удача кэшируется менеджером, так что лишний круг — только у первого
    // запроса. Вечного цикла нет: повторный вызов с НЕИЗМЕНЁННЫМИ
    // реквизитами Qt считает отказом и возвращает 401 наружу.
    QObject::connect(&impl_->net, &QNetworkAccessManager::authenticationRequired,
                     &impl_->net,
                     [config, impl = impl_.get()](QNetworkReply*,
                                                  QAuthenticator* authenticator) {
                         impl->challenged = true;
                         authenticator->setUser(config.user);
                         authenticator->setPassword(config.password);
                     });
}

// БЕЗУСЛОВНЫЙ Basic ШЛЁМ САМИ, а не через QAuthenticator, по двум причинам,
// и обе замерены пробником против живого wsgidav:
//
//   1. КОДИРОВКА. Qt кодирует пароль для basic auth не в UTF-8 (basic родом
//      из времён latin1), и пароль с кириллицей сервер отвергает — 401 на
//      каждой операции. Пароли у людей всякие, и терять их из-за буквы «ё»
//      нельзя. Здесь — честный UTF-8, как шлёт curl.
//   2. ЛИШНИЙ КРУГ. Через QAuthenticator каждый запрос идёт дважды: первый
//      получает 401, второй — с логином. Синк мерится счётчиками запросов,
//      и удваивать их незачем.
//
// Digest-серверу упреждающий Basic не просто бесполезен — после первого же
// challenge он ЗАТИРАЕТ digest-заголовок, который Qt кладёт из кэша (сырой
// Authorization сильнее), и всё, кроме первого запроса, валится в 401.
// Поэтому: сервер бросил challenge — Basic больше не шлём вовсе, реквизиты
// живут у QAuthenticator (см. конструктор).
void WebDavCloud::authorize(::QNetworkRequest* request) const {
    if (config_.user.isEmpty()) return;
    if (impl_->challenged) return;
    const QByteArray pair =
        config_.user.toUtf8() + ':' + config_.password.toUtf8();
    request->setRawHeader("Authorization", "Basic " + pair.toBase64());
}

WebDavCloud::~WebDavCloud() = default;

namespace {

// Дождаться ответа, не давая программе висеть вечно, — но и не рубя живую
// передачу: сторож следит за БЕЗДЕЙСТВИЕМ, а не за длительностью. Большое
// вложение на медленном канале едет минутами, и это нормально; беда — когда
// байты перестали ходить вовсе. Каждый ушедший или пришедший кусок
// перезапускает таймер. Прежний дедлайн на всю операцию оборвал первую же
// живую заливку крупной картинки ровно на 30-й секунде («Operation
// canceled») — большие файлы были обречены по построению.
// ВТОРАЯ ЛОВУШКА, найденная тем же живым сервером: у ЗАЛИВКИ прогресс — это
// байты, ушедшие в БУФЕР TLS-сокета, а не принятые сервером. Полтора
// мегабайта оседают в буфере мгновенно, uploadProgress доходит до 100% — и
// дальше, пока медленный сервер (замерен канал ~16 КБ/с) вычитывает сокет,
// сигналов нет ВООБЩЕ: бездействие неизмеримо. Потому после полной отправки
// тела ответу даётся допуск из расчёта самого медленного терпимого канала
// (kSlowestBytesPerSec — не порог владельца, просто дно терпения): живой
// медленный сервер доживает, мёртвый отваливается, когда допуск вышел.
constexpr qint64 kSlowestBytesPerSec = 4 * 1024;

void waitFor(QNetworkReply* reply, int timeoutMs, qint64 uploadBytes = 0) {
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QTimer timer;
    timer.setSingleShot(true);
    const auto sentAll = std::make_shared<bool>(uploadBytes <= 0);
    QObject::connect(reply, &QNetworkReply::uploadProgress, &timer,
                     [&timer, timeoutMs, sentAll](qint64 sent, qint64 total) {
                         if (total > 0 && sent >= total) *sentAll = true;
                         timer.start(timeoutMs);
                     });
    QObject::connect(reply, &QNetworkReply::downloadProgress, &timer,
                     [&timer, timeoutMs](qint64, qint64) { timer.start(timeoutMs); });

    QElapsedTimer whole;
    whole.start();
    const qint64 drainBudgetMs =
        qint64(timeoutMs) + uploadBytes * 1000 / kSlowestBytesPerSec;
    QObject::connect(&timer, &QTimer::timeout,
                     [reply, timeoutMs, sentAll, &whole, drainBudgetMs, &timer] {
                         if (*sentAll && whole.elapsed() < drainBudgetMs) {
                             // Тело ушло целиком, сервер дочитывает буфер:
                             // молчание здесь — не смерть канала.
                             timer.start(timeoutMs);
                             return;
                         }
                         // Пометка «оборвал сторож»: httpTrouble скажет про
                         // молчание канала, не безликое «Operation canceled».
                         reply->setProperty("zamettiStalledMs", timeoutMs);
                         reply->abort();  // abort сам приведёт к finished
                     });
    timer.start(timeoutMs);
    if (!reply->isFinished()) loop.exec();
}

}  // namespace

bool WebDavCloud::list(QVector<Entry>* out, QString* error) {
    Q_ASSERT(out != nullptr);
    ++traffic_.requests;
    QNetworkRequest request(config_.base);
    authorize(&request);
    request.setRawHeader("Depth", "1");
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/xml; charset=utf-8"));
    // Спрашиваем ровно четыре свойства: имя, размер, etag, дату. Allprop
    // заставил бы сервер считать и слать лишнее на каждый блоб. Дата — для
    // сводки «когда облако правили» в окне хранилищ; движку синка она не
    // судья (истина — хеш, mtime и etag — подсказки).
    const QByteArray body =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        "<d:propfind xmlns:d=\"DAV:\"><d:prop>"
        "<d:getcontentlength/><d:getetag/><d:getlastmodified/><d:resourcetype/>"
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
            } else if (name == QLatin1String("getlastmodified")) {
                // RFC 1123 («Sun, 30 Aug 2026 14:53:35 GMT») — обязательный
                // формат DAV. Разборщик Qt (RFC2822Date) ждёт ЧИСЛОВОЕ
                // смещение и зону словом не берёт — живой Apache отдавал
                // ровно « GMT», и дата молча пустела (пробник webdav).
                QString text = reader.readElementText().trimmed();
                if (text.endsWith(QLatin1String(" GMT"))) {
                    text.chop(4);
                    text += QStringLiteral(" +0000");
                }
                current.lastModified =
                    QDateTime::fromString(text, Qt::RFC2822Date).toUTC();
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

bool WebDavCloud::get(const QString& name, QByteArray* bytes, QString* etag,
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

// ПАКЕТНОЕ СКАЧИВАНИЕ: до kInFlight реплаев в полёте в ОДНОМ событийном
// цикле (QNetworkAccessManager это умеет, порядок завершения любой; поток —
// тот же рабочий, что и у остальных операций). Боль первой загрузки — сотни
// мелких блобов, каждый ценой RTT: последовательный прогон против живого
// сервера стоил ~0.6 с на запрос (замерено 28.08.2026). У каждого реплая
// свой сторож бездействия — контракт тот же, что у waitFor.
void WebDavCloud::getMany(const QStringList& names, QHash<QString, Fetched>* out) {
    Q_ASSERT(out != nullptr);
    if (names.isEmpty()) return;
    constexpr int kInFlight = 6;
    QEventLoop loop;
    int next = 0;
    int active = 0;
    int done = 0;
    std::function<void()> pump = [&] {
        while (next < int(names.size()) && active < kInFlight) {
            const QString name = names.at(next++);
            ++traffic_.requests;
            if (badName(name)) {
                Fetched bad;
                bad.error = QStringLiteral("webdav: bad blob name %1").arg(name);
                out->insert(name, bad);
                ++done;
                continue;
            }
            QNetworkRequest request(config_.base.resolved(QUrl(name)));
            authorize(&request);
            QNetworkReply* reply = impl_->net.get(request);
            ++active;
            auto* watchdog = new QTimer(reply);
            watchdog->setSingleShot(true);
            const int timeoutMs = config_.timeoutMs;
            QObject::connect(reply, &QNetworkReply::downloadProgress, watchdog,
                             [watchdog, timeoutMs](qint64, qint64) {
                                 watchdog->start(timeoutMs);
                             });
            QObject::connect(watchdog, &QTimer::timeout, reply, [reply, timeoutMs] {
                reply->setProperty("zamettiStalledMs", timeoutMs);
                reply->abort();
            });
            watchdog->start(timeoutMs);
            QObject::connect(reply, &QNetworkReply::finished, &loop, [&, reply, name] {
                Fetched one;
                if (reply->error() != QNetworkReply::NoError) {
                    one.error = httpTrouble(QStringLiteral("GET %1").arg(name), reply);
                } else {
                    one.bytes = reply->readAll();
                    one.ok = true;
                    traffic_.bytesDown += one.bytes.size();
                }
                out->insert(name, one);
                reply->deleteLater();
                --active;
                ++done;
                if (done == int(names.size())) {
                    loop.quit();
                    return;
                }
                pump();
            });
        }
    };
    pump();
    // Все имена могли оказаться негодными — тогда ждать нечего; обработчики
    // же реплаев раньше exec не бегут (события стоят в очереди).
    if (done < int(names.size())) loop.exec();
}

bool WebDavCloud::put(const QString& name, const QByteArray& bytes, QString* etag,
                       QString* error) {
    return putIfMatch(name, bytes, QString(), etag, nullptr, error);
}

bool WebDavCloud::putIfMatch(const QString& name, const QByteArray& bytes,
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
    // Размер тела — сторожу: после полной отправки ответ ждётся с допуском
    // на медленное вычитывание сервера (см. waitFor).
    waitFor(reply.get(), config_.timeoutMs, bytes.size());
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

bool WebDavCloud::del(const QString& name, QString* error) {
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

bool WebDavCloud::removeTree(QString* error) {
    // ОДИН запрос: DELETE по коллекции стирает её рекурсивно (RFC 4918
    // §9.6.1) — стирать сотни блобов по одному в разы медленнее (решение
    // владельца, 30.08.2026). 404 — облака и так нет, это удача.
    ++traffic_.requests;
    QNetworkRequest request(config_.base);
    authorize(&request);
    std::unique_ptr<QNetworkReply, void (*)(QNetworkReply*)> reply(
        impl_->net.deleteResource(request),
        [](QNetworkReply* r) { r->deleteLater(); });
    waitFor(reply.get(), config_.timeoutMs);
    const int code =
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (code == 404) return true;
    if (reply->error() != QNetworkReply::NoError) {
        if (error != nullptr)
            *error = httpTrouble(QStringLiteral("DELETE (collection)"), reply.get());
        return false;
    }
    return true;
}

bool WebDavCloud::mkdirOnce(QString* error) {
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
