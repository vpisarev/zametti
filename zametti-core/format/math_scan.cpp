#include "math_scan.h"

#include <QChar>

namespace zametti {
namespace {

// Одна реализация над двумя видами текста: байты и QStringView. Правила ASCII,
// и сравнения с char-литералами верны для обоих (QChar сравнивается с char).
template <class View>
bool isSpaceAt(const View& text, size_t i) {
    const auto c = text[i];
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}
template <class View>
bool isDigitAt(const View& text, size_t i) {
    const auto c = text[i];
    return c >= '0' && c <= '9';
}

constexpr size_t kNone = static_cast<size_t>(-1);

template <class View>
size_t sizeOf(const View& text) { return static_cast<size_t>(text.size()); }

// Сколько подряд идущих обратных косых стоит перед этим знаком. Нечётное число
// означает, что сам знак экранирован: `\$` — литеральный доллар, а `\\$` —
// экранированная косая и НАСТОЯЩИЙ доллар.
template <class View>
size_t backslashesBefore(const View& text, size_t at) {
    size_t count = 0;
    while (at > count && text[at - count - 1] == '\\') ++count;
    return count;
}

template <class View>
bool escaped(const View& text, size_t at) {
    return (backslashesBefore(text, at) % 2) == 1;
}

template <class View>
bool mathBordersOkT(const View& text, size_t open, size_t close, bool display) {
    const size_t skip = display ? 2 : 1;
    if (close <= open + skip) return false;               // пустое тело
    if (open + skip >= sizeOf(text)) return false;
    if (display) return true;                              // у выключной правил нет
    if (isSpaceAt(text, open + 1)) return false;           // открывающий перед пробелом
    if (isSpaceAt(text, close - 1)) return false;          // закрывающий после пробела
    if (close + 1 < sizeOf(text) && isDigitAt(text, close + 1)) return false;   // «цена $5 и $10»
    return true;
}

template <class View>
std::vector<MathSpan> scanMathT(const View& text) {
    std::vector<MathSpan> found;
    size_t i = 0;
    while (i < sizeOf(text)) {
        if (text[i] != '$' || escaped(text, i)) {
            ++i;
            continue;
        }

        const bool display = i + 1 < sizeOf(text) && text[i + 1] == '$';
        const size_t open = i;
        const size_t bodyStart = open + (display ? 2 : 1);

        if (display) {
            // Выключная: ищем закрывающие `$$`, хоть через десять строк. Правил
            // про пробелы у неё нет — она и так стоит отдельно.
            size_t j = bodyStart;
            size_t close = kNone;
            while (j + 1 < sizeOf(text)) {
                if (text[j] == '$' && text[j + 1] == '$' && !escaped(text, j)) {
                    close = j;
                    break;
                }
                ++j;
            }
            if (close == kNone || close == bodyStart) {
                // Пары нет или тело пустое — это не формула, а просто доллары.
                ++i;
                continue;
            }
            found.push_back(MathSpan{int32_t(open), int32_t(close + 2), true});
            i = close + 2;
            continue;
        }

        // Инлайн. Открывающий доллар не должен стоять перед пробелом: «$ x$» —
        // не формула. Пустое тело («$$» уже разобрано выше) тоже не формула.
        if (bodyStart >= sizeOf(text) || isSpaceAt(text, bodyStart)) {
            ++i;
            continue;
        }

        size_t j = bodyStart;
        size_t close = kNone;
        while (j < sizeOf(text)) {
            if (text[j] == '$' && !escaped(text, j)) {
                // Закрывающий не после пробела и не перед цифрой. Второе — то
                // самое правило про «цена $5 и $10»: без него всё, что между
                // двумя ценами, оказалось бы математикой.
                if (mathBordersOkT(text, open, j, false)) {
                    close = j;
                    break;
                }
                // Не подошёл — ищем следующий, формула может кончаться дальше.
                ++j;
                continue;
            }
            // ПУСТАЯ СТРОКА КОНЧАЕТ ПОИСК. Формула вправе переехать через
            // перенос строки (в корпусе таких три), но не через границу абзаца:
            // иначе одинокий доллар склеивал бы полдокумента в «формулу».
            if (text[j] == '\n') {
                size_t k = j + 1;
                while (k < sizeOf(text) && (text[k] == ' ' || text[k] == '\t')) ++k;
                if (k < sizeOf(text) && text[k] == '\n') break;
            }
            ++j;
        }
        if (close == kNone) {
            ++i;
            continue;
        }
        found.push_back(MathSpan{int32_t(open), int32_t(close + 1), false});
        i = close + 1;
    }
    return found;
}

}  // namespace

bool mathBordersOk(std::string_view text, size_t open, size_t close, bool display) {
    return mathBordersOkT(text, open, close, display);
}
bool mathBordersOk(QStringView text, size_t open, size_t close, bool display) {
    return mathBordersOkT(text, open, close, display);
}
std::vector<MathSpan> scanMath(std::string_view text) { return scanMathT(text); }
std::vector<MathSpan> scanMath(QStringView text) { return scanMathT(text); }

bool wholeMath(QStringView text) {
    const std::vector<MathSpan> found = scanMath(text);
    return found.size() == 1 && found.front().start == 0 && found.front().end == text.size();
}

}  // namespace zametti
