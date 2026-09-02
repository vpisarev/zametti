// PlainEditView — ОБЩАЯ БАЗА ВИДОВ ПЛОСКОГО ТЕКСТА: исходник заметки
// (MarkdownEditView) и конфиг программы (JsonEditView).
//
// Вынесено из MarkdownEditView (refactor3, просьба владельца: «курсор должен
// выглядеть и работать так же, как при редактировании заметок — если не
// унифицировано, унифицировать»). Здесь живёт всё, что у обоих видов одно:
//
//   * QPlainTextEdit с ленивой вёрсткой (цена показа не растёт с размером) и
//     переносом по ширине окна;
//   * СВОЯ КАРЕТКА цвета и толщины из настроек, мигающая общим CaretBlink:
//     штатная погашена (setCursorWidth(0)), колонка каретки перерисовывается
//     поверх штатной отрисовки (repaintOverNativeCaret), чтобы чужая чёрная
//     черта не пережила ни одного кадра (qt-caret-facts);
//   * точки у перенесённых строк на левом поле (WrapMarks), колонка ограничена
//     той же шириной, что и обычный вид (applyContentWidth), свой масштаб;
//   * setText/text: текст по блокам (toPlainText() подменяет U+00A0 — нашёл
//     владелец), своя история правки с чистого листа на каждый showSource;
//   * клавиши: Esc → leaveRequested, Ctrl+Z на дне своего стека →
//     undoExhausted (решает контроллер), Enter → автоотступ предыдущей строки
//     (pressEnter — виртуальный, исходник знает списки), Tab/Shift+Tab →
//     пробелы до стопа / снять до стопа, выделение в несколько строк — единым
//     сдвигом (pressTab — виртуальный, исходник двигает пункты);
//   * поиск и замена по плоскому тексту (TextSearchTarget), подсветки одним
//     списком extraSelections с хуком extraOverlays для наследников.
//
// Наследник добавляет подсветчик, свои клавиши и цвета; база в конструкторе
// refreshAppearance() НЕ зовёт (виртуальный) — наследник зовёт его последним.

#ifndef ZAMETTI_PLAIN_EDIT_VIEW_H
#define ZAMETTI_PLAIN_EDIT_VIEW_H

#include "caret_blink.h"
#include "text_search_target.h"

#include <QList>
#include <QPlainTextEdit>
#include <QTimer>
#include <QTextEdit>

#include <vector>

class QPainter;
class QTextBlock;

namespace zametti {

class PlainEditView : public QPlainTextEdit, public TextSearchTarget {
    Q_OBJECT

public:
    explicit PlainEditView(QWidget* parent = nullptr);
    ~PlainEditView() override;

    // Показать текст и встать кареткой в строку/колонку. Своя история правки
    // начинается заново: подстановка текста — не правка человека.
    void setText(const QString& text, int line, int column);
    // Текст буква в букву, по блокам (см. заголовок).
    QString text() const;
    int caretLine() const;
    int caretColumn() const;

    // Перечитать оформление: палитра, шрифт, стоп табуляции, поля. Наследник
    // дополняет (подсветчик, сочетания) и зовёт базу.
    virtual void refreshAppearance();

    // МАСШТАБ У ВИДА СВОЙ (решение владельца для исходника; у конфига — тоже).
    void applyZoom(qreal zoom);
    qreal zoom() const { return zoom_; }

    // --- TextSearchTarget ---------------------------------------------------
    int findMatches(const Query& query) override;
    bool matchesCapped() const override { return capped_; }
    int matchCount() const override { return int(matches_.size()); }
    int currentMatch() const override { return current_; }
    void stepMatch(int direction) override;
    // Встать на N-е найденное (список результатов адресует находки номером).
    void goToMatch(int index);
    void clearMatches() override;
    bool canReplace() const override { return !isReadOnly(); }
    bool replaceCurrentMatch(const QString& with) override;
    int replaceAllMatches(const Query& query, const QString& with) override;
    QString searchPreset() const override;
    QWidget& searchWidget() override { return *this; }

signals:
    void leaveRequested();   // Esc — выйти из режима
    // Ctrl+Z на ДНЕ своего стека отмены: отменять в тексте больше нечего.
    // Решает контроллер: вид знает только, что стек пуст.
    void undoExhausted();
    // Найденное пересчитано само (после правки): полосе поиска пора обновить
    // счётчик — число вхождений и их места изменились.
    void matchesChanged();
    // Ctrl+колесо — просьба шагнуть масштаб; правит ступень окно, той же
    // развилкой, что и клавиши (то же правило, что у NoteView).
    void zoomStepRequested(int delta);

protected:
    void paintEvent(QPaintEvent* event) override;
    void focusInEvent(QFocusEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

    // Стоп табуляции в пробелах: им ставит Tab и им рисуются литеральные табы.
    virtual int tabStop() const;
    // Цвет точек у перенесённых строк.
    virtual QColor wrapMarkColor() const;
    // Enter (shift — Shift+Enter): база — перенос с отступом строки, обрезанным
    // по каретке (в колонке 0 строка просто уезжает вниз). Одна скобка отмены.
    virtual void pressEnter(bool shift);
    // Tab / Shift+Tab: база — одна строка: пробелы до стопа в месте каретки /
    // снять до стопа ведущих пробелов; выделение в несколько строк — единый
    // сдвиг на стоп, пустые строки не трогаются, после сдвига выделение
    // охватывает строки целиком (второй Tab двигает их же).
    virtual void pressTab(bool back);
    // Подсветки наследника (плашки кода и т. п.) — ДО найденного; только по
    // видимому: пересчёт идёт на прокрутку, правку и размер.
    virtual void extraOverlays(QList<QTextEdit::ExtraSelection>& shown) { (void)shown; }

    // --- помощники для наследников ------------------------------------------
    // Строки выделения целиком: [first, last]; выделения нет — строка каретки.
    // Выделение, кончающееся ровно в начале строки, эту строку не захватывает.
    struct LineSpan {
        int first = 0;
        int last = 0;
    };
    static LineSpan spanOf(const QTextCursor& at);
    // Отступ строки знаками — дословно, табы вместе с пробелами.
    static QString indentStringOf(const QTextBlock& block);
    // Строки [first, last]: delta > 0 — вставить столько пробелов в начало,
    // delta < 0 — снять до стольких ведущих пробелов; пустые строки не
    // трогаются. Без своей скобки отмены — зовущий открывает её.
    void indentLines(int first, int last, int delta);
    // Выделить строки [first, last] целиком; forward — якорь в начале.
    void selectLines(int first, int last, bool forward);
    // Пересчитать подсветки (плашки наследника + найденное).
    void refreshOverlays();

private:
    // Плоский текст вида — по нему ищут. Свежесть держит ревизия документа.
    const QString& flatText();

    class WrapMarks;
    void paintWrapMarks(QPainter& painter, const QRect& area);
    void placeWrapMarks();
    void showCaret();
    QRect caretRect() const;
    void repaintOverNativeCaret(QPainter& painter);
    // fromResize — зовёт resizeEvent: пересчитывать вёрстку не нужно, базовый
    // обработчик идёт следом и сделает это сам.
    void applyContentWidth(bool fromResize = false);

    WrapMarks* wrapMarks_ = nullptr;
    CaretBlink caretBlink_;
    qreal zoom_ = 1.0;
    int viewportMargin_ = 0;
    // Идёт пересчёт вёрстки: защита от входа в него из его же сигналов.
    bool rewrapping_ = false;
    // Пересчёт подсветок, отложенный до возврата в цикл событий: сигналы,
    // по которым он нужен, приходят изнутри чужой работы (см. конструктор).
    QTimer overlaysSoon_;
    // Повтор поиска после правки — через короткую паузу: он читает весь текст.
    QTimer searchSoon_;
    // Найденное — местами в плоском тексте; текущее — номер в этом списке.
    // Длина у каждого своя: у выражения находки разной длины. Совпадение
    // хранится при находке — из него замена разворачивает группы.
    struct Match {
        int offset = 0;
        int length = 0;
        QRegularExpressionMatch match;
    };
    std::vector<Match> matches_;
    // Запрос помнится: замена перезапускает поиск сама.
    Query query_;
    // ПЛОСКИЙ ТЕКСТ — С КЭШЕМ ПО РЕВИЗИИ. toPlainText() у заметки владельца в
    // 8.4 МБ стоит 15 мс (замер), а поиск идёт на КАЖДУЮ букву запроса —
    // документ при этом не меняется вовсе. Ревизия Qt меняется на любой правке,
    // и по ней кэш освежается сам.
    QString flat_;
    int flatRevision_ = -1;
    // Остановились по потолку: найденного больше, чем в списке.
    bool capped_ = false;
    int current_ = -1;
};

}  // namespace zametti

#endif  // ZAMETTI_PLAIN_EDIT_VIEW_H
