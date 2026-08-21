// ZSyntaxHighlighterJSON — подсветка JSON-конфига поверх QTextDocument.
//
// Для редактора настроек внутри программы (refactor3). Правила те же, что у
// разборщика конфига (stripJsonSugar): «//» до конца строки — комментарий,
// но только ВНЕ строк ("https://…" — строка); внутри строки экранированная
// кавычка учитывается. Закрытая строка, за которой (через пробелы) стоит
// двоеточие, — ключ; прочие строки — значения; числа; true/false/null целыми
// словами; скобки, двоеточия и запятые — пунктуация. Ручной сканер строки без
// регэкспов: O(длины строки). Состояний блока нет: строки JSON не переносятся,
// а /* */ конфиг не понимает по замыслу.
//
// Живёт в ядре (QtGui, виджетов не тянет), цвета — своя секция настроек
// (ZSettings::JsonEditing) параметром по значению, как у ZSyntaxHighlighterMD.
// Подсветка — форматы раскладки, содержимое и стек отмены не трогаются.

#ifndef ZAMETTI_JSON_HIGHLIGHTER_H
#define ZAMETTI_JSON_HIGHLIGHTER_H

#include "settings.h"

#include <QSyntaxHighlighter>
#include <QTextCharFormat>

namespace zametti {

class ZSyntaxHighlighterJSON : public QSyntaxHighlighter {
    Q_OBJECT

public:
    ZSyntaxHighlighterJSON(QTextDocument* document, ZSettings::JsonEditing rules);

protected:
    void highlightBlock(const QString& text) override;

private:
    ZSettings::JsonEditing rules_;
    QTextCharFormat key_;
    QTextCharFormat string_;
    QTextCharFormat number_;
    QTextCharFormat keyword_;
    QTextCharFormat comment_;
    QTextCharFormat punctuation_;
};

}  // namespace zametti

#endif  // ZAMETTI_JSON_HIGHLIGHTER_H
