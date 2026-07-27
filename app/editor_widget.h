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
#include "note_view.h"

#include <QElapsedTimer>
#include <QString>
#include <QTimer>

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

    // Сохранить, если есть что. interactive — показывать ли окно с ошибкой.
    void save(bool interactive);

    double scrollRatio() const;
    void setScrollRatio(double ratio);

signals:
    void fileChanged(const QString& path);

private:
    void rebuild(const Document& doc, int cursor, double ratio);
    void recordEdit();
    void onContentsChanged();

    EditHistory history_;
    QString path_;

    // Пересборка документа меняет его содержимое и потому неотличима от правки
    // — если не поднять флаг. Без него undo записывал бы сам себя в историю.
    bool rebuilding_ = false;

    QTimer autosave_;
    QElapsedTimer sinceLastEdit_;
    QString lastComplaint_;
};

}  // namespace zametti

#endif  // ZAMETTI_EDITOR_WIDGET_H
