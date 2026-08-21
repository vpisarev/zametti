// РЕЖИМ ИСХОДНИКА ПЕРЕЖИВАЕТ ПОХОД В ИСТОРИЮ (просьба владельца, refactor3).
//
// Пока режим исходника идёт, Ctrl+Z на дне его стека уводит в стек заметки, а с
// пустого стека заметки — в историю. Прежде из истории человек возвращался в
// обычный вид, хотя ушёл из исходника. Здесь проверяется весь круг на живых
// объектах — редактор с хранилищем, контроллер истории, контроллер исходника,
// — связанных так же, как в окне (attachHistory), и главное требование
// владельца: после восстановления из слепка итог ТОТ ЖЕ, что без режима
// исходника (восстановление — одна правка заметки, переключение без правок
// стек не трогает). Эталон — параллельный риг без режима исходника.

#include "editor_widget.h"
#include "history_controller.h"
#include "history_view.h"
#include "markdown_controller.h"
#include "markdown_edit_view.h"
#include "pieces.h"

#include "test_util.h"
#include "testdata.h"

#include <QDir>
#include <QFile>
#include <QTest>
#include <QTextCursor>
#include <QTextDocument>

#include <string>
#include <vector>

namespace {

QString g_root;

std::string textOf(zametti::NoteEditor& editor) {
    return markdownOf(blocksOf(*editor.document()));
}

// Заметка в хранилище с двумя слепками в журнале: файл — вторая версия; обе
// записаны редактором, как в жизни (открыли — записали — правили — записали),
// но ДРУГИМ экземпляром: редактор рига открывает её со свежим стеком, как при
// запуске программы (открытые заметки редактор держит в кэше вместе со стеком).
QString makeNote(const QString& name, zametti::NoteEditor& editor, const QString& first,
                 const QString& second) {
    QDir().mkpath(g_root + QStringLiteral("/.zametti"));
    QDir().mkpath(g_root + QStringLiteral("/history"));
    const QString path = g_root + QLatin1Char('/') + name + QStringLiteral(".md");
    {
        QFile file(path);
        if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
            file.write(QByteArray("<!-- zametti\ncreated: 2026-01-01T00:00:00Z\n-->\n\n") +
                       first.toUtf8());
    }
    {
        zametti::NoteEditor writer;
        writer.setStoreRoot(g_root);
        writer.openFile(path);
        QTest::qWait(20);
        writer.save(false, /*force=*/true);   // первый слепок
        QTextCursor whole(writer.document());
        whole.select(QTextCursor::Document);
        writer.setTextCursor(whole);
        writer.insertPlainText(second.trimmed());
        QTest::qWait(20);
        writer.save(false);                   // второй слепок
        QTest::qWait(20);
    }
    editor.openFile(path);
    QTest::qWait(20);
    return path;
}

// Окно режима: редактор, история и исходник, связанные как в main().
struct Rig {
    zametti::NoteEditor editor;
    zametti::HistoryView historyView;
    zametti::HistoryController history;
    zametti::MarkdownEditView view;
    zametti::MarkdownController markdown;
    std::vector<bool> markdownModes;   // журнал modeChanged режима исходника

    Rig() : history(editor, historyView), markdown(editor, view) {
        editor.setStoreRoot(g_root);
        editor.resize(700, 500);
        editor.show();
        view.resize(700, 500);
        view.show();
        historyView.resize(700, 500);
        historyView.show();
        markdown.attachHistory(history);
        QObject::connect(&markdown, &zametti::MarkdownController::modeChanged, &editor,
                         [this](bool on) { markdownModes.push_back(on); });
        QTest::qWait(20);
    }
    void undoKey() {
        QTest::keyClick(&view, Qt::Key_Z, Qt::ControlModifier);
        QTest::qWait(10);
    }
};

// (а) Исходник → Ctrl+Z до истории → выход из истории → снова исходник.
void checkReturnsFromHistory() {
    Rig rig;
    makeNote(QStringLiteral("круг"), rig.editor, QStringLiteral("раз\n"), QStringLiteral("два\n"));
    ZT_TRUE("вошли в исходник", rig.markdown.enter());
    ZT_EQ("стек заметки пуст — как при запуске", std::string("0"),
          std::to_string(rig.editor.document()->availableUndoSteps()));

    rig.undoKey();   // дно стека режима → отмена заметке → история
    ZT_TRUE("история открылась", rig.history.active());
    ZT_TRUE("исходник отложен", !rig.markdown.active());

    rig.history.leave();
    QTest::qWait(10);
    ZT_TRUE("из истории вернулись в исходник", rig.markdown.active());
    ZT_EQ("вид показывает исходник заметки", std::string("два\n"),
          rig.view.source().toStdString());
    ZT_EQ("кнопка режима погасла и снова зажглась", std::string("1 0 1"),
          [&] {
              std::string out;
              for (bool on : rig.markdownModes) out += (out.empty() ? "" : " ") + std::string(on ? "1" : "0");
              return out;
          }());
    rig.markdown.leave();
}

// (б) Кнопка истории при идущем исходнике с несохранённой правкой текста:
// правка уходит в заметку и файл, история открывается, выход — исходник
// обратно с этой правкой.
void checkHistoryButtonSuspends() {
    Rig rig;
    const QString path = makeNote(QStringLiteral("кнопка"), rig.editor, QStringLiteral("раз\n"),
                                  QStringLiteral("два\n"));
    ZT_TRUE("вошли", rig.markdown.enter());
    QTextCursor edit = rig.view.textCursor();
    edit.movePosition(QTextCursor::End);
    edit.insertText(QStringLiteral("\nтри\n"));

    ZT_TRUE("история открылась", rig.history.enter());
    ZT_TRUE("исходник отложен", !rig.markdown.active());
    ZT_EQ("правка исходника наложена на заметку", std::string("два\n\nтри\n"), textOf(rig.editor));
    QFile file(path);
    ZT_TRUE("и записана в файл", file.open(QIODevice::ReadOnly) && file.readAll().contains("три"));

    rig.history.leave();
    QTest::qWait(10);
    ZT_TRUE("исходник вернулся", rig.markdown.active());
    ZT_EQ("с правкой", std::string("два\n\nтри\n"), rig.view.source().toStdString());
    rig.markdown.leave();
}

// (в) Сценарий владельца дословно, с эталоном: старт в исходнике, Ctrl+Z →
// история → восстановить предыдущую → снова исходник с восстановленным → в
// обычный вид → тот же текст, один шаг отмены, Ctrl+Z возвращает живую
// версию. Эталон — то же самое без исходника.
void checkRestoreMatchesPlainPath() {
    Rig rig;
    makeNote(QStringLiteral("восст"), rig.editor, QStringLiteral("раз\n"), QStringLiteral("два\n"));
    ZT_TRUE("вошли в исходник", rig.markdown.enter());
    rig.undoKey();
    ZT_TRUE("история", rig.history.active());
    ZT_TRUE("шаг назад — к первому слепку", rig.history.stepBack() || rig.history.index() == 0);
    ZT_TRUE("восстановили", rig.history.restore() > 0);
    QTest::qWait(20);
    ZT_TRUE("история закрылась", !rig.history.active());
    ZT_TRUE("исходник вернулся", rig.markdown.active());
    ZT_EQ("и показывает восстановленное", std::string("раз\n"), rig.view.source().toStdString());

    ZT_EQ("выход без правок — ноль кусков", std::string("0"), std::to_string(rig.markdown.leave()));
    ZT_EQ("обычный вид: восстановленный текст", std::string("раз\n"), textOf(rig.editor));
    // Глубина стека — числом команд Qt (availableUndoSteps считает команды, а
    // не шаги: у восстановления их три в одном блоке), сверяется с эталоном.
    const int stepsViaMarkdown = rig.editor.document()->availableUndoSteps();
    ZT_TRUE("восстановление отменяемо", rig.editor.document()->isUndoAvailable());
    rig.editor.undo();
    QTest::qWait(10);
    ZT_EQ("один Ctrl+Z вернул живую версию", std::string("два\n"), textOf(rig.editor));
    ZT_TRUE("и это был единственный шаг", !rig.editor.document()->isUndoAvailable());

    // Эталон: без исходника.
    Rig plain;
    makeNote(QStringLiteral("эталон"), plain.editor, QStringLiteral("раз\n"), QStringLiteral("два\n"));
    plain.editor.undo();   // дно стека → история
    ZT_TRUE("эталон: история", plain.history.active());
    ZT_TRUE("эталон: шаг назад", plain.history.stepBack() || plain.history.index() == 0);
    ZT_TRUE("эталон: восстановили", plain.history.restore() > 0);
    QTest::qWait(20);
    ZT_EQ("эталон: тот же текст", std::string("раз\n"), textOf(plain.editor));
    ZT_EQ("эталон: та же глубина стека", std::to_string(plain.editor.document()->availableUndoSteps()),
          std::to_string(stepsViaMarkdown));
    plain.editor.undo();
    QTest::qWait(10);
    ZT_EQ("эталон: Ctrl+Z вернул живую версию", std::string("два\n"), textOf(plain.editor));
    ZT_TRUE("эталон: и это был единственный шаг", !plain.editor.document()->isUndoAvailable());
}

// (г) Переключение без правок стек не трогает: правка → Ctrl+Z → вход/выход →
// redo жив; десять кругов не меняют глубину. И Ctrl+Z на дне при непустом
// стеке заметки остаётся в обычном виде (сценарий A).
void checkSwitchingKeepsUndoStack() {
    Rig rig;
    makeNote(QStringLiteral("стек"), rig.editor, QStringLiteral("раз\n"), QStringLiteral("два\n"));
    QTextCursor at = rig.editor.textCursor();
    at.movePosition(QTextCursor::End);
    rig.editor.setTextCursor(at);
    QTest::keyClicks(&rig.editor, QStringLiteral(" tri"));   // набором, как человек (латиницей: keyClicks знает только ASCII)
    QTest::qWait(20);
    ZT_TRUE("правка в стеке", rig.editor.document()->isUndoAvailable());
    // Набор ложится в стек серией (пробел рвёт серию): отменяем до дна стека
    // заметки — в историю NoteEditor::undo с пустого стека не зовём.
    for (int i = 0; i < 10 && rig.editor.document()->isUndoAvailable(); ++i) {
        rig.editor.undo();
        QTest::qWait(10);
    }
    ZT_EQ("отменена", std::string("два\n"), textOf(rig.editor));
    ZT_TRUE("redo доступен", rig.editor.document()->isRedoAvailable());
    const int redoSteps = rig.editor.document()->availableRedoSteps();

    for (int i = 0; i < 10; ++i) {
        ZT_TRUE("вошли", rig.markdown.enter());
        ZT_EQ("вышли без кусков", std::string("0"), std::to_string(rig.markdown.leave()));
    }
    ZT_TRUE("глубина undo та же", !rig.editor.document()->isUndoAvailable());
    ZT_EQ("redo пережил десять кругов", std::to_string(redoSteps),
          std::to_string(rig.editor.document()->availableRedoSteps()));
    for (int i = 0; i < 10 && rig.editor.document()->isRedoAvailable(); ++i) {
        rig.editor.redo();
        QTest::qWait(10);
    }
    ZT_EQ("redo вернул правку", std::string("два tri\n"), textOf(rig.editor));

    // Сценарий A: стек заметки непуст — Ctrl+Z на дне режима отменяет шаг
    // заметки и остаётся в обычном виде.
    ZT_TRUE("вошли", rig.markdown.enter());
    rig.undoKey();
    ZT_TRUE("режим закрылся", !rig.markdown.active());
    ZT_TRUE("в историю не ушли", !rig.history.active());
    ZT_TRUE("шаг заметки отменён", textOf(rig.editor) != "два tri\n");
}

}  // namespace

TEST(MarkdownHistory, All) {
    g_root = zt::TestData::outDir(QStringLiteral("markdown-history"));
    checkReturnsFromHistory();
    checkHistoryButtonSuspends();
    checkRestoreMatchesPlainPath();
    checkSwitchingKeepsUndoStack();
}
