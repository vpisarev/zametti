// ШТАТНЫЙ СТЕК ОТМЕНЫ QTextDocument: чем именно мы его теряем.
//
// Довод, по которому стек когда-то выключили, звучал так: он
// хранит и смену форматов, то есть облик попадал бы в историю наравне с
// набранным текстом; и он умирает при пересборке, а пересборка — способ
// применить новый облик.
//
// Довод не проверялся ни разу. Синтетический пробник (zametti-bench zoom,
// раздел 7) уже показал, что из наших записей засоряют стек только две —
// setBlockFormat и setFrameFormat, — а setDefaultFont, setTextWidth,
// markContentsDirty и setIndentWidth чисты. Но синтетика не отвечает на главное:
// сколько шагов появляется на НАСТОЯЩИЙ жест в НАСТОЯЩЕМ редакторе.
//
// Здесь и меряется. Стек включается прямо на живом документе открытой заметки,
// дальше делается жест, и печатается три числа:
//
//   шагов      — сколько команд насчитал сам Qt;
//   впустую    — сколько раз человек нажмёт Ctrl+Z, прежде чем отмена доберётся
//                до его собственного текста. ЭТО ГЛАВНОЕ ЧИСЛО: длину стека
//                считает Qt, а нажатия считает человек;
//   жив        — пережил ли стек жест вообще (пересборка его убивает).
//
// Пробник ничего не проверяет и в приёмку не входит.

#include "editor_widget.h"
#include "settings.h"

#include "test_util.h"

#include "scratch_files.h"
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QKeyEvent>
#include <QScrollBar>
#include <QTest>
#include <QTextDocument>
#include <QTextEdit>

#include <cstdio>
#include <string>

namespace {

QString writeNote(const QString& dir, const QString& name, const QString& text) {
    QDir().mkpath(dir);
    const QString path = dir + QLatin1Char('/') + name;
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return {};
    file.write(text.toUtf8());
    file.close();
    return path;
}

void typeText(zametti::NoteEditor& editor, const QString& text) {
    for (const QChar c : text) {
        QKeyEvent press(QEvent::KeyPress, Qt::Key_unknown, Qt::NoModifier, QString(c));
        QApplication::sendEvent(&editor, &press);
    }
}

void pressKey(zametti::NoteEditor& editor, int key, Qt::KeyboardModifiers mods = Qt::NoModifier) {
    QKeyEvent press(QEvent::KeyPress, key, mods);
    QApplication::sendEvent(&editor, &press);
}

// Один жест: включаем стек начисто, делаем, считаем.
void measure(zametti::NoteEditor& editor, const char* what,
             const std::function<void()>& gesture) {
    QTextDocument* doc = editor.document();
    // Начисто: setUndoRedoEnabled(true) на уже включённом ничего не сбрасывает,
    // поэтому гасим и зажигаем — заодно видно, что стек и правда пуст.
    doc->setUndoRedoEnabled(false);
    doc->setUndoRedoEnabled(true);
    QTest::qWait(5);

    const QString before = doc->toPlainText();
    gesture();
    QTest::qWait(30);

    // Документ мог смениться: пересборка заводит новый или чистит стек.
    QTextDocument* now = editor.document();
    const bool alive = now == doc && now->isUndoRedoEnabled();
    const int steps = now->availableUndoSteps();

    // Сколько нажатий уйдёт мимо текста. Считается ОДИНАКОВО, менял жест текст
    // или нет: если не менял, то каждое доступное нажатие и есть чистое
    // загрязнение — человек жмёт Ctrl+Z, а текст не двигается.
    int wasted = 0;
    if (alive) {
        const QString after = now->toPlainText();
        while (wasted < 500 && now->isUndoAvailable()) {
            now->undo();
            if (now->toPlainText() != after) break;
            ++wasted;
        }
    }

    std::printf("   %-42s шагов %-4d впустую %-5s стек %s\n", what, steps,
                alive ? (wasted >= 500 ? "500+" : std::to_string(wasted).c_str()) : "?",
                alive ? "жив" : "УБИТ");
    std::fflush(stdout);
}

}  // namespace

int ztUndoProbe(int argc, char** argv) {
    (void)argc;
    (void)argv;
    zametti::loadSettings(nullptr);

    const QString dir = QStringLiteral("/tmp/zametti-undo-probe");
    zt::dropTree(dir);
    const QString plain = writeNote(dir, QStringLiteral("простая.md"),
                                    QStringLiteral("# Заголовок\n\n"
                                                   "Первый абзац, в нём есть слова.\n\n"
                                                   "- пункт раз\n- пункт два\n"));

    zametti::NoteEditor editor;
    editor.resize(800, 600);
    editor.show();
    QTest::qWait(30);
    editor.openFile(plain);
    QTest::qWait(30);

    std::printf("\nШТАТНЫЙ СТЕК ОТМЕНЫ: что переживает жест\n");
    std::printf("   «впустую» — сколько Ctrl+Z уйдёт мимо текста человека.\n");
    std::printf("   «500+» значит, что отмена до текста НЕ ДОХОДИТ ВОВСЕ.\n\n");

    measure(editor, "набрать один знак", [&] { typeText(editor, QStringLiteral("а")); });
    measure(editor, "набрать десять знаков",
            [&] { typeText(editor, QStringLiteral("мама мыла ")); });
    measure(editor, "Enter (операция splitBlock)",
            [&] { pressKey(editor, Qt::Key_Return); });
    measure(editor, "Backspace", [&] { pressKey(editor, Qt::Key_Backspace); });
    measure(editor, "потянуть окно за угол", [&] {
        editor.resize(600, 600);
        QTest::qWait(30);
        editor.resize(900, 600);
    });
    measure(editor, "Ctrl+= (масштаб)", [&] { editor.applyZoom(1.5); });
    measure(editor, "Ctrl+0 (масштаб обратно)", [&] { editor.applyZoom(1.0); });
    // По отдельности — кто именно засоряет. applyContentWidth пишет поля
    // корневой рамки, и это единственная запись вида в простой заметке.
    measure(editor, "setZoom сам по себе (только шрифт)",
            [&] { editor.setZoom(editor.zoom() * 1.3); });
    measure(editor, "applyContentWidth после смены шрифта",
            [&] { editor.applyContentWidth(); });
    measure(editor, "applyContentWidth сам по себе",
            [&] { editor.applyContentWidth(); });
    measure(editor, "syncFormulas сам по себе (таблицы считаются лениво из intrinsicSize)", [&] {
        editor.syncFormulas();
    });
    measure(editor, "прокрутка", [&] {
        editor.verticalScrollBar()->setValue(editor.verticalScrollBar()->maximum());
    });

    // Заметка с объектами: у них резерв места пишется в документ полями блоков,
    // и это второй по величине подозреваемый после пересборки.
    const QString rich = writeNote(dir, QStringLiteral("с-объектами.md"),
                                   QStringLiteral("# Со всяким\n\n"
                                                  "| a | b |\n|---|---|\n| 1 | 2 |\n\n"
                                                  "$$\\frac{a}{b}$$\n\n"
                                                  "Хвост.\n"));
    editor.openFile(rich);
    QTest::qWait(50);
    std::printf("\n   заметка с таблицей и выключной формулой:\n");
    measure(editor, "набрать знак", [&] { typeText(editor, QStringLiteral("ы")); });
    measure(editor, "потянуть окно за угол", [&] {
        editor.resize(600, 600);
        QTest::qWait(30);
        editor.resize(900, 600);
    });

    // Отдельно — путь ЧЕРЕЗ РЕДАКТОР: Ctrl+Z приходит в NoteEditor::undo, а не
    // прямо в документ, и по дороге есть и сохранение, и режим истории.
    editor.openFile(plain);
    QTest::qWait(50);
    {
        const QString before = editor.document()->toPlainText().left(40);
        typeText(editor, QStringLiteral("СЛОВО"));
        QTest::qWait(50);
        const QString typed = editor.document()->toPlainText().left(40);
        std::printf("\n   через редактор: шагов %d, отмена доступна %s\n",
                    editor.document()->availableUndoSteps(),
                    editor.document()->isUndoAvailable() ? "да" : "нет");
        editor.undo();
        QTest::qWait(50);
        const QString after = editor.document()->toPlainText().left(40);
        std::printf("   было   [%s]\n   набрал [%s]\n   отмена [%s]\n",
                    qPrintable(before), qPrintable(typed), qPrintable(after));
        std::printf("   дно цепочки (дальше — история): %s\n",
                    editor.document()->isUndoAvailable() ? "нет" : "ДА");
    }

    // Автозамена: набрано два знака, потом операция. Сколько шагов и что
    // снимает каждое нажатие.
    {
        const QString rule = writeNote(dir, QStringLiteral("правило.md"),
                                       QStringLiteral("текст\n"));
        editor.openFile(rule);
        QTest::qWait(50);
        QTextCursor at = editor.textCursor();
        at.setPosition(editor.document()->firstBlock().position());
        editor.setTextCursor(at);
        std::printf("\n   автозамена «* »:\n");
        std::printf("     до набора: шагов %d\n", editor.document()->availableUndoSteps());
        typeText(editor, QStringLiteral("*"));
        QTest::qWait(20);
        std::printf("     после «*»: шагов %d\n", editor.document()->availableUndoSteps());
        typeText(editor, QStringLiteral(" "));
        QTest::qWait(50);
        std::printf("     после пробела: шагов %d, текст [%s]\n",
                    editor.document()->availableUndoSteps(),
                    qPrintable(editor.document()->toPlainText().left(30)));
        for (int i = 1; i <= 4; ++i) {
            editor.undo();
            QTest::qWait(20);
            std::printf("     undo %d: [%s]\n", i,
                        qPrintable(editor.document()->toPlainText().left(30)));
        }
    }

    // МОЖНО ЛИ РВАТЬ СКЛЕЙКУ QT. Владелец однажды уже сказал про слипшийся
    // набор «буфера на 200 шагов как будто нет», и наши три правила границы
    // серии написаны ровно из-за этого. Qt склеивает по-своему; вопрос — есть
    // ли способ поставить границу там, где нам надо.
    {
        const QString g = writeNote(dir, QStringLiteral("склейка.md"),
                                    QStringLiteral("начало\n"));
        editor.openFile(g);
        QTest::qWait(50);
        QTextDocument* d = editor.document();

        typeText(editor, QStringLiteral("мама мыла раму"));
        QTest::qWait(50);
        std::printf("\n   склейка набора:\n");
        std::printf("     без границ: шагов %d\n", d->availableUndoSteps());

        editor.openFile(g);
        QTest::qWait(50);
        d = editor.document();
        typeText(editor, QStringLiteral("мама"));
        {
            QTextCursor cut(d);
            cut.beginEditBlock();
            cut.endEditBlock();
        }
        typeText(editor, QStringLiteral(" мыла"));
        QTest::qWait(50);
        std::printf("     пустая скобка посередине: шагов %d\n", d->availableUndoSteps());
        const QString all = d->toPlainText();
        editor.undo();
        QTest::qWait(20);
        std::printf("     после набора [%s] один undo даёт [%s]\n",
                    qPrintable(all.left(30)),
                    qPrintable(d->toPlainText().left(30)));
    }

    // Чем ставится ГРАНИЦА шага: пробуем вставлять текст самим, в скобке.
    {
        QTextDocument d;
        d.setUndoRedoEnabled(true);
        QTextCursor c(&d);
        c.insertText(QStringLiteral("начало"));

        auto steps = [&d] { return d.availableUndoSteps(); };
        const int base = steps();

        // 1. Подряд, без скобок — Qt склеивает.
        for (const QChar ch : QStringLiteral("мама"))
            QTextCursor(&d).insertText(QString(ch));
        // Осторожно: каждый новый курсор в начале документа. Пишем в конец.
        d.clear();
        d.setUndoRedoEnabled(false);
        d.setUndoRedoEnabled(true);
        QTextCursor at(&d);
        at.movePosition(QTextCursor::End);
        for (const QChar ch : QStringLiteral("мама"))
            at.insertText(QString(ch));
        const int plain = steps();

        // 2. Каждое слово — в своей скобке.
        d.clear();
        d.setUndoRedoEnabled(false);
        d.setUndoRedoEnabled(true);
        QTextCursor at2(&d);
        for (const QString& word : {QStringLiteral("мама"), QStringLiteral(" мыла"),
                                    QStringLiteral(" раму")}) {
            at2.beginEditBlock();
            for (const QChar ch : word) at2.insertText(QString(ch));
            at2.endEditBlock();
        }
        const int grouped = steps();

        std::printf("\n   границы шага (base %d):\n", base);
        std::printf("     четыре знака подряд, без скобок: шагов %d\n", plain);
        std::printf("     три слова, каждое в своей скобке: шагов %d\n", grouped);
        int presses = 0;
        while (d.isUndoAvailable() && presses < 10) {
            d.undo();
            ++presses;
            std::printf("     undo %d: [%s]\n", presses,
                        qPrintable(d.toPlainText()));
        }
    }

    // REDO ПОСЛЕ СЕРИИ ОТМЕН. Жалоба владельца: набираю с правками Backspace,
    // несколько Ctrl+Z — хорошо, а повтор возвращает пару слов и встаёт.
    {
        const QString r = writeNote(dir, QStringLiteral("повтор.md"),
                                    QStringLiteral("начало\n"));
        editor.openFile(r);
        QTest::qWait(50);
        QTextCursor at = editor.textCursor();
        at.movePosition(QTextCursor::End);
        editor.setTextCursor(at);

        typeText(editor, QStringLiteral("один два тир"));
        pressKey(editor, Qt::Key_Backspace);
        pressKey(editor, Qt::Key_Backspace);
        typeText(editor, QStringLiteral("ри четыре пять"));
        QTest::qWait(60);
        const QString full = editor.document()->toPlainText();

        int undos = 0;
        while (undos < 12 && editor.document()->isUndoAvailable()) {
            editor.undo();
            QTest::qWait(10);
            ++undos;
        }
        const QString bottom = editor.document()->toPlainText();
        int redos = 0;
        while (redos < 12 && editor.document()->isRedoAvailable()) {
            editor.redo();
            QTest::qWait(10);
            ++redos;
        }
        const QString back = editor.document()->toPlainText();
        std::printf("\n   повтор после серии отмен: отмен %d, повторов %d\n", undos, redos);
        std::printf("     набрано  [%s]\n     дно      [%s]\n     вернулось[%s]\n",
                    qPrintable(full.trimmed()), qPrintable(bottom.trimmed()),
                    qPrintable(back.trimmed()));
        std::printf("     повтор вернул всё: %s\n", back == full ? "ДА" : "НЕТ");
    }

    // ГДЕ КАРЕТКА ПОСЛЕ Enter И Ctrl+Z. Жалоба владельца: уезжает в начало
    // документа.
    {
        QString big = QStringLiteral("# Заголовок\n\n");
        for (int i = 0; i < 40; ++i)
            big += QStringLiteral("Абзац номер %1, в нём достаточно слов.\n\n").arg(i);
        const QString path = writeNote(dir, QStringLiteral("enter.md"), big);
        editor.openFile(path);
        QTest::qWait(50);

        QTextDocument* d = editor.document();
        const int middle = d->blockCount() / 2;
        QTextCursor at(d);
        at.setPosition(d->findBlockByNumber(middle).position() + 5);
        editor.setTextCursor(at);
        const int before = editor.textCursor().position();

        pressKey(editor, Qt::Key_Return);
        QTest::qWait(40);
        const int afterEnter = editor.textCursor().position();

        editor.undo();
        QTest::qWait(40);
        const int afterUndo = editor.textCursor().position();

        std::printf("\n   Enter и Ctrl+Z: каретка была %d, после Enter %d, после отмены %d\n",
                    before, afterEnter, afterUndo);
        std::printf("     блок каретки после отмены: %d (был %d)\n",
                    editor.textCursor().blockNumber(), middle);
        std::printf("     шагов отмены осталось: %d\n", d->availableUndoSteps());
    }

    // СКОЛЬКО ШАГОВ ОТМЕНЫ ДЕЛАЕТ САМ Qt на набранной фразе — и ЗАВИСИТ ЛИ ЭТО
    // ОТ ПАУЗ. Догадка владельца: Qt может резать шаг сам, когда человек
    // задумался. Меряем НА ЧИСТОМ QTextEdit, без нашего кода вовсе.
    {
        const QString text = QStringLiteral("мама мыла раму очень долго и упорно");
        std::printf("\n   ЧИСТЫЙ QTextEdit: фраза из %d знаков, %d слов\n",
                    int(text.size()), int(text.split(QLatin1Char(' ')).size()));
        for (const int pause : {0, 100, 400, 1500}) {
            QTextEdit plain;
            plain.resize(800, 600);
            plain.show();
            QTest::qWait(20);
            plain.document()->setUndoRedoEnabled(true);

            for (const QChar ch : text) {
                QKeyEvent press(QEvent::KeyPress, Qt::Key_unknown, Qt::NoModifier, QString(ch));
                QApplication::sendEvent(&plain, &press);
                // Пауза — только между словами: внутри слова человек не думает.
                if (ch == QLatin1Char(' ') && pause > 0) QTest::qWait(pause);
            }
            QTest::qWait(50);

            int presses = 0;
            while (plain.document()->isUndoAvailable() && presses < 50) {
                plain.undo();
                ++presses;
            }
            std::printf("     пауза между словами %4d мс → отменяется за %d нажатий\n", pause,
                        presses);
        }
    }

    std::printf("\n");
    return 0;
}
