// Просмотрщик: дерево заметок слева, отрендеренный документ справа.
//
// Это не самоцель, а первый стенд для проверки ядра. Отсюда же работает режим
// --check: прогнать parse → serialize и показать расхождение с оригиналом.

#include "doc_model.h"
#include "editor_widget.h"
#include "note_tree.h"
#include "store.h"
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
#include <QMenu>
#include <QKeyEvent>
#include <QMessageBox>
#include <QPushButton>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTextBlock>
#include <QTextCursor>
#include <QFile>
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
    "               zametti --root каталог-хранилища\n"
    "               zametti --check файл.md\n"
    "               zametti --dump-config\n";

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
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            printHelp();
            return 0;
        }
        if (arg == "--check") check = true;
        else if (arg == "--dump-config") dumpConfig = true;
        else if (arg == "--noconfig") noConfig = true;
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

    QSplitter window(Qt::Horizontal);
    zametti::NoteTreeView tree;
    zametti::NoteEditor editor;

    zametti::NoteTreeModel model(
        storeRoot.isEmpty()
            ? zametti::NoteTreeModel::rootFor(current, zametti::appearance().notesRoot)
            : QFileInfo(storeRoot).absoluteFilePath());

    // Свежая заметка хранилища — первая ОТКРЫВАЕМАЯ (директории не в счёт),
    // поиском в глубину; пустое хранилище получает первую заметку тут же.
    std::function<QModelIndex(const QModelIndex&)> firstNote =
        [&](const QModelIndex& parent) -> QModelIndex {
        for (int i = 0; i < model.rowCount(parent); ++i) {
            const QModelIndex child = model.index(i, 0, parent);
            if (!model.filePath(child).isEmpty()) return child;
            const QModelIndex inside = firstNote(child);
            if (inside.isValid()) return inside;
        }
        return {};
    };
    if (current.isEmpty()) {
        QModelIndex first = firstNote(QModelIndex());
        if (!first.isValid()) {
            QString newError;
            const QString made = zametti::store::newNote(
                QFileInfo(storeRoot).absoluteFilePath(), QString(), &newError);
            if (made.isEmpty()) {
                std::fprintf(stderr, "%s\n", newError.toUtf8().constData());
                return 2;
            }
            model.refresh();
            first = firstNote(QModelIndex());
        }
        current = model.filePath(first);
        if (current.isEmpty()) {
            std::fprintf(stderr, "в хранилище нет ни одной открываемой заметки\n");
            return 2;
        }
    }
    if (session.treeSort == QStringLiteral("name"))
        model.setSortMode(zametti::NoteTreeModel::SortMode::ByName);
    tree.setModel(&model);
    tree.setHeaderHidden(true);
    tree.setEditTriggers(model.isStore() ? QAbstractItemView::EditKeyPressed
                                         : QAbstractItemView::NoEditTriggers);
    if (model.isStore()) {
        tree.setDragDropMode(QAbstractItemView::InternalMove);
        tree.setDefaultDropAction(Qt::MoveAction);
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

    zametti::applyPalette(editor);
    zametti::applyPalette(tree);

    window.addWidget(&tree);
    window.addWidget(&editor);
    window.setStretchFactor(1, 1);   // при растяжении окна растёт текст, а не панель
    window.setChildrenCollapsible(false);

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
    const auto windowTitleFor = [&](const QString& file) {
        if (model.isStore()) {
            const QModelIndex index = model.indexForPath(file);
            const QString title =
                index.isValid() ? model.data(index, Qt::DisplayRole).toString()
                                : QString();
            if (!title.isEmpty()) return title;
        }
        return QFileInfo(file).completeBaseName();
    };
    QObject::connect(&editor, &zametti::NoteEditor::fileChanged, &window,
                     [&](const QString& file) {
                         window.setWindowTitle(windowTitleFor(file) +
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

    // Ткнули в заметку мышью — работать дальше человек будет в тексте, значит и
    // каретка должна быть там. Стрелками по дереву при этом ходить можно
    // по-прежнему: фокус переносит только щелчок, а не всякая смена выбора.
    //
    // Без этого в пустой заметке было и вовсе не за что зацепиться: текста нет,
    // каретки нет, и непонятно, куда набирать.
    QObject::connect(&tree, &QTreeView::clicked, &editor, [&](const QModelIndex& index) {
        if (!model.filePath(index).isEmpty()) editor.setFocus();
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

    // Правка файла хранилища мимо редактора: только для закрытых заметок —
    // открытая правится через редактор, иначе сторож примет запись за чужую.
    const auto rewriteNote = [&](const QString& file,
                                 auto&& change) -> bool {
        std::string bytes;
        if (!readFile(file, bytes)) return false;
        zametti::Document doc = zametti::parse(bytes);
        change(doc);
        const std::string out = zametti::serialize(doc);
        std::ofstream outFile(file.toStdString(), std::ios::binary | std::ios::trunc);
        if (!outFile) return false;
        outFile.write(out.data(), std::streamsize(out.size()));
        return bool(outFile);
    };

    // Обновить дерево, не потеряв ни раскрытых веток, ни выбранной заметки.
    const auto refreshTree = [&](const QString& keepPath) {
        const QStringList open = expandedDirs();
        model.refresh();
        for (const QString& dir : open) {
            const QModelIndex index = model.indexForPath(dir);
            if (index.isValid()) tree.expand(index);
        }
        const QModelIndex keep = model.indexForPath(
            keepPath.isEmpty() ? editor.filePath() : keepPath);
        if (keep.isValid()) {
            const QSignalBlocker blocked(tree.selectionModel());
            for (QModelIndex up = keep.parent(); up.isValid(); up = up.parent())
                tree.expand(up);
            tree.setCurrentIndex(keep);
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
                if (b.rawSource.empty() && b.kind == zametti::Kind::VSpace) continue;
                if (b.rawSource.empty() && b.kind == zametti::Kind::Heading) {
                    b.text = title.toUtf8().toStdString();
                    return;
                }
                break;
            }
            zametti::Block heading;
            heading.kind = zametti::Kind::Heading;
            heading.headingLevel = 1;
            heading.text = title.toUtf8().toStdString();
            zametti::Block gap;
            gap.kind = zametti::Kind::VSpace;
            doc.blocks.insert(doc.blocks.begin(), std::move(gap));
            doc.blocks.insert(doc.blocks.begin(), std::move(heading));
        });
        if (file == editor.filePath()) editor.openFile(file);
        refreshTree(file);
    });

    // Перенос: правка parent. Открытая — через редактор, закрытая — по файлу.
    const auto moveNote = [&](const QString& noteId, const QString& parentId) {
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
        refreshTree(file);
    };
    QObject::connect(&model, &zametti::NoteTreeModel::moveRequested, &window,
                     [&](const QString& noteId, const QString& parentId) {
                         moveNote(noteId, parentId);
                     });

    // Del: заметка едет в корзину; в корзине — насовсем, с подтверждением.
    // Корзина — заметка с role: trash, заводится при первом удалении.
    const auto deleteIndex = [&](const QModelIndex& index) {
        if (!model.isStore() || !index.isValid()) return;
        const QString noteId = model.idOf(index);
        if (noteId.isEmpty() || noteId == model.trashId()) return;
        const QString file = model.nodePath(index);

        if (model.inTrash(index)) {
            const auto answer = QMessageBox::question(
                &window, QStringLiteral("zametti"),
                QStringLiteral("Удалить насовсем «%1»?")
                    .arg(model.data(index, Qt::DisplayRole).toString()));
            if (answer != QMessageBox::Yes) return;
            const bool wasOpen = file == editor.filePath();
            QFile::remove(file);
            refreshTree(wasOpen ? QString() : editor.filePath());
            if (wasOpen) {
                const QModelIndex first = model.index(0, 0, QModelIndex());
                if (first.isValid() && !model.filePath(first).isEmpty())
                    editor.openFile(model.filePath(first));
            }
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
                zametti::Block heading;
                heading.kind = zametti::Kind::Heading;
                heading.headingLevel = 1;
                heading.text = "Корзина";
                doc.blocks.push_back(std::move(heading));
            });
            model.refresh();
            trash = model.trashId();
        }
        if (!trash.isEmpty()) moveNote(noteId, trash);
    };
    {
        auto* del = new QShortcut(QKeySequence::Delete, &tree);
        del->setContext(Qt::WidgetWithChildrenShortcut);
        QObject::connect(del, &QShortcut::activated, &window,
                         [&] { deleteIndex(tree.currentIndex()); });
    }

    // Новая заметка или папка. Папка — та же заметка, но с role: folder в
    // мете: опустевшая папка не превращается обратно в заметку.
    const auto createNote = [&](const QString& parentId, bool folder) {
        if (!model.isStore()) return;
        editor.save(false);
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
                zametti::Block heading;
                heading.kind = zametti::Kind::Heading;
                heading.headingLevel = 1;
                heading.text = "Новая папка";
                doc.blocks.push_back(std::move(heading));
            });
        }
        refreshTree(made);
        const QModelIndex fresh = model.indexForPath(made);
        if (folder) {
            if (fresh.isValid()) tree.edit(fresh);   // сразу дать имя
        } else {
            editor.setFocus();
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

    // Контекстное меню дерева: создание, переименование, корзина, сортировка.
    tree.setContextMenuPolicy(Qt::CustomContextMenu);
    QObject::connect(&tree, &QWidget::customContextMenuRequested, &window,
                     [&](const QPoint& pos) {
        if (!model.isStore()) return;
        // Клик мимо строк — меню действует на выделенную заметку: пункт
        // «В корзину» не должен пропадать из-за промаха мышью.
        QModelIndex at = tree.indexAt(pos);
        if (!at.isValid()) at = tree.currentIndex();
        QMenu menu(&tree);
        menu.addAction(QStringLiteral("Новая заметка"),
                       [&] { createNote(model.folderIdFor(at), false); });
        menu.addAction(QStringLiteral("Новая папка"),
                       [&] { createNote(model.folderIdFor(at), true); });
        if (at.isValid()) {
            menu.addAction(QStringLiteral("Переименовать"), [&] { tree.edit(at); });
            menu.addAction(model.inTrash(at) ? QStringLiteral("Удалить насовсем")
                                             : QStringLiteral("В корзину"),
                           [&] { deleteIndex(at); });
        }
        menu.addSeparator();
        QMenu* sorting = menu.addMenu(QStringLiteral("Сортировка"));
        const auto applySort = [&](zametti::NoteTreeModel::SortMode mode) {
            const QStringList open = expandedDirs();
            const QString keep = editor.filePath();
            model.setSortMode(mode);
            for (const QString& dir : open) {
                const QModelIndex index = model.indexForPath(dir);
                if (index.isValid()) tree.expand(index);
            }
            const QModelIndex back = model.indexForPath(keep);
            if (back.isValid()) {
                const QSignalBlocker blocked(tree.selectionModel());
                tree.setCurrentIndex(back);
            }
        };
        QAction* byDate = sorting->addAction(QStringLiteral("По дате правки"));
        byDate->setCheckable(true);
        byDate->setChecked(model.sortMode() ==
                           zametti::NoteTreeModel::SortMode::ByModified);
        QObject::connect(byDate, &QAction::triggered, &window, [&] {
            applySort(zametti::NoteTreeModel::SortMode::ByModified);
        });
        QAction* byName = sorting->addAction(QStringLiteral("По имени"));
        byName->setCheckable(true);
        byName->setChecked(model.sortMode() ==
                           zametti::NoteTreeModel::SortMode::ByName);
        QObject::connect(byName, &QAction::triggered, &window, [&] {
            applySort(zametti::NoteTreeModel::SortMode::ByName);
        });
        menu.exec(tree.viewport()->mapToGlobal(pos));
    });

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
        out.splitterState = window.saveState();
        out.expandedDirs = expandedDirs();
        out.storeRoot = model.isStore() ? model.nodePath(QModelIndex()) : QString();
        out.treeSort = model.sortMode() == zametti::NoteTreeModel::SortMode::ByName
                           ? QStringLiteral("name")
                           : QStringLiteral("modified");
        zametti::saveSession(out);
    });

    return app.exec();
}
