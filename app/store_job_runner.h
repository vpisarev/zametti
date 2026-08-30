// StoreJobRunner — работы окна хранилищ: синхронно, без виджетов (§2.5).
//
// Исполняет Job модели и возвращает Outcome; ни потоков, ни Qt-виджетов —
// окно уводит run() в рабочий поток само, наборы зовут его прямо.
//
// ДИАЛОГ НЕ КАЧАЕТ И НЕ ЛЬЁТ НИЧЕГО, КРОМЕ ГОЛОВЫ ХРАНИЛИЩА (решение
// владельца, 30.08.2026): манифест, конверт, cloud.json. Вся синхронизация
// содержимого идёт ПОСЛЕ нажатия Open и закрытия окна обычным прогоном
// (SyncController); отдельного кода выкачки здесь не существует.
//
// СВЯЗКА СЮДА ПРИХОДИТ КОПИЛКОЙ (TakenSecrets у окна): настоящий keyring
// живёт при главном потоке. Ключ, если он нужен работе (смена пароля), окно
// подсаживает в копилку ДО запуска; добытое работой окно перекладывает в
// настоящую связку по завершении, включая просьбы забыть (disconnect).

#ifndef ZAMETTI_STORE_JOB_RUNNER_H
#define ZAMETTI_STORE_JOB_RUNNER_H

#include "store_manager_model.h"

#include "keyfile.h"

namespace zametti {

class SecretStore;

class StoreJobRunner {
public:
    // mintParams — параметры чеканки ключа; наборам боевой Argon2id не нужен.
    explicit StoreJobRunner(const Keyfile::KdfParams& mintParams = Keyfile::defaults())
        : mintParams_(mintParams) {}

    // Секреты в job уже настоящие (окно развернуло «взять из связки» до
    // запуска); secrets — копилка для добытого.
    StoreManagerModel::Outcome run(const StoreManagerModel::Job& job,
                                   SecretStore& secrets);

protected:
    StoreManagerModel::Outcome check(const StoreManagerModel::Job& job);
    StoreManagerModel::Outcome create(const StoreManagerModel::Job& job,
                                      SecretStore& secrets);
    StoreManagerModel::Outcome changePassword(const StoreManagerModel::Job& job,
                                              SecretStore& secrets);
    StoreManagerModel::Outcome eraseAndReseed(const StoreManagerModel::Job& job,
                                              SecretStore& secrets);
    StoreManagerModel::Outcome eraseAndDisconnect(const StoreManagerModel::Job& job,
                                                  SecretStore& secrets);

    Keyfile::KdfParams mintParams_;
};

}  // namespace zametti

#endif  // ZAMETTI_STORE_JOB_RUNNER_H
