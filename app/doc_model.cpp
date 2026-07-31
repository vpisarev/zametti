#include "doc_model.h"

#include <QTextBlock>
#include <QTextDocument>
#include <QTextFragment>

namespace zametti {

bool isRawBlock(const QTextBlock& block) {
    return block.blockFormat().boolProperty(RawProperty);
}

bool isContinuationBlock(const QTextBlock& block) {
    return block.blockFormat().boolProperty(ContinuationProperty);
}

bool isVSpaceBlock(const QTextBlock& block) {
    return !isRawBlock(block) && kindOf(block) == Kind::VSpace;
}

bool blocksWouldMerge(const QTextBlock& previous, const QTextBlock& next) {
    if (!previous.isValid() || !next.isValid()) return false;
    // wouldMerge смотрит только на род и на дословность — большего для этого
    // вопроса и не нужно, поэтому обходимся заготовками, а не читаем блоки
    // целиком.
    Document ir;
    auto stub = [&ir](const QTextBlock& block, bool asPrevious) {
        Block out;
        if (isRawBlock(block)) {
            // Дословный кусок лежит построчно; для wouldMerge довольно знать,
            // законченный ли это HTML-комментарий — он прозрачен для соседства
            // (см. Document::isClosedHtmlComment), прочее дословное непрозрачно.
            // Первая строка куска — назад по строкам-продолжениям.
            QTextBlock first = block;
            while (isContinuationBlock(first) && first.previous().isValid())
                first = first.previous();
            const bool closed = asPrevious &&
                                first.text().startsWith(QStringLiteral("<!--")) &&
                                block.text().endsWith(QStringLiteral("-->"));
            // Заготовка дословного куска: точные байты не важны, важно лишь,
            // законченный ли это комментарий.
            out.raw = true;
            out.text = ir.append(closed ? "<!---->\n" : " ");
        } else {
            out.kind = kindOf(block);
        }
        return out;
    };
    const Block first = stub(previous, true);
    const Block second = stub(next, false);
    return ir.wouldMerge(first, second);
}

Kind kindOf(const QTextBlock& block) {
    // Блок без свойства — не дословный кусок, а обычный абзац: так выглядят
    // блоки, которые Qt завёл сам, помимо сборщика.
    return static_cast<Kind>(block.blockFormat().intProperty(KindProperty));
}

int levelOf(const QTextBlock& block) {
    // Отсутствие свойства и есть «вне списка»: нулевой уровень — настоящий,
    // это верхний уровень списка, и путать их нельзя.
    //
    // У пустой строки уровня не бывает ПО ПОСТРОЕНИЮ: пустая строка ничья,
    // и застрявшее на ней свойство — мусор от правок, а не уровень. Читатели
    // его не видят, а нормализация вычищает.
    const QTextBlockFormat format = block.blockFormat();
    if (!isRawBlock(block) && kindOf(block) == Kind::VSpace) return -1;
    return format.hasProperty(LevelProperty) ? format.intProperty(LevelProperty) : -1;
}

bool isListBlock(const QTextBlock& block) {
    return !isRawBlock(block) && isList(kindOf(block));
}

int irIndexOfBlock(const QTextBlock& block) {
    // Считаем начала логических блоков до этого места включительно, а номер —
    // на единицу меньше. Начинать с нуля и считать только предыдущие нельзя:
    // строка-продолжение получила бы номер следующего блока IR, а не своего.
    int index = isContinuationBlock(block) ? -1 : 0;
    for (QTextBlock prev = block.previous(); prev.isValid(); prev = prev.previous())
        if (!isContinuationBlock(prev)) ++index;
    return index;
}

QTextBlock blockForIrIndex(const QTextDocument& doc, int index) {
    int seen = 0;
    for (QTextBlock block = doc.begin(); block.isValid(); block = block.next()) {
        if (isContinuationBlock(block)) continue;
        if (seen == index) return block;
        ++seen;
    }
    return QTextBlock();
}

MarkerStyle markerOf(const QTextBlock& block) {
    const QTextBlockFormat format = block.blockFormat();
    return {static_cast<Marker>(format.intProperty(MarkerProperty)),
            format.boolProperty(CheckedProperty)};
}

bool isTaskBlock(const QTextBlock& block) {
    return isListBlock(block) && markerOf(block).marker == Marker::Task;
}

bool isOrderedBlock(const QTextBlock& block) {
    return isListBlock(block) && markerOf(block).marker == Marker::Ordered;
}

int ordinalOf(const QTextBlock& block) {
    if (!isListBlock(block)) return 0;
    const int level = levelOf(block);
    const bool ordered = isOrderedBlock(block);

    int ordinal = 1;
    for (QTextBlock prev = block.previous(); prev.isValid(); prev = prev.previous()) {
        if (isVSpaceBlock(prev)) continue;           // просторный список — всё тот же список
        if (!isListBlock(prev)) {
            // Блок внутри пункта — второй абзац, код — список не заканчивает:
            // нумерация за ним продолжается. Так же смотрит и сериализатор.
            if (levelOf(prev) >= 0) continue;
            break;                                   // абзац или дословный кусок рвёт прогон
        }
        const int prevLevel = levelOf(prev);
        if (prevLevel > level) continue;             // вложенный подсписок прогон не рвёт
        if (prevLevel < level) break;                // вышли из своего уровня
        if (isOrderedBlock(prev) != ordered) break;
        ++ordinal;
    }
    return ordinal;
}

void ListRuns::reset() {
    for (Level& level : levels_) level.alive = false;
}

bool ListRuns::startsNewRun(int level, bool ordered) const {
    return level >= 0 && size_t(level) < levels_.size() && levels_[size_t(level)].alive &&
           levels_[size_t(level)].ordered != ordered;
}

int ListRuns::next(int level, bool ordered) {
    if (level < 0) level = 0;
    const size_t index = size_t(level);
    if (levels_.size() <= index + 1) levels_.resize(index + 2);

    Level& own = levels_[index];
    own.ordinal = (own.alive && own.ordered == ordered) ? own.ordinal + 1 : 1;
    own.alive = true;
    own.ordered = ordered;
    // Всё, что глубже, закончилось вместе с предыдущим пунктом этого уровня.
    for (size_t k = index + 1; k < levels_.size(); ++k) levels_[k].alive = false;
    return own.ordinal;
}

BlockImageRef blockImageRef(const QTextBlock& block) {
    if (!block.isValid() || isRawBlock(block)) return {};
    if (kindOf(block) != Kind::Paragraph) return {};

    // Image-спан целым абзацем: каждый кусок помечен SpanImage с одним путём.
    // Картинка в середине текста фотографией не показывается — только стилем.
    QString href;
    bool whole = true;
    for (QTextBlock::iterator it = block.begin(); !it.atEnd(); ++it) {
        const QTextFragment fragment = it.fragment();
        if (!fragment.isValid() || fragment.text().isEmpty()) continue;
        const QTextCharFormat format = fragment.charFormat();
        const bool image = (format.intProperty(SpanStyleProperty) & SpanImage) != 0 &&
                           !format.anchorHref().isEmpty();
        if (!image || (!href.isEmpty() && href != format.anchorHref())) {
            whole = false;
            break;
        }
        href = format.anchorHref();
    }
    if (whole && !href.isEmpty()) {
        // Ширина — из "#w=N" в пути. Фрагмент остаётся байтами пути (ядро его
        // не трактует), но вид и ресайз читают и пишут ровно его.
        qreal width = 0.0;
        QString path = href;
        const qsizetype hash = href.lastIndexOf(QLatin1Char('#'));
        if (hash >= 0 && href.mid(hash, 3) == QStringLiteral("#w=")) {
            bool ok = false;
            const double w = href.mid(hash + 3).toDouble(&ok);
            if (ok && w > 0.0) {
                width = w;
                path = href.left(hash);
            }
        }
        return {path, width, false, true};
    }

    // Вики-вложение Obsidian: строка целиком "![[путь]]" или "![[путь|ширина]]".
    // Модель хранит его дословным текстом абзаца (wikilinks не переписываются),
    // но фотографию по нему показать можно и нужно.
    const QString text = block.text().trimmed();
    if (!text.startsWith(QStringLiteral("![[")) || !text.endsWith(QStringLiteral("]]")))
        return {};
    QString inner = text.mid(3, text.size() - 5);
    if (inner.isEmpty() || inner.contains(QStringLiteral("]]"))) return {};
    qreal width = 0.0;
    const qsizetype bar = inner.lastIndexOf(QLatin1Char('|'));
    if (bar >= 0) {
        // После черты либо ширина, либо подпись (Obsidian допускает обе);
        // подпись фотографии не мешает — просто остаётся своя ширина.
        bool ok = false;
        const double w = inner.mid(bar + 1).trimmed().toDouble(&ok);
        if (ok && w > 0.0) width = w;
        inner = inner.left(bar);
    }
    inner = inner.trimmed();
    if (inner.isEmpty()) return {};
    return {inner, width, true, true};
}

}  // namespace zametti
