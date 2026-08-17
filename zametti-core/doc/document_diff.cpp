// Сравнение двух версий заметки — глагол самой заметки.
//
// ЕДИНИЦА СРАВНЕНИЯ — СТРОКА КАНОНИЧЕСКОГО MARKDOWN, и потому обе стороны
// сперва записываются. Записываем КАЖДУЮ РОВНО ОДИН РАЗ, хотя сравнений два:
// прямое и обратное отличаются только порядком доводов, а строки у них одни и
// те же (решение владельца — на этом и построена подпись, возвращающая пару).
//
// Возвращаются ДВЕ заметки, по одной на сторону: показываем свою — метки
// считаны против чужой, нажали Tab — показывается чужая, и зелёное с красным
// меняются местами. Каждая строка сравнения становится блоком, а что с ней
// стало — свойством блока (DiffMarkProperty). Свойством, а не заливкой: цвет
// принадлежит виду, и документ-разность отличается от заметки только тем, что
// у его блоков есть эта метка.

#include "document_impl.h"

#include "diff.h"
#include "doc_model.h"
#include "settings.h"

#include <QTextBlock>
#include <QTextBlockFormat>
#include <QTextCharFormat>
#include <QTextCursor>

namespace zametti {

namespace {

// Строки заметки так, как их видит сравнение: канонический markdown без шапки.
// Хвостовой пустой строки нет — текст кончается переводом строки, и строкой
// файла эта пустота не является, а в сравнение лезла бы «изменением» при любой
// правке хвоста.
QStringList linesOf(const std::vector<SourceLine>& source) {
    QStringList out;
    out.reserve(int(source.size()));
    for (const SourceLine& line : source) out.append(line.text);
    if (!out.isEmpty() && out.last().isEmpty()) out.removeLast();
    return out;
}

}  // namespace

// Одна сторона сравнения как заметка. Метод, а не свободная функция: он пишет
// прямо в живой документ, и никакого промежуточного представления между
// строками сравнения и блоками не заводится.
ZDocument ZDocument::diffSide(const diff::Result& result) {
    ZDocument out;

    QTextCharFormat text;
    if (!settings().look.codeFamily.isEmpty())
        text.setFontFamilies({QString(settings().look.codeFamily)});
    setFontStep(text, settings().look.codeStep);

    QTextCursor caret(&out.d_->text);
    bool first = true;
    for (const diff::Row& row : result.rows) {
        QTextBlockFormat block;
        block.setProperty(DiffMarkProperty, int(row.mark));
        if (first) {
            caret.setBlockFormat(block);
            caret.setCharFormat(text);
            first = false;
        } else {
            caret.insertBlock(block, text);
        }
        // Строки, которой на этой стороне нет, не показываем выдумкой — она и
        // есть пустая. Место под неё остаётся, и в этом весь смысл: по Tab
        // текст в ней появляется, а всё вокруг стоит намертво.
        caret.insertText(row.text());
    }
    return out;
}

int ZDocument::diffMarkAt(int index) const {
    return diffMarkOf(d_->text.findBlockByNumber(index));
}

std::pair<ZDocument, ZDocument> ZDocument::getDiff(const ZDocument& other) const {
    const QStringList mine = linesOf(sourceLines());
    const QStringList theirs = linesOf(other.sourceLines());
    // Два прогона DTL вместо одного стоят 9 мс на самой большой заметке
    // владельца и считаются один раз на слепок.
    return {diffSide(diff::compare(theirs, mine)), diffSide(diff::compare(mine, theirs))};
}

std::pair<ZDocument, ZDocument> ZDocument::getDiff(std::string_view markdown) const {
    // Байты приходят из журнала, и канон у них наш же — но проверять это на
    // слово нельзя: сравнение обязано идти по каноническим строкам, иначе
    // разница в оформлении показалась бы изменением заметки.
    ZDocument other;
    other.loadMarkdown(markdown);
    return getDiff(other);
}

}  // namespace zametti
