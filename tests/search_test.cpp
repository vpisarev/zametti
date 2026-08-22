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
#include "find_bar.h"
#include "document.h"
#include "search.h"
#include "settings.h"
#include "doc_model.h"
#include "pieces.h"
#include "store_search.h"
#include "test_util.h"

#include <vector>
#include "testdata.h"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QTextBlock>
#include <QShortcut>
#include <QSignalSpy>
#include <QScrollBar>
#include <QTest>
#include <QTextEdit>
#include <QTextCursor>
#include <QTextDocument>
#include <QVBoxLayout>

#include <string>

using zametti::Piece;
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

zametti::ZDocument noteOf(const QString& text) {
    zametti::ZDocument doc;
    const QByteArray bytes = text.toUtf8();
    doc.loadMarkdown(std::string_view(bytes.constData(), size_t(bytes.size())));
    return doc;
}

int countIn(const QString& text, const QString& needle) {
    return int(noteOf(text).find(zametti::makeQuery(needle)).size());
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
    // Метаданные лежат в std::vector<Piece>::meta, а не в блоках, — поиск до них не
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

    // ОБЪЕКТЫ ИЩУТСЯ ПО ИСХОДНИКУ (решение владельца, сессия 5): таблица и
    // формула — объекты с U+FFFC в тексте блока, и без этого правила их не
    // видел бы ни поиск по заметке, ни поиск по хранилищу и истории (все
    // ходят ZDocument::find).
    ZT_TRUE("слово в ячейке таблицы находится",
            countIn(QStringLiteral("| a | сено |\n|---|---|\n| сено | b |\n"), "сено") == 2);
    ZT_TRUE("формула ищется по исходнику: «gamma» находит \\gamma",
            countIn(QStringLiteral("$$\\gamma x$$\n"), "gamma") == 1);
    ZT_TRUE("и хвостовая строка результата берётся из исходника объекта", [] {
        const zametti::ZDocument doc = noteOf(QStringLiteral("| a | сено |\n|---|---|\n| сено | b |\n"));
        const std::vector<zametti::Hit> hits = doc.find(zametti::makeQuery(QStringLiteral("сено")));
        if (hits.size() != 2 || !hits[1].inObject) return false;
        const zametti::HitLine line = doc.hitLine(hits[1]);
        return line.text.contains(QStringLiteral("| сено | b |")) && !line.text.contains(QLatin1Char('\n'));
    }());
}

void checkHitLine() {
    const zametti::ZDocument doc = noteOf(
        QStringLiteral("```\nочень длинная строка, в середине которой прячется "
                       "искомое слово, и дальше ещё столько же текста подряд\n```\n"));
    const auto hits = doc.find(zametti::makeQuery(QStringLiteral("искомое")));
    ZT_TRUE("совпадение в длинной строке найдено", hits.size() == 1);
    if (hits.empty()) return;
    const zametti::HitLine line = doc.hitLine(hits[0]);
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

// СЦЕНАРИЙ ВЛАДЕЛЬЦА: находим вхождения и удаляем их по очереди, чередуя
// Delete и F3 («Новая классная заметочка», запрос «од»).
//
// Два правила разом, и оба — общие для вёрстки и исходника: после правки поиск
// повторяется целиком («смещения изменились и количество изменилось»), а
// следующий шаг идёт к БЛИЖАЙШЕЙ находке от каретки, а не через одну.
void checkDeleteEveryMatchOneByOne() {
    const QString source = zt::TestData::file(QStringLiteral("search-steps-fixture.md"));
    if (source.isEmpty()) {
        std::printf("  (фикстуры search-steps-fixture.md нет — проверка пропущена)\n");
        return;
    }

    zametti::NoteEditor editor;
    editor.resize(1000, 800);
    editor.show();
    QTest::qWait(20);
    ZT_TRUE("заметка открылась", editor.openFile(source));
    QTest::qWait(60);

    const QString needle = QStringLiteral("од");
    const int found = editor.findMatches(needle, false);
    ZT_TRUE("вхождения нашлись (" + std::to_string(found) + ")", found > 1);

    QTextCursor home(editor.document());
    editor.setTextCursor(home);
    for (int done = 0; done < found; ++done) {
        editor.stepMatch(1);
        QTextCursor at = editor.textCursor();
        ZT_EQ("шаг встал на вхождение", needle.toStdString(), at.selectedText().toStdString());
        at.removeSelectedText();
        const int want = found - done - 1;
        for (int waited = 0; waited < 3000 && editor.matchCount() != want; waited += 50)
            QTest::qWait(50);
        ZT_EQ("после удаления вхождений стало меньше", std::to_string(want),
              std::to_string(editor.matchCount()));
    }
    ZT_TRUE("удалены все — в тексте вхождений не осталось",
            !editor.document()->toPlainText().contains(needle));
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

    // ПОДСВЕЧИВАЕТСЯ ТОЛЬКО ВИДИМОЕ (правило «цена — от показанного»): в
    // длинной заметке совпадений тысячи, а на экране — десятки, и подсветок
    // Qt считает ровно столько, сколько видно; при прокрутке перекладываются.
    {
        QString body = QStringLiteral("# Много\n\n");
        for (int i = 0; i < 1500; ++i) body += QStringLiteral("строка с сено номер %1\n\n").arg(i);
        const QString longPath = g_root + QStringLiteral("/00000000000010.md");
        note("00000000000010", "modified: 2025-01-01T00:00:00Z\n", body.toUtf8().constData());
        zametti::NoteEditor many;
        many.resize(700, 500);
        many.show();
        QTest::qWait(20);
        many.openFile(longPath);
        QTest::qWait(20);
        const int all = many.findMatches(QStringLiteral("сено"), false);
        ZT_TRUE("найдены все полторы тысячи", all == 1500);
        const int lit = int(many.extraSelections().size());
        ZT_TRUE("подсвечено только видимое: " + std::to_string(lit) + " из " + std::to_string(all),
                lit > 0 && lit < 200);
        // Прокрутили в конец — подсветка переехала за видом.
        many.verticalScrollBar()->setValue(many.verticalScrollBar()->maximum());
        QTest::qWait(10);
        bool lastLit = false;
        for (const QTextEdit::ExtraSelection& sel : many.extraSelections())
            if (sel.cursor.selectionStart() > many.document()->characterCount() - 200) lastLit = true;
        ZT_TRUE("после прокрутки подсвечены совпадения у конца", lastLit);
    }

    // Вхождения в таблице подсвечивает вид на сетке, а не ExtraSelection над
    // знаком объекта: подсветок Qt три (текст), а на сетке — две ячейки цветом
    // подсветки.
    ZT_TRUE("подсветок Qt — только текстовые (" + std::to_string(int(editor.extraSelections().size())) + ")",
            editor.extraSelections().size() == 3);
    {
        int table = -1;
        for (QTextBlock b = editor.document()->firstBlock(); b.isValid(); b = b.next())
            if (zametti::isTableObjectBlock(b)) table = b.blockNumber();
        ZT_TRUE("таблица — объект", table >= 0);
        if (table >= 0) {
            const QRectF area = editor.tableRect(table);
            const QImage shot = editor.grab().toImage();
            const QColor mark = zametti::settings().style().searchHighlight();
            const QPoint origin = editor.viewport()->mapTo(&editor, QPoint(0, 0));
            const int scroll = editor.verticalScrollBar()->value();
            int painted = 0;
            for (int px = int(area.left()); px < int(area.right()); ++px)
                for (int py = int(area.top()); py < int(area.bottom()); ++py) {
                    const int sx = px + origin.x();
                    const int sy = py - scroll + origin.y();
                    if (sx < 0 || sy < 0 || sx >= shot.width() || sy >= shot.height()) continue;
                    const QColor at = shot.pixelColor(sx, sy);
                    // Бледная подсветка (альфа 110) на белом — тот же оттенок.
                    if (qAbs(at.red() - mark.red()) < 40 && qAbs(at.blue() - mark.blue()) < 60 &&
                        at.green() < 235)
                        ++painted;
                }
            ZT_TRUE("вхождения в ячейках подсвечены на сетке (" + std::to_string(painted) + " точек)",
                    painted > 20);
        }
    }
    // F3 доводит до вхождения в таблице и выбирает её.
    editor.stepMatch(1);
    editor.stepMatch(1);
    editor.stepMatch(1);
    ZT_TRUE("четвёртое вхождение — в таблице", editor.currentMatch() == 3 &&
                                                zametti::isTableObjectBlock(editor.textCursor().block()));

    // «Заменить все» — один шаг отмены. Сравниваем markdown, а не toPlainText:
    // в тексте блока-таблицы стоит U+FFFC, и по нему проверка была бы пустышкой.
    const std::string before = markdownOf(blocksOf(*editor.document()));
    const int replaced =
        editor.replaceAllMatches(QStringLiteral("сено"), false, QStringLiteral("солома"));
    ZT_TRUE("заменены все вхождения", replaced == 5);
    ZT_TRUE("в заметке не осталось искомого",
            markdownOf(blocksOf(*editor.document())).find("сено") == std::string::npos);
    editor.undo();
    QTest::qWait(20);
    ZT_TRUE("одна отмена возвращает всё", markdownOf(blocksOf(*editor.document())) == before);

    // Замена ОДНОГО вхождения внутри таблицы переписывает исходник объекта.
    editor.findMatches(QStringLiteral("сено"), false);
    editor.goToMatch(3);
    ZT_TRUE("текущее — в таблице", zametti::isTableObjectBlock(editor.textCursor().block()));
    ZT_TRUE("замена одного вхождения в таблице удалась", editor.replaceCurrentMatch(QStringLiteral("солома")));
    ZT_TRUE("в таблице заменена одна ячейка",
            markdownOf(blocksOf(*editor.document())).find("| столбец | солома |") != std::string::npos &&
                markdownOf(blocksOf(*editor.document())).find("| сено | ещё |") != std::string::npos);
    editor.undo();
    QTest::qWait(20);
    ZT_TRUE("и она отменяется", markdownOf(blocksOf(*editor.document())) == before);

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

// Возврат в заметку показывает то место, где читали: средняя колонка
// переключает заметки часто, и каждый раз прыгать в начало — мучение.
void checkCaretMemory() {
    const QString first = g_root + QStringLiteral("/00000000000001.md");
    const QString second = g_root + QStringLiteral("/00000000000002.md");

    zametti::NoteEditor editor;
    editor.resize(700, 500);
    editor.show();
    QTest::qWait(20);
    editor.openFile(first);
    QTest::qWait(20);

    QTextCursor cursor = editor.textCursor();
    cursor.movePosition(QTextCursor::End);
    editor.setTextCursor(cursor);
    const int remembered = editor.textCursor().position();
    ZT_TRUE("каретка сдвинута с начала", remembered > 0);

    editor.openFile(second);
    QTest::qWait(20);
    ZT_TRUE("в новой заметке каретка в начале", editor.textCursor().position() == 0);

    editor.openFile(first);
    QTest::qWait(20);
    ZT_TRUE("вернулись — каретка на прежнем месте",
            editor.textCursor().position() == remembered);
}

// КЭШ ПОИСКА ПРИ СМЕНЕ ЗАМЕТКИ (сценарий владельца: «Ficus Tutorial», ищем,
// F3 на первое, уходим на «Карамазовых», возвращаемся — а он пишет „нет
// совпадений“, хотя стоит на первом»). Найденное живёт при заметке (ZNote):
// у чужой заметки его нет, у своей — то же с тем же номером текущего; правка
// делает его несвежим, и поиск идёт заново; заметка, перечитанная с диска, —
// новый объект, и кэш ушёл вместе со старым, а не подменён на «нет».
void checkSearchSurvivesSwitch() {
    const QString first = g_root + QStringLiteral("/00000000000001.md");    // «сено» ×1
    const QString second = g_root + QStringLiteral("/00000000000002.md");   // «сено» ×3
    const QString third = g_root + QStringLiteral("/00000000000003.md");    // нет

    zametti::NoteEditor editor;
    editor.resize(700, 500);
    editor.show();
    QTest::qWait(20);
    editor.openFile(second);
    QTest::qWait(20);
    ZT_EQ("три вхождения", std::string("3"),
          std::to_string(editor.findMatches(QStringLiteral("сено"), false)));
    editor.goToMatch(0);
    ZT_EQ("F3 — на первом", std::string("0"), std::to_string(editor.currentMatch()));

    editor.openFile(third);
    QTest::qWait(20);
    ZT_EQ("у чужой заметки найденного нет", std::string("0"), std::to_string(editor.matchCount()));
    ZT_EQ("и поиск в ней честно пуст", std::string("0"),
          std::to_string(editor.findMatches(QStringLiteral("сено"), false)));

    editor.openFile(second);
    QTest::qWait(20);
    ZT_EQ("вернулись — найденное при заметке", std::string("3"),
          std::to_string(editor.matchCount()));
    ZT_EQ("и текущее — то же первое", std::string("0"), std::to_string(editor.currentMatch()));
    // Тот же запрос по неправленной заметке — из кэша: текущее не сбрасывается.
    ZT_EQ("повторный запрос — те же три", std::string("3"),
          std::to_string(editor.findMatches(QStringLiteral("сено"), false)));
    ZT_EQ("текущее пережило повторный запрос", std::string("0"),
          std::to_string(editor.currentMatch()));

    // Правка делает найденное несвежим: ищется заново, текущего нет.
    QTextCursor end = editor.textCursor();
    end.movePosition(QTextCursor::End);
    editor.setTextCursor(end);
    QTest::keyClicks(&editor, QStringLiteral(" x"));
    QTest::qWait(20);
    ZT_EQ("после правки — заново, вхождений столько же", std::string("3"),
          std::to_string(editor.findMatches(QStringLiteral("сено"), false)));
    ZT_EQ("а текущего после пересчёта нет", std::string("-1"),
          std::to_string(editor.currentMatch()));
    editor.undo();
    QTest::qWait(20);

    // Заметка изменилась на диске, пока мы были в другой: вернулись — объект
    // новый, найденного при нём нет; поиск заново находит уже по новому тексту.
    editor.openFile(first);
    QTest::qWait(20);
    editor.findMatches(QStringLiteral("сено"), false);
    QTest::qWait(20);
    note(QStringLiteral("00000000000002"), QStringLiteral("modified: 2021-01-01T00:00:00Z\n"),
         QStringLiteral("# Сено\n\nтеперь тут только сено\n"));
    editor.openFile(second);
    QTest::qWait(20);
    ZT_EQ("перечитанная с диска — без старого найденного", std::string("0"),
          std::to_string(editor.matchCount()));
    ZT_EQ("новый поиск — по новому тексту (заголовок и строка)", std::string("2"),
          std::to_string(editor.findMatches(QStringLiteral("сено"), false)));
}

// История ЗАПРОСОВ (не заметок): что попадает в список, в каком порядке и
// сколько его хранится. Живёт между запусками, поэтому проверяется отдельно от
// самого поиска.
void checkQueryHistory() {
    const QStringList onlyHay{QStringLiteral("сено")};
    const QStringList strawFirst{QStringLiteral("солома"), QStringLiteral("сено")};
    const QStringList hayFirst{QStringLiteral("сено"), QStringLiteral("солома")};
    const QStringList hayAndNeedle{QStringLiteral("сено"), QStringLiteral("иголка")};

    zametti::FindBar bar;
    bar.open(zametti::FindBar::Mode::InNote, QStringLiteral("сено"));
    bar.rememberQuery();
    ZT_TRUE("запрос попал в историю", bar.history() == onlyHay);

    bar.open(zametti::FindBar::Mode::InNote, QStringLiteral("солома"));
    bar.rememberQuery();
    ZT_TRUE("свежий запрос сверху", bar.history() == strawFirst);

    // Повтор не плодит строк, а всплывает наверх.
    bar.open(zametti::FindBar::Mode::InNote, QStringLiteral("сено"));
    bar.rememberQuery();
    ZT_TRUE("повтор всплывает, а не дублируется", bar.history() == hayFirst);

    // Короткий запрос не исполняется поиском — и в историю не идёт.
    bar.open(zametti::FindBar::Mode::InNote, QStringLiteral("с"));
    bar.rememberQuery();
    ZT_TRUE("однобуквенный запрос не запоминается", bar.history().size() == 2);

    // Список из прошлого запуска: дубли и пустые строки отсеиваются.
    zametti::FindBar restored;
    restored.setHistory({QStringLiteral("сено"), QString(), QStringLiteral("сено"),
                         QStringLiteral("  "), QStringLiteral("иголка")});
    ZT_TRUE("при загрузке дубли и пустые отброшены", restored.history() == hayAndNeedle);

    // Потолок: сколько бы ни искали, помним настроенное число.
    const int limit = zametti::settings().ui().findHistoryLimit();
    zametti::FindBar many;
    for (int i = 0; i < limit + 10; ++i) {
        many.open(zametti::FindBar::Mode::InNote, QStringLiteral("запрос%1").arg(i));
        many.rememberQuery();
    }
    ZT_TRUE("список не растёт бесконечно", many.history().size() == limit);
    ZT_TRUE("самый свежий остался первым",
            many.history().first() == QStringLiteral("запрос%1").arg(limit + 9));

    // Ходить по истории надо в ОБЕ стороны: вверх — к старым, вниз — обратно
    // к новым и дальше к своему, недоискавшемуся запросу. На этом поймался:
    // сначала шаг считался поиском текущего текста по списку, и из повтора
    // вниз возвращало в ту же строку — казалось, что ходит только вверх.
    zametti::FindBar walk;
    walk.setHistory({QStringLiteral("первый"), QStringLiteral("второй"),
                     QStringLiteral("третий")});
    walk.open(zametti::FindBar::Mode::InNote, QStringLiteral("своё"));
    walk.stepHistory(-1);
    ZT_TRUE("вверх — самый свежий", walk.query() == QStringLiteral("первый"));
    walk.stepHistory(-1);
    ZT_TRUE("ещё вверх — следующий", walk.query() == QStringLiteral("второй"));
    walk.stepHistory(1);
    ZT_TRUE("вниз возвращает к свежему", walk.query() == QStringLiteral("первый"));
    walk.stepHistory(1);
    ZT_TRUE("ниже истории — свой недонабранный запрос",
            walk.query() == QStringLiteral("своё"));
    walk.stepHistory(1);
    ZT_TRUE("ниже своего запроса ничего нет", walk.query() == QStringLiteral("своё"));
    walk.stepHistory(-1);
    walk.stepHistory(-1);
    walk.stepHistory(-1);
    walk.stepHistory(-1);
    ZT_TRUE("выше самого старого не уходим", walk.query() == QStringLiteral("третий"));
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

static int ztRunSuite(int argc, char** argv) {
    (void)argc;
    (void)argv;
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
    checkDeleteEveryMatchOneByOne();
    checkCaretMemory();
    checkSearchSurvivesSwitch();
    checkQueryHistory();
    checkShortcutsReachWindow();

    QDir(g_root).removeRecursively();
    return zt::report("поиск");
}

// Набор целиком одним TEST: тело не тронуто, argv ему собран здесь.
// Дробить на отдельные проверки — отдельная работа, по одному набору.
TEST(Search, All) {
    std::vector<QByteArray> ztArgs{QByteArrayLiteral("search_test")};
    std::vector<char*> ztArgv;
    for (QByteArray& a : ztArgs) ztArgv.push_back(a.data());
    EXPECT_EQ(0, ztRunSuite(int(ztArgv.size()), ztArgv.data()));
}
