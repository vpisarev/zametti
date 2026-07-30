// Поиск: по списку брифа этапа 4.
//
// Три уровня, и каждый проверяется своим способом:
//   * чистая логика (search.h) — smart case, границы блоков, что вообще
//     считается текстом;
//   * поиск по хранилищу (store_search.h) — что он не блокирует UI-поток и что
//     новый запрос отменяет старый;
//   * поиск и замена в открытой заметке (NoteEditor) — счётчик, обход,
//     «заменить все» одним шагом отмены, дословность rawSource.

#include "editor_widget.h"
#include "parser.h"
#include "search.h"
#include "store_search.h"
#include "test_util.h"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QShortcut>
#include <QSignalSpy>
#include <QTest>
#include <QTextDocument>
#include <QVBoxLayout>

#include <string>

using zametti::Document;
using zametti::Query;
using zametti::SearchResult;

namespace {

QString g_root;

void note(const QString& id, const QString& meta, const QString& body) {
    QFile f(g_root + QLatin1Char('/') + id + QStringLiteral(".md"));
    if (!f.open(QIODevice::WriteOnly)) return;
    QString text = QStringLiteral("<!-- zametti\n") + meta + QStringLiteral("-->\n");
    if (!body.isEmpty()) text += QStringLiteral("\n") + body;
    f.write(text.toUtf8());
}

Document parseText(const QString& text) {
    const std::string bytes = text.toUtf8().toStdString();
    return zametti::parse(bytes);
}

int countIn(const QString& text, const QString& needle) {
    return int(zametti::findInDocument(parseText(text), zametti::makeQuery(needle)).size());
}

QString readFile(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return QStringLiteral("<нет файла>");
    return QString::fromUtf8(file.readAll());
}

// --- 1. Логика поиска -------------------------------------------------------

void checkSmartCase() {
    const QString corpus =
        QStringLiteral("# Заметка\n\nДом стоял на горе, а поодаль ещё один дом.\n");

    ZT_TRUE("нижний регистр ищет без учёта регистра", countIn(corpus, "дом") == 2);
    ZT_TRUE("заглавная в запросе делает поиск точным", countIn(corpus, "Дом") == 1);
    ZT_TRUE("запрос целиком заглавными тоже точный", countIn(corpus, "ДОМ") == 0);

    // Фолдинг юникодный: ASCII-tolower не тронул бы кириллицу вовсе, и «ёлка»
    // не нашла бы «Ёлка».
    ZT_TRUE("Ё и ё — одна буква при нечувствительном поиске",
            countIn(QStringLiteral("Ёлка и ёлка\n"), "ёлка") == 2);
    ZT_TRUE("умляуты тоже",
            countIn(QStringLiteral("Über über\n"), "über") == 2);

    const Query lower = zametti::makeQuery(QStringLiteral("дом"));
    const Query upper = zametti::makeQuery(QStringLiteral("Дом"));
    ZT_TRUE("smart case виден в самом запросе",
            !lower.caseSensitive && upper.caseSensitive);
    ZT_TRUE("порог в два знака", zametti::makeQuery(QStringLiteral("д")).tooShort() &&
                                     !lower.tooShort());
}

void checkSeesWhatUserSees() {
    // Метаданные лежат в Document::meta, а не в блоках, — поиск до них не
    // добирается по построению.
    const QString withMeta =
        QStringLiteral("<!-- zametti\ncreated: 2019-05-05T00:00:00Z\n-->\n\n"
                       "# Заголовок\n\nтекст без года\n");
    ZT_TRUE("год из created не находится", countIn(withMeta, "2019") == 0);
    ZT_TRUE("а из текста — находится",
            countIn(QStringLiteral("год 2019 в тексте\n"), "2019") == 1);

    // Маркеры разметки в тексте блока не живут: ищется то, что видно.
    ZT_TRUE("слово внутри ** ** находится",
            countIn(QStringLiteral("совсем **жирный** кусок\n"), "жирный") == 1);
    ZT_TRUE("звёздочки эмфазиса не ищутся",
            countIn(QStringLiteral("совсем **жирный** кусок\n"), "**жирный") == 0);
    ZT_TRUE("маркер пункта не ищется",
            countIn(QStringLiteral("- пункт списка\n"), "- пункт") == 0);
    ZT_TRUE("текст пункта ищется",
            countIn(QStringLiteral("- пункт списка\n"), "пункт списка") == 1);
    ZT_TRUE("решётки заголовка не ищутся",
            countIn(QStringLiteral("## Раздел\n"), "## Раздел") == 0);

    // Границу блока совпадение не пересекает — осознанное ограничение.
    ZT_TRUE("через границу блоков не находится",
            countIn(QStringLiteral("первый абзац\n\nвторой абзац\n"), "абзац второй") == 0);
    ZT_TRUE("внутри одного блока — находится",
            countIn(QStringLiteral("первый абзац второй\n"), "абзац второй") == 1);

    // Блок кода — текст: искать в нём надо.
    ZT_TRUE("в блоке кода ищется",
            countIn(QStringLiteral("```cpp\nint value = 42;\n```\n"), "value") == 1);

    // Перекрывающиеся вхождения считаются все: F3 обойдёт их так же.
    ZT_TRUE("перекрывающиеся вхождения считаются",
            countIn(QStringLiteral("ааа\n"), "аа") == 2);
}

void checkHitLine() {
    const Document doc = parseText(
        QStringLiteral("```\nочень длинная строка, в середине которой прячется "
                       "искомое слово, и дальше ещё столько же текста подряд\n```\n"));
    const auto hits = zametti::findInDocument(doc, zametti::makeQuery(QStringLiteral("искомое")));
    ZT_TRUE("совпадение в длинной строке найдено", hits.size() == 1);
    if (hits.empty()) return;
    const zametti::HitLine line = zametti::hitLine(doc, hits[0]);
    ZT_TRUE("строка обрезана по краям", line.text.size() < 130);
    ZT_TRUE("совпадение на своём месте в обрезанной строке",
            line.text.mid(line.offset, line.length) == QStringLiteral("искомое"));
    ZT_TRUE("обрезка помечена многоточием", line.text.startsWith(QChar(0x2026)));
}

// --- 2. Поиск по хранилищу --------------------------------------------------

void checkStoreSearch() {
    zametti::StoreSearch search;
    QSignalSpy spy(&search, &zametti::StoreSearch::found);

    search.search(g_root, QStringLiteral("иголка"));
    ZT_TRUE("ответ пришёл", spy.wait(5000));
    ZT_TRUE("ровно один ответ", spy.count() == 1);
    if (spy.isEmpty()) return;
    {
        const auto results = spy.at(0).at(1).value<QVector<SearchResult>>();
        ZT_TRUE("иголка нашлась в одной заметке", results.size() == 1);
        ZT_TRUE("и это стог", results.isEmpty() ||
                                  results[0].title == QStringLiteral("Стог"));
        ZT_TRUE("строка результата несёт совпадение",
                results.isEmpty() ||
                    results[0].line.mid(results[0].lineOffset, results[0].lineLength) ==
                        QStringLiteral("иголка"));
    }

    // Отмена: пускаем запрос и тут же перебиваем другим. В списке должен
    // оказаться ответ только на второй — первый отменяется между файлами.
    spy.clear();
    search.search(g_root, QStringLiteral("сено"));
    search.search(g_root, QStringLiteral("иголка"));
    ZT_TRUE("ответ на второй запрос пришёл", spy.wait(5000));
    QTest::qWait(300);   // дать отменённому шанс всё-таки ответить
    bool stale = false;
    for (int i = 0; i < spy.count(); ++i)
        if (spy.at(i).at(0).toString() == QStringLiteral("сено")) stale = true;
    ZT_TRUE("результатов отменённого запроса нет", !stale);
    ZT_TRUE("ответ ровно один", spy.count() == 1);

    // UI-поток при этом свободен: замеряем, сколько занимает сам вызов.
    QElapsedTimer timer;
    timer.start();
    search.search(g_root, QStringLiteral("сено"));
    const qint64 blocked = timer.elapsed();
    ZT_TRUE("запуск поиска не занимает UI-поток", blocked < 20);
    spy.wait(5000);
}

// --- 3. Поиск и замена в открытой заметке -----------------------------------

void checkEditorSearch() {
    const QString path = g_root + QStringLiteral("/00000000000009.md");
    note("00000000000009", "modified: 2025-01-01T00:00:00Z\n",
         "# Повторы\n\nсено и сено, а рядом опять сено.\n\n"
         "| столбец | сено |\n| --- | --- |\n| сено | ещё |\n");

    zametti::NoteEditor editor;
    editor.resize(700, 500);
    editor.show();
    QTest::qWait(20);
    editor.openFile(path);
    QTest::qWait(20);

    const int found = editor.findMatches(QStringLiteral("сено"), false);
    ZT_TRUE("все вхождения найдены, включая дословную таблицу", found == 5);
    ZT_TRUE("до первого перехода текущего нет", editor.currentMatch() == -1);

    editor.stepMatch(1);
    ZT_TRUE("первый шаг встаёт на совпадение", editor.currentMatch() == 0);
    editor.stepMatch(-1);
    ZT_TRUE("шаг назад ходит по кругу", editor.currentMatch() == found - 1);
    editor.stepMatch(1);
    ZT_TRUE("и вперёд по кругу тоже", editor.currentMatch() == 0);

    // Подсветка живёт вне документа: правок она не делает.
    ZT_TRUE("подсветка не пометила заметку изменённой", !editor.document()->isModified());

    // «Заменить все» — один шаг отмены.
    const QString before = editor.toPlainText();
    const int replaced =
        editor.replaceAllMatches(QStringLiteral("сено"), false, QStringLiteral("солома"));
    ZT_TRUE("заменены все вхождения", replaced == 5);
    ZT_TRUE("в тексте не осталось искомого",
            !editor.toPlainText().contains(QStringLiteral("сено")));
    editor.undo();
    QTest::qWait(20);
    ZT_TRUE("одна отмена возвращает всё", editor.toPlainText() == before);

    // Дословный кусок остаётся дословным: заменяется только текст, разметка
    // таблицы цела.
    editor.replaceAllMatches(QStringLiteral("сено"), false, QStringLiteral("солома"));
    editor.save(false);
    QTest::qWait(20);
    const QString written = readFile(path);
    ZT_TRUE("таблица осталась таблицей",
            written.contains(QStringLiteral("| столбец | солома |")) &&
                written.contains(QStringLiteral("| --- | --- |")));
    ZT_TRUE("метаданные целы", written.contains(QStringLiteral("<!-- zametti")));
    ZT_TRUE("в файле нет искомого", !written.contains(QStringLiteral("сено")));
}

// Сочетания должны доходить до окна, а не застревать в редакторе: QTextEdit
// объявляет своими куда больше сочетаний, чем кажется, и через ShortcutOverride
// съедает их молча. На этом уже дважды ловились (Ctrl+Z и Ctrl+N), поэтому
// проверяем механически.
void checkShortcutsReachWindow() {
    QWidget window;
    auto* layout = new QVBoxLayout(&window);
    zametti::NoteEditor editor;
    layout->addWidget(&editor);
    window.resize(600, 400);
    window.show();
    QTest::qWait(20);
    editor.setFocus();
    QTest::qWait(20);

    struct Probe {
        const char* name;
        QKeySequence keys;
        Qt::Key key;
        Qt::KeyboardModifiers mods;
        bool fired = false;
    };
    Probe probes[] = {
        {"Ctrl+F доходит до окна", QKeySequence::Find, Qt::Key_F, Qt::ControlModifier},
        {"Ctrl+H доходит до окна", QKeySequence::Replace, Qt::Key_H, Qt::ControlModifier},
        {"Ctrl+Shift+F доходит до окна", QKeySequence(QStringLiteral("Ctrl+Shift+F")),
         Qt::Key_F, Qt::ControlModifier | Qt::ShiftModifier},
        {"F3 доходит до окна", QKeySequence(Qt::Key_F3), Qt::Key_F3, Qt::NoModifier},
        {"Shift+F3 доходит до окна", QKeySequence(Qt::SHIFT | Qt::Key_F3), Qt::Key_F3,
         Qt::ShiftModifier},
    };
    for (Probe& probe : probes) {
        auto* shortcut = new QShortcut(probe.keys, &window);
        QObject::connect(shortcut, &QShortcut::activated, &window,
                         [&probe] { probe.fired = true; });
    }
    for (Probe& probe : probes) {
        QTest::keyClick(&editor, probe.key, probe.mods);
        QTest::qWait(10);
        ZT_TRUE(probe.name, probe.fired);
    }
}

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    g_root = QDir::tempPath() + QStringLiteral("/zametti-search-test");
    QDir(g_root).removeRecursively();
    QDir().mkpath(g_root + QStringLiteral("/.zametti"));

    note("00000000000001", "created: 2019-01-01T00:00:00Z\nmodified: 2020-01-01T00:00:00Z\n",
         "# Стог\n\nв стоге сена нашлась иголка\n");
    note("00000000000002", "modified: 2021-01-01T00:00:00Z\n",
         "# Сено\n\nсено, солома, снова сено\n");
    note("00000000000003", "modified: 2022-01-01T00:00:00Z\n",
         "# Тишина\n\nздесь ничего такого нет\n");

    checkSmartCase();
    checkSeesWhatUserSees();
    checkHitLine();
    checkStoreSearch();
    checkEditorSearch();
    checkShortcutsReachWindow();

    QDir(g_root).removeRecursively();
    return zt::report("поиск");
}
