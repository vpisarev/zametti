// Нижняя немодальная панель поиска.
//
// Одна по конструкции для всех трёх команд: Ctrl+F показывает поле и счётчик,
// Ctrl+H добавляет к нему поле замены, Ctrl+Shift+F — список результатов по
// всему хранилищу. Немодальная: работа не встаёт, пока панель открыта, и Esc
// закрывает её, снимая подсветку.
//
// Тумблер регулярных выражений — западающая кнопка справа от истории запросов
// (решение владельца, 27.08.2026). Он не отдельный режим панели, а признак
// ЗАПРОСА: панель отдаёт его наружу, а собирает запрос окно (makeQuery), и
// оттуда признак сам доезжает до всех, кто ищет.

#ifndef ZAMETTI_FIND_BAR_H
#define ZAMETTI_FIND_BAR_H

#include <QLabel>
#include <QLineEdit>
#include <QToolButton>
#include <QWidget>

namespace zametti {

class FindBar : public QWidget {
    Q_OBJECT

public:
    // InNote — по открытой заметке, Replace — она же с заменой, Global — по
    // всему хранилищу, History — по слепку И по всей истории этой заметки
    // (только в режиме истории; замена там невозможна по построению).
    enum class Mode { InNote, Replace, Global, History };

    explicit FindBar(QWidget* parent = nullptr);

    // Открыть в нужном виде. preset — что положить в поле (выделенное в
    // редакторе); пусто — оставить прежний запрос.
    void open(Mode mode, const QString& preset);
    Mode mode() const { return mode_; }

    QString query() const { return find_->text(); }
    QString replacement() const { return replace_->text(); }
    // Тумблер выражений. Живёт между запусками, поэтому читается и ставится
    // снаружи (state.json), как и история запросов.
    bool regexOn() const;
    void setRegexOn(bool on);
    // НЕДОПИСАННОЕ ВЫРАЖЕНИЕ — красные буквы запроса, и ничего больше (решение
    // владельца): ни слова об ошибке, правка продолжается. Панель сама этого не
    // решает — ей говорит окно, разобрав запрос.
    void setQueryUsable(bool usable);
    // Счётчик «3/17» или пояснение вроде «ничего не найдено».
    void setStatus(const QString& text);

    // История запросов. Живёт между запусками (state.json), поэтому список
    // отдаётся наружу целиком, а не прячется внутри панели.
    void setHistory(const QStringList& items);
    QStringList history() const { return history_; }
    // Запомнить нынешний запрос. Зовётся, когда им ВОСПОЛЬЗОВАЛИСЬ, а не на
    // каждую букву: иначе история заполнится обрывками недонабранного.
    void rememberQuery();
    // Шаг по истории: -1 — к старым запросам, +1 — обратно к новым и дальше к
    // тому, что набирали сами. Открыто наружу и ради теста: клавиатуру в
    // тесте виджету не подашь, а ходить надо в обе стороны.
    void stepHistory(int direction);

signals:
    void queryChanged(const QString& text);
    // Щёлкнули тумблер: запрос тот же, а искать надо заново.
    void regexToggled(bool on);
    void findNext();
    void findPrevious();
    void replaceOne();
    void replaceAll();
    void closed();

protected:
    // Enter — следующее, Shift+Enter — предыдущее, Esc — закрыть. Через
    // keyPressEvent, а не через ярлыки окна: пока панель открыта, эти клавиши
    // принадлежат ей.
    void keyPressEvent(QKeyEvent* event) override;

private:
    void showHistory();
    // Иконки кнопок: цвет и плотность экрана берутся у настроек и окна — как в
    // тулбаре, чтобы значки панели и тулбара не разъезжались.
    void restyleButtons();

    bool queryUsable_ = true;

    Mode mode_ = Mode::InNote;
    QStringList history_;
    // Где стоим в списке: -1 — не в истории, правим своё.
    int historyAt_ = -1;
    QString typed_;
    QToolButton* historyButton_ = nullptr;
    QToolButton* regexButton_ = nullptr;
    QToolButton* previousButton_ = nullptr;
    QToolButton* nextButton_ = nullptr;
    QLineEdit* find_ = nullptr;
    QLineEdit* replace_ = nullptr;
    QLabel* status_ = nullptr;
    QToolButton* replaceButton_ = nullptr;
    QToolButton* replaceAllButton_ = nullptr;
    QLabel* replaceLabel_ = nullptr;
};

}  // namespace zametti

#endif  // ZAMETTI_FIND_BAR_H
