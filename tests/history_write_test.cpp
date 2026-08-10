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
    for (const QString& id : {QStringLiteral("01aaaaaaaaaaaa"), QStringLiteral("01bbbbbbbbbbbb")}) {
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
    checkNoEqualNeighbours();

    return zt::report("что история не пишет");
}
