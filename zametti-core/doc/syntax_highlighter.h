// ZSyntaxHighlighterMD — подсветка СЫРОГО markdown поверх QTextDocument.
//
// ЗАЧЕМ. Первый шаг к режиму правки исходника (решение владельца, сессия 7):
// сегодня подсвечиваются строки разности в режиме истории — их читать стало
// удобнее, — а завтра тот же класс расцветит документ с сырым markdown, в
// котором человек правит текст как в файле. Живёт в ядре (QSyntaxHighlighter —
// QtGui, виджетов не тянет), цвета и ступени берёт из стиля документа.
//
// ЧТО РАСЦВЕЧИВАЕТСЯ (правило одно на строку, состояние — только у забора кода):
//   * заголовки `# …` — на ступень крупнее (headingStep, лестница
//     кеглей: абсолютных размеров в документе нет — масштаб один setDefaultFont);
//   * маркеры списков `- ` `* ` `+ `, номера `1. ` `1) `, задачи `- [ ] ` /
//     `-[x] ` — акцентным цветом (accent);
//   * формулы `$…$` и `$$…$$` в строке — акцентным цветом;
//   * `**жирный**` / `__жирный__` — жирным; `_курсив_` / `*курсив*` — курсивом;
//   * `` `код` `` в строке и блок кода между заборами ``` / ~~~ — на подложке
//     codeBackground секции; забор — состояние блока, оно живёт между строками.
//
// СТРОКИ РАЗНОСТИ «−» (DiffMarkProperty == Removed) НЕ ПОДСВЕЧИВАЮТСЯ: они не
// часть показанного слепка (решение владельца — просто чёрным по красному), но
// состояние забора сквозь них проносится, иначе код после убранной строки
// внутри блока потерял бы подложку.
//
// Подсветка — ФОРМАТЫ РАСКЛАДКИ (QTextLayout::formats), а не формат знаков:
// содержимое документа не меняется, стек отмены чист, а форматы знаков
// сборщика (шрифт строк) остаются как были.

#ifndef ZAMETTI_SYNTAX_HIGHLIGHTER_H
#define ZAMETTI_SYNTAX_HIGHLIGHTER_H

#include "settings.h"

#include <QRegularExpression>
#include <QSyntaxHighlighter>
#include <QTextCharFormat>

#include <memory>

namespace zametti {

class ZSyntaxHighlighterMD : public QSyntaxHighlighter {
    Q_OBJECT

public:
    // Прикрепляется к документу (становится его ребёнком). Цвета и ступень
    // заголовков — своя часть настроек (секция markdownHighlighting) параметром,
    // как у всех классов проекта; baseStep — ступень кегля строк документа (у
    // строк разности diffStep, у сырого markdown 0): заголовки крупнее неё.
    ZSyntaxHighlighterMD(QTextDocument* document, ZSettings::MarkdownHighlighting rules,
                         int baseStep = 0);

    // Состояния блока (previousBlockState/currentBlockState).
    enum State { Plain = 0, InFence = 1 };

protected:
    void highlightBlock(const QString& text) override;

private:
    ZSettings::MarkdownHighlighting rules_;
    QTextCharFormat accent_;    // маркеры, номера, задачи, формулы
    QTextCharFormat code_;      // код — подложка
    QTextCharFormat heading_;   // заголовки — на ступень крупнее
    QTextCharFormat bold_;
    QTextCharFormat italic_;
    QRegularExpression fence_;
    QRegularExpression heading_re_;
    QRegularExpression task_;
    QRegularExpression bullet_;
    QRegularExpression ordered_;
    QRegularExpression codeSpan_;
    QRegularExpression displayMath_;
    QRegularExpression inlineMath_;
    QRegularExpression boldStar_;
    QRegularExpression boldUnder_;
    QRegularExpression italicStar_;
    QRegularExpression italicUnder_;
    // Спаны кода и формул — атомарны: внутри них ни жирного, ни курсива, ни
    // маркеров; занятые места помечаются, и остальные правила их обходят.
    void applySpans(const QString& text, const QRegularExpression& re, const QTextCharFormat& format,
                    QVector<bool>& taken, const QTextCharFormat& base);
};

}  // namespace zametti

#endif  // ZAMETTI_SYNTAX_HIGHLIGHTER_H
