// Просмотрщик: дерево заметок слева, отрендеренный документ справа.
//
// Это не самоцель, а первый стенд для проверки ядра. Отсюда же работает режим
// --check: прогнать parse → serialize и показать расхождение с оригиналом.

#include "editor_widget.h"
#include "note_tree.h"
#include "note_view.h"
#include "parser.h"
#include "serializer.h"
#include "settings.h"

#include <QApplication>
#include <QFileInfo>
#include <QFont>
#include <QIcon>
#include <QItemSelectionModel>
#include <QKeySequence>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTimer>
#include <QTreeView>

#include <algorithm>
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

const char* kUsage =
    "использование: zametti [--noconfig] [файл.md]\n"
    "               zametti --check файл.md\n"
    "               zametti --dump-config\n";

void printUsage() { std::fputs(kUsage, stderr); }

// Справка идёт в stdout и с нулевым кодом: её просят намеренно, это не ошибка.
void printHelp() {
    std::fputs(kUsage, stdout);
    std::printf(
        "\n"
        "Просмотрщик заметок в markdown. Слева дерево заметок, справа документ.\n"
        "Без имени файла открывается тот, что читали в прошлый раз.\n"
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
        "\n"
        "Файлы:\n"
        "  %s\n"
        "      оформление; приложение его только читает, править вручную.\n"
        "      Полный список параметров — по ключу --dump-config\n"
        "  %s\n"
        "      последняя заметка, прокрутка, зум, геометрия окна, раскрытые ветки;\n"
        "      переписывается при выходе\n",
        zametti::configPath().toUtf8().constData(),
        zametti::statePath().toUtf8().constData());
}

}  // namespace

int main(int argc, char** argv) {
    // Имя приложения задаём до разбора ключей: от него зависят пути к конфигу и
    // состоянию, а их печатает --help, не создавая ни окна, ни QApplication.
    QCoreApplication::setApplicationName(QStringLiteral("zametti"));

    QString path;
    bool check = false;
    bool dumpConfig = false;
    bool noConfig = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            printHelp();
            return 0;
        }
        if (arg == "--check") check = true;
        else if (arg == "--dump-config") dumpConfig = true;
        else if (arg == "--noconfig") noConfig = true;
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

    // Без аргумента открываем то, что читали в прошлый раз.
    if (path.isEmpty()) path = session.lastFile;
    if (path.isEmpty()) {
        printUsage();
        return 2;
    }

    const QString current = QFileInfo(path).absoluteFilePath();

    QSplitter window(Qt::Horizontal);
    zametti::NoteTreeView tree;
    zametti::NoteEditor editor;

    zametti::NoteTreeModel model(
        zametti::NoteTreeModel::rootFor(current, zametti::appearance().notesRoot));
    tree.setModel(&model);
    tree.setHeaderHidden(true);
    tree.setEditTriggers(QAbstractItemView::NoEditTriggers);
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

    zametti::applyPalette(editor);
    zametti::applyPalette(tree);

    window.addWidget(&tree);
    window.addWidget(&editor);
    window.setStretchFactor(1, 1);   // при растяжении окна растёт текст, а не панель
    window.setChildrenCollapsible(false);

    QObject::connect(&editor, &zametti::NoteEditor::fileChanged, &window,
                     [&window](const QString& file) {
                         window.setWindowTitle(QFileInfo(file).completeBaseName() +
                                               QStringLiteral(" — zametti"));
                     });

    // Кегль задан явно в каждом формате, поэтому штатный зум QTextEdit до него
    // не дотягивается: при смене масштаба документ собирается заново из того же
    // содержимого. В историю правок это не попадает — облик не содержимое.
    editor.setZoom(std::clamp(session.zoom, zametti::appearance().zoomMin,
                              zametti::appearance().zoomMax));
    if (!editor.openFile(current)) return 2;

    QObject::connect(tree.selectionModel(), &QItemSelectionModel::currentChanged, &tree,
                     [&](const QModelIndex& index, const QModelIndex&) {
                         // На каталоге ничего не открываем: он только раскрывается.
                         const QString file = model.filePath(index);
                         if (!file.isEmpty() && file != editor.filePath()) editor.openFile(file);
                     });

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

    shortcut(QKeySequence::Save, [&] { editor.save(true); });
    shortcut(QKeySequence::Undo, [&] { editor.undo(); });
    shortcut(QKeySequence::Redo, [&] { editor.redo(); });

    // Фокус ушёл из приложения — момент, когда человек переключился на что-то
    // другое и меньше всего ждёт потери правок.
    QObject::connect(&app, &QGuiApplication::focusWindowChanged, &window,
                     [&](QWindow* focused) {
                         if (focused == nullptr) editor.save(false);
                     });

    if (!session.windowGeometry.isEmpty()) window.restoreGeometry(session.windowGeometry);
    else window.resize(1150, 780);
    if (!session.splitterState.isEmpty()) window.restoreState(session.splitterState);
    else window.setSizes({zametti::appearance().sidebarWidth, 800});
    window.show();

    // Показать текущую заметку в дереве надо после show(): раскрытие веток
    // требует уже созданных представлений. Раскрытые ветки восстанавливаем до
    // того, как показать текущую заметку, иначе её раскрытие затеряется среди
    // прочих. Сигнал глушим — иначе выделение немедленно вызвало бы повторную
    // загрузку того же файла.
    for (const QString& dir : session.expandedDirs) {
        const QModelIndex index = model.indexForPath(dir);
        if (index.isValid() && model.isDirectory(index)) tree.expand(index);
    }

    const QModelIndex currentIndex = model.indexForPath(current);
    if (currentIndex.isValid()) {
        const QSignalBlocker blocked(tree.selectionModel());
        for (QModelIndex up = currentIndex.parent(); up.isValid(); up = up.parent())
            tree.expand(up);
        tree.setCurrentIndex(currentIndex);
        tree.scrollTo(currentIndex, QAbstractItemView::PositionAtCenter);
    }

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
        out.splitterState = window.saveState();
        out.expandedDirs = expandedDirs();
        zametti::saveSession(out);
    });

    return app.exec();
}
