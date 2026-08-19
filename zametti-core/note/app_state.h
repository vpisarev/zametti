// ZAppState — состояние сеанса приложения, то есть содержимое state.json.
//
// Отдельная структура с методами (решение владельца: всё, что попадает в
// state.json, — в свою структуру ради модульности; не пассивная структура, а
// класс, который сам читает и пишет свой файл и сам держит свои правила —
// например, «каретки по заметкам без дублей и не больше kCaretLimit»).
//
// Это НЕ настройки: конфиг человек правит руками, а state.json программа
// пишет сама на выходе. Пишет его ТОЛЬКО ZApp (единые ворота), окно лишь
// наполняет поля.

#ifndef ZAMETTI_APP_STATE_H
#define ZAMETTI_APP_STATE_H

#include "caret_spot.h"
#include "settings.h"

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>


namespace zametti {

class ZAppState {
public:
    // Прочитать из state.json; файла нет или он битый — умолчания.
    static ZAppState load();
    // Записать в state.json.
    void save() const;
    // Путь файла состояния (рядом с конфигом).
    static QString path();

    // Поля закрыты, доступ методами (решение владельца: во всех нетривиальных
    // классах, в ZAppState в частности) — теми же макросами, что у настроек:
    // name() читает, setName() пишет; числа с обрезкой.

    // --- что было открыто ------------------------------------------------
    ZM_SETTING_PLAIN(QString, lastFile, LastFile, )
    // Хранилище прошлого запуска: без параметров возвращаемся в него.
    ZM_SETTING_PLAIN(QString, storeRoot, StoreRoot, )
    // Сортировка дерева: "modified" (свежие сверху) или "name".
    ZM_SETTING_PLAIN(QString, treeSort, TreeSort, )
    // ГДЕ БЫЛА КАРЕТКА в последней заметке и что было выделено.
    //
    // Здесь лежала ДОЛЯ ПРОКРУТКИ, и она попадала мимо: высота документа от
    // запуска к запуску другая — окно шире, масштаб иной, картинки и формулы
    // добирают свою высоту уже после того, как долю применили, — и та же доля
    // указывает на совсем другое место (владелец: «попадаю на тот же документ,
    // но в совсем другое место»). Место — это каретка, а не пиксели.
    //
    // Теперь место каретки лежит в carets по id заметки; эти два поля
    // читаются у старых state.json и пишутся для совместимости.
    ZM_SETTING(int, caret, Caret, 0, 0, 1 << 30)
    ZM_SETTING(int, anchor, Anchor, 0, 0, 1 << 30)
    ZM_SETTING(qreal, zoom, Zoom, 1.0, 0.1, 16.0)

    // --- окно ------------------------------------------------------------
    ZM_SETTING_PLAIN(QByteArray, windowGeometry, WindowGeometry, )
    ZM_SETTING_PLAIN(QByteArray, splitterState, SplitterState, )
    // Левые панели убраны кнопкой тулбара. Хранится отдельно от splitterState:
    // тот помнит ШИРИНЫ, и если спрятать панели, схлопнув их в ноль, ширины
    // потеряются и по возвращении панели придут не туда, где были.
    ZM_SETTING_PLAIN(bool, panelsHidden, PanelsHidden, false)
    // РЕЖИМ ПРАВКИ ИСХОДНИКА ИДЁТ (кнопка [M] нажата). Свойство ПРИЛОЖЕНИЯ, а
    // не заметки (решение владельца): по заметкам ходят, каждая открывается
    // исходником, кнопка не гаснет — и всё это переживает перезапуск.
    ZM_SETTING_PLAIN(bool, markdownMode, MarkdownMode, false)
    // МАСШТАБ РЕЖИМА ИСХОДНИКА — СВОЙ, отдельно от zoom обычного вида (решение
    // владельца). Иначе выходило так: увеличил текст в исходнике, отжал [M] — и
    // заметка вдруг крупнее, хотя её масштаб не трогали.
    ZM_SETTING(qreal, markdownZoom, MarkdownZoom, 1.0, 0.1, 16.0)
    // Ширина списка записей в режиме истории (сплиттер справа от разности);
    // 0 — не двигали, берётся ширина средней колонки из настроек. Просьба
    // владельца: список крал место у разности, а столько ему не нужно.
    ZM_SETTING(int, historyListWidth, HistoryListWidth, 0, 0, 10000)
    ZM_SETTING_PLAIN(QStringList, expandedDirs, ExpandedDirs, )

    // --- панели ----------------------------------------------------------
    // Прежние запросы поиска, свежий первым. Не путать с историей заметок —
    // её нет: это то, что набирали в поле поиска.
    ZM_SETTING_PLAIN(QStringList, searchHistory, SearchHistory, )
    // Куда вывозили в прошлый раз. Переживает перезапуск: вывозят обычно в одно
    // и то же место, и начинать каждый раз с «Документов» — значит каждый раз
    // идти по дереву каталогов заново.
    ZM_SETTING_PLAIN(QString, exportDir, ExportDir, )
    // Галочка «Сохранять имя и метаданные» в диалоге вывоза .md. Выбор
    // человека, а не свойство заметки.
    ZM_SETTING_PLAIN(bool, exportKeepMeta, ExportKeepMeta, false)

public:
    // --- каретки по заметкам ---------------------------------------------
    // Где человек стоял в каждой из недавних заметок — по id заметки, без
    // дублей, свежие впереди, не больше kCaretLimit. Хранит место сама заметка
    // (ZNote); сюда оно попадает, когда заметка уходит из кэша открытых или
    // программа выходит (ZApp), и отсюда возвращается при следующем открытии.
    struct CaretEntry {
        QString noteId;
        int cursor = 0;
        int anchor = 0;
        int scroll = 0;
    };
    static constexpr int kCaretLimit = 500;
    void rememberCaret(const QString& noteId, const CaretSpot& spot);
    // Место каретки в заметке по id; неизвестная заметка — начало документа
    // (0, 0, 0): вызывающему не о чем спрашивать дальше (решение владельца —
    // «чуть-чуть интеллекта каждому классу», чтобы логика не размазывалась).
    CaretSpot caretOf(const QString& noteId) const;
    bool knowsCaret(const QString& noteId) const;
    const QList<CaretEntry>& carets() const { return carets_; }

protected:
    QList<CaretEntry> carets_;
};

}  // namespace zametti

#endif  // ZAMETTI_APP_STATE_H
