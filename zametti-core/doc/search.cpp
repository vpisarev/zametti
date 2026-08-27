#include "search.h"

#include "doc_model.h"

#include <QTextBlock>
#include <QTextDocument>

namespace zametti {

namespace {

// Мягкий перенос строки (Shift+Enter) лежит в тексте блока знаком U+2028, а не
// переводом строки, — и для PCRE2 это обычный знак: замерено, что «^» на нём не
// срабатывает, «.» его ест, а «\s» его не видит вовсе. Человеку же он —
// НАСТОЯЩАЯ СТРОКА (в файл уходит переводом), в отличие от переноса по ширине
// окна, который дело показа: отресайзил окно — якоря выражения ехать не должны.
//
// Поэтому текст для поиска приводится: U+2028 → «\n». Знак в знак, длина та же,
// смещения не съезжают, и вид с исходником отвечают одинаково.
QString withPlainBreaks(QString text) {
    text.replace(QChar::LineSeparator, QLatin1Char('\n'));
    return text;
}

}  // namespace

Query makeQuery(const QString& text, bool regex) {
    Query query;
    query.needle = text;
    query.regex = regex;

    // Регистр важен, если человек сам его задал. Сравниваем с нижним
    // регистром, а не ищем заглавные по алфавиту: у кириллицы и у любого
    // другого письма свои правила, и знать их — дело QString.
    //
    // В ВЫРАЖЕНИИ ЗАГЛАВНАЯ ПОСЛЕ КОСОЙ НЕ В СЧЁТ: «\D», «\S», «\W», «\B» — это
    // классы, а не набранная человеком заглавная буква. Иначе «\d» и «\D»
    // молча включали бы разный регистр поиска.
    QString letters = text;
    if (regex) {
        letters.clear();
        for (qsizetype i = 0; i < text.size(); ++i) {
            if (text.at(i) == QLatin1Char('\\')) {
                ++i;   // и сам знак класса пропускаем
                continue;
            }
            letters += text.at(i);
        }
    }
    query.caseSensitive = letters != letters.toLower();

    if (!regex || text.isEmpty()) return query;

    // ОПЦИИ ВЫРАЖЕНИЯ — три, и каждая замерена пробником:
    //
    //   Multiline — «^» и «$» стоят на краях СТРОКИ, а не всего блока. Без неё
    //     якорь ловил бы только начало блока целиком.
    //   UseUnicodeProperties — «\w», «\d», «\b» перестают быть только про ASCII.
    //     Без неё «\w+» не находит НИ ОДНОГО русского слова, а «\bкот\b» — ни
    //     одного кота: у заметочника на русском это не мелочь, а негодность.
    //   CaseInsensitive — по smart case, как и у обычного поиска.
    QRegularExpression::PatternOptions options =
        QRegularExpression::MultilineOption | QRegularExpression::UseUnicodePropertiesOption;
    if (!query.caseSensitive) options |= QRegularExpression::CaseInsensitiveOption;
    query.pattern = QRegularExpression(text, options);
    // Недописанное выражение — не ошибка, а промежуточное состояние набора:
    // человек ещё закрывает скобку. Ищем ничего, говорим об этом одним
    // признаком, а панель красит буквы запроса.
    query.valid = query.pattern.isValid();
    return query;
}

// ОДИН ШАГОВЫЙ ЦИКЛ на оба входа: и на плоский текст, и на блок за блоком.
// Здесь живут все правила про шаг — и обычный поиск, и выражение ходят им.
template <class Sink>
void scanText(const QString& text, const Query& query, Sink&& sink) {
    if (!query.usable() || text.isEmpty()) return;

    if (!query.regex) {
        qsizetype at = text.indexOf(query.needle, 0, query.sensitivity());
        while (at >= 0) {
            if (!sink(int(at), int(query.needle.size()), nullptr)) return;
            // ШАГ ЧЕРЕЗ ДЛИНУ НАЙДЕННОГО: перекрывающихся вхождений у нас нет
            // (решение владельца) — «аа» в «ааа» это одно вхождение, и счётчик
            // «3/17» считает ровно то, что обойдёт F3 и заменит «заменить всё».
            at = text.indexOf(query.needle, at + query.needle.size(), query.sensitivity());
        }
        return;
    }

    qsizetype from = 0;
    while (from <= text.size()) {
        const QRegularExpressionMatch match = query.pattern.match(text, from);
        if (!match.hasMatch()) return;
        const qsizetype start = match.capturedStart();
        const qsizetype end = match.capturedEnd();
        // ПУСТОЕ СОВПАДЕНИЕ ЦЕЛИКОМ находкой не считается: на неё не встать по
        // F3, нечем подсветить и нечего заменить. Обход при этом не встаёт —
        // шаг на знак вперёд, — поэтому «а*» на «бббаа» честно находит «аа».
        // Пустая ГРУППА внутри непустого совпадения законна, как в perl и
        // python, и разворачивается заменой в пустоту.
        if (end > start) {
            if (!sink(int(start), int(end - start), &match)) return;
            from = end;
        } else {
            from = start + 1;
        }
    }
}

std::vector<FlatHit> findInText(const QString& raw, const Query& query) {
    std::vector<FlatHit> hits;
    const QString text = withPlainBreaks(raw);
    scanText(text, query, [&](int offset, int length, const QRegularExpressionMatch* match) {
        hits.push_back(FlatHit{offset, length, match != nullptr ? *match : QRegularExpressionMatch()});
        return true;
    });
    return hits;
}

namespace {

// Как менять регистр у того, что кладётся дальше. Состояний ДВА, и это не
// прихоть: в perl «\L\uслово» даёт «Слово» — первая буква от \u, остальные от
// \L. Пока состояние было одно, \u сбивал \L совсем (поймал набор).
enum class CaseRun { None, Upper, Lower };   // \U и \L — до \E
enum class CaseOnce { None, Upper, Lower };  // \u и \l — одна буква

void appendCased(QString& out, const QString& piece, CaseRun run, CaseOnce& once) {
    if (piece.isEmpty()) return;
    QString body = piece;
    QString head;
    if (once != CaseOnce::None) {
        head = piece.left(1);
        head = once == CaseOnce::Upper ? head.toUpper() : head.toLower();
        body = piece.mid(1);
        once = CaseOnce::None;
    }
    switch (run) {
        case CaseRun::None: break;
        case CaseRun::Upper: body = body.toUpper(); break;
        case CaseRun::Lower: body = body.toLower(); break;
    }
    out += head;
    out += body;
}

}  // namespace

QString expandReplacement(const Query& query, const QRegularExpressionMatch& match,
                          const QString& tmpl) {
    // БЕЗ ВЫРАЖЕНИЯ ЗАМЕНА БУКВАЛЬНА. «$1» в поле замены — это доллар и
    // единица: человек, меняющий цену на «$2», не должен получать чужую группу.
    if (!query.regex) return tmpl;

    QString out;
    out.reserve(tmpl.size());
    CaseRun run = CaseRun::None;
    CaseOnce once = CaseOnce::None;

    const auto group = [&](int number) {
        // Группы, которой не было в совпадении, — пустота (как в perl).
        return number <= match.lastCapturedIndex() ? match.captured(number) : QString();
    };

    for (qsizetype i = 0; i < tmpl.size();) {
        const QChar c = tmpl.at(i);

        if (c == QLatin1Char('$') && i + 1 < tmpl.size()) {
            qsizetype j = i + 1;
            int number = -1;
            if (tmpl.at(j) == QLatin1Char('{')) {
                const qsizetype close = tmpl.indexOf(QLatin1Char('}'), j + 1);
                bool ok = false;
                if (close > j + 1) {
                    const int value = tmpl.mid(j + 1, close - j - 1).toInt(&ok);
                    if (ok) {
                        number = value;
                        j = close + 1;
                    }
                }
            } else if (tmpl.at(j) == QLatin1Char('&')) {
                number = 0;
                ++j;
            } else if (tmpl.at(j).isDigit()) {
                int value = 0;
                while (j < tmpl.size() && tmpl.at(j).isDigit()) {
                    value = value * 10 + tmpl.at(j).digitValue();
                    ++j;
                }
                number = value;
            }
            if (number >= 0) {
                appendCased(out, group(number), run, once);
                i = j;
                continue;
            }
        }

        if (c == QLatin1Char('\\') && i + 1 < tmpl.size()) {
            const QChar next = tmpl.at(i + 1);
            if (next.isDigit()) {
                qsizetype j = i + 1;
                int value = 0;
                while (j < tmpl.size() && tmpl.at(j).isDigit()) {
                    value = value * 10 + tmpl.at(j).digitValue();
                    ++j;
                }
                appendCased(out, group(value), run, once);
                i = j;
                continue;
            }
            switch (next.unicode()) {
                case u'U': run = CaseRun::Upper; i += 2; continue;
                case u'L': run = CaseRun::Lower; i += 2; continue;
                case u'u': once = CaseOnce::Upper; i += 2; continue;
                case u'l': once = CaseOnce::Lower; i += 2; continue;
                case u'E': run = CaseRun::None; once = CaseOnce::None; i += 2; continue;
                case u'n': appendCased(out, QStringLiteral("\n"), run, once); i += 2; continue;
                case u't': appendCased(out, QStringLiteral("\t"), run, once); i += 2; continue;
                default: appendCased(out, QString(next), run, once); i += 2; continue;
            }
        }

        appendCased(out, QString(c), run, once);
        ++i;
    }
    return out;
}

HitLine hitLineInText(const QString& text, int offset, int length, int radius) {
    HitLine out;
    if (offset < 0 || offset > text.size()) return out;

    const auto isBreak = [](QChar c) {
        return c == QChar::LineSeparator || c == QLatin1Char('\n');
    };
    qsizetype from = offset;
    while (from > 0 && !isBreak(text.at(from - 1))) --from;
    qsizetype to = offset;
    while (to < text.size() && !isBreak(text.at(to))) ++to;

    const qsizetype start = qMax(from, qsizetype(offset) - radius);
    const qsizetype end = qMin(to, qsizetype(offset + length) + radius);
    QString line = text.mid(start, end - start);
    int at = int(offset - start);
    if (start > from) {
        line.prepend(QChar(0x2026));
        ++at;
    }
    if (end < to) line.append(QChar(0x2026));

    out.text = line;
    out.offset = at;
    // Совпадение выражения умеет тянуться через несколько строк; в списке
    // видна одна, и подсветка прижимается к её краю.
    out.length = qBound(0, length, int(line.size()) - at);
    return out;
}

void forEachHit(const QTextDocument& doc, const Query& query,
                const std::function<bool(const HitPlace&)>& sink) {
    if (!query.usable()) return;

    int index = 0;
    std::vector<ObjectSpan> objects;
    for (QTextBlock block = doc.begin(); block.isValid(); block = block.next(), ++index) {
        bool inObject = false;
        objects.clear();
        // У объекта (таблица, формула) ищем ПО ИСХОДНИКУ: в тексте блока один
        // U+FFFC. Строчные формулы подставлены исходником на местах — карта
        // objects говорит, где они.
        const QString text = withPlainBreaks(searchableTextOf(block, &inObject, &objects));
        if (text.isEmpty()) continue;

        bool stop = false;
        scanText(text, query, [&](int offset, int length, const QRegularExpressionMatch* match) {
            // Вхождение, пересёкшее границу строчного объекта, не считается:
            // рядом эти знаки стоят только в тексте поиска, а на экране между
            // ними вёрстка.
            const int span = inObject ? -1 : hitSpanIndex(objects, offset, offset + length);
            if (span == -2) return true;

            HitPlace place;
            place.blockIndex = index;
            place.block = &block;
            place.text = &text;
            place.objects = &objects;
            place.inObject = inObject;
            place.span = span;
            place.offset = offset;
            place.length = length;
            place.match = match;
            if (sink(place)) return true;
            stop = true;
            return false;
        });
        if (stop) return;
    }
}

}  // namespace zametti
