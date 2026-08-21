// РЕЖИМ ПРАВКИ НАСТРОЕК ЦЕЛИКОМ: модель (ZConfigFile), вид (JsonEditView) и
// контроллер вместе — так же, как их связывает окно.
//
// Что спрашивается: конфига нет — открылся шаблон; Tab ставит пробелы до стопа
// jsonEditing.tabIndent, Enter держит отступ, Ctrl+/ комментирует и снимает
// комментарий (строка, выделение, смешанное), Ctrl+S пишет файл, Esc пишет и
// закрывает режим, undo/redo работают в рамках сеанса, каретка того же цвета и
// толщины, что в заметке, и правленый конфиг ПРИМЕНЯЕТСЯ (это делает окно —
// здесь проверяется через loadSettings своим каталогом настроек).

#include "config_file.h"
#include "editor_widget.h"
#include "json_edit_view.h"
#include "settings.h"
#include "settings_controller.h"
#include "settings_hook.h"

#include "keys.h"
#include "test_util.h"
#include "testdata.h"

#include <QDir>
#include <QFile>
#include <QImage>
#include <QTemporaryDir>
#include <QTest>
#include <QTextBlock>
#include <QTextCursor>

#include <memory>
#include <string>

namespace {

QString g_dir;

std::string readFile(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return file.readAll().toStdString();
}

// Живое окно режима: вид, модель и контроллер между ними — как в main().
struct Rig {
    QString path;
    std::shared_ptr<zametti::ZConfigFile> model;
    zametti::JsonEditView view;
    zametti::SettingsController controller;

    explicit Rig(const QString& name)
        : path(QDir(g_dir).filePath(name)),
          model(std::make_shared<zametti::ZConfigFile>(path)),
          controller(view, model) {
        view.resize(700, 500);
        view.show();
        QTest::qWait(20);
    }
    QString lineAt(int number) const {
        return view.document()->findBlockByNumber(number).text();
    }
    void putCaret(int line, int column) {
        QTextCursor at(view.document());
        const QTextBlock block = view.document()->findBlockByNumber(line);
        at.setPosition(block.position() + column);
        view.setTextCursor(at);
    }
    void selectLines(int first, int last) {
        QTextCursor at(view.document());
        at.setPosition(view.document()->findBlockByNumber(first).position());
        const QTextBlock end = view.document()->findBlockByNumber(last);
        at.setPosition(end.position() + end.length() - 1, QTextCursor::KeepAnchor);
        view.setTextCursor(at);
    }
};

// Конфига нет — открылся шаблон; и он разбирается в пустой объект.
void checkOpensTemplate() {
    Rig rig(QStringLiteral("шаблон/config.json"));
    ZT_TRUE("вошли в режим", rig.controller.enter());
    ZT_TRUE("режим идёт", rig.controller.active());
    ZT_TRUE("файл появился", QFile::exists(rig.path));
    ZT_EQ("вид показывает шаблон", zametti::configTemplate().toStdString(),
          rig.view.text().toStdString());
    ZT_TRUE("вышли", rig.controller.leave());
    ZT_TRUE("режим кончился", !rig.controller.active());
}

// Tab — пробелы до стопа jsonEditing.tabIndent (не знак табуляции), Shift+Tab
// снимает; Enter держит отступ предыдущей строки.
void checkTabAndEnter() {
    Rig rig(QStringLiteral("клавиши/config.json"));
    ZT_TRUE("вошли", rig.controller.enter());
    rig.view.setText(QStringLiteral("{\n    \"font\": {\n"), 0, 0);

    const int stop = zametti::settings().jsonEditing().tabIndent();
    rig.putCaret(0, 0);
    QTest::keyClick(&rig.view, Qt::Key_Tab);
    ZT_EQ("Tab — пробелы до стопа", std::string(size_t(stop), ' ') + "{",
          rig.lineAt(0).toStdString());
    ZT_TRUE("знака табуляции нет", !rig.view.text().contains(QLatin1Char('\t')));
    QTest::keyClick(&rig.view, Qt::Key_Backtab);
    ZT_EQ("Shift+Tab снял", std::string("{"), rig.lineAt(0).toStdString());

    // Enter в конце строки с отступом — новая строка с тем же отступом.
    rig.putCaret(1, int(rig.lineAt(1).size()));
    QTest::keyClick(&rig.view, Qt::Key_Return);
    ZT_EQ("Enter держит отступ", std::string(4, ' '), rig.lineAt(2).toStdString());
    ZT_EQ("и каретка в конце отступа", std::string("4"), std::to_string(rig.view.caretColumn()));
    rig.controller.leave();
}

// Ctrl+/ (jsonEditing.commentKey): строка каретки, выделение, смешанное
// выделение; один шаг отмены на нажатие.
void checkCommentToggle() {
    Rig rig(QStringLiteral("комментарии/config.json"));
    ZT_TRUE("вошли", rig.controller.enter());
    rig.view.setText(QStringLiteral("{\n  \"a\": 1,\n  \"b\": 2,\n\n  \"c\": 3\n}\n"), 1, 3);

    ZT_TRUE("сочетание есть", zt::pressKey(&rig.view, zametti::settings().jsonEditing().commentKey()));
    ZT_EQ("строка закомментирована", std::string("  // \"a\": 1,"), rig.lineAt(1).toStdString());
    ZT_TRUE("отмена одним шагом", rig.view.document()->isUndoAvailable());
    rig.view.undo();
    ZT_EQ("и вернула строку", std::string("  \"a\": 1,"), rig.lineAt(1).toStdString());
    zt::pressKey(&rig.view, zametti::settings().jsonEditing().commentKey());
    zt::pressKey(&rig.view, zametti::settings().jsonEditing().commentKey());
    ZT_EQ("второе нажатие снимает", std::string("  \"a\": 1,"), rig.lineAt(1).toStdString());

    // Выделение в несколько строк: пустая не трогается, «//» встаёт в
    // минимальной колонке отступа.
    rig.view.setText(QStringLiteral("{\n  \"a\": 1,\n    \"b\": 2,\n\n  \"c\": 3\n}\n"), 0, 0);
    rig.selectLines(1, 4);
    zt::pressKey(&rig.view, zametti::settings().jsonEditing().commentKey());
    ZT_EQ("первая строка", std::string("  // \"a\": 1,"), rig.lineAt(1).toStdString());
    ZT_EQ("вторая — с её отступом", std::string("  //   \"b\": 2,"), rig.lineAt(2).toStdString());
    ZT_EQ("пустая не тронута", std::string(), rig.lineAt(3).toStdString());
    ZT_EQ("последняя", std::string("  // \"c\": 3"), rig.lineAt(4).toStdString());
    ZT_TRUE("выделение осталось на тех же строках",
            rig.view.textCursor().hasSelection() &&
                rig.view.document()->findBlock(qMin(rig.view.textCursor().anchor(),
                                                    rig.view.textCursor().position()))
                        .blockNumber() == 1);

    // Смешанное выделение (часть строк уже с «//») — комментируем всё.
    rig.view.setText(QStringLiteral("  // \"a\": 1,\n  \"b\": 2,\n"), 0, 0);
    rig.selectLines(0, 1);
    zt::pressKey(&rig.view, zametti::settings().jsonEditing().commentKey());
    ZT_EQ("уже закомментированная — ещё раз", std::string("  // // \"a\": 1,"),
          rig.lineAt(0).toStdString());
    ZT_EQ("и вторая тоже", std::string("  // \"b\": 2,"), rig.lineAt(1).toStdString());
    zt::pressKey(&rig.view, zametti::settings().jsonEditing().commentKey());
    ZT_EQ("а теперь снялись обе", std::string("  // \"a\": 1,"), rig.lineAt(0).toStdString());
    rig.controller.leave();
}

// Ctrl+S пишет файл; Esc пишет и закрывает режим; undo/redo — в рамках сеанса.
void checkSaveAndLeave() {
    Rig rig(QStringLiteral("запись/config.json"));
    ZT_TRUE("вошли", rig.controller.enter());
    rig.view.setText(QStringLiteral("{ \"font\": { \"pointSize\": 13 } }\n"), 0, 0);

    int savedOk = 0;
    QObject::connect(&rig.controller, &zametti::SettingsController::saved, &rig.view,
                     [&savedOk](bool ok, const QString&) { savedOk += ok ? 1 : 0; });
    ZT_TRUE("Ctrl+S записал", rig.controller.save());
    ZT_EQ("и сказал об этом сигналом", std::string("1"), std::to_string(savedOk));
    ZT_EQ("файл на диске", std::string("{ \"font\": { \"pointSize\": 13 } }\n"), readFile(rig.path));
    ZT_TRUE("режим не кончился", rig.controller.active());

    // Набор и отмена — свои, как у обычного текстового редактора.
    QTextCursor at(rig.view.document());
    at.movePosition(QTextCursor::End);
    rig.view.setTextCursor(at);
    QTest::keyClicks(&rig.view, QStringLiteral("// tail"));
    QTest::qWait(10);
    ZT_TRUE("набранное в тексте", rig.view.text().contains(QStringLiteral("// tail")));
    while (rig.view.document()->isUndoAvailable()) rig.view.undo();
    ZT_TRUE("отмена вернула текст", !rig.view.text().contains(QStringLiteral("// tail")));
    rig.view.redo();
    ZT_TRUE("повтор вернул набранное", rig.view.text().contains(QStringLiteral("//")));

    // Esc — записать и выйти (диалогов подтверждения у нас нет).
    while (rig.view.document()->isUndoAvailable()) rig.view.undo();
    at.movePosition(QTextCursor::End);
    rig.view.setTextCursor(at);
    QTest::keyClicks(&rig.view, QStringLiteral("// bye"));
    QTest::keyClick(&rig.view, Qt::Key_Escape);
    QTest::qWait(10);
    ZT_TRUE("Esc закрыл режим", !rig.controller.active());
    ZT_TRUE("и записал набранное", readFile(rig.path).find("// bye") != std::string::npos);
}

// БИТЫЙ JSON ТОЖЕ ЗАПИСЫВАЕТСЯ (решение владельца: файл человека), но
// программой не применяется — об этом говорит проверка модели, а показывает
// окно. Проверяем обе половины: файл записан, check жалуется со строкой.
void checkBrokenIsWrittenButNotApplied() {
    Rig rig(QStringLiteral("битый/config.json"));
    ZT_TRUE("вошли", rig.controller.enter());
    rig.view.setText(QStringLiteral("{\n  \"font\": { \"pointSize\": }\n}\n"), 0, 0);
    ZT_TRUE("записался", rig.controller.save());
    ZT_TRUE("файл на диске битый", readFile(rig.path).find("\"pointSize\": }") != std::string::npos);
    const zametti::ZConfigFile::Check check = zametti::ZConfigFile::check(rig.view.text());
    ZT_TRUE("и проверка о нём говорит", !check.ok && !check.error.isEmpty());
    ZT_EQ("со строкой", std::string("2"), std::to_string(check.line));
    rig.controller.leave();
}

// ПРАВЛЕНЫЙ КОНФИГ ПРИМЕНЯЕТСЯ. Настройки перечитываются своим каталогом
// (XDG_CONFIG_HOME набора), и вид берёт новый стоп табуляции.
void checkAppliedAfterSave() {
    QTemporaryDir home;
    const QByteArray previousHome = qgetenv("XDG_CONFIG_HOME");
    qputenv("XDG_CONFIG_HOME", home.path().toLocal8Bit());

    zametti::JsonEditView view;
    auto model = std::make_shared<zametti::ZConfigFile>(zametti::configPath());
    zametti::SettingsController controller(view, model);
    view.resize(700, 500);
    view.show();
    QTest::qWait(20);
    ZT_TRUE("вошли", controller.enter());
    view.setText(QStringLiteral("{ \"jsonEditing\": { \"tabIndent\": 8 } }\n"), 0, 0);
    ZT_TRUE("записали", controller.save());

    QString error;
    ZT_TRUE("настройки перечитаны: " + error.toStdString(), zametti::loadSettings(&error));
    ZT_EQ("новый стоп в настройках", std::string("8"),
          std::to_string(zametti::settings().jsonEditing().tabIndent()));
    view.refreshAppearance();
    view.setText(QStringLiteral("x\n"), 0, 0);
    QTest::keyClick(&view, Qt::Key_Tab);
    ZT_EQ("и Tab ставит восемь пробелов", std::string(8, ' ') + "x",
          view.document()->findBlockByNumber(0).text().toStdString());
    controller.leave();

    // Настройки и каталог возвращаем как были: наборы идут одним процессом.
    zametti::mutableSettingsForTests() = zametti::ZSettings{};
    qputenv("XDG_CONFIG_HOME", previousHome);
}

// КАРЕТКА — как в заметке: цвет caretColor, толщина caretWidth × масштаб
// (общий PlainEditView; та же проверка, что у вида исходника).
void checkCaretLook() {
    Rig rig(QStringLiteral("каретка/config.json"));
    ZT_TRUE("вошли", rig.controller.enter());
    rig.view.setText(QStringLiteral("{ \"some\": \"text here\" }\n"), 0, 0);
    rig.view.setFocus();
    QTest::qWait(20);
    ZT_TRUE("фокус у вида", rig.view.hasFocus());
    rig.putCaret(0, 5);
    QCoreApplication::processEvents();

    const QColor caret = zametti::settings().style().caretColor();
    const int want = qMax(1, qRound(zametti::settings().style().caretWidth() * rig.view.zoom()));
    const QRect r = rig.view.cursorRect();
    const QImage shot = rig.view.viewport()->grab().toImage();
    const int y = r.center().y();
    int run = 0;
    for (int x = r.left(); x < shot.width() && shot.pixelColor(x, y) == caret; ++x) ++run;
    ZT_EQ("каретка цвета caretColor и толщиной caretWidth", std::to_string(want),
          std::to_string(run));
    rig.controller.leave();
}

}  // namespace

TEST(SettingsEdit, All) {
    g_dir = zt::TestData::outDir(QStringLiteral("settings-edit"));
    checkOpensTemplate();
    checkTabAndEnter();
    checkCommentToggle();
    checkSaveAndLeave();
    checkBrokenIsWrittenButNotApplied();
    checkAppliedAfterSave();
    checkCaretLook();
}

// ЯРЛЫК ОКНА СЪЕДАЕТ Esc (дефект, найденный ревью refactor3). В живом окне
// Esc — QShortcut на окне (main.cpp), а он срабатывает РАНЬШЕ, чем нажатие
// доходит до виджета с фокусом: значит keyPressEvent вида его не видит вовсе,
// и обещанный README выход из режима по Esc не работал бы. Порядок решает
// escapeActionFor — одно место на все режимы (там же ловится Esc у поля языка
// и панели поиска).
namespace {

void checkEscapeGoesThroughWindowShortcut() {
    // Порядок в самой функции: выйти из режима — после того, как закрылось всё,
    // что открыто ПОВЕРХ текста.
    using zametti::EscapeAction;
    ZT_TRUE("режим закрывается, когда больше нечего закрывать",
            zametti::escapeActionFor(false, false, false, true) == EscapeAction::LeaveMode);
    ZT_TRUE("панель поиска раньше режима",
            zametti::escapeActionFor(false, false, true, true) == EscapeAction::CloseFindBar);
    ZT_TRUE("поле языка раньше всех",
            zametti::escapeActionFor(true, false, true, true) == EscapeAction::CloseLanguageEditor);
    ZT_TRUE("без режима — как было",
            zametti::escapeActionFor(false, false, false, false) == EscapeAction::Nothing);
}

}  // namespace

TEST(SettingsEscape, All) { checkEscapeGoesThroughWindowShortcut(); }
