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
#include "zoom_scale.h"

#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>


namespace zametti {

class ZStorageManager;

class ZAppState {
public:
    // Прочитать из state.json; файла нет или он битый — умолчания. Секция
    // "stores" ПРИНАДЛЕЖИТ ZStorageManager (решение владельца, 30.08.2026):
    // приложение работает со списком только через него, а здесь массив живёт
    // ровно один миг сериализации — прочитанная секция тут же отдаётся
    // менеджеру и НЕ хранится (nullptr — секция пропускается: наборам и
    // утилитам без списка она не нужна).
    static ZAppState load(ZStorageManager* stores = nullptr);
    // Записать в state.json; секцию "stores" в тот же миг отдаёт менеджер.
    // Файл пишет по-прежнему только ZApp (единые ворота).
    void save(const ZStorageManager& stores) const;
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

    // --- ТРИ МАСШТАБА, И НИ ОДИН НЕ УМНОЖАЕТСЯ НА ДРУГОЙ -------------------
    //
    // Число — СТУПЕНЬ k шкалы 2^(k/12) (zoom_scale.h), а не множитель:
    // множитель, накопленный домножением, копил в файле мусор вида
    // 1.1000000000000003. Ступени три, и они независимы (решение владельца):
    // текст заметки, плоские виды и оболочка. Ctrl+Alt+− уменьшает оболочку и
    // НЕ трогает текст заметки, Ctrl+= увеличивает заметку и НЕ трогает
    // тулбар; на бумагу не влияет ни одна из трёх — у PDF свои кегли и своего
    // масштаба нет вовсе.
    //
    // Старые дробные ключи (zoom, plainZoom, historyZoom, markdownZoom)
    // читаются и переводятся в ступени при загрузке — см. app_state.cpp.
    ZM_SETTING(int, noteZoom, NoteZoom, 0, kZoomStepsMin, kZoomStepsMax)

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
    // МАСШТАБ ПЛОСКИХ ВИДОВ — ОДИН НА ВСЕХ (решение владельца, refactor3;
    // разность истории влита сюда же 02.09.2026): правка исходника, правка
    // настроек и разность слепка — один и тот же моноширинный текст одним
    // кеглем (monospacePoint), и разъезжаться в масштабе им незачем. Прежде у
    // каждого было своё число, и режимы открывались разного размера. От
    // масштаба ЗАМЕТКИ он по-прежнему отдельный: исходник читают иначе, чем
    // вёрстку, — и Ctrl+= в любом плоском виде НЕ трогает скрытую заметку
    // (это правило дважды ошибалось на живых людях, см. zoom_target.h).
    ZM_SETTING(int, sourceZoom, SourceZoom, 0, kZoomStepsMin, kZoomStepsMax)
    // МАСШТАБ ОБОЛОЧКИ: тулбар, дерево, список заметок, полоса сведений,
    // панель поиска, диалоги. Правится Ctrl+Alt+= / Ctrl+Alt+− / Ctrl+Alt+0.
    //
    // Живёт в state.json, а не в конфиге, и это осознанно: одна и та же
    // машина под разными системами (и с разными мониторами) просит разной
    // плотности, а конфиг у человека один и переезжает вместе с ним. Системное
    // масштабирование экрана здесь ни при чём — его делает Qt, а это
    // ЛИЧНАЯ поправка поверх.
    ZM_SETTING(int, interfaceZoom, InterfaceZoom, 0, kZoomStepsMin, kZoomStepsMax)
    // THE SCALE OF A BOOK (brief 18): the pages of the reading mode are read
    // from further away than a note is edited, so they keep a step of their
    // own — Ctrl+= on a page does not touch the editor's step.
    ZM_SETTING(int, bookZoom, BookZoom, 0, kZoomStepsMin, kZoomStepsMax)
    // Ширина списка записей в режиме истории (сплиттер справа от разности);
    // 0 — не двигали, берётся ширина средней колонки из настроек. Просьба
    // владельца: список крал место у разности, а столько ему не нужно.
    ZM_SETTING(int, historyListWidth, HistoryListWidth, 0, 0, 10000)
    ZM_SETTING_PLAIN(QStringList, expandedDirs, ExpandedDirs, )

    // --- панели ----------------------------------------------------------
    // Прежние запросы поиска, свежий первым. Не путать с историей заметок —
    // её нет: это то, что набирали в поле поиска.
    ZM_SETTING_PLAIN(QStringList, searchHistory, SearchHistory, )
    // Тумблер регулярных выражений в панели поиска. Живёт между запусками
    // вместе с историей запросов: человек, который ищет выражениями, ищет ими
    // и завтра, а щёлкать тумблер каждый запуск — работа на ровном месте.
    ZM_SETTING_PLAIN(bool, searchRegex, SearchRegex, false)
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
        // The reading place and the mode (brief 18), see CaretSpot.
        int readingBlock = 0;
        int readingLine = 0;
        int mode = 0;
    };
    static constexpr int kCaretLimit = 500;
    // THE CARET HALF ONLY: cursor, anchor, scroll. The reading half of the
    // entry (place, mode) is written by the two below and survives this call
    // — the editor stashes the caret on every switch of notes, and it must
    // not wipe where the book was being read.
    void rememberCaret(const QString& noteId, const CaretSpot& spot);
    // The reading place (brief 18): the line at the top of the left page.
    void rememberReading(const QString& noteId, int block, int line);
    // The mode the note was left in: 1 — reading, 2 — editing.
    void rememberMode(const QString& noteId, int mode);
    // Место каретки в заметке по id; неизвестная заметка — начало документа
    // (0, 0, 0): вызывающему не о чем спрашивать дальше (решение владельца —
    // «чуть-чуть интеллекта каждому классу», чтобы логика не размазывалась).
    CaretSpot caretOf(const QString& noteId) const;
    bool knowsCaret(const QString& noteId) const;
    const QList<CaretEntry>& carets() const { return carets_; }

protected:
    // Разбор секции масштабов вместе с миграцией старых дробных ключей —
    // отдельным методом: сама эта развилка (объект против числа) и есть всё
    // знание о прежнем формате, и держать её надо в одном месте.
    void readZoom(const QJsonObject& root);

    QList<CaretEntry> carets_;
};

}  // namespace zametti

#endif  // ZAMETTI_APP_STATE_H
