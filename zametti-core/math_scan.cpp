#include "math_scan.h"

namespace zametti {
namespace {

bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
bool isDigit(char c) { return c >= '0' && c <= '9'; }

// Сколько подряд идущих обратных косых стоит перед этим байтом. Нечётное число
// означает, что сам байт экранирован: `\$` — литеральный доллар, а `\\$` —
// экранированная косая и НАСТОЯЩИЙ доллар.
size_t backslashesBefore(std::string_view text, size_t at) {
    size_t count = 0;
    while (at > count && text[at - count - 1] == '\\') ++count;
    return count;
}

bool escaped(std::string_view text, size_t at) {
    return (backslashesBefore(text, at) % 2) == 1;
}

}  // namespace

bool mathBordersOk(std::string_view text, size_t open, size_t close, bool display) {
    const size_t skip = display ? 2 : 1;
    if (close <= open + skip) return false;               // пустое тело
    if (open + skip >= text.size()) return false;
    if (display) return true;                              // у выключной правил нет
    if (isSpace(text[open + 1])) return false;             // открывающий перед пробелом
    if (isSpace(text[close - 1])) return false;            // закрывающий после пробела
    if (close + 1 < text.size() && isDigit(text[close + 1])) return false;   // «цена $5 и $10»
    return true;
}

std::vector<MathSpan> scanMath(std::string_view text) {
    std::vector<MathSpan> found;
    size_t i = 0;
    while (i < text.size()) {
        if (text[i] != '$' || escaped(text, i)) {
            ++i;
            continue;
        }

        const bool display = i + 1 < text.size() && text[i + 1] == '$';
        const size_t open = i;
        const size_t bodyStart = open + (display ? 2 : 1);

        if (display) {
            // Выключная: ищем закрывающие `$$`, хоть через десять строк. Правил
            // про пробелы у неё нет — она и так стоит отдельно.
            size_t j = bodyStart;
            size_t close = std::string_view::npos;
            while (j + 1 < text.size()) {
                if (text[j] == '$' && text[j + 1] == '$' && !escaped(text, j)) {
                    close = j;
                    break;
                }
                ++j;
            }
            if (close == std::string_view::npos || close == bodyStart) {
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
        if (bodyStart >= text.size() || isSpace(text[bodyStart])) {
            ++i;
            continue;
        }

        size_t j = bodyStart;
        size_t close = std::string_view::npos;
        while (j < text.size()) {
            if (text[j] == '$' && !escaped(text, j)) {
                // Закрывающий не после пробела и не перед цифрой. Второе — то
                // самое правило про «цена $5 и $10»: без него всё, что между
                // двумя ценами, оказалось бы математикой.
                if (mathBordersOk(text, open, j, false)) {
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
                while (k < text.size() && (text[k] == ' ' || text[k] == '\t')) ++k;
                if (k < text.size() && text[k] == '\n') break;
            }
            ++j;
        }
        if (close == std::string_view::npos) {
            ++i;
            continue;
        }
        found.push_back(MathSpan{int32_t(open), int32_t(close + 1), false});
        i = close + 1;
    }
    return found;
}

}  // namespace zametti
