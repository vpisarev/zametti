// Виды режима истории: баннер над текстом разности и таймлайн сбоку.
//
// Режим громкий по решению владельца: человек обязан видеть, что перед ним
// прошлое, а не тихо подменённые байты. Отсюда две части, и обе на виду всё
// время режима — баннер с обеими дверьми наружу («К текущей версии»,
// «Восстановить эту») и список записей, по которому видно, куда ещё можно
// шагнуть.
//
// Виджеты ничего не знают ни про журнал, ни про редактор: им приносят готовые
// строки, а они отдают сигналы. Так их можно показать в снимке Xvfb, не заводя
// хранилища.

#ifndef ZAMETTI_HISTORY_PANEL_H
#define ZAMETTI_HISTORY_PANEL_H

#include "journal.h"

#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QWidget>

namespace zametti {

// Человеческое время записи: «14 марта 2024, 21:40». Отдельно от виджета —
// нужно и заголовку окна.
QString historyMoment(qint64 msSinceEpoch);

// Штамп для заголовка окна: «history:2024-03-14 21:40:05» (решение владельца).
// Машинный вид с секундами намеренно: заголовок окна — не то место, где нужна
// красота, зато по нему видно точный момент, а два соседних слепка одной
// минуты не выглядят одинаково.
QString historyStamp(qint64 msSinceEpoch);

// Короткое имя вида записи для таймлайна.
QString historyKindName(ZJournal::Kind kind);

class HistoryBanner : public QWidget {
    Q_OBJECT

public:
    explicit HistoryBanner(QWidget* parent = nullptr);

    // Что показано: время слепка, его вид и сколько строк тронуто против базы
    // (changed < 0 — не писать).
    void setSnapshot(qint64 time, ZJournal::Kind kind, int changed = -1);

    // Какая база сравнения показана сейчас: предыдущая запись или свежая
    // версия заметки.
    void setBaseIsFresh(bool fresh);

    // Печатающую клавишу в слепке отбили — коротко подсветить «Восстановить
    // эту» и сказать словами, что делать. Восстановление только явным жестом,
    // и подсветка тут вместо действия, а не в придачу к нему.
    void flashRestore();

signals:
    void leaveRequested();
    void restoreRequested();
    // Переключили базу сравнения: сравнивать со свежей версией или с
    // предыдущей записью.
    void baseChanged(bool fresh);

protected:
    // Надпись ужимается первой (в узком окне место — кнопкам), но не режется,
    // а укорачивается многоточием.
    void resizeEvent(QResizeEvent* event) override;

private:
    QLabel* text_;
    QString fullText_;
    void showText(const QString& text);
    // Пара залипающих кнопок: база сравнения. Пара, а не один переключатель с
    // меняющейся надписью, — просьба владельца: у одной кнопки не видно
    // второго состояния, и читается она наоборот через раз. (Пара «вид:»
    // снята в сессии 7 вместе с видом полосок: вид один — markdown.)
    QPushButton* fromFresh_;
    QPushButton* fromPrevious_;
    QPushButton* leave_;
    QPushButton* restore_;
    QString restoreStyle_;
};

// Таймлайн: время, вид и размер каждой записи. Закрытие панели — это выход из
// режима, поэтому у неё есть свой крестик, а сигнал тот же, что у кнопки «К
// текущей версии».
class HistoryTimeline : public QWidget {
    Q_OBJECT

public:
    explicit HistoryTimeline(QWidget* parent = nullptr);

    // Заполнить записями. Свежие сверху: в прошлое человек идёт сверху вниз,
    // как в списке заметок.
    void setEntries(const QVector<ZJournal::Record>& entries);
    // Отметить показанную запись (номер в журнале, не в списке).
    void setCurrent(int index);
    // Ширина, при которой строки списка не режутся: по самой длинной записи
    // плюс полоса прокрутки и рамка. Ей и меряется список по умолчанию —
    // столько ему и нужно, а не ширину средней колонки (владелец: список крал
    // место у разности).
    int contentWidth() const;

signals:
    void entryChosen(int index);
    void closeRequested();

private:
    QListWidget* list_;
    QVector<ZJournal::Record> entries_;
    bool quiet_ = false;   // выделение переставляем сами — сигнал не нужен
};

}  // namespace zametti

#endif  // ZAMETTI_HISTORY_PANEL_H
