// ШТАТНЫЙ СТЕК ОТМЕНЫ QTextDocument: чем именно мы его теряем.
//
// Довод, по которому стек выключен, записан в edit_history.h и звучит так: он
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

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QKeyEvent>
#include <QScrollBar>
#include <QTest>
#include <QTextDocument>

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
    zametti::loadAppearance(nullptr);

    const QString dir = QStringLiteral("/tmp/zametti-undo-probe");
    QDir(dir).removeRecursively();
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
    measure(editor, "applyContentWidth сам по себе",
            [&] { editor.applyContentWidth(); });
    measure(editor, "syncFormulas + syncTables сами по себе", [&] {
        editor.syncTables();
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

    std::printf("\n");
    return 0;
}
