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
#include <QFileSystemWatcher>
#include <QKeySequence>
#include <QString>
#include <QTimer>

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

    // Доля прокрутки — только для запоминания места между запусками. Внутри
    // правок она не годится: документ пересобирается целиком, его высота от
    // правки к правке меняется, и доля от новой высоты каждый раз попадает не
    // туда. Замер: пункт за пунктом заметка уползала вверх на ~25 px за правку.
    double scrollRatio() const;
    void setScrollRatio(double ratio);

    // Экранная высота курсора — то, за что цепляется взгляд. Пересборка держит
    // именно её, а не долю: строка под курсором остаётся на месте.
    int cursorAnchor() const;

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

private:
    // Выполняет операцию, доводит документ до вида, который построил бы
    // сборщик, и заводит отдельный шаг истории. Возвращает то же, что операция.
    bool runOperation(bool (*op)(QTextDocument&, QTextCursor&));

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
    void rebuild(const Document& doc, int cursor, int anchorY);
    void recordEdit();
    void onContentsChanged();
    void onFileChanged(const QString& path);
    // Применяет внешнее содержимое как обычную правку: один шаг истории, и undo
    // возвращает то, что было до внешнего изменения.
    void adoptExternal(const std::string& text);
    void watchFile();

    EditHistory history_;
    QString path_;

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
    QByteArray knownContent_;
    bool externalPending_ = false;
    std::string externalText_;

    QTimer autosave_;
    QElapsedTimer sinceLastEdit_;
    QString lastComplaint_;
};

}  // namespace zametti

#endif  // ZAMETTI_EDITOR_WIDGET_H
