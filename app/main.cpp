// Просмотрщик: дерево заметок слева, отрендеренный документ справа.
//
// Это не самоцель, а первый стенд для проверки ядра. Отсюда же работает режим
// --check: прогнать parse → serialize и показать расхождение с оригиналом.

#include "doc_model.h"
#include "editor_widget.h"
#include "find_bar.h"
#include "history_panel.h"
#include "note_list.h"
#include "note_tree.h"
#include "search.h"
#include "search_results.h"
#include "store_search.h"
#include "journal.h"
#include "store.h"
#include "note_view.h"
#include "parser.h"
#include "resources.h"
#include "serializer.h"
#include "settings.h"
#include "about_window.h"
#include "status_bar.h"
#include "toolbar.h"

#include <QApplication>
#include <QFileInfo>
#include <QFont>
#include <QIcon>
#include <QItemSelectionModel>
#include <QKeySequence>
#include <QListView>
#include <QMenu>
#include <QKeyEvent>
#include <QFileDialog>
#include <QMessageBox>
#include <QDesktopServices>
#include <QDateTime>
#include <QLockFile>
#include <QSysInfo>
#include <QProcess>
#include <QThreadPool>
#include <QUrl>
#include <QPushButton>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTextBlock>
#include <QTextCursor>
#include <QFile>
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

    std::string out = zametti::serialize(zametti::parse(src));
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

// Жив ли процесс с таким номером. Сигнал 0 ничего не посылает, а только
// проверяет право послать: единственный переносимый по UNIX способ спросить
// «этот pid ещё существует?».
bool processAlive(qint64 pid) {
#ifdef Q_OS_UNIX
    return ::kill(pid_t(pid), 0) == 0 || errno == EPERM;
#else
    Q_UNUSED(pid);
    return true;   // на прочих системах не гадаем: пусть решает --unlock
#endif
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
        "  Ctrl+K            зачёркнутый\n"
        "  Ctrl+E            код в строке; он же выходит из кавычек при наборе\n"
        "  Ctrl+Shift+E      выделенное в блок кода и обратно в текст\n"
        "\n"
        "Файлы:\n"
        "  %s\n"
        "      оформление; приложение его только читает, править вручную.\n"
        "      Полный список параметров — по ключу --dump-config\n"
        "  %s\n"
        "      последняя заметка, прокрутка, зум, геометрия окна, раскрытые ветки;\n"
        "      переписывается при выходе\n",
        keysFor(zametti::appearance().toggleTaskKey).constData(),
        padFor(zametti::appearance().toggleTaskKey).constData(),
        keysFor(zametti::appearance().moveUpKey).constData(),
        padFor(zametti::appearance().moveUpKey).constData(),
        keysFor(zametti::appearance().moveDownKey).constData(),
        padFor(zametti::appearance().moveDownKey).constData(),
        keysFor(zametti::appearance().makeBulletKey).constData(),
        padFor(zametti::appearance().makeBulletKey).constData(),
        keysFor(zametti::appearance().makeOrderedKey).constData(),
        padFor(zametti::appearance().makeOrderedKey).constData(),
        keysFor(zametti::appearance().makeTaskKey).constData(),
        padFor(zametti::appearance().makeTaskKey).constData(),
        keysFor(zametti::appearance().makeParagraphKey).constData(),
        padFor(zametti::appearance().makeParagraphKey).constData(),
        zametti::configPath().toUtf8().constData(),
        zametti::statePath().toUtf8().constData());
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

    // Ни --dump-config, ни --check не должны требовать дисплея: они работают в
    // конвейерах и в CI.
    if (dumpConfig) {
        const QByteArray json = zametti::defaultAppearanceJson();
        std::fwrite(json.constData(), 1, size_t(json.size()), stdout);
        return 0;
    }
    if (check) {
        if (path.isEmpty()) {
            printUsage();
            return 2;
        }
        return runCheck(path);
    }

    QApplication app(argc, argv);
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

    // --noconfig нужен, чтобы посмотреть на вид по умолчанию, не убирая свой
    // конфиг: удобно и при правке конфига, и при разговоре о том, «как оно
    // выглядит из коробки».
    QString configError;
    if (!noConfig && !zametti::loadAppearance(&configError)) {
        // Молча подставить умолчания нельзя: опечатка в конфиге выглядела бы
        // как «настройка не работает».
        std::fprintf(stderr, "конфиг не разобран, взяты значения по умолчанию:\n  %s\n",
                     configError.toUtf8().constData());
    }

    const zametti::Session session = zametti::loadSession();

    // Без аргумента открываем то, что читали в прошлый раз. С --root — свежую
    // заметку хранилища (или прошлую, если она из этого же хранилища).
    // Хранилище прошлого запуска запоминается: без параметров возвращаемся
    // в него, ключ --root каждый раз не нужен.
    if (path.isEmpty()) path = session.lastFile;
    if (storeRoot.isEmpty() && path.isEmpty() && !session.storeRoot.isEmpty() &&
        zametti::NoteTreeModel::isStoreRoot(session.storeRoot))
        storeRoot = session.storeRoot;
    if (storeRoot.isEmpty() && !path.isEmpty() && !session.storeRoot.isEmpty() &&
        QFileInfo(path).absoluteFilePath().startsWith(
            QFileInfo(session.storeRoot).absoluteFilePath()) &&
        zametti::NoteTreeModel::isStoreRoot(session.storeRoot))
        storeRoot = session.storeRoot;
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
    QWidget middle;
    QWidget rightSide;

    zametti::NoteTreeView tree;
    zametti::NoteEditor editor;
    zametti::NoteListModel list;
    QListView listView;

    zametti::NoteTreeModel model(
        storeRoot.isEmpty()
            ? zametti::NoteTreeModel::rootFor(current, zametti::appearance().notesRoot)
            : QFileInfo(storeRoot).absoluteFilePath());
    // Левая панель — только папки (этап 4). Заметки живут в средней колонке;
    // из дерева они не пропадают, но наружу не показываются.
    model.setFoldersOnly(model.isStore());

    // Редактор узнаёт своё хранилище: без него истории правок не будет вовсе
    // (одиночный файл, открытый вне хранилища, журналу негде лежать).
    editor.setStoreRoot(model.isStore() ? model.nodePath(QModelIndex()) : QString());

    // Одно хранилище — одна программа. Второй экземпляр на том же хранилище
    // писал бы в те же файлы и те же журналы, ничего не зная о первом, поэтому
    // он просто не запускается. Замок файловый, потому что процессы разные;
    // внутри процесса потоки разводит замок самого журнала. Стоит это 4.4 мс
    // один раз за запуск. Другое хранилище открыть вторым окном по-прежнему
    // можно: замок лежит внутри хранилища.
    //
    // Забытый замок после падения программы не беда: QLockFile хранит в нём
    // pid и имя машины и снимает замок, чей процесс не жив.
    static QLockFile storeLock(
        zametti::journal::storeLockPath(model.isStore() ? model.nodePath(QModelIndex())
                                                        : QDir::tempPath()));
    if (model.isStore()) {
        // --unlock: снять забытый замок. Обычно он снимается сам, но бывает,
        // что QLockFile судить не берётся — тот же pid достался чужому
        // процессу, хранилище на сетевой шаре. Тогда ключ решает спор руками.
        if (unlock) {
            qint64 pid = 0;
            QString host, appName;
            if (storeLock.getLockInfo(&pid, &host, &appName))
                std::fprintf(stderr, "снимаю замок хранилища (был за pid %lld на «%s»)\n",
                             (long long)pid, host.toUtf8().constData());
            else
                std::fprintf(stderr, "замка на хранилище и не было\n");
            QFile::remove(zametti::journal::storeLockPath(model.nodePath(QModelIndex())));
        }
        // QLockFile сам снимает забытый замок только через полминуты, а
        // перезапуск сразу после падения — самый частый случай. Поэтому
        // спрашиваем сами: если замок нашей машины, а процесса с таким pid уже
        // нет, значит это наш собственный труп — снимаем и продолжаем. Живой
        // pid не трогаем никогда.
        if (!storeLock.tryLock(0)) {
            qint64 pid = 0;
            QString host, appName;
            if (storeLock.getLockInfo(&pid, &host, &appName) &&
                host == QSysInfo::machineHostName() && pid > 0 && !processAlive(pid)) {
                std::fprintf(stderr, "снимаю забытый замок хранилища (pid %lld не жив)\n",
                             (long long)pid);
                QFile::remove(zametti::journal::storeLockPath(model.nodePath(QModelIndex())));
            }
        }
        if (!storeLock.isLocked() && !storeLock.tryLock(0)) {
            qint64 pid = 0;
            QString host, appName;
            storeLock.getLockInfo(&pid, &host, &appName);
            std::fprintf(stderr,
                         "это хранилище уже открыто другой копией zametti:\n  %s\n"
                         "  замок держит pid %lld на «%s»\n"
                         "Если та копия давно умерла: zametti --root … --unlock\n",
                         model.nodePath(QModelIndex()).toUtf8().constData(), (long long)pid,
                         host.toUtf8().constData());
            return 3;
        }
    }

    // Свежая заметка хранилища — первая ОТКРЫВАЕМАЯ (директории не в счёт),
    // поиском в глубину; пустое хранилище получает первую заметку тут же.
    if (current.isEmpty()) {
        QString first = model.firstNoteId();
        if (first.isEmpty()) {
            QString newError;
            const QString made = zametti::store::newNote(
                QFileInfo(storeRoot).absoluteFilePath(), QString(), &newError);
            if (made.isEmpty()) {
                std::fprintf(stderr, "%s\n", newError.toUtf8().constData());
                return 2;
            }
            model.refresh();
            first = model.firstNoteId();
        }
        current = model.pathOfId(first);
        if (current.isEmpty()) {
            std::fprintf(stderr, "в хранилище нет ни одной открываемой заметки\n");
            return 2;
        }
    }
    if (session.treeSort == QStringLiteral("name")) {
        model.setSortMode(zametti::NoteTreeModel::SortMode::ByName);
        list.setSortMode(zametti::NoteTreeModel::SortMode::ByName);
    }
    tree.setModel(&model);
    tree.setHeaderHidden(true);
    tree.setEditTriggers(model.isStore() ? QAbstractItemView::EditKeyPressed
                                         : QAbstractItemView::NoEditTriggers);
    if (model.isStore()) {
        // Не InternalMove: заметку тащат из средней колонки, а это другая
        // модель — для дерева такой перенос внешний.
        tree.setDragDropMode(QAbstractItemView::DragDrop);
        tree.setDefaultDropAction(Qt::MoveAction);
        tree.setDropIndicatorShown(true);
        tree.setAcceptDrops(true);
    }
    tree.setUniformRowHeights(true);

    QFont sidebarFont(zametti::appearance().sidebarFontFamily.isEmpty()
                          ? zametti::appearance().fontFamily
                          : zametti::appearance().sidebarFontFamily);
    sidebarFont.setPointSizeF(zametti::appearance().sidebarFontPoint);
    tree.setFont(sidebarFont);

    zametti::NoteTreeDelegate delegate;
    tree.setItemDelegate(&delegate);

    QObject::connect(&tree, &QTreeView::expanded, &tree,
                     [&model](const QModelIndex& i) { model.setExpanded(i, true); });
    QObject::connect(&tree, &QTreeView::collapsed, &tree,
                     [&model](const QModelIndex& i) { model.setExpanded(i, false); });

    // Средняя колонка: плоский список заметок целиком. Переключатель сортировки
    // стоял здесь комбобоксом, а теперь живёт на тулбаре парой кнопок:
    // сортировка одна на обе панели, и место ей над всем окном, а не над одной
    // из колонок.
    zametti::NoteListDelegate listDelegate;
    {
        auto* layout = new QVBoxLayout(&middle);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);
        listView.setModel(&list);
        listView.setItemDelegate(&listDelegate);
        listView.setFont(sidebarFont);
        listView.setUniformItemSizes(false);   // высота строки зависит от сниппета
        // Горизонтальной прокрутки в списке быть не должно: строка и так
        // укорачивается по ширине, а полоса отъедала правый край — даты
        // обрезались (замерено на снимке).
        listView.setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        listView.setSelectionMode(QAbstractItemView::SingleSelection);
        listView.setDragEnabled(true);         // перетащить заметку на папку слева
        listView.setDragDropMode(QAbstractItemView::DragOnly);
        listView.setContextMenuPolicy(Qt::CustomContextMenu);
        layout->addWidget(&listView, 1);
    }

    // Правая сторона — заметка, под ней список найденного (появляется только у
    // поиска по всему хранилищу) и панель поиска у самого низа, как в Sublime.
    zametti::FindBar findBar;
    zametti::HistoryBanner historyBanner;
    zametti::HistoryTimeline historyTimeline;
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

        // Режим истории: баннер НАД текстом, таймлайн СБОКУ. Обе части видны
        // всё время режима и обе спрятаны вне его — тем режим и громкий.
        historyBanner.hide();
        historyTimeline.hide();
        historyTimeline.setFont(sidebarFont);
        historyTimeline.setFixedWidth(zametti::appearance().noteListWidth);
        auto* middleRow = new QHBoxLayout;
        middleRow->setContentsMargins(0, 0, 0, 0);
        middleRow->setSpacing(0);
        middleRow->addWidget(&editor, 1);
        middleRow->addWidget(&historyTimeline);

        layout->addWidget(&historyBanner);
        layout->addLayout(middleRow, 1);
        layout->addWidget(&resultsView);
        layout->addWidget(&findBar);
    }
    // Дебаунс по замеру этапа 4: полный проход по хранилищу — 11 мс тёплым и
    // 113 мс на десятикратном корпусе, так что 150 мс успевают проглотить
    // любой из них, а набор не тормозит.
    searchDebounce.setSingleShot(true);
    searchDebounce.setInterval(150);
    findBar.setHistory(session.searchHistory);

    // Облик применяется ОДНИМ местом — и на старте, и когда конфиг поправили
    // снаружи. Два места разошлись бы: половина настроек подхватывалась бы на
    // лету, половина только после перезапуска, и понять, какая именно, было бы
    // нельзя.
    const auto applyAppearance = [&] {
        QFont font(zametti::appearance().sidebarFontFamily.isEmpty()
                       ? zametti::appearance().fontFamily
                       : zametti::appearance().sidebarFontFamily);
        font.setPointSizeF(zametti::appearance().sidebarFontPoint);
        tree.setFont(font);
        listView.setFont(font);
        resultsView.setFont(font);
        historyTimeline.setFont(font);
        historyTimeline.setFixedWidth(zametti::appearance().noteListWidth);

        zametti::applyPalette(editor, editor.inHistory());
        zametti::applyPalette(tree);
        zametti::applyPalette(listView);
        zametti::applyPalette(resultsView);

        toolbar.refreshAppearance();
        statusBar.refreshAppearance();
        editor.refreshAppearance();

        // Делегаты читают настройки прямо при отрисовке — им довольно
        // перерисовки, но размеры строк они считают там же, и без сброса
        // подсказок список остался бы с прежними высотами.
        tree.doItemsLayout();
        listView.doItemsLayout();
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
        if (!zametti::loadAppearance(&error)) {
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
        statusBar.setMessage(QString());
    });
    watchConfig();

    splitter.addWidget(&tree);
    if (model.isStore()) splitter.addWidget(&middle);
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
                const std::string value = editor.meta().get(key);
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
            info.wordsKnown = editor.statsFresh() && editor.stats().valid;
        }
        statusBar.setNote(info);
        const zametti::CaretPlace place = editor.caretPlace();
        statusBar.setCaret(place.line, place.column);
    };
    QObject::connect(&editor, &zametti::NoteEditor::statsChanged, &window, showStats);
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
                         const zametti::ImageFacts facts = editor.caretImage();
                         zametti::StatusBar::ImageInfo shown;
                         shown.valid = facts.valid;
                         shown.name = facts.name;
                         shown.caption = facts.caption;
                         shown.format = facts.format;
                         shown.size = facts.size;
                         shown.bytes = facts.bytes;
                         shown.frames = facts.frames;
                         shown.colorSpace = facts.colorSpace;
                         shown.bits = facts.bits;
                         shown.exists = facts.exists;
                         statusBar.setImage(shown);
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
    // Идёт синхронизация боковых колонок с открытой заметкой. Обработчики
    // выделения на это время молчат: иначе перестановка курсора читалась бы
    // как выбор человека и открывала бы другую заметку.
    bool revealing = false;

    // Открытая заметка видна в обеих боковых колонках, откуда бы её ни
    // открыли: из общего списка «All notes», из результатов поиска, из другой
    // папки. Слева курсор встаёт на папку, где она лежит, и предки
    // раскрываются; в середине она становится текущей строкой.
    //
    // Сигналы обеих панелей при этом заглушены: курсор здесь указатель, а не
    // навигация. Иначе перестановка курсора в дереве перезаполняла бы средний
    // список, и просмотр «всех заметок» схлопывался бы до одной папки при
    // первом же щелчке.
    const auto revealOpenNote = [&](const QString& file) {
        if (!model.isStore() || file.isEmpty()) return;
        const QModelIndex folder =
            model.folderIndexForNote(QFileInfo(file).completeBaseName());
        if (folder.isValid()) {
            // Не QSignalBlocker: замерено пробником, что с заглушенными
            // сигналами курсор дерева не переставляется вовсе — «было=All
            // notes, стало=All notes», — а без глушения встаёт куда надо.
            // Поэтому сигнал идёт как обычно, а его обработчик на время
            // синхронизации выключен флагом: курсор здесь указатель, а не
            // навигация, и средний список от него перезаполняться не должен.
            revealing = true;
            for (QModelIndex up = folder; up.isValid(); up = up.parent()) tree.expand(up);
            tree.setCurrentIndex(folder);
            tree.scrollTo(folder);
            revealing = false;
        }

        // Заметки может не быть в списке вовсе — так бывает, когда из поиска
        // открыли заметку из другой папки. Тогда список пересобирается по той
        // папке, где она лежит: пустая средняя колонка рядом с открытым
        // текстом читалась бы как потеря места.
        QModelIndex row = list.indexForPath(file);
        if (!row.isValid() && folder.isValid()) {
            list.setRows(model.notesInSubtree(folder));
            row = list.indexForPath(file);
        }
        if (!row.isValid()) return;
        revealing = true;
        listView.setCurrentIndex(row);
        listView.scrollTo(row);
        revealing = false;
    };

    // Точка одна: заметку открывает только openFile, и он же говорит об этом
    // сигналом. Связь очередью, а не прямым вызовом, — иначе синхронизация
    // выполнялась бы ВНУТРИ ещё не доигранной смены выделения (щелчок по
    // папке открывает первую заметку прямо из обработчика currentChanged), и
    // та, завершившись, возвращала бы курсор дерева на прежнее место.
    // Замерено пробником: папка находилась верно, но выделение откатывалось.
    QObject::connect(&editor, &zametti::NoteEditor::fileChanged, &window,
                     [&](const QString& file) {
                         window.setWindowTitle(windowTitleFor(file) +
                                               QStringLiteral(" — zametti"));
                         revealOpenNote(file);
                     },
                     Qt::QueuedConnection);

    // Кегль задан явно в каждом формате, поэтому штатный зум QTextEdit до него
    // не дотягивается: при смене масштаба документ собирается заново из того же
    // содержимого. В историю правок это не попадает — облик не содержимое.
    editor.setZoom(std::clamp(session.zoom, zametti::appearance().zoomMin,
                              zametti::appearance().zoomMax));
    if (!editor.openFile(current)) return 2;

    // Средняя колонка наполняется по выбранной слева папке. Открытая заметка,
    // если она в этом поддереве, остаётся выбранной — переключение папки не
    // должно уводить человека с того, что он читает; иначе открывается первая
    // заметка списка (так ведёт себя Apple Notes).
    const auto fillList = [&](const QModelIndex& folder, bool openFirst) {
        if (!model.isStore()) return;
        list.setRows(model.notesInSubtree(folder));
        const QModelIndex keep = list.indexForPath(editor.filePath());
        if (keep.isValid()) {
            const QSignalBlocker blocked(listView.selectionModel());
            listView.setCurrentIndex(keep);
            listView.scrollTo(keep);
            return;
        }
        if (!openFirst || list.rowCount() == 0) return;
        const QModelIndex first = list.index(0, 0);
        {
            const QSignalBlocker blocked(listView.selectionModel());
            listView.setCurrentIndex(first);
        }
        const QString file = list.pathAt(first);
        if (!file.isEmpty() && file != editor.filePath()) editor.openFile(file);
    };

    QObject::connect(tree.selectionModel(), &QItemSelectionModel::currentChanged, &tree,
                     [&](const QModelIndex& index, const QModelIndex&) {
                         if (revealing) return;
                         if (model.isStore()) {
                             fillList(index, true);
                             return;
                         }
                         // Вне хранилища панель одна: заметки живут в дереве.
                         const QString file = model.filePath(index);
                         if (!file.isEmpty() && file != editor.filePath()) editor.openFile(file);
                     });

    // Щелчок по УЖЕ выбранной папке. Курсор мог встать на неё сам — так
    // работает подсветка открытой заметки, — и тогда currentChanged больше не
    // сработает, а сузить список надо: человек ткнул в папку явно и ждёт
    // увидеть только её заметки. Программная перестановка курсора сюда не
    // попадает: clicked приходит только от настоящего щелчка, и по стрелке
    // раскрытия он тоже не приходит.
    QObject::connect(&tree, &QAbstractItemView::clicked, &tree,
                     [&](const QModelIndex& index) {
                         if (model.isStore()) fillList(index, true);
                     });

    // Выбор строки списка открывает заметку. Фокус при этом не переезжает:
    // ↑/↓ должны ходить по списку, а не по тексту (правило средней колонки).
    QObject::connect(listView.selectionModel(), &QItemSelectionModel::currentChanged,
                     &listView, [&](const QModelIndex& index, const QModelIndex&) {
                         if (revealing) return;
                         const QString file = list.pathAt(index);
                         if (!file.isEmpty() && file != editor.filePath()) editor.openFile(file);
                     });

    // Enter в списке — перейти к правке: выбор уже сделан, дальше человек
    // хочет печатать.
    QObject::connect(&listView, &QAbstractItemView::activated, &listView,
                     [&](const QModelIndex&) { editor.setFocus(); });

    // Сохранение переписало файл — заголовок, начало текста и дата в строке
    // списка меняются вслед за ним.
    QObject::connect(&editor, &zametti::NoteEditor::fileSaved, &window,
                     [&](const QString& file) { model.refreshNote(file); });
    QObject::connect(&model, &zametti::NoteTreeModel::noteRowChanged, &window,
                     [&](const QString& id) { list.updateRow(model.rowOf(id)); });

    // Щелчок по заметке в дереве фокуса НЕ переводит: человек работает с
    // деревом (Del удаляет заметку, а не буквы её заголовка — на этом
    // поймано). В текст фокус попадает щелчком по самому тексту или после
    // Ctrl+N.

    // Раскрытые ветки собираем обходом дерева: у QTreeView нет готового списка,
    // а хранить путь каждой ветки отдельно незачем — их десятки.
    std::function<void(const QModelIndex&, QStringList&)> collectExpanded =
        [&](const QModelIndex& parent, QStringList& out) {
            const int rows = model.rowCount(parent);
            for (int i = 0; i < rows; ++i) {
                const QModelIndex child = model.index(i, 0, parent);
                if (!model.isDirectory(child)) continue;
                if (tree.isExpanded(child)) out.append(model.nodePath(child));
                collectExpanded(child, out);
            }
        };
    auto expandedDirs = [&]() {
        QStringList out;
        collectExpanded(QModelIndex(), out);
        return out;
    };

    const auto shortcut = [&window](const QKeySequence& keys, auto&& slot) {
        QObject::connect(new QShortcut(keys, &window), &QShortcut::activated, &window, slot);
    };
    auto stepZoom = [&editor](qreal factor) {
        editor.applyZoom(std::clamp(editor.zoom() * factor, zametti::appearance().zoomMin,
                                    zametti::appearance().zoomMax));
    };
    // Ctrl+= рядом с Ctrl++: увеличивают одной и той же клавишей, с шифтом и без.
    shortcut(QKeySequence(QStringLiteral("Ctrl+=")),
             [&] { stepZoom(zametti::appearance().zoomStep); });
    shortcut(QKeySequence(QStringLiteral("Ctrl++")),
             [&] { stepZoom(zametti::appearance().zoomStep); });
    shortcut(QKeySequence(QStringLiteral("Ctrl+-")),
             [&] { stepZoom(1.0 / zametti::appearance().zoomStep); });
    shortcut(QKeySequence(QStringLiteral("Ctrl+0")), [&] { editor.applyZoom(1.0); });

    // Отмена и повтор живут в самом редакторе: QTextEdit объявляет их своими и
    // до ярлыка окна они не доходят.
    shortcut(QKeySequence::Save, [&] { editor.save(true); });

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
    QHash<QString, int> visitedSnapshot;
    // Какой слепок показан прямо сейчас. Нужен отдельно от editor.historyIndex()
    // ровно в один момент — при выходе из режима, когда индекс уже обнулён.
    int lastHistoryIndex = -1;

    const auto showHistoryState = [&] {
        const int at = editor.historyIndex();
        const auto& entries = editor.timeline().entries;
        if (at < 0 || at >= entries.size()) return;
        historyBanner.setSnapshot(entries[at].time, entries[at].kind);
        historyTimeline.setCurrent(at);
        window.setWindowTitle(windowTitleFor(editor.filePath()) + QStringLiteral(" — ") +
                              zametti::historyStamp(entries[at].time) +
                              QStringLiteral(" — zametti"));
    };

    QObject::connect(&editor, &zametti::NoteEditor::historyModeChanged, &window,
                     [&](bool on) {
                         historyBanner.setVisible(on);
                         historyTimeline.setVisible(on);
                         // Тонировка поля: слегка пожелтевший от времени фон,
                         // чтобы прошлое было видно ещё до чтения баннера.
                         zametti::applyPalette(editor, on);
                         // Три кнопки истории на тулбаре живут ровно столько,
                         // сколько идёт режим: вне его им не на чем работать.
                         toolbar.setHistoryMode(on);
                         if (on) {
                             // Заголовок и выделение приедут с historyIndexChanged:
                             // редактор шлёт его следом, уже показав слепок.
                             historyTimeline.setEntries(editor.timeline().entries);
                             return;
                         }
                         // Уходим — запоминаем, откуда: «назад к посещённому»
                         // вернёт сюда же. Индекс берётся ДО выхода, потому что
                         // после него historyIndex() уже -1.
                         if (lastHistoryIndex >= 0 && !editor.filePath().isEmpty())
                             visitedSnapshot.insert(editor.filePath(), lastHistoryIndex);
                         window.setWindowTitle(windowTitleFor(editor.filePath()) +
                                               QStringLiteral(" — zametti"));
                     });
    QObject::connect(&editor, &zametti::NoteEditor::historyIndexChanged, &window,
                     [&](int index) {
                         lastHistoryIndex = index;
                         showHistoryState();
                     });
    QObject::connect(&editor, &zametti::NoteEditor::historyEditRefused, &historyBanner,
                     &zametti::HistoryBanner::flashRestore);
    // Закрытие таймлайна и «К текущей версии» — одна и та же дверь наружу.
    QObject::connect(&historyBanner, &zametti::HistoryBanner::leaveRequested, &editor,
                     [&] { editor.leaveHistory(); });
    QObject::connect(&historyTimeline, &zametti::HistoryTimeline::closeRequested, &editor,
                     [&] { editor.leaveHistory(); });
    QObject::connect(&historyTimeline, &zametti::HistoryTimeline::entryChosen, &editor,
                     [&](int index) { editor.enterHistory(index); });
    // Восстановление — одно на баннер и на кнопку тулбара. Две копии этого
    // кода однажды разошлись бы в мелочи вроде текста в статусе, и человек
    // получил бы два разных ответа на один и тот же жест.
    const auto restoreFromHistory = [&] {
        bool alreadyCurrent = false;
        const qint64 source = editor.restoreShownSnapshot(&alreadyCurrent);
        if (alreadyCurrent) {
            findBar.setStatus(QStringLiteral("этот слепок и есть нынешняя версия"));
            return;
        }
        if (source == 0) return;
        findBar.setStatus(QStringLiteral("восстановлено из слепка %1")
                              .arg(zametti::historyMoment(source)));
    };
    QObject::connect(&historyBanner, &zametti::HistoryBanner::restoreRequested, &window,
                     [&] { restoreFromHistory(); });

    // Правка файла хранилища мимо редактора: только для закрытых заметок —
    // открытая правится через редактор, иначе сторож примет запись за чужую.
    //
    // ЖАЛУЕТСЯ САМА. Семь мест зовут её — переименование, «в корзину»,
    // восстановление, назначение роли папки, — и ни одно не смотрело на ответ.
    // Отказ записи означал бы, что папка на диске осталась заметкой, а
    // выброшенная заметка — невыброшенной, и оба раза молча. Ответ по-прежнему
    // возвращается: кому надо ветвиться — ветвится.
    const auto complain = [&window](const QString& file, const QString& why) {
        std::fprintf(stderr, "правка заметки не удалась: %s — %s\n",
                     file.toUtf8().constData(), why.toUtf8().constData());
        QMessageBox::warning(&window, QStringLiteral("zametti"),
                             QStringLiteral("Не удалось записать %1: %2")
                                 .arg(QFileInfo(file).fileName(), why));
    };
    const auto rewriteNote = [&](const QString& file,
                                 auto&& change) -> bool {
        std::string bytes;
        if (!readFile(file, bytes)) {
            complain(file, QStringLiteral("файл не читается"));
            return false;
        }
        zametti::Document doc = zametti::parse(bytes);
        change(doc);
        const std::string out = zametti::serialize(doc);
        std::ofstream outFile(file.toStdString(), std::ios::binary | std::ios::trunc);
        if (!outFile) {
            complain(file, QStringLiteral("файл не открывается на запись"));
            return false;
        }
        outFile.write(out.data(), std::streamsize(out.size()));
        if (outFile) return true;
        complain(file, QStringLiteral("запись оборвалась"));
        return false;
    };

    // Обновить дерево и список, не потеряв ни раскрытых веток, ни выбранной
    // папки, ни выбранной заметки. Выделение ставится с заглушенными
    // сигналами: иначе каждое обновление перезагружало бы заметку.
    const auto refreshTree = [&](const QString& keepPath) {
        const QStringList open = expandedDirs();
        const QString folderPath = model.nodePath(tree.currentIndex());
        model.refresh();
        for (const QString& dir : open) {
            const QModelIndex index = model.indexForPath(dir);
            if (index.isValid()) tree.expand(index);
        }
        const QString want = keepPath.isEmpty() ? editor.filePath() : keepPath;
        if (!model.isStore()) {
            const QModelIndex keep = model.indexForPath(want);
            if (keep.isValid()) {
                const QSignalBlocker blocked(tree.selectionModel());
                for (QModelIndex up = keep.parent(); up.isValid(); up = up.parent())
                    tree.expand(up);
                tree.setCurrentIndex(keep);
            }
            return;
        }
        QModelIndex folder = model.indexForPath(folderPath);
        if (folder.isValid()) {
            const QSignalBlocker blocked(tree.selectionModel());
            for (QModelIndex up = folder.parent(); up.isValid(); up = up.parent())
                tree.expand(up);
            tree.setCurrentIndex(folder);
        }
        list.setRows(model.notesInSubtree(folder));
        const QModelIndex keep = list.indexForPath(want);
        if (keep.isValid()) {
            const QSignalBlocker blocked(listView.selectionModel());
            listView.setCurrentIndex(keep);
            listView.scrollTo(keep);
        }
    };

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
        rewriteNote(file, [&](zametti::Document& doc) {
            for (auto& b : doc.blocks) {
                if (!b.raw && b.kind == zametti::Kind::VSpace) continue;
                if (!b.raw && b.kind == zametti::Kind::Heading) {
                    // Правка текста — это дописать байты в хвост арены и
                    // перенацелить Range: на месте арену не правят.
                    b.text = doc.append(title.toUtf8().toStdString());
                    return;
                }
                break;
            }
            zametti::Block heading =
                doc.newBlock(zametti::Kind::Heading, title.toUtf8().toStdString());
            heading.headingLevel = 1;
            zametti::Block gap;
            gap.kind = zametti::Kind::VSpace;
            doc.blocks.insert(doc.blocks.begin(), gap);
            doc.blocks.insert(doc.blocks.begin(), heading);
        });
        if (file == editor.filePath()) editor.openFile(file);
        refreshTree(file);
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
            rewriteNote(file, [&](zametti::Document& doc) {
                doc.meta.present = true;
                if (parentId.isEmpty()) doc.meta.unset("parent");
                else doc.meta.set("parent", parentId.toStdString());
            });
        }
        refreshTree(keepPath);
    };
    QObject::connect(&model, &zametti::NoteTreeModel::moveRequested, &window,
                     [&](const QString& noteId, const QString& parentId) {
                         moveNote(noteId, parentId,
                                  model.nodePath(QModelIndex()) + QLatin1Char('/') +
                                      noteId + QStringLiteral(".md"));
                     });

    // Del: заметка едет в корзину; в корзине — насовсем, с подтверждением.
    // Корзина — заметка с role: trash, заводится при первом удалении.
    // Del / «В корзину». Пустая заметка и пустая папка удаляются сразу — в
    // ФАЙЛОВУЮ корзину ОС (при тестировании их плодится много, гонять их
    // через заметочную корзину — трата времени). Непустая заметка едет в
    // заметочную корзину (role: trash; заводится при первом удалении); из
    // корзины — насовсем, с подтверждением, но тоже через корзину ОС.
    // Выделение после удаления уходит к соседу: раскрывать корзину и
    // «показывать» удалённое не надо.
    const auto deleteNote = [&](const QString& noteId) {
        if (!model.isStore() || noteId.isEmpty() || !model.hasNote(noteId)) return;
        if (noteId == model.trashId()) return;
        const QString file = model.pathOfId(noteId);
        const bool wasOpen = file == editor.filePath();

        // Сосед по папке — будущий выделенный.
        const QString fallback = model.pathOfId(model.neighbourOf(noteId));

        const auto settleAfter = [&] {
            refreshTree(fallback.isEmpty() ? QString() : fallback);
            if (!wasOpen) return;
            QString open = fallback;
            if (open.isEmpty()) open = model.pathOfId(model.firstNoteId());
            if (!open.isEmpty()) editor.openFile(open);
        };

        // Пустое — в корзину ОС без разговоров. Пустая папка — без детей;
        // пустая заметка — без содержательного текста (открытая меряется по
        // документу: набранное могло ещё не сохраниться).
        bool empty = false;
        if (model.isFolderId(noteId)) {
            empty = model.childCountOf(noteId) == 0;
        } else if (wasOpen) {
            empty = editor.toPlainText().trimmed().isEmpty();
        } else {
            std::string bytes;
            if (readFile(file, bytes)) {
                const zametti::Document doc = zametti::parse(bytes);
                empty = true;
                for (const auto& b : doc.blocks)
                    if (b.raw || b.kind != zametti::Kind::VSpace) {
                        empty = false;
                        break;
                    }
            }
        }
        if (empty || model.inTrashId(noteId)) {
            if (!empty) {
                const auto answer = QMessageBox::question(
                    &window, QStringLiteral("zametti"),
                    QStringLiteral("Удалить насовсем «%1»?")
                        .arg(model.titleOfId(noteId)));
                if (answer != QMessageBox::Yes) return;
            }
            // Если заметка открыта, сначала сохраняем: иначе последним слепком
            // в истории осталось бы состояние до последних правок, а человек
            // удаляет то, что видит.
            if (wasOpen) editor.save(false);
            // Само удаление — в хранилище: там же живёт правило «сначала
            // надгробие, потом файл» и обещание никогда не удалять журнал.
            QString deleteError;
            if (!zametti::store::deleteNoteFile(model.nodePath(QModelIndex()), noteId,
                                                &deleteError)) {
                QMessageBox::warning(&window, QStringLiteral("zametti"), deleteError);
                return;
            }
            if (!deleteError.isEmpty())
                std::fprintf(stderr, "%s\n", deleteError.toUtf8().constData());
            settleAfter();
            return;
        }

        QString trash = model.trashId();
        if (trash.isEmpty()) {
            QString newError;
            const QString made = zametti::store::newNote(
                model.nodePath(QModelIndex()), QString(), &newError);
            if (made.isEmpty()) {
                QMessageBox::warning(&window, QStringLiteral("zametti"), newError);
                return;
            }
            rewriteNote(made, [](zametti::Document& doc) {
                doc.meta.set("role", "trash");
                zametti::Block heading = doc.newBlock(zametti::Kind::Heading, "Корзина");
                heading.headingLevel = 1;
                doc.blocks.push_back(heading);
            });
            model.refresh();
            trash = model.trashId();
        }
        if (trash.isEmpty()) return;

        // Происхождение — в мету: прежний родитель по id и путь из имён папок
        // от корня. По ним «Восстановить» вернёт заметку на место, а если
        // папки уже нет — пересоздаст цепочку по именам. Неизвестные ключи
        // переживают круг записи, редактору они не видны.
        const QString originParent = model.parentIdOf(noteId);
        QString titlePath = model.ancestorTitles(noteId).join(QLatin1Char('/'));
        titlePath.replace(QStringLiteral("--"), QStringLiteral("-"));   // мета не терпит "--"

        const auto stampTrash = [&](zametti::NoteMeta& meta) {
            meta.set("parent", trash.toStdString());
            if (originParent.isEmpty()) meta.unset("trash-parent");
            else meta.set("trash-parent", originParent.toStdString());
            if (titlePath.isEmpty()) meta.unset("trash-path");
            else meta.set("trash-path", titlePath.toUtf8().toStdString());
        };
        if (wasOpen) {
            editor.editMeta(stampTrash);
        } else {
            rewriteNote(file, [&](zametti::Document& doc) {
                doc.meta.present = true;
                stampTrash(doc.meta);
            });
        }
        settleAfter();
    };

    // «Восстановить» из корзины: прежний родитель жив — туда; нет — цепочка
    // папок пересоздаётся по именам с корня; совсем ничего — в корень.
    const auto restoreNote = [&](const QString& noteId) {
        if (!model.isStore() || !model.inTrashId(noteId)) return;
        if (noteId.isEmpty() || noteId == model.trashId()) return;
        const QString root = model.nodePath(QModelIndex());
        const QString file = root + QLatin1Char('/') + noteId + QStringLiteral(".md");

        std::string bytes;
        if (!readFile(file, bytes)) {
            // Человек нажал «восстановить», и ничего не произошло бы вовсе.
            complain(file, QStringLiteral("файл не читается"));
            return;
        }
        zametti::Document doc = zametti::parse(bytes);
        const QString savedParent =
            QString::fromStdString(doc.meta.get("trash-parent"));
        const QString savedPath =
            QString::fromUtf8(doc.meta.get("trash-path").c_str());

        QString target;   // id папки назначения; пусто — корень
        if (!savedParent.isEmpty() && model.isFolderId(savedParent) &&
            !model.inTrashId(savedParent))
            target = savedParent;
        if (target.isEmpty() && !savedPath.isEmpty()) {
            QString parentId;   // идём по цепочке имён, пересоздавая недостающее
            for (const QString& name :
                 savedPath.split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
                QString foundId = model.childFolderByTitle(parentId, name);
                if (foundId.isEmpty()) {
                    QString newError;
                    const QString made =
                        zametti::store::newNote(root, parentId, &newError);
                    if (made.isEmpty()) {
                        QMessageBox::warning(&window, QStringLiteral("zametti"),
                                             newError);
                        return;
                    }
                    rewriteNote(made, [&](zametti::Document& folderDoc) {
                        folderDoc.meta.set("role", "folder");
                        zametti::Block heading = folderDoc.newBlock(
                            zametti::Kind::Heading, name.toUtf8().toStdString());
                        heading.headingLevel = 1;
                        folderDoc.blocks.push_back(heading);
                    });
                    model.refresh();
                    foundId = QFileInfo(made).completeBaseName();
                }
                parentId = foundId;
            }
            target = parentId;
        }

        const auto restoreMeta = [&](zametti::NoteMeta& meta) {
            if (target.isEmpty()) meta.unset("parent");
            else meta.set("parent", target.toStdString());
            meta.unset("trash-parent");
            meta.unset("trash-path");
        };
        if (file == editor.filePath()) {
            editor.editMeta(restoreMeta);
        } else {
            rewriteNote(file, [&](zametti::Document& noteDoc) {
                noteDoc.meta.present = true;
                restoreMeta(noteDoc.meta);
            });
        }
        refreshTree(file);   // показать, куда вернулась
    };
    // Del в дереве бьёт по папке, Del в списке — по заметке. Каждый ярлык
    // висит на своём виджете: до этого Del из дерева удалял буквы заголовка в
    // редакторе, и лечится это только привязкой к виджету.
    {
        auto* del = new QShortcut(QKeySequence::Delete, &tree);
        del->setContext(Qt::WidgetWithChildrenShortcut);
        QObject::connect(del, &QShortcut::activated, &window,
                         [&] { deleteNote(model.idOf(tree.currentIndex())); });
    }
    {
        auto* del = new QShortcut(QKeySequence::Delete, &listView);
        del->setContext(Qt::WidgetWithChildrenShortcut);
        QObject::connect(del, &QShortcut::activated, &window,
                         [&] { deleteNote(list.idAt(listView.currentIndex())); });
    }

    // Новая заметка или папка. Папка — та же заметка, но с role: folder в
    // мете: опустевшая папка не превращается обратно в заметку.
    const auto createNote = [&](const QString& requestedParent, bool folder) {
        if (!model.isStore()) return;
        editor.save(false);
        // В корзине ничего не создаётся: Ctrl+N из корзины — на глобальный
        // уровень (правило владельца).
        QString parentId = requestedParent;
        if (!parentId.isEmpty() &&
            (!model.hasNote(parentId) || model.inTrashId(parentId)))
            parentId.clear();
        QString newError;
        const QString made = zametti::store::newNote(
            model.nodePath(QModelIndex()), parentId, &newError);
        if (made.isEmpty()) {
            QMessageBox::warning(&window, QStringLiteral("zametti"), newError);
            return;
        }
        if (folder) {
            rewriteNote(made, [](zametti::Document& doc) {
                doc.meta.set("role", "folder");
                zametti::Block heading = doc.newBlock(zametti::Kind::Heading, "Новая папка");
                heading.headingLevel = 1;
                doc.blocks.push_back(heading);
            });
        }
        refreshTree(made);
        const QModelIndex fresh = model.indexForPath(made);
        if (folder) {
            if (fresh.isValid()) tree.edit(fresh);   // сразу дать имя
        } else {
            // Открыть явно: refreshTree выделяет с заглушенными сигналами
            // (иначе каждое обновление перезагружало бы заметку), так что
            // на открытие через выделение полагаться нельзя.
            editor.openFile(made);
            editor.setFocus();
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
            const QString made = zametti::store::importNote(
                model.nodePath(QModelIndex()), parentId, file, &error);
            if (made.isEmpty()) {
                failed.append(QFileInfo(file).fileName() + QStringLiteral(": ") + error);
                continue;
            }
            if (first.isEmpty()) first = made;
        }

        if (!first.isEmpty()) {
            refreshTree(first);
            // Открыть явно: refreshTree выделяет с заглушенными сигналами.
            editor.openFile(first);
            editor.setFocus();
        }
        if (!failed.isEmpty()) {
            QMessageBox::warning(
                &window, QStringLiteral("zametti"),
                QStringLiteral("Не импортировано файлов: %1\n\n%2")
                    .arg(failed.size())
                    .arg(failed.join(QLatin1Char('\n'))));
        }
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
    grab->onNew = [&] { createNote(model.folderIdFor(tree.currentIndex()), false); };
    editor.installEventFilter(grab);
    shortcut(QKeySequence::New,
             [&] { createNote(model.folderIdFor(tree.currentIndex()), false); });

    // Сортировка одна на обе панели: и папки слева, и заметки в середине
    // упорядочены одинаково. Переключатель — над средней колонкой.
    const auto applySort = [&](zametti::NoteTreeModel::SortMode mode) {
        if (mode == model.sortMode()) return;
        const QStringList open = expandedDirs();
        const QString folderPath = model.nodePath(tree.currentIndex());
        const QString keep = editor.filePath();
        model.setSortMode(mode);
        list.setSortMode(mode);
        for (const QString& dir : open) {
            const QModelIndex index = model.indexForPath(dir);
            if (index.isValid()) tree.expand(index);
        }
        const QModelIndex folder = model.indexForPath(folderPath);
        if (folder.isValid()) {
            const QSignalBlocker blocked(tree.selectionModel());
            tree.setCurrentIndex(folder);
        }
        const QModelIndex back = list.indexForPath(keep);
        if (back.isValid()) {
            const QSignalBlocker blocked(listView.selectionModel());
            listView.setCurrentIndex(back);
            listView.scrollTo(back);
        }
    };
    // Две кнопки вместо комбобокса — группа с единственным нажатым: нажать
    // «по имени» значит отжать «по дате». Отжать обе нельзя, поэтому повторное
    // нажатие уже нажатой возвращает её назад, а не оставляет обе пустыми:
    // сортировки «никакой» не бывает.
    const auto showSortMode = [&toolbar](zametti::NoteTreeModel::SortMode mode) {
        const bool byName = mode == zametti::NoteTreeModel::SortMode::ByName;
        toolbar.setChecked(zametti::Toolbar::Button::SortByName, byName);
        toolbar.setChecked(zametti::Toolbar::Button::SortByDate, !byName);
    };
    showSortMode(model.sortMode());

    // --- внешний редактор ----------------------------------------------------
    //
    // Открывается настоящий файл хранилища целиком, вместе с шапкой метаданных.
    // Никаких временных копий и «очищенных» выгрузок: инвариант «файл правится
    // чем угодно» — основа формата, а мета-комментарий выбран именно за то, что
    // переживает чужие редакторы.
    const auto openExternally = [&](const QString& file) {
        if (file.isEmpty()) return;
        editor.save(false);   // сначала на диск, иначе снаружи откроется старое
        const QString command = zametti::appearance().externalEditor;
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
            refreshTree(editor.filePath());
        });
        ask->open();
    });

    QObject::connect(&editor, &zametti::NoteEditor::externalEditorRequested, &window,
                     [&](const QString& file) { openExternally(file); });

    // Внешняя правка могла сменить parent — это законный перенос — или тронуть
    // заголовок. Дерево и список догоняют файл. Пересканируем всё хранилище, а
    // не одну строку: перенос меняет структуру, а внешние правки редки (7 мс
    // по замеру этапа 4 — цена, которую не жалко).
    QObject::connect(&editor, &zametti::NoteEditor::externalAdopted, &window,
                     [&](const QString& file) {
                         if (model.isStore()) refreshTree(file);
                     });

    // Контекстное меню левой панели — про папки: создание, переименование,
    // корзина. Тело самой папки открывается отдельным пунктом: в средней
    // колонке папок нет, и иначе до её текста было бы не добраться.
    tree.setContextMenuPolicy(Qt::CustomContextMenu);
    QObject::connect(&tree, &QWidget::customContextMenuRequested, &window,
                     [&](const QPoint& pos) {
        if (!model.isStore()) return;
        // Клик мимо строк — меню действует на выделенную папку: пункт
        // «В корзину» не должен пропадать из-за промаха мышью.
        QModelIndex at = tree.indexAt(pos);
        if (!at.isValid()) at = tree.currentIndex();
        const QString id = model.idOf(at);
        QMenu menu(&tree);
        menu.addAction(QStringLiteral("Новая заметка"),
                       [&] { createNote(model.folderIdFor(at), false); });
        menu.addAction(QStringLiteral("Новая папка"),
                       [&] { createNote(model.folderIdFor(at), true); });
        menu.addAction(QStringLiteral("Импортировать…"),
                       [&] { importNotes(model.folderIdFor(at)); });
        if (!id.isEmpty()) {
            menu.addSeparator();
            // «Открыть как заметку» здесь больше нет. Папка — структура, а не
            // заметка; то, что она лежит в хранилище файлом .md, — особенность
            // хранения, и наружу её выпускать незачем. В теле такого файла
            // положено быть только шапке и заголовку, а редактор рано или
            // поздно завёл бы там текст, который никто уже не увидит.
            menu.addAction(QStringLiteral("Переименовать"), [&] { tree.edit(at); });
            if (model.inTrashId(id) && id != model.trashId())
                menu.addAction(QStringLiteral("Восстановить"), [&] { restoreNote(id); });
            menu.addAction(model.inTrashId(id) ? QStringLiteral("Удалить насовсем")
                                               : QStringLiteral("В корзину"),
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
                       [&] { importNotes(model.folderIdFor(tree.currentIndex())); });
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
                    if (model.inTrashId(folderId)) continue;
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
        menu.addAction(QStringLiteral("Открыть во внешнем редакторе"),
                       [&] { openExternally(model.pathOfId(id)); });
        menu.addSeparator();
        if (model.inTrashId(id))
            menu.addAction(QStringLiteral("Восстановить"), [&] { restoreNote(id); });
        menu.addAction(model.inTrashId(id) ? QStringLiteral("Удалить насовсем")
                                           : QStringLiteral("В корзину"),
                       [&] { deleteNote(id); });
        menu.exec(listView.viewport()->mapToGlobal(pos));
    });

    // --- поиск --------------------------------------------------------------
    //
    // Панель одна на три команды; какая из них действует, решает её вид. F3
    // отдаётся той панели, что открыта, — открытие одной закрывает другую по
    // построению: панель-то одна.
    const auto searchRoot = [&] { return model.nodePath(QModelIndex()); };

    const auto updateInNoteSearch = [&](const QString& text) {
        const zametti::Query query = zametti::makeQuery(text);
        if (query.isEmpty()) {
            editor.clearMatches();
            findBar.setStatus(QString());
            return;
        }
        const int count = editor.findMatches(query.needle, query.caseSensitive);
        findBar.setStatus(count == 0
                              ? QStringLiteral("нет совпадений")
                              : QStringLiteral("%1/%2")
                                    .arg(editor.currentMatch() + 1)
                                    .arg(count));
    };

    const auto showCounter = [&] {
        if (editor.matchCount() == 0) {
            findBar.setStatus(QStringLiteral("нет совпадений"));
            return;
        }
        findBar.setStatus(QStringLiteral("%1/%2")
                              .arg(editor.currentMatch() + 1)
                              .arg(editor.matchCount()));
    };

    QObject::connect(&findBar, &zametti::FindBar::queryChanged, &window,
                     [&](const QString& text) {
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
        if (file.isEmpty()) return;
        // Показать заметку в боковых колонках — общий путь через fileChanged,
        // отдельного кода здесь больше не нужно.
        if (file != editor.filePath()) editor.openFile(file);
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
        editor.stepMatch(direction);
        showCounter();
    };
    QObject::connect(&findBar, &zametti::FindBar::findNext, &window, [&] { stepSearch(1); });
    QObject::connect(&findBar, &zametti::FindBar::findPrevious, &window,
                     [&] { stepSearch(-1); });

    QObject::connect(&findBar, &zametti::FindBar::replaceOne, &window, [&] {
        if (editor.currentMatch() < 0) editor.stepMatch(1);
        editor.replaceCurrentMatch(findBar.replacement());
        showCounter();
    });
    QObject::connect(&findBar, &zametti::FindBar::replaceAll, &window, [&] {
        const zametti::Query query = zametti::makeQuery(findBar.query());
        if (query.isEmpty()) return;
        const int replaced =
            editor.replaceAllMatches(query.needle, query.caseSensitive, findBar.replacement());
        findBar.setStatus(QStringLiteral("заменено: %1").arg(replaced));
    });

    QObject::connect(&findBar, &zametti::FindBar::closed, &window, [&] {
        editor.clearMatches();
        storeSearch.cancel();
        searchDebounce.stop();
        resultsView.hide();
        results.clear();
        editor.setFocus();
    });

    const auto openFind = [&](zametti::FindBar::Mode mode) {
        const bool global = mode == zametti::FindBar::Mode::Global;
        if (!global) {
            resultsView.hide();
            results.clear();
            storeSearch.cancel();
        } else {
            editor.clearMatches();
        }
        // Выделенное в редакторе — готовый запрос: чаще всего ищут именно то,
        // на что смотрят.
        QString preset = editor.textCursor().selectedText();
        if (preset.contains(QChar::ParagraphSeparator)) preset.clear();
        findBar.open(mode, preset);
    };
    shortcut(QKeySequence::Find, [&] { openFind(zametti::FindBar::Mode::InNote); });
    shortcut(QKeySequence::Replace, [&] { openFind(zametti::FindBar::Mode::Replace); });
    shortcut(QKeySequence(QStringLiteral("Ctrl+Shift+F")), [&] {
        if (!model.isStore()) return;
        openFind(zametti::FindBar::Mode::Global);
    });
    shortcut(QKeySequence(Qt::Key_F3), [&] { stepSearch(1); });
    shortcut(QKeySequence(Qt::SHIFT | Qt::Key_F3), [&] { stepSearch(-1); });

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
        tree.setVisible(visible);
        if (model.isStore()) middle.setVisible(visible);
        if (visible && keptSizes.size() == splitter.count()) splitter.setSizes(keptSizes);
        toolbar.setChecked(Button::Panels, !visible);
        toolbar.buttonFor(Button::Panels)
            ->setToolTip(visible ? QStringLiteral("Скрыть боковые панели")
                                 : QStringLiteral("Показать боковые панели"));
    };
    {
        if (session.panelsHidden) showPanels(false);

        // Обещания. Погашенная кнопка без объяснения читается как поломка, а
        // не как «будет позже», поэтому у каждой — своя причина словами.
        toolbar.setPromise(Button::Cloud,
                           QStringLiteral("появится вместе с синхронизацией"));
        toolbar.setPromise(Button::Export, QStringLiteral("появится в этом этапе"));

        toolbar.setPromise(Button::SearchInHistory,
                           QStringLiteral("появится вместе с единым поиском"));
        if (!model.isStore()) {
            // Открыт одиночный файл, а не хранилище: создавать и сортировать
            // нечего и негде. Это не «пока не сделано», а другое состояние мира.
            const QString single = QStringLiteral("открыт один файл, а не хранилище");
            for (Button id : {Button::NewNote, Button::NewFolder, Button::ImportNotes,
                              Button::SortByName, Button::SortByDate})
                toolbar.setPromise(id, single);
        }
        toolbar.setHistoryMode(editor.inHistory());

        QObject::connect(&toolbar, &zametti::Toolbar::pressed, &window, [&](Button id) {
            switch (id) {
            case Button::NewNote:
                createNote(model.folderIdFor(tree.currentIndex()), false);
                break;
            case Button::NewFolder:
                createNote(model.folderIdFor(tree.currentIndex()), true);
                break;
            case Button::ImportNotes:
                importNotes(model.folderIdFor(tree.currentIndex()));
                break;
            case Button::InsertImages:
                editor.chooseAndInsertImages();
                break;
            case Button::Panels:
                showPanels(!toolbar.isChecked(Button::Panels));
                break;
            case Button::SortByName:
                applySort(zametti::NoteTreeModel::SortMode::ByName);
                showSortMode(model.sortMode());
                break;
            case Button::SortByDate:
                applySort(zametti::NoteTreeModel::SortMode::ByModified);
                showSortMode(model.sortMode());
                break;
            case Button::HistoryRestore:
                restoreFromHistory();
                break;
            case Button::HistoryForward:
                editor.leaveHistory();
                break;
            case Button::HistoryRewind: {
                // Закладка ставится при уходе из режима, а нажимают кнопку внутри
                // него: значит она ведёт к слепку прошлого захода, а не к тому,
                // на котором стоим. Закладки нет — молчим, а не прыгаем наугад.
                const int at = visitedSnapshot.value(editor.filePath(), -1);
                if (at >= 0) editor.enterHistory(at);
                break;
            }
            case Button::Search:
                openFind(zametti::FindBar::Mode::InNote);
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
            case Button::Cloud:
            case Button::SearchInHistory:
                break;   // обещания: кнопки погашены, сюда не доходит
            }
        });
    }
    {
        // Esc закрывает панель, откуда бы ни нажали: в самой панели его ловит
        // её keyPressEvent, а из редактора — этот ярлык.
        auto* escape = new QShortcut(QKeySequence(Qt::Key_Escape), &window);
        QObject::connect(escape, &QShortcut::activated, &window, [&] {
            if (findBar.isHidden()) return;
            findBar.hide();
            emit findBar.closed();
        });
    }

    // Фокус ушёл из приложения — момент, когда человек переключился на что-то
    // другое и меньше всего ждёт потери правок.
    QObject::connect(&app, &QGuiApplication::focusWindowChanged, &window,
                     [&](QWindow* focused) {
                         if (focused == nullptr) editor.save(false);
                     });

    if (!session.windowGeometry.isEmpty()) window.restoreGeometry(session.windowGeometry);
    else window.resize(1150, 780);
    if (!session.splitterState.isEmpty()) splitter.restoreState(session.splitterState);
    else if (model.isStore())
        splitter.setSizes({zametti::appearance().sidebarWidth,
                           zametti::appearance().noteListWidth, 700});
    else
        splitter.setSizes({zametti::appearance().sidebarWidth, 800});
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
    // прочих. Сигнал глушим — иначе выделение немедленно вызвало бы повторную
    // загрузку того же файла.
    for (const QString& dir : session.expandedDirs) {
        const QModelIndex index = model.indexForPath(dir);
        if (index.isValid() && model.isDirectory(index)) tree.expand(index);
    }

    if (model.isStore()) {
        // Слева выделяется папка открытой заметки, в середине — сама заметка.
        // Список наполняется от папки: показать заметку в контексте её соседей
        // важнее, чем сразу вывалить всё хранилище.
        const QString noteId = QFileInfo(current).completeBaseName();
        QModelIndex folder = model.folderIndexForNote(noteId);
        if (!folder.isValid()) folder = model.indexForPath(model.nodePath(QModelIndex()));
        if (folder.isValid()) {
            const QSignalBlocker blocked(tree.selectionModel());
            for (QModelIndex up = folder.parent(); up.isValid(); up = up.parent())
                tree.expand(up);
            tree.setCurrentIndex(folder);
            tree.scrollTo(folder);
        }
        list.setRows(model.notesInSubtree(folder));
        const QModelIndex row = list.indexForPath(current);
        if (row.isValid()) {
            const QSignalBlocker blocked(listView.selectionModel());
            listView.setCurrentIndex(row);
            listView.scrollTo(row, QAbstractItemView::PositionAtCenter);
        }
    } else {
        const QModelIndex currentIndex = model.indexForPath(current);
        if (currentIndex.isValid()) {
            const QSignalBlocker blocked(tree.selectionModel());
            for (QModelIndex up = currentIndex.parent(); up.isValid(); up = up.parent())
                tree.expand(up);
            tree.setCurrentIndex(currentIndex);
            tree.scrollTo(currentIndex, QAbstractItemView::PositionAtCenter);
        }
    }

    // Фокус — после show() и после того, как дерево показало текущую заметку: до
    // show() окно ещё не решило, кому его отдать, и наш выбор затёрся бы первым
    // же виджетом в разделителе. Каретка при запуске должна быть сразу в тексте:
    // дерево нужно, чтобы выбрать заметку, а не чтобы в нём находиться.
    editor.setFocus();

    // Прокрутку можно ставить только когда документ уже разложен по размерам
    // окна, а это происходит после show(), в следующем проходе цикла событий.
    const double startRatio = session.lastFile == current ? session.scrollRatio : 0.0;
    if (startRatio > 0.0)
        QTimer::singleShot(0, &editor, [&editor, startRatio] { editor.setScrollRatio(startRatio); });

    QObject::connect(&app, &QCoreApplication::aboutToQuit, &window, [&] {
        // На выходе окно с ошибкой показывать поздно: жалуемся в stderr.
        editor.save(false);

        zametti::Session out;
        out.lastFile = editor.filePath();
        out.scrollRatio = editor.scrollRatio();
        out.zoom = editor.zoom();
        out.windowGeometry = window.saveGeometry();
        out.splitterState = splitter.saveState();
        out.panelsHidden = toolbar.isChecked(zametti::Toolbar::Button::Panels);
        out.expandedDirs = expandedDirs();
        out.searchHistory = findBar.history();
        out.storeRoot = model.isStore() ? model.nodePath(QModelIndex()) : QString();
        out.treeSort = model.sortMode() == zametti::NoteTreeModel::SortMode::ByName
                           ? QStringLiteral("name")
                           : QStringLiteral("modified");
        zametti::saveSession(out);
    });

    return app.exec();
}
