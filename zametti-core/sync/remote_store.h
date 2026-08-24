// Адаптер удалённого хранилища (m17, сессия 3): интерфейс и его реализации.
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

#ifndef ZAMETTI_SYNC_REMOTE_STORE_H
#define ZAMETTI_SYNC_REMOTE_STORE_H

#include <QString>
#include <QVector>

namespace zametti {

class RemoteStore {
public:
    struct Entry {
        QString name;
        qint64 size = 0;
        QString etag;      // подсказка сервера; пусто — сервер её не дал
    };

    // Сколько это стоило по проводу. Запросы считаются все, включая неудачные.
    struct Traffic {
        qint64 requests = 0;
        qint64 bytesUp = 0;
        qint64 bytesDown = 0;
    };

    virtual ~RemoteStore() = default;

    // Всё, что лежит в удалённом каталоге. Ложь — не дошли (объяснение в
    // error); пустой список — дошли, а там пусто. Эти два случая разные, и
    // путать их нельзя: пустота значила бы «удалить всё локальное».
    virtual bool list(QVector<Entry>* out, QString* error = nullptr) = 0;

    // Скачать. etag может быть nullptr, если он не нужен.
    virtual bool get(const QString& name, QByteArray* bytes, QString* etag,
                     QString* error = nullptr) = 0;

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

    const Traffic& traffic() const { return traffic_; }
    void resetTraffic() { traffic_ = Traffic(); }

protected:
    Traffic traffic_;
};

}  // namespace zametti

#endif  // ZAMETTI_SYNC_REMOTE_STORE_H
