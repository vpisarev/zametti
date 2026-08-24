// Шифр блобов облака (m17, сессия 3): интерфейс и единственная реализация.
//
// Всё, что уезжает в облако шифрованным (журнал, вложение), едет ОДНИМ
// видом обёртки:
//
//   "ZBLB" (4) | версия обёртки (1) | nonce (24) | шифротекст с тегом (n+16)
//
// XChaCha20-Poly1305, случайный 24-байтовый nonce на КАЖДУЮ печать — потому
// повторная заливка того же содержимого даёт другие байты, и повторение
// nonce исключено рождением, а не бухгалтерией.
//
// AAD — тип блоба, storeId и имя блоба (плюс версия обёртки, которую ставит
// сам шифр): скопировать чужой валидный блоб на место другого — под другим
// именем, в другое хранилище, под видом другого типа или со сброшенной
// версией — не выйдет, тег не сойдётся. Защита стоит ноль байт трафика.
//
// Интерфейс — чтобы шифр можно было сменить (решение владельца); ключ
// приходит и хранится ТОЛЬКО как Keyfile (решение владельца, ревью плана).

#ifndef ZAMETTI_SYNC_BLOB_CIPHER_H
#define ZAMETTI_SYNC_BLOB_CIPHER_H

#include "keyfile.h"

#include <QByteArray>
#include <QString>

#include <memory>

namespace zametti {

enum class BlobKind { Journal, Attachment };

// Кому принадлежит блоб. Версию обёртки сюда НЕ кладут: её единственный
// писатель — шифр (печать ставит свою, вскрытие берёт из заголовка блоба).
struct BlobAad {
    BlobKind kind = BlobKind::Journal;
    QString storeId;
    QString name;                       // имя блоба на сервере, напр. "<id>.log"
};

class BlobCipher {
public:
    virtual ~BlobCipher() = default;

    // Печать: plaintext → блоб. Ложь — только беда рождения (нет памяти).
    virtual bool seal(const QByteArray& plain, const BlobAad& aad,
                      QByteArray* blob, QString* error = nullptr) const = 0;

    // Вскрытие: блоб → plaintext. Ложь — порча, подмена, чужой ключ или
    // downgrade: блоб отвергается ЦЕЛИКОМ, «чуть-чуть другого» текста не
    // бывает.
    virtual bool open(const QByteArray& blob, const BlobAad& aad,
                      QByteArray* plain, QString* error = nullptr) const = 0;
};

class XChaChaCipher : public BlobCipher {
public:
    static constexpr int kWrapVersion = 1;
    // Байты обёртки помимо шифротекста: магия + версия + nonce + тег.
    static constexpr int kOverhead = 4 + 1 + 24 + 16;

    // Фабрика — единственная дверь: проверяет, что ключ живой, и отдаёт
    // владение shared_ptr (решение владельца). Пусто — объяснение в error.
    static std::shared_ptr<BlobCipher> make(const Keyfile& keyfile,
                                            QString* error = nullptr);

    bool seal(const QByteArray& plain, const BlobAad& aad,
              QByteArray* blob, QString* error = nullptr) const override;
    bool open(const QByteArray& blob, const BlobAad& aad,
              QByteArray* plain, QString* error = nullptr) const override;

protected:
    explicit XChaChaCipher(const Keyfile& keyfile);

    // Ключ хранится КАК Keyfile, не байтами (решение владельца).
    Keyfile keyfile_;
};

}  // namespace zametti

#endif  // ZAMETTI_SYNC_BLOB_CIPHER_H
