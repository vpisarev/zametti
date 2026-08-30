// Адаптер облачного хранилища (m17, сессия 3): интерфейс и его реализации.
//
// ПЯТЬ ОПЕРАЦИЙ, и больше ничего (бриф, часть 2): list, get, put, del,
// mkdirOnce. Каталогов нет — плоское пространство имён, как и у самого
// хранилища; имена блобов открытые (id непрозрачны). Про заметки, картинки
// и шифрование адаптер не знает вовсе: он возит байты.
//
// Каркас обязан допускать S3 без правки движка — поэтому здесь нет ни
// PROPFIND, ни etag-семантики WebDAV: etag это просто «метка версии, какой
// её назвал сервер», и она ПОДСКАЗКА (нестабильные etag — известная болячка
// WebDAV, переживается ярусом хешей).
//
// Счётчик трафика — не украшение: бриф требует мерить синк счётчиками
// («устойчивый синк = 1 листинг, 0 чтений содержимого»), и мерить их должен
// тот, кто ходит по проводу.

#ifndef ZAMETTI_SYNC_CLOUD_STORE_H
#define ZAMETTI_SYNC_CLOUD_STORE_H

#include <QDateTime>
#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>

namespace zametti {

class CloudStore {
public:
    struct Entry {
        QString name;
        qint64 size = 0;
        QString etag;             // подсказка сервера; пусто — сервер её не дал
        QDateTime lastModified;   // метка сервера, UTC; невалидна — не назвал
    };

    // Сколько это стоило по проводу. Запросы считаются все, включая неудачные.
    struct Traffic {
        qint64 requests = 0;
        qint64 bytesUp = 0;
        qint64 bytesDown = 0;
    };

    virtual ~CloudStore() = default;

    // Всё, что лежит в удалённом каталоге. Ложь — не дошли (объяснение в
    // error); пустой список — дошли, а там пусто. Эти два случая разные, и
    // путать их нельзя: пустота значила бы «удалить всё локальное».
    virtual bool list(QVector<Entry>* out, QString* error = nullptr) = 0;

    // Скачать. etag может быть nullptr, если он не нужен.
    virtual bool get(const QString& name, QByteArray* bytes, QString* etag,
                     QString* error = nullptr) = 0;

    // ПАКЕТНОЕ СКАЧИВАНИЕ: несколько блобов разом, чтобы канал не простаивал
    // по кругу «запрос — ответ» (боль первой загрузки: сотни мелких блобов,
    // и каждый стоил RTT — 0.6 с на запрос к живому серверу, замерено).
    // Провал ОДНОГО имени — не провал пакета: итог по каждому отдельно.
    // Дефолт — последовательный цикл get; сетевые адаптеры держат несколько
    // запросов в полёте.
    struct Fetched {
        QByteArray bytes;
        bool ok = false;
        QString error;
    };
    virtual void getMany(const QStringList& names, QHash<QString, Fetched>* out);

    // Залить целиком (перезаписав). Отдаёт новый etag, если сервер его назвал.
    virtual bool put(const QString& name, const QByteArray& bytes, QString* etag,
                     QString* error = nullptr) = 0;

    // Залить, только если удалённая версия — та, которую мы видели. Сервер,
    // не умеющий If-Match, МОЛЧА деградирует в обычный put: дефолтная
    // реализация ниже делает именно это. preconditionFailed — «сервер сказал
    // нет, там уже другое»; ошибкой это не считается.
    virtual bool putIfMatch(const QString& name, const QByteArray& bytes,
                            const QString& expectedEtag, QString* etag,
                            bool* preconditionFailed, QString* error = nullptr);

    virtual bool del(const QString& name, QString* error = nullptr) = 0;

    // Завести удалённый каталог, если его ещё нет. Идемпотентно и дёшево:
    // зовётся раз за прогон, «уже есть» — не ошибка.
    virtual bool mkdirOnce(QString* error = nullptr) = 0;

    // СНЕСТИ ВСЁ ОБЛАКО ОДНИМ ЖЕСТОМ (стирание, §2.6): после удачи в облаке
    // не остаётся ни одного объекта. Дефолт — листинг и удаление по одному
    // (годится и S3-подобным без каталогов); WebDAV делает это ОДНИМ запросом
    // DELETE по коллекции — стирать сотни блобов по одному в разы медленнее
    // (решение владельца, 30.08.2026). Сам каталог после удачи может как
    // исчезнуть, так и остаться пустым — это сила адаптера, не обещание;
    // кому нужен пустой, зовёт mkdirOnce следом. Идемпотентно: пустое или
    // отсутствующее облако — уже удача.
    virtual bool removeTree(QString* error = nullptr);

    const Traffic& traffic() const { return traffic_; }
    void resetTraffic() { traffic_ = Traffic(); }

protected:
    Traffic traffic_;
};

}  // namespace zametti

#endif  // ZAMETTI_SYNC_CLOUD_STORE_H
