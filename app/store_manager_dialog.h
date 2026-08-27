// Диалог управления хранилищами — дверь кнопки database (решение владельца,
// 27.08.2026: прежний системный выбор каталога стал частью этого окна).
//
// Слева СПИСОК хранилищ устройства (ZAppState::stores, строка —
// ZStorage::Config): «+» добавляет, «−» забывает строку (папку и облако не
// трогая; у открытого хранилища «−» погашен — строка вернулась бы при
// следующем прицеплении), Open (и двойной клик) — переключиться. Справа —
// ФОРМА выбранной строки: папка, адрес облака (url или каталог), логин и оба
// пароля. Это ввод данных, не подтверждение — принципу «никаких диалогов» не
// противоречит (бриф m17).
//
// Что умеет форма, решает состояние папки и облака (все ветки — в ядре):
//   * папка-хранилище + криптопароль      → connectRemote (пустое облако
//     чеканит ключ — пароль спрашивается дважды: опечатка запечатала бы его
//     навсегда; чужое облако — честный отказ ядра с ярлыками);
//   * папка-хранилище, пароль пуст, ключ в keyring → неразрушительное
//     обновление адреса/логина/пароля сервера (setRemote + writeRemoteConfig);
//   * пустая папка + облако               → probeCloud ДО скачивания (человек
//     не выбирает папку под хранилище, которого не окажется) → initFromRemote;
//   * пустая папка без облака             → «завести новое хранилище?» —
//     второе санкционированное исключение из «никаких подтверждений»;
//   * занятая посторонним папка           → отказ словами, до ядра.
//
// СБРОС ПАРОЛЯ ШИФРОВАНИЯ (пароль забыт: конверт без пароля не развернуть по
// построению) — единственная стирающая операция над облаком, потому
// единственная здесь С ПЕРЕСПРОСОМ — исключение, названное владельцем в
// брифе этой сессии, как «удалить насовсем» у Архива.
//
// ПОТОКИ РАЗДЕЛЕНЫ ПО ПРИРОДЕ ВЕЩЕЙ, как в SyncController: сеть и Argon2id
// (~секунда на разворот — порог владельца) бегут в рабочем потоке, форма на
// это время глохнет, но окно живёт. Keyring (DBus при главном цикле) диалог
// трогает ТОЛЬКО из главного потока: рабочему отдаётся приёмник-копилка
// (TakenSecrets), добытое перекладывается в keyring по завершении.

#ifndef ZAMETTI_STORE_MANAGER_DIALOG_H
#define ZAMETTI_STORE_MANAGER_DIALOG_H

#include "keyfile.h"
#include "secret_store.h"
#include "zstorage.h"

#include <QDialog>
#include <QList>
#include <QString>

#include <functional>
#include <memory>
#include <thread>

class QCheckBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;

namespace zametti {

class StoreManagerDialog : public QDialog {
    Q_OBJECT

public:
    // Итог всего окна. Список применяется ВСЕГДА (и по Close, и по Esc):
    // добавленное хранилище — не черновик, терять его молча нельзя.
    struct Result {
        QList<ZStorage::Config> stores;   // итоговый список устройства
        QString switchToRoot;             // непусто — переключиться сюда
        // Облако ОТКРЫТОГО хранилища настроили или сменили: окну стоит
        // запустить прогон (сброс пароля сюда не входит — он уже всё залил).
        bool cloudChangedForCurrent = false;
        // switchToRoot — только что скачанное хранилище: там пока манифест и
        // корень, остальное обязан привезти первый прогон — независимо от
        // настройки sync.onStart.
        bool downloadedNew = false;
    };

    // stores — строки списка (копия; итог в result().stores); currentRoot —
    // корень открытого хранилища (пусто — окно без хранилища); storage — его
    // живой объект: облако ОТКРЫТОГО настраивается на нём, чужие корни
    // открываются временным ZStorage под замком. secrets — keyring (или среда
    // у обвязки), трогается только в главном потоке. mintParams — параметры
    // чеканки ключа; наборам боевой Argon2id не нужен.
    StoreManagerDialog(QWidget* parent, const QList<ZStorage::Config>& stores,
                       const QString& currentRoot, std::shared_ptr<ZStorage> storage,
                       std::shared_ptr<SecretStore> secrets,
                       const Keyfile::KdfParams& mintParams = Keyfile::defaults());
    ~StoreManagerDialog() override;

    // ВАЖНО ВЫЗЫВАЮЩЕМУ: перед attachStore на switchToRoot диалог должен
    // умереть — он держит копию shared_ptr открытого хранилища, а сторож
    // забытой копии в attachStore валит отладочную сборку.
    const Result& result() const { return result_; }

    // Адрес облака из строки формы: http(s):// — WebDAV (с хвостовым «/»,
    // как у CLI), пустая строка — облака нет, всё прочее — каталог. Чистая
    // функция, проверяется набором.
    static void setCloudAddress(ZStorage::Config& cfg, const QString& server,
                                const QString& user, bool allowInsecureHttp);
    // Обратная сторона: что показать в поле адреса.
    static QString cloudAddressText(const ZStorage::Config& cfg);

protected:
    // Занятое окно не закрывается: рабочий поток держит хранилище, его
    // дожидаются, а не бросают.
    void reject() override;

    void rebuildList(int selectRow);
    void showEntry(int row);
    void beginNewEntry();
    void forgetSelected();
    void openSelected();
    void onApply();
    // Переспрос (единственный здесь) — и включение режима сброса; наборы
    // входят в режим мимо модального вопроса, через armResetMode.
    void enterResetMode();
    void armResetMode();
    void leaveResetMode();

    // Ветки onApply; cfg — уже собранный из формы адрес (root заполнен).
    void applyToStore(const ZStorage::Config& cfg, const QString& serverPassword,
                      const QString& password);
    void addFromCloud(const ZStorage::Config& cfg, const QString& serverPassword,
                      const QString& password);
    void resetPassword(const ZStorage::Config& cfg, const QString& serverPassword,
                       const QString& password);
    // Строка легла в список (и в форму); вернувшееся из работы имя — тоже.
    void settleEntry(const ZStorage::Config& entry);

    // Открытое хранилище? — тогда работать на живом объекте, чужой корень —
    // временным ZStorage под замком (внутри job, в рабочем потоке).
    bool isCurrentRoot(const QString& root) const;

    // Работа в рабочем потоке: job возвращает пустую строку при удаче, done
    // зовётся в главном. Результаты job складывает в поля — к ним никто не
    // прикасается, пока поток жив.
    void startWork(const QString& status, std::function<QString()> job,
                   std::function<void(const QString&)> done);
    void setBusy(bool on);
    void say(const QString& text, bool trouble);

    std::shared_ptr<ZStorage> storage_;   // открытое хранилище; может быть пуст
    std::shared_ptr<SecretStore> secrets_;
    QString currentRoot_;                 // канонизированный
    Keyfile::KdfParams mintParams_;
    Result result_;

    QListWidget* list_ = nullptr;
    QPushButton* addButton_ = nullptr;
    QPushButton* removeButton_ = nullptr;
    QPushButton* openButton_ = nullptr;
    QLineEdit* folder_ = nullptr;
    QPushButton* browseButton_ = nullptr;
    QLineEdit* server_ = nullptr;
    QLineEdit* user_ = nullptr;
    QLineEdit* serverPassword_ = nullptr;
    QCheckBox* insecureHttp_ = nullptr;
    QLabel* passwordLabel_ = nullptr;
    QLineEdit* password_ = nullptr;
    QLabel* password2Label_ = nullptr;
    QLineEdit* password2_ = nullptr;
    QPushButton* applyButton_ = nullptr;
    QPushButton* resetButton_ = nullptr;
    QLabel* status_ = nullptr;
    QPushButton* closeButton_ = nullptr;

    std::thread worker_;
    bool busy_ = false;
    bool newEntry_ = false;   // форма про ещё не добавленную папку
    bool resetMode_ = false;
    int selected_ = -1;       // строка result_.stores, которую показывает форма

    // --- результаты рабочего потока (пишутся до done, читаются после) ------
    ZStorage::CloudProbe probe_;
    // Свежесть облака (конверта нет — пароль дважды) выясняется отдельным
    // шагом; правка адреса или смена строки её забывает.
    bool freshnessKnown_ = false;
    bool cloudFresh_ = false;
    ZStorage::Config settled_;        // строка, какой ей быть после удачи
    QString resetSummary_;
    Keyfile takenKey_;                // что положить в keyring (пусто — нечего)
    QString takenServerPassword_;
    QString takenStoreId_;
};

}  // namespace zametti

#endif  // ZAMETTI_STORE_MANAGER_DIALOG_H
