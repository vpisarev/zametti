// ZSyntaxHighlighterMD — подсветка СЫРОГО markdown поверх QTextDocument.
//
// ЗАЧЕМ. Первый шаг к режиму правки исходника (решение владельца, сессия 7):
// сегодня подсвечиваются строки разности в режиме истории — их читать стало
// удобнее, — а завтра тот же класс расцветит документ с сырым markdown, в
// котором человек правит текст как в файле. Живёт в ядре (QSyntaxHighlighter —
// QtGui, виджетов не тянет), цвета и ступени берёт из стиля документа.
//
// ЧТО РАСЦВЕЧИВАЕТСЯ (правило одно на строку, состояние — только у забора кода):
//   * заголовки `# …` — жирным и на ступень крупнее (headingStep, лестница
//     кеглей: абсолютных размеров в документе нет — масштаб один setDefaultFont);
//   * маркеры списков `- ` `* ` `+ `, номера `1. ` `1) `, задачи `- [ ] ` /
//     `-[x] ` — акцентным цветом и жирным;
//   * формулы `$…$` и `$$…$$` в строке — акцентным цветом (не жирным);
//   * ссылки `[текст](адрес)`, `<адрес>` и голые адреса `https://…`, `www.…`
//     (автоссылки GFM; хвостовая пунктуация не в счёт) — цветом link с
//     подчёркиванием;
//     картинки `![подпись](файл)` — цветом image, адрес — с подчёркиванием;
//   * `**жирный**` / `__жирный__` — жирным; `_курсив_` / `*курсив*` — курсивом;
//   * `` `код` `` в строке — на подложке codeBackground; блок кода между
//     заборами ``` / ~~~ — только СОСТОЯНИЕ блока (InFence): плашку во всю
//     колонку кладёт вид, а знаки подсветчик там не красит — двойная подложка
//     была вдвое темнее (нашёл владелец).
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

    // Состояние блока (previousBlockState/currentBlockState): 0 — обычная
    // строка; иначе «внутри забора кода», и в состоянии же лежит КОЛОНКА
    // забора (отступ ```): 1 + колонка. Виду она нужна, чтобы класть плашку
    // кода с того же отступа, что и забор, — блок кода внутри пункта списка
    // читается вложенным, а не во всю колонку (просьба владельца).
    enum State { Plain = 0 };
    static int fenceState(int column) { return 1 + qMax(0, column); }
    static bool inFence(int state) { return state >= 1; }
    static int fenceColumn(int state) { return state >= 1 ? state - 1 : 0; }

protected:
    void highlightBlock(const QString& text) override;

private:
    ZSettings::MarkdownHighlighting rules_;
    QTextCharFormat accent_;    // формулы — акцентом
    QTextCharFormat marker_;    // маркеры, номера, задачи, `#` — акцентом и жирным
    QTextCharFormat code_;      // код в строке — подложка
    QTextCharFormat heading_;   // заголовки — жирным и на ступень крупнее
    QTextCharFormat link_;
    QTextCharFormat image_;
    QTextCharFormat bold_;
    QTextCharFormat italic_;
    QRegularExpression fence_;
    QRegularExpression heading_re_;
    QRegularExpression task_;
    QRegularExpression bullet_;
    QRegularExpression ordered_;
    QRegularExpression codeSpan_;
    QRegularExpression imageLink_;
    QRegularExpression link_re_;
    QRegularExpression autoLink_;
    QRegularExpression bareUrl_;
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
