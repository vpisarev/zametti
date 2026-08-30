// StoreManagerModel — ВИД окна хранилищ, без единого виджета (§2.3 матрицы,
// docs/zametti-store-window-matrix.md; заголовок реконструирован по уцелевшему
// .cpp после катастрофы 30.08.2026).
//
// Разбор прежнего окна нашёл 21 беду, и добрая половина — одного рода:
// доступность кнопок считалась в семи местах по-разному, а связь с облаком и
// свежесть лежали полями окна и залипали при переключении строк. Теперь у
// каждого вопроса ровно одно место:
//
//   ZStorageManager   — строки списка и ФАКТЫ (что на диске, что в связке);
//   StoreManagerModel — выбор, черновики, память «что видели в облаке» и
//                       ЕДИНСТВЕННЫЙ пересчёт всего видимого (snapshot);
//   StoreJobRunner    — работы, синхронно и без виджетов;
//   StoreManagerDialog — раскладка и исполнение, больше ничего.
//
// На жест модель отвечает НАМЕРЕНИЕМ (Reaction): спросить человека, сделать
// работу, переключиться, закрыть. Исполняет намерение окно; наборы зовут те
// же жесты без окна и сверяют снимки.
//
// СЕКРЕТЫ В ЧЕРНОВИК НЕ КЛАДУТСЯ НИКОГДА. Пароль, лежащий в связке,
// показывается заглушкой (Field::stub — кружочки); первое нажатие клавиши её
// стирает, и заглушка не уезжает в работу как пароль. Связка при пересчёте
// снимка СПРАШИВАЕТСЯ (has), но не читается — см. ZStorageManager.

#ifndef ZAMETTI_STORE_MANAGER_MODEL_H
#define ZAMETTI_STORE_MANAGER_MODEL_H

#include "zstorage.h"
#include "zstorage_manager.h"

#include <QDateTime>
#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>

namespace zametti {

class StoreManagerModel {
public:
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
    // живёт до закрытия окна (сеансовые — переживают и переоткрытие окна,
    // adoptDrafts). НА ДИСК НЕ ИДЁТ НИКОГДА.
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

    // --- намерение модели ----------------------------------------------------
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
        // РАЗРЕШЕНИЕ ЗАПЕЧАТАТЬ ПУСТОЕ ОБЛАКО. Выдаёт его только модель — и
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

    // openRoot — корень открытого хранилища (пусто — окно без хранилища).
    StoreManagerModel(ZStorageManager& stores, const QString& openRoot);

    // --- выбор и черновики ---------------------------------------------------
    void select(int row);
    int selected() const { return selected_; }
    bool isOpenRow() const;
    // Сеансовые черновики: живут у обвязки, переживают переоткрытие окна.
    void adoptDrafts(QHash<QString, Draft>* drafts);
    // Набранные адрес и логин — в строки списка (переживают выход из
    // программы, даже когда связь не состоялась). Зовёт умирающее окно.
    void stashDrafts();
    const QHash<QString, Draft>& drafts() const { return *drafts_; }

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
    Reaction openPressed();
    Reaction resetPressed();
    // Ответ на переспрос: choice — индекс кнопки (последняя — отказ).
    Reaction answered(Question::Kind kind, int choice);
    // Итог работы: память строки, сообщение, пересчёт фактов; может вернуть
    // следующий шаг (Reset, заказанный до проверки, показывает переспрос).
    Reaction jobFinished(Job::Kind kind, const Outcome& outcome);

protected:
    QString rootKey(int row) const;
    ZStorage::Config rowConfig(int row) const;
    // Факты — ПО ПОЛЮ ПУТИ черновика, а не по строке списка: человек мог
    // набрать другую папку и ещё не применить.
    ZStorageManager::Facts rowFacts(int row) const;
    const Draft& draft() const;
    Draft& draft();
    ZStorage::Config cfgFromDraft() const;
    Job jobFor(Job::Kind kind) const;
    // Шаг «свежести» перед работой, запечатывающей пустое облако.
    bool sealingNeedsRepeat();

    ZStorageManager& stores_;
    QString openRoot_;
    int selected_ = -1;
    // Черновики по ключу-корню; по умолчанию свои, adoptDrafts подменяет.
    QHash<QString, Draft> ownDrafts_;
    QHash<QString, Draft>* drafts_ = &ownDrafts_;
    QHash<QString, CloudSeen> seen_;
    QString message_;
    bool messageAlarm_ = false;
    // Reset, заказавший проверку: переспрос покажется её итогом.
    bool askResetAfterCheck_ = false;
    // Open, заказавший применение набранного облака: открытие — её итогом.
    bool openAfterCheck_ = false;
};

}  // namespace zametti

#endif  // ZAMETTI_STORE_MANAGER_MODEL_H
