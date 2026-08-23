// Когда история ОБЯЗАНА молчать.
//
// Правило владельца: в журнале не должно быть двух одинаковых слепков подряд.
// Набрали «abcd», четыре раза нажали Backspace — заметка вернулась к себе
// прежней, и записывать нечего: ни файл, ни журнал трогать не за чем.
//
// Проверяется не «одинаковые ли байты» (это очевидно), а то, что программа
// доходит до этого сравнения ДО записи. Документ-то помечен изменённым:
// Qt считает изменением каждое нажатие, и отличить «вернулось как было» от
// «поправлено» можно только сравнив то, что получилось, с тем, что лежит.
//
// Сравнение идёт с ОТПЕЧАТКОМ последней записи, который редактор держит в
// памяти, — файл ради этого не читается.

#include "document_saver.h"
#include "zstorage.h"
#include "history_rules.h"
#include "editor_widget.h"
#include "history_rig.h"
#include "journal.h"
#include "times.h"

#include "test_util.h"
#include "testdata.h"

#include <QApplication>
#include <QKeyEvent>
#include <QDir>
#include <QFile>
#include <QDateTime>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTextCursor>
#include <QTextDocument>

#include <string>
#include <vector>

namespace {

QString g_root;

QByteArray fileBytes(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return f.readAll();
}

QString makeNote(const QString& id, const std::string& body) {
    QDir().mkpath(g_root + QStringLiteral("/.zametti"));
    QDir().mkpath(g_root + QStringLiteral("/history"));
    const QString path = g_root + QLatin1Char('/') + id + QStringLiteral(".md");
    QFile file(path);
    if (file.open(QIODevice::WriteOnly)) {
        // Время в НОВОМ виде — ISO-8601 с офсетом (этап 15). Со старой меткой
        // («…Z») первое же сохранение переписало бы её ленивой миграцией, и в
        // журнале появилась бы лишняя запись: опорная с прежним видом шапки и
        // следующая с новым. Это законное поведение миграции, и проверяется
        // оно отдельно (checkLazyTimeMigration ниже) — а здешние проверки про
        // отбор записей, и мешать им переезд формата незачем.
        const std::string text =
            "<!-- zametti\ncreated: 2020-01-01T00:00:00+03:00\n-->\n\n" + body;
        file.write(text.data(), qint64(text.size()));
    }
    return path;
}

int recordCount(const QString& id) {
    zametti::ZStorage history(g_root);
    zametti::journal::ZJournal journal;
    QString error;
    if (!history.readJournal(id, &journal, &error)) return -1;
    return int(journal.size());
}

// Сколько записей ГОВОРЯТ О СОДЕРЖИМОМ и не погашены — то есть сколько вешек
// человек видит в истории. Отличается от recordCount на записи гашения: возврат
// к уже записанному состоянию оставляет такую запись, и она обязана уехать в
// облако, но вешкой не является.
int contentRecordCount(const QString& id) {
    zametti::ZStorage history(g_root);
    zametti::journal::ZJournal journal;
    QString error;
    if (!history.readJournal(id, &journal, &error)) return -1;
    int count = 0;
    for (int i = 0; i < journal.size(); ++i)
        if (journal.at(i).statesContent() && !journal.isVoided(i)) ++count;
    return count;
}

// Все слепки журнала по порядку: ими и проверяется «нет двух одинаковых».
std::vector<QByteArray> snapshots(const QString& id) {
    std::vector<QByteArray> out;
    zametti::ZStorage history(g_root);
    zametti::journal::ZJournal journal;
    QString error;
    if (!history.readJournal(id, &journal, &error)) return out;
    for (int i = 0; i < journal.size(); ++i) {
        QByteArray blob;
        if (history.journalSnapshot(id, i, &blob, &error)) out.push_back(blob);
    }
    return out;
}

void checkNoOpEditWritesNothing() {
    const QString id = QStringLiteral("01aaaaaaaaaaaa");
    const QString path = makeNote(id, "# Заметка\n\nПервая строка.\n\nВторая строка.\n");

    zametti::NoteEditor editor;
    editor.setStoreRoot(g_root);
    editor.openFile(path);
    // Опорная запись: с чем заметку открыли.
    editor.save(false);
    const int before = recordCount(id);
    ZT_TRUE("журнал начат: записей " + std::to_string(before), before >= 1);

    const qint64 sizeBefore = QFileInfo(path).size();
    const QDateTime timeBefore = QFileInfo(path).lastModified();

    // Встаём в пустое место — в конец второй строки, не на картинку и не в код.
    QTextCursor caret(editor.document());
    caret.movePosition(QTextCursor::End);
    editor.setTextCursor(caret);

    // Набрали и стёрли.
    caret.insertText(QStringLiteral("abcd"));
    for (int i = 0; i < 4; ++i) caret.deletePreviousChar();
    ZT_TRUE("документ помечен изменённым (иначе проверять нечего)",
            editor.document()->isModified());

    editor.save(false);

    ZT_TRUE("новой записи в журнале не появилось: было " + std::to_string(before) +
                ", стало " + std::to_string(recordCount(id)),
            recordCount(id) == before);
    ZT_TRUE("файл заметки не переписан", QFileInfo(path).size() == sizeBefore &&
                                             QFileInfo(path).lastModified() == timeBefore);
}

// А настоящая правка записывается — иначе предыдущая проверка ничего не значит.
void checkRealEditWrites() {
    const QString id = QStringLiteral("01bbbbbbbbbbbb");
    const QString path = makeNote(id, "# Вторая\n\nТекст.\n");

    zametti::NoteEditor editor;
    editor.setStoreRoot(g_root);
    editor.openFile(path);
    editor.save(false);
    const int before = recordCount(id);

    QTextCursor caret(editor.document());
    caret.movePosition(QTextCursor::End);
    caret.insertText(QStringLiteral(" ещё"));
    editor.save(false);

    ZT_TRUE("настоящая правка записана: было " + std::to_string(before) + ", стало " +
                std::to_string(recordCount(id)),
            recordCount(id) == before + 1);
}

// Ни в одном журнале не должно быть двух одинаковых слепков подряд — что бы
// человек ни делал.
void checkNoEqualNeighbours() {
    for (const QString& id : {QStringLiteral("01aaaaaaaaaaaa"), QStringLiteral("01bbbbbbbbbbbb"),
                              QStringLiteral("01eeeeeeeeeeee")}) {
        const std::vector<QByteArray> all = snapshots(id);
        for (size_t i = 1; i < all.size(); ++i)
            ZT_TRUE("в журнале " + id.toStdString() + " записи " + std::to_string(i - 1) +
                        " и " + std::to_string(i) + " различны",
                    all[i] != all[i - 1]);
    }
}

// Отмена шагает СЛОВАМИ, а не буквами и не сессиями.
//
// Владелец: «буфера на 200 шагов как будто нет, слишком быстро мы попадаем в
// историю» и «сгруппировать undo по словам, как у Apple». До починки серия
// набора кончалась только тишиной в 700 мс, а сохранение её не разрывало: пока
// человек печатает ровно, весь набор ложился ОДНИМ шагом (замер: после 150
// правок глубина цепочки — единица). Два Ctrl+Z — и редактор уходил в историю,
// к чужому слепку.
//
// Печатаем по одной букве курсором РЕДАКТОРА: правка чужим курсором не двигает
// каретку, и для разбора границ это совсем другой случай (на этом я сперва и
// намерил ерунду).
// НАБОР — НАСТОЯЩИМИ НАЖАТИЯМИ, а не вставкой курсором.
//
// Прежняя редакция писала прямо в документ через QTextCursor, и это перестало
// быть набором: граница шага отмены живёт теперь во вводе (NoteEditor::insertTyped),
// а вставка мимо него — один сплошной шаг Qt. Проверка «отмена по словам» на
// такой подделке спрашивала не то, что заявлено в её имени.
void typeText(zametti::NoteEditor& editor, const QString& text) {
    for (const QChar ch : text) {
        QKeyEvent press(QEvent::KeyPress, Qt::Key_unknown, Qt::NoModifier, QString(ch));
        QApplication::sendEvent(&editor, &press);
    }
}

QString tailOf(const zametti::NoteEditor& editor, int chars) {
    const QString all = editor.document()->toPlainText();
    return all.right(qMin(chars, int(all.size())));
}

void checkUndoByWords() {
    const QString path = makeNote(QStringLiteral("01cccccccccccc"), "# Слова\n\n");
    zametti::NoteEditor editor;
    editor.setStoreRoot(g_root);
    editor.openFile(path);

    QTextCursor caret = editor.textCursor();
    caret.movePosition(QTextCursor::End);
    editor.setTextCursor(caret);
    typeText(editor, QStringLiteral("один два три четыре"));
    ZT_EQ("набралось", std::string("один два три четыре"), tailOf(editor, 19).toStdString());

    editor.undo();
    ZT_EQ("первая отмена убрала последнее слово", std::string("один два три "),
          tailOf(editor, 13).toStdString());
    editor.undo();
    ZT_EQ("вторая — предыдущее", std::string("один два "), tailOf(editor, 9).toStdString());
    editor.undo();
    ZT_EQ("третья — ещё одно", std::string("один "), tailOf(editor, 5).toStdString());
}

// Сколько слов помещается в цепочку. Владелец просил буфер на сотни правок;
// проверяем, что сотня слов отменяется, не сваливаясь в историю.
void checkUndoDepth() {
    const QString path = makeNote(QStringLiteral("01dddddddddddd"), "# Глубина\n\n");
    zametti::NoteEditor editor;
    editor.setStoreRoot(g_root);
    editor.openFile(path);

    QTextCursor caret = editor.textCursor();
    caret.movePosition(QTextCursor::End);
    editor.setTextCursor(caret);
    const int words = 120;
    for (int i = 0; i < words; ++i) typeText(editor, QStringLiteral("слово "));

    // Дно цепочки — просьба уйти в историю; контроллер туда и уходит.
    zt::HistoryRig rig(editor);
    int steps = 0;
    while (!rig.active() && steps <= words + 5) {
        editor.undo();
        if (rig.active()) break;
        ++steps;
    }
    rig.leave();
    ZT_TRUE("отменилось " + std::to_string(steps) + " слов из " + std::to_string(words) +
                ", а не свалились в историю",
            steps >= words);
}

// Мелкая правка ЗАМЕНЯЕТ прошлую запись, крупная — заводит новую.
//
// Правило владельца: не склеивать почти одинаковые копии, а убирать только что
// сделанную (если она свежая) и писать вместо неё свежую. Иначе за час правки
// одной заметки в таймлайне копится сотня вешек, сквозь которые не видно
// настоящих.
void checkSmallEditsReplace() {
    const QString id = QStringLiteral("01eeeeeeeeeeee");
    const QString path = makeNote(id, "# Замена\n\nНачало.\n");

    zametti::NoteEditor editor;
    editor.setStoreRoot(g_root);
    editor.openFile(path);
    editor.save(false);
    const int base = recordCount(id);
    ZT_TRUE("опорная запись есть", base >= 1);

    // Десять мелких правок подряд: каждая меньше порога.
    for (int i = 0; i < 10; ++i) {
        QTextCursor caret = editor.textCursor();
        caret.movePosition(QTextCursor::End);
        caret.insertText(QStringLiteral(" ещё%1").arg(i));
        editor.setTextCursor(caret);
        editor.save(false);
    }
    ZT_TRUE("десять мелких правок дали одну запись, а не десять: было " +
                std::to_string(base) + ", стало " + std::to_string(recordCount(id)),
            recordCount(id) == base + 1);

    // А крупная — своя вешка. Порог по умолчанию 100 знаков.
    QTextCursor caret = editor.textCursor();
    caret.movePosition(QTextCursor::End);
    // Латиница: кириллица в однобайтовом литерале не помещается (-Werror в Debug).
    caret.insertText(QString(300, QLatin1Char('x')));
    editor.setTextCursor(caret);
    editor.save(false);
    ZT_TRUE("крупная правка завела новую запись: стало " + std::to_string(recordCount(id)),
            recordCount(id) == base + 2);

    // И журнал остался читаемым: слепки достаются, последний равен файлу.
    const std::vector<QByteArray> all = snapshots(id);
    ZT_TRUE("все слепки читаются: " + std::to_string(all.size()) + " из " +
                std::to_string(recordCount(id)),
            int(all.size()) == recordCount(id));
    QFile file(path);
    ZT_TRUE("файл читается", file.open(QIODevice::ReadOnly));
    ZT_TRUE("последний слепок — это то, что лежит в файле",
            !all.empty() && all.back() == file.readAll());
}

// Сценарий владельца целиком: поработали, ЗАКРЫЛИ ПРОГРАММУ, открыли заново,
// набрали и стёрли набранное. Не появится ли снова пара одинаковых слепков?
//
// Закрытие программы здесь настоящее: второй заход идёт другим экземпляром
// редактора, то есть без единого байта в памяти от первого. Всё, на что он
// может опереться, — файл и журнал на диске.
void checkAcrossRestart() {
    const QString id = QStringLiteral("01ffffffffffff");
    const QString path = makeNote(id, "# Через перезапуск\n\nОснова.\n");

    {
        zametti::NoteEditor first;
        first.setStoreRoot(g_root);
        first.openFile(path);
        first.save(false);
        QTextCursor caret = first.textCursor();
        caret.movePosition(QTextCursor::End);
        first.setTextCursor(caret);
        caret.insertText(QStringLiteral(" работа"));
        first.setTextCursor(caret);
        first.save(false, true);   // так пишет выход из программы
    }

    const int afterFirst = recordCount(id);

    {
        zametti::NoteEditor second;
        second.setStoreRoot(g_root);
        second.openFile(path);
        QTextCursor caret = second.textCursor();
        caret.movePosition(QTextCursor::End);
        second.setTextCursor(caret);
        caret.insertText(QStringLiteral("abcd"));
        second.setTextCursor(caret);
        for (int i = 0; i < 4; ++i) {
            QTextCursor back = second.textCursor();
            back.deletePreviousChar();
            second.setTextCursor(back);
        }
        second.save(false, true);
    }

    ZT_TRUE("после набора и стирания записей не прибавилось: было " +
                std::to_string(afterFirst) + ", стало " + std::to_string(recordCount(id)),
            recordCount(id) == afterFirst);

    // И ни одной пары одинаковых слепков во всём журнале — не только соседних.
    const std::vector<QByteArray> all = snapshots(id);
    for (size_t i = 0; i < all.size(); ++i)
        for (size_t j = i + 1; j < all.size(); ++j)
            ZT_TRUE("слепки " + std::to_string(i) + " и " + std::to_string(j) +
                        " различаются не только штампом modified",
                    !zametti::sameApartFromModified(all[i], all[j]));
}

// НАБРАЛИ, СОХРАНИЛИ, ОТМЕНИЛИ, СОХРАНИЛИ — и в журнале не должно остаться
// двух одинаковых записей.
//
// Это случай владельца из «Пробуем Obsidian»: записи 9 и 11 там одинаковы, а
// между ними стоит 10, и проверка «новая не равна последней» его не ловила.
//
//   было: …, M0            набрали: …, M0, M0'      отменили: …, M0
//
// Самая старая из одинаковых остаётся, новая не пишется вовсе.
void checkUndoDoesNotDuplicate() {
    const QString id = QStringLiteral("01aabbccddeeff");
    const QString path = makeNote(id, "# Отмена\n\nОснова.\n");

    zametti::NoteEditor editor;
    editor.setStoreRoot(g_root);
    editor.openFile(path);
    editor.save(false);
    const int base = recordCount(id);
    ZT_TRUE("опорная запись есть", base >= 1);
    const QByteArray before = snapshots(id).back();

    // Правка крупная: иначе она заменила бы прошлую запись, и случай был бы
    // другой — нам нужна именно вторая запись в журнале.
    QTextCursor caret = editor.textCursor();
    caret.movePosition(QTextCursor::End);
    editor.setTextCursor(caret);
    caret.insertText(QString(300, QLatin1Char('x')));
    editor.setTextCursor(caret);
    editor.save(false);
    ZT_TRUE("правка встала отдельной записью: " + std::to_string(recordCount(id)),
            recordCount(id) == base + 1);

    // Отменяем её и сохраняем: заметка вернулась к тому, что уже есть в журнале.
    editor.undo();
    editor.save(false, true);

    // Вешек снова столько, сколько было: набранное и отменённое погашено.
    ZT_TRUE("после отмены вешек стало " + std::to_string(contentRecordCount(id)) + ", а ждали " +
                std::to_string(base),
            contentRecordCount(id) == base);
    // А в файле осталась запись гашения — ей и ехать в облако, чтобы «набрал и
    // отменил» доехало до других устройств, а не воскресло объединением.
    ZT_TRUE("гашение записано отдельной записью: " + std::to_string(recordCount(id)),
            recordCount(id) == base + 1);
    const std::vector<QByteArray> all = snapshots(id);
    ZT_TRUE("и последняя запись — та самая старая",
            !all.empty() && zametti::sameApartFromModified(all.back(), before));

    // Последний слепок обязан совпасть с файлом: иначе история врёт про то,
    // что лежит на диске.
    QFile file(path);
    ZT_TRUE("файл читается", file.open(QIODevice::ReadOnly));
    ZT_TRUE("последний слепок — это то, что в файле",
            !all.empty() && zametti::sameApartFromModified(all.back(), file.readAll()));
}

// --- ленивая миграция: оба триггера пер-заметочные ---------------------------
//
// Триггеров ровно два, и оба про ОДНУ заметку: первая запись в её журнал и
// первое чтение её истории. Просто открыть заметку и смотреть на неё — журнала
// не касается вовсе.

// Журнал с дубликатами, каким его писала программа до этапа 10: записи в обход
// правил отбора и шапка без версии содержимого.
void makeDirtyJournal(const QString& id, qint64 when) {
    zametti::ZStorage history(g_root);
    const QByteArray a = "<!-- zametti\ncreated: 2020-01-01T00:00:00Z\n"
                         "modified: 2020-01-01T00:00:01Z\n-->\n\n# Грязь\n\nодин\n";
    const QByteArray b = "<!-- zametti\ncreated: 2020-01-01T00:00:00Z\n"
                         "modified: 2020-01-01T00:00:02Z\n-->\n\n# Грязь\n\n"
                         "один два три четыре пять шесть семь восемь девять десять\n";
    const QByteArray a2 = "<!-- zametti\ncreated: 2020-01-01T00:00:00Z\n"
                          "modified: 2020-01-01T00:00:03Z\n-->\n\n# Грязь\n\nодин\n";
    QString error;
    history.appendToJournal(id, zametti::journal::NewRecord::save(a, zametti::journal::Stamp::at(when)), &error);
    history.appendToJournal(id, zametti::journal::NewRecord::save(b, zametti::journal::Stamp::at(when + 1000)), &error);
    history.appendToJournal(id, zametti::journal::NewRecord::save(a2, zametti::journal::Stamp::at(when + 2000)), &error);

    // Шапку — на старый лад, иначе чистить нечего: нынешний append заводит
    // журнал сразу чищеным.
    const QString path = history.journalPath(id);
    QFile file(path);
    ZT_TRUE("грязный журнал открыт", file.open(QIODevice::ReadOnly));
    QByteArray bytes = file.readAll();
    file.close();
    const QByteArray clean =
        zametti::journal::ZJournal::headerBytes(QString::fromLatin1(zametti::journal::kCleanVersion));
    bytes = zametti::journal::ZJournal::headerBytes(QString()) + bytes.mid(clean.size());
    QFile out(path);
    ZT_TRUE("грязный журнал переписан", out.open(QIODevice::WriteOnly | QIODevice::Truncate));
    out.write(bytes);
    out.close();
}

QString cleanVersionOf(const QString& id) {
    zametti::ZStorage history(g_root);
    zametti::journal::ZJournal journal;
    QString error;
    if (!history.readJournal(id, &journal, &error)) return QStringLiteral("не читается");
    return journal.cleanVersion();
}

// Триггер первый: первая запись в журнал.
void checkSaveMigrates() {
    const QString id = QStringLiteral("01gggggggggggg");
    const QString path = makeNote(id, "# Грязь\n\nодин\n");
    makeDirtyJournal(id, QDateTime::currentMSecsSinceEpoch() - 60 * 60 * 1000);
    ZT_EQ("журнал заведён старым (v0)", std::string(), cleanVersionOf(id).toStdString());
    ZT_TRUE("и дубликаты в нём есть", recordCount(id) == 3);

    zametti::NoteEditor editor;
    editor.setStoreRoot(g_root);
    editor.openFile(path);
    ZT_EQ("просмотр заметки журнал не тронул", std::string(),
          cleanVersionOf(id).toStdString());

    QTextCursor caret = editor.textCursor();
    caret.movePosition(QTextCursor::End);
    caret.insertText(QStringLiteral(" и ещё длинный дописанный кусок текста сверх того"));
    editor.setTextCursor(caret);
    editor.save(false);

    ZT_EQ("после первой же записи журнал чищен", std::string("0.1"),
          cleanVersionOf(id).toStdString());
    // Три записи с возвратом сходятся к одной, и к ней добавляется свежая.
    ZT_TRUE("дубликаты вычищены: записей " + std::to_string(recordCount(id)), recordCount(id) == 2);
}

// Триггер второй: первое чтение истории. Ctrl+Z, доехавший до дна цепочки,
// проваливается в историю — и журнал обязан быть чищен ДО первого шага.
void checkHistoryReadMigrates() {
    const QString id = QStringLiteral("01hhhhhhhhhhhh");
    const QString path = makeNote(id, "# Грязь\n\nодин\n");
    makeDirtyJournal(id, QDateTime::currentMSecsSinceEpoch() - 60 * 60 * 1000);

    zametti::NoteEditor editor;
    editor.setStoreRoot(g_root);
    editor.openFile(path);
    ZT_EQ("до входа в историю журнал не тронут", std::string(),
          cleanVersionOf(id).toStdString());

    zt::HistoryRig rig(editor);
    ZT_TRUE("вход в историю удался", rig.enter());
    ZT_EQ("журнал вычищен входом в историю", std::string("0.1"),
          cleanVersionOf(id).toStdString());
    ZT_TRUE("и таймлайн показывает уже чистую историю: записей " +
                std::to_string(rig.controller.timeline()->count()),
            rig.controller.timeline()->count() == 1);
    rig.leave();
}

// --- вход в историю ---------------------------------------------------------

// Вход = ещё одна точка сохранения. Грязный буфер рождает запись, чистый — нет.
void checkEnterSavesDirtyBuffer() {
    const QString id = QStringLiteral("01iiiiiiiiiiii");
    const QString path = makeNote(id, "# Вход\n\nОснова.\n");

    zametti::NoteEditor editor;
    editor.setStoreRoot(g_root);
    editor.openFile(path);
    editor.save(false);
    const int base = recordCount(id);
    ZT_TRUE("опорная запись есть", base >= 1);

    // Чистый буфер: вход записи не добавляет.
    zt::HistoryRig rig(editor);
    ZT_TRUE("вошли в историю", rig.enter());
    rig.leave();
    ZT_TRUE("с чистым буфером записи не появилось: было " + std::to_string(base) +
                ", стало " + std::to_string(recordCount(id)),
            recordCount(id) == base);

    // Грязный буфер: вход обязан его записать, иначе набранное не попало бы в
    // прошлое, за которым человек как раз и пошёл.
    QTextCursor caret = editor.textCursor();
    caret.movePosition(QTextCursor::End);
    caret.insertText(QStringLiteral(" длинная дописка, которой хватит на новую запись целиком"));
    editor.setTextCursor(caret);
    ZT_TRUE("вошли в историю со свежими правками", rig.enter());
    ZT_TRUE("вершина работы записана: было " + std::to_string(base) + ", стало " +
                std::to_string(recordCount(id)),
            recordCount(id) == base + 1);

    // И вершина таймлайна — настоящая головная запись, равная живому буферу:
    // «Вернуть» на ней честно отказывается.
    bool alreadyCurrent = false;
    rig.controller.restore(&alreadyCurrent);
    ZT_TRUE("на вершине восстанавливать нечего", alreadyCurrent);
    rig.leave();
}

// ИМЕНОВАННЫЙ ИНВАРИАНТ: режим истории живого буфера не трогает. Вышли — и
// Ctrl+Z продолжает отматывать до-исторические правки, а не начинает с чистого
// листа.
void checkHistoryLeavesUndoStackAlone() {
    const QString id = QStringLiteral("01jjjjjjjjjjjj");
    const QString path = makeNote(id, "# Стек\n\n");

    zametti::NoteEditor editor;
    editor.setStoreRoot(g_root);
    editor.openFile(path);
    QTextCursor caret = editor.textCursor();
    caret.movePosition(QTextCursor::End);
    editor.setTextCursor(caret);
    typeText(editor, QStringLiteral("один два три "));
    const int stepsBefore = editor.undoSteps();
    ZT_TRUE("шагов отмены набралось: " + std::to_string(stepsBefore), stepsBefore >= 3);

    zt::HistoryRig rig(editor);
    ZT_TRUE("вошли в историю", rig.enter());
    // Режим истории живёт вне редактора (сессия 7): цепочка отмены на месте и
    // в режиме — она НЕ ОБЯЗАНА пустеть, редактор ничего не подменяет. Стережём
    // это как инвариант: было столько же.
    ZT_TRUE("в режиме цепочка отмены не тронута: " + std::to_string(editor.undoSteps()),
            editor.undoSteps() == stepsBefore);
    rig.leave();

    ZT_TRUE("вернулись с той же цепочкой: было " + std::to_string(stepsBefore) + ", стало " +
                std::to_string(editor.undoSteps()),
            editor.undoSteps() == stepsBefore);
    editor.undo();
    ZT_EQ("и Ctrl+Z отменяет до-историческую правку", std::string("один два "),
          tailOf(editor, 9).toStdString());
}

// Каретка и выделение переживают заход в историю.
//
// Владелец: «ставим курсор не в начало заметки, жмём историю, жмём ещё раз —
// текст выделен от начала до курсора». Вход запоминал только позицию каретки, а
// второй конец выделения оставался чужим — и заметка возвращалась выделенной.
void checkHistoryKeepsCaretAndSelection() {
    const QString id = QStringLiteral("01kkkkkkkkkkkk");
    const QString path = makeNote(id, "# Каретка\n\nПервая строка.\n\nВторая строка.\n");

    zametti::NoteEditor editor;
    editor.setStoreRoot(g_root);
    editor.openFile(path);
    editor.save(false);

    // Каретка в середине, выделения НЕТ.
    QTextCursor caret = editor.textCursor();
    caret.movePosition(QTextCursor::End);
    caret.movePosition(QTextCursor::PreviousCharacter, QTextCursor::MoveAnchor, 5);
    editor.setTextCursor(caret);
    const int at = editor.textCursor().position();
    ZT_TRUE("каретка не в начале: " + std::to_string(at), at > 0);
    ZT_TRUE("и выделения нет", !editor.textCursor().hasSelection());

    zt::HistoryRig rig(editor);
    ZT_TRUE("вошли в историю", rig.enter());
    rig.leave();
    ZT_TRUE("вернулись без выделения", !editor.textCursor().hasSelection());
    ZT_TRUE("и каретка на месте: " + std::to_string(editor.textCursor().position()),
            editor.textCursor().position() == at);

    // А выделение, которое БЫЛО, возвращается целым — иначе первая проверка
    // проходила бы и при «выделение всегда снимаем».
    QTextCursor chosen = editor.textCursor();
    chosen.movePosition(QTextCursor::PreviousWord, QTextCursor::KeepAnchor);
    editor.setTextCursor(chosen);
    const int anchor = editor.textCursor().anchor();
    const int position = editor.textCursor().position();
    ZT_TRUE("выделение сделано", editor.textCursor().hasSelection());

    ZT_TRUE("снова вошли в историю", rig.enter());
    rig.leave();
    ZT_TRUE("выделение вернулось тем же",
            editor.textCursor().anchor() == anchor &&
                editor.textCursor().position() == position);
}

// ЗАГЛУШКА ДИФФА НЕ ИМЕЕТ ПРАВА ПОПАСТЬ В ФАЙЛ ЗАМЕТКИ.
//
// Владелец нашёл посреди своего «Ficus Tutorial» строку «удалено: 2 строки» —
// нашу подпись из режима истории. В поле в этот момент лежит документ СЛЕПКА со
// вставленными заглушками, и любая запись из этого состояния уносит их на диск.
void checkHistoryNeverWritesToFile() {
    const QString id = QStringLiteral("01mmmmmmmmmmmm");
    const QString path = makeNote(id, "# Заметка\n\nпервый\n\nвторой\n\nтретий\n");

    zametti::NoteEditor editor;
    editor.setStoreRoot(g_root);
    editor.openFile(path);
    editor.save(false);

    // ТРИ ЗАПИСИ, и встать надо на среднюю: у неё и заглушка есть (абзац
    // «второй» исчез), и содержимое отличается от файла. На последней записи
    // проверка была бы пустышкой — там слепок равен файлу, и запись ничего бы
    // не изменила даже без починки. На этом я и попался в первой редакции.
    const auto write = [&](const char* body) {
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return;
        file.write(QByteArray("<!-- zametti\ncreated: 2020-01-01T00:00:00Z\n-->\n\n") + body);
    };
    zametti::ZStorage history(g_root);
    QString error;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    // Опорная запись с исходным содержимым уже есть — её положило открытие
    // заметки. Дописываем только то, что после неё.
    // Времена ПОСЛЕ опорной записи: её время — время файла, то есть «сейчас».
    // Поставь я записи в прошлое — и слепок оказался бы старше своей базы, а
    // подпись сменилась бы на «добавлено» (на этом я и попался).
    history.appendToJournal(id, zametti::journal::NewRecord::save(QByteArray("<!-- zametti\ncreated: 2020-01-01T00:00:00Z\n-->\n\n"
                              "# Заметка\n\nпервый\n\nтретий\n"), zametti::journal::Stamp::at(now + 60'000)), &error);
    write("# Заметка\n\nпервый\n\nтретий\n\nчетвёртый\n");
    history.appendToJournal(id, zametti::journal::NewRecord::save(QByteArray("<!-- zametti\ncreated: 2020-01-01T00:00:00Z\n-->\n\n"
                              "# Заметка\n\nпервый\n\nтретий\n\nчетвёртый\n"), zametti::journal::Stamp::at(now + 120'000)), &error);

    editor.openFile(path);
    zt::HistoryRig rig(editor);
    ZT_TRUE("вошли в историю", rig.enter());
    ZT_TRUE("встали на среднюю запись", rig.enter(1));
    // В документе разности убранный абзац «второй» ВИДЕН (своим текстом) —
    // именно он не имеет права попасть в файл; и слепок отличается от файла —
    // иначе проверять нечего.
    ZT_TRUE("убранная строка в виде есть — иначе проверять нечего",
            rig.shownText().contains(QStringLiteral("второй")));
    ZT_TRUE("и слепок отличается от файла — иначе проверять тоже нечего",
            rig.controller.timeline()->snapshotBody().find("четвёртый") == std::string::npos);

    QFile before(path);
    ZT_TRUE("файл читается", before.open(QIODevice::ReadOnly));
    const QByteArray was = before.readAll();
    before.close();

    // ВОТ ОНА, ДВЕРЬ: так пишет выход из программы и уход из заметки — не
    // спрашивая признак «изменён».
    editor.save(false, true);
    editor.save(true);

    QFile after(path);
    ZT_TRUE("файл читается и после", after.open(QIODevice::ReadOnly));
    const QByteArray now2 = after.readAll();
    ZT_TRUE("файл заметки не тронут записью из режима истории", now2 == was);
    ZT_TRUE("и строки из документа разности в нём нет", !now2.contains("второй"));
    rig.leave();
}

// ЛЕНИВАЯ МИГРАЦИЯ ВРЕМЁН (этап 15) на живой заметке старого вида.
//
// Метка «…Z» переезжает в ISO-8601 с офсетом при ПЕРВОМ сохранении, момент при
// этом остаётся тем же. Заодно спрашиваем цену перееза: журнал растёт ровно на
// одну запись — ту, которой человек и правил заметку. Просто открыть заметку и
// ничего не трогать — не переписывает ни файла, ни истории.
void checkLazyTimeMigration() {
    const QString id = QStringLiteral("01ddeeff001122");
    const QString path = g_root + QLatin1Char('/') + id + QStringLiteral(".md");
    {
        QDir().mkpath(g_root + QStringLiteral("/.zametti"));
        QFile file(path);
        if (file.open(QIODevice::WriteOnly)) {
            const QByteArray text =
                "<!-- zametti\ncreated: 2019-03-14T09:26:53Z\n"
                "modified: 2019-03-14T09:26:53Z\n-->\n\n# Старая\n\nТекст.\n";
            file.write(text);
        }
    }
    const QByteArray was = fileBytes(path);

    zametti::NoteEditor editor;
    editor.setStoreRoot(g_root);
    editor.openFile(path);
    editor.save(false);
    ZT_TRUE("открытие и сохранение без правок файл не трогают", fileBytes(path) == was);
    const int base = recordCount(id);

    QTextCursor caret = editor.textCursor();
    caret.movePosition(QTextCursor::End);
    editor.setTextCursor(caret);
    caret.insertText(QStringLiteral(" дописано"));
    editor.setTextCursor(caret);
    editor.save(false);

    const QString written = QString::fromUtf8(fileBytes(path));
    const QRegularExpressionMatch created =
        QRegularExpression(QStringLiteral("created: ([^\n]+)")).match(written);
    ZT_TRUE("created на месте", created.hasMatch());
    ZT_TRUE("created переехал в вид с офсетом: " + created.captured(1).toStdString(),
            created.captured(1) != QStringLiteral("2019-03-14T09:26:53Z") &&
                (created.captured(1).contains(QLatin1Char('+')) ||
                 created.captured(1).contains(QLatin1Char('-'), Qt::CaseSensitive)));
    ZT_TRUE("и остался тем же моментом",
            zametti::store::parseNoteTime(created.captured(1)) ==
                QDateTime::fromString(QStringLiteral("2019-03-14T09:26:53Z"), Qt::ISODate));
    ZT_TRUE("переезд стоил ровно одной записи журнала — той, что и была правкой: " +
                std::to_string(recordCount(id) - base),
            recordCount(id) == base + 1);
}

}  // namespace

static int ztRunSuite(int argc, char** argv) {
    (void)argc;
    (void)argv;
    QTemporaryDir tmp;
    if (!tmp.isValid()) {
        std::printf("не завёлся временный каталог\n");
        return 2;
    }
    g_root = tmp.path();

    checkUndoByWords();
    checkUndoDepth();
    checkNoOpEditWritesNothing();
    checkRealEditWrites();
    checkSmallEditsReplace();
    checkUndoDoesNotDuplicate();
    checkLazyTimeMigration();
    checkAcrossRestart();
    checkNoEqualNeighbours();
    checkSaveMigrates();
    checkHistoryReadMigrates();
    checkEnterSavesDirtyBuffer();
    checkHistoryLeavesUndoStackAlone();
    checkHistoryKeepsCaretAndSelection();
    checkHistoryNeverWritesToFile();

    return zt::report("что история не пишет");
}

// Набор целиком одним TEST: тело не тронуто, argv ему собран здесь.
// Дробить на отдельные проверки — отдельная работа, по одному набору.
TEST(HistoryWrite, All) {
    std::vector<QByteArray> ztArgs{QByteArrayLiteral("history_write_test")};
    std::vector<char*> ztArgv;
    for (QByteArray& a : ztArgs) ztArgv.push_back(a.data());
    EXPECT_EQ(0, ztRunSuite(int(ztArgv.size()), ztArgv.data()));
}
