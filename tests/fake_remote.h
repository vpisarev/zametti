// MemoryRemote — «облако» без диска вовсе (m17, сессия 3).
//
// Зачем второй фиктивный адаптер, когда есть FolderRemote: он проверяет, что
// движок и наборы не опираются на файловую природу удалённого хранилища.
// Каркас обязан допускать S3 без правки движка (бриф), а S3 — это не
// каталог; адаптер в памяти — самая дешёвая проверка этого обещания.
//
// Здесь же живёт КОНТРАКТ RemoteStore одной функцией: тот же набор проверок
// гоняется по FolderRemote, по MemoryRemote и по настоящему WebDAV. Реализации
// расходятся в мелочах (кто как называет etag), но обещания у них одни.

#ifndef ZAMETTI_TESTS_FAKE_REMOTE_H
#define ZAMETTI_TESTS_FAKE_REMOTE_H

#include "hash.h"
#include "remote_store.h"

#include <QMap>

#include "test_util.h"

namespace zt {

class MemoryRemote : public zametti::RemoteStore {
public:
    bool list(QVector<Entry>* out, QString* error = nullptr) override {
        Q_ASSERT(out != nullptr);
        ++traffic_.requests;
        if (!made_) {
            if (error != nullptr) *error = QStringLiteral("memory remote: no dir yet");
            return false;
        }
        out->clear();
        for (auto it = blobs_.constBegin(); it != blobs_.constEnd(); ++it) {
            Entry entry;
            entry.name = it.key();
            entry.size = it.value().size();
            entry.etag = etagOf(it.value());
            out->append(entry);
        }
        return true;
    }

    bool get(const QString& name, QByteArray* bytes, QString* etag,
             QString* error = nullptr) override {
        Q_ASSERT(bytes != nullptr);
        ++traffic_.requests;
        if (!blobs_.contains(name)) {
            if (error != nullptr)
                *error = QStringLiteral("memory remote: no such blob %1").arg(name);
            return false;
        }
        *bytes = blobs_.value(name);
        traffic_.bytesDown += bytes->size();
        if (etag != nullptr) *etag = etagOf(*bytes);
        return true;
    }

    bool put(const QString& name, const QByteArray& bytes, QString* etag,
             QString* error = nullptr) override {
        ++traffic_.requests;
        if (name.isEmpty() || name.contains(QLatin1Char('/'))) {
            if (error != nullptr)
                *error = QStringLiteral("memory remote: bad blob name %1").arg(name);
            return false;
        }
        blobs_.insert(name, bytes);
        traffic_.bytesUp += bytes.size();
        if (etag != nullptr) *etag = etagOf(bytes);
        return true;
    }

    bool putIfMatch(const QString& name, const QByteArray& bytes,
                    const QString& expectedEtag, QString* etag,
                    bool* preconditionFailed, QString* error = nullptr) override {
        const QString current =
            blobs_.contains(name) ? etagOf(blobs_.value(name)) : QString();
        if (current != expectedEtag) {
            ++traffic_.requests;
            if (preconditionFailed != nullptr) *preconditionFailed = true;
            if (error != nullptr)
                *error = QStringLiteral("memory remote: %1 changed under us").arg(name);
            return false;
        }
        if (preconditionFailed != nullptr) *preconditionFailed = false;
        return put(name, bytes, etag, error);
    }

    bool del(const QString& name, QString* error = nullptr) override {
        Q_UNUSED(error);
        ++traffic_.requests;
        blobs_.remove(name);   // идемпотентно, как везде
        return true;
    }

    bool mkdirOnce(QString* error = nullptr) override {
        Q_UNUSED(error);
        ++traffic_.requests;
        made_ = true;
        return true;
    }

protected:
    static QString etagOf(const QByteArray& bytes) {
        return QString::fromStdString(
                   zametti::hashOf(std::string(bytes.constData(), size_t(bytes.size())))
                       .hex())
            .left(32);
    }

    QMap<QString, QByteArray> blobs_;
    bool made_ = false;
};

// КОНТРАКТ адаптера: одни и те же обещания для всех реализаций. Каталог
// «сервера» должен быть пуст; функция за собой прибирает.
//
// conditionalPuts — умеет ли ЭТОТ адаптер условную заливку. Спрашивается
// прямо, а не угадывается: сервер, не умеющий If-Match, обязан МОЛЧА
// деградировать в put (дефолт RemoteStore), и проверять его надо другим
// ожиданием, а не «или так, или так» — такая проверка не умеет краснеть.
inline void checkRemoteContract(zametti::RemoteStore& remote, const char* who,
                                bool conditionalPuts = true) {
    using Entry = zametti::RemoteStore::Entry;
    const auto say = [who](const char* what) {
        return std::string(who) + ": " + what;
    };
    QString error;

    ZT_TRUE(say("mkdirOnce заводит каталог").c_str(), remote.mkdirOnce(&error));
    ZT_TRUE(say("mkdirOnce повторно — не беда").c_str(), remote.mkdirOnce(&error));

    QVector<Entry> listing;
    ZT_TRUE(say("листинг пустого каталога удаётся").c_str(), remote.list(&listing, &error));
    ZT_TRUE(say("и он пуст").c_str(), listing.isEmpty());

    const QByteArray body("байты блоба, довольно длинные для проверки");
    QString etag;
    ZT_TRUE(say("put").c_str(), remote.put(QStringLiteral("01aaaa.log"), body, &etag, &error));
    ZT_TRUE(say("сервер назвал etag").c_str(), !etag.isEmpty());

    ZT_TRUE(say("листинг видит блоб").c_str(), remote.list(&listing, &error));
    ZT_TRUE(say("ровно один").c_str(), listing.size() == 1);
    if (listing.size() == 1) {
        ZT_EQ(say("имя").c_str(), "01aaaa.log", listing.first().name.toStdString());
        ZT_TRUE(say("размер").c_str(), listing.first().size == body.size());
        ZT_EQ(say("etag листинга — тот же, что у put").c_str(), etag.toStdString(),
              listing.first().etag.toStdString());
    }

    QByteArray back;
    QString getEtag;
    ZT_TRUE(say("get").c_str(),
            remote.get(QStringLiteral("01aaaa.log"), &back, &getEtag, &error));
    ZT_TRUE(say("скачано побайтово то же").c_str(), back == body);
    ZT_EQ(say("etag у get тот же").c_str(), etag.toStdString(), getEtag.toStdString());

    // Перезаливка другого содержимого меняет etag; того же — не меняет.
    QString etag2;
    ZT_TRUE(say("перезаливка другим").c_str(),
            remote.put(QStringLiteral("01aaaa.log"), QByteArray("другое тело"), &etag2, &error));
    ZT_TRUE(say("etag изменился").c_str(), etag2 != etag);
    QString etag3;
    ZT_TRUE(say("перезаливка тем же").c_str(),
            remote.put(QStringLiteral("01aaaa.log"), QByteArray("другое тело"), &etag3, &error));
    ZT_EQ(say("etag тот же").c_str(), etag2.toStdString(), etag3.toStdString());

    // Условная заливка: со «своим» etag проходит, с чужим — отказ по условию,
    // и это НЕ ошибка связи.
    QString etag4;
    bool clash = true;
    ZT_TRUE(say("putIfMatch со своим etag").c_str(),
            remote.putIfMatch(QStringLiteral("01aaaa.log"), QByteArray("третье"),
                              etag3, &etag4, &clash, &error));
    ZT_TRUE(say("столкновения не было").c_str(), !clash);
    QString etag5;
    clash = false;
    const bool stale = remote.putIfMatch(QStringLiteral("01aaaa.log"),
                                         QByteArray("четвёртое"), etag3, &etag5,
                                         &clash, &error);
    QByteArray afterStale;
    ZT_TRUE(say("get после устаревшего putIfMatch").c_str(),
            remote.get(QStringLiteral("01aaaa.log"), &afterStale, nullptr, &error));
    if (conditionalPuts) {
        ZT_TRUE(say("putIfMatch с устаревшим etag отказан").c_str(), !stale);
        ZT_TRUE(say("и назван столкновением, а не бедой связи").c_str(), clash);
        ZT_TRUE(say("содержимое сервера не тронуто").c_str(),
                afterStale == QByteArray("третье"));
    } else {
        // Деградация — тоже обещание: не отказ, а обычная заливка, молча.
        ZT_TRUE(say("без If-Match putIfMatch деградирует в put").c_str(), stale);
        ZT_TRUE(say("столкновением это не зовётся").c_str(), !clash);
        ZT_TRUE(say("и содержимое заменено").c_str(),
                afterStale == QByteArray("четвёртое"));
    }

    // Чего нет — того нет; но пустота каталога и обрыв связи различимы.
    ZT_TRUE(say("get несуществующего — отказ").c_str(),
            !remote.get(QStringLiteral("01нетуб.log"), &back, nullptr, &error));
    ZT_TRUE(say("и объяснён").c_str(), !error.isEmpty());

    ZT_TRUE(say("del").c_str(), remote.del(QStringLiteral("01aaaa.log"), &error));
    ZT_TRUE(say("del повторно — не беда").c_str(),
            remote.del(QStringLiteral("01aaaa.log"), &error));
    ZT_TRUE(say("после del листинг пуст").c_str(), remote.list(&listing, &error));
    ZT_TRUE(say("совсем пуст").c_str(), listing.isEmpty());

    // Имена с путём внутрь не пускаются. Проверяем именем, которым МОЖНО
    // сбежать вверх (родительский каталог существует, запись бы удалась):
    // «под/каталог.log» без стража тоже провалится — потому что подкаталога
    // нет, — и такая проверка не умеет краснеть.
    ZT_TRUE(say("имя, ведущее вверх, отвергнуто").c_str(),
            !remote.put(QStringLiteral("../сбежал.log"), body, nullptr, &error));
    ZT_TRUE(say("имя со слэшем отвергнуто").c_str(),
            !remote.put(QStringLiteral("под/каталог.log"), body, nullptr, &error));

    ZT_TRUE(say("счётчик запросов рос").c_str(), remote.traffic().requests > 0);
    ZT_TRUE(say("счётчик залитых байтов рос").c_str(), remote.traffic().bytesUp > 0);
    ZT_TRUE(say("счётчик скачанных байтов рос").c_str(), remote.traffic().bytesDown > 0);
}

}  // namespace zt

#endif  // ZAMETTI_TESTS_FAKE_REMOTE_H
