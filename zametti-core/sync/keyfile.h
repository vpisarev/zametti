// Keyfile — конверт мастер-ключа облака и ЕДИНСТВЕННЫЙ носитель самого ключа
// в программе (решение владельца, сессия 3 этапа 17).
//
// В облаке лежит этот файл: мастер-ключ (32 случайных байта), завёрнутый
// ключом из пароля — Argon2id(пароль, соль) → XChaCha20-Poly1305. Параметры
// Argon2id живут В ФАЙЛЕ рядом с солью: будущие устройства читают их оттуда,
// а не из констант — поэтому дефолты можно менять, не ломая старые keyfile.
//
// Формат — JSON + base64, как zametti.json: параметры открыты и читаются
// глазами, бинарное (соль, nonce, шифротекст ключа) в base64. Незнакомые
// ключи внутри своей версии переживают перезапись; версия новее нашей —
// вежливый отказ разворота, а не порча.
//
// ЖИВОЙ КЛЮЧ (после create/unwrap) тоже живёт здесь, и голым QByteArray по
// программе не ходит: шифр и keyring получают Keyfile и спрашивают байты
// через единственную protected-дверь key(). Обнуляются байты ровно один раз —
// когда умирает последняя копия (Keyfile копируется, шифр держит свою).
//
// ЧИСТОЕ ЗНАЧЕНИЕ, БЕЗ ФАЙЛОВ (как ZStorage::Identity): разбор и сборка
// здесь, а где байты лежат — на сервере, в keyring — знают другие.

#ifndef ZAMETTI_SYNC_KEYFILE_H
#define ZAMETTI_SYNC_KEYFILE_H

#include <QByteArray>
#include <QJsonObject>
#include <QString>

#include <memory>

namespace zametti {

class Keyfile {
public:
    // Версия ФОРМАТА КОНВЕРТА; входит в AAD заворота, так что откат версии в
    // тексте файла валит тег, а не тихо меняет разбор.
    static constexpr int kVersion = 1;
    static constexpr int kKeyBytes = 32;
    // Имя блоба в облаке — здесь один раз. Лежит открытым текстом рядом с
    // шифрованными блобами: сверка etag работает до ввода пароля.
    static constexpr char kRemoteName[] = "keyfile";

    struct KdfParams {
        quint64 opslimit = 0;        // проходов Argon2id
        quint64 memlimitBytes = 0;   // память на один разворот
    };
    // Дефолты создания — порог владельца (24.08.2026, калибровка
    // `zametti-bench argon2` на его машине: 512 МиБ × 4 прохода, ~1.5 с;
    // «раз в два-три года можно и подождать»). Разворот параметры НЕ читает —
    // он берёт их из самого файла.
    static KdfParams defaults();

    Keyfile() = default;
    Keyfile(const Keyfile&) = default;
    Keyfile& operator=(const Keyfile&) = default;
    ~Keyfile();

    // Рождение: новый случайный ключ, сразу завёрнутый паролем. После удачи
    // ключ живой (hasKey), конверт готов к отправке (toBytes).
    static bool create(const QString& storeId, const QString& password,
                       const KdfParams& params, Keyfile* out,
                       QString* error = nullptr);

    // Разбор байтов конверта. Ключ после разбора ещё СПИТ — его будит unwrap.
    // Версия новее нашей разбирается (tooNew станет истиной), но не будится.
    bool parse(const QByteArray& bytes, QString* error = nullptr);

    // Байты конверта для сервера. Живой ключ в них НЕ уезжает. Keyfile без
    // конверта (из keyring) собирать нечего — пустой ответ.
    QByteArray toBytes() const;

    // Разворот: Argon2id по параметрам ИЗ ФАЙЛА, отказ AEAD = неверный пароль
    // или порча (различить их нельзя по построению). Удача будит ключ.
    bool unwrap(const QString& password, QString* error = nullptr);

    // Смена пароля: тот же ключ, новая соль, новый nonce, новые параметры.
    // Если ключ ещё спит, сперва будится старым паролем.
    bool rewrap(const QString& oldPassword, const QString& newPassword,
                const KdfParams& params, QString* error = nullptr);

    bool isEmpty() const { return storeId_.isEmpty(); }
    bool hasKey() const;
    const QString& storeId() const { return storeId_; }
    const QString& created() const { return created_; }
    int version() const { return version_; }
    KdfParams params() const { return params_; }
    // Формат новее нашего: не работать, а не портить.
    bool tooNew() const { return version_ > kVersion; }

protected:
    // Живой ключ за shared_ptr: копий Keyfile много, буфер один, и обнулить
    // его надо ровно один раз — в смерти последней копии.
    struct LiveKey;

    // Единственная дверь к байтам ключа. Друзья — те, кому ключ нужен по
    // работе: шифр и хранилища секретов. Наружу байты не выходят.
    QByteArray key() const;
    // Keyfile из keyring: живой ключ без конверта (конверт живёт на сервере).
    static Keyfile fromLiveKey(const QString& storeId, const QByteArray& keyBytes);

    friend class XChaChaCipher;
    friend class EnvSecrets;
    friend class KeyringSecrets;
    friend class BundledSecrets;   // правила одной записи-свёртка

    int version_ = kVersion;
    QString storeId_;
    QString created_;
    KdfParams params_;
    QByteArray salt_;      // crypto_pwhash_SALTBYTES
    QByteArray nonce_;     // 24 байта XChaCha
    QByteArray wrapped_;   // шифротекст ключа с приклеенным тегом
    QJsonObject extra_;    // незнакомые ключи — едут обратно в файл
    std::shared_ptr<LiveKey> live_;
};

}  // namespace zametti

#endif  // ZAMETTI_SYNC_KEYFILE_H
