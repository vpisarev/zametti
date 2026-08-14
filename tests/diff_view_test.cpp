// Два вида разности в режиме истории — на живом редакторе.
//
// Главная проверка здесь одна, и она же — инвариант B этапа: НАБОРЫ
// ИЗМЕНЁННЫХ СТРОК У ОБОИХ ВИДОВ СОВПАДАЮТ. Вид «как под капотом» показывает
// строки сравнения напрямую и потому служит эталоном; вид с полосками идёт до
// строк через блоки, и расхождение означало бы баг соответствия
// «строка ↔ блок», а не мелочь оформления. Гоняется на корпусе, если он есть.
//
// Остальное — то, что глазами проверяется плохо: Tab не сбивает позицию,
// удержание Alt показывает вторую сторону, F4 ходит по кругу, переключатель
// базы и вправду меняет сравнение, заглушка говорит, сколько строк пропало.
//
// Снимки приёмки кладутся, если указан каталог вторым аргументом.

#include "diff.h"
#include "diff_view.h"
#include "document_reader.h"
#include "editor_widget.h"
#include "serializer.h"
#include "history_panel.h"
#include "journal.h"
#include "settings.h"

#include "test_util.h"

#include <vector>
#include "testdata.h"

#include <QApplication>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QKeyEvent>
#include <QTest>
#include <QAbstractTextDocumentLayout>
#include <QListWidget>
#include <QScrollBar>
#include <QVBoxLayout>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTextDocument>

#include <set>
#include <string>

using namespace zametti;

namespace {

template <typename T>
std::string num(T value) { return std::to_string(value); }

QString g_root;
QString g_corpus;

// Заметка с историей: две версии, обе в журнале. Возвращает путь.
QString makeNoteWithHistory(const QString& id, const QByteArray& first,
                            const QByteArray& second) {
    QDir().mkpath(g_root + QStringLiteral("/.zametti"));
    QDir().mkpath(g_root + QStringLiteral("/history"));
    const QString path = g_root + QLatin1Char('/') + id + QStringLiteral(".md");
    QFile file(path);
    if (file.open(QIODevice::WriteOnly)) file.write(second);
    file.close();

    journal::History history(g_root);
    QString error;
    const qint64 now = 1'700'000'000'000LL;
    history.append(id, journal::Kind::Save, now, first, 0, &error);
    history.append(id, journal::Kind::Save, now + 60'000, second, 0, &error);
    return path;
}

QByteArray note(const char* body, const char* stamp) {
    return QByteArray("<!-- zametti\ncreated: 2026-01-01T00:00:00Z\nmodified: ") + stamp +
           "\n-->\n\n" + body;
}

// Строки, которые вид «как под капотом» считает тронутыми: у него блок
// документа и строка сравнения — одно и то же.
std::set<int> changedLinesFromPlain(NoteEditor& editor) {
    std::set<int> out;
    const QVector<diff::Mark>& marks = editor.diffMarks();
    const diff::Result& result = editor.diffResult();
    for (int i = 0; i < marks.size() && i < result.rows.size(); ++i) {
        if (marks[i] == diff::Mark::Same) continue;
        if (result.rows[i].after >= 0) out.insert(result.rows[i].after);
    }
    return out;
}

// Строки, которые вид с полосками объявляет тронутыми: все строки тронутых
// блоков. Множество ШИРЕ — блок целиком тронутым не бывает, — поэтому и
// сверяются они по-разному: эталон обязан лежать внутри, а каждый тронутый
// блок обязан содержать хоть одну строку эталона.
std::set<int> changedLinesFromBars(NoteEditor& editor) {
    std::set<int> out;
    const QVector<diff::Mark>& marks = editor.diffMarks();
    // Номер блока ПОКАЗАННОГО документа переводим в блок слепка картой
    // источников: показывается копия, и номера у неё свои.
    const QVector<int>& source = editor.diffSourceBlocks();
    const diff::Text& text = editor.diffShownText();
    const QTextDocument* doc = editor.document();
    int number = 0;
    for (QTextBlock block = doc->begin(); block.isValid(); block = block.next(), ++number) {
        if (number >= marks.size() || marks[number] == diff::Mark::Same) continue;
        const int ir = number < source.size() ? source[number] : -1;
        if (ir < 0 || ir >= text.blocks.size()) continue;   // дорисованное нами
        for (int line = text.blocks[ir].first;
             line < text.blocks[ir].first + text.blocks[ir].count; ++line)
            out.insert(line);
    }
    return out;
}

// ИНВАРИАНТ B. Эталон — вид «как под капотом».
void checkViewsAgree(NoteEditor& editor, const std::string& what) {
    editor.setDiffPlainView(true);
    const std::set<int> plain = changedLinesFromPlain(editor);
    editor.setDiffPlainView(false);
    const std::set<int> bars = changedLinesFromBars(editor);

    bool covered = true;
    for (int line : plain) covered = covered && bars.count(line) > 0;
    ZT_TRUE(what + ": каждая изменённая строка попала в полоску (" + num(plain.size()) +
                " из " + num(bars.size()) + ")",
            covered);

    // И наоборот: полоска без единой изменённой строки — обещание пустоты.
    const QVector<diff::Mark>& marks = editor.diffMarks();
    const diff::Text& text = editor.diffShownText();
    const QTextDocument* doc = editor.document();
    const QVector<int>& source = editor.diffSourceBlocks();
    bool everyBarHasLine = true;
    int number = 0;
    for (QTextBlock block = doc->begin(); block.isValid(); block = block.next(), ++number) {
        if (number >= marks.size() || marks[number] == diff::Mark::Same) continue;
        const int ir = number < source.size() ? source[number] : -1;
        if (ir < 0 || ir >= text.blocks.size()) continue;
        bool any = false;
        for (int line : plain)
            any = any || (line >= text.blocks[ir].first &&
                          line < text.blocks[ir].first + text.blocks[ir].count);
        everyBarHasLine = everyBarHasLine && any;
    }
    ZT_TRUE(what + ": и под каждой полоской есть изменённая строка", everyBarHasLine);
}

NoteEditor* openHistory(NoteEditor& editor, const QString& path) {
    editor.setStoreRoot(g_root);
    editor.openFile(path);
    if (!editor.enterHistory()) return nullptr;
    return &editor;
}

void checkBasics() {
    const QString path = makeNoteWithHistory(
        QStringLiteral("01dddddddddd01"),
        note("# Заголовок\n\nПервый абзац.\n\nВторой абзац.\n\n- пункт\n- ещё пункт\n", "a"),
        note("# Заголовок\n\nПервый абзац поправленный.\n\n- пункт\n- ещё пункт\n"
             "- третий пункт\n", "b"));

    NoteEditor editor;
    ZT_TRUE("вошли в историю", openHistory(editor, path) != nullptr);
    ZT_TRUE("сравнение нашло изменения: " + num(editor.diffChangedLines()),
            editor.diffChangedLines() > 0);
    checkViewsAgree(editor, "простой случай");

    // ЗАГЛУШКА. Второй абзац исчез — на его месте обязана быть строка,
    // говорящая, сколько текста пропало.
    editor.setDiffPlainView(false);
    bool foundGap = false;
    for (QTextBlock block = editor.document()->begin(); block.isValid(); block = block.next())
        if (block.text().startsWith(QStringLiteral("удалено:"))) foundGap = true;
    ZT_TRUE("вспомогательная строка про удалённый абзац есть", foundGap);

    // ХОДЬБА ПО ИЗМЕНЕНИЯМ. Первый шаг обязан привести на изменённый блок,
    // а круг — вернуть на то же место.
    QTextCursor top(editor.document());
    top.setPosition(0);
    editor.setTextCursor(top);
    ZT_TRUE("шаг к изменению удался", editor.diffStep(true));
    const int first = editor.textCursor().blockNumber();
    ZT_TRUE("и он привёл на изменённое место",
            first < editor.diffMarks().size() &&
                editor.diffMarks()[first] != diff::Mark::Same);
    int steps = 0;
    while (steps < 20) {
        editor.diffStep(true);
        ++steps;
        if (editor.textCursor().blockNumber() == first) break;
    }
    ZT_TRUE("ходьба идёт по кругу: шагов " + num(steps), steps < 20);

    editor.leaveHistory();
}

// СМЕНА ВИДА (полоски ↔ markdown) держит место — то же правило, что и у смены
// стороны: держимся за верхнюю кромку окна, а не за каретку.
void checkViewSwitchKeepsPlace() {
    QString before = QStringLiteral("# Длинная\n\n");
    for (int i = 0; i < 120; ++i) before += QStringLiteral("строка номер %1\n\n").arg(i);
    QString after = before;
    after.replace(QStringLiteral("строка номер 60\n"),
                  QStringLiteral("строка номер 60 поправленная\n"));
    const QString path = makeNoteWithHistory(
        QStringLiteral("01dddddddddd02"),
        note(before.toUtf8().constData(), "a"), note(after.toUtf8().constData(), "b"));

    NoteEditor editor;
    editor.resize(800, 600);
    editor.show();
    QApplication::processEvents();
    ZT_TRUE("вошли в историю", openHistory(editor, path) != nullptr);

    editor.verticalScrollBar()->setValue(editor.verticalScrollBar()->maximum() / 2);
    QApplication::processEvents();
    const int scrollBefore = editor.verticalScrollBar()->value();
    ZT_TRUE("прокрутили в середину: " + num(scrollBefore), scrollBefore > 100);

    // Строка сравнения, которая стоит у верхней кромки, — её и держим.
    const int lineBefore = editor.diffLineOnTop();
    ZT_TRUE("строка у кромки известна: " + num(lineBefore), lineBefore > 0);

    editor.setDiffPlainView(true);
    ZT_EQ("вид «как под капотом» открылся на той же строке", num(lineBefore),
          num(editor.diffLineOnTop()));
    ZT_TRUE("и не в начале документа", editor.verticalScrollBar()->value() > 0);

    editor.setDiffPlainView(false);
    ZT_EQ("и обратно на ту же строку", num(lineBefore), num(editor.diffLineOnTop()));
    editor.leaveHistory();
}

// СРАВНЕНИЕ РАЗВОРАЧИВАЕТСЯ ВМЕСТЕ СО СТОРОНОЙ. Правило владельца: зелёное —
// «есть здесь и нет у соперника», красное — наоборот, с какой бы стороны ни
// смотрели. Значит по Tab зелёное обязано стать красной заглушкой, и наоборот.
//
// Случаи нарочно простые, дописка в конец: стоит абзацам оказаться на одном
// месте — и дифф спарит их в «изменено», а никакого зелёного не будет вовсе
// (на этом первая редакция проверки и споткнулась).
void checkSidesFlipColours() {
    const auto markOfLine = [](NoteEditor& editor, const QString& needle) {
        const QTextDocument* doc = editor.document();
        int number = 0;
        for (QTextBlock b = doc->begin(); b.isValid(); b = b.next(), ++number)
            if (b.text().contains(needle) && number < editor.diffMarks().size())
                return editor.diffMarks()[number];
        return diff::Mark::Same;
    };
    const auto hasGap = [](NoteEditor& editor) {
        for (QTextBlock b = editor.document()->begin(); b.isValid(); b = b.next())
            if (b.text().startsWith(QStringLiteral("удалено:")) ||
                b.text().startsWith(QStringLiteral("добавлено:")))
                return true;
        return false;
    };

    // Дописали абзац: в слепке он зелёный, в базе его нет вовсе.
    {
        const QString path = makeNoteWithHistory(
            QStringLiteral("01dddddddddd10"),
            note("# Заголовок\n\nпервый\n", "a"),
            note("# Заголовок\n\nпервый\n\nдописанный\n", "b"));
        NoteEditor editor;
        editor.resize(800, 600);
        ZT_TRUE("вошли в историю", openHistory(editor, path) != nullptr);
        ZT_TRUE("дописанный абзац зелёный",
                markOfLine(editor, QStringLiteral("дописанный")) == diff::Mark::Added);
        ZT_TRUE("и заглушек на этой стороне нет", !hasGap(editor));

        editor.setDiffPeek(true);
        ZT_TRUE("на второй стороне его нет вовсе",
                !editor.document()->toPlainText().contains(QStringLiteral("дописанный")));
        ZT_TRUE("а на его месте красная заглушка", hasGap(editor));
        editor.setDiffPeek(false);
        ZT_TRUE("вернулись — снова зелёный",
                markOfLine(editor, QStringLiteral("дописанный")) == diff::Mark::Added);
        editor.leaveHistory();
    }

    // И наоборот: абзац стёрли. В слепке — красная заглушка, в базе — зелёный.
    {
        const QString path = makeNoteWithHistory(
            QStringLiteral("01dddddddddd11"),
            note("# Заголовок\n\nпервый\n\nстёртый\n", "a"),
            note("# Заголовок\n\nпервый\n", "b"));
        NoteEditor editor;
        editor.resize(800, 600);
        ZT_TRUE("вошли в историю", openHistory(editor, path) != nullptr);
        ZT_TRUE("на месте стёртого — заглушка", hasGap(editor));

        editor.setDiffPeek(true);
        ZT_TRUE("на второй стороне стёртый абзац зелёный",
                markOfLine(editor, QStringLiteral("стёртый")) == diff::Mark::Added);
        ZT_TRUE("и заглушек там нет", !hasGap(editor));
        editor.leaveHistory();
    }
}

// Удержание Alt показывает ВТОРУЮ СТОРОНУ: текст, которого в слепке нет.
void checkAltShowsOtherSide() {
    const QString path = makeNoteWithHistory(
        QStringLiteral("01dddddddddd03"),
        note("# Заголовок\n\nбыло вот так\n", "a"),
        note("# Заголовок\n\nстало иначе\n", "b"));
    NoteEditor editor;
    ZT_TRUE("вошли в историю", openHistory(editor, path) != nullptr);

    const QString shown = editor.document()->toPlainText();
    ZT_TRUE("показан слепок", shown.contains(QStringLiteral("стало иначе")));
    ZT_TRUE("и в нём нет прежнего текста", !shown.contains(QStringLiteral("было вот так")));

    editor.setDiffPeek(true);
    const QString peeked = editor.document()->toPlainText();
    ZT_TRUE("с зажатым Alt виден текст базы", peeked.contains(QStringLiteral("было вот так")));
    ZT_TRUE("а нового текста нет", !peeked.contains(QStringLiteral("стало иначе")));

    editor.setDiffPeek(false);
    ZT_EQ("отпустили — вернулся слепок", shown.toStdString(),
          editor.document()->toPlainText().toStdString());
    editor.leaveHistory();
}

// Клавиши доходят до редактора. Проверяется именно ПРОВОДКА: вызвать
// setDiffPeek напрямую умеет и набор, а вот дойдёт ли до неё зажатый Alt —
// вопрос обработчика событий, и отвечать на него надо событием.
void checkKeysAreWired() {
    const QString path = makeNoteWithHistory(
        QStringLiteral("01dddddddddd05"),
        note("# Заголовок\n\nбыло вот так\n", "a"),
        note("# Заголовок\n\nстало иначе\n", "b"));

    // РЕДАКТОР В ОКНЕ С СОСЕДОМ, а не сам по себе. Это не декорация: Qt отдаёт
    // Tab на переход фокуса, когда поле не редактируемое, и у одинокого
    // редактора фокусу уходить некуда — Tab доходит до нас «сам собой». В живом
    // окне рядом дерево и список, и там Tab уезжал к ним. Проверка, не имевшая
    // соседа, была пустышкой и этого не видела.
    QWidget window;
    auto* layout = new QVBoxLayout(&window);
    auto* neighbour = new QListWidget(&window);
    neighbour->addItem(QStringLiteral("сосед, которому достался бы фокус"));
    auto* editorPtr = new NoteEditor(&window);
    layout->addWidget(neighbour);
    layout->addWidget(editorPtr);
    window.resize(800, 600);
    window.show();
    // ОКНО ОБЯЗАНО СТАТЬ АКТИВНЫМ. Ярлыки окна (QShortcut) срабатывают только в
    // активном окне; под голым Xvfb, где нет оконного менеджера, окно само
    // активным не становится, и проверка краснела бы там, где всё в порядке.
    window.activateWindow();
    window.raise();
    (void)QTest::qWaitForWindowActive(&window, 1000);
    QApplication::processEvents();
    NoteEditor& editor = *editorPtr;
    installHistoryShortcuts(&window, editor);   // ровно то, что делает окно
    editor.setFocus();
    ZT_TRUE("вошли в историю", openHistory(editor, path) != nullptr);
    ZT_TRUE("фокус в редакторе", window.focusWidget() == &editor);

    // TAB МЕНЯЕТ СТОРОНУ СРАВНЕНИЯ (решение владельца). Alt убран совсем: на
    // linux его забирает оконный менеджер. Клавиши шлём ОКНУ, а не редактору:
    // ярлык окна ловится до того, как событие дойдёт до виджета, и проверять
    // надо именно этот путь.
    QTest::keyClick(&window, Qt::Key_Tab);
    ZT_TRUE("Tab показал вторую сторону", editor.diffPeek());
    ZT_TRUE("и в поле текст базы",
            editor.document()->toPlainText().contains(QStringLiteral("было вот так")));
    ZT_TRUE("и не увёл фокус к соседу", window.focusWidget() == &editor);

    QTest::keyClick(&window, Qt::Key_Tab);
    ZT_TRUE("Tab вернул слепок", !editor.diffPeek());
    ZT_TRUE("и текст слепка на месте",
            editor.document()->toPlainText().contains(QStringLiteral("стало иначе")));

    // И ТО ЖЕ САМОЕ, КОГДА ФОКУС НЕ В ТЕКСТЕ. Владелец: «встаёшь на слепок в
    // списке справа — F4 не работает». Отдаём фокус соседу и стучим снова.
    neighbour->setFocus();
    ZT_TRUE("фокус у соседа", window.focusWidget() == neighbour);
    QTest::keyClick(&window, Qt::Key_Tab);
    ZT_TRUE("Tab работает и с фокусом в чужом виджете", editor.diffPeek());
    QTest::keyClick(&window, Qt::Key_Tab);
    ZT_TRUE("и обратно", !editor.diffPeek());

    // МЕНЯТЬ ДОКУМЕНТ ИЗ ОБРАБОТЧИКА СОБЫТИЯ — опасное место: старый документ
    // умирает, пока Qt ещё разбирается с событием. Владелец на этом получил
    // падение (тогда — на Alt). Стучим часто и с прокруткой событий между.
    for (int i = 0; i < 12; ++i) {
        QTest::keyClick(&window, Qt::Key_Tab);
        QApplication::processEvents();
    }
    ZT_TRUE("двенадцать переключений подряд программу не уронили", true);

    // F4 — из конфига, поэтому нажимаем не «F4», а то, что там записано. И
    // тоже с фокусом у соседа: это и была жалоба.
    const QKeySequence next(appearance().diffNextKey);
    ZT_TRUE("сочетание для ходьбы по изменениям задано", next.count() > 0);
    QTextCursor top(editor.document());
    top.setPosition(0);
    editor.setTextCursor(top);
    neighbour->setFocus();
    QTest::keyClick(&window, Qt::Key(next[0].key()), next[0].keyboardModifiers());
    const int at = editor.textCursor().blockNumber();
    ZT_TRUE("шаг по изменениям сработал клавишей и без фокуса в тексте",
            at < editor.diffMarks().size() && editor.diffMarks()[at] != diff::Mark::Same);
    editor.leaveHistory();
}

// F4 ставит изменение в ЗОЛОТОЕ СЕЧЕНИЕ окна, а не у нижней кромки.
//
// Владелец: «скроллится до самой ранней позиции, откуда видны изменения —
// цветная полоска в самом низу». Так и было: ensureCursorVisible прокручивает
// МИНИМАЛЬНО. Теперь изменение встаёт примерно на 38% высоты сверху.
void checkStepLandsInGolden() {
    QString before = QStringLiteral("# Длинная\n\n");
    for (int i = 0; i < 200; ++i) before += QStringLiteral("строка номер %1\n\n").arg(i);
    QString after = before;
    after.replace(QStringLiteral("строка номер 150\n"),
                  QStringLiteral("строка номер 150 поправленная\n"));
    const QString path = makeNoteWithHistory(
        QStringLiteral("01dddddddddd06"),
        note(before.toUtf8().constData(), "a"), note(after.toUtf8().constData(), "b"));

    NoteEditor editor;
    editor.resize(800, 600);
    editor.show();
    QApplication::processEvents();
    ZT_TRUE("вошли в историю", openHistory(editor, path) != nullptr);

    QTextCursor top(editor.document());
    top.setPosition(0);
    editor.setTextCursor(top);
    editor.verticalScrollBar()->setValue(0);
    ZT_TRUE("шаг к изменению удался", editor.diffStep(true));

    const QTextBlock block = editor.textCursor().block();
    const qreal y = editor.document()->documentLayout()->blockBoundingRect(block).top() -
                    editor.verticalScrollBar()->value();
    const qreal height = editor.viewport()->height();
    ZT_TRUE("изменение оказалось не у кромки, а около золотого сечения: " +
                num(int(y)) + " из " + num(int(height)),
            height > 0 && y > height * 0.2 && y < height * 0.6);
}

// Переход к ДРУГОМУ слепку держит место примерно там же. Точного места в чужом
// тексте нет, а начинать каждый слепок с начала — неудобно (просьба владельца).
void checkSnapshotSwitchKeepsPlace() {
    QString first = QStringLiteral("# Длинная\n\n");
    for (int i = 0; i < 150; ++i) first += QStringLiteral("строка номер %1\n\n").arg(i);
    QString second = first;
    second.replace(QStringLiteral("строка номер 80\n"),
                   QStringLiteral("строка номер 80 поправленная\n"));
    const QString path = makeNoteWithHistory(
        QStringLiteral("01dddddddddd08"),
        note(first.toUtf8().constData(), "a"), note(second.toUtf8().constData(), "b"));

    NoteEditor editor;
    editor.resize(800, 600);
    editor.show();
    QApplication::processEvents();
    ZT_TRUE("вошли в историю", openHistory(editor, path) != nullptr);

    QTextCursor top(editor.document());
    top.setPosition(0);
    editor.setTextCursor(top);
    ZT_TRUE("встали на изменение", editor.diffStep(true));
    const int scrollBefore = editor.verticalScrollBar()->value();
    ZT_TRUE("и это не начало: прокрутка " + num(scrollBefore), scrollBefore > 0);

    // Шагаем к предыдущей записи — как щелчок по таймлайну.
    ZT_TRUE("перешли к другому слепку", editor.enterHistory(0));
    const int scrollAfter = editor.verticalScrollBar()->value();
    ZT_TRUE("вид остался примерно там же: было " + num(scrollBefore) + ", стало " +
                num(scrollAfter),
            scrollAfter > scrollBefore / 2);
    editor.leaveHistory();
}

// ПОКАЗЫВАЕТСЯ КОПИЯ, А СЛЕПОК ЛЕЖИТ НЕТРОНУТЫМ.
//
// Владелец нашёл подпись «удалено: 2 строки» посреди своей заметки: тогда
// показывался сам слепок, в который вставлялись блоки-заглушки, и запись при
// выходе унесла их на диск. Теперь показывается ИЛЛЮСТРИРОВАННАЯ КОПИЯ, а
// слепок — отдельный объект: что видно на экране и что программа может
// записать, это разные вещи.
void checkShownIsACopy() {
    const QString path = makeNoteWithHistory(
        QStringLiteral("01dddddddddd12"),
        note("# Заголовок\n\nпервый\n\nвторой\n", "a"),
        note("# Заголовок\n\nпервый\n", "b"));
    NoteEditor editor;
    editor.resize(800, 600);
    ZT_TRUE("вошли в историю", openHistory(editor, path) != nullptr);

    ZT_TRUE("человек видит вспомогательную строку",
            editor.document()->toPlainText().contains(QStringLiteral("удалено:")));

    // А сам слепок её не содержит — и восстановление берёт именно его.
    const std::string snapshot = serialize(editor.shownSnapshotIr());
    ZT_TRUE("в слепке вспомогательной строки нет",
            snapshot.find("удалено:") == std::string::npos);
    ZT_TRUE("а его текст на месте", snapshot.find("первый") != std::string::npos);

    // И восстановление кладёт в заметку слепок, а не показанное.
    editor.restoreShownSnapshot();
    const QString live = editor.document()->toPlainText();
    ZT_TRUE("после восстановления в заметке нет вспомогательных строк",
            !live.contains(QStringLiteral("удалено:")));
    ZT_TRUE("а текст слепка есть", live.contains(QStringLiteral("первый")));
}

// TAB (СМЕНА СТОРОНЫ) ДЕРЖИТ МЕСТО — ТО, ЧТО НА ЭКРАНЕ.
//
// Проверки на это НЕ БЫЛО: она была написана на смену ВИДА, а когда Tab отдали
// стороне, никто её не переписал. И вторая, важнее: держаться надо за верхнюю
// кромку окна, а не за каретку. Читая историю, человек крутит колесо — каретка
// остаётся там, где её оставили, чаще всего в начале документа. Владелец увидел
// это как «Tab убегает, иногда вообще на начало», а прежняя проверка ставила
// каретку сама и потому ничего не замечала.
void checkSideSwitchKeepsPlace() {
    QString before = QStringLiteral("# Длинная\n\n");
    for (int i = 0; i < 150; ++i) before += QStringLiteral("строка номер %1\n\n").arg(i);
    QString after = before;
    after.replace(QStringLiteral("строка номер 70\n"),
                  QStringLiteral("строка номер 70 поправленная\n"));
    const QString path = makeNoteWithHistory(
        QStringLiteral("01dddddddddd13"),
        note(before.toUtf8().constData(), "a"), note(after.toUtf8().constData(), "b"));

    NoteEditor editor;
    editor.resize(800, 600);
    editor.show();
    QApplication::processEvents();
    ZT_TRUE("вошли в историю", openHistory(editor, path) != nullptr);

    // КРУТИМ КОЛЕСО, А НЕ СТАВИМ КАРЕТКУ — как человек.
    QTextCursor top(editor.document());
    top.setPosition(0);
    editor.setTextCursor(top);
    editor.verticalScrollBar()->setValue(editor.verticalScrollBar()->maximum() / 2);
    QApplication::processEvents();
    const int scrollBefore = editor.verticalScrollBar()->value();
    ZT_TRUE("прокрутили в середину: " + num(scrollBefore), scrollBefore > 100);
    ZT_TRUE("а каретка осталась в начале — это и есть случай владельца",
            editor.textCursor().position() == 0);

    editor.setDiffPeek(true);
    const int scrollPeek = editor.verticalScrollBar()->value();
    ZT_TRUE("вторая сторона показана на том же месте: было " + num(scrollBefore) +
                ", стало " + num(scrollPeek),
            qAbs(scrollPeek - scrollBefore) <= 40);

    editor.setDiffPeek(false);
    const int scrollBack = editor.verticalScrollBar()->value();
    ZT_TRUE("и обратно на то же место: " + num(scrollBack),
            qAbs(scrollBack - scrollBefore) <= 40);
    editor.leaveHistory();
}

// Смена облика (масштаб) в режиме истории не стирает слепок. Цепочка отмены
// здесь своя и пустая, и общий путь пересборки дал бы чистый лист.
void checkZoomKeepsSnapshot() {
    const QString path = makeNoteWithHistory(
        QStringLiteral("01dddddddddd07"),
        note("# Заголовок\n\nбыло вот так\n", "a"),
        note("# Заголовок\n\nстало иначе\n", "b"));
    NoteEditor editor;
    editor.resize(800, 600);
    ZT_TRUE("вошли в историю", openHistory(editor, path) != nullptr);
    ZT_TRUE("слепок показан",
            editor.document()->toPlainText().contains(QStringLiteral("стало иначе")));
    editor.applyZoom(1.4);
    ZT_TRUE("после смены масштаба слепок на месте, а не чистый лист",
            editor.document()->toPlainText().contains(QStringLiteral("стало иначе")));
    ZT_TRUE("и полоски не потерялись", !editor.diffMarks().isEmpty());
    editor.applyZoom(1.0);
    editor.leaveHistory();
}

// Переключатель базы и вправду меняет сравнение: со свежей версией у последней
// записи разницы нет вовсе, а с предыдущей — есть.
void checkBaseSwitch() {
    const QString path = makeNoteWithHistory(
        QStringLiteral("01dddddddddd04"),
        note("# Заголовок\n\nстарое\n", "a"),
        note("# Заголовок\n\nновое\n", "b"));
    NoteEditor editor;
    ZT_TRUE("вошли в историю", openHistory(editor, path) != nullptr);
    ZT_TRUE("с предыдущей записью разница есть: " + num(editor.diffChangedLines()),
            editor.diffChangedLines() > 0);

    editor.setDiffFromFresh(true);
    ZT_EQ("со свежей версией у последнего слепка разницы нет", num(0),
          num(editor.diffChangedLines()));
    editor.setDiffFromFresh(false);
    ZT_TRUE("вернули базу — вернулась и разница", editor.diffChangedLines() > 0);
    editor.leaveHistory();
}

// Тот же круг на живых заметках: берём файл корпуса, режем в нём каждый пятый
// абзац и правим каждый третий — и сверяем виды.
void checkCorpus() {
    if (g_corpus.isEmpty()) {
        std::fprintf(stderr, "ПРОПУЩЕНО: корпус не задан — сверка видов только на придуманных "
                             "случаях\n");
        return;
    }
    // По всему дереву: заметки владельца лежат по подкаталогам, и брать только
    // верхний уровень значило бы проверить два файла из трёхсот.
    QStringList files;
    QDirIterator walk(g_corpus, {QStringLiteral("*.md")}, QDir::Files,
                      QDirIterator::Subdirectories);
    while (walk.hasNext()) files << walk.next();
    files.sort();

    int done = 0;
    for (const QString& name : files) {
        if (done >= 12) break;   // дюжины хватает: случаи повторяются
        QFile file(name);
        if (!file.open(QIODevice::ReadOnly)) continue;
        const QByteArray text = file.readAll();
        if (text.size() < 200) continue;

        QList<QByteArray> lines = text.split('\n');
        QByteArray changed;
        for (int i = 0; i < lines.size(); ++i) {
            if (i % 5 == 4) continue;                       // выбросили строку
            changed += lines[i];
            if (i % 3 == 0 && !lines[i].trimmed().isEmpty()) changed += " (правка)";
            if (i + 1 < lines.size()) changed += "\n";
        }
        const QString id = QStringLiteral("01ccccccccc%1").arg(done, 3, 10, QLatin1Char('0'));
        const QString path = makeNoteWithHistory(id, text, changed);

        NoteEditor editor;
        if (openHistory(editor, path) == nullptr) continue;
        checkViewsAgree(editor, QFileInfo(name).fileName().toStdString());
        editor.leaveHistory();
        ++done;
    }
    ZT_TRUE("на корпусе проверено файлов: " + num(done), done > 0);
}

// --- прибор ------------------------------------------------------------------
//
// Сколько стоит показать слепок и сколько — переключить сторону (Tab). Владелец:
// «на ficus tutorial прямо заметно подтормаживает, TAB около секунды».
//
//   taskset -c 0 ./diff_view_test --bench <файл.md>
void bench(const QString& file) {
    QFile source(file);
    if (!source.open(QIODevice::ReadOnly)) {
        std::printf("не прочитан: %s\n", file.toUtf8().constData());
        return;
    }
    const QByteArray text = source.readAll();
    // Правим ЖЁСТКО: выбрасываем каждую третью строку и правим каждую пятую.
    // Так и выглядит месяц работы над заметкой, а от мягкой правки место при
    // переключении держится само собой — на ней беду не поймать.
    QByteArray changed;
    int line = 0;
    for (const QByteArray& one : text.split('\n')) {
        ++line;
        if (line % 3 == 0) continue;
        changed += one;
        if (line % 5 == 0) changed += " (правка)";
        changed += "\n";
    }
    const QString path = makeNoteWithHistory(QStringLiteral("01bbbbbbbbbb01"), text, changed);

    NoteEditor editor;
    editor.resize(900, 700);
    editor.show();
    editor.setStoreRoot(g_root);
    QElapsedTimer open;
    open.start();
    editor.openFile(path);
    std::printf("открыть заметку: %lld мс\n", (long long)open.elapsed());
    QApplication::processEvents();

    QElapsedTimer clock;
    clock.start();
    const bool ok = editor.enterHistory();
    const qint64 enter = clock.elapsed();
    if (!ok) {
        std::printf("история не открылась\n");
        return;
    }
    std::printf("вход в историю: %lld мс\n", (long long)enter);

    for (int i = 0; i < 4; ++i) {
        clock.restart();
        editor.setDiffPeek(i % 2 == 0);
        std::printf("  сторона %s: %lld мс\n", i % 2 == 0 ? "база " : "слепок",
                    (long long)clock.elapsed());
    }
    for (int i = 0; i < 4; ++i) {
        clock.restart();
        editor.setDiffPlainView(i % 2 == 0);
        std::printf("  вид %s: %lld мс\n", i % 2 == 0 ? "markdown" : "полоски ",
                    (long long)clock.elapsed());
    }
    // ДЕРЖИТ ЛИ TAB МЕСТО на настоящей большой заметке.
    {
        QTextCursor top(editor.document());
        top.setPosition(0);
        editor.setTextCursor(top);
        // Уходим в середину: там и живёт настоящий случай.
        QTextCursor middle(editor.document());
        middle.setPosition(editor.document()->characterCount() / 2);
        editor.setTextCursor(middle);
        editor.ensureCursorVisible();
        const int was = editor.verticalScrollBar()->value();
        editor.setDiffPeek(true);
        const int peek = editor.verticalScrollBar()->value();
        editor.setDiffPeek(false);
        const int back = editor.verticalScrollBar()->value();
        std::printf("  место при Tab: было %d, на второй стороне %d, вернулись %d\n",
                    was, peek, back);
    }

    const auto& entries = editor.timeline().entries;
    if (entries.size() >= 2) {
        clock.restart();
        editor.enterHistory(0);
        std::printf("  переход к другому слепку: %lld мс\n", (long long)clock.elapsed());
    }
    editor.leaveHistory();
}

void writeShots(const QString& dir) {
    QDir().mkpath(dir);
    // Баннер режима: два переключателя парами, «подглядеть» и обе двери наружу.
    {
        HistoryBanner banner;
        banner.setSnapshot(1'700'000'000'000LL, journal::Kind::Save);
        banner.resize(1100, banner.sizeHint().height());
        banner.grab().save(QDir(dir).filePath(QStringLiteral("баннер-истории.png")));
    }
    const QString path = makeNoteWithHistory(
        QStringLiteral("01dddddddddd09"),
        note("# Список дел\n\nБыло записано так.\n\nЭтот абзац потом исчезнет.\n\n"
             "- купить хлеб\n- позвонить маме\n",
             "a"),
        note("# Список дел\n\nСтало записано иначе.\n\n- купить хлеб\n- позвонить маме\n"
             "- забрать посылку\n",
             "b"));
    NoteEditor editor;
    editor.resize(900, 600);
    if (openHistory(editor, path) == nullptr) return;
    editor.show();
    QApplication::processEvents();

    editor.setDiffPlainView(false);
    QApplication::processEvents();
    editor.grab().save(QDir(dir).filePath(QStringLiteral("дифф-полоски.png")));

    editor.setDiffPeek(true);
    QApplication::processEvents();
    editor.grab().save(QDir(dir).filePath(QStringLiteral("дифф-полоски-alt.png")));
    editor.setDiffPeek(false);

    editor.setDiffPlainView(true);
    QApplication::processEvents();
    editor.grab().save(QDir(dir).filePath(QStringLiteral("дифф-под-капотом.png")));
    editor.leaveHistory();
}

}  // namespace

static int ztRunSuite(int argc, char** argv) {
    QTemporaryDir tmp;
    if (!tmp.isValid()) {
        std::printf("не завёлся временный каталог\n");
        return 2;
    }
    g_root = tmp.path();
    if (argc > 2 && std::string(argv[1]) == "--bench") {
        bench(QString::fromLocal8Bit(argv[2]));
        return 0;
    }
    if (argc > 1) g_corpus = QString::fromLocal8Bit(argv[1]);

    checkBasics();
    checkViewSwitchKeepsPlace();
    checkSidesFlipColours();
    checkAltShowsOtherSide();
    checkStepLandsInGolden();
    checkKeysAreWired();
    checkSnapshotSwitchKeepsPlace();
    checkShownIsACopy();
    checkSideSwitchKeepsPlace();
    checkZoomKeepsSnapshot();
    checkBaseSwitch();
    checkCorpus();
    if (argc > 2) writeShots(QString::fromLocal8Bit(argv[2]));

    return zt::report("diff-view");
}

// Набор целиком одним TEST: тело не тронуто, argv ему собран здесь.
// Дробить на отдельные проверки — отдельная работа, по одному набору.
TEST(DiffView, All) {
    std::vector<QByteArray> ztArgs{QByteArrayLiteral("diff_view_test")};
    ztArgs.push_back((zt::TestData::corpus(QStringLiteral("corpus"))).toLocal8Bit());
    std::vector<char*> ztArgv;
    for (QByteArray& a : ztArgs) ztArgv.push_back(a.data());
    EXPECT_EQ(0, ztRunSuite(int(ztArgv.size()), ztArgv.data()));
}
