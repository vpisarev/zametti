// Сохранение заметки.
//
// Путь один: QTextDocument → document_reader → IR → serialize. Никакого второго
// способа получить текст файла нет и быть не должно.
//
// Перед тем как заменить файл, записанный текст разбирается обратно и
// полученный IR сравнивается с IR документа. Не совпало — старый файл не
// трогается вовсе, а буфер уходит в аварийный файл рядом. Это последний рубеж
// против потери данных, и он не отключается.

#ifndef ZAMETTI_DOCUMENT_SAVER_H
#define ZAMETTI_DOCUMENT_SAVER_H

#include "ir.h"

#include <QString>

#include <functional>

class QTextDocument;

namespace zametti {

enum class SaveResult {
    Unchanged,   // на диске уже ровно это — не трогаем даже mtime
    Written,     // записано
    Rescued,     // самопроверка не прошла: файл цел, буфер в аварийном файле
    Failed,      // записать не удалось вовсе
};

struct SaveOutcome {
    SaveResult result = SaveResult::Failed;
    QString message;      // человеку, при Rescued и Failed
    QString rescuePath;   // непусто при Rescued
    // Как записанное прочтётся обратно. Обычно это ровно то, что было в
    // документе, но не всегда: голую ссылку человек набирает текстом, а файл
    // читает её ссылкой. Виджет догоняет документ этим содержимым.
    Document reread;
    bool differsFromDocument = false;
};

// reader подменяется только тестом самопроверки: испортить читателя иначе
// нечем, а проверять последний рубеж обязательно.
using DocumentReaderFn = std::function<Document(const QTextDocument&)>;

// Что из документа уйдёт в файл: приведение к тому, что markdown умеет
// выразить. Наружу — ради фаззинга операций: он обязан проверять ровно ту же
// сверку, что и запись, иначе проверял бы не то.
Document documentForFile(Document doc);

// Совпадают ли строение и текст. Разметка внутри строки не сравнивается: голую
// ссылку человек набирает текстом, а файл читает её ссылкой.
bool sameSkeleton(const Document& a, const Document& b);

SaveOutcome saveDocument(const QTextDocument& doc, const QString& path,
                         const QString& timestamp, DocumentReaderFn reader = nullptr);

// Отметка времени для имени аварийного файла: вынесена наружу, чтобы тест не
// зависел от часов.
QString rescueTimestamp();

}  // namespace zametti

#endif  // ZAMETTI_DOCUMENT_SAVER_H
