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

#include "document.h"

#include <QPlainTextEdit>

#include <memory>

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

protected:
    // Tab заполняет ПРОБЕЛАМИ до ближайшего стопа (editor.codeTabWidth), а не
    // ставит знак табуляции: таб в markdown значим (в начале строки это блок
    // кода с отступом), и набирать его случайно нельзя. Shift+Tab снимает до
    // предыдущего стопа; на выделении из нескольких строк оба двигают строки.
    void keyPressEvent(QKeyEvent* event) override;

private:
    void showMatchHighlights();

    std::shared_ptr<ZSyntaxHighlighterMD> highlighter_;
    // Найденное — позициями в плоском тексте; текущее — номер в этом списке.
    std::vector<int> matches_;
    int needle_ = 0;
    int current_ = -1;
};

}  // namespace zametti

#endif  // ZAMETTI_MARKDOWN_EDIT_VIEW_H
