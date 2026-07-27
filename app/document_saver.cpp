#include "document_saver.h"

#include "document_reader.h"
#include "json_dump.h"
#include "parser.h"
#include "serializer.h"

#include <QDateTime>
#include <QFile>
#include <QSaveFile>

#include <algorithm>
#include <vector>

namespace zametti {
namespace {

QByteArray toBytes(const std::string& text) {
    return QByteArray(text.data(), static_cast<qsizetype>(text.size()));
}

// Содержимое файла целиком. Заметки маленькие, и побайтовое сравнение и точнее
// хеша, и короче: не надо рассуждать о коллизиях. Читаем именно файл, а не
// помним последнюю запись, — тогда правка снаружи не приводит к «уже сохранено».
QByteArray fileContents(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return file.readAll();
}

bool writeFile(const QString& path, const QByteArray& data, QString* error) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error != nullptr) *error = file.errorString();
        return false;
    }
    const bool ok = file.write(data) == data.size() && file.flush();
    if (!ok && error != nullptr) *error = file.errorString();
    file.close();
    return ok;
}

}  // namespace

QString rescueTimestamp() {
    return QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
}

namespace {

bool isSpace(char c) { return c == ' ' || c == '\t'; }

// Совпадают ли строение и текст. Разметка внутри строки не сравнивается: см.
// пояснение в самопроверке.
bool sameSkeleton(const Document& a, const Document& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].rawSource != b[i].rawSource) return false;
        if (!a[i].rawSource.empty()) continue;
        if (a[i].kind != b[i].kind || a[i].level != b[i].level ||
            a[i].headingLevel != b[i].headingLevel || a[i].info != b[i].info ||
            a[i].text != b[i].text)
            return false;
    }
    return true;
}

// Неразрывный пробел в UTF-8. Именно им сохраняются отступы: обычный пробел в
// начале строки markdown съедает, а этот — нет.
//
// Совет писать сущность "&nbsp;" не годится: ядро отдаёт её буквальным текстом,
// и в заметке было бы видно "&nbsp;" вместо отступа. Прямой знак проходит круг
// целиком — проверено, включая схему из трёх строк.
const char* const kNbsp = "\xC2\xA0";

// Края строк. Ведущие пробелы становятся неразрывными — отступ значим, им
// рисуют схемы и лесенки. Концевые выбрасываются: они как ведущие нули,
// незначащие, а markdown их всё равно съедает.
//
// Строка из одних пробелов считается пустой: несколько раз нажатый пробел на
// пустой строке — не отступ, и оставлять от него неразрывные знаки незачем.
//
// Литеральные блоки не трогаем: в коде и дословных кусках пробел и так значим.
Block withEdgesNormalised(Block block) {
    // В коде пробел значим — его копируют и вставляют в терминал, и хитрым
    // знакам там взяться неоткуда. Трогаем только завершающий перевод строки:
    // забор всё равно ставится с новой строки, и без него разбор вернул бы
    // текст с переводом, а самопроверка честно не дала бы записать.
    if (block.kind == Kind::Code && block.rawSource.empty()) {
        if (!block.text.empty() && block.text.back() != '\n') block.text.push_back('\n');
        return block;
    }
    if (!block.rawSource.empty()) return block;

    // Висящий перенос в конце — след только что нажатого Enter. В файле он даёт
    // пустую строку, а пустая строка абзац заканчивает.
    while (!block.text.empty() && block.text.back() == '\n') block.text.pop_back();

    const std::string& text = block.text;
    std::vector<int> map(text.size() + 1, 0);
    std::string out;

    size_t line = 0;
    for (;;) {
        size_t end = text.find('\n', line);
        const bool last = end == std::string::npos;
        if (last) end = text.size();

        size_t start = line;
        while (start < end && isSpace(text[start])) ++start;
        size_t stop = end;
        while (stop > start && isSpace(text[stop - 1])) --stop;

        // Пустая строка внутри блока — не содержимое: в файле она блок
        // заканчивает, и разбор вернул бы два блока вместо одного. Такую строку
        // выбрасываем вместе с её разделителем.
        const bool blank = start >= stop;
        if (!blank && !out.empty()) out.push_back('\n');

        // Ведущие пробелы: каждый становится неразрывным. Отступ значим, им
        // рисуют схемы и лесенки.
        for (size_t k = line; k < start; ++k) {
            map[k] = int(out.size());
            if (!blank) out += kNbsp;
        }
        for (size_t k = start; k < stop; ++k) {
            map[k] = int(out.size());
            out.push_back(text[k]);
        }
        for (size_t k = stop; k <= end && k < text.size(); ++k) map[k] = int(out.size());

        if (last) {
            map[text.size()] = int(out.size());
            break;
        }
        line = end + 1;
    }

    for (Span& span : block.inlines) {
        const size_t from = size_t(qBound(0, span.offset, int(text.size())));
        const size_t to = size_t(qBound(0, span.offset + span.length, int(text.size())));
        span.offset = map[from];
        span.length = map[to] - map[from];
    }
    block.inlines.erase(std::remove_if(block.inlines.begin(), block.inlines.end(),
                                       [](const Span& s) { return s.length <= 0; }),
                        block.inlines.end());
    block.text = std::move(out);
    return block;
}

// Пустой абзац markdown выразить нечем: пустая строка в файле — разделитель
// блоков, а не блок. В документе он заводится каждым Enter, и без этой уборки
// самопроверка честно ловила бы расхождение при каждом сохранении.
//
// Пустой пункт списка при этом остаётся: "-" в файле записывается прекрасно.
Document forFile(Document doc) {
    Document out;
    out.reserve(doc.size());
    for (Block& block : doc) {
        Block trimmed = withEdgesNormalised(std::move(block));
        if (trimmed.rawSource.empty() && trimmed.kind == Kind::Paragraph &&
            trimmed.text.empty())
            continue;
        out.push_back(std::move(trimmed));
    }
    return out;
}

}  // namespace

SaveOutcome saveDocument(const QTextDocument& doc, const QString& path,
                         const QString& timestamp, DocumentReaderFn reader) {
    const Document ir =
        forFile(reader ? reader(doc) : readDocument(doc));
    const QByteArray text = toBytes(serialize(ir));

    if (QFile::exists(path) && fileContents(path) == text)
        return {SaveResult::Unchanged, {}, {}, {}, false};

    // Последний рубеж: то, что мы собрались записать, должно читаться обратно в
    // тот же документ.
    //
    // Строго сверяются строение и текст: число блоков, род, уровень, язык,
    // содержимое. Разметка внутри строки может оказаться богаче — голую ссылку
    // человек набирает текстом, а файл читает её ссылкой, и не дать этого
    // записать значило бы запретить писать ссылки. Байт при этом не теряется:
    // текст блока обязан совпасть до знака.
    const Document reread =
        parse(std::string(text.constData(), static_cast<size_t>(text.size())));
    if (!sameSkeleton(ir, reread)) {
        const QString rescuePath = path + QStringLiteral(".rescue-") + timestamp;
        QString error;
        if (!writeFile(rescuePath, text, &error)) {
            return {SaveResult::Failed,
                    QStringLiteral("самопроверка не прошла, и аварийный файл не записан: ") +
                        error,
                    {}, {}, false};
        }
        return {SaveResult::Rescued,
                QStringLiteral("самопроверка перед записью не прошла: разобранное обратно "
                               "не совпало с документом. Файл не тронут, буфер сохранён в ") +
                    rescuePath,
                rescuePath, {}, false};
    }

    // Замена файла целиком и разом: QSaveFile пишет во временный файл рядом и
    // переименовывает его на commit. Оборванная запись не оставит половину
    // заметки на месте целой.
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return {SaveResult::Failed,
                QStringLiteral("не открыть на запись: ") + file.errorString(), {}, {}, false};
    }
    file.write(text);
    if (!file.commit()) {
        return {SaveResult::Failed, QStringLiteral("не записать: ") + file.errorString(), {},
                {}, false};
    }
    return {SaveResult::Written, {}, {}, reread, toJson(reread) != toJson(ir)};
}

}  // namespace zametti
