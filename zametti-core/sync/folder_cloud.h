// FolderCloud — «облако» в локальном каталоге (m17, сессия 3).
//
// Не эмулятор ради эмулятора: он делает две работы сразу.
//
//   1. НАБОРЫ. Весь движок синхронизации проверяется без сети и без
//      контейнеров, на порядки быстрее; настоящий WebDAV остаётся ПРИЁМКОЙ,
//      а не условием запуска (решение владельца).
//   2. РУБИЛЬНИКИ. Обрыв на середине заливки, отказ листинга, перевыдача
//      etag без изменения содержимого — всё это на живом сервере не
//      воспроизвести по заказу, а движок обязан их переживать.
//
// Счётчики операций — под требование брифа мерить синк числами: «устойчивый
// синк = 1 листинг, 0 чтений содержимого, 0 шифрований».
//
// etag здесь честный: BLAKE3 содержимого (плюс соль, если её накрутили
// рубильником). Такой etag стабилен и меняется ровно с содержимым — то есть
// ведёт себя как ЛУЧШИЙ мыслимый сервер; беды нестабильных etag имитируются
// нарочно, setEtagSalt.

#ifndef ZAMETTI_SYNC_FOLDER_CLOUD_H
#define ZAMETTI_SYNC_FOLDER_CLOUD_H

#include "cloud_store.h"
#include "zsystem.h"

#include <QHash>
#include <QString>

namespace zametti {

class FolderCloud : public CloudStore {
public:
    explicit FolderCloud(const QString& dir);

    bool list(QVector<Entry>* out, QString* error = nullptr) override;
    bool get(const QString& name, QByteArray* bytes, QString* etag,
             QString* error = nullptr) override;
    bool put(const QString& name, const QByteArray& bytes, QString* etag,
             QString* error = nullptr) override;
    bool putIfMatch(const QString& name, const QByteArray& bytes,
                    const QString& expectedEtag, QString* etag,
                    bool* preconditionFailed, QString* error = nullptr) override;
    bool del(const QString& name, QString* error = nullptr) override;
    bool mkdirOnce(QString* error = nullptr) override;

    // --- то, чего у настоящего сервера не спросишь ---

    struct Counters {
        int lists = 0, gets = 0, puts = 0, dels = 0, mkdirs = 0;
    };
    const Counters& counters() const { return counters_; }
    void resetCounters() { counters_ = Counters(); }

    // Следующие `times` вызовов операции `op` ("list", "get", "put", "del",
    // "mkdir") провалятся с внятной ошибкой. Так проверяется, что прогон
    // переживает обрыв в любой точке и достраивается следующим.
    void failNext(const QString& op, int times = 1);

    // Перевыдача etag без изменения содержимого — болячка WebDAV. Соль
    // подмешивается в etag, содержимое не трогается: движок обязан на этом
    // не заливать заново (он сверит хеш).
    void setEtagSalt(int salt) { etagSalt_ = salt; }

    const QString& dir() const { return dir_; }

protected:
    // Разрушать в каталоге-облаке — только через дверь, и область та же, что
    // сам каталог. Отдаётся значением: dir_ не меняется, но и разъехаться с
    // областью тогда нечему. АДРЕС ОБЛАКА ОТНОСИТЕЛЬНЫМ НЕ БЫВАЕТ — набранное
    // «../..» стоило владельцу каталога, — и здесь это видно делом: с
    // относительным dir_ область не состоится и не удалится ничего.
    ZSystem files() const { return ZSystem(ZSystem::Area::Cloud, dir_); }

    // Путь файла блоба; пусто — имя негодное (со слэшем, "..", пустое).
    QString pathOf(const QString& name) const;
    // Съесть один рубильник, если он взведён.
    bool tripped(const QString& op, QString* error);
    QString etagOf(const QByteArray& bytes) const;

    QString dir_;
    Counters counters_;
    QHash<QString, int> failures_;
    int etagSalt_ = 0;
};

}  // namespace zametti

#endif  // ZAMETTI_SYNC_FOLDER_CLOUD_H
