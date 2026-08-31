// ZStorageManager — хранилища устройства: список, факты, окно управления.
//
// ОДИН класс обслуживает главный диалог управления хранилищами целиком
// (решение владельца, 30.08.2026: «если ZStorageManager не способен обслужить
// главный диалог — архитектура неправильная»). Прежние три сущности — модель
// окна StoreManagerModel, раннер работ StoreJobRunner и сам менеджер — слиты
// сюда; диалогу остаются раскладка, потоки и исполнение намерений.
//
// Разбор прежнего окна нашёл 21 беду, и добрая половина — одного рода:
// доступность кнопок считалась в семи местах по-разному, а связь с облаком и
// свежесть лежали полями окна и залипали при переключении строк. Теперь у
// каждого вопроса ровно одно место — этот класс, и диалог спрашивает у него
// ВСЁ: какие есть хранилища, что набрано, что видели в облаке, какие кнопки
// живы (snapshot — ЕДИНСТВЕННЫЙ пересчёт всего видимого).
//
// Класс — хозяин раздела "stores": [...] в state.json: он один канонизирует
// пути, дедуплицирует строки и отвечает, что лежит по названному пути и что
// про него знает связка. Сам файл state.json по-прежнему пишет ТОЛЬКО ZApp
// (правило проекта): менеджер лишь отдаёт и принимает свою секцию
// (storesToJson/storesFromJson). Сериализуется ТОЛЬКО список строк: черновики
// и память «что видели» на диск не идут никогда.
//
// ФАКТЫ — ЭТО НЕ ЧЕРНОВИКИ. Факты — то, что можно узнать у диска и связки:
// какого рода каталог (inspect), сводка хранилища (localSummary), есть ли в
// связке ключ и пароли (SecretStore::has — атрибуты, БЕЗ чтения секрета: на
// маке чтение вправе поднять системный вопрос, а спрашивается это на каждое
// переключение строки). Ответы кэшируются по каноническому пути; refresh()
// сбрасывает кэш строки после работы, которая могла факты сменить.
// Черновики — то, что человек НАБРАЛ в форме; живут по ключу-корню весь
// процесс (переживают переоткрытие окна) и на диск не идут.
//
// СЕКРЕТЫ В ЧЕРНОВИК НЕ КЛАДУТСЯ НИКОГДА. Пароль, лежащий в связке,
// показывается заглушкой (Field::stub — кружочки); первое нажатие клавиши её
// стирает, и заглушка не уезжает в работу как пароль. Связка при пересчёте
// снимка СПРАШИВАЕТСЯ (has), но не читается.
//
// На жест класс отвечает НАМЕРЕНИЕМ (Reaction): спросить человека, сделать
// работу, переключиться, закрыть. Исполняет намерение окно; наборы зовут те
// же жесты без окна и сверяют снимки.

#ifndef ZAMETTI_ZSTORAGE_MANAGER_H
#define ZAMETTI_ZSTORAGE_MANAGER_H

#include "keyfile.h"
#include "zstorage.h"

#include <QDateTime>
#include <QHash>
#include <QJsonArray>
#include <QList>
#include <QString>
#include <QStringList>

#include <memory>

namespace zametti {

class SecretStore;

class ZStorageManager {
public:
    // Связка нужна фактам (Known); без неё они честно Unknown. Отдаётся
    // указателем, а не читается глобально: наборы подсовывают подделку.
    // mintParams — параметры чеканки ключа для работ (runJob); наборам
    // боевой Argon2id не нужен.
    explicit ZStorageManager(std::shared_ptr<SecretStore> secrets = nullptr,
                             const Keyfile::KdfParams& mintParams = Keyfile::defaults());
    // Связка приезжает и позже (ZApp живёт раньше, чем main решит, keyring
    // это или среда); прежние ответы Known пересчитываются.
    void setSecrets(std::shared_ptr<SecretStore> secrets);

    // Ключ строки — канонический вид пути: один и тот же каталог, названный
    // с хвостовым слэшем или через «..», не должен плодить две строки.
    static QString canonicalRoot(const QString& root);

    // --- список ------------------------------------------------------------
    const QList<ZStorage::Config>& stores() const { return stores_; }
    int size() const { return int(stores_.size()); }
    bool isEmpty() const { return stores_.isEmpty(); }
    // Без дублей по канонизированному root; знакомая строка обновляется НА
    // МЕСТЕ (порядок стабилен — список в диалоге не прыгает), новая — в конец.
    // Имя, не приехавшее с записью, наследуется от прежней: прежнее дороже
    // пустого.
    void remember(const ZStorage::Config& entry);
    void forget(const QString& root);
    // Строка по корню; неизвестный корень — пустая запись.
    ZStorage::Config storeFor(const QString& root) const;

    // --- текущее открытое хранилище -----------------------------------------
    // Первоклассное понятие менеджера (решение владельца, 30.08.2026):
    // ставит его ZApp при подключении и отключении хранилища, пустой ключ —
    // ничего не открыто. Окно лишь читает.
    void setOpenRoot(const QString& root) { openRoot_ = canonicalRoot(root); }
    const QString& openRoot() const { return openRoot_; }

    // --- секция "stores" в state.json ---------------------------------------
    QJsonArray storesToJson() const;
    void storesFromJson(const QJsonArray& stores);

    // --- факты строки --------------------------------------------------------
    // Тройка ответов связки: спрошено — да/нет; связки нет — неизвестно.
    enum class Known { Unknown, Yes, No };
    struct Facts {
        ZStorage::DirKind kind = ZStorage::DirKind::Missing;
        ZStorage::Summary stats;              // пусты, когда kind != Store
        Known key = Known::Unknown;           // мастер-ключ в связке?
        Known serverPassword = Known::Unknown;
        Known encryptionPassword = Known::Unknown;
    };
    // Факты ЛЮБОГО названного пути, не только строки списка: человек мог
    // набрать другую папку в форме, и показывать он вправе то, что набрано.
    // Ответ кэшируется по каноническому пути до refresh()/remember()/forget().
    // const с mutable-кэшем — логическая константность: snapshot() обязан
    // спрашивать факты, ничего не меняя по существу.
    Facts facts(const QString& folder) const;
    // Работа могла сменить факты (завелось хранилище, лёг секрет): сбросить
    // кэш строки обязан кто-то один — вот он; пересчитает следующий facts().
    void refresh(const QString& root);

    // --- окно: поля формы и черновики ---------------------------------------
    // Поля формы; правка любого идёт через edit() и стирает сообщение.
    enum class FieldId {
        Folder,
        Server,
        ServerDir,
        Login,
        ServerPassword,
        EncryptionPassword,
        Repeat,
    };

    // Черновик строки: что человек набрал. Заводится один раз на строку и
    // живёт, пока живёт менеджер (то есть весь процесс: набранное переживает
    // и переоткрытие окна). НА ДИСК НЕ ИДЁТ НИКОГДА.
    struct Draft {
        QString folder;
        QString server;      // базовый адрес провайдера ИЛИ каталог-облако
        QString serverDir;   // папка хранилища внутри сервера (Cloud dir)
        QString login;
        QString serverPassword;
        QString encryptionPassword;
        QString repeat;
        // Первое нажатие клавиши в поле пароля стирает заглушку связки; после
        // этого пустое поле — это «пусто», а не «возьми из связки».
        bool serverPasswordTouched = false;
        bool encryptionTouched = false;
    };

    // Что мы ВИДЕЛИ в облаке по этому адресу (итог Check или другой работы).
    // Память строки, не глобальная: переключился и вернулся — факты те же
    // (беда F разбора).
    struct CloudSeen {
        enum class State {
            NotChecked,     // ещё не смотрели
            Empty,          // достучались, там пусто — законный старт
            Ours,           // наше хранилище
            Foreign,        // чужое хранилище
            Incomplete,     // манифест без конверта
            NoAnswer,       // не достучались
            LoginRefused,   // сервер отверг логин
            WrongPassword,  // облако наше, но пароль шифрования не подошёл
        };
        State state = State::NotChecked;
        QString address;          // к какому адресу относится увиденное
        ZStorage::Summary stats;  // сводка листинга (метка — UTC)
    };

    // --- снимок: всё видимое, посчитанное один раз ---------------------------
    struct Line {
        QString text;
        bool alarm = false;
    };
    struct Button {
        bool enabled = false;
        QString label;
    };
    struct Field {
        QString text;
        QString placeholder;
        bool enabled = true;
        bool stub = false;           // показать кружочки: «пароль в связке»
        bool eyeEnabled = false;
        bool placeholderAlarm = false;   // красное требование — только по «нет»
    };
    struct Row {
        QString title;
        QString root;
        bool open = false;
    };
    struct Snapshot {
        QList<Row> rows;
        int selected = -1;
        QString folder;
        bool folderFrozen = false;   // путь открытого хранилища под замком
        bool folderMissing = false;  // папка не нашлась — путь красным
        Field server;
        Field serverDir;
        Field login;
        Field serverPassword;
        Field encryptionPassword;
        QString repeat;
        bool repeatVisible = false;
        Button add, remove, browse, check, reset, open, close;
        Line local;     // факт про папку — всегда
        Line cloud;     // факт про облако — всегда
        Line message;   // про последний жест; обычно пусто, одна строка
    };

    // --- намерение -----------------------------------------------------------
    struct Question {
        enum class Kind { None, Forget, Create, ResetCloud };
        Kind kind = Kind::None;
        QString text;
        QString detail;
        QStringList choices;   // последняя — отказ
    };
    struct Job {
        enum class Kind {
            None,
            Check,               // разведка probeCloud — без пароля шифрования
            Create,              // пустая папка: завести (± облако)
            ChangePassword,      // ключ в связке: один конверт, ноль стираний
            EraseAndReseed,      // стереть и запечатать новым паролем
            EraseAndDisconnect,  // стереть и забыть адрес с секретами
        };
        Kind kind = Kind::None;
        QString root;
        ZStorage::Config cfg;
        // РАЗРЕШЕНИЕ ЗАПЕЧАТАТЬ ПУСТОЕ ОБЛАКО. Выдаёт его только снимок — и
        // только когда повтор пароля пройден (автомат свежести): первый Check
        // по неизвестному адресу лишь смотрит, запечатывает второй.
        bool sealEmpty = false;
        QString serverPassword;
        bool serverPasswordFromKeyring = false;   // пусто и не тронуто — взять из связки
        QString encryptionPassword;
        bool encryptionFromKeyring = false;
    };
    struct Outcome {
        bool ok = false;
        QString message;       // одно предложение в статус
        bool alarm = false;
        CloudSeen seen;        // NotChecked — работа облако не разглядывала
        bool downloadedNew = false;   // Create из облака: голова приехала
    };
    struct Reaction {
        Question question;
        Job job;
        QString switchToRoot;
        bool close = false;    // отцепить открытое хранилище немедленно
    };

    // Открытие окна: выбор — строка открытого хранилища, а нет его — первая;
    // сообщение, недоделанные намерения и память «что видели в облаке»
    // чистятся (новое окно перепроверяет облако — снаружи мир мог измениться).
    // Черновики НЕ трогаются — набранное переживает переоткрытие (склероз).
    void beginSession();

    // --- выбор и черновики ---------------------------------------------------
    void select(int row);
    int selected() const { return selected_; }
    bool isOpenRow() const;
    // Набранные адрес и логин — в строки списка (переживают выход из
    // программы, даже когда связь не состоялась). Зовёт умирающее окно.
    void stashDrafts();
    const QHash<QString, Draft>& drafts() const { return drafts_; }

    void setMessage(const QString& text, bool alarm = false);
    void noteSeen(const CloudSeen& seen);
    const CloudSeen& seen() const;
    // Код клетки матрицы (§1) — имена тестов читаются через документ.
    QString cellCode() const;

    // ЕДИНСТВЕННЫЙ пересчёт всего видимого (таблицы A и §1.9 матрицы).
    Snapshot snapshot() const;

    // --- жесты ---------------------------------------------------------------
    void edit(FieldId field, const QString& text);
    Reaction addFolder(const QString& dir);
    Reaction forgetPressed();
    Reaction checkPressed();
    // Check без нажатия — при выборе строки, у которой пароль сервера уже в
    // связке, а облако в этом окне ещё не проверялось (владелец, 31.08).
    Reaction maybeAutoCheck();
    Reaction openPressed();
    Reaction resetPressed();
    // Ответ на переспрос: choice — индекс кнопки (последняя — отказ).
    Reaction answered(Question::Kind kind, int choice);
    // Итог работы: память строки, сообщение, пересчёт фактов; может вернуть
    // следующий шаг (Reset, заказанный до проверки, показывает переспрос).
    Reaction jobFinished(Job::Kind kind, const Outcome& outcome);

    // --- работы (zstorage_manager_jobs.cpp) ----------------------------------
    // Исполнить работу: синхронно, без виджетов; окно уводит вызов в рабочий
    // поток само, наборы зовут прямо. КОНТРАКТ ПОТОКОВ: runJob читает только
    // свой job, secrets и mintParams_, к списку, черновикам и кэшам не
    // прикасается — GUI-поток в это время вправе читать снимки; сериализацию
    // жестов обеспечивает busy-режим окна.
    //
    // Секреты в job уже настоящие (окно развернуло «взять из связки» до
    // запуска); secrets — копилка для добытого: настоящий keyring живёт при
    // главном потоке, окно перекладывает добытое по завершении, включая
    // просьбы забыть (disconnect).
    Outcome runJob(const Job& job, SecretStore& secrets) const;

protected:
    // storeId каталога — чтением zametti.json, с кэшем: identity стоит чтения
    // файла, а facts спрашиваются на каждое переключение строки.
    QString storeIdOf(const QString& key) const;
    QString rootKey(int row) const;
    // Индекс строки по каноническому корню; нет такой — −1.
    int rowOf(const QString& root) const;
    // Переезд строки вслед за новой папкой (беда I матрицы; сценарии 3 и 4
    // владельца): старая строка забывается, черновик и память «что видели»
    // едут за ключом, выбор — на новую. Сирота со старым путём не остаётся.
    // Новая строка к этому моменту уже в списке (remember — у вызывающего).
    void relocateRow(const QString& oldKey, const QString& newKey);
    ZStorage::Config rowConfig(int row) const;
    // Факты — ПО ПОЛЮ ПУТИ черновика, а не по строке списка: человек мог
    // набрать другую папку и ещё не применить.
    Facts rowFacts(int row) const;
    const Draft& draft() const;
    Draft& draft();
    ZStorage::Config cfgFromDraft() const;
    Job jobFor(Job::Kind kind) const;
    // Шаг «свежести» перед работой, запечатывающей пустое облако.
    bool sealingNeedsRepeat();
    // Пять работ runJob — тот же контракт потоков, что у него.
    Outcome checkJob(const Job& job, SecretStore& secrets) const;
    Outcome createJob(const Job& job, SecretStore& secrets) const;
    Outcome changePasswordJob(const Job& job, SecretStore& secrets) const;
    Outcome eraseAndReseedJob(const Job& job, SecretStore& secrets) const;
    Outcome eraseAndDisconnectJob(const Job& job, SecretStore& secrets) const;

    std::shared_ptr<SecretStore> secrets_;
    Keyfile::KdfParams mintParams_;
    QList<ZStorage::Config> stores_;
    // Кэши фактов — mutable: facts() логически ничего не меняет, а зваться
    // обязан из const-снимка.
    mutable QHash<QString, Facts> factsCache_;
    mutable QHash<QString, QString> storeIds_;

    QString openRoot_;
    int selected_ = -1;
    QHash<QString, Draft> drafts_;
    QHash<QString, CloudSeen> seen_;
    QString message_;
    bool messageAlarm_ = false;
    // Reset, заказавший проверку: переспрос покажется её итогом.
    bool askResetAfterCheck_ = false;
    // Open, заказавший применение набранного облака: открытие — её итогом.
    bool openAfterCheck_ = false;
};

}  // namespace zametti

#endif  // ZAMETTI_ZSTORAGE_MANAGER_H
