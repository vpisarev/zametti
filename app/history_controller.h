// Контроллер режима истории: вход, выход, шаги, восстановление, клавиши.
//
// MVC режима (сессия 7): модель — ZNoteTimeline (ядро), виды — HistoryView
// (баннер + текст разности) и HistoryTimeline (список записей), контроллер —
// здесь. Редактор про историю не знает: он отдаёт журнал и байты живой заметки,
// принимает восстановленное тело одной правкой и просит войти в режим двумя
// сигналами (дно цепочки отмены, архивная заметка). Живой буфер из редактора
// не уезжает никогда — на время режима редактор просто скрыт (окно
// переключает стек по modeChanged).
//
// Отдельным объектом, а не лямбдами в main(): проводка режима — ровно то, что
// ломалось молча (F4 без фокуса в тексте, Tab, уезжавший в дерево), и набор
// обязан звать ту же проводку, что и окно.

#ifndef ZAMETTI_HISTORY_CONTROLLER_H
#define ZAMETTI_HISTORY_CONTROLLER_H

#include "editor_widget.h"
#include "history_panel.h"
#include "history_search.h"
#include "history_view.h"
#include "znote_timeline.h"

#include <QObject>

#include <memory>

namespace zametti {

class HistoryController : public QObject {
    Q_OBJECT

public:
    HistoryController(NoteEditor& editor, HistoryView& view, QObject* parent = nullptr);

    // Войти в режим на записи index (-1 — последняя со слепком); в режиме —
    // показать другую запись. false — истории нет.
    //
    // ВХОД В ИСТОРИЮ — ЕЩЁ ОДНА ТОЧКА СОХРАНЕНИЯ, в один ряд с уходом из
    // заметки и выходом из программы: незаписанные правки уходят в файл, а
    // значит и в журнал, — человек пошёл смотреть прошлое, и вершина его работы
    // обязана в этом прошлом оказаться. Признак «изменён» не спрашивается
    // (force); лишней записи не бывает — правила отбора внутри save работают.
    bool enter(int index = -1);
    // Вернуться к живой версии. Вне режима ничего не делает.
    void leave();
    bool active() const { return timeline_ != nullptr; }
    int index() const { return timeline_ != nullptr ? timeline_->index() : -1; }
    // Какая запись была показана последней (и после выхода): окну для памяти
    // «куда возвращаться».
    int lastIndex() const { return lastIndex_; }
    std::shared_ptr<ZNoteTimeline> timeline() const { return timeline_; }

    bool select(int index);
    bool stepBack();
    // Шаг вперёд с последнего слепка выводит из режима — «в конце возвращаемся
    // к живой».
    bool stepForward();
    void setBaseFresh(bool fresh);
    // Восстановить показанный слепок в живую заметку. Возвращает время записи
    // (для подписи); 0 — не вышло; alreadyCurrent — слепок и есть нынешняя
    // версия (режим всё равно закрывается).
    qint64 restore(bool* alreadyCurrent = nullptr);
    // То же по нажатию «Restore this one»: сперва вопрос «Revert note to the
    // snapshot from …?» (исключение владельца из правила «без диалогов»),
    // по «Yes» — restore(). Вопрос немодальный: ответ приходит сигналом.
    void askAndRestore();
    // Поиск по истории ЭТОЙ заметки: сперва ленивая чистка журнала, потом
    // проход по всем слепкам. Работает и вне режима.
    HistorySearchReport searchHistory(const QString& text);

    // КЛАВИШИ РЕЖИМА — ЯРЛЫКАМИ ОКНА, а не обработчиками текста: фокус в этом
    // режиме запросто оказывается в списке слепков (владелец: «F4 не работает,
    // надо сперва кликнуть на документе»). F4/Shift+F4 — из настроек. Ярлыки
    // живут, пока живо окно; включены только в режиме.
    void installShortcuts(QWidget* window);

    // Облик сменился (шрифт, цвета, масштаб облика): документы разности
    // собираются заново тем же слепком.
    void refreshAppearance();

signals:
    void modeChanged(bool on);
    void indexChanged(int index);
    // Восстановлено из записи (время записи) / слепок и был нынешней версией.
    void restored(qint64 sourceTime);
    void restoreWasCurrent();
    void editRefused();

private:
    NoteEditor& editor_;
    HistoryView& view_;
    HistoryTimeline& list_;
    std::shared_ptr<ZNoteTimeline> timeline_;
    int lastIndex_ = -1;
    std::vector<QShortcut*> shortcuts_;
    void showState();
};

}  // namespace zametti

#endif  // ZAMETTI_HISTORY_CONTROLLER_H
