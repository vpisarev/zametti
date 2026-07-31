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

// Пустое хранилище: каталог, служебные ".zametti/" и ".rescue/" (побитые
// файлы редактора; на сервер не синхронизируется) и "history/" (история
// заметок; синхронизируется). Отказывается работать в непустом каталоге.
// false — и объяснение в error.
bool initStore(const QString& dir, QString* error);

// Пустая заметка с корректным id и каркасом метаданных (created — сейчас;
// parent — если задан и существует). Возвращает путь к файлу; пусто — ошибка.
QString newNote(const QString& root, const QString& parentId, QString* error);

// Один .md-файл в хранилище. Источник НЕ трогается: делается копия под
// свежим id, с нашей шапкой метаданных и в каноническом виде — то есть
// serialize(parse(x)). Канонизация сразу, а не когда-нибудь: человек должен
// увидеть результат импорта немедленно, а не через первое сохранение.
//
// Времена берутся из шапки источника, если она наша; иначе из файловой
// системы. id и role источника не наследуются никогда: id принадлежит этому
// хранилищу, а role сделал бы из заметки папку.
//
// Возвращает путь созданной заметки; пусто — ошибка, объяснение в error.
QString importNote(const QString& root, const QString& parentId, const QString& sourcePath,
                   QString* error);

struct ImportOptions {
    QString root;
    QString from;
    QString appleManifest;   // пусто — манифеста нет
    bool dryRun = false;
};

// Импорт дерева .md: подкаталог → заметка-каталог, файл → заметка, иерархия
// через parent, содержимое через parse/serialize ядра (импорт и есть
// нормализация). Хранилище плоское до конца: вложения получают такие же
// 14-значные id, как заметки, и ложатся в тот же каталог ("<id>.webp");
// одинаковое содержимое — один файл. png перегоняется в webp без потерь
// (cwebp), heic — в webp q90 (heif-convert + cwebp), jpeg и webp — байт в
// байт; без кодеков — копия как есть и беда в отчёте. Вики-вложения
// ("![[attach/x|W]]", "[[attach/x]]") переписываются в канон image-спана
// "![родное-имя](<id>.ext#w=W)"; прочие wikilinks не трогаются. Ссылки на
// .md внутри набора — на "<id>.md". Отчёт соответствия кладётся рядом с
// хранилищем: "<root>.import-report.txt" (не внутрь: verify считает чужие
// файлы бедой).
bool importTree(const ImportOptions& options, Report& report);

// Полная проверка: id-имена у заметок и вложений, метаданные, ноль дрейфа
// serialize(parse(x)), ссылки parent и циклы, существование целей
// ![...]-ссылок, осиротевшие вложения (отчёт, не удалять). Служебные
// .zametti/, .rescue/ и history/ прозрачны.
bool verifyStore(const QString& root, Report& report);

}  // namespace zametti::store

#endif  // ZAMETTI_STORE_H
