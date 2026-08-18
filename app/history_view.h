// Вид режима истории: баннер с кнопками сверху и разность слепка построчно.
//
// Модель — ZNoteTimeline (ядро): что показано, с чем сравнено, документ
// разности. Здесь только показ: текст разности в поле только для чтения с
// «+»/«−» на поле, удержание места при смене базы и слепка, F4 по изменениям,
// клавиши режима, копирование из прошлого. Живой заметки этот вид не касается
// вовсе — она в редакторе, а редактор в режиме истории просто скрыт.
//
// Вид держит показанный ZDocument за ручку сам: модель вправе выбросить свои
// документы (смена слепка, облика), а виджет ещё смотрит на прежний до
// возврата в цикл событий (урок этапа 10: документ, вырванный из-под Qt
// синхронно, роняет программу).

#ifndef ZAMETTI_HISTORY_VIEW_H
#define ZAMETTI_HISTORY_VIEW_H

#include "history_panel.h"
#include "note_view.h"
#include "znote_timeline.h"

#include <QWidget>

#include <memory>
#include <vector>

namespace zametti {

// Текст разности. Наследник NoteView: масштаб, поиск с подсветкой, показ места
// в золотом сечении — общие с редактором, здесь только своё.
class DiffTextView : public NoteView {
    Q_OBJECT

public:
    explicit DiffTextView(QWidget* parent = nullptr);

    // Показать таймлайн: документ разности текущей пары (слепок × база).
    // Повторный вызов с тем же таймлайном — перечитать документ у модели
    // (слепок или база сменились), удержав место по строке слепка.
    void attach(std::shared_ptr<ZNoteTimeline> timeline);
    // Ничего не показывать: прежний документ отпускается отложенно.
    void detach();
    std::shared_ptr<ZNoteTimeline> timeline() const { return timeline_; }

    // Шаг к следующему/предыдущему изменённому КУСКУ, циклически. Кусок —
    // непрерывный ряд «−» и следующий за ним ряд «+» (любой из рядов может быть
    // пуст). Текущий кусок показывается оранжевой полосой на поле, а не
    // выделением: выделение забивало бы заливку строк, и оно нужно человеку
    // для копирования (просьба владельца). false — изменений нет вовсе.
    bool stepChange(bool forward);
    // Текущий кусок — блоки [first, last]; first < 0 — не выбран.
    struct Hunk {
        int first = -1;
        int last = -1;
        // Из чего состоит: Added — только добавленные, Removed — только
        // убранные, Changed — и те и другие. Этим же цветом рисуется полоса.
        diff::Mark kind = diff::Mark::Same;
        bool valid() const { return first >= 0; }
    };
    Hunk currentHunk() const { return hunk_; }

    // Строка СЛЕПКА у верхней кромки окна — ею держится место, ею же оно
    // проверяется. -1 — нечего держать.
    int lineOnTop();

signals:
    // Печатающая клавиша в слепке: не восстанавливаем ничего (решение
    // владельца), баннер подсвечивает «Восстановить эту».
    void editRefused();
    void leaveRequested();
    void stepBackRequested();
    void stepForwardRequested();

protected:
    // Найденное лежит у слепка (как у заметки — при заметке).
    NoteSearch& searchCache() override;
    const NoteSearch& searchCache() const override;
    // «+»/«−» на поле у каждой видимой добавленной/убранной строки.
    void paintBlockMargin(QPainter& painter, const QTextBlock& block, const QRectF& rect) override;
    void keyPressEvent(QKeyEvent* event) override;
    bool event(QEvent* event) override;
    // Копия — сырой текст строк: блоки документа разности и есть строки
    // markdown, и экранировать в них нечего.
    QMimeData* createMimeDataFromSelection() const override;

private:
    std::shared_ptr<ZNoteTimeline> timeline_;
    ZDocument shown_;                  // показанный документ — держим живым
    std::vector<ZDocument> retiring_;  // отпущенные, но ещё не умершие
    Hunk hunk_;                        // куда привёл F4
    // Кусок, начинающийся с блока start: ряд одной метки, за ним ряд другой.
    Hunk hunkFrom(int start) const;
    void present(int keepLine, int keepOffset);
    void retire(ZDocument previous);
};

// Баннер + текст. Сигналы баннера и текста — наружу, контроллеру.
class HistoryView : public QWidget {
    Q_OBJECT

public:
    explicit HistoryView(QWidget* parent = nullptr);

    void attach(std::shared_ptr<ZNoteTimeline> timeline);
    void detach();
    bool isAttached() const { return text_->timeline() != nullptr; }
    std::shared_ptr<ZNoteTimeline> timeline() const { return text_->timeline(); }
    // Модель сменила слепок или базу — перечитать документ и переписать баннер.
    void refresh();

    DiffTextView& textView() { return *text_; }
    HistoryBanner& banner() { return *banner_; }

signals:
    void leaveRequested();
    void restoreRequested();
    void baseChanged(bool fresh);
    void stepBackRequested();
    void stepForwardRequested();
    void editRefused();

private:
    HistoryBanner* banner_;
    DiffTextView* text_;
    void syncBanner();
};

}  // namespace zametti

#endif  // ZAMETTI_HISTORY_VIEW_H
