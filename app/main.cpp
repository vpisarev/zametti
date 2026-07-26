// Просмотрщик: дерево заметок слева, отрендеренный документ справа.
//
// Это не самоцель, а первый стенд для проверки ядра. Отсюда же работает режим
// --check: прогнать parse → serialize и показать расхождение с оригиналом.

#include "document_builder.h"
#include "note_tree.h"
#include "parser.h"
#include "serializer.h"
#include "settings.h"

#include <QApplication>
#include <QFileInfo>
#include <QFont>
#include <QIcon>
#include <QItemSelectionModel>
#include <QKeySequence>
#include <QScrollBar>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTextBrowser>
#include <QTextCursor>
#include <QTextDocument>
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

void printUsage() {
    std::fprintf(stderr,
                 "использование: zametti [--noconfig] [файл.md]\n"
                 "               zametti --check файл.md\n"
                 "               zametti --dump-config\n");
}

}  // namespace

int main(int argc, char** argv) {
    QString path;
    bool check = false;
    bool dumpConfig = false;
    bool noConfig = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
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
    QCoreApplication::setApplicationName(QStringLiteral("zametti"));
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

    QString current = QFileInfo(path).absoluteFilePath();
    std::string src;
    if (!readFile(current, src)) {
        std::fprintf(stderr, "не читается: %s\n", current.toUtf8().constData());
        return 2;
    }

    QSplitter window(Qt::Horizontal);
    zametti::NoteTreeView tree;
    QTextBrowser view;

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

    view.setOpenExternalLinks(true);
    zametti::applyPalette(view);
    zametti::applyPalette(tree);

    window.addWidget(&tree);
    window.addWidget(&view);
    window.setStretchFactor(1, 1);   // при растяжении окна растёт текст, а не панель
    window.setChildrenCollapsible(false);

    // Кегль задан явно в каждом формате, поэтому штатный зум QTextEdit до него
    // не дотягивается: при смене масштаба документ собирается заново. Место в
    // тексте держим по доле прокрутки — в пикселях оно после пересборки другое.
    qreal zoom = std::clamp(session.zoom, zametti::appearance().zoomMin,
                            zametti::appearance().zoomMax);
    zametti::Document doc = zametti::parse(src);

    auto scrollRatio = [&view]() -> double {
        const QScrollBar* bar = view.verticalScrollBar();
        return bar->maximum() > 0 ? double(bar->value()) / bar->maximum() : 0.0;
    };
    auto rebuild = [&view, &doc, &zoom](double ratio) {
        zametti::buildDocument(doc, *view.document(), zoom);
        view.moveCursor(QTextCursor::Start);
        QScrollBar* bar = view.verticalScrollBar();
        bar->setValue(int(ratio * bar->maximum()));
    };
    auto setTitle = [&window](const QString& file) {
        window.setWindowTitle(QFileInfo(file).completeBaseName() + QStringLiteral(" — zametti"));
    };

    auto openNote = [&](const QString& file) {
        std::string text;
        if (!readFile(file, text)) {
            std::fprintf(stderr, "не читается: %s\n", file.toUtf8().constData());
            return;
        }
        current = file;
        doc = zametti::parse(text);
        setTitle(file);
        rebuild(0.0);
    };

    QObject::connect(tree.selectionModel(), &QItemSelectionModel::currentChanged, &tree,
                     [&](const QModelIndex& index, const QModelIndex&) {
                         // На каталоге ничего не открываем: он только раскрывается.
                         const QString file = model.filePath(index);
                         if (!file.isEmpty() && file != current) openNote(file);
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

    auto applyZoom = [&](qreal factor) {
        const qreal next = std::clamp(zoom * factor, zametti::appearance().zoomMin,
                                      zametti::appearance().zoomMax);
        if (next == zoom) return;
        const double ratio = scrollRatio();
        zoom = next;
        rebuild(ratio);
    };

    const auto shortcut = [&window](const QKeySequence& keys, auto&& slot) {
        QObject::connect(new QShortcut(keys, &window), &QShortcut::activated, &window, slot);
    };
    // Ctrl+= рядом с Ctrl++: увеличивают одной и той же клавишей, с шифтом и без.
    shortcut(QKeySequence(QStringLiteral("Ctrl+=")),
             [&] { applyZoom(zametti::appearance().zoomStep); });
    shortcut(QKeySequence(QStringLiteral("Ctrl++")),
             [&] { applyZoom(zametti::appearance().zoomStep); });
    shortcut(QKeySequence(QStringLiteral("Ctrl+-")),
             [&] { applyZoom(1.0 / zametti::appearance().zoomStep); });
    shortcut(QKeySequence(QStringLiteral("Ctrl+0")), [&] {
        if (zoom == qreal(1.0)) return;
        const double ratio = scrollRatio();
        zoom = 1.0;
        rebuild(ratio);
    });

    setTitle(current);
    if (!session.windowGeometry.isEmpty()) window.restoreGeometry(session.windowGeometry);
    else window.resize(1150, 780);
    if (!session.splitterState.isEmpty()) window.restoreState(session.splitterState);
    else window.setSizes({zametti::appearance().sidebarWidth, 800});
    window.show();

    rebuild(0.0);

    // Показать текущую заметку в дереве надо после show(): раскрытие веток
    // требует уже созданных представлений. Сигнал глушим, иначе выделение
    // немедленно вызвало бы повторную загрузку того же файла.
    // Раскрытые ветки восстанавливаем до того, как показать текущую заметку:
    // иначе её раскрытие затерялось бы среди прочих.
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
    if (startRatio > 0.0) {
        QTimer::singleShot(0, &view, [&view, startRatio] {
            QScrollBar* bar = view.verticalScrollBar();
            bar->setValue(int(startRatio * bar->maximum()));
        });
    }

    QObject::connect(&app, &QCoreApplication::aboutToQuit, &window, [&] {
        zametti::Session out;
        out.lastFile = current;
        out.scrollRatio = scrollRatio();
        out.zoom = zoom;
        out.windowGeometry = window.saveGeometry();
        out.splitterState = window.saveState();
        out.expandedDirs = expandedDirs();
        zametti::saveSession(out);
    });

    return app.exec();
}
