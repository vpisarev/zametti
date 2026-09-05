// ДОСТУПНОСТЬ КНОПОК ТУЛБАРА: правило и — главное — ПРОВОДКА.
//
// Беда владельца (27.08.2026): «после просмотра помощи тулбар не
// восстанавливается, многие кнопки по-прежнему задизаблены». Правило было
// верным; не звали пересчёт. Поэтому набор устроен в два слоя, и второй важнее
// первого:
//
//   1. ПРАВИЛО — перебором, без единого виджета: для каждого состояния мира
//      названы ВСЕ погашенные кнопки, и всё прочее обязано гореть. Завели новую
//      кнопку — набор скажет, что про неё не подумали.
//   2. ПРОВОДКА — живой стенд: Toolbar, NoteEditor, ReaderView и контроллер.
//      И железное условие: после того как контроллер построен, набор НИ РАЗУ не
//      зовёт refresh() руками. Всё обязано приезжать сигналами — забытая
//      подписка и была бедой.
//
// Здесь же прикалывается ловушка, найденная по дороге: пересчёт не имеет права
// затирать живые подписи кнопок (у сортировок они говорят про направление).

#include "editor_widget.h"
#include "reader_view.h"
#include "resources.h"
#include "test_util.h"
#include "testdata.h"
#include "toolbar.h"
#include "toolbar_controller.h"
#include "toolbar_state.h"
#include "zapp.h"
#include "zstorage.h"

#include <QApplication>
#include <QDir>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>

#include <memory>
#include <set>
#include <string>
#include <vector>

namespace {

using zametti::ReaderView;
using zametti::Toolbar;
using zametti::ToolbarController;
using zametti::ToolbarState;
using zametti::ZStorage;
using Button = zametti::Toolbar::Button;

std::string s(const QString& q) { return q.toStdString(); }

zametti::ZJournal::Rules rules() { return zametti::ZJournal::Rules{}; }

QString nameOf(Button id) {
    for (const Toolbar::Spec& spec : Toolbar::specs())
        if (spec.id == id) return QString::fromUtf8(spec.icon);
    return QStringLiteral("?");
}

// Кто погашен правилом при этом состоянии мира.
std::set<int> darkOf(const ToolbarState& state) {
    std::set<int> dark;
    for (const Toolbar::Spec& spec : Toolbar::specs())
        if (!zametti::toolbarPromiseFor(spec.id, state).isEmpty()) dark.insert(int(spec.id));
    return dark;
}

// Кто погашен НА САМОМ ВИДЖЕТЕ. Спрашиваем то же, что видит человек.
std::set<int> darkOnBar(const Toolbar& bar) {
    std::set<int> dark;
    for (const Toolbar::Spec& spec : Toolbar::specs())
        if (!bar.isEnabled(spec.id)) dark.insert(int(spec.id));
    return dark;
}

std::string listOf(const std::set<int>& ids) {
    QStringList names;
    for (int id : ids) names << nameOf(Button(id));
    return s(names.join(QStringLiteral(", ")));
}

// Ожидание: ровно эти кнопки погашены, и ни одной больше.
void expectDark(const std::string& what, const std::set<int>& got, const std::vector<Button>& want) {
    std::set<int> wanted;
    for (Button id : want) wanted.insert(int(id));
    ZT_EQ(what, listOf(wanted), listOf(got));
}

}  // namespace

// --- 1. ПРАВИЛО --------------------------------------------------------------

static int ztRunRule() {
    // Без хранилища горит одна кнопка — «открыть хранилище». Пустое окно
    // обязано подсказывать само (решение владельца).
    {
        ToolbarState state;
        state.store = false;
        std::vector<Button> all;
        for (const Toolbar::Spec& spec : Toolbar::specs())
            if (spec.id != Button::OpenStore) all.push_back(spec.id);
        expectDark("без хранилища погашено всё, кроме «открыть хранилище»", darkOf(state), all);
        ZT_EQ("и причина названа словами", "no storage is open",
              s(zametti::toolbarPromiseFor(Button::NewNote, state)));
    }

    // Обычная заметка в открытом хранилище: не погашено ничего, кроме облака
    // без настроенного синка.
    {
        ToolbarState state;
        state.store = true;
        state.cloudConfigured = true;
        expectDark("на обычной заметке горят все кнопки", darkOf(state), {});
    }

    // ДОКУМЕНТАЦИЯ. Гаснет то, что ПИШЕТ, плюс история — её у вшитого
    // документа нет вовсе. Вывоз, поиск, сортировки, панели, настройки и
    // справка обязаны гореть: читать и уносить прочитанное никто не запрещал.
    {
        ToolbarState state;
        state.store = true;
        state.cloudConfigured = true;
        state.documentation = true;
        // The reading mode too (brief 18): the documentation has a page of its own.
        expectDark("на документации гаснут шесть кнопок", darkOf(state),
                   {Button::InsertImages, Button::History, Button::Lock, Button::Reading,
                    Button::Toc, Button::MarkdownEdit});
        ZT_EQ("и причина говорит про документацию", "documentation is read-only",
              s(zametti::toolbarPromiseFor(Button::MarkdownEdit, state)));
    }

    // ЗАПЕРТАЯ заметка (`access: read-only`) и АРХИВНАЯ: история у них
    // работает как у всякой другой — смотреть прошлое не значит его менять.
    for (int kind = 0; kind < 2; ++kind) {
        ToolbarState state;
        state.store = true;
        state.cloudConfigured = true;
        (kind == 0 ? state.readOnlyNote : state.archivedNote) = true;
        const std::string what = kind == 0 ? "на запертой" : "на архивной";
        // The lock button too: a frozen or archived note is not unlocked by it.
        // And the reading mode on an archived note: the archive page shows it
        // as it was; a frozen note reads as a book like any other.
        if (kind == 0)
            expectDark(what + " гаснут три кнопки", darkOf(state),
                       {Button::InsertImages, Button::Lock, Button::MarkdownEdit});
        else
            expectDark(what + " гаснут четыре кнопки", darkOf(state),
                       {Button::InsertImages, Button::Lock, Button::Reading, Button::MarkdownEdit});
        ZT_EQ(what + " причина говорит про заметку", "this note is read-only",
              s(zametti::toolbarPromiseFor(Button::InsertImages, state)));
        ZT_TRUE(what + " история горит",
                zametti::toolbarPromiseFor(Button::History, state).isEmpty());
    }

    // THE SOFT LOCK (brief 18): a locked note is edited by the program, so
    // nothing goes dark — the lock button itself lights, whatever the state
    // of the lock. A BOOK hides its history (owner's decision, 05.09.2026).
    {
        ToolbarState state;
        state.store = true;
        state.cloudConfigured = true;
        state.lockedNote = true;
        expectDark("на запертой мягким замком не гаснет ничего", darkOf(state), {});
        state.tempUnlocked = true;
        expectDark("на временно открытой — тоже", darkOf(state), {});
        state.book = true;
        expectDark("у книги гаснет история", darkOf(state), {Button::History});
        ZT_EQ("и причина говорит про книгу", "a book keeps its journal, but shows no history",
              s(zametti::toolbarPromiseFor(Button::History, state)));
    }

    // Облако: не настроен синк — кнопка гаснет, и причина это его же слова.
    {
        ToolbarState state;
        state.store = true;
        state.cloudConfigured = false;
        state.cloudStatus = QStringLiteral("cloud is not set up");
        expectDark("ненастроенное облако гаснет", darkOf(state), {Button::Cloud});
        ZT_EQ("причина — слова самого синка", "cloud is not set up",
              s(zametti::toolbarPromiseFor(Button::Cloud, state)));
    }

    // ПОГАШЕННЫХ БЕЗ ПРИЧИНЫ НЕ БЫВАЕТ: серая кнопка молча читается как
    // поломка. Спрашиваем на всех состояниях сразу.
    for (int mask = 0; mask < 128; ++mask) {
        ToolbarState state;
        state.store = (mask & 1) != 0;
        state.documentation = (mask & 2) != 0;
        state.readOnlyNote = (mask & 4) != 0;
        state.archivedNote = (mask & 8) != 0;
        state.lockedNote = (mask & 16) != 0;
        state.tempUnlocked = (mask & 32) != 0;
        state.book = (mask & 64) != 0;
        state.cloudConfigured = true;
        for (const Toolbar::Spec& spec : Toolbar::specs()) {
            const QString why = zametti::toolbarPromiseFor(spec.id, state);
            if (why.isEmpty()) continue;
            ZT_TRUE(std::string("причина непуста у ") + s(nameOf(spec.id)), !why.trimmed().isEmpty());
        }
        ZT_TRUE("«открыть хранилище» горит всегда",
                zametti::toolbarPromiseFor(Button::OpenStore, state).isEmpty());
    }

    return zt::report("правило тулбара");
}

// --- 2. ПРОВОДКА -------------------------------------------------------------

static int ztRunWiring() {
    QTemporaryDir home;
    const QString root = home.path() + QStringLiteral("/store");
    QString error;
    ZT_TRUE("хранилище заведено", ZStorage(root).init(&error));

    auto storage = std::make_shared<ZStorage>(root);
    storage->reload();
    zametti::ZApp::instance().addInfoFolder(*storage);

    const QString plain = storage->createNote(QString(), false, &error);
    const QString locked = storage->createNote(QString(), false, &error);
    ZT_TRUE("заметки созданы", !plain.isEmpty() && !locked.isEmpty());
    ZT_TRUE("одна из них заперта", storage->setReadOnly(locked, true, rules(), &error));
    storage->reload();

    Toolbar bar;
    zametti::NoteEditor editor;
    editor.setStorage(storage);
    ReaderView docs(nullptr, ReaderView::Tint::Plain);

    // Читатель — как в окне: сам смотрит на живые объекты.
    ToolbarController control(bar, editor, docs, [&] {
        ToolbarState state;
        state.store = true;
        state.documentation = !docs.path().isEmpty();
        state.readOnlyNote = editor.isReadOnlyNote();
        state.archivedNote = editor.isArchivedNote();
        state.cloudConfigured = true;
        return state;
    });

    // ДАЛЬШЕ refresh() РУКАМИ НЕ ЗОВЁТСЯ НИ РАЗУ. Всё, что ниже, обязано
    // приехать сигналами — забытая подписка и была бедой владельца.

    editor.openFile(storage->pathOf(plain));
    QCoreApplication::processEvents();
    expectDark("на обычной заметке горят все кнопки", darkOnBar(bar), {});

    editor.openFile(storage->pathOf(locked));
    QCoreApplication::processEvents();
    expectDark("на запертой гаснут три", darkOnBar(bar),
               {Button::InsertImages, Button::Lock, Button::MarkdownEdit});
    ZT_EQ("и причина написана в подсказке", "this note is read-only",
          s(bar.promiseFor(Button::MarkdownEdit)));

    // ВОТ ОНА, БЕДА: вернулись к обычной заметке — кнопки обязаны загореться
    // САМИ. Со снятой подпиской на fileChanged здесь и краснеет.
    editor.openFile(storage->pathOf(plain));
    QCoreApplication::processEvents();
    expectDark("вернулись к обычной — всё горит снова", darkOnBar(bar), {});

    // ДОКУМЕНТАЦИЯ: показали — погасло три; убрали — загорелись сами. Это
    // дословно случай владельца («после просмотра помощи»).
    const QStringList docFiles = zametti::embeddedDocs();
    ZT_TRUE("вшитая документация есть", !docFiles.isEmpty());
    if (!docFiles.isEmpty()) {
        docs.showFile(docFiles.first(), QStringLiteral("info:probe"));
        QCoreApplication::processEvents();
        expectDark("на документации гаснут шесть", darkOnBar(bar),
                   {Button::InsertImages, Button::History, Button::Lock, Button::Reading,
                    Button::Toc, Button::MarkdownEdit});
        ZT_EQ("и причина про документацию", "documentation is read-only",
              s(bar.promiseFor(Button::History)));

        // Так это и происходит в окне: страница чтения закрывается, заметка
        // открывается заново.
        docs.clear();
        editor.openFile(storage->pathOf(plain));
        QCoreApplication::processEvents();
        expectDark("после документации тулбар восстановился", darkOnBar(bar), {});
    }

    // СИММЕТРИЯ «переключился-вернулся == открыл заново» — обязательная
    // проверка проекта: состояние кнопок после A→B→A обязано совпасть с тем,
    // что было после свежего открытия A.
    {
        editor.openFile(storage->pathOf(plain));
        QCoreApplication::processEvents();
        const std::set<int> fresh = darkOnBar(bar);
        editor.openFile(storage->pathOf(locked));
        QCoreApplication::processEvents();
        editor.openFile(storage->pathOf(plain));
        QCoreApplication::processEvents();
        ZT_EQ("возврат к заметке даёт то же, что свежее открытие", listOf(fresh),
              listOf(darkOnBar(bar)));
    }

    // НАЖАТОСТЬ — ДРУГАЯ ОСЬ. Режим правки исходника принадлежит приложению, а
    // не заметке (решение владельца): пересчёт доступности не имеет права его
    // снимать.
    bar.setChecked(Button::MarkdownEdit, true);
    editor.openFile(storage->pathOf(locked));
    QCoreApplication::processEvents();
    ZT_TRUE("смена заметки не снимает нажатую кнопку режима",
            bar.isChecked(Button::MarkdownEdit));
    bar.setChecked(Button::MarkdownEdit, false);

    // ЖИВАЯ ПОДПИСЬ ПЕРЕЖИВАЕТ ПЕРЕСЧЁТ. У сортировок подпись говорит про
    // НАПРАВЛЕНИЕ, и пересчёт, собирающий подпись заново из таблицы, стирал бы
    // её на каждой смене заметки.
    const QString own = QStringLiteral("By created date, oldest first\ncommon order");
    bar.setTip(Button::SortByCreated, own);
    editor.openFile(storage->pathOf(plain));
    QCoreApplication::processEvents();
    {
        QToolButton* button = bar.buttonFor(Button::SortByCreated);
        ZT_TRUE("кнопка сортировки на месте", button != nullptr);
        if (button != nullptr)
            ZT_TRUE("живая подпись пережила пересчёт: " + s(button->toolTip()),
                    button->toolTip().contains(QStringLiteral("oldest first")));
    }

    return zt::report("проводка тулбара");
}

TEST(ToolbarState, All) {
    EXPECT_EQ(0, ztRunRule());
    EXPECT_EQ(0, ztRunWiring());
}
