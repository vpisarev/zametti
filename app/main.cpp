// Просмотрщик: дерево заметок слева, отрендеренный документ справа.
//
// Это не самоцель, а первый стенд для проверки ядра. Отсюда же работает режим
// --check: прогнать parse → serialize и показать расхождение с оригиналом.

#include "doc_model.h"
#include "editor_widget.h"
#include "key_binding.h"
#include "formula.h"
#include "find_bar.h"
#include "history_controller.h"
#include "markdown_controller.h"
#include "history_panel.h"
#include "history_view.h"
#include "markdown_edit_view.h"
#include "note_list.h"
#include "note_panels.h"
#include "note_tree.h"
#include "search.h"
#include "search_results.h"
#include "store_search.h"
#include "journal.h"
#include "store.h"
#include "note_view.h"
#include "resources.h"
#include "document.h"
#include "znote.h"
#include "serializer.h"
#include "settings.h"
#include "sort_order.h"
#include "archive.h"
#include "lost_found.h"
#include "about_window.h"
#include "export_note.h"
#include "export_pdf.h"
#include "status_bar.h"
#include "toolbar.h"
#include "zapp.h"

#include <QApplication>
#include <QFileInfo>
#include <QFont>
#include <QIcon>
#include <QItemSelectionModel>
#include <QKeySequence>
#include <QStandardPaths>
#include <QListView>
#include <QMenu>
#include <QKeyEvent>
#include <QCheckBox>
#include <QGridLayout>
#include <QFileDialog>
#include <QMessageBox>
#include <QDesktopServices>
#include <QDateTime>
#include <QSysInfo>
#include <QProcess>
#include <QThreadPool>
#include <QUrl>
#include <QPushButton>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStackedWidget>
#include <QTextBlock>
#include <QTextCursor>
#include <QFile>
#include <QElapsedTimer>
#include <QTimer>
#include <QTreeView>
#include <QPointer>
#include <QFileSystemWatcher>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <functional>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace {

bool readFile(const QString& path, std::string& out) {
    std::ifstream in(path.toStdString(), std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

std::vector<std::string> splitLines(const std::string& s) {
    std::vector<std::string> lines;
    size_t pos = 0;
    while (pos <= s.size()) {
        size_t e = s.find('\n', pos);
        if (e == std::string::npos) {
            if (pos < s.size()) lines.push_back(s.substr(pos));
            break;
        }
        lines.push_back(s.substr(pos, e - pos));
        pos = e + 1;
        if (pos == s.size()) break;
    }
    return lines;
}

// Построчный дифф. Не «первый различающийся байт»: расхождение надо читать
// глазами, а не вычислять.
int runCheck(const QString& path) {
    std::string src;
    if (!readFile(path, src)) {
        std::fprintf(stderr, "не читается: %s\n", path.toUtf8().constData());
        return 2;
    }

    // Круг файла целиком — шапка и тело: его держит заметка, не документ.
    zametti::ZNote note;
    note.load(src);
    const std::string out = note.toMarkdown();
    if (out == src) return 0;

    std::vector<std::string> a = splitLines(src);
    std::vector<std::string> b = splitLines(out);
    size_t n = a.size() > b.size() ? a.size() : b.size();
    for (size_t i = 0; i < n; ++i) {
        const std::string* ea = i < a.size() ? &a[i] : nullptr;
        const std::string* eb = i < b.size() ? &b[i] : nullptr;
        if (ea != nullptr && eb != nullptr && *ea == *eb) continue;
        if (ea != nullptr) std::printf("%5zu - %s\n", i + 1, ea->c_str());
        if (eb != nullptr) std::printf("%5zu + %s\n", i + 1, eb->c_str());
    }
    return 1;
}


const char* kUsage =
    "использование: zametti [--noconfig] [файл.md]\n"
    "               zametti --root каталог-хранилища\n"
    "               zametti --check файл.md\n"
    "               zametti --dump-config\n"
    "               zametti --root каталог-хранилища --unlock\n";

void printUsage() { std::fputs(kUsage, stderr); }

// Справка идёт в stdout и с нулевым кодом: её просят намеренно, это не ошибка.
// Сочетаний на команду может быть несколько; в конфиге они через точку с
// запятой, а человеку читать удобнее через запятую.
QByteArray keysFor(const QString& keys) {
    if (keys.trimmed().isEmpty()) return QByteArrayLiteral("(нет)");   // сочетание убрано
    return QString(keys).replace(QStringLiteral("; "), QStringLiteral(", ")).toUtf8();
}

QByteArray padFor(const QString& keys) {
    return QByteArray(qMax(0, 16 - int(keysFor(keys).size())), ' ');
}

void printHelp() {
    std::fputs(kUsage, stdout);
    std::printf(
        "\n"
        "Просмотрщик заметок в markdown. Слева дерево заметок, справа документ.\n"
        "Без имени файла открывается тот, что читали в прошлый раз.\n"
        "\n"
        "Хранилище (--root):\n"
        "  Ctrl+N            новая заметка (ребёнок выбранной в дереве)\n"
        "  F2                переименовать заметку (правит её первый заголовок)\n"
        "  Del в дереве      в корзину; в корзине — насовсем, с подтверждением\n"
        "  перетаскивание    перенос заметки; каталог — это заметка с детьми\n"
        "\n"
        "Ключи:\n"
        "  --check файл.md   прогнать разбор и обратную запись, показать расхождение\n"
        "                    с оригиналом; ненулевой код возврата при расхождении.\n"
        "                    Дисплей не нужен\n"
        "  --dump-config     напечатать все параметры оформления со значениями\n"
        "                    по умолчанию, в том же виде, в каком их ждёт конфиг\n"
        "  --noconfig        не читать конфиг, взять умолчания\n"
        "  --help, -h        эта справка\n"
        "\n"
        "Клавиши:\n"
        "  Ctrl+=, Ctrl+-    крупнее, мельче\n"
        "  Ctrl+0            исходный масштаб\n"
        "  Ctrl+S            сохранить сейчас\n"
        "  Ctrl+Z, Ctrl+Y    отменить, вернуть\n"
        "  Enter             в тексте — перенос строки, второй подряд — новый абзац;\n"
        "                    в списке — новый пункт, на пустом пункте выйти из него\n"
        "  Shift+Enter       наоборот: в тексте новый абзац, в пункте перенос строки\n"
        "  Backspace         в начале пункта — сделать его абзацем\n"
        "  Tab, Shift+Tab    двигать пункт по уровням вложенности\n"
        "  %s%s  переключить задачу: сделана или нет\n"
        "  %s%s  переставить пункт вверх\n"
        "  %s%s  переставить пункт вниз\n"
        "  %s%s  сделать маркированным списком\n"
        "  %s%s  сделать нумерованным списком\n"
        "  %s%s  сделать списком задач\n"
        "  %s%s  сделать обычным текстом\n"
        "  Ctrl+B, Ctrl+I    жирный, курсив\n"
        "  Ctrl+/            зачёркнутый\n"
        "  Ctrl+E            код в строке; он же выходит из кавычек при наборе\n"
        "  Ctrl+Shift+E      выделенное в блок кода и обратно в текст\n"
        "  %s%s  править исходник markdown (и обратно)\n"
        "\n"
        "Файлы:\n"
        "  %s\n"
        "      оформление; приложение его только читает, править вручную.\n"
        "      Полный список параметров — по ключу --dump-config\n"
        "  %s\n"
        "      последняя заметка, прокрутка, зум, геометрия окна, раскрытые ветки;\n"
        "      переписывается при выходе\n",
        keysFor(zametti::settings().editor().toggleTaskKey()).constData(),
        padFor(zametti::settings().editor().toggleTaskKey()).constData(),
        keysFor(zametti::settings().editor().moveUpKey()).constData(),
        padFor(zametti::settings().editor().moveUpKey()).constData(),
        keysFor(zametti::settings().editor().moveDownKey()).constData(),
        padFor(zametti::settings().editor().moveDownKey()).constData(),
        keysFor(zametti::settings().editor().makeBulletKey()).constData(),
        padFor(zametti::settings().editor().makeBulletKey()).constData(),
        keysFor(zametti::settings().editor().makeOrderedKey()).constData(),
        padFor(zametti::settings().editor().makeOrderedKey()).constData(),
        keysFor(zametti::settings().editor().makeTaskKey()).constData(),
        padFor(zametti::settings().editor().makeTaskKey()).constData(),
        keysFor(zametti::settings().editor().makeParagraphKey()).constData(),
        padFor(zametti::settings().editor().makeParagraphKey()).constData(),
        keysFor(zametti::settings().editor().markdownModeKey()).constData(),
        padFor(zametti::settings().editor().markdownModeKey()).constData(),
        zametti::configPath().toUtf8().constData(),
        zametti::ZAppState::path().toUtf8().constData());
}

}  // namespace

int main(int argc, char** argv) {
    // Имя приложения задаём до разбора ключей: от него зависят пути к конфигу и
    // состоянию, а их печатает --help, не создавая ни окна, ни QApplication.
    QCoreApplication::setApplicationName(QStringLiteral("zametti"));

    QString path;
    QString storeRoot;
    bool check = false;
    bool dumpConfig = false;
    bool noConfig = false;
    bool unlock = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            printHelp();
            return 0;
        }
        if (arg == "--check") check = true;
        else if (arg == "--dump-config") dumpConfig = true;
        else if (arg == "--noconfig") noConfig = true;
        else if (arg == "--unlock") unlock = true;
        else if (arg == "--root" && i + 1 < argc) {
            storeRoot = QString::fromLocal8Bit(argv[++i]);
        }
        else if (arg.rfind("--", 0) == 0) {
            std::fprintf(stderr, "неизвестный ключ: %s\n", arg.c_str());
            return 2;
        } else {
            path = QString::fromLocal8Bit(argv[i]);
        }
    }

    // --dump-config печатает готовый JSON и ни о чём Qt не спрашивает.
    if (dumpConfig) {
        const QByteArray json = zametti::defaultSettingsJson();
        std::fwrite(json.constData(), 1, size_t(json.size()), stdout);
        return 0;
    }

    // ДИСПЛЕЙ НУЖЕН ДАЖЕ --check. Приведение к канону идёт через сборку живого
    // документа, а сборщик спрашивает метрики шрифта, то есть требует
    // QGuiApplication. Раньше здесь стояло обещание «не требует дисплея», и
    // --check падал с core dump на первом же файле.
    //
    // zametti — программа с окном, и это нормально. Ненормально было падение:
    // в конвейере и в CI дисплея нет, поэтому под --check платформу задаём
    // сами. ТОЛЬКО под --check: обычному запуску платформу выбирает Qt, и
    // навязанный offscreen оставил бы человека без окна. Заданную снаружи не
    // перебиваем и здесь — иначе приёмочные снимки под Xvfb молча уехали бы в
    // offscreen.
    // Спрашиваем именно «пуста ли»: qEnvironmentVariableIsSet считает
    // установленной и ПУСТУЮ переменную, а пустая платформа для Qt не платформа
    // — он уходит в автоопределение и без дисплея падает. Поймал сторож
    // canon-without-display.
    if (check && qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");

    QApplication app(argc, argv);

    if (check) {
        if (path.isEmpty()) {
            printUsage();
            return 2;
        }
        // Шрифты — до сборки: сборщик берёт метрики у семейств из настроек, и
        // без влинкованных гарнитур Qt молча подставит свои.
        zametti::loadEmbeddedFonts();
        return runCheck(path);
    }
    // Оболочки рабочего стола (в том числе док GNOME) берут иконку не у окна, а
    // из .desktop-файла с этим именем — см. packaging/zametti.desktop.
    QGuiApplication::setDesktopFileName(QStringLiteral("zametti"));
    QGuiApplication::setWindowIcon(QIcon(QStringLiteral(":/zametti.png")));

    // Шрифты — до чтения конфига и до первого виджета: настройки называют
    // семейства по имени, и если имя некому отдать, Qt молча подставит своё.
    // Жалуемся, но работаем: без шрифта программа некрасива, а не мертва.
    for (const QString& face : zametti::loadEmbeddedFonts())
        std::fprintf(stderr, "влинкованный шрифт не принят Qt: %s\n",
                     face.toUtf8().constData());

    // Движок формул — здесь же: подъём стоит 22 мс, и они не должны достаться
    // первой формуле человека (прогревочный рендер внутри init). Не поднялся —
    // жалуемся и живём дальше: формулы покажутся рамкой ошибки, но заметки
    // читаются и правятся.
    {
        QString formulaError;
        if (!zametti::Formulas::init(&formulaError))
            std::fprintf(stderr, "движок формул не поднялся: %s\n",
                         formulaError.toUtf8().constData());
    }

    // --noconfig нужен, чтобы посмотреть на вид по умолчанию, не убирая свой
    // конфиг: удобно и при правке конфига, и при разговоре о том, «как оно
    // выглядит из коробки».
    // ОБЪЕКТ ПРИЛОЖЕНИЯ: настройки, состояние сеанса (единые ворота к state.json),
    // каретки по заметкам, кэш иконок. Один на процесс.
    zametti::ZApp zapp;
    QString configError;
    QStringList configUnknown;
    if (!noConfig && !zapp.reloadSettings(&configError, &configUnknown)) {
        // Молча подставить умолчания нельзя: опечатка в конфиге выглядела бы
        // как «настройка не работает».
        std::fprintf(stderr, "конфиг не разобран, взяты значения по умолчанию:\n  %s\n",
                     configError.toUtf8().constData());
    }

    const zametti::ZAppState& session = zapp.state();

    // Без аргумента открываем то, что читали в прошлый раз. С --root — свежую
    // заметку хранилища (или прошлую, если она из этого же хранилища).
    // Хранилище прошлого запуска запоминается: без параметров возвращаемся
    // в него, ключ --root каждый раз не нужен.
    if (path.isEmpty()) path = session.lastFile();
    if (storeRoot.isEmpty() && path.isEmpty() && !session.storeRoot().isEmpty() &&
        zametti::NoteTreeModel::isStoreRoot(session.storeRoot()))
        storeRoot = session.storeRoot();
    if (storeRoot.isEmpty() && !path.isEmpty() && !session.storeRoot().isEmpty() &&
        QFileInfo(path).absoluteFilePath().startsWith(
            QFileInfo(session.storeRoot()).absoluteFilePath()) &&
        zametti::NoteTreeModel::isStoreRoot(session.storeRoot()))
        storeRoot = session.storeRoot();
    if (!storeRoot.isEmpty()) {
        const QString absRoot = QFileInfo(storeRoot).absoluteFilePath();
        if (!zametti::NoteTreeModel::isStoreRoot(absRoot)) {
            std::fprintf(stderr, "не похоже на хранилище (нет .zametti): %s\n",
                         absRoot.toUtf8().constData());
            return 2;
        }
        if (path.isEmpty() || !QFileInfo(path).absoluteFilePath().startsWith(absRoot))
            path = QString();   // выберем свежую после построения дерева
    } else if (path.isEmpty()) {
        printUsage();
        return 2;
    }

    QString current = path.isEmpty() ? QString() : QFileInfo(path).absoluteFilePath();

    // Контейнеры объявлены ПЕРЕД теми виджетами, которых они усыновят через
    // layout: добавление в раскладку делает виджет ребёнком, а объекты на
    // стеке разрушаются в обратном порядке объявления. Контейнер, объявленный
    // позже ребёнка, умирал первым и удалял его вторым разом — приложение
    // падало при закрытии окна крестиком (замечено владельцем).
    //
    // Окном был сам сплиттер, пока не появился тулбар: полоса кнопок стоит над
    // всеми тремя панелями сразу, а не внутри одной из них. Отсюда оболочка —
    // самый внешний виджет, в ней сверху вниз тулбар и сплиттер.
    QWidget window;
    QSplitter splitter(Qt::Horizontal);
    zametti::Toolbar toolbar;
    zametti::StatusBar statusBar;
    QWidget rightSide;
    // Редактор и вид истории — на одном месте, по одному за раз: режим истории
    // показывает не редактор с подменённым документом, а свой вид, а редактор
    // с живой заметкой на это время просто скрыт (сессия 7). СТЕК ОБЪЯВЛЕН ДО
    // СВОИХ ДЕТЕЙ: addWidget делает его их родителем, а дети — объекты на
    // стеке, и умирать они обязаны раньше родителя (иначе тот удалит их сам —
    // двойное освобождение на выходе; так и вышло в первой примерке).
    QStackedWidget textStack;

    zametti::NoteEditor editor;

    // Хранилище открывает объект приложения; левая и средняя колонки — его
    // проекция (NotePanels: дерево папок, список заметок, показ открытой).
    zametti::NotePanels panels(zapp.openStorage(
        storeRoot.isEmpty()
            ? zametti::NoteTreeModel::rootFor(current, zametti::settings().store().notesRoot())
            : QFileInfo(storeRoot).absoluteFilePath()));
    zametti::NoteTreeModel& model = panels.model();
    zametti::NoteTreeView& tree = panels.tree();
    zametti::NoteListModel& list = panels.list();
    QListView& listView = panels.listView();

    // Редактор узнаёт своё хранилище: без него истории правок не будет вовсе
    // (одиночный файл, открытый вне хранилища, журналу негде лежать).
    editor.setStorage(model.isStore() ? zapp.storage() : nullptr);

    // Одно хранилище — одна программа: замок держит само хранилище
    // (ZStorage::lock, там же снятие забытого замка мёртвого процесса и
    // --unlock); здесь только слово человеку и код выхода.
    if (model.isStore()) {
        if (unlock)
            std::fprintf(stderr, "%s\n", zapp.storage()->forceUnlock().note.toUtf8().constData());
        const zametti::ZStorage::LockReport locked = zapp.storage()->lock();
        if (!locked.note.isEmpty())
            std::fprintf(stderr, "%s\n", locked.note.toUtf8().constData());
        if (!locked.locked) {
            std::fprintf(stderr,
                         "это хранилище уже открыто другой копией zametti:\n  %s\n"
                         "  замок держит pid %lld на «%s»\n"
                         "Если та копия давно умерла: zametti --root … --unlock\n",
                         model.nodePath(QModelIndex()).toUtf8().constData(),
                         (long long)locked.holderPid, locked.holderHost.toUtf8().constData());
            return 3;
        }
    }

    // РАЗОВАЯ МИГРАЦИЯ СТАРОЙ КОРЗИНЫ — здесь, сразу после замка и до того, как
    // дерево кто-нибудь увидит. Заметки из корзины переезжают в новый вид
    // (`archived: yes`, `parent` := прежний родитель), опустевшая
    // заметка-корзина уходит. Идемпотентно: корзины нет — не делает ничего, и
    // при каждом следующем запуске это просто один проход по каталогу.
    // Ленивые миграции хранилища — его дело; здесь только слово человеку и
    // перестройка дерева, если что-то переехало.
    if (model.isStore()) {
        // Дерево перестроится само: каталог перечитан — хранилище сказало.
        for (const QString& line : zapp.storage()->migrate())
            std::fprintf(stderr, "%s\n", line.toUtf8().constData());
    }

    // Свежая заметка хранилища — первая ОТКРЫВАЕМАЯ (директории не в счёт),
    // поиском в глубину; пустое хранилище получает первую заметку тут же.
    if (current.isEmpty()) {
        QString first = model.firstNoteId();
        if (first.isEmpty()) {
            QString newError;
            if (zapp.storage()->createNote(QString(), false, &newError).isEmpty()) {
                std::fprintf(stderr, "%s\n", newError.toUtf8().constData());
                return 2;
            }
            first = model.firstNoteId();
        }
        current = model.pathOfId(first);
        if (current.isEmpty()) {
            std::fprintf(stderr, "в хранилище нет ни одной открываемой заметки\n");
            return 2;
        }
    }
    // ПОРЯДОК КОРНЯ — переключатель интерфейса, а не метка в файле: корень не
    // заметка, писать метку некуда. Он же — запасной для всякой папки, у
    // которой нет ни своей метки, ни помеченного предка. Живёт в state.json и
    // не синхронизируется: у каждого устройства свой вкус по умолчанию.
    //
    // Старое состояние («name» / «modified», до этапа 13) читается тем же
    // разбором: ключ без направления — законная краткая запись.
    if (const auto saved = zametti::parseSortOrder(session.treeSort())) panels.setRootSort(*saved);

    QFont sidebarFont(zametti::settings().ui().sidebarFontFamily().isEmpty()
                          ? zametti::settings().style().fontFamily()
                          : zametti::settings().ui().sidebarFontFamily());
    sidebarFont.setPointSizeF(zametti::settings().ui().sidebarFontPoint());
    panels.setSidebarFont(sidebarFont);

    // Правая сторона — заметка, под ней список найденного (появляется только у
    // поиска по всему хранилищу) и панель поиска у самого низа, как в Sublime.
    zametti::FindBar findBar;
    zametti::HistoryView historyView;
    // Третья страница стека: та же заметка сырым markdown. Объявлен ПОСЛЕ
    // textStack, как и остальные его дети (см. довод у стека).
    zametti::MarkdownEditView markdownView;
    zametti::SearchResultsModel results;
    zametti::SearchResultsDelegate resultsDelegate;
    QListView resultsView;
    zametti::StoreSearch storeSearch;
    QTimer searchDebounce;
    {
        auto* layout = new QVBoxLayout(&rightSide);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);
        resultsView.setModel(&results);
        resultsView.setItemDelegate(&resultsDelegate);
        resultsView.setFont(sidebarFont);
        resultsView.setUniformItemSizes(false);
        resultsView.setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        resultsView.setMaximumHeight(240);
        resultsView.hide();

        // Режим истории: вид истории (баннер над разностью и списком записей)
        // НА МЕСТЕ редактора; вне режима его нет вовсе — тем режим и громкий.
        historyView.list().setFont(sidebarFont);
        textStack.addWidget(&editor);
        textStack.addWidget(&historyView);
        textStack.addWidget(&markdownView);
        textStack.setCurrentWidget(&editor);

        layout->addWidget(&textStack, 1);
        layout->addWidget(&resultsView);
        layout->addWidget(&findBar);
    }
    // Дебаунс по замеру этапа 4: полный проход по хранилищу — 11 мс тёплым и
    // 113 мс на десятикратном корпусе, так что 150 мс успевают проглотить
    // любой из них, а набор не тормозит.
    searchDebounce.setSingleShot(true);
    searchDebounce.setInterval(150);
    findBar.setHistory(session.searchHistory());
    zametti::HistoryController history(editor, historyView);
    zametti::MarkdownController markdown(editor, markdownView);

    // Облик применяется ОДНИМ местом — и на старте, и когда конфиг поправили
    // снаружи. Два места разошлись бы: половина настроек подхватывалась бы на
    // лету, половина только после перезапуска, и понять, какая именно, было бы
    // нельзя.
    const auto applyAppearance = [&] {
        QFont font(zametti::settings().ui().sidebarFontFamily().isEmpty()
                       ? zametti::settings().style().fontFamily()
                       : zametti::settings().ui().sidebarFontFamily());
        font.setPointSizeF(zametti::settings().ui().sidebarFontPoint());
        panels.setSidebarFont(font);
        resultsView.setFont(font);
        historyView.list().setFont(font);

        zametti::applyPalette(editor);
        panels.refreshAppearance();
        zametti::applyPalette(resultsView);

        toolbar.refreshAppearance();
        statusBar.refreshAppearance();
        editor.refreshAppearance();
        history.refreshAppearance();
        markdown.refreshAppearance();

        // Делегаты читают настройки прямо при отрисовке — им довольно
        // перерисовки, но размеры строк они считают там же, и без сброса
        // подсказок список остался бы с прежними высотами.
        resultsView.doItemsLayout();
    };
    applyAppearance();

    // Конфиг правят снаружи — своего окна настроек у нас нет. Следим за файлом
    // и перечитываем его сами: иначе после каждой правки цвета пришлось бы
    // перезапускать программу.
    //
    // Редакторы пишут конфиг «обрезать → записать» или через переименование, и
    // слежение при этом слетает: путь возвращаем обратно на каждом срабатывании.
    // Отстойник — от той же привычки: первый сигнал часто застаёт файл пустым
    // или недописанным, и разбирать его в этот миг значит ругаться на мусор.
    QFileSystemWatcher configWatcher;
    QTimer configSettle;
    configSettle.setSingleShot(true);
    configSettle.setInterval(300);
    const auto watchConfig = [&configWatcher] {
        const QString path = zametti::configPath();
        if (QFile::exists(path) && !configWatcher.files().contains(path))
            configWatcher.addPath(path);
    };
    QObject::connect(&configWatcher, &QFileSystemWatcher::fileChanged, &window,
                     [&](const QString&) {
                         watchConfig();
                         configSettle.start();
                     });
    QObject::connect(&configSettle, &QTimer::timeout, &window, [&] {
        watchConfig();
        // Несохранённое — на диск до перезагрузки облика: пересборка документа
        // проходит через всю модель, и терять правки на ней недопустимо.
        editor.save(true);

        QString error;
        QStringList unknown;
        if (!zametti::loadSettings(&error, &unknown)) {
            // Мусор в конфиге — это не повод перекрашивать окно наугад:
            // работаем на прежних значениях и говорим, что именно не так.
            std::fprintf(stderr, "конфиг не принят: %s\n", error.toUtf8().constData());
            // В полосе — причина, а не путь. Путь длиннее всей полосы, и в
            // первом же снимке многоточие съело ровно то, ради чего сообщение
            // и показывают: «object is missing after a comma».
            QString why = error;
            const QString prefix = zametti::configPath() + QStringLiteral(": ");
            if (why.startsWith(prefix)) why = why.mid(prefix.size());
            statusBar.setMessage(QStringLiteral("конфиг не принят: %1").arg(why));
            return;
        }
        applyAppearance();
        // Незнакомый ключ — это не «настройка не работает», а опечатка или
        // придуманное имя, и молчать о нём нельзя: владелец потерял вечер на
        // «caretColor» вместо «colors.caret».
        statusBar.setMessage(unknown.isEmpty()
                                 ? QString()
                                 : QStringLiteral("в конфиге не понято: %1")
                                       .arg(unknown.join(QStringLiteral(", "))));
        if (!unknown.isEmpty())
            std::fprintf(stderr, "в конфиге не понято: %s\n",
                         unknown.join(QStringLiteral(", ")).toUtf8().constData());
    });
    watchConfig();
    // Про конфиг, прочитанный на старте, сказать надо сразу, а не ждать, пока
    // человек его тронет: незнакомый ключ выглядит как «настройка не работает».
    if (!configUnknown.isEmpty()) {
        std::fprintf(stderr, "в конфиге не понято: %s\n",
                     configUnknown.join(QStringLiteral(", ")).toUtf8().constData());
        statusBar.setMessage(QStringLiteral("в конфиге не понято: %1")
                                 .arg(configUnknown.join(QStringLiteral(", "))));
    }

    splitter.addWidget(&panels.tree());
    if (panels.isStore()) splitter.addWidget(&panels.listPanel());
    splitter.addWidget(&rightSide);
    splitter.setStretchFactor(splitter.count() - 1, 1);   // растёт текст, а не панели
    splitter.setChildrenCollapsible(false);

    {
        auto* shell = new QVBoxLayout(&window);
        shell->setContentsMargins(0, 0, 0, 0);
        shell->setSpacing(0);
        shell->addWidget(&toolbar);
        shell->addWidget(&splitter, 1);
        shell->addWidget(&statusBar);
    }

    // Полоса сведений. Числа приносит редактор: слова и строки он считает сам,
    // размер и даты берутся у файла и у метаданных заметки. Здесь только сборка
    // в кучку — считать окно ничего не должно.
    const auto showStats = [&editor, &statusBar] {
        zametti::StatusBar::NoteInfo info;
        const QString path = editor.filePath();
        info.valid = !path.isEmpty();
        if (info.valid) {
            const QFileInfo file(path);
            info.path = path;
            info.bytes = file.size();
            // Обе даты — из шапки заметки, а не у файла: копирование хранилища
            // отметки файловой системы теряет (первый же снимок показал
            // «правлена» временем копирования). Файл остаётся запасным ходом
            // для заметок без шапки.
            const auto fromMeta = [&editor](const char* key) {
                const std::string value = editor.header().get(key);
                return value.empty() ? QDateTime()
                                     : QDateTime::fromString(QString::fromStdString(value),
                                                             Qt::ISODate);
            };
            info.created = fromMeta("created");
            info.modified = fromMeta("modified");
            if (!info.created.isValid()) info.created = file.birthTime();
            if (!info.modified.isValid()) info.modified = file.lastModified();
            info.words = editor.stats().words;
            info.lines = editor.stats().lines;
            info.images = editor.stats().images;
            info.wordsKnown = editor.statsFresh() && editor.stats().valid;
            info.suspect = editor.selfCheckFailed();
        }
        statusBar.setNote(info);
        const zametti::CaretPlace place = editor.caretPlace();
        statusBar.setCaret(place.line, place.column);
    };
    QObject::connect(&editor, &zametti::NoteEditor::statsChanged, &window, showStats);
    // Ввоз картинок: полоса говорит, почему сейчас нельзя править.
    QObject::connect(&editor, &zametti::NoteEditor::importStatus, &window,
                     [&](const QString& text) {
                         statusBar.setMessage(text);
                         // Пока везём — запираем ВСЁ, чем можно поменять
                         // заметку или уйти с неё: дерево, список и тулбар.
                         // Одним движением, а не проверкой в каждой операции:
                         // проверок два десятка, и забыть одну — вопрос
                         // времени, а цена ошибки — вставка в чужую заметку.
                         const bool locked = !text.isEmpty();
                         panels.tree().setEnabled(!locked);
                         panels.listView().setEnabled(!locked);
                         // Явно QWidget::: у тулбара есть свой setEnabled(кнопка,
                         // да/нет), и он перекрывает виджетный.
                         toolbar.QWidget::setEnabled(!locked);
                     });
    QObject::connect(&editor, &zametti::NoteEditor::fileChanged, &window,
                     [showStats](const QString&) { showStats(); });
    QObject::connect(&editor, &zametti::NoteEditor::fileSaved, &window,
                     [showStats](const QString&) { showStats(); });
    // Место каретки — на каждое движение: по указателю строк это 0.9 мкс,
    // документ при этом не обходится вовсе.
    QObject::connect(&editor, &zametti::NoteEditor::cursorPositionChanged, &window,
                     [&editor, &statusBar] {
                         const zametti::CaretPlace place = editor.caretPlace();
                         statusBar.setCaret(place.line, place.column);

                         // Картинка под кареткой. Только заголовок файла и
                         // только по разу на картинку: сведения кэшируются, а
                         // движение внутри одной картинки панель не трогает.
                         statusBar.setImage(editor.caretImage());
                     });

    // Файл изменился снаружи, а правки не сохранены. Окно неблокирующее: работа
    // не встаёт, пока человек думает, и молча мы ничего не затираем.
    QObject::connect(&editor, &zametti::NoteEditor::externalChangeDetected, &window, [&] {
        auto* ask = new QMessageBox(&window);
        ask->setAttribute(Qt::WA_DeleteOnClose);
        ask->setWindowModality(Qt::NonModal);
        ask->setIcon(QMessageBox::Question);
        ask->setWindowTitle(QStringLiteral("zametti"));
        ask->setText(QFileInfo(editor.filePath()).fileName() +
                     QStringLiteral(" изменилась снаружи, а здесь есть несохранённые "
                                    "правки."));
        QPushButton* mine =
            ask->addButton(QStringLiteral("Оставить мои"), QMessageBox::AcceptRole);
        QPushButton* theirs =
            ask->addButton(QStringLiteral("Взять внешние"), QMessageBox::DestructiveRole);
        ask->setDefaultButton(mine);
        ask->setInformativeText(
            QStringLiteral("Оставить мои — внешняя версия будет перезаписана при "
                           "сохранении. Взять внешние — правки можно вернуть отменой."));
        QObject::connect(ask, &QMessageBox::finished, &window, [&editor, ask, theirs] {
            editor.resolveExternalConflict(ask->clickedButton() == theirs);
        });
        ask->open();
    });

    // В хранилище имя файла — непрозрачный id, в заголовок окна идёт
    // заголовок самой заметки.
    //
    // Спрашиваем по id, а не по QModelIndex: дерево показывает только папки, и
    // у заметки видимой строки нет вовсе — indexForPath вернул бы пустоту, а
    // откат по имени файла показал бы человеку id.
    const auto windowTitleFor = [&](const QString& file) {
        if (model.isStore()) {
            const QString title = model.titleOfId(QFileInfo(file).completeBaseName());
            if (!title.isEmpty()) return title;
        }
        return QFileInfo(file).completeBaseName();
    };
    // Точка одна: заметку открывает только openFile, и он же говорит об этом
    // сигналом. Связь очередью, а не прямым вызовом, — иначе синхронизация
    // выполнялась бы ВНУТРИ ещё не доигранной смены выделения (щелчок по
    // папке открывает первую заметку прямо из обработчика currentChanged), и
    // та, завершившись, возвращала бы курсор дерева на прежнее место.
    // Замерено пробником: папка находилась верно, но выделение откатывалось.
    QObject::connect(&editor, &zametti::NoteEditor::fileChanged, &panels,
                     &zametti::NotePanels::setCurrentNote);   // прямо: панели сверяются с ней
    QObject::connect(&editor, &zametti::NoteEditor::fileChanged, &window,
                     [&](const QString& file) {
                         window.setWindowTitle(windowTitleFor(file) +
                                               QStringLiteral(" — zametti"));
                         panels.showNote(file);
                     },
                     Qt::QueuedConnection);
    // Панели говорят, что человек выбрал; открывает только openFile.
    QObject::connect(&panels, &zametti::NotePanels::noteChosen, &window,
                     [&](const QString& file, bool takeFocus) { editor.openFile(file, takeFocus); });
    QObject::connect(&panels, &zametti::NotePanels::editRequested, &window,
                     [&] { editor.setFocus(); });

    // Кегль задан явно в каждом формате, поэтому штатный зум QTextEdit до него
    // не дотягивается: при смене масштаба документ собирается заново из того же
    // содержимого. В историю правок это не попадает — облик не содержимое.
    editor.setZoom(std::clamp(session.zoom(), zametti::settings().ui().zoomMin(),
                              zametti::settings().ui().zoomMax()));

    // МЕСТО КАРЕТКИ, ПЕРЕЖИВШЕЕ ПЕРЕЗАПУСК, — В ПАМЯТЬ РЕДАКТОРА, до открытия.
    // Дальше заметка открывается обычной дорогой и встаёт туда же, где её
    // оставили, — тем же кодом, что возвращает каретку при переходе туда-сюда
    // между заметками. Отдельного пути «поставить каретку при запуске» нет и
    // быть не должно: это второе место, где решается «куда встать при
    // открытии», и оно однажды разойдётся с первым.
    // Каретка по заметке живёт в состоянии приложения по id (ZAppState::carets);
    // старые state.json помнили её только у последней заметки — подхватываем и
    // их, если про эту заметку иначе ничего не известно.
    if (session.lastFile() == current &&
        !zapp.state().knowsCaret(QFileInfo(current).completeBaseName()))
        zapp.state().rememberCaret(QFileInfo(current).completeBaseName(),
                                   {session.caret(), session.anchor(), 0});
    if (!editor.openFile(current)) return 2;

    // Сохранение переписало файл — заголовок, начало текста и дата в строке
    // списка меняются вслед за ним: хранилище перечитывает заметку и говорит
    // дереву, дерево — списку.
    QObject::connect(&editor, &zametti::NoteEditor::fileSaved, &window,
                     [&](const QString& file) { model.refreshNote(file); });

    const auto shortcut = [&window](const QKeySequence& keys, auto&& slot) {
        QObject::connect(new QShortcut(keys, &window), &QShortcut::activated, &window, slot);
    };
    // Масштаб один на программу: и у живой заметки, и у вида истории — иначе,
    // вернувшись из истории, человек увидел бы другой кегль.
    //
    // НО У РЕЖИМА ИСХОДНИКА ОН СВОЙ (решение владельца). Исходник читают иначе,
    // чем заметку — моноширинным, по колонкам, — и кегль ему нужен другой.
    // Прежде клавиши в режиме молча увеличивали СКРЫТЫЙ обычный вид: отжал [M],
    // а заметка вдруг крупнее, хотя её масштаб не трогали.
    auto applyZoom = [&](qreal value) {
        if (markdown.active()) {
            markdownView.applyZoom(value);
            return;
        }
        editor.applyZoom(value);
        historyView.textView().applyZoom(value);
    };
    auto stepZoom = [&](qreal factor) {
        const qreal now = markdown.active() ? markdownView.zoom() : editor.zoom();
        applyZoom(std::clamp(now * factor, zametti::settings().ui().zoomMin(),
                             zametti::settings().ui().zoomMax()));
    };
    // Ctrl+= рядом с Ctrl++: увеличивают одной и той же клавишей, с шифтом и без.
    shortcut(QKeySequence(QStringLiteral("Ctrl+=")),
             [&] { stepZoom(zametti::settings().ui().zoomStep()); });
    shortcut(QKeySequence(QStringLiteral("Ctrl++")),
             [&] { stepZoom(zametti::settings().ui().zoomStep()); });
    shortcut(QKeySequence(QStringLiteral("Ctrl+-")),
             [&] { stepZoom(1.0 / zametti::settings().ui().zoomStep()); });
    shortcut(QKeySequence(QStringLiteral("Ctrl+0")), [&] { applyZoom(1.0); });

    // Отмена и повтор живут в самом редакторе: QTextEdit объявляет их своими и
    // до ярлыка окна они не доходят.
    shortcut(QKeySequence::Save, [&] {
        // В РЕЖИМЕ ИСХОДНИКА истина живёт в тексте: сперва наложить, потом
        // записывать. Из режима при этом не выходим — человек попросил
        // сохранить, а не закончить.
        if (markdown.active()) {
            markdown.saveWithoutLeaving();
            return;
        }
        editor.save(true);
    });

    // Живой заголовок: правится первый заголовок в редакторе — обновляется и
    // подпись в дереве, не дожидаясь сохранения. Заголовок — первый
    // содержательный блок; без заголовка — первая строка, усечённая.
    QObject::connect(&editor, &QTextEdit::textChanged, &window, [&] {
        if (!model.isStore()) return;
        QString title;
        for (QTextBlock block = editor.document()->begin(); block.isValid();
             block = block.next()) {
            if (!zametti::isRawBlock(block) &&
                zametti::kindOf(block) == zametti::Kind::VSpace)
                continue;
            QString text = block.text();
            const qsizetype eol = text.indexOf(QChar::LineSeparator);
            if (eol >= 0) text = text.left(eol);
            title = text.trimmed().left(64);
            break;
        }
        if (title.isEmpty()) title = QStringLiteral("Без названия");
        model.updateTitle(editor.filePath(), title);
        window.setWindowTitle(title + QStringLiteral(" — zametti"));
    });

    // --- режим истории ------------------------------------------------------
    //
    // Окно только показывает и передаёт: что показать, решает редактор, он же
    // держит журнал. Заголовок в режиме получает машинный штамп с секундами
    // (решение владельца): по нему видно точный момент, и два слепка одной
    // минуты не выглядят одинаково.
    // Закладка для челночных походов «утащил кусок → к последней → вставил →
    // назад». Живёт в сеансе и по заметке: журнал про неё знать не должен —
    // это привычка человека, а не свойство заметки. Обновляется при каждом
    // уходе из режима, поэтому «назад» всегда ведёт туда, откуда только что
    // ушли, а не в начало времён.
    //
    // Читать её сейчас некому: кнопка «назад к посещённому» с тулбара убрана до
    // отдельного брифа про историю. Закладка остаётся — она про поведение,
    // которое никуда не делось, и выбрасывать её, чтобы через бриф написать
    // заново, незачем.
    QHash<QString, int> visitedSnapshot;
    // Ширина списка записей: из state.json; человек двигает ручку — запоминаем,
    // на выходе пишем обратно.
    int historyListWidth = session.historyListWidth();
    QObject::connect(&historyView, &zametti::HistoryView::listWidthChanged, &window,
                     [&](int width) { historyListWidth = width; });

    const auto showHistoryState = [&] {
        const std::shared_ptr<zametti::ZNoteTimeline> tl = history.timeline();
        if (tl == nullptr || !tl->isOpen()) return;
        window.setWindowTitle(windowTitleFor(editor.filePath()) + QStringLiteral(" — ") +
                              zametti::historyStamp(tl->snapshotTime()) +
                              QStringLiteral(" — zametti"));
    };

    QObject::connect(&history, &zametti::HistoryController::modeChanged, &window, [&](bool on) {
        // Вид истории на месте редактора; таймлайн сбоку. Из истории
        // возвращаемся туда, откуда пришли: в исходник, если режим исходника
        // идёт (его возобновляет сам MarkdownController), иначе в редактор.
        textStack.setCurrentWidget(on ? static_cast<QWidget*>(&historyView)
                                      : markdown.active() ? static_cast<QWidget*>(&markdownView)
                                                          : static_cast<QWidget*>(&editor));
        if (on) {
            // Ширина списка — та, что человек выставил (state.json); не
            // выставлял — по содержимому списка, не шире средней колонки. Ставится ПОСЛЕ
            // того, как стек покажет вид (очередью): размеры, заданные до
            // показа, сплиттер перекладывает по sizeHint детей.
            const auto applyWidth = [&] {
                historyView.setListWidth(historyListWidth, zametti::settings().ui().noteListWidth());
            };
            applyWidth();
            QTimer::singleShot(0, &window, applyWidth);
        }
        // Кнопка тулбара показывает состояние режима, откуда бы в него ни
        // вошли: Ctrl+Z, доехавший до дна цепочки, приводит сюда же, и кнопка
        // обязана загореться.
        toolbar.setChecked(zametti::Toolbar::Button::History, on);
        if (on) {
            historyView.setFocus();
            return;
        }
        // Список находок по слепкам без режима истории не значит ничего:
        // щёлкать в нём стало не по чему.
        if (findBar.mode() == zametti::FindBar::Mode::History) {
            results.clear();
            resultsView.hide();
            findBar.hide();
        }
        // Уходим — запоминаем, откуда: «назад к посещённому» вернёт сюда же.
        if (history.lastIndex() >= 0 && !editor.filePath().isEmpty())
            visitedSnapshot.insert(editor.filePath(), history.lastIndex());
        window.setWindowTitle(windowTitleFor(editor.filePath()) + QStringLiteral(" — zametti"));
        if (!markdown.active()) editor.setFocus();
    });
    // РЕЖИМ ПРАВКИ ИСХОДНИКА — третья страница стека, на месте редактора. Как и
    // у истории, кнопка тулбара показывает состояние режима, откуда бы в него
    // ни вошли (кнопка, сочетание из настроек, восстановление на старте).
    QObject::connect(&markdown, &zametti::MarkdownController::modeChanged, &window, [&](bool on) {
        textStack.setCurrentWidget(on ? static_cast<QWidget*>(&markdownView)
                                      : static_cast<QWidget*>(&editor));
        toolbar.setChecked(zametti::Toolbar::Button::MarkdownEdit, on);
        if (on) {
            markdownView.setFocus();
            return;
        }
        editor.setFocus();
    });
    // РЕЖИМ ИСХОДНИКА ПЕРЕЖИВАЕТ ПОХОД В ИСТОРИЮ (см. MarkdownController).
    // Подключается ПОСЛЕ обработчиков стека выше: возобновление режима на выходе
    // из истории обязано идти последним, чтобы страница и фокус остались за ним.
    markdown.attachHistory(history);
    QObject::connect(&markdown, &zametti::MarkdownController::applyRefused, &window, [&](int code) {
        // Текст не принят — и человек обязан узнать почему, а не гадать, отчего
        // кнопка не гаснет. Второй случай — дефект, и он назван дефектом.
        statusBar.setMessage(code == -1
                            ? QStringLiteral("в исходнике набрана шапка заметки — уберите её")
                            : QStringLiteral("правка не наложилась и отменена — это дефект"));
    });

    QObject::connect(&history, &zametti::HistoryController::indexChanged, &window,
                     [&](int) { showHistoryState(); });
    // Восстановление — одно на баннер и на кнопку тулбара; статус пишет окно.
    QObject::connect(&history, &zametti::HistoryController::restored, &window, [&](qint64 source) {
        findBar.setStatus(QStringLiteral("восстановлено из слепка %1")
                              .arg(zametti::historyMoment(source)));
    });
    QObject::connect(&history, &zametti::HistoryController::restoreWasCurrent, &window, [&] {
        findBar.setStatus(QStringLiteral("этот слепок и есть нынешняя версия"));
    });


    // ПРАВКА ШАПКИ ЗАКРЫТОЙ ЗАМЕТКИ ЖАЛУЕТСЯ САМА. Отказ записи означал бы, что
    // папка на диске осталась заметкой, а перенесённая — неперенесённой, и оба
    // раза молча. Пишет хранилище (ZStorage::rename/move/setSortMark — штатный
    // путь записи и шаг журнала); здесь только слово человеку.
    const auto complain = [&window](const QString& file, const QString& why) {
        std::fprintf(stderr, "правка заметки не удалась: %s — %s\n",
                     file.toUtf8().constData(), why.toUtf8().constData());
        QMessageBox::warning(&window, QStringLiteral("zametti"),
                             QStringLiteral("Не удалось записать %1: %2")
                                 .arg(QFileInfo(file).fileName(), why));
    };

    // ПЕРЕЧИТАТЬ ХРАНИЛИЩЕ. Каталог меняется и мимо нас: файл вернули из
    // системной корзины, положили заметку соседней программой, синхронизация
    // принесла чужое. Раньше это лечилось только перезапуском (владелец
    // наткнулся, восстанавливая заметку с картинками).
    //
    // Дверей три, и все зовут одно и то же: F5, Ctrl+R и пункт «Обновить» в
    // контекстных меню.
    const auto reloadStore = [&] {
        // Тот же проход, что и при открытии: файл могли вернуть из системной
        // корзины или положить руками, пока программа работала.
        for (const QString& line : zapp.storage()->migrate())
            std::fprintf(stderr, "%s\n", line.toUtf8().constData());
        zapp.storage()->reload();   // дерево и список догонят по сигналу
        statusBar.setMessage(QStringLiteral("хранилище перечитано"));
        QTimer::singleShot(1500, &statusBar, [&statusBar] { statusBar.setMessage(QString()); });
    };
    shortcut(QKeySequence(Qt::Key_F5), reloadStore);
    shortcut(QKeySequence(QStringLiteral("Ctrl+R")), reloadStore);

    // СТОРОЖ КАТАЛОГА — ПО УМОЛЧАНИЮ ВЫКЛЮЧЕН (`notes.watchFolder`, решение
    // владельца): штатный путь обновить хранилище один и явный — F5, Ctrl+R или
    // «Обновить» в меню. Сам сторож живёт в хранилище (ZStorage::setWatching):
    // сверяет состав каталога и перечитывает его на разнице, а панели догоняют
    // по сигналу. Открытую заметку он не трогает: у неё свой сторож.
    if (panels.isStore() && zametti::settings().store().watchStore()) zapp.storage()->setWatching(true);

    // F2: новый заголовок. Открытая заметка правится через редактор (первый
    // содержательный блок), закрытая — через ядро по файлу.
    QObject::connect(&model, &zametti::NoteTreeModel::renameRequested, &window,
                     [&](const QString& file, const QString& title) {
        if (file == editor.filePath()) {
            for (QTextBlock block = editor.document()->begin(); block.isValid();
                 block = block.next()) {
                if (!zametti::isRawBlock(block) &&
                    zametti::kindOf(block) == zametti::Kind::VSpace)
                    continue;
                if (zametti::isRawBlock(block) ||
                    zametti::kindOf(block) != zametti::Kind::Heading)
                    break;   // без заголовка — файл перепишет ветка ниже
                QTextCursor cursor(block);
                cursor.setPosition(block.position());
                cursor.setPosition(block.position() + block.length() - 1,
                                   QTextCursor::KeepAnchor);
                cursor.insertText(title);
                editor.save(false);
                return;
            }
            editor.save(false);
        }
        {
            QString error;
            if (!zapp.storage()->rename(zametti::ZStorage::idOfPath(file), title,
                                        zametti::NoteEditor::historyRules(), &error))
                complain(file, error);
        }
        if (file == editor.filePath()) editor.openFile(file);
        panels.selectNote(file);
    });

    // Перенос: правка parent. Открытая — через редактор, закрытая — по файлу.
    // keepPath: кого выделить после (перетаскиванию — саму заметку; корзине —
    // соседа: раскрывать корзину и «показывать» удалённое не надо).
    const auto moveNote = [&](const QString& noteId, const QString& parentId,
                              const QString& keepPath) {
        const QString file =
            model.nodePath(QModelIndex()) + QLatin1Char('/') + noteId +
            QStringLiteral(".md");
        if (file == editor.filePath()) {
            editor.setMetaParent(parentId);
        } else {
            QString error;
            if (!zapp.storage()->move(noteId, parentId, zametti::NoteEditor::historyRules(),
                                      &error))
                complain(file, error);
        }
        panels.selectNote(keepPath);
    };
    QObject::connect(&model, &zametti::NoteTreeModel::moveRequested, &window,
                     [&](const QString& noteId, const QString& parentId) {
                         moveNote(noteId, parentId,
                                  model.nodePath(QModelIndex()) + QLatin1Char('/') +
                                      noteId + QStringLiteral(".md"));
                     });

    // Del / «В архив». Пустая заметка и пустая папка удаляются сразу — в
    // ФАЙЛОВУЮ корзину ОС (при тестировании их плодится много, гонять их через
    // архив — трата времени). Непустая уходит В АРХИВ: тело в журнал, файл —
    // стаб, в шапке пометка `archived: yes`, а `parent` не трогается вовсе.
    // Из архива — насовсем, с подтверждением, и тогда журнал уезжает вместе с
    // заметкой: тело живёт в нём и больше нигде.
    // Выделение после удаления уходит к соседу: раскрывать Архив и
    // «показывать» убранное не надо.
    const auto deleteNote = [&](const QString& noteId) {
        if (!model.isStore() || noteId.isEmpty() || !model.hasNote(noteId)) return;
        // Сама строка «Архив» ничему не подлежит: файла за ней нет.
        if (!model.hasNote(noteId)) return;
        const QString file = model.pathOfId(noteId);
        const bool wasOpen = file == editor.filePath();

        // Сосед по папке — будущий выделенный.
        const QString fallback = model.pathOfId(model.neighbourOf(noteId));

        const auto settleAfter = [&] {
            panels.selectNote(fallback);
            if (!wasOpen) return;
            QString open = fallback;
            if (open.isEmpty()) open = model.pathOfId(model.firstNoteId());
            if (!open.isEmpty()) editor.openFile(open);
        };

        // Пустое — в корзину ОС без разговоров. Пустая папка — без детей;
        // пустая заметка — без содержательного текста: открытая меряется по
        // документу (набранное могло ещё не сохраниться), закрытая — по
        // хранилищу.
        const bool empty = wasOpen && !model.isFolderId(noteId)
                               ? editor.toPlainText().trimmed().isEmpty()
                               : zapp.storage()->isEmptyNote(noteId);
        if (empty || model.inArchiveId(noteId)) {
            if (!empty) {
                const auto answer = QMessageBox::question(
                    &window, QStringLiteral("zametti"),
                    QStringLiteral("Удалить насовсем «%1»?")
                        .arg(model.titleOfId(noteId)));
                if (answer != QMessageBox::Yes) return;
            }
            // Если заметка открыта, сначала сохраняем: иначе последним слепком
            // в истории осталось бы состояние до последних правок, а человек
            // удаляет то, что видит. Само удаление — дело хранилища: надгробие
            // или журнал вместе с архивной, картинки следом.
            if (wasOpen) editor.save(false);
            QString deleteError;
            if (!zapp.storage()->remove(noteId, &deleteError)) {
                QMessageBox::warning(&window, QStringLiteral("zametti"), deleteError);
                return;
            }
            settleAfter();
            return;
        }

        // АРХИВАЦИЯ. Никакого переноса: `parent` у заметки остаётся прежним, в
        // шапке появляется пометка, а тело уезжает в журнал — файл становится
        // стабом (store/archive.h). Открытую заметку сперва сохраняем: человек
        // убирает то, что видит, и последние правки обязаны попасть в историю
        // раньше среза.
        if (wasOpen) editor.save(false);
        // Папка уезжает вместе с содержимым — это знает хранилище.
        QStringList failed;
        zapp.storage()->archive(noteId, zametti::NoteEditor::historyRules(), &failed);
        if (!failed.isEmpty())
            QMessageBox::warning(&window, QStringLiteral("zametti"),
                                 QStringLiteral("Убрать в архив удалось не всё:\n%1")
                                     .arg(failed.join(QLatin1Char('\n'))));
        // Открытую заметку перечитываем с диска: на её месте теперь стаб, и
        // редактор обязан показать то, что в файле, а не то, что помнит.
        if (wasOpen) editor.openFile(file);
        settleAfter();
    };

    // «Вернуть из архива»: пометка снимается, тело приезжает из головы журнала,
    // и заметка оказывается ровно там, откуда её убрали, — `parent` всё это
    // время лежал в её шапке нетронутым. Прежняя корзина ради этого держала два
    // ключа и умела пересоздавать цепочку папок по именам; архиву не нужно
    // ничего: родитель умер — заметка станет сиротой и уедет в бюро находок
    // штатной починкой, как всякая другая.
    const auto restoreNote = [&](const QString& noteId) {
        if (!model.isStore() || noteId.isEmpty()) return;
        if (!model.inArchiveId(noteId)) return;
        const QString file = zapp.storage()->pathOf(noteId);
        QStringList failed;
        zapp.storage()->restore(noteId, &failed);
        if (!failed.isEmpty())
            QMessageBox::warning(&window, QStringLiteral("zametti"),
                                 QStringLiteral("Вернуть удалось не всё:\n%1")
                                     .arg(failed.join(QLatin1Char('\n'))));
        // Открытая заметка была стабом — перечитываем: тело вернулось.
        if (file == editor.filePath()) editor.openFile(file);
        // И ПОКАЗАТЬ, КУДА ВЕРНУЛАСЬ: вторичным выделением её папки (пунктирная
        // рамка, предки раскрыты). Курсор дерева и средняя колонка — человека,
        // они остаются: он смотрит архив и видит, куда заметка ушла.
        panels.showNote(file);
    };
    // Del в дереве бьёт по папке, Del в списке — по заметке (ярлыки — у панелей).
    QObject::connect(&panels, &zametti::NotePanels::deleteRequested, &window,
                     [&](const QString& id) { deleteNote(id); });

    // Новая заметка или папка. Папка — та же заметка, но с role: folder в
    // мете: опустевшая папка не превращается обратно в заметку.
    const auto createNote = [&](const QString& requestedParent, bool folder) {
        if (!model.isStore()) return;
        editor.save(false);
        QString newError;
        const QString madeId = zapp.storage()->createNote(requestedParent, folder, &newError);
        if (madeId.isEmpty()) {
            QMessageBox::warning(&window, QStringLiteral("zametti"), newError);
            return;
        }
        const QString made = zapp.storage()->pathOf(madeId);
        // Дерево уже перестроено по сигналу хранилища.
        if (folder) {
            const QModelIndex fresh = model.indexForPath(made);
            if (fresh.isValid()) tree.edit(fresh);   // сразу дать имя
        } else {
            editor.openFile(made);   // показ в панелях — вслед за открытием
        }
    };

    // Импорт .md в хранилище. Источник не трогается: делается копия под
    // свежим id, с шапкой и сразу в каноническом виде — чтобы человек тут же
    // увидел, во что превратился его файл, а не узнал об этом при первом
    // сохранении. Выбор множественный: приносят обычно не по одному файлу.
    const auto importNotes = [&](const QString& parentId) {
        if (!model.isStore()) return;
        const QStringList files = QFileDialog::getOpenFileNames(
            &window, QStringLiteral("Импортировать заметки"), QString(),
            QStringLiteral("Заметки markdown (*.md *.markdown);;Все файлы (*)"));
        if (files.isEmpty()) return;

        QString first;
        QStringList failed;
        for (const QString& file : files) {
            QString error;
            const QString madeId = zapp.storage()->importNote(parentId, file, &error);
            const QString made = madeId.isEmpty() ? QString() : zapp.storage()->pathOf(madeId);
            if (made.isEmpty()) {
                failed.append(QFileInfo(file).fileName() + QStringLiteral(": ") + error);
                continue;
            }
            if (first.isEmpty()) first = made;
        }

        if (!first.isEmpty()) editor.openFile(first);   // дерево уже догнало по сигналу
        if (!failed.isEmpty()) {
            QMessageBox::warning(
                &window, QStringLiteral("zametti"),
                QStringLiteral("Не импортировано файлов: %1\n\n%2")
                    .arg(failed.size())
                    .arg(failed.join(QLatin1Char('\n'))));
        }
    };

    // Вывоз открытой заметки наружу. Куда и в каком виде — решает расширение,
    // которое выберет человек: .md или .pdf. Двух кнопок для этого не нужно,
    // диалог сохранения и так спрашивает и то, и другое.
    //
    // Имя предлагается ПО ЗАГОЛОВКУ, а не по имени файла: в хранилище имя это
    // идентификатор, и «01n6r08s8wy52h.md» снаружи не говорит ничего.
    //
    // Каталог запоминается на время сеанса: вывозят обычно несколько заметок
    // подряд и в одно место.
    // Каталог вывоза переживает и смену формата, и перезапуск программы.
    QString exportDir = session.exportDir();
    bool exportKeepMeta = session.exportKeepMeta();
    const auto exportNote = [&](const QString& file) {
        if (file.isEmpty()) return;
        // Пишем ДО вывоза: иначе наружу уехала бы заметка без последних правок,
        // а человек об этом не узнал бы — на экране-то они есть. Записывается
        // открытая заметка, и вывозят чаще всего именно её; вывоз чужой
        // заметки из списка её правок не касается.
        if (file == editor.filePath()) editor.save(false);

        const QString noteId = QFileInfo(file).completeBaseName();
        const QString title = model.titleOfId(noteId);
        // Имя зависит от галочки: чистый вывоз зовётся по заголовку, вывоз «как
        // есть» — по id, потому что он и предназначен для другого хранилища
        // zametti, где имя файла это идентификатор.
        const auto nameFor = [&](bool keepMeta) {
            return keepMeta ? noteId
                            : zametti::fileNameFromTitle(title.isEmpty() ? noteId : title);
        };
        QString name = nameFor(exportKeepMeta);
        if (exportDir.isEmpty() || !QFileInfo(exportDir).isDir())
            exportDir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);

        const QString markdown = QStringLiteral("Markdown с картинками (*.md)");
        const QString pdf = QStringLiteral("PDF (*.pdf)");

        // Диалог свой, а не getSaveFileName: тому нельзя сказать «сменили
        // фильтр — смени и расширение», а без этого человек, выбрав PDF,
        // сохранял файл с именем «Заметка.md» и получал markdown.
        QFileDialog dialog(&window, QStringLiteral("Вывезти заметку"), exportDir);
        dialog.setAcceptMode(QFileDialog::AcceptSave);
        dialog.setNameFilters({markdown, pdf});
        dialog.setDefaultSuffix(QStringLiteral("md"));
        // СВОЙ ДИАЛОГ, А НЕ СИСТЕМНЫЙ: в системный виджет не вставить, а
        // галочка нужна именно здесь, рядом с именем файла. Qt в этом случае
        // молча ничего не показывает — поэтому DontUseNativeDialog стоит явно.
        dialog.setOption(QFileDialog::DontUseNativeDialog, true);
        auto* keepMetaBox =
            new QCheckBox(QStringLiteral("Сохранять имя и метаданные"), &dialog);
        keepMetaBox->setChecked(exportKeepMeta);
        keepMetaBox->setToolTip(QStringLiteral(
            "Файл уедет как есть: с шапкой и под именем-идентификатором.\n"
            "Такой файл, положенный в другое хранилище zametti, попадёт в «Бюро находок».\n"
            "Без галочки наружу уезжает чистый markdown с человеческим именем."));
        if (auto* grid = qobject_cast<QGridLayout*>(dialog.layout()))
            grid->addWidget(keepMetaBox, grid->rowCount(), 0, 1, grid->columnCount());
        // ИМЯ ЦЕЛИКОМ, С КАТАЛОГОМ. selectFile с относительным именем ставит
        // файл в каталог по умолчанию, а не в тот, что задан диалогу: каталог
        // сбрасывался на «Документы» и при открытии, и при каждой смене
        // формата. Владелец наткнулся на оба случая.
        dialog.selectFile(zametti::exportTargetPath(exportDir, name, false));
        // Галочка меняет ИМЯ: «как есть» зовётся по id, чистый вывоз — по
        // заголовку. Меняем только имя, каталог человек уже выбрал сам.
        QObject::connect(keepMetaBox, &QCheckBox::toggled, &dialog,
                         [&dialog, &nameFor, &name, pdf](bool on) {
            name = nameFor(on);
            const QString chosenPath = dialog.selectedFiles().value(0);
            const QString where = chosenPath.isEmpty() ? dialog.directory().absolutePath()
                                                       : QFileInfo(chosenPath).absolutePath();
            dialog.selectFile(zametti::exportTargetPath(
                where, name, dialog.selectedNameFilter() == pdf));
        });
        QObject::connect(&dialog, &QFileDialog::filterSelected, &dialog,
                         [&dialog, &name, pdf](const QString& chosen) {
            const bool paper = chosen == pdf;
            dialog.setDefaultSuffix(paper ? QStringLiteral("pdf") : QStringLiteral("md"));
            // Имя берём то, что человек уже набрал, а не предложенное:
            // переключение фильтра не повод отменять его правку. Каталог — тот,
            // в котором он сейчас стоит, а не тот, с которого начали.
            const QString chosenPath = dialog.selectedFiles().value(0);
            QString base = QFileInfo(chosenPath).completeBaseName();
            if (base.isEmpty()) base = name;
            const QString where = chosenPath.isEmpty() ? dialog.directory().absolutePath()
                                                       : QFileInfo(chosenPath).absolutePath();
            dialog.selectFile(zametti::exportTargetPath(where, base, paper));
        });
        if (dialog.exec() != QDialog::Accepted) return;
        QString target = dialog.selectedFiles().value(0);
        if (target.isEmpty()) return;

        // Расширение мог не набрать никто: setDefaultSuffix спасает не всегда
        // (набранная точка в имени сходит за расширение). Тогда решает фильтр.
        if (QFileInfo(target).suffix().isEmpty())
            target += dialog.selectedNameFilter() == pdf ? QStringLiteral(".pdf")
                                                         : QStringLiteral(".md");
        exportDir = QFileInfo(target).absolutePath();

        const QString suffix = QFileInfo(target).suffix().toLower();
        zametti::ExportReport report;
        if (suffix == QStringLiteral("pdf")) {
            report = zametti::exportPdf(file, target, zametti::settings().pdf(), title);
        } else {
            exportKeepMeta = keepMetaBox->isChecked();
            report = zametti::exportMarkdown(file, target, exportKeepMeta);
        }

        if (!report.ok()) {
            QMessageBox::warning(&window, QStringLiteral("zametti"),
                                 QStringLiteral("Не удалось вывезти заметку.\n%1")
                                     .arg(report.error));
            return;
        }
        // Молчать нельзя ровно в двух случаях: что-то переименовано или чего-то
        // не нашлось. В остальных человек и так видит файл там, где просил.
        if (!report.notes.isEmpty())
            QMessageBox::information(&window, QStringLiteral("zametti"),
                                     QStringLiteral("Заметка вывезена в %1.\n\n%2")
                                         .arg(QFileInfo(target).fileName(),
                                              report.notes.join(QStringLiteral("\n"))));
    };

    // Ctrl+N: редактор перехватывает сочетание через ShortcutOverride, до
    // оконного ярлыка оно не доживало — поэтому фильтр на самом редакторе.
    struct NewNoteGrab : QObject {
        std::function<void()> onNew;
        bool eventFilter(QObject*, QEvent* event) override {
            if (event->type() != QEvent::ShortcutOverride &&
                event->type() != QEvent::KeyPress)
                return false;
            auto* key = static_cast<QKeyEvent*>(event);
            if (!key->matches(QKeySequence::New)) return false;
            if (event->type() == QEvent::KeyPress) onNew();
            event->accept();
            return true;
        }
    };
    auto* grab = new NewNoteGrab;
    grab->setParent(&window);
    grab->onNew = [&] { createNote(panels.currentFolderId(), false); };
    editor.installEventFilter(grab);
    shortcut(QKeySequence::New,
             [&] { createNote(panels.currentFolderId(), false); });

    // ПОРЯДОК СОРТИРОВКИ — у панелей: дерево сортируется по папкам (своя метка →
    // родительская → переключатель корня), список идёт порядком выбранной папки,
    // а кнопкам тулбара панели говорят сигналом, что показывать (Toolbar::showSort:
    // значок — про направление, цвет — про источник порядка).
    QObject::connect(&panels, &zametti::NotePanels::sortShown, &window,
                     [&](zametti::SortOrder order, bool fromMark) { toolbar.showSort(order, fromMark); });

    // ЗАПИСЬ ПОРЯДКА. Выбрана папка — метка уходит в её шапку и переживает всё,
    // включая синхронизацию: это метаданные, а не настройка интерфейса. Выбран
    // корень — двигается только переключатель, и ни один файл хранилища не
    // меняется (инвариант A брифа). Пустой order означает «сбросить»: метка
    // убирается, папка снова наследует.
    //
    // modified папки при этом НЕ поднимается: пометка — правка
    // организационная, как перенос, и всплывать наверх списка от неё папка не
    // должна (правило этапа 7). Держится это тем, что оба пути записи —
    // ZStorage::setSortMark и editMeta — штампа не ставят.
    const auto setSortFor = [&](const QString& folderId,
                                std::optional<zametti::SortOrder> order) {
        if (folderId.isEmpty()) {
            panels.setRootSort(order.value_or(zametti::defaultOrder(zametti::SortKey::Modified)));
            return;
        }
        const QString file = model.pathOfId(folderId);
        if (file.isEmpty()) return;
        const auto change = [&order](zametti::NoteHeader& meta) {
            zametti::applySortMark(meta, order);
        };
        if (file == editor.filePath()) {
            editor.editMeta(change);
            // Метку читает каталог — перечитать заметку; дерево пересортируется
            // по сигналу (метка — структурная новость).
            model.refreshNote(file);
        } else {
            QString error;
            if (!zapp.storage()->setSortMark(folderId, order, zametti::NoteEditor::historyRules(),
                                             &error))
                complain(file, error);
        }
        panels.syncSort();
    };
    panels.syncSort();

    // НАЖАТИЕ КНОПКИ. По неактивной — включить её ключ в направлении по
    // умолчанию; по активной — перевернуть направление. Записывается это туда
    // же, куда смотрит человек: выбрана папка — в её шапку, выбран корень — в
    // переключатель.
    const auto pressSort = [&](zametti::SortKey key) {
        bool fromMark = false;
        const zametti::SortOrder now = panels.currentSort(&fromMark);
        setSortFor(panels.currentFolderId(), zametti::pressedSort(now, key));
    };

    // --- внешний редактор ----------------------------------------------------
    //
    // Открывается настоящий файл хранилища целиком, вместе с шапкой метаданных.
    // Никаких временных копий и «очищенных» выгрузок: инвариант «файл правится
    // чем угодно» — основа формата, а мета-комментарий выбран именно за то, что
    // переживает чужие редакторы.
    const auto openExternally = [&](const QString& file) {
        if (file.isEmpty()) return;
        editor.save(false);   // сначала на диск, иначе снаружи откроется старое
        const QString command = zametti::settings().editor().externalEditor();
        if (command.isEmpty()) {
            QDesktopServices::openUrl(QUrl::fromLocalFile(file));
            return;
        }
        QStringList parts = QProcess::splitCommand(command);
        if (parts.isEmpty()) return;
        const QString program = parts.takeFirst();
        bool gotPlaceholder = false;
        for (QString& part : parts) {
            if (!part.contains(QStringLiteral("%f"))) continue;
            part.replace(QStringLiteral("%f"), file);
            gotPlaceholder = true;
        }
        // Без %f путь всё равно нужен: команда без него открыла бы редактор
        // пустым, и это выглядело бы как «не работает».
        if (!gotPlaceholder) parts.append(file);
        if (!QProcess::startDetached(program, parts))
            QMessageBox::warning(&window, QStringLiteral("zametti"),
                                 QStringLiteral("не запускается: %1").arg(command));
    };

    // Внешний редактор снёс или обкорнал шапку. Молчать нельзя: заметка без
    // меты уезжает в корень и теряет дату создания. Окно неблокирующее —
    // работа не встаёт, пока человек думает.
    QObject::connect(&editor, &zametti::NoteEditor::metaDamaged, &window,
                     [&](const QString& file, const QStringList& keys) {
        auto* ask = new QMessageBox(&window);
        ask->setAttribute(Qt::WA_DeleteOnClose);
        ask->setWindowModality(Qt::NonModal);
        ask->setIcon(QMessageBox::Warning);
        ask->setWindowTitle(QStringLiteral("zametti"));
        ask->setText(QStringLiteral("Во внешней правке «%1» пропало: %2.")
                         .arg(model.titleOfId(QFileInfo(file).completeBaseName()),
                              keys.join(QStringLiteral(", "))));
        ask->setInformativeText(
            QStringLiteral("Восстановить прежние значения? Правки текста сохранятся, "
                           "вернуть можно отменой (Ctrl+Z)."));
        QPushButton* restore =
            ask->addButton(QStringLiteral("Восстановить"), QMessageBox::AcceptRole);
        ask->addButton(QStringLiteral("Оставить как есть"), QMessageBox::RejectRole);
        ask->setDefaultButton(restore);
        QObject::connect(ask, &QMessageBox::finished, &window, [&, ask, restore] {
            if (ask->clickedButton() == restore) editor.restoreDamagedMeta();
            else editor.forgetDamagedMeta();
            model.refreshNote(editor.filePath());
        });
        ask->open();
    });

    QObject::connect(&editor, &zametti::NoteEditor::exportRequested, &window,
                     [&](const QString& path) { exportNote(path); });
    QObject::connect(&editor, &zametti::NoteEditor::externalEditorRequested, &window,
                     [&](const QString& file) { openExternally(file); });

    // Внешняя правка могла сменить parent — это законный перенос — или тронуть
    // заголовок. Дерево и список догоняют файл: хранилище перечитывает заметку
    // и само решает, строка это или структура (сменился родитель — дерево
    // строится заново).
    QObject::connect(&editor, &zametti::NoteEditor::externalAdopted, &window,
                     [&](const QString& file) {
                         if (model.isStore()) model.refreshNote(file);
                     });

    // Контекстное меню левой панели — про папки: создание, переименование,
    // корзина. Тело самой папки открывается отдельным пунктом: в средней
    // колонке папок нет, и иначе до её текста было бы не добраться.
    tree.setContextMenuPolicy(Qt::CustomContextMenu);
    // «Очистить корзину» больше нет. Удаление насовсем стало пер-заметочным
    // (пункт в контекстном меню Архива): у архивной заметки тело живёт в
    // журнале, и «выкинуть всё разом» — это стереть историю десятков заметок
    // одним нажатием. Скопом такое не делают.

    QObject::connect(&tree, &QWidget::customContextMenuRequested, &window,
                     [&](const QPoint& pos) {
        if (!model.isStore()) return;
        // Клик мимо строк — меню действует на выделенную папку: пункт
        // «В корзину» не должен пропадать из-за промаха мышью.
        QModelIndex at = tree.indexAt(pos);
        if (!at.isValid()) at = tree.currentIndex();
        const QString id = model.idOf(at);
        QMenu menu(&tree);
        menu.addAction(QStringLiteral("Обновить (F5)"), [&] { reloadStore(); });
        menu.addSeparator();
        menu.addAction(QStringLiteral("Новая заметка"),
                       [&] { createNote(model.folderIdFor(at), false); });
        menu.addAction(QStringLiteral("Новая папка"),
                       [&] { createNote(model.folderIdFor(at), true); });
        menu.addAction(QStringLiteral("Импортировать…"),
                       [&] { importNotes(model.folderIdFor(at)); });

        // «Сортировать по» — вторая дверь туда же, куда ведут три кнопки
        // тулбара, и единственная, где можно СБРОСИТЬ метку: кнопками порядок
        // только задают. Пункт «умолчанию» есть у папки и нет у корня —
        // корневой переключатель не наследует ни от кого.
        {
            const QString folderId = model.folderIdFor(at);
            QMenu* sortMenu = menu.addMenu(QStringLiteral("Сортировать по"));
            bool fromMark = false;
            const zametti::SortOrder now =
                model.effectiveSortFor(folderId, panels.rootSort(), &fromMark);
            const bool own = model.explicitSortOf(folderId).has_value();
            if (!folderId.isEmpty()) {
                QAction* reset = sortMenu->addAction(QStringLiteral("умолчанию"), [&, folderId] {
                    setSortFor(folderId, std::nullopt);
                });
                reset->setCheckable(true);
                reset->setChecked(!own);
                sortMenu->addSeparator();
            }
            for (const zametti::SortOrder order :
                 {zametti::SortOrder{zametti::SortKey::Name, true},
                  zametti::SortOrder{zametti::SortKey::Name, false},
                  zametti::SortOrder{zametti::SortKey::Modified, false},
                  zametti::SortOrder{zametti::SortKey::Modified, true},
                  zametti::SortOrder{zametti::SortKey::Created, false},
                  zametti::SortOrder{zametti::SortKey::Created, true}}) {
                QAction* item = sortMenu->addAction(zametti::sortOrderTitle(order),
                                                    [&, folderId, order] {
                                                        setSortFor(folderId, order);
                                                    });
                item->setCheckable(true);
                // Галочка стоит на ДЕЙСТВУЮЩЕМ порядке, даже если он
                // унаследован: меню отвечает на вопрос «как сейчас», а «своё
                // или наследство» видно по пункту «умолчанию».
                item->setChecked(order == now);
            }
        }

        if (!id.isEmpty()) {
            menu.addSeparator();
            // «Открыть как заметку» здесь больше нет. Папка — структура, а не
            // заметка; то, что она лежит в хранилище файлом .md, — особенность
            // хранения, и наружу её выпускать незачем. В теле такого файла
            // положено быть только шапке и заголовку, а редактор рано или
            // поздно завёл бы там текст, который никто уже не увидит.
            menu.addAction(QStringLiteral("Переименовать"), [&] { tree.edit(at); });
            if (model.inArchiveId(id))
                menu.addAction(QStringLiteral("Вернуть из архива"), [&] { restoreNote(id); });
            menu.addAction(model.inArchiveId(id) ? QStringLiteral("Удалить насовсем")
                                                 : QStringLiteral("В архив"),
                           [&] { deleteNote(id); });
        }
        menu.exec(tree.viewport()->mapToGlobal(pos));
    });

    // Контекстное меню списка — про заметки: перенос, корзина, восстановление.
    QObject::connect(&listView, &QWidget::customContextMenuRequested, &window,
                     [&](const QPoint& pos) {
        if (!model.isStore()) return;
        QModelIndex at = listView.indexAt(pos);
        if (!at.isValid()) at = listView.currentIndex();
        const QString id = list.idAt(at);
        QMenu menu(&listView);

        // Импорт есть всегда, даже когда щёлкнули мимо строк и заметки под
        // курсором нет вовсе: он про папку, а не про строку.
        menu.addAction(QStringLiteral("Импортировать…"),
                       [&] { importNotes(panels.currentFolderId()); });
        if (id.isEmpty()) {
            menu.exec(listView.viewport()->mapToGlobal(pos));
            return;
        }
        menu.addSeparator();

        // Перенос: список папок плоским перечнем с отступами. Перетаскивание
        // работает и так, но мышью через всё дерево — не для длинного списка.
        QMenu* moveTo = menu.addMenu(QStringLiteral("Перенести в папку"));
        std::function<void(const QModelIndex&, int)> addFolders =
            [&](const QModelIndex& parent, int depth) {
                for (int row = 0; row < model.rowCount(parent); ++row) {
                    const QModelIndex folder = model.index(row, 0, parent);
                    const QString folderId = model.idOf(folder);
                    if (model.inArchiveId(folderId)) continue;
                    const QString label =
                        QString(depth * 4, QLatin1Char(' ')) + model.titleOf(folder);
                    moveTo->addAction(label, [&, folderId] {
                        moveNote(id, folderId, model.pathOfId(id));
                    });
                    addFolders(folder, depth + 1);
                }
            };
        addFolders(QModelIndex(), 0);

        menu.addSeparator();
        menu.addAction(QStringLiteral("Обновить (F5)"), [&] { reloadStore(); });
        menu.addSeparator();
        menu.addAction(QStringLiteral("Открыть во внешнем редакторе"),
                       [&] { openExternally(model.pathOfId(id)); });
        menu.addAction(QStringLiteral("Экспортировать…"),
                       [&] { exportNote(model.pathOfId(id)); });
        menu.addSeparator();
        if (model.inArchiveId(id))
            menu.addAction(QStringLiteral("Вернуть из архива"), [&] { restoreNote(id); });
        menu.addAction(model.inArchiveId(id) ? QStringLiteral("Удалить насовсем")
                                             : QStringLiteral("В архив"),
                       [&] { deleteNote(id); });
        menu.exec(listView.viewport()->mapToGlobal(pos));
    });

    // --- поиск --------------------------------------------------------------
    //
    // Панель одна на три команды; какая из них действует, решает её вид. F3
    // отдаётся той панели, что открыта, — открытие одной закрывает другую по
    // построению: панель-то одна.
    const auto searchRoot = [&] { return model.nodePath(QModelIndex()); };

    // ГДЕ ИЩЕМ: в живой заметке или в показанном слепке истории. Механика
    // поиска у обоих видов одна (NoteView), цель выбирается режимом.
    const auto searchTarget = [&]() -> zametti::NoteView& {
        if (history.active()) return historyView.textView();
        return editor;
    };

    // В РЕЖИМЕ ИСХОДНИКА ИЩЕМ ПО ПЛОСКОМУ ТЕКСТУ. Ветка, а не третий случай в
    // searchTarget: общего у двух поисков ровно ноль, кроме слова «поиск».
    // Там найденное живёт при заметке и адресуется блоками и объектами, здесь —
    // смещениями в тексте виджета.
    const auto updateInNoteSearch = [&](const QString& text) {
        const zametti::Query query = zametti::makeQuery(text);
        if (markdown.active()) {
            if (query.isEmpty()) {
                markdownView.clearMatches();
                findBar.setStatus(QString());
                return;
            }
            const int found = markdownView.findMatches(query.needle, query.caseSensitive);
            findBar.setStatus(found == 0 ? QStringLiteral("нет совпадений")
                                         : QStringLiteral("%1/%2")
                                               .arg(markdownView.currentMatch() + 1)
                                               .arg(found));
            return;
        }
        zametti::NoteView& target = searchTarget();
        if (query.isEmpty()) {
            target.clearMatches();
            findBar.setStatus(QString());
            return;
        }
        const int count = target.findMatches(query.needle, query.caseSensitive);
        findBar.setStatus(count == 0
                              ? QStringLiteral("нет совпадений")
                              : QStringLiteral("%1/%2")
                                    .arg(target.currentMatch() + 1)
                                    .arg(count));
    };

    // РЕЖИМ ИСТОРИИ: Ctrl+F ищет и по показанному слепку, и по всей истории
    // этой заметки (решение владельца). Подсветка и F3 остаются на слепке —
    // это то, на что человек смотрит, — а список внизу показывает, в каких ещё
    // слепках встречается искомое.
    const auto updateHistorySearch = [&](const QString& text) {
        updateInNoteSearch(text);   // подсветка в слепке и счётчик
        const zametti::Query query = zametti::makeQuery(text);
        if (query.isEmpty() || query.tooShort()) {
            results.clear();
            resultsView.hide();
            if (!query.isEmpty()) findBar.setStatus(QStringLiteral("нужно два знака"));
            return;
        }
        const zametti::HistorySearchReport report = history.searchHistory(text);
        results.setResults(report.hits);
        resultsView.setVisible(!report.hits.isEmpty());
        // Счётчик слепка уже написан updateInNoteSearch; дописываем к нему
        // историю, иначе одно из двух чисел молча пропадёт.
        zametti::NoteView& target = searchTarget();
        const QString inSnapshot = target.matchCount() > 0
                                       ? QStringLiteral("%1/%2 в слепке")
                                             .arg(target.currentMatch() + 1)
                                             .arg(target.matchCount())
                                       : QStringLiteral("в слепке нет");
        findBar.setStatus(report.hits.isEmpty()
                              ? inSnapshot + QStringLiteral(", в истории тоже")
                              : QStringLiteral("%1; в истории %2 в %3 слепках%4")
                                    .arg(inSnapshot)
                                    .arg(report.hits.size())
                                    .arg(report.withHits)
                                    .arg(report.truncated ? QStringLiteral(", показаны не все")
                                                          : QString()));
    };

    const auto showCounter = [&] {
        if (markdown.active()) {
            findBar.setStatus(markdownView.matchCount() == 0
                                  ? QStringLiteral("нет совпадений")
                                  : QStringLiteral("%1/%2")
                                        .arg(markdownView.currentMatch() + 1)
                                        .arg(markdownView.matchCount()));
            return;
        }
        zametti::NoteView& target = searchTarget();
        if (target.matchCount() == 0) {
            findBar.setStatus(QStringLiteral("нет совпадений"));
            return;
        }
        findBar.setStatus(QStringLiteral("%1/%2")
                              .arg(target.currentMatch() + 1)
                              .arg(target.matchCount()));
    };

    // СМЕНА ЗАМЕТКИ ПРИ ОТКРЫТОМ ПОИСКЕ — ПОИСК ЗАНОВО (решение владельца).
    // Найденное — курсоры в показанном документе; с новой заметкой они
    // пропадают, а счётчик оставался старым: «2 вхождения» на чужой заметке
    // и «нет совпадений» при возврате на ту, где каретка стоит на находке.
    // Режим истории здесь ни при чём: смена заметки его кончает сама.
    QObject::connect(&editor, &zametti::NoteEditor::fileChanged, &window, [&](const QString&) {
        if (findBar.isHidden()) return;
        if (findBar.mode() == zametti::FindBar::Mode::InNote ||
            findBar.mode() == zametti::FindBar::Mode::Replace)
            updateInNoteSearch(findBar.query());
    });

    QObject::connect(&findBar, &zametti::FindBar::queryChanged, &window,
                     [&](const QString& text) {
        if (findBar.mode() == zametti::FindBar::Mode::History) {
            updateHistorySearch(text);
            return;
        }
        if (findBar.mode() == zametti::FindBar::Mode::Global) {
            const zametti::Query query = zametti::makeQuery(text);
            if (query.isEmpty() || query.tooShort()) {
                storeSearch.cancel();
                searchDebounce.stop();
                results.clear();
                findBar.setStatus(query.isEmpty() ? QString()
                                                  : QStringLiteral("нужно два знака"));
                return;
            }
            searchDebounce.start();
            return;
        }
        updateInNoteSearch(text);
    });

    QObject::connect(&searchDebounce, &QTimer::timeout, &window, [&] {
        storeSearch.search(searchRoot(), findBar.query());
    });

    QObject::connect(&storeSearch, &zametti::StoreSearch::found, &window,
                     [&](const QString& text, const QVector<zametti::SearchResult>& found,
                         bool truncated, qint64 elapsedMs) {
        // Ответ мог прийти на запрос, который уже никому не нужен: пока он
        // бежал, в поле успели дописать. Отсеиваем по самому запросу.
        if (text != findBar.query()) return;
        results.setResults(found);
        resultsView.setVisible(findBar.mode() == zametti::FindBar::Mode::Global);
        int notes = 0;
        for (int row = 0; row < results.rowCount(); ++row)
            if (results.isHeader(results.index(row, 0))) ++notes;
        if (found.isEmpty()) {
            findBar.setStatus(QStringLiteral("ничего"));
            return;
        }
        findBar.setStatus(truncated
                              ? QStringLiteral("%1+ в %2, показаны не все")
                                    .arg(found.size())
                                    .arg(notes)
                              : QStringLiteral("%1 в %2 за %3 мс")
                                    .arg(found.size())
                                    .arg(notes)
                                    .arg(elapsedMs));
    });

    // Показать найденное: открыть заметку и встать ровно на то совпадение, по
    // которому щёлкнули. Считаем по порядковому номеру внутри заметки, а не по
    // смещению: в документе текст блока и текст IR — одно и то же, а вот
    // смещения от начала файла у них разные.
    const auto openResult = [&](const QModelIndex& index) {
        if (!index.isValid() || results.isHeader(index)) return;
        const QString file = index.data(zametti::SearchResultsModel::PathRole).toString();
        const int ordinal = index.data(zametti::SearchResultsModel::OrdinalRole).toInt();

        // Находка в слепке истории. Запись ищем по паре (время, отпечаток), а
        // не по номеру: чистка журнала выкидывает дубликаты, и номера съезжают.
        const qint64 stamp =
            index.data(zametti::SearchResultsModel::SnapshotTimeRole).toLongLong();
        if (stamp > 0) {
            const QByteArray raw =
                index.data(zametti::SearchResultsModel::SnapshotDigestRole).toByteArray();
            zametti::Digest digest;
            if (raw.size() == qsizetype(digest.bytes.size()))
                std::memcpy(digest.bytes.data(), raw.constData(), digest.bytes.size());
            const std::shared_ptr<zametti::ZNoteTimeline> tl = history.timeline();
            const int at = tl != nullptr
                               ? zametti::journal::indexOfEntry(tl->journal(), stamp, digest)
                               : -1;
            if (at >= 0) history.enter(at);
            const zametti::Query query = zametti::makeQuery(findBar.query());
            zametti::NoteView& target = searchTarget();
            target.findMatches(query.needle, query.caseSensitive);
            target.goToMatch(ordinal);
            return;
        }
        if (file.isEmpty()) return;
        // Показать заметку в боковых колонках — общий путь через fileChanged,
        // отдельного кода здесь больше не нужно.
        //
        // Фокус НЕ забираем — второе и последнее исключение из правила «открыли
        // заметку, каретка в тексте». Сюда попадают и щелчок по находке, и
        // ходьба по списку находок, и просто набор в строке поиска: список
        // перестраивается на каждую букву, текущая строка меняется сама, и
        // утащить фокус значило бы выдернуть строку поиска из-под пальцев.
        if (file != editor.filePath()) editor.openFile(file, false);
        const zametti::Query query = zametti::makeQuery(findBar.query());
        editor.findMatches(query.needle, query.caseSensitive);
        editor.goToMatch(ordinal);
    };
    QObject::connect(&resultsView, &QAbstractItemView::clicked, &window, openResult);
    QObject::connect(resultsView.selectionModel(), &QItemSelectionModel::currentChanged,
                     &window, [&](const QModelIndex& index, const QModelIndex&) {
                         openResult(index);
                     });

    const auto stepSearch = [&](int direction) {
        if (findBar.isHidden()) return;
        if (markdown.active()) {
            markdownView.stepMatch(direction);
            showCounter();
            return;
        }
        // В режиме истории F3 ходит по находкам ПОКАЗАННОГО СЛЕПКА: список
        // внизу про другие слепки, и прыгать по нему клавишей означало бы
        // менять показанную запись на каждое нажатие.
        if (findBar.mode() == zametti::FindBar::Mode::Global) {
            if (results.rowCount() == 0) return;
            const QModelIndex at = resultsView.currentIndex();
            const QModelIndex next =
                results.firstHit(at.isValid() ? at.row() + direction : 0, direction);
            if (next.isValid()) {
                resultsView.setCurrentIndex(next);
                resultsView.scrollTo(next);
            }
            return;
        }
        searchTarget().stepMatch(direction);
        showCounter();
    };
    QObject::connect(&findBar, &zametti::FindBar::findNext, &window, [&] { stepSearch(1); });
    QObject::connect(&findBar, &zametti::FindBar::findPrevious, &window,
                     [&] { stepSearch(-1); });

    QObject::connect(&findBar, &zametti::FindBar::replaceOne, &window, [&] {
        if (markdown.active()) {
            // Замена в режиме — обычная правка текста: она ложится в СВОЙ буфер
            // отмены режима, а в заметку попадёт одним куском при выходе.
            markdownView.replaceCurrentMatch(findBar.replacement());
            const zametti::Query query = zametti::makeQuery(findBar.query());
            markdownView.findMatches(query.needle, query.caseSensitive);
            showCounter();
            return;
        }
        if (editor.currentMatch() < 0) editor.stepMatch(1);
        editor.replaceCurrentMatch(findBar.replacement());
        showCounter();
    });
    QObject::connect(&findBar, &zametti::FindBar::replaceAll, &window, [&] {
        const zametti::Query query = zametti::makeQuery(findBar.query());
        if (query.isEmpty()) return;
        if (markdown.active()) {
            findBar.setStatus(QStringLiteral("заменено: %1")
                                  .arg(markdownView.replaceAllMatches(query.needle, query.caseSensitive,
                                                               findBar.replacement())));
            return;
        }
        const int replaced =
            editor.replaceAllMatches(query.needle, query.caseSensitive, findBar.replacement());
        findBar.setStatus(QStringLiteral("заменено: %1").arg(replaced));
    });

    QObject::connect(&findBar, &zametti::FindBar::closed, &window, [&] {
        editor.clearMatches();
        markdownView.clearMatches();
        historyView.textView().clearMatches();
        storeSearch.cancel();
        searchDebounce.stop();
        resultsView.hide();
        results.clear();
        if (markdown.active()) {
            markdownView.setFocus();
            return;
        }
        searchTarget().setFocus();
    });

    const auto openFind = [&](zametti::FindBar::Mode requested) {
        // В РЕЖИМЕ ИСТОРИИ ЗАМЕНЫ НЕТ ПО ПОСТРОЕНИЮ: слепок только для чтения.
        // Ctrl+H там открывает обычный поиск, а не отказывается молча.
        zametti::FindBar::Mode mode = requested;
        if (history.active() && (mode == zametti::FindBar::Mode::InNote ||
                                 mode == zametti::FindBar::Mode::Replace))
            mode = zametti::FindBar::Mode::History;
        const bool global = mode == zametti::FindBar::Mode::Global ||
                            mode == zametti::FindBar::Mode::History;
        if (!global) {
            resultsView.hide();
            results.clear();
            storeSearch.cancel();
        } else {
            searchTarget().clearMatches();
        }
        // Выделенное в редакторе (или в слепке, или в исходнике) — готовый
        // запрос: чаще всего ищут именно то, на что смотрят.
        QString preset = markdown.active() ? markdownView.textCursor().selectedText()
                                           : searchTarget().textCursor().selectedText();
        if (preset.contains(QChar::ParagraphSeparator)) preset.clear();
        findBar.open(mode, preset);
    };
    shortcut(QKeySequence::Find, [&] { openFind(zametti::FindBar::Mode::InNote); });
    shortcut(QKeySequence::Replace, [&] { openFind(zametti::FindBar::Mode::Replace); });
    // Поиск по всем заметкам: и кнопкой тулбара, и сочетанием — одним кодом.
    const auto openStoreFind = [&] {
        if (!model.isStore()) return;
        // Поиск по истории ВСЕХ заметок в этот этап не входит (решение
        // владельца: пер-заметочный сильно быстрее и востребованнее). Молчать
        // нельзя — человек нажал и не увидел бы ничего.
        if (history.active()) {
            statusBar.setMessage(
                QStringLiteral("поиск по истории всех заметок пока не поддерживается"));
            QTimer::singleShot(3000, &statusBar,
                               [&statusBar] { statusBar.setMessage(QString()); });
            return;
        }
        openFind(zametti::FindBar::Mode::Global);
    };
    shortcut(QKeySequence(QStringLiteral("Ctrl+Shift+F")), openStoreFind);
    // Правка исходника — сочетание из настроек (editor.markdownModeKey), список
    // через точку с запятой, как у всех прочих команд.
    for (const QKeySequence& keys :
         zametti::keySequencesOf(zametti::settings().editor().markdownModeKey()))
        shortcut(keys, [&] { markdown.toggle(); });
    shortcut(QKeySequence(Qt::Key_F3), [&] { stepSearch(1); });
    shortcut(QKeySequence(Qt::SHIFT | Qt::Key_F3), [&] { stepSearch(-1); });

    // Клавиши режима истории (ходьба по изменениям) ставит контроллер — он же
    // зовётся из набора.
    history.installShortcuts(&window);


    // --- тулбар --------------------------------------------------------------
    //
    // Кнопки не делают ничего своего: каждая зовёт то же самое, что и ярлык или
    // пункт меню. Иначе тулбар стал бы вторым набором правил.
    // Ни одна из этих переменных не может жить во вложенном блоке: лямбда,
    // отданная в connect, переживает блок и держала бы висячие ссылки. Поэтому
    // всё, что она захватывает, объявлено на уровне main — как и остальное окно.
    using Button = zametti::Toolbar::Button;

    // Панели убираются и возвращаются одной кнопкой. Ширины запоминаются ПЕРЕД
    // тем, как прятать: сплиттер хранит размеры видимых виджетов, и спрятанные
    // панели вернулись бы схлопнутыми.
    QList<int> keptSizes = splitter.sizes();
    const auto showPanels = [&](bool visible) {
        if (!visible) keptSizes = splitter.sizes();
        panels.setVisible(visible);
        if (visible && keptSizes.size() == splitter.count()) splitter.setSizes(keptSizes);
        // Кнопка НАЖАТА, когда панели видны, а не наоборот: нажатый
        // переключатель означает «это включено». Прежде было зеркально —
        // панели пропадали, а кнопка загоралась.
        toolbar.setChecked(Button::Panels, visible);
        toolbar.buttonFor(Button::Panels)
            ->setToolTip(visible ? QStringLiteral("Скрыть боковые панели")
                                 : QStringLiteral("Показать боковые панели"));
    };
    {
        // Зовём ВСЕГДА, а не только когда панели спрятаны: кнопка обязана
        // показывать своё состояние с первой секунды, а не с первого нажатия.
        showPanels(!session.panelsHidden());

        // Обещания. Погашенная кнопка без объяснения читается как поломка, а
        // не как «будет позже», поэтому у каждой — своя причина словами.
        toolbar.setPromise(Button::Cloud,
                           QStringLiteral("появится вместе с синхронизацией"));

        if (!model.isStore()) {
            // Открыт одиночный файл, а не хранилище: создавать и сортировать
            // нечего и негде. Это не «пока не сделано», а другое состояние мира.
            // Поиск по всем заметкам сюда же: искать не по чему.
            const QString single = QStringLiteral("открыт один файл, а не хранилище");
            // История живёт в хранилище (history/<id>.log), и у одиночного
            // файла её нет вовсе — это не «пока не сделано», а другое
            // состояние мира.
            for (Button id : {Button::NewNote, Button::NewFolder, Button::ImportNotes,
                              Button::SortByName, Button::SortByDate, Button::SortByCreated,
                              Button::SearchInStore, Button::History})
                toolbar.setPromise(id, single);
        }

        QObject::connect(&toolbar, &zametti::Toolbar::pressed, &window, [&](Button id) {
            switch (id) {
            case Button::NewNote:
                createNote(panels.currentFolderId(), false);
                break;
            case Button::NewFolder:
                createNote(panels.currentFolderId(), true);
                break;
            case Button::ImportNotes:
                importNotes(panels.currentFolderId());
                break;
            case Button::InsertImages:
                editor.chooseAndInsertImages();
                break;
            case Button::Panels:
                showPanels(toolbar.isChecked(Button::Panels));
                break;
            case Button::SortByName:
                pressSort(zametti::SortKey::Name);
                break;
            case Button::SortByDate:
                pressSort(zametti::SortKey::Modified);
                break;
            case Button::SortByCreated:
                pressSort(zametti::SortKey::Created);
                break;
            case Button::MarkdownEdit:
                // ПЕРЕКЛЮЧАТЕЛЬ, как и история: горит — заметка показана
                // исходником. Кнопку в согласие с режимом приводит modeChanged
                // (вход бывает и с клавиши), здесь только просьба переключить.
                markdown.toggle();
                break;
            case Button::History:
                // ПЕРЕКЛЮЧАТЕЛЬ: горит — идёт режим истории, нажали снова —
                // вернулись к текущей версии. Прямой вход, минуя
                // внутридокументный стек отмены; поиск кнопка НЕ включает —
                // она открывает режим, а искать в слепке отдельный жест
                // (Ctrl+F).
                if (!toolbar.isChecked(Button::History)) {
                    history.leave();
                    break;
                }
                if (!history.enter()) {
                    toolbar.setChecked(Button::History, false);
                    // Случай редкий (опорная запись кладётся при открытии
                    // заметки), но молчать нельзя: нажали — не случилось
                    // ничего. Сообщение само уходит: полоса сведений нужна ей
                    // самой, а не нашей жалобе.
                    statusBar.setMessage(QStringLiteral("у этой заметки истории пока нет"));
                    QTimer::singleShot(3000, &statusBar,
                                       [&statusBar] { statusBar.setMessage(QString()); });
                }
                break;
            case Button::Search:
                openFind(zametti::FindBar::Mode::InNote);
                break;
            case Button::SearchInStore:
                // Ровно то же, что Ctrl+Shift+F: одна дверь на кнопку и на
                // сочетание клавиш, иначе они разойдутся.
                openStoreFind();
                break;
            case Button::Help: {
                // Одно окно на программу: второе нажатие поднимает открытое, а
                // не заводит близнеца.
                static QPointer<zametti::AboutWindow> about;
                if (about.isNull()) about = new zametti::AboutWindow(&window);
                about->show();
                about->raise();
                about->activateWindow();
                break;
            }
            case Button::Settings: {
                // Конфига может не быть вовсе: тогда пишем шаблон — весь
                // список параметров, целиком закомментированный. Открывать
                // человеку пустоту и предлагать «наберите сами» нельзя.
                QString error;
                if (!zametti::writeConfigTemplate(&error)) {
                    QMessageBox::warning(&window, QStringLiteral("zametti"), error);
                    break;
                }
                const QString path = zametti::configPath();
                if (!QDesktopServices::openUrl(QUrl::fromLocalFile(path))) {
                    // Внешнего редактора не нашлось — молчать нельзя: человек
                    // нажал кнопку и не увидел бы ничего.
                    QMessageBox::warning(
                        &window, QStringLiteral("zametti"),
                        QStringLiteral("Не удалось открыть конфиг во внешнем редакторе.\n%1")
                            .arg(path));
                }
                break;
            }
            case Button::Export:
                exportNote(editor.filePath());
                break;
            case Button::Cloud:
                break;   // обещание: кнопка погашена, сюда не доходит
            }
        });
    }
    {
        // Esc закрывает панель, откуда бы ни нажали: в самой панели его ловит
        // её keyPressEvent, а из редактора — этот ярлык.
        auto* escape = new QShortcut(QKeySequence(Qt::Key_Escape), &window);
        QObject::connect(escape, &QShortcut::activated, &window, [&] {
            // ОДНА ДВЕРЬ НА КЛАВИШУ. Ярлык окна срабатывает раньше, чем
            // нажатие доходит до виджета с фокусом, — поэтому Esc в поле ввода
            // языка не отменял ввод, а просто пропадал (нашёл владелец). Сам
            // порядок живёт в escapeActionFor: до лямбды внутри main() набор
            // не дотягивается, а до функции — вполне.
            switch (zametti::escapeActionFor(editor.codeLanguageEditor() != nullptr,
                                             editor.caretInOpenObject(), !findBar.isHidden())) {
                case zametti::EscapeAction::CloseLanguageEditor:
                    editor.closeCodeLanguageEditor();
                    return;
                case zametti::EscapeAction::CloseObject:
                    editor.closeOpenObject();
                    return;
                case zametti::EscapeAction::CloseFindBar:
                    findBar.hide();
                    emit findBar.closed();
                    return;
                case zametti::EscapeAction::Nothing:
                    return;
            }
        });
    }

    // Фокус ушёл из приложения — момент, когда человек переключился на что-то
    // другое и меньше всего ждёт потери правок.
    QObject::connect(&app, &QGuiApplication::focusWindowChanged, &window,
                     [&](QWindow* focused) {
                         if (focused == nullptr) editor.save(false);
                     });

    if (!session.windowGeometry().isEmpty()) window.restoreGeometry(session.windowGeometry());
    else window.resize(1150, 780);
    if (!session.splitterState().isEmpty()) splitter.restoreState(session.splitterState());
    else if (model.isStore())
        splitter.setSizes({zametti::settings().ui().sidebarWidth(),
                           zametti::settings().ui().noteListWidth(), 700});
    else
        splitter.setSizes({zametti::settings().ui().sidebarWidth(), 800});
    window.show();

    // Прореживание журналов — фоном и один раз за запуск. В отдельном потоке
    // потому, что полный проход по корпусу владельца стоит 1.6 секунды, а
    // держать окно неподвижным столько времени ради уборки истории незачем.
    //
    // Запирать журналы не нужно: thin перед подменой файла сверяет его размер
    // и время правки с теми, что видел на чтении, и молча отступает, если под
    // руками дописали запись. Поэтому поток не знает ни про окно, ни про
    // открытую заметку — он трогает только файлы в history/.
    if (model.isStore()) {
        const QString storePath = model.nodePath(QModelIndex());
        QThreadPool::globalInstance()->start([storePath] {
            zametti::journal::History history(storePath);
            const zametti::journal::ThinReport report =
                history.thinAll(QDateTime::currentMSecsSinceEpoch());
            for (const QString& name : report.trimmed)
                std::fprintf(stderr, "журнал %s: оборванный хвост отрезан\n",
                             name.toUtf8().constData());
            for (const QString& line : report.problems)
                std::fprintf(stderr, "журнал не прорежен: %s\n", line.toUtf8().constData());
            if (report.recordsBefore != report.recordsAfter)
                std::fprintf(stderr, "журналы прорежены: записей %lld -> %lld, байт %lld -> %lld\n",
                             (long long)report.recordsBefore, (long long)report.recordsAfter,
                             (long long)report.bytesBefore, (long long)report.bytesAfter);
        });
    }

    // Показать текущую заметку в дереве надо после show(): раскрытие веток
    // требует уже созданных представлений. Раскрытые ветки восстанавливаем до
    // того, как показать текущую заметку, иначе её раскрытие затеряется среди
    // прочих. Показ — без вопросов к правилу курсора: слева выделяется папка
    // открытой заметки (её порядок — и на старте тоже), в середине — сама
    // заметка; вне хранилища — строка заметки в дереве.
    panels.restoreExpanded(session.expandedDirs());
    panels.showNote(current, /*primary=*/true);

    // Фокус — после show() и после того, как дерево показало текущую заметку: до
    // show() окно ещё не решило, кому его отдать, и наш выбор затёрся бы первым
    // же виджетом в разделителе. Каретка при запуске должна быть сразу в тексте:
    // дерево нужно, чтобы выбрать заметку, а не чтобы в нём находиться.
    editor.setFocus();

    // РЕЖИМ ПРАВКИ ИСХОДНИКА ПЕРЕЖИВАЕТ ПЕРЕЗАПУСК (решение владельца): вышли
    // из программы с нажатой [M] — вернулись в неё же. После открытия заметки и
    // после фокуса: входить в режим нечем, пока показывать нечего.
    markdownView.applyZoom(session.markdownZoom());
    if (session.markdownMode()) markdown.enter();



    QObject::connect(&app, &QCoreApplication::aboutToQuit, &window, [&] {
        // ИСХОДНИК НАКЛАДЫВАЕМ ДО ЗАПИСИ. Пока идёт режим, истина живёт в тексте
        // вида, и заметка о ней не знает: записать её первой значило бы
        // потерять всё, что человек набрал перед выходом.
        if (markdown.active()) markdown.saveWithoutLeaving();
        // На выходе окно с ошибкой показывать поздно: жалуемся в stderr.
        editor.save(false, true);   // выходим: пробуем записать, не спрашивая признак

        // Каретка открытой заметки — в состояние по id, как у всех остальных.
        editor.rememberCurrentCaretInApp();
        zametti::ZAppState& out = zapp.state();
        out.setLastFile(editor.filePath());
        out.setCaret(editor.caretPosition());
        out.setAnchor(editor.caretAnchor());
        out.setZoom(editor.zoom());
        out.setWindowGeometry(window.saveGeometry());
        out.setSplitterState(splitter.saveState());
        out.setHistoryListWidth(historyListWidth);
        out.setPanelsHidden(!toolbar.isChecked(zametti::Toolbar::Button::Panels));
        out.setMarkdownMode(markdown.active());
        out.setMarkdownZoom(markdownView.zoom());
        out.setExpandedDirs(panels.expandedDirs());
        out.setSearchHistory(findBar.history());
        out.setStoreRoot(model.isStore() ? model.nodePath(QModelIndex()) : QString());
        out.setExportDir(exportDir);
        out.setExportKeepMeta(exportKeepMeta);
        // Переключатель КОРНЯ, а не действующий порядок: последний может быть
        // задан меткой открытой папки, и запиши мы его — чужая метка стала бы
        // общим умолчанием при следующем запуске.
        out.setTreeSort(zametti::sortOrderToString(panels.rootSort()));
        zapp.saveState();
    });

    // ПРОБНИК ВЫХОДА (не пользовательский ключ): ZAMETTI_PROBE_QUIT_MS=N — выйти
    // через N мс штатным путём, ZAMETTI_PROBE_HISTORY=1 — перед этим войти в
    // режим истории. Нужен, чтобы порядок разрушения окна (виджеты на стеке,
    // родители и дети, документы разности) проверялся запуском под Xvfb, а не
    // рассуждением: двойное освобождение на выходе однажды нашёл владелец, а не
    // набор — набор окна целиком не собирает.
    // ZAMETTI_PROBE_SHOT=<файл.png> — снимок окна перед выходом (приёмка на
    // копии хранилища владельца, глазами, а не рассуждением).
    if (const QByteArray quitAfter = qgetenv("ZAMETTI_PROBE_QUIT_MS"); !quitAfter.isEmpty()) {
        const int ms = qMax(0, quitAfter.toInt());
        if (qEnvironmentVariableIsSet("ZAMETTI_PROBE_HISTORY"))
            QTimer::singleShot(ms / 2, &window, [&] {
                std::fprintf(stderr, "пробник: режим истории %s\n",
                             history.enter() ? "включён" : "не включился");
            });
        if (const QByteArray shot = qgetenv("ZAMETTI_PROBE_SHOT"); !shot.isEmpty())
            QTimer::singleShot(ms * 3 / 4, &window, [&window, shot] {
                window.grab().save(QString::fromLocal8Bit(shot));
            });
        QTimer::singleShot(ms, &app, &QCoreApplication::quit);
    }
    return app.exec();
}
