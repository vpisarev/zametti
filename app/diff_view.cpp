#include "diff_view.h"

#include "settings.h"

#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>

namespace zametti::diff {
namespace {

// Цвет метки. Одинаковый во всех видах: полоска на поле, заливка строки и
// заглушка обязаны говорить одно и то же.
QColor colorOf(Mark mark) {
    const Appearance& look = appearance();
    switch (mark) {
        case Mark::Added: return look.diffAdded;
        case Mark::Removed: return look.diffRemoved;
        case Mark::Changed: return look.diffChanged;
        case Mark::Same: break;
    }
    return QColor();
}

}  // namespace

QString gapLabel(int lines) {
    // Безличная форма («удалено: 1 строка», «удалено: 5 строк») — единственная,
    // которая живёт со всеми тремя окончаниями сразу.
    const int tens = lines % 100;
    const int ones = lines % 10;
    QString word = QStringLiteral("строк");
    if (tens < 11 || tens > 14) {
        if (ones == 1) word = QStringLiteral("строка");
        else if (ones >= 2 && ones <= 4) word = QStringLiteral("строки");
    }
    return QStringLiteral("удалено: %1 %2").arg(lines).arg(word);
}

Illustrated illustrate(const Document& snapshot, const BlockMarks& marks) {
    Illustrated out;
    // КОПИЯ ЦЕЛИКОМ, вместе с ареной: дальше мы дописываем в неё свои байты, а
    // исходный слепок обязан остаться неизменным до последнего байта — по нему
    // работает восстановление.
    out.ir = snapshot;
    out.ir.blocks.clear();
    out.ir.blocks.reserve(snapshot.blocks.size() + size_t(marks.gapBefore.size()) + 1);

    // Вспомогательная строка курсивом: она не текст заметки, и выглядеть как
    // текст заметки не должна.
    const auto addLabel = [&](int lines) {
        Block label = out.ir.newBlock(Kind::Paragraph, gapLabel(lines).toStdString());
        Inline italic;
        italic.text = {0, label.text.size()};
        italic.set(InlineItalic, true);
        label.inlines = out.ir.appendInlines({&italic, 1});
        // Инвариант IR: два блока, которые в файле слиплись бы, разделяет
        // пустая строка. Копия эта на диск не уходит, но собирается тем же
        // сборщиком, и жить по общим правилам ей дешевле, чем объясняться.
        if (!out.ir.blocks.empty() && out.ir.wouldMerge(out.ir.blocks.back(), label)) {
            out.ir.blocks.push_back(out.ir.newBlock(Kind::VSpace));
            out.blockMark.append(Mark::Same);
            out.sourceBlock.append(-1);
        }
        out.ir.blocks.push_back(label);
        out.blockMark.append(Mark::Removed);
        out.sourceBlock.append(-1);
    };

    for (size_t i = 0; i < snapshot.blocks.size(); ++i) {
        const auto gap = marks.gapBefore.constFind(int(i));
        if (gap != marks.gapBefore.constEnd() && gap.value() > 0) addLabel(gap.value());
        const Block& block = snapshot.blocks[i];
        if (!out.ir.blocks.empty() && out.ir.wouldMerge(out.ir.blocks.back(), block)) {
            out.ir.blocks.push_back(out.ir.newBlock(Kind::VSpace));
            out.blockMark.append(Mark::Same);
            out.sourceBlock.append(-1);
        }
        out.ir.blocks.push_back(block);
        out.blockMark.append(int(i) < marks.blocks.size() ? marks.blocks[int(i)] : Mark::Same);
        out.sourceBlock.append(int(i));
    }
    if (marks.gapAtEnd > 0) addLabel(marks.gapAtEnd);
    return out;
}

void buildPlainDocument(const Result& result, QTextDocument& target, qreal zoom,
                        QVector<Mark>* markOfBlock) {
    const Appearance& look = appearance();
    target.clear();
    if (markOfBlock != nullptr) markOfBlock->clear();

    // Поле слева — под полоски разности: без него они легли бы прямо на первый
    // знак строки (видно на снимке приёмки).
    target.setDocumentMargin(look.diffBarWidth * 4 * zoom);

    QTextCharFormat text;
    text.setFontFamilies({look.codeFamily});
    text.setFontPointSize(look.codePointSize * zoom);

    QTextCursor caret(&target);
    bool first = true;
    for (const Row& row : result.rows) {
        QTextBlockFormat block;
        // Заливка строки — едва заметная: моноширинный markdown должен
        // читаться как markdown, а не как светофор. Само же «сюда смотреть»
        // говорит полоска на поле, она сплошная.
        if (row.mark != Mark::Same) {
            QColor tint = colorOf(row.mark);
            tint.setAlpha(qBound(0, look.diffTint, 255));
            block.setBackground(tint);
        }
        if (first) {
            caret.setBlockFormat(block);
            caret.setCharFormat(text);
            first = false;
        } else {
            caret.insertBlock(block, text);
        }
        // Строки, которой на этой стороне нет, не показываем пустой строкой с
        // выдумкой — она и есть пустая. Место под неё остаётся, и в этом весь
        // смысл: по Alt текст в ней появляется, а всё вокруг стоит намертво.
        caret.insertText(row.text());
        if (markOfBlock != nullptr) markOfBlock->append(row.mark);
    }
    if (result.rows.isEmpty() && markOfBlock != nullptr) markOfBlock->append(Mark::Same);
}

}  // namespace zametti::diff
