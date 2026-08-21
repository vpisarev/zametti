// ВИД ПРАВКИ ИСХОДНИКА: заметка как текст, буква в букву.
//
// ЗАЧЕМ ОТДЕЛЬНЫЙ ВИДЖЕТ, А НЕ РЕЖИМ РЕДАКТОРА. В обычном виде разметки как
// знаков нет вовсе: `**жирный**` лежит текстом «жирный» с жирным форматом, а
// таблица и формула — одним знаком U+FFFC. Показать исходник — значит показать
// ДРУГОЕ содержимое, и держать два содержимого в одном документе нельзя. Здесь
// живёт свой QTextDocument с плоским текстом; живой документ заметки не
// трогается вовсе — ни через люк getDocument(), никак.
//
// Всё общее с другими видами плоского текста — каретка, поля, точки переносов,
// масштаб, поиск, Tab/Enter вне списков, Esc и дно стека — в базе
// PlainEditView (refactor3). Здесь — то, что знает только исходник: подсветка
// markdown, плашки под блоками кода, списки клавишами (Enter продолжает пункт,
// Tab/Shift+Tab двигают пункт, toggleTaskKey переключает задачу).
//
// СВОЙ БУФЕР ОТМЕНЫ (просьба владельца): пока идёт режим, Ctrl+Z отменяет
// правку ТЕКСТА, шаг за шагом, как в обычном текстовом редакторе. В стек отмены
// заметки вся работа ляжет ОДНИМ шагом — при возврате, наложением
// (ZDocument::applySourceText).

#ifndef ZAMETTI_MARKDOWN_EDIT_VIEW_H
#define ZAMETTI_MARKDOWN_EDIT_VIEW_H

#include "document.h"
#include "plain_edit_view.h"

#include <QKeySequence>
#include <QList>

#include <memory>

namespace zametti {

class ZSyntaxHighlighterMD;

class MarkdownEditView : public PlainEditView {
    Q_OBJECT

public:
    explicit MarkdownEditView(QWidget* parent = nullptr);
    ~MarkdownEditView() override;

    // Показать исходник и встать кареткой в это место (история — заново).
    void showSource(const QString& markdown, SourcePos caret);
    // Текст вида буква в букву (см. PlainEditView::text).
    QString source() const { return text(); }
    // Где стоит каретка сейчас — в тех же единицах, в которых её принимали.
    SourcePos caretPos() const;

    void refreshAppearance() override;

protected:
    // Клавиши режима (см. раздел «клавиши» в .cpp): Enter продолжает пункт
    // списка и держит отступ, Shift+Enter продолжает пункт строкой содержимого,
    // Tab/Shift+Tab двигают пункт (вне списка — пробелы до стопа, не знак
    // табуляции: таб в markdown значим), toggleTaskKey переключает задачу.
    // Всё — правки текста, по одному шагу отмены.
    void keyPressEvent(QKeyEvent* event) override;
    void pressEnter(bool shift) override;
    void pressTab(bool back) override;
    // Плашки под блоками кода — подложкой под найденным.
    void extraOverlays(QList<QTextEdit::ExtraSelection>& shown) override;

private:
    void toggleTasks();

    std::shared_ptr<ZSyntaxHighlighterMD> highlighter_;
    QList<QKeySequence> toggleTaskKeys_;
};

}  // namespace zametti

#endif  // ZAMETTI_MARKDOWN_EDIT_VIEW_H
