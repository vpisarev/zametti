// ВИД ПРАВКИ ИСХОДНИКА: заметка как текст, буква в букву.
//
// ЗАЧЕМ ОТДЕЛЬНЫЙ ВИДЖЕТ, А НЕ РЕЖИМ РЕДАКТОРА. В обычном виде разметки как
// знаков нет вовсе: `**жирный**` лежит текстом «жирный» с жирным форматом, а
// таблица и формула — одним знаком U+FFFC. Показать исходник — значит показать
// ДРУГОЕ содержимое, и держать два содержимого в одном документе нельзя. Здесь
// живёт свой QTextDocument с плоским текстом; живой документ заметки не
// трогается вовсе — ни через люк getDocument(), никак.
//
// QPlainTextEdit, а не QTextEdit: у него ленивая вёрстка (QPlainTextDocument-
// Layout), и цена показа не растёт с размером заметки — замер стенда paste
// показывал ровно это. Для сырого markdown на сотню мегабайт (цель владельца)
// другой вёрстки и не надо.
//
// СВОЙ БУФЕР ОТМЕНЫ (просьба владельца): пока идёт режим, Ctrl+Z отменяет
// правку ТЕКСТА, шаг за шагом, как в обычном текстовом редакторе. В стек отмены
// заметки вся работа ляжет ОДНИМ шагом — при возврате, наложением
// (ZDocument::applySourceText).

#ifndef ZAMETTI_MARKDOWN_EDIT_VIEW_H
#define ZAMETTI_MARKDOWN_EDIT_VIEW_H

#include "caret_blink.h"
#include "document.h"

#include <QKeySequence>
#include <QList>
#include <QPlainTextEdit>

#include <memory>

class QPainter;

namespace zametti {

class ZSyntaxHighlighterMD;

class MarkdownEditView : public QPlainTextEdit {
    Q_OBJECT

public:
    explicit MarkdownEditView(QWidget* parent = nullptr);
    ~MarkdownEditView() override;

    // Показать исходник и встать кареткой в это место. Своя история правки
    // начинается заново: текст другой заметки — не продолжение прежней.
    void showSource(const QString& markdown, SourcePos caret);
    QString source() const { return toPlainText(); }
    // Где стоит каретка сейчас — в тех же единицах, в которых её принимали.
    SourcePos caretPos() const;

    // Перечитать оформление: гарнитура, кегль, цвета, подсветка.
    void refreshAppearance();

    // --- МАСШТАБ У РЕЖИМА СВОЙ --------------------------------------------
    //
    // Решение владельца: Ctrl+= и Ctrl+- в исходнике не трогают масштаб
    // обычного вида и наоборот. Иначе выходило вот что: человек увеличивает
    // текст в исходнике, отжимает [M] — и заметка вдруг стала крупнее, хотя её
    // масштаб он не трогал. Своё число живёт в state.json (markdownZoom).
    void applyZoom(qreal zoom);
    qreal zoom() const { return zoom_; }

    // --- поиск (Ctrl+F живёт у окна, механика — здесь) ---------------------
    //
    // Своя, а не общая с NoteView: там найденное живёт при заметке и адресуется
    // блоками и объектами, здесь — плоский текст. Общего у них ровно ноль,
    // кроме слова «поиск».
    int findMatches(const QString& text, bool caseSensitive);
    int matchCount() const { return int(matches_.size()); }
    int currentMatch() const { return current_; }
    // Шаг по найденному, циклически; false — не найдено ничего.
    bool stepMatch(int direction);
    void clearMatches();
    // Заменить текущее вхождение / все. Обе — обычная правка текста, и
    // отменяются буфером режима.
    bool replaceCurrent(const QString& with);
    int replaceAll(const QString& text, bool caseSensitive, const QString& with);

signals:
    void leaveRequested();   // Esc — выйти из режима
    // Ctrl+Z на ДНЕ своего стека отмены: отменять в тексте больше нечего, и
    // отмена отдаётся заметке — как в обычном виде, где дно стека ведёт в
    // слепки журнала. Решает контроллер: вид знает только, что стек пуст.
    void undoExhausted();

protected:
    // КАРЕТКА — СВОЯ, цвета и толщины из настроек, как в обычном виде (просьба
    // владельца): штатная рисуется инверсией и не красится (qt-caret-facts),
    // поэтому она погашена (setCursorWidth(0)), а своя рисуется поверх
    // штатной отрисовки и мигает общим CaretBlink.
    void paintEvent(QPaintEvent* event) override;
    void focusInEvent(QFocusEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    // Поля вьюпорта: колонка исходника не растягивается на всю ширину широкого
    // окна (просьба владельца), а стоит посередине, как в обычном виде.
    void resizeEvent(QResizeEvent* event) override;
    // Клавиши режима (см. раздел «клавиши» в .cpp): Enter продолжает пункт
    // списка и держит отступ, Shift+Enter продолжает пункт строкой содержимого,
    // Tab/Shift+Tab двигают пункт (вне списка — пробелы до стопа, не знак
    // табуляции: таб в markdown значим), toggleTaskKey переключает задачу,
    // Esc просит выйти. Всё — правки текста, по одному шагу отмены.
    void keyPressEvent(QKeyEvent* event) override;

private:
    // ТОЧКИ У ПЕРЕНЕСЁННЫХ СТРОК (просьба владельца): на левом поле, у начала
    // каждой визуальной строки, кроме первой в абзаце, — точка на половине
    // высоты буквы. Поле — отдельный дочерний виджет в отступе вьюпорта (приём
    // Qt для номеров строк): рисуется сам, от текста и каретки не зависит.
    class WrapMarks;
    void paintWrapMarks(QPainter& painter, const QRect& area);
    void placeWrapMarks();

    void showCaret();
    QRect caretRect() const;
    // Колонка каретки заново, без штатного курсора (см. .cpp): тот же ход, что
    // NoteView::repaintOverNativeCaret.
    void repaintOverNativeCaret(QPainter& painter);
    void pressEnter(bool shift);
    void pressTab(bool back);
    void toggleTasks();

    // ПОДСВЕТКИ ОДНИМ СПИСКОМ. У QPlainTextEdit extraSelections один на всех, и
    // держать их порознь нельзя: кто поставит вторым, сотрёт первого. Плашки
    // блоков кода и найденное собираются вместе — и только по ВИДИМОМУ.
    void refreshOverlays();
    void applyContentWidth();

    std::shared_ptr<ZSyntaxHighlighterMD> highlighter_;
    WrapMarks* wrapMarks_ = nullptr;
    CaretBlink caretBlink_;
    QList<QKeySequence> toggleTaskKeys_;
    qreal zoom_ = 1.0;
    int viewportMargin_ = 0;
    // Найденное — позициями в плоском тексте; текущее — номер в этом списке.
    std::vector<int> matches_;
    int needle_ = 0;
    int current_ = -1;
};

}  // namespace zametti

#endif  // ZAMETTI_MARKDOWN_EDIT_VIEW_H
