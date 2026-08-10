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
#include "editor_widget.h"
#include "journal.h"
#include "test_util.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTextCursor>
#include <QTextDocument>

#include <string>
#include <vector>

namespace {

QString g_root;

QString makeNote(const QString& id, const std::string& body) {
    QDir().mkpath(g_root + QStringLiteral("/.zametti"));
    QDir().mkpath(g_root + QStringLiteral("/history"));
    const QString path = g_root + QLatin1Char('/') + id + QStringLiteral(".md");
    QFile file(path);
    if (file.open(QIODevice::WriteOnly)) {
        const std::string text =
            "<!-- zametti\ncreated: 2020-01-01T00:00:00Z\n-->\n\n" + body;
        file.write(text.data(), qint64(text.size()));
    }
    return path;
}

int recordCount(const QString& id) {
    zametti::journal::History history(g_root);
    zametti::journal::Journal journal;
    QString error;
    if (!history.read(id, &journal, &error)) return -1;
    return int(journal.entries.size());
}

// Все слепки журнала по порядку: ими и проверяется «нет двух одинаковых».
std::vector<QByteArray> snapshots(const QString& id) {
    std::vector<QByteArray> out;
    zametti::journal::History history(g_root);
    zametti::journal::Journal journal;
    QString error;
    if (!history.read(id, &journal, &error)) return out;
    for (int i = 0; i < journal.entries.size(); ++i) {
        QByteArray blob;
        if (history.snapshotAt(id, i, &blob, &error)) out.push_back(blob);
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
void typeText(zametti::NoteEditor& editor, const QString& text) {
    for (const QChar ch : text) {
        QTextCursor caret = editor.textCursor();
        caret.insertText(QString(ch));
        editor.setTextCursor(caret);
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

    int steps = 0;
    while (!editor.inHistory() && steps <= words + 5) {
        editor.undo();
        if (editor.inHistory()) break;
        ++steps;
    }
    editor.leaveHistory();
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

    ZT_TRUE("после отмены записей стало " + std::to_string(recordCount(id)) + ", а ждали " +
                std::to_string(base),
            recordCount(id) == base);
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

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
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
    checkAcrossRestart();
    checkNoEqualNeighbours();

    return zt::report("что история не пишет");
}
