// РЕЖИМ ПРАВКИ НАСТРОЕК ЦЕЛИКОМ: модель (ZConfigFile), вид (JsonEditView) и
// контроллер вместе — так же, как их связывает окно.
//
// Что спрашивается: конфига нет — открылся шаблон; Tab ставит пробелы до стопа
// editor.tabWidth, Enter держит отступ, Ctrl+/ комментирует и снимает
// комментарий (строка, выделение, смешанное), Ctrl+S пишет файл, Esc пишет и
// закрывает режим, undo/redo работают в рамках сеанса, каретка того же цвета и
// толщины, что в заметке, и правленый конфиг ПРИМЕНЯЕТСЯ (это делает окно —
// здесь проверяется через loadSettings своим каталогом настроек).

#include "config_file.h"
#include "editor_widget.h"
#include "json_edit_view.h"
#include "note_view.h"
#include "markdown_edit_view.h"
#include "settings.h"
#include "settings_controller.h"
#include "settings_hook.h"

#include "keys.h"
#include "test_util.h"
#include "testdata.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QApplication>
#include <QImage>
#include <QMenu>
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
    zametti::NoteEditor editor;
    zametti::JsonEditView view;
    zametti::SettingsController controller;

    explicit Rig(const QString& name)
        : path(QDir(g_dir).filePath(name)),
          model(std::make_shared<zametti::ZConfigFile>(path)),
          controller(editor, view, model) {
        editor.resize(700, 500);
        editor.show();
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

// Tab — пробелы до стопа editor.tabWidth (не знак табуляции), Shift+Tab
// снимает; Enter держит отступ предыдущей строки.
void checkTabAndEnter() {
    Rig rig(QStringLiteral("клавиши/config.json"));
    ZT_TRUE("вошли", rig.controller.enter());
    rig.view.setText(QStringLiteral("{\n    \"font\": {\n"), 0, 0);

    const int stop = zametti::settings().editor().tabWidth();
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
    rig.view.setText(QStringLiteral("{ \"fonts\": { \"noteSize\": 13 } }\n"), 0, 0);

    int savedOk = 0;
    QObject::connect(&rig.controller, &zametti::SettingsController::saved, &rig.view,
                     [&savedOk](bool ok, const QString&) { savedOk += ok ? 1 : 0; });
    ZT_TRUE("Ctrl+S записал", rig.controller.save());
    ZT_EQ("и сказал об этом сигналом", std::string("1"), std::to_string(savedOk));
    ZT_EQ("файл на диске", std::string("{ \"fonts\": { \"noteSize\": 13 } }\n"), readFile(rig.path));
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
    rig.view.setText(QStringLiteral("{\n  \"fonts\": { \"noteSize\": }\n}\n"), 0, 0);
    ZT_TRUE("записался", rig.controller.save());
    ZT_TRUE("файл на диске битый", readFile(rig.path).find("\"noteSize\": }") != std::string::npos);
    const zametti::ZConfigFile::Check check = zametti::ZConfigFile::check(rig.view.text());
    ZT_TRUE("и проверка о нём говорит", !check.ok && !check.error.isEmpty());
    ZT_EQ("со строкой", std::string("2"), std::to_string(check.line));
    rig.controller.leave();
}

// ПРАВЛЕНЫЙ КОНФИГ ПРИМЕНЯЕТСЯ. Настройки перечитываются своим каталогом
// (ZAMETTI_CONFIG_DIR набора), и вид берёт новый стоп табуляции.
void checkAppliedAfterSave() {
    QTemporaryDir home;
    const QByteArray previousHome = qgetenv(zametti::kConfigDirVar);
    qputenv(zametti::kConfigDirVar, home.path().toLocal8Bit());

    zametti::NoteEditor editor;
    zametti::JsonEditView view;
    auto model = std::make_shared<zametti::ZConfigFile>(zametti::configPath());
    zametti::SettingsController controller(editor, view, model);
    view.resize(700, 500);
    view.show();
    QTest::qWait(20);
    ZT_TRUE("вошли", controller.enter());
    view.setText(QStringLiteral("{ \"editor\": { \"tabWidth\": 8 } }\n"), 0, 0);
    ZT_TRUE("записали", controller.save());

    QString error;
    ZT_TRUE("настройки перечитаны: " + error.toStdString(), zametti::loadSettings(&error));
    ZT_EQ("новый стоп в настройках", std::string("8"),
          std::to_string(zametti::settings().editor().tabWidth()));
    view.refreshAppearance();
    view.setText(QStringLiteral("x\n"), 0, 0);
    QTest::keyClick(&view, Qt::Key_Tab);
    ZT_EQ("и Tab ставит восемь пробелов", std::string(8, ' ') + "x",
          view.document()->findBlockByNumber(0).text().toStdString());
    controller.leave();

    // Настройки и каталог возвращаем как были: наборы идут одним процессом.
    zametti::mutableSettingsForTests() = zametti::ZSettings{};
    qputenv(zametti::kConfigDirVar, previousHome);
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

// ШРИФТ У ПРАВКИ КОНФИГА — ТОТ ЖЕ, ЧТО У ИСХОДНИКА, И СВОЕЙ НАСТРОЙКИ У НЕГО
// НЕТ (решение владельца): гарнитура кода и кегль текста, оба из стиля
// документа. Спрашивается ДЕЙСТВИЕМ — сравнением двух живых видов при одном
// масштабе, а не чтением кода: заведи кто-нибудь `jsonEditing.fontFamily`,
// набор покраснеет.
void checkFontMatchesSourceMode() {
    Rig rig(QStringLiteral("шрифт/config.json"));
    ZT_TRUE("вошли", rig.controller.enter());
    zametti::MarkdownEditView source;
    source.resize(700, 500);
    source.show();
    QTest::qWait(20);

    ZT_EQ("гарнитура та же", source.font().family().toStdString(),
          rig.view.font().family().toStdString());
    ZT_EQ("кегль тот же", std::to_string(source.font().pointSizeF()),
          std::to_string(rig.view.font().pointSizeF()));
    ZT_EQ("и это гарнитура кода из стиля",
          zametti::settings().style().codeFamily().toStdString(),
          rig.view.font().family().toStdString());
    // Кегль у плоских видов СВОЙ (fonts.monospaceSize), а не кегль заметки:
    // здесь читают колонками, и согласовать моноширинный с основным шрифтом —
    // дело человека.
    ZT_EQ("а кегль — кегль моноширинного",
          std::to_string(zametti::settings().style().monospacePoint()),
          std::to_string(rig.view.font().pointSizeF()));

    // И под масштабом: число у плоских видов одно на двоих (его ставит окно,
    // см. applyZoom в main.cpp), а кегль из него выводится одинаково.
    rig.view.applyZoom(2.0);
    source.applyZoom(2.0);
    ZT_EQ("под масштабом тоже сходятся", std::to_string(source.font().pointSizeF()),
          std::to_string(rig.view.font().pointSizeF()));
    rig.view.applyZoom(1.0);
    rig.controller.leave();
}

// ОТКРЫЛИ ЗАМЕТКУ — РЕЖИМ ЗАКРЫЛСЯ, А ПРАВКА КОНФИГА ЗАПИСАНА (решение
// владельца): щелчок по заметке в дереве просит показать заметку, а не конфиг.
void checkNoteOpeningLeavesMode() {
    Rig rig(QStringLiteral("уход/config.json"));
    const QString note = QDir(g_dir).filePath(QStringLiteral("уход/заметка.md"));
    {
        QDir().mkpath(QFileInfo(note).absolutePath());
        QFile file(note);
        ZT_TRUE("заметка записана", file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write("# Заголовок\n\nтекст\n");
    }
    ZT_TRUE("вошли", rig.controller.enter());
    rig.view.setText(QStringLiteral("{ \"fonts\": { \"noteSize\": 14 } }\n"), 0, 0);

    rig.editor.openFile(note);
    QTest::qWait(20);
    ZT_TRUE("режим закрылся сам", !rig.controller.active());
    ZT_EQ("и правка конфига записана", std::string("{ \"fonts\": { \"noteSize\": 14 } }\n"),
          readFile(rig.path));
    ZT_TRUE("заметка открыта", rig.editor.filePath() == note);
}

// ЦВЕТ ТЕКСТА В ВЫДЕЛЕНИИ — НАСТРОЙКА (просьба владельца, refactor3):
// colors.selectionForeground. Прозрачный (умолчание) значит «выведи сам» —
// обычный цвет текста, как было до неё; заданный уходит в палитру, а её
// спрашивают ВСЕ: заметка, плоские виды, списки заметок и находок, поля
// подписи и языка. Проверяется действием — палитрой живого вида.
void checkSelectionForeground() {
    // УМОЛЧАНИЯ ЦВЕТОВ ПРИНАДЛЕЖАТ ВЛАДЕЛЬЦУ, и утверждать их здесь нельзя:
    // проверяется ПРАВИЛО (прозрачный — «выведи сам», заданный — берётся), а
    // не то, каким сегодня выбран цвет. Первая редакция набора требовала
    // прозрачного умолчания и покраснела, стоило владельцу выбрать свой цвет.
    zametti::ZDocStyle look = zametti::settings().style();

    zametti::JsonEditView view;
    view.resize(400, 200);
    view.show();
    QTest::qWait(20);

    look.setSelectionForeground(QColor(0, 0, 0, 0));   // прозрачный — «выведи сам»
    zametti::applyPalette(view, /*history=*/false, look);
    ZT_EQ("без настройки текст выделения — цвет текста",
          view.palette().color(QPalette::Text).name().toStdString(),
          view.palette().color(QPalette::HighlightedText).name().toStdString());

    look.setSelectionForeground(QColor(0x20, 0x40, 0x90));
    zametti::applyPalette(view, /*history=*/false, look);
    ZT_EQ("заданный цвет уходит в палитру", std::string("#204090"),
          view.palette().color(QPalette::HighlightedText).name().toStdString());

    // И то же правило одним местом — для тех, кто ставит палитру сам (список
    // слепков истории).
    ZT_EQ("правило одно на всех", std::string("#204090"),
          zametti::selectedTextColour(look, view.palette()).name().toStdString());
    zametti::ZDocStyle plain = zametti::settings().style();
    plain.setSelectionForeground(QColor(0, 0, 0, 0));
    ZT_EQ("и с прозрачным — цвет текста палитры",
          view.palette().color(QPalette::Text).name().toStdString(),
          zametti::selectedTextColour(plain, view.palette()).name().toStdString());
}

// ЦВЕТА ВЫДЕЛЕНИЯ ДЕЙСТВУЮТ И В КОНТЕКСТНЫХ МЕНЮ (просьба владельца): они
// рисуются палитрой ПРИЛОЖЕНИЯ, и без неё пункт подсвечивался бы системным
// синим при жёлтом выделении в заметке. Спрашивается действием — палитрой
// свежесозданного меню, а не чтением кода.
void checkMenuFollowsSelectionColours() {
    const QPalette before = QApplication::palette();

    zametti::ZDocStyle look = zametti::settings().style();
    look.setSelectionBackground(QColor(0x2b, 0x6c, 0xb0));
    look.setSelectionForeground(QColor(0xff, 0xff, 0xff));
    zametti::applySelectionPaletteToApp(look);

    QMenu menu;
    menu.addAction(QStringLiteral("пункт"));
    ZT_EQ("фон подсветки меню — цвет выделения", std::string("#2b6cb0"),
          menu.palette().color(QPalette::Highlight).name().toStdString());
    ZT_EQ("текст подсветки меню — цвет текста выделения", std::string("#ffffff"),
          menu.palette().color(QPalette::HighlightedText).name().toStdString());
    ZT_EQ("и у неактивной группы тоже", std::string("#2b6cb0"),
          menu.palette().color(QPalette::Inactive, QPalette::Highlight).name().toStdString());

    // Прозрачный selectionForeground и тут значит «выведи сам».
    look.setSelectionForeground(QColor(0, 0, 0, 0));
    zametti::applySelectionPaletteToApp(look);
    QMenu plainMenu;
    ZT_EQ("без настройки — обычный цвет текста",
          plainMenu.palette().color(QPalette::Text).name().toStdString(),
          plainMenu.palette().color(QPalette::HighlightedText).name().toStdString());

    // Палитра приложения — общая на процесс: возвращаем как было, наборы идут
    // одним процессом.
    QApplication::setPalette(before);
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
    checkFontMatchesSourceMode();
    checkNoteOpeningLeavesMode();
    checkSelectionForeground();
    checkMenuFollowsSelectionColours();
}

// ЯРЛЫК ОКНА СЪЕДАЕТ Esc (дефект, найденный ревью refactor3). В живом окне
// Esc — QShortcut на окне (main.cpp), а он срабатывает РАНЬШЕ, чем нажатие
// доходит до виджета с фокусом: значит keyPressEvent вида его не видит вовсе,
// и обещанный README выход из режима по Esc не работал бы. Порядок решает
// escapeActionFor — одно место на все режимы (там же ловится Esc у поля языка
// и панели поиска).
namespace {

void checkEscapeGoesThroughWindowShortcut() {
    // Порядок в самой функции: выйти из правки настроек — после того, как
    // закрылось всё, что открыто ПОВЕРХ текста.
    using zametti::EscapeAction;
    ZT_TRUE("правка настроек закрывается, когда больше нечего закрывать",
            zametti::escapeActionFor(false, false, false, true) == EscapeAction::LeaveMode);
    ZT_TRUE("панель поиска раньше неё",
            zametti::escapeActionFor(false, false, true, true) == EscapeAction::CloseFindBar);
    ZT_TRUE("поле языка раньше всех",
            zametti::escapeActionFor(true, false, true, true) == EscapeAction::CloseLanguageEditor);
    ZT_TRUE("без правки настроек — как было",
            zametti::escapeActionFor(false, false, false, false) == EscapeAction::Nothing);
}

}  // namespace

TEST(SettingsEscape, All) { checkEscapeGoesThroughWindowShortcut(); }
