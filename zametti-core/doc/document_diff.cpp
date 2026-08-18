// Документ разности — глагол ZDocument.
//
// СТРОКА СРАВНЕНИЯ СТАНОВИТСЯ БЛОКОМ, а что с ней стало — свойством блока
// (DiffMarkProperty): по нему вид рисует «+»/«−» на поле и по нему ходит F4.
// Убранная строка показывается ЯВНО, своим текстом (решение владельца, сессия
// 7: два сравнения вместо четырёх обязаны давать столько же сведений, сколько
// давали четыре, — значит, старый текст виден без всякого переключения).
// Изменённая строка — пара блоков «− старая / + новая», как в unified diff.
//
// Заливка строки ставится здесь же, при сборке, из стиля документа: облик у
// нас запечён в документе (масштаб — один setDefaultFont, пересборки нет), и
// документ разности живёт по тому же правилу, что и заметка. Показывать его —
// дело вида, собирать — дело документа; второго строителя нет.

#include "document_impl.h"

#include "diff.h"
#include "doc_model.h"
#include "document_builder.h"
#include "settings.h"

#include <QFontMetricsF>
#include <QTextBlock>
#include <QTextBlockFormat>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextFrame>

namespace zametti {

namespace {

QColor tintOf(diff::Mark mark, const ZDocStyle& style) {
    QColor colour;
    switch (mark) {
        case diff::Mark::Added: colour = style.diffAdded(); break;
        case diff::Mark::Removed: colour = style.diffRemoved(); break;
        case diff::Mark::Changed: colour = style.diffChanged(); break;
        case diff::Mark::Same: return QColor();
    }
    colour.setAlpha(qBound(0, style.diffTint(), 255));
    return colour;
}

}  // namespace

qreal ZDocument::diffGutterWidth(const ZDocStyle& style) {
    // Два знака кода: под глиф и воздух рядом с ним. Считается ОТ ШРИФТА КОДА
    // базового кегля — той же меркой, что и поля сборщика (sideMargin × «A»).
    QFont code = layoutBaseFont(style);
    if (!style.codeFamily().isEmpty()) code.setFamily(QString(style.codeFamily()));
    code.setPointSizeF(code.pointSizeF() * fontStepFactor(style.codeStep()));
    return 2.0 * QFontMetricsF(code).horizontalAdvance(QLatin1Char('0'));
}

ZDocument ZDocument::fromDiff(const diff::Result& result, std::shared_ptr<const ZDocStyle> style,
                              QVector<int>* rowOfBlock) {
    ZDocument out;
    if (style != nullptr) out.setStyle(std::move(style));
    const ZDocStyle& look = out.style();
    QTextDocument& target = out.d_->text;
    // Документ разности только показывают: цепочка отмены ему не нужна, а
    // сборка в неё попадать не должна.
    target.setUndoRedoEnabled(false);
    if (rowOfBlock != nullptr) rowOfBlock->clear();

    // Поля — как у сборщика заметки, плюс слева место под «+»/«−»: глиф стоит
    // ПЕРЕД строкой, на поле, а не в тексте — копирование остаётся чистым, и
    // мягкий перенос длинной строки глифу не мешает.
    const qreal charUnit = layoutCharUnit(look);
    const qreal lineUnit = layoutLineUnit(look);
    QTextFrameFormat rootFormat = target.rootFrame()->frameFormat();
    rootFormat.setLeftMargin(look.sideMargin() * charUnit + diffGutterWidth(look));
    rootFormat.setRightMargin(look.sideMargin() * charUnit);
    rootFormat.setTopMargin(look.verticalMargin() * lineUnit);
    rootFormat.setBottomMargin(look.verticalMargin() * lineUnit);
    target.rootFrame()->setFrameFormat(rootFormat);

    QTextCharFormat text;
    if (!look.codeFamily().isEmpty()) text.setFontFamilies({QString(look.codeFamily())});
    setFontStep(text, look.codeStep());

    QTextCursor caret(&target);
    bool first = true;
    const auto addBlock = [&](diff::Mark mark, const QString& line, int row) {
        QTextBlockFormat block;
        block.setProperty(DiffMarkProperty, int(mark));
        const QColor tint = tintOf(mark, look);
        if (tint.isValid()) block.setBackground(tint);
        if (first) {
            caret.setBlockFormat(block);
            caret.setCharFormat(text);
            first = false;
        } else {
            caret.insertBlock(block, text);
        }
        caret.insertText(line);
        if (rowOfBlock != nullptr) rowOfBlock->append(row);
    };

    for (int i = 0; i < result.rows.size(); ++i) {
        const diff::Row& row = result.rows[i];
        switch (row.mark) {
            case diff::Mark::Same:
                addBlock(diff::Mark::Same, row.textAfter, i);
                break;
            case diff::Mark::Added:
                addBlock(diff::Mark::Added, row.textAfter, i);
                break;
            case diff::Mark::Removed:
                // Убранная строка — своим текстом: она и есть то, что пропало.
                addBlock(diff::Mark::Removed, row.textBefore, i);
                break;
            case diff::Mark::Changed:
                // Пара «− старая / + новая»: обе строки одного места, обе видны.
                addBlock(diff::Mark::Removed, row.textBefore, i);
                addBlock(diff::Mark::Added, row.textAfter, i);
                break;
        }
    }
    // Пустое сравнение (обе стороны пусты) — один пустой блок с меткой Same,
    // чтобы у документа был хотя бы один блок с ответом.
    if (first) addBlock(diff::Mark::Same, QString(), -1);
    return out;
}

int ZDocument::diffMarkAt(int index) const {
    return diffMarkOf(d_->text.findBlockByNumber(index));
}

}  // namespace zametti
