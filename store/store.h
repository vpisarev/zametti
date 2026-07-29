// Плоское хранилище заметок: init / new / import / verify.
//
// Библиотека, а не только CLI: тесты зовут функции напрямую, main — тонкий
// разбор аргументов. Правило безопасности: импорт НИКОГДА не пишет в
// источник — новое хранилище создаётся рядом, старое дерево остаётся эталоном.

#ifndef ZAMETTI_STORE_H
#define ZAMETTI_STORE_H

#include <QString>
#include <QStringList>

namespace zametti::store {

// Отчёт — человекочитаемые строки; беды считаются отдельно, по ним код
// возврата. Молчаливых пропусков нет: всё несопоставившееся — в отчёт.
struct Report {
    QStringList lines;
    int problems = 0;

    void note(const QString& line) { lines.append(line); }
    void problem(const QString& line) {
        lines.append(QStringLiteral("БЕДА: ") + line);
        ++problems;
    }
};

// Пустое хранилище: каталог, ".zametti/". Отказывается работать в непустом
// каталоге. false — и объяснение в error.
bool initStore(const QString& dir, QString* error);

// Пустая заметка с корректным id и каркасом метаданных (created — сейчас;
// parent — если задан и существует). Возвращает путь к файлу; пусто — ошибка.
QString newNote(const QString& root, const QString& parentId, QString* error);

struct ImportOptions {
    QString root;
    QString from;
    QString appleManifest;   // пусто — манифеста нет
    bool dryRun = false;
};

// Импорт дерева .md: подкаталог → заметка-каталог, файл → заметка, иерархия
// через parent, содержимое через parse/serialize ядра (импорт и есть
// нормализация). Вложения — в attachments/ под хеш-именами байт в байт,
// ссылки на .md внутри набора — на "<id>.md". Отчёт соответствия кладётся
// рядом с хранилищем: "<root>.import-report.txt" (не внутрь: verify считает
// чужие файлы в хранилище бедой).
bool importTree(const ImportOptions& options, Report& report);

// Полная проверка: id-имена, метаданные, ноль дрейфа serialize(parse(x)),
// ссылки parent и циклы, цели ![...]-ссылок в attachments/ и соответствие
// хеш-имён содержимому, осиротевшие вложения (отчёт, не удалять).
bool verifyStore(const QString& root, Report& report);

}  // namespace zametti::store

#endif  // ZAMETTI_STORE_H
