// ВИД ПРАВКИ КОНФИГА: config.json как текст, с подсветкой JSON.
//
// Второй наследник PlainEditView (первый — вид исходника заметки): каретка,
// поля колонки, масштаб, точки переносов, поиск, Tab пробелами и Enter с
// автоотступом — общие, здесь только своё: подсветчик JSON, свой стоп
// табуляции (editor.tabWidth) и комментирование строк по Ctrl+/
// (jsonEditing.commentKey).
//
// Модель — ZConfigFile (ядро), контроллер — SettingsController. Вид о файле не
// знает вовсе: ему дают текст и забирают текст.

#ifndef ZAMETTI_JSON_EDIT_VIEW_H
#define ZAMETTI_JSON_EDIT_VIEW_H

#include "plain_edit_view.h"

#include <QKeySequence>
#include <QList>

#include <memory>

namespace zametti {

class ZSyntaxHighlighterJSON;

class JsonEditView : public PlainEditView {
    Q_OBJECT

public:
    explicit JsonEditView(QWidget* parent = nullptr);
    ~JsonEditView() override;

    void refreshAppearance() override;

protected:
    void keyPressEvent(QKeyEvent* event) override;
    // Стоп табуляции — свой: конфиг правят с отступом в четыре пробела, а не
    // тем, чем набирают код в заметках.
    int tabStop() const override;
    QColor wrapMarkColor() const override;

private:
    // ЗАКОММЕНТИРОВАТЬ / РАСКОММЕНТИРОВАТЬ строки выделения (или строку
    // каретки). Все непустые строки уже с «//» — снимаем; иначе ставим «// »
    // в минимальной колонке отступа этих строк. Один шаг отмены.
    void toggleComment();

    std::shared_ptr<ZSyntaxHighlighterJSON> highlighter_;
    QList<QKeySequence> commentKeys_;
};

}  // namespace zametti

#endif  // ZAMETTI_JSON_EDIT_VIEW_H
