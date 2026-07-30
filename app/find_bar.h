// Нижняя немодальная панель поиска.
//
// Одна по конструкции для всех трёх команд: Ctrl+F показывает поле и счётчик,
// Ctrl+H добавляет к нему поле замены, Ctrl+Shift+F — список результатов по
// всему хранилищу. Немодальная: работа не встаёт, пока панель открыта, и Esc
// закрывает её, снимая подсветку.
//
// Тумблера регэкспов здесь нет (не-цель этапа 4), но место под него оставлено:
// панель уже умеет показывать и прятать свои части, и добавить кнопку — это
// одна строка, а не перекройка.

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
    enum class Mode { InNote, Replace, Global };

    explicit FindBar(QWidget* parent = nullptr);

    // Открыть в нужном виде. preset — что положить в поле (выделенное в
    // редакторе); пусто — оставить прежний запрос.
    void open(Mode mode, const QString& preset);
    Mode mode() const { return mode_; }

    QString query() const { return find_->text(); }
    QString replacement() const { return replace_->text(); }
    // Счётчик «3/17» или пояснение вроде «ничего не найдено».
    void setStatus(const QString& text);

    // История запросов. Живёт между запусками (state.json), поэтому список
    // отдаётся наружу целиком, а не прячется внутри панели.
    void setHistory(const QStringList& items);
    QStringList history() const { return history_; }
    // Запомнить нынешний запрос. Зовётся, когда им ВОСПОЛЬЗОВАЛИСЬ, а не на
    // каждую букву: иначе история заполнится обрывками недонабранного.
    void rememberQuery();

signals:
    void queryChanged(const QString& text);
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

    Mode mode_ = Mode::InNote;
    QStringList history_;
    QToolButton* historyButton_ = nullptr;
    QLineEdit* find_ = nullptr;
    QLineEdit* replace_ = nullptr;
    QLabel* status_ = nullptr;
    QToolButton* replaceButton_ = nullptr;
    QToolButton* replaceAllButton_ = nullptr;
    QLabel* replaceLabel_ = nullptr;
};

}  // namespace zametti

#endif  // ZAMETTI_FIND_BAR_H
