#include "doc_model.h"

#include <QDebug>
#include <QStringList>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextFragment>
#include <QTextFrame>


namespace zametti {
namespace {

// Свойства формата — в порядке ключа: QMap уже упорядочен, но полагаться на
// это в отпечатке не хочется. Значение печатаем через QDebug: у QBrush и QFont
// toString() пуст, а сравнивать надо именно их.
QString formatFingerprint(const QTextFormat& format, const QList<int>& skip) {
    QString out;
    const QMap<int, QVariant> properties = format.properties();
    for (auto it = properties.begin(); it != properties.end(); ++it) {
        if (skip.contains(it.key())) continue;
        QString value;
        QDebug(&value).nospace() << it.value();
        out += QStringLiteral("%1=%2;").arg(it.key()).arg(value.trimmed());
    }
    return out;
}

}  // namespace

QString blockFingerprint(const QTextBlock& block, const QList<int>& skip) {
    QString out = QStringLiteral("блок«%1» формат[%2] знаки[%3]")
                      .arg(block.text(), formatFingerprint(block.blockFormat(), skip),
                           formatFingerprint(block.charFormat(), skip));
    for (QTextBlock::iterator it = block.begin(); !it.atEnd(); ++it) {
        const QTextFragment fragment = it.fragment();
        if (!fragment.isValid()) continue;
        out += QStringLiteral(" кусок«%1»[%2]")
                   .arg(fragment.text(), formatFingerprint(fragment.charFormat(), skip));
    }
    return out;
}

QString documentFingerprint(const QTextDocument& doc, const QList<int>& skip) {
    QString out = QStringLiteral("шрифт[%1] поле[%2]\n")
                      .arg(doc.defaultFont().toString())
                      .arg(doc.documentMargin());
    if (doc.rootFrame() != nullptr)
        out += QStringLiteral("рамка[%1]\n")
                   .arg(formatFingerprint(doc.rootFrame()->frameFormat(), skip));
    int number = 0;
    for (QTextBlock block = doc.begin(); block.isValid(); block = block.next(), ++number)
        out += QStringLiteral("%1 %2\n").arg(number).arg(blockFingerprint(block, skip));
    return out;
}

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

ImageAlign imageAlignFromText(QString text, bool* ok) {
    if (ok != nullptr) *ok = true;
    text = text.trimmed();
    if (text == QStringLiteral("align=left")) return ImageAlign::Left;
    if (text == QStringLiteral("align=right")) return ImageAlign::Right;
    if (text == QStringLiteral("align=center")) return ImageAlign::Center;
    if (ok != nullptr) *ok = false;
    return ImageAlign::Center;
}

QString imageAlignText(ImageAlign align) {
    switch (align) {
        case ImageAlign::Left: return QStringLiteral("align=left");
        case ImageAlign::Right: return QStringLiteral("align=right");
        case ImageAlign::Center: break;
    }
    // Умолчание не пишется: заметка не обязана хранить то, чего человек не
    // задавал.
    return {};
}

QString imageRefText(const BlockImageRef& ref) {
    QStringList extras;
    if (ref.widthHint > 0.0) extras << QString::number(qRound(ref.widthHint));
    const QString align = imageAlignText(ref.align);
    if (!align.isEmpty()) extras << align;

    if (ref.wiki) {
        // "![[путь]]", "![[путь|ширина]]", "![[путь|ширина|align=left]]".
        QString inner = ref.path;
        for (const QString& extra : extras) inner += QLatin1Char('|') + extra;
        return QStringLiteral("![[%1]]").arg(inner);
    }
    // Адрес image-спана: атрибуты во фрагменте, "путь#w=560&align=left".
    if (extras.isEmpty()) return ref.path;
    QStringList pairs;
    if (ref.widthHint > 0.0)
        pairs << QStringLiteral("w=") + QString::number(qRound(ref.widthHint));
    if (!align.isEmpty()) pairs << align;
    return ref.path + QLatin1Char('#') + pairs.join(QLatin1Char('&'));
}

// Атрибуты картинки — ширина и выравнивание — разбираются одинаково в обеих
// формах записи: каждое поле пробуется как число, потом как выравнивание, а
// что не подошло, то подпись, и её мы не трогаем. Порядок полей поэтому
// значения не имеет.
namespace {

void takeImageAttribute(const QString& field, qreal& width, ImageAlign& align) {
    bool number = false;
    const double value = field.trimmed().toDouble(&number);
    if (number && value > 0.0) {
        width = value;
        return;
    }
    bool known = false;
    const ImageAlign parsed = imageAlignFromText(field, &known);
    if (known) align = parsed;
}

}  // namespace

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
        // Атрибуты — во фрагменте пути: "#w=560", "#w=560&align=left".
        // Фрагмент остаётся байтами пути (ядро его не трактует), но вид,
        // ресайз и выравнивание читают и пишут ровно его.
        qreal width = 0.0;
        ImageAlign align = ImageAlign::Center;
        QString path = href;
        const qsizetype hash = href.lastIndexOf(QLatin1Char('#'));
        if (hash >= 0) {
            const QStringList pairs =
                href.mid(hash + 1).split(QLatin1Char('&'), Qt::SkipEmptyParts);
            bool understood = !pairs.isEmpty();
            for (const QString& pair : pairs) {
                if (pair.startsWith(QStringLiteral("w="))) {
                    bool ok = false;
                    const double w = pair.mid(2).toDouble(&ok);
                    if (ok && w > 0.0) width = w;
                    else understood = false;
                } else {
                    bool ok = false;
                    const ImageAlign parsed = imageAlignFromText(pair, &ok);
                    if (ok) align = parsed;
                    else understood = false;
                }
            }
            // Чужой якорь в пути картинкой не заведует: путь оставляем целиком.
            if (understood) path = href.left(hash);
            else {
                width = 0.0;
                align = ImageAlign::Center;
            }
        }
        return {path, width, align, false, true};
    }

    // Вики-вложение Obsidian: строка целиком "![[путь]]", "![[путь|ширина]]"
    // или "![[путь|ширина|align=left]]". Модель хранит его дословным текстом
    // абзаца (wikilinks не переписываются), но фотографию по нему показать
    // можно и нужно.
    const QString text = block.text().trimmed();
    if (!text.startsWith(QStringLiteral("![[")) || !text.endsWith(QStringLiteral("]]")))
        return {};
    const QString inner = text.mid(3, text.size() - 5);
    if (inner.isEmpty() || inner.contains(QStringLiteral("]]"))) return {};

    const QStringList fields = inner.split(QLatin1Char('|'));
    qreal width = 0.0;
    ImageAlign align = ImageAlign::Center;
    // Первое поле — путь, остальные атрибуты; подпись (Obsidian её допускает)
    // фотографии не мешает — просто ни числом, ни выравниванием не окажется.
    for (qsizetype i = 1; i < fields.size(); ++i) takeImageAttribute(fields.at(i), width, align);

    const QString path = fields.value(0).trimmed();
    if (path.isEmpty()) return {};
    return {path, width, align, true, true};
}

}  // namespace zametti
