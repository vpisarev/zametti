#include "search.h"

namespace zametti {

Query makeQuery(const QString& text) {
    Query query;
    query.needle = text;
    // Регистр важен, если человек сам его задал. Сравниваем с нижним
    // регистром, а не ищем заглавные по алфавиту: у кириллицы и у любого
    // другого письма свои правила, и знать их — дело QString.
    query.caseSensitive = text != text.toLower();
    return query;
}

QString blockText(const Block& block) {
    return QString::fromStdString(block.rawSource.empty() ? block.text : block.rawSource);
}

std::vector<Hit> findInDocument(const Document& doc, const Query& query) {
    std::vector<Hit> hits;
    if (query.isEmpty()) return hits;
    int ordinal = 0;
    for (size_t i = 0; i < doc.blocks.size(); ++i) {
        const QString text = blockText(doc.blocks[i]);
        if (text.isEmpty()) continue;
        qsizetype at = text.indexOf(query.needle, 0, query.sensitivity());
        while (at >= 0) {
            hits.push_back(Hit{int(i), int(at), int(query.needle.size()), ordinal++});
            // Со следующего знака, а не через длину запроса: перекрывающиеся
            // вхождения («аа» в «ааа») — тоже вхождения, и счётчик «3/17»
            // обязан считать их так же, как их потом обойдёт F3.
            at = text.indexOf(query.needle, at + 1, query.sensitivity());
        }
    }
    return hits;
}

HitLine hitLine(const Document& doc, const Hit& hit, int radius) {
    HitLine out;
    if (hit.block < 0 || size_t(hit.block) >= doc.blocks.size()) return out;
    const QString text = blockText(doc.blocks[size_t(hit.block)]);
    if (hit.offset < 0 || hit.offset > text.size()) return out;

    // Строка, в которой стоит совпадение: у блока их может быть несколько
    // (мягкие переносы, блок кода), а в списке результатов нужна одна.
    qsizetype from = text.lastIndexOf(QLatin1Char('\n'), hit.offset > 0 ? hit.offset - 1 : 0);
    from = from < 0 ? 0 : from + 1;
    qsizetype to = text.indexOf(QLatin1Char('\n'), hit.offset);
    if (to < 0) to = text.size();

    // Окно вокруг совпадения: длинную строку кода целиком в список не
    // вместить, а совпадение обязано быть видно.
    qsizetype start = qMax(from, qsizetype(hit.offset) - radius);
    qsizetype end = qMin(to, qsizetype(hit.offset + hit.length) + radius);
    QString line = text.mid(start, end - start);
    int offset = int(hit.offset - start);
    if (start > from) {
        line.prepend(QChar(0x2026));
        ++offset;
    }
    if (end < to) line.append(QChar(0x2026));

    out.text = line;
    out.offset = offset;
    out.length = hit.length;
    return out;
}

}  // namespace zametti
