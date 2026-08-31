// Окно управления хранилищами — ТОНКОЕ: раскладка и исполнение, и больше
// ничего (§2.10 разбора, docs/zametti-store-window-matrix.md).
//
// Решения живут не здесь, а в ZStorageManager (ядро): список хранилищ
// устройства, факты строк, выбор, черновики, подписи и доступность кнопок,
// две строки фактов; на жест он отвечает НАМЕРЕНИЕМ. Окно спрашивает у него
// ВСЁ (решение владельца, 30.08.2026: один класс обслуживает диалог целиком).
//
// Окну остаётся четыре обязанности: собрать виджеты, показать снимок
// (render — единственная дорога от менеджера к экрану), спросить человека,
// когда менеджер просит переспрос, и увести работу в рабочий поток.
//
// ПОЧЕМУ ТАК. Прежнее окно решало всё само, и разбор нашёл в нём 21 беду —
// добрая половина одного рода: доступность кнопок считалась в семи местах
// по-разному, а связь с облаком и свежесть лежали полями окна и залипали при
// переключении строк. Теперь у каждого вопроса одно место, и все они
// проверяются наборами без единого виджета.
//
// РАСКЛАДКА (бриф владельца 30.08.2026): слева рамка со списком и рядом
// «+ −» под ним, справа рамка формы той же высоты; поля тянутся по ширине
// рамки; под кнопками Check/Reset cloud — два фрейма фактов Local и Cloud по
// две строки каждый; Open (или Create) стоит внизу РЯДОМ с Close — это два
// способа выйти из окна, а не действие над списком.
//
// ПОТОКИ РАЗДЕЛЕНЫ ПО ПРИРОДЕ ВЕЩЕЙ, как в SyncController: сеть и Argon2id
// (~секунда на разворот — порог владельца) бегут в рабочем потоке, форма на
// это время глохнет, но окно живёт и ЗАКРЫВАЕТСЯ. Связку (DBus при главном
// цикле) окно трогает ТОЛЬКО из главного потока: рабочему отдаётся
// приёмник-копилка, добытое перекладывается по завершении.

#ifndef ZAMETTI_STORE_MANAGER_DIALOG_H
#define ZAMETTI_STORE_MANAGER_DIALOG_H

#include "keyfile.h"
#include "secret_store.h"
#include "zstorage_manager.h"

#include <QDialog>
#include <QHash>
#include <QString>
#include <QStringList>

#include <functional>
#include <memory>
#include <thread>

class QAction;
class QFrame;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;

namespace zametti {

class FrozenText;
class TakenSecrets;

class StoreManagerDialog : public QDialog {
    Q_OBJECT

public:
    // Итог окна. Списка здесь больше нет: строки живут в ZStorageManager и
    // правятся на месте — «применить при закрытии» стало нечего.
    struct Result {
        QString switchToRoot;             // непусто — переключиться сюда
        // Облако ОТКРЫТОГО хранилища настроили или сменили: окну стоит
        // запустить прогон.
        bool cloudChangedForCurrent = false;
        // switchToRoot — только что заведённое из облака хранилище: там пока
        // манифест и корень, остальное обязан привезти первый прогон.
        bool downloadedNew = false;
    };

    // stores — ЖИВОЙ менеджер устройства (правится на месте); открытое
    // хранилище он уже знает сам (setOpenRoot ставит ZApp::openStorage);
    // secrets — связка, трогается только в главном потоке.
    StoreManagerDialog(QWidget* parent, ZStorageManager& stores,
                       std::shared_ptr<SecretStore> secrets);
    ~StoreManagerDialog() override;

    const Result& result() const { return result_; }

    // Крюк «отцепи текущее хранилище сейчас»: зовётся из «−» по открытой
    // строке. Обвязка отдаёт сюда attachStore с пустым корнем.
    void setDetachCurrent(std::function<void()> hook) { detachCurrent_ = std::move(hook); }

protected:
    // Занятое окно не закрывается ТОЛЬКО пока рабочий поток держит хранилище;
    // ждать его — обязанность деструктора, а не человека.
    void reject() override;
    void accept() override;
    bool eventFilter(QObject* watched, QEvent* event) override;

    // ЕДИНСТВЕННАЯ ДОРОГА ОТ МЕНЕДЖЕРА К ЭКРАНУ. Всё видимое приходит одним
    // снимком; своих решений окно не принимает.
    void render();
    // Исполнить намерение менеджера: спросить, сделать работу, переключиться.
    void act(const ZStorageManager::Reaction& reaction);
    // Переспрос; возвращает индекс выбранной кнопки (последняя — отказ).
    //
    // ВИРТУАЛЬНЫЙ РАДИ ОБЕЗЬЯНЫ: модальное окно останавливает поток, и
    // случайный прогон (zametti-bench store-monkey) без этой двери не смог бы
    // ткнуть ни в один переспрос — то есть половина жестов осталась бы
    // непроверенной. Тот же довод у askFolder: системный выбор папки обезьяне
    // не показать.
    virtual int ask(const ZStorageManager::Question& question);
    virtual QString askFolder();
    void runJob(const ZStorageManager::Job& job);

    void rebuildList(const ZStorageManager::Snapshot& snap);
    // Подпись строки списка: «✓ имя» у открытого хранилища.
    static QString shownTitle(const ZStorageManager::Row& row);
    void placeBrowseButton();
    void addStore();
    // Наборам — те же жесты мимо модальных вопросов и системного выбора папки.
    void addFolder(const QString& dir);
    void dropSelected();
    void chooseReset(int road);

    QAction* addEyeToggle(QLineEdit* field, ZStorageManager::FieldId which);
    void startWork(const QString& status, std::function<void()> job,
                   std::function<void()> done);
    void setBusy(bool on);
    // Лекарство от склероза (закон владельца): набранное переживает окно и
    // выход из программы. Адрес и логин — в строки списка, набранные пароли —
    // в связку, причём НЕПРОВЕРЕННЫЙ НЕ ЗАТИРАЕТ ПРОВЕРЕННОГО. Зовётся из
    // деструктора — единственной двери, через которую окно уходит всегда.
    void stashAll();

    ZStorageManager& stores_;
    std::shared_ptr<SecretStore> secrets_;
    std::function<void()> detachCurrent_;
    Result result_;

    QListWidget* list_ = nullptr;
    QFrame* listFrame_ = nullptr;
    QFrame* formFrame_ = nullptr;
    QPushButton* addButton_ = nullptr;
    QPushButton* removeButton_ = nullptr;
    QPushButton* openButton_ = nullptr;
    QLineEdit* folder_ = nullptr;
    FrozenText* folderFreeze_ = nullptr;
    QWidget* browseHolder_ = nullptr;
    QPushButton* browseButton_ = nullptr;
    QLineEdit* server_ = nullptr;
    QLineEdit* serverDir_ = nullptr;
    QLineEdit* user_ = nullptr;
    QLineEdit* serverPassword_ = nullptr;
    QAction* serverEye_ = nullptr;
    QAction* passwordEye_ = nullptr;
    QLabel* passwordLabel_ = nullptr;
    QLineEdit* password_ = nullptr;
    QLabel* password2Label_ = nullptr;
    QLineEdit* password2_ = nullptr;
    QPushButton* checkButton_ = nullptr;
    QPushButton* resetButton_ = nullptr;
    QLabel* localLine_ = nullptr;    // «Local: …» — две строки фактов, без рамки
    QLabel* cloudLine_ = nullptr;    // «Cloud: …» — две строки фактов, без рамки
    QLabel* status_ = nullptr;       // про последний жест; обычно пусто
    QPushButton* closeButton_ = nullptr;

    std::thread worker_;
    bool busy_ = false;
    QStringList shownRoots_;   // что сейчас в списке — чтобы не перестраивать зря

    // --- работа в рабочем потоке (пишется до done, читается после) ---------
    std::shared_ptr<TakenSecrets> taken_;
    ZStorageManager::Job runningJob_;
    ZStorageManager::Outcome outcome_;
};

}  // namespace zametti

#endif  // ZAMETTI_STORE_MANAGER_DIALOG_H
