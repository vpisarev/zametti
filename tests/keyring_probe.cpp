// Ручная приёмка keyring (m17, сессия 3): записать / прочитать / удалить под
// живым Secret Service (GNOME Keyring, KWallet).
//
// Это НЕ набор: живой keyring — вещь машины, не проверки, и гонять его в
// zametti-tests значило бы гадить в связку пользователя при каждом прогоне.
// Пробник печатает каждый шаг; САМИ СЕКРЕТЫ НЕ ПЕЧАТАЕТ.
//
//   zametti-bench keyring [--store-id <id>]
//
// Работает на выдуманном storeId (дефолт ниже) и подчищает за собой.

#include "blob_cipher.h"
#include "keyring_secrets.h"

#include <cstdio>
#include <cstring>

int ztKeyringProbe(int argc, char** argv);

namespace {

int fail(const char* what, const QString& error) {
    std::printf("ПРОВАЛ: %s: %s\n", what, error.toUtf8().constData());
    return 1;
}

}  // namespace

int ztKeyringProbe(int argc, char** argv) {
    using zametti::BlobAad;
    using zametti::BlobKind;
    using zametti::Keyfile;
    using zametti::KeyringSecrets;
    using zametti::XChaChaCipher;

    QString storeId = QStringLiteral("01keyringprobe");
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--store-id") == 0 && i + 1 < argc)
            storeId = QString::fromUtf8(argv[++i]);

    KeyringSecrets keyring;
    std::printf("служба секретов: %s\n",
                keyring.available() ? "доступна" : "НЕДОСТУПНА");
    if (!keyring.available()) return 1;

    QString error;

    // Ключ: родить, положить, достать, сверить через шифр, убрать.
    Keyfile made;
    if (!Keyfile::create(storeId, "пароль-пробника", {1, quint64(8) << 20},
                         &made, &error))
        return fail("create", error);
    QByteArray blob;
    if (!XChaChaCipher::make(made, &error)
             ->seal("проба", BlobAad{BlobKind::Journal, storeId, "probe.log"},
                    &blob, &error))
        return fail("seal", error);
    std::printf("ключ: рождён и запечатал пробный блоб\n");

    if (!keyring.storeKey(made, &error)) return fail("storeKey", error);
    std::printf("ключ: уложен в связку (метка zametti-key-%s)\n",
                storeId.toUtf8().constData());

    Keyfile loaded;
    if (!keyring.loadKey(storeId, &loaded, &error)) return fail("loadKey", error);
    QByteArray plain;
    if (!XChaChaCipher::make(loaded, &error)
             ->open(blob, BlobAad{BlobKind::Journal, storeId, "probe.log"},
                    &plain, &error) ||
        plain != "проба")
        return fail("ключ из связки не вскрыл печать исходного", error);
    std::printf("ключ: прочитан из связки, вскрывает печать исходного\n");

    if (!keyring.clearKey(storeId, &error)) return fail("clearKey", error);
    if (keyring.loadKey(storeId, &loaded, &error)) {
        std::printf("ПРОВАЛ: ключ читается после удаления\n");
        return 1;
    }
    std::printf("ключ: удалён, повторное чтение честно отказывает\n");

    // Пароль сервера: положить, достать, убрать.
    if (!keyring.setServerPassword(storeId, QStringLiteral("проба-пароля"), &error))
        return fail("setServerPassword", error);
    if (keyring.serverPassword(storeId, &error) != QStringLiteral("проба-пароля"))
        return fail("serverPassword вернул не то", error);
    std::printf("пароль сервера: уложен и прочитан тем же\n");
    if (!keyring.clearServerPassword(storeId, &error))
        return fail("clearServerPassword", error);
    if (!keyring.serverPassword(storeId, &error).isEmpty()) {
        std::printf("ПРОВАЛ: пароль читается после удаления\n");
        return 1;
    }
    std::printf("пароль сервера: удалён, повторное чтение честно отказывает\n");

    std::printf("keyring: приёмка пройдена\n");
    return 0;
}
