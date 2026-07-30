// Виджет редактора: перевод ввода в операции и связь документа с историей.
//
// Собственной логики правки здесь нет и быть не должно — она живёт в
// editor_ops. Здесь только: что нажали, какую операцию звать, когда записать
// шаг истории и когда сохранить.
//
// Ключевое разделение — содержимое против облика. Правка содержимого заводит
// шаг истории; смена облика (масштаб, шрифт, цвета, отступы) пересобирает
// документ из текущего содержимого и историю не трогает вовсе.

#ifndef ZAMETTI_EDITOR_WIDGET_H
#define ZAMETTI_EDITOR_WIDGET_H

#include "edit_history.h"
#include "editor_ops.h"
#include "note_view.h"

#include <QElapsedTimer>
#include <QTextBlock>
#include <QFileSystemWatcher>
#include <QKeySequence>
#include <QSet>
#include <QString>
#include <QTimer>

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace zametti {

class NoteEditor : public NoteView {
    Q_OBJECT

public:
    explicit NoteEditor(QWidget* parent = nullptr);

    // Открыть заметку. Прежняя сохраняется, история начинается заново.
    bool openFile(const QString& path);
    QString filePath() const { return path_; }

    // Масштаб — облик: документ пересобирается, история остаётся как была.
    void applyZoom(qreal zoom);

    // Пересобрать документ из текущего содержимого. Звать после любой смены
    // настроек оформления.
    void refreshAppearance();

    void undo();
    void redo();

    // Файл изменился снаружи, а у нас есть несохранённые правки: пока человек
    // не решит, чьё содержимое брать, мы ничего не трогаем.
    bool hasExternalConflict() const { return externalPending_; }
    void resolveExternalConflict(bool takeExternal);

    // Сохранить, если есть что. interactive — показывать ли окно с ошибкой.
    void save(bool interactive);

    // Перенос открытой заметки в хранилище — правка её меты. Пустой parent —
    // в корень. Здесь, а не снаружи: meta_ живёт в редакторе, и файл под
    // открытой заметкой переписывать нельзя — сторож примет за чужую правку.
    void setMetaParent(const QString& parentId);

    // Доля прокрутки — только для запоминания места между запусками. Внутри
    // правок она не годится: документ пересобирается целиком, его высота от
    // правки к правке меняется, и доля от новой высоты каждый раз попадает не
    // туда. Замер: пункт за пунктом заметка уползала вверх на ~25 px за правку.
    double scrollRatio() const;
    void setScrollRatio(double ratio);

    // За что держится вид при пересборке: блок у верхней кромки окна и его
    // высота относительно неё.
    //
    // Держаться за курсор нельзя: его собственный блок при правке меняет
    // высоту — пункт списка, ставший абзацем, получает другие отступы, — и
    // тогда весь текст выше уезжает (замер: 23 px). Блок над правкой не
    // меняется вовсе, и вид стоит намертво.
    struct ViewAnchor {
        int irIndex = -1;   // блок IR у кромки; меньше нуля — держаться не за что
        int above = 0;      // насколько его верх выше кромки
    };
    ViewAnchor viewAnchor() const;

signals:
    void fileChanged(const QString& path);
    // Файл изменился снаружи, а у нас есть несохранённые правки. Окно с
    // вопросом показывает тот, кто нас создал: виджет о нём знать не должен.
    void externalChangeDetected();

protected:
    // Обменный формат — сам markdown. Переопределять обязательно: иначе Qt
    // кладёт в буфер собственный HTML и вставляет чужой HTML прямо в документ,
    // минуя модель.
    // Команды правки видны в меню, а не только в сочетаниях клавиш: сочетаний
    // много, и запоминать их никто не обязан.
    void contextMenuEvent(QContextMenuEvent* event) override;

    QMimeData* createMimeDataFromSelection() const override;
    bool canInsertFromMimeData(const QMimeData* source) const override;
    void insertFromMimeData(const QMimeData* source) override;

    // Только перевод ввода в вызовы операций. Ни одной правки документа отсюда:
    // иначе правило «одна операция — один шаг истории» держать нечем, а
    // инварианты расползаются по обработчикам событий.
    void keyPressEvent(QKeyEvent* event) override;
    // Щелчок по чекбоксу — самый ходовой способ отметить задачу, и мимо
    // клавиатуры он идти не должен.
    void mousePressEvent(QMouseEvent* event) override;
    // Двойной щелчок по рамке не должен выделять строку: человек метил в
    // чекбокс, а не в слово под ним.
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    // Угол фотографии тянется мышью: наведение меняет курсор, перетаскивание
    // меряет ширину вживую, отпускание записывает её операцией (с историей).
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    // У правого края ссылки набор её продолжал: Qt берёт оформление знака перед
    // курсором, а у ссылки оно с адресом. Пробел и запятая после ссылки уезжали
    // внутрь неё, и в файл шло "[текст ,](адрес)".
    void dropLinkAtRightEdge();

    // Блок, чью рамку задачи накрыл щелчок. Недействительный — мимо.
    QTextBlock checkboxUnder(const QMouseEvent& event) const;

    // Блок, за угол чьей фотографии можно взяться в этой точке вьюпорта.
    // onRight/onBottom — какой именно угол (для курсора и знака дельты).
    QTextBlock imageCornerUnder(const QPoint& pos, bool* onRight = nullptr,
                                bool* onBottom = nullptr);

    // Движение вверх-вниз держит экранный X, а поля у блоков разные: маркер
    // списка отодвигает текст пункта вправо. Из-за этого шаг вниз из начала
    // пункта попадал не в начало абзаца, а на пару знаков внутрь него — и
    // выделение прихватывало лишнее. Поправка возвращает курсор на то же место
    // относительно начала текста блока.
    void keepColumnAcrossMargins(QKeyEvent* event);

protected:

private:
    // Выполняет операцию, доводит документ до вида, который построил бы
    // сборщик, и заводит отдельный шаг истории. Возвращает то же, что операция.
    bool runOperation(bool (*op)(QTextDocument&, QTextCursor&));
    // То же с замыканием: ресайзу фотографии нужна ширина.
    bool runOperation(const std::function<bool(QTextDocument&, QTextCursor&)>& op);

    // Перестановка пунктов идёт не над курсором, а над IR: операция возвращает
    // готовый документ, и собрать его — уже наше дело.
    bool moveItem(int direction);

    // Правка над IR: операция возвращает готовый документ, дальше общий путь —
    // шаг истории, пересборка, курсор по месту в IR.
    bool applyIrEdit(const MoveResult& edit);

    // literal — вставить как есть, без разбора: Ctrl+Shift+V и всё, что попадает
    // в литеральный блок, где markdown не действует.
    void pasteMarkdown(const QString& text, bool literal);

    // anchorY — экранная высота, на которой должен остаться курсор. Меньше нуля
    // означает "просто покажи курсор".
    // Показать место правки. Как футбольный арбитр: пока действие в пределах
    // видимости, стоит на месте и не дёргает картинку; ушло за край — бежит в
    // центр событий, а не к ближайшей кромке.
    //
    // Обычный ensureCursorVisible прокручивает ровно на минимум, то есть всегда
    // ставит курсор впритык к краю окна — после отмены правки, сделанной парой
    // страниц выше, смотреть на неё приходилось в самом низу экрана.
    void showEditPlace(int scrollBefore);

    // Зазор между кареткой и кромкой окна при движении курсора. Qt прокручивает
    // ровно до касания края, и поле страницы при этом уезжает за кромку: текст,
    // который набираешь, оказывается вплотную к рамке окна. Держим тот же зазор,
    // что и поле страницы, — им же он и меряется.
    void keepCaretOffEdge();

    void rebuild(const Document& doc, int cursor, const ViewAnchor& anchor);
    void recordEdit();
    void onContentsChanged();
    void onCaretMoved();
    void tidyLeftLine(const QTextCursor& left);
    void tidySweep(const QTextCursor& caret);
    void onFileChanged(const QString& path);
    void onExternalSettled();
    // Применяет внешнее содержимое как обычную правку: один шаг истории, и undo
    // возвращает то, что было до внешнего изменения.
    void adoptExternal(const std::string& text);
    void watchFile();

    EditHistory history_;
    QString path_;

    // Перетаскивание угла фотографии. Фото прижато к левому краю колонки,
    // поэтому у всех углов работает дельта: от центра — растёт, к центру —
    // ужимается (у левых углов знак горизонтали перевёрнут).
    int imageResizeBlock_ = -1;        // номер блока; -1 — не тянем
    qreal imageResizePressX_ = 0.0;    // x нажатия в координатах вьюпорта
    qreal imageResizeSign_ = 1.0;      // +1 правые углы, -1 левые
    qreal imageResizeStart_ = 0.0;     // ширина на старте, логические пиксели
    bool imageHoverCorner_ = false;

    // Ссылки: Ctrl+клик открывает адрес в системном браузере. Qt в
    // редактируемом виджете ссылок сам не активирует (замерено пробником),
    // поэтому нажатие и отпускание сверяются здесь.
    QString pressedAnchor_;

    // Внутри хитро-отрисованной строки-фотографии каретке делать нечего:
    // любой заход внутрь неё сводится к началу строки, шаг вправо с начала
    // перепрыгивает строку целиком. Направление различается по прошлой
    // позиции.
    void snapCaretOffImage();
    int lastCaretPosition_ = 0;
    bool snappingCaret_ = false;

    // Пересборка документа и операции меняют его содержимое и потому неотличимы
    // от набора — если не поднять флаг. Без него undo записывал бы сам себя, а
    // операция заводила бы два шага вместо одного: один от contentsChanged и
    // один свой.
    bool recordingSuspended_ = false;

    QKeySequence moveUpKey_;
    QKeySequence moveDownKey_;
    // Сочетание и операция, которую оно вызывает. Списком, а не полями: их
    // становится много, и перечислять каждое в keyPressEvent — верный способ
    // однажды забыть одно.
    std::vector<std::pair<QKeySequence, bool (*)(QTextDocument&, QTextCursor&)>> bindings_;

    // Начертания живут отдельно: без выделения они меняют не документ, а формат
    // следующей буквы.
    struct InlineStyle {
        int bits = 0;
        bool (*op)(QTextDocument&, QTextCursor&) = nullptr;
    };
    std::vector<std::pair<QKeySequence, InlineStyle>> inlineBindings_;
    // Следим за файлом. Хеш нам не нужен: заметки маленькие, и содержимое
    // сравнивается побайтово — точнее и короче, чем рассуждать о коллизиях.
    // Время правки файла — только подсказка, ему мы не верим.
    QFileSystemWatcher watcher_;
    // Отстойник внешних правок: внешние редакторы пишут «обрезать → записать»,
    // и сторож стреляет на пустом файле посреди записи. Перечитываем только
    // после паузы тишины, иначе в историю попадал пустой документ.
    QTimer externalSettle_;
    bool externalEmptyRetried_ = false;
    QByteArray knownContent_;
    // Строка, на которой каретка стояла в прошлый раз: уходя со строки,
    // редактор стирает её хвостовые пробелы, а опустевшую превращает в
    // пустую строку. Держится курсором — переживает правки.
    QTextCursor lastLine_;
    bool tidying_ = false;
    // Метаданные открытой заметки. В QTextDocument их нет — редактор их не
    // видит, — поэтому от открытия до сохранения они живут здесь.
    NoteMeta meta_;
    bool externalPending_ = false;
    std::string externalText_;

    QTimer autosave_;
    QElapsedTimer sinceLastEdit_;
    QString lastComplaint_;
    // Файлы, про которые в этой сессии больше не предупреждать: человек знает,
    // что файл не в ладах с моделью, и правит его руками.
    QSet<QString> mutedComplaints_;
};

}  // namespace zametti

#endif  // ZAMETTI_EDITOR_WIDGET_H
