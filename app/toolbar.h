// Тулбар: одна полоса кнопок над всем окном.
//
// Устройство. Виджет ничего не делает сам — он только сообщает, что нажали.
// Ни модели, ни редактора, ни хранилища он не знает: кто на что подписан,
// решает окно. Иначе тулбар стал бы вторым местом, где живут правила работы с
// заметками, и рассинхронизация была бы вопросом времени (см. уговор «всё, что
// касается заметки, живёт в объекте заметки»).
//
// Кнопки перечислены ОДНИМ СПИСКОМ (Button ниже), и из него же строится
// раскладка. Это не украшение: список — единственное место, где сказано, какая
// кнопка какой иконкой рисуется, что у неё в тултипе и в какой она группе.
// Проверка ходит туда же, поэтому «добавили кнопку и забыли тултип» краснеет.
//
// Задизейбленные кнопки — обещания, а не мусор. Они видны, но приглушены, и
// тултип объясняет, чего именно ждать. Убрать их до поры нельзя: тулбар,
// меняющий состав, каждый раз заново заставляет искать глазами нужное.

#ifndef ZAMETTI_TOOLBAR_H
#define ZAMETTI_TOOLBAR_H

#include <QHash>
#include <QString>
#include <QWidget>

#include <span>

class QToolButton;

namespace zametti {

class Toolbar : public QWidget {
    Q_OBJECT

public:
    // Порядок значений — порядок кнопок слева направо. Все кнопки стоят одной
    // грядой слева: поиск в правом верхнем углу владелец отменил, решив, что
    // искать удобнее там же, где всё остальное.
    enum class Button {
        NewNote,
        NewFolder,
        ImportNotes,
        InsertImages,
        Export,
        Cloud,
        Panels,
        SortByName,
        SortByDate,
        // Вход в историю заметки — ВТОРАЯ ДВЕРЬ туда же, куда ведёт Ctrl+Z,
        // доехавший до дна цепочки отмены. Нужна именно вторая: после десятка
        // правок проваливание требует сперва размотать свои же правки, а
        // человек хочет просто посмотреть прошлое (решение владельца).
        History,
        // Ctrl+F и Ctrl+Shift+F.
        Search,
        SearchInStore,
        Settings,
        Help,
    };

    // Что кнопка такое. Держится рядом с ней, а не в трёх местах кода.
    struct Spec {
        Button id;
        const char* icon;      // имя svg в :/icons, без расширения
        const char* tip;       // тултип без шортката
        const char* shortcut;  // шорткат для тултипа; пусто — нет
        int group;             // соседние с одним номером стоят вплотную
        bool checkable;
    };

    static std::span<const Spec> specs();

    explicit Toolbar(QWidget* parent = nullptr);

    // Перечитать оформление: цвета, размер иконки, шрифт тултипов.
    void refreshAppearance();

    void setEnabled(Button id, bool on);
    void setChecked(Button id, bool on);
    bool isChecked(Button id) const;
    // Кнопка есть в раскладке и доступна для нажатия. Нужно проверкам.
    bool isEnabled(Button id) const;

    // Обещание, которого пока нет. Кнопка гаснет, а тултип объясняет причину —
    // молча погашенная кнопка читается как поломка.
    void setPromise(Button id, const QString& why);

    QToolButton* buttonFor(Button id) const;

signals:
    void pressed(Toolbar::Button id);

protected:
    // Плотность экрана — величина ПЛАВАЮЩАЯ: окно переезжает между мониторами,
    // и растр, нарисованный под прежнюю, начинает мылить. На этапе 8 ровно это
    // заставляло картинки наползать друг на друга. Ловим событие и
    // перерисовываем иконки, а не надеемся на первый замер.
    bool event(QEvent* e) override;

    // Волосяная черта по низу. Без неё полоса кнопок сливается с панелями:
    // фон у них разный на три единицы яркости, а глаз ищет край.
    void paintEvent(QPaintEvent* e) override;

private:
    void build();
    void restyle();

    QHash<int, QToolButton*> buttons_;
    QHash<int, QString> promises_;
};

}  // namespace zametti

#endif  // ZAMETTI_TOOLBAR_H
