#include "doc_model.h"

#include "math_scan.h"

#include <QDebug>
#include <QStringList>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextFragment>
#include <QFontMetricsF>
#include <QTextFrame>
#include <QTextLayout>

#include <algorithm>
#include <cmath>


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
    for (QTextBlock block = doc.begin(); block.isValid(); block = block.next(), ++number) {
        // ВЫСОТА СТРОКИ У БЛОКА-ФОРМУЛЫ ПРИНАДЛЕЖИТ ВИДУ, как и нижнее поле у
        // блока с картинкой: вид знает размер вёрстки, а сборщик — нет. У
        // прочих блоков она сборщикова (заголовки, код), и слепо выкинуть её из
        // сверки значило бы ослепить проверку заплатки там, где она нужна.
        QList<int> blockSkip = skip;
        if (const BlockFormulaRef formula = blockFormulaRef(block);
            formula.valid && formula.display) {
            blockSkip.append(QTextFormat::BlockBottomMargin);
            blockSkip.append(int(QTextFormat::LineHeight));
            // И ЦВЕТ ТЕКСТА. У показанной вёрсткой формулы исходник погашен
            // прозрачным — это признак ПОКАЗА, а не модели: сборщик о нём не
            // знает и знать не должен.
            blockSkip.append(QTextFormat::ForegroundBrush);
        }
        out += QStringLiteral("%1 %2\n").arg(number).arg(blockFingerprint(block, blockSkip));
    }
    return out;
}

qreal fontStepFactor(int step) {
    // Таблица Qt, снятая пробником. Держим её у себя, а не считаем на глаз:
    // размеры маркеров, формул и плашек обязаны совпадать с тем, во что Qt
    // разрешит ступень, иначе резерв под них разойдётся с нарисованным.
    static constexpr qreal kFactor[] = {0.7, 0.8, 1.0, 1.2, 1.5, 2.0, 2.4};
    const int clamped = std::clamp(step, kFontStepMin, kFontStepMax);
    return kFactor[clamped - kFontStepMin];
}

int nearestFontStep(qreal factor) {
    int best = 0;
    qreal bestMiss = -1.0;
    for (int step = kFontStepMin; step <= kFontStepMax; ++step) {
        // Промах меряем ОТНОСИТЕЛЬНЫЙ: на глаз 0.7 против 0.8 отличается так
        // же сильно, как 2.0 против 2.4, а разность их — втрое.
        const qreal miss = std::abs(std::log(fontStepFactor(step) / factor));
        if (bestMiss < 0.0 || miss < bestMiss) {
            bestMiss = miss;
            best = step;
        }
    }
    return best;
}

void setFontStep(QTextCharFormat& format, int step) {
    format.setProperty(QTextFormat::FontSizeAdjustment,
                       std::clamp(step, kFontStepMin, kFontStepMax));
}

qreal assignedLineHeight(const QTextBlock& block) {
    const QTextBlockFormat format = block.blockFormat();
    // Естественная высота строки: её знает разметка. У неразмеченного блока
    // разметки ещё нет (Qt размечает лениво) — тогда спрашиваем метрики шрифта.
    qreal natural = 0.0;
    const QTextLayout* layout = block.layout();
    if (layout != nullptr && layout->lineCount() > 0) natural = layout->lineAt(0).height();
    if (natural <= 0.0) natural = QFontMetricsF(block.charFormat().font()).height();
    return format.lineHeight(natural, 1.0);
}

int diffMarkOf(const QTextBlock& block) {
    const QTextBlockFormat format = block.blockFormat();
    return format.hasProperty(DiffMarkProperty) ? format.intProperty(DiffMarkProperty) : -1;
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
    // Законченный ли комментарий предыдущий кусок: дословное лежит построчно,
    // поэтому начало ищем назад по строкам-продолжениям, а конец берём у самого
    // блока.
    bool closedComment = false;
    if (isRawBlock(previous)) {
        QTextBlock head = previous;
        while (isContinuationBlock(head) && head.previous().isValid()) head = head.previous();
        closedComment = head.text().startsWith(QStringLiteral("<!--")) &&
                        previous.text().endsWith(QStringLiteral("-->"));
    }
    return wouldMerge(kindOf(previous), isRawBlock(previous), closedComment, kindOf(next),
                      isRawBlock(next));
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

QVector<int> irIndexOfEveryBlock(const QTextDocument& doc) {
    QVector<int> out;
    int index = -1;
    for (QTextBlock block = doc.begin(); block.isValid(); block = block.next()) {
        if (!isContinuationBlock(block)) ++index;
        out.append(index);
    }
    return out;
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

BlockFormulaRef blockFormulaRef(const QTextBlock& block) {
    if (!block.isValid() || isRawBlock(block)) return {};
    // ВЫКЛЮЧНАЯ ФОРМУЛА — ЦЕЛЫЙ БЛОК (решение владельца: строчная спаном,
    // выключная объектом). Текст блока и есть её исходник вместе с долларами,
    // разметки внутри нет: спрашивать спаны незачем.
    if (kindOf(block) == Kind::Math) {
        QString source;
        for (QTextBlock::iterator it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            if (!fragment.isValid()) continue;
            QString piece = fragment.text();
            switch (fragment.charFormat().intProperty(BreakSourceProperty)) {
                case BreakCarriageReturn:
                    piece.replace(QChar::LineSeparator, QLatin1Char('\r'));
                    break;
                case BreakParagraph:
                    piece.replace(QChar::LineSeparator, QChar(QChar::ParagraphSeparator));
                    break;
                default:
                    piece.replace(QChar::LineSeparator, QLatin1Char('\n'));
                    break;
            }
            source += piece;
        }
        if (source.isEmpty()) return {};
        const std::string bytes = source.toStdString();
        const std::vector<MathSpan> found = scanMath(bytes);
        if (found.size() != 1 || found.front().start != 0 ||
            size_t(found.front().end) != bytes.size())
            return {};   // правкой формулу разорвали — это уже не формула
        const int skip = found.front().display ? 2 : 1;
        BlockFormulaRef ref;
        ref.source = source;
        ref.latex = source.mid(skip, source.size() - 2 * skip);
        // Блоком показывается ВЫКЛЮЧНАЯ формула, даже если долларов по одному:
        // отдельной строкой её так и задумывал автор, и все читалки показывают
        // её выключной.
        ref.display = true;
        ref.valid = true;
        return ref;
    }
    if (kindOf(block) != Kind::Paragraph) return {};

    // Абзац ЦЕЛИКОМ — одна формула. Формула в середине текста объектом не
    // бывает: она живёт внутри строки и рисуется инлайн-объектом.
    QString source;
    for (QTextBlock::iterator it = block.begin(); !it.atEnd(); ++it) {
        const QTextFragment fragment = it.fragment();
        if (!fragment.isValid() || fragment.text().isEmpty()) continue;
        const QTextCharFormat format = fragment.charFormat();
        if ((format.intProperty(SpanStyleProperty) & SpanMath) == 0) return {};
        QString piece = fragment.text();
        // Многострочная выключная живёт в ОДНОМ блоке: переносы внутри неё —
        // разделители строк U+2028, а каким знаком они были в файле, помнит
        // свойство. Движку и канону нужен файл, а не то, что видит раскладка.
        switch (format.intProperty(BreakSourceProperty)) {
            case BreakCarriageReturn: piece.replace(QChar::LineSeparator, QLatin1Char('\r')); break;
            case BreakParagraph:      piece.replace(QChar::LineSeparator,
                                                    QChar(QChar::ParagraphSeparator)); break;
            case BreakNewline:        piece.replace(QChar::LineSeparator, QLatin1Char('\n')); break;
            default: break;
        }
        source += piece;
    }
    if (source.isEmpty()) return {};

    // Канон общий, из ядра: «похоже на формулу» и «является формулой» — разные
    // вопросы, и второй уже решён одним местом.
    const std::string bytes = source.toStdString();
    const std::vector<MathSpan> found = scanMath(bytes);
    if (found.size() != 1 || found.front().start != 0 || size_t(found.front().end) != bytes.size())
        return {};

    const MathSpan& span = found.front();
    const int skip = span.display ? 2 : 1;
    BlockFormulaRef ref;
    ref.source = source;
    ref.latex = source.mid(skip, source.size() - 2 * skip);
    ref.display = span.display;
    ref.valid = true;
    return ref;
}

// Разбор атрибутов из адреса image-спана: "путь#w=560&align=left". Фрагмент
// остаётся байтами пути (ядро его не трактует), но вид, ресайз и выравнивание
// читают и пишут ровно его.
BlockImageRef imageRefOfSpan(const QString& href, const QString& alt) {
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
    return {path, alt, width, align, false, true};
}

// Вики-вложение Obsidian: строка целиком "![[путь]]", "![[путь|ширина]]" или
// "![[путь|ширина|align=left]]".
BlockImageRef imageRefOfWiki(const QString& source) {
    const QString text = source.trimmed();
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
    return {path, QString(), width, align, true, true};
}

BlockImageRef blockImageRef(const QTextBlock& block) {
    if (!block.isValid() || isRawBlock(block)) return {};
    if (kindOf(block) != Kind::Paragraph) return {};

    // ФОТОГРАФИЯ-ОБЪЕКТ. В документе она — один знак U+FFFC, а всё о ней лежит
    // в свойствах его формата: подпись, адрес, дословный исходник. Спрашиваем
    // их, а не текст блока: текста у объекта нет.
    //
    // ОБЪЕКТ ОБЯЗАН БЫТЬ БЛОКОМ ЦЕЛИКОМ. Набранная рядом буква делает строку
    // обычным текстом — и пока эта проверка отсутствовала, Backspace за такой
    // буквой считал строку фотографией и сносил её целиком.
    if (block.text().size() == 1 &&
        block.text().at(0) == QChar::ObjectReplacementCharacter) {
        const QTextCharFormat format = block.begin().fragment().charFormat();
        if (format.objectType() != ImageObject) return {};
        if (format.hasProperty(ObjectAltProperty))
            return imageRefOfSpan(format.anchorHref(),
                               format.property(ObjectAltProperty).toString());
        return imageRefOfWiki(format.property(ObjectSourceProperty).toString());
    }

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
    if (whole && !href.isEmpty()) return imageRefOfSpan(href, block.text());

    return imageRefOfWiki(block.text());
}

}  // namespace zametti
