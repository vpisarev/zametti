// Просмотрщик: дерево заметок слева, отрендеренный документ справа.
//
// Это не самоцель, а первый стенд для проверки ядра. Отсюда же работает режим
// --check: прогнать parse → serialize и показать расхождение с оригиналом.

#include "doc_model.h"
#include "editor_widget.h"
#include "key_binding.h"
#include "formula.h"
#include "heif_handler.h"
#include "find_bar.h"
#include "history_controller.h"
#include "markdown_controller.h"
#include "settings_controller.h"
#include "history_panel.h"
#include "reader_view.h"
#include "toolbar_controller.h"
#include "zoom_target.h"
#include "history_view.h"
#include "image_viewer.h"
#include "markdown_edit_view.h"
#include "note_list.h"
#include "note_panels.h"
#include "note_tree.h"
#include "search.h"
#include "search_results.h"
#include "store_search.h"
#include "journal.h"
#include "note_view.h"
#include "resources.h"
#include "document.h"
#include "znote.h"
#include "zstorage.h"
#include "zsystem.h"
#include "serializer.h"
#include "settings.h"
#include "sort_order.h"
#include "about_window.h"
#include "export_note.h"
#include "export_pdf.h"
#include "status_bar.h"
#include "store_cli.h"
#include "keyring_secrets.h"
#include "pending_deletes_dialog.h"
#include "secret_store.h"
#include "store_manager_dialog.h"
#include "sync_controller.h"
#include "toolbar.h"
#include "zapp.h"

#include <QApplication>
#include <QGuiApplication>
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
#include <QCollator>
#include <QMessageBox>
#include <QDesktopServices>
#include <QDateTime>
#include <QSysInfo>
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
#include <QDir>
#include <QFileSystemWatcher>
#include <QVBoxLayout>
#include <QWidget>

// После Qt: windows.h тащит за собой пол-мира имён, и Qt лучше разобрать
// раньше. NOMINMAX задан toolchain'ом — иначе макросы min/max сломали бы
// std::min здесь же, ниже.
#ifdef Q_OS_WIN
#include <windows.h>
#endif

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace {

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
    // ЧИТАЕТ ХРАНИЛИЩЕ, а не std::ifstream. Раньше стояло
    // `std::ifstream in(path.toStdString())`, и под Windows это молча ломалось
    // на любом пути с кириллицей: узкий поток берёт имя в кодировке ANSI
    // текущей системы, а toStdString() отдаёт UTF-8. Ловится сразу — режим
    // --check стоит у набора canon-without-display, и файл там называется
    // «канон-без-дисплея.md».
    if (!zametti::ZStorage::readFileBytes(path, src)) {
        std::fprintf(stderr, "cannot read: %s\n", path.toUtf8().constData());
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


// ОДНОЙ ЗАМЕТКОЙ ПРОГРАММА НЕ ОТКРЫВАЕТСЯ (решение владельца 26.08.2026).
// Раньше можно было позвать `zametti файл.md`, и корень хранилища выводился
// подъёмом по каталогам от этого файла. Путь убран целиком: он добавлял
// сложность (выведение корня, его ловушки с регистром и с хранилищем вне
// домашнего каталога) и не добавлял ценности — одну заметку правят любым
// редактором. Программа работает С ХРАНИЛИЩЕМ: его задаёт --root, а дальше он
// помнится в state.json.
//
// Позиционный файл остался ровно у --check: это проверка канона, а не
// открытие заметки.
const char* kUsage =
    "usage: zametti [--noconfig] [--config-dir dir]\n"
    "       zametti --root store-directory\n"
    "                 (without it: the store from the last session, or an empty\n"
    "                  window with the storage button lit)\n"
    "       zametti --check file.md\n"
    "       zametti --dump-config\n"
    "       zametti --root store-directory --unlock\n"
    "       zametti store <command> ...   (see: zametti store --help)\n";

void printUsage() { std::fputs(kUsage, stderr); }

// Справка идёт в stdout и с нулевым кодом: её просят намеренно, это не ошибка.
// Сочетаний на команду может быть несколько; в конфиге они через точку с
// запятой, а человеку читать удобнее через запятую.
QByteArray keysFor(const QString& keys) {
    if (keys.trimmed().isEmpty()) return QByteArrayLiteral("(none)");   // сочетание убрано
    return QString(keys).replace(QStringLiteral("; "), QStringLiteral(", ")).toUtf8();
}

QByteArray padFor(const QString& keys) {
    return QByteArray(qMax(0, 16 - int(keysFor(keys).size())), ' ');
}

void printHelp() {
    std::fputs(kUsage, stdout);
    std::printf(
        "\n"
        "Markdown note viewer. Note tree on the left, document on the right.\n"
        "Without a file name, opens the one read last time.\n"
        "\n"
        "Store (--root):\n"
        "  the database button (leftmost) opens another store or creates one in an\n"
        "  empty directory; with no store open it is the only button that works\n"
        "  Ctrl+N            new note (child of the one selected in the tree)\n"
        "  F2                rename note (edits its first heading)\n"
        "  Del in tree       to archive; in archive — permanently, with confirmation\n"
        "  drag and drop     move a note; a folder is a note with children\n"
        "\n"
        "Options:\n"
        "  --check file.md   run parse and write-back, show the difference\n"
        "                    from the original; non-zero exit code on mismatch.\n"
        "                    No display needed\n"
        "  --dump-config     print every appearance parameter with its default\n"
        "                    value, in the same form the config expects\n"
        "  --noconfig        skip the config, use defaults\n"
        "  --config-dir dir  keep config.json and state.json there, not in the\n"
        "                    usual place; the directory is created if missing\n"
        "  --help, -h        this help\n"
        "\n"
        "Store from the command line:\n"
        "  zametti store <command> ...\n"
        "                    verify, thin, import, recompress, sync and the rest;\n"
        "                    the full list is in: zametti store --help\n"
        "\n"
        "Keys:\n"
        "  Ctrl+=, Ctrl+-    zoom in, zoom out\n"
        "  Ctrl+0            reset zoom\n"
        "  Ctrl+S            save now\n"
        "  Ctrl+Z, Ctrl+Y    undo, redo\n"
        "  Enter             in text — line break, twice in a row — new paragraph;\n"
        "                    in a list — new item, on an empty item leaves the list\n"
        "  Shift+Enter       the reverse: new paragraph in text, line break in an item\n"
        "  Backspace         at the start of an item — turn it into a paragraph\n"
        "  Tab, Shift+Tab    move an item across nesting levels\n"
        "  %s%s  toggle task: done or not\n"
        "  %s%s  move item up\n"
        "  %s%s  move item down\n"
        "  %s%s  make a bulleted list\n"
        "  %s%s  make a numbered list\n"
        "  %s%s  make a task list\n"
        "  %s%s  make plain text\n"
        "  Ctrl+B, Ctrl+I    bold, italic\n"
        "  Ctrl+/            strikethrough\n"
        "  Ctrl+E            inline code; also steps out of the backticks while typing\n"
        "  Ctrl+Shift+E      selection to a code block and back to text\n"
        "  %s%s  edit markdown source (and back)\n"
        "\n"
        "Files:\n"
        "  %s\n"
        "      appearance; edit it in the app (the gear button) or by hand.\n"
        "      Full parameter list — see --dump-config\n"
        "  %s\n"
        "      last note, scroll position, zoom, window geometry, expanded branches;\n"
        "      rewritten on exit\n",
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

#ifdef Q_OS_WIN
// ОКНО БЕЗ ЧЁРНОГО ОКНА, НО С ВЫВОДОМ В КОНСОЛЬ (решение владельца 26.08.2026).
//
// zametti.exe собран подсистемой GUI: запуск из проводника не должен открывать
// рядом консоль. Плата у такой программы одна — своей консоли у неё нет, и
// весь stdout/stderr уходит в никуда, включая --help, --check и жалобы на
// незнакомый ключ. Для заметочника с CLI-режимом это молчаливая пропажа
// ровно того класса, которого мы не терпим.
//
// Лечится тем, чем лечат все GUI-программы с командным режимом: если нас
// позвали ИЗ консоли, подцепляемся к консоли родителя и переоткрываем на неё
// потоки. Позвали из проводника — AttachConsole честно откажет, и мы молча
// остаёмся немыми, как и положено оконной программе.
void attachParentConsole() {
    if (AttachConsole(ATTACH_PARENT_PROCESS) == 0) return;
    // freopen на CONOUT$/CONIN$, а не на "CON": первое работает и когда часть
    // потоков перенаправлена в файл, второе — нет.
    FILE* unused = nullptr;
    freopen_s(&unused, "CONOUT$", "w", stdout);
    freopen_s(&unused, "CONOUT$", "w", stderr);
    freopen_s(&unused, "CONIN$", "r", stdin);
}
#endif

// Аргументы командной строки — строками Qt, ДО создания QApplication.
//
// Под Windows argv приходит в кодировке ANSI текущей системы, и путь с
// кириллицей в имени превращается в вопросительные знаки ещё до того, как мы
// на него посмотрим. Настоящая строка лежит в GetCommandLineW; берём её. Это
// та же беда, что у узкого open() в writeNewFile, и лечится так же — широким
// вариантом системного вызова.
QStringList commandLineArgs(int argc, char** argv) {
    QStringList out;
#ifdef Q_OS_WIN
    (void)argc;
    (void)argv;
    int count = 0;
    LPWSTR* wide = CommandLineToArgvW(GetCommandLineW(), &count);
    if (wide != nullptr) {
        out.reserve(count);
        for (int i = 0; i < count; ++i)
            out.append(QString::fromWCharArray(wide[i]));
        LocalFree(wide);
        return out;
    }
    // Не вышло — берём узкие: хуже, чем широкие, но лучше, чем ничего.
#endif
    out.reserve(argc);
    for (int i = 0; i < argc; ++i) out.append(QString::fromLocal8Bit(argv[i]));
    return out;
}

}  // namespace

int main(int argc, char** argv) {
#ifdef Q_OS_WIN
    attachParentConsole();
#endif
    // Имя приложения задаём до разбора ключей: от него зависят пути к конфигу и
    // состоянию, а их печатает --help, не создавая ни окна, ни QApplication.
    QCoreApplication::setApplicationName(QStringLiteral("zametti"));

    QStringList args = commandLineArgs(argc, argv);

    // --config-dir <path> — ГДЕ ЛЕЖИТ ХОЗЯЙСТВО ПРОГРАММЫ: config.json,
    // state.json, бухгалтерия синка, err.log и sync.log.
    //
    // Разбирается ПЕРВЫМ, до подкоманды `store`, до --help и до создания
    // QApplication. Иначе поздно: место конфига спрашивают и справка (она
    // печатает пути), и утилита хранилища, и первое же чтение настроек.
    //
    // Кладётся в ту самую переменную среды, которой это место уводили и раньше
    // (ZAMETTI_CONFIG_DIR): дверь остаётся одна, а ключ — просто удобный способ
    // её открыть, когда переменную выставлять неоткуда. Ключ СИЛЬНЕЕ
    // переменной: названное руками важнее унаследованного окружением.
    //
    // Каталог заводится сразу: без этого программа узнала бы о его
    // недоступности только в миг записи state.json, то есть при выходе.
    for (qsizetype i = 1; i < args.size(); ++i) {
        const QString& arg = args.at(i);
        QString dir;
        qsizetype eat = 0;
        if (arg == QLatin1String("--config-dir")) {
            // Ключ узнаём ДАЖЕ без значения: иначе он проваливается в общий
            // цикл и человек читает «unknown option: --config-dir» — то есть
            // «такого ключа нет» вместо «ключу нужен каталог».
            if (i + 1 >= args.size()) {
                std::fprintf(stderr, "--config-dir needs a directory\n");
                return 2;
            }
            dir = args.at(i + 1);
            eat = 2;
        } else if (arg.startsWith(QLatin1String("--config-dir="))) {
            dir = arg.mid(int(sizeof("--config-dir=")) - 1);
            eat = 1;
        } else {
            continue;
        }
        if (dir.isEmpty()) {
            std::fprintf(stderr, "--config-dir needs a directory\n");
            return 2;
        }
        // Абсолютный путь — от рабочего каталога процесса, и это здесь
        // законно: человек называет СВОЙ каталог оттуда, где стоит.
        dir = QDir(dir).absolutePath();
        if (!QDir().mkpath(dir)) {
            std::fprintf(stderr, "cannot create the config directory: %s\n",
                         dir.toLocal8Bit().constData());
            return 2;
        }
        qputenv(zametti::kConfigDirVar, dir.toLocal8Bit());
        args.remove(i, eat);
        --i;
    }

    // ПОДКОМАНДА `store` — командный вид хранилища (бывшая программа
    // zametti-store, слита сюда 26.08.2026 решением владельца).
    //
    // Разбирается ДО цикла ключей: у утилиты свои ключи (--from, --id,
    // --apple-manifest), и в общем цикле они уткнулись бы в «unknown option».
    //
    // Дисплея ей не нужно, а QtGui нужен: ядро строит живой QTextDocument, и
    // QGuiApplication без платформенного плагина не стартует вовсе. Заданную
    // снаружи платформу не перебиваем — тот же приём, что у --check ниже.
    // Виджеты здесь не создаются: QGuiApplication, а не QApplication.
    if (args.size() > 1 && args.at(1) == QLatin1String("store")) {
        if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
            qputenv("QT_QPA_PLATFORM", "offscreen");
        QGuiApplication storeApp(argc, argv);
        // Декодеры картинок, которых libheif не знает по рождению (AV1 через
        // libgav1). ДО первого потока: реестр плагинов libheif — обычный
        // std::set без замка, а recompress и import читают картинки.
        zametti::HeifHandler::registerCodecs();
        QStringList storeArgs = args;
        storeArgs.removeAt(1);
        return zametti::StoreCli(storeArgs).run();
    }

    QString checkFile;   // позиционный аргумент; смысл имеет только с --check
    QString storeRoot;
    // Корень НАЗВАЛ человек ключом — или он взялся из state.json/конфига. От
    // этого зависит, что делать, если он не хранилище: названный вслух путь
    // мимо цели это ошибка человека, и съедать её нельзя.
    bool fromCommandLine = false;
    bool check = false;
    bool dumpConfig = false;
    bool noConfig = false;
    bool unlock = false;
    for (qsizetype i = 1; i < args.size(); ++i) {
        const QString& arg = args.at(i);
        if (arg == QLatin1String("--help") || arg == QLatin1String("-h")) {
            printHelp();
            return 0;
        }
        if (arg == QLatin1String("--check")) check = true;
        else if (arg == QLatin1String("--dump-config")) dumpConfig = true;
        else if (arg == QLatin1String("--noconfig")) noConfig = true;
        else if (arg == QLatin1String("--unlock")) unlock = true;
        else if (arg == QLatin1String("--root") && i + 1 < args.size()) {
            storeRoot = args.at(++i);
            fromCommandLine = true;
        }
        else if (arg.startsWith(QLatin1String("--"))) {
            std::fprintf(stderr, "unknown option: %s\n", arg.toLocal8Bit().constData());
            return 2;
        } else {
            checkFile = arg;
        }
    }

    // Позиционный файл без --check — это бывший режим «открой одну заметку».
    // Отвечаем внятно, а не молча его игнорируем: человек, набравший старую
    // команду, должен узнать, чем она заменена, а не смотреть на пустое окно.
    if (!checkFile.isEmpty() && !check) {
        std::fprintf(stderr,
                     "zametti works with a store, not with a single file.\n"
                     "  open the store:  zametti --root <store-directory>\n"
                     "  check one file:  zametti --check %s\n",
                     checkFile.toUtf8().constData());
        return 2;
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
        if (checkFile.isEmpty()) {
            printUsage();
            return 2;
        }
        // Шрифты — до сборки: сборщик берёт метрики у семейств из настроек, и
        // без влинкованных гарнитур Qt молча подставит свои.
        zametti::loadEmbeddedFonts();
    // Декодеры картинок, которых libheif не знает по рождению (AV1 через
    // libgav1). ДО первого потока: реестр плагинов libheif — обычный std::set
    // без замка, а картинки читает и фоновый поток ввоза, и сама libheif в
    // несколько потоков на плиточной сетке.
    zametti::HeifHandler::registerCodecs();

        return runCheck(checkFile);
    }
    // Декодеры картинок, которых libheif не знает по рождению (AV1 через
    // libgav1). ДО первого потока: реестр плагинов libheif — обычный std::set
    // без замка, а картинки читает и фоновый поток ввоза, и сама libheif в
    // несколько потоков на плиточной сетке.
    zametti::HeifHandler::registerCodecs();

    // Оболочки рабочего стола (в том числе док GNOME) берут иконку не у окна, а
    // из .desktop-файла с этим именем — см. packaging/linux/zametti.desktop.
    QGuiApplication::setDesktopFileName(QStringLiteral("zametti"));
    QGuiApplication::setWindowIcon(QIcon(QStringLiteral(":/zametti.png")));

    // Шрифты — до чтения конфига и до первого виджета: настройки называют
    // семейства по имени, и если имя некому отдать, Qt молча подставит своё.
    // Жалуемся, но работаем: без шрифта программа некрасива, а не мертва.
    for (const QString& face : zametti::loadEmbeddedFonts())
        std::fprintf(stderr, "embedded font rejected by Qt: %s\n",
                     face.toUtf8().constData());

    // Движок формул — здесь же: подъём стоит 22 мс, и они не должны достаться
    // первой формуле человека (прогревочный рендер внутри init). Не поднялся —
    // жалуемся и живём дальше: формулы покажутся рамкой ошибки, но заметки
    // читаются и правятся.
    {
        QString formulaError;
        if (!zametti::Formulas::init(&formulaError))
            std::fprintf(stderr, "formula engine failed to start: %s\n",
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
        std::fprintf(stderr, "config not parsed, defaults in use:\n  %s\n",
                     configError.toUtf8().constData());
    }

    const zametti::ZAppState& session = zapp.state();

    // ЧТО ОТКРЫВАЕМ. Хранилище задаёт --root; без ключа берём то, в котором
    // работали в прошлый раз, — оно помнится в state.json, и ключ каждый раз не
    // нужен. Заметку внутри выбираем прошлую (session.lastFile), а если её нет
    // или она из другого хранилища — свежую, уже после построения дерева.
    //
    // Хранилища не нашлось вовсе — говорим, чем его задать. Раньше на этом
    // месте можно было открыть одну заметку и вывести корень от неё; этот путь
    // убран (см. kUsage).
    QString path = session.lastFile();
    if (storeRoot.isEmpty() && !session.storeRoot().isEmpty() &&
        zametti::NoteTreeModel::isStoreRoot(session.storeRoot()))
        storeRoot = session.storeRoot();
    // Хранилище из конфига (store.root) — путь ОТНОСИТЕЛЬНО домашнего каталога.
    // Третьим номером: явный ключ важнее, прошлый сеанс важнее записанного раз
    // и навсегда. Раньше эту ветку держал NoteTreeModel::rootFor, ушедший
    // вместе с режимом одной заметки, — а сам ключ конфига остался нужным:
    // им хранилище задаётся насовсем, без --root при каждом запуске.
    if (storeRoot.isEmpty()) {
        const QString configured = zametti::settings().store().notesRoot();
        if (!configured.isEmpty()) {
            const QString fromHome = QDir::home().filePath(configured);
            if (QFileInfo(fromHome).isDir()) storeRoot = fromHome;
        }
    }
    // ХРАНИЛИЩА МОЖЕТ И НЕ БЫТЬ — окно поднимется пустым, и в нём горит одна
    // кнопка: «открыть или завести хранилище». Так выглядит первый запуск, и
    // это внятнее, чем подсказка в терминале, которого человек не видел.
    //
    // Но ЯВНО НАЗВАННЫЙ каталог — другое дело: раз человек его назвал, значит
    // ошибся в пути, и молча открыть вместо него пустое окно значило бы съесть
    // его ошибку (решение владельца).
    QString absRoot = storeRoot.isEmpty() ? QString()
                                          : QFileInfo(storeRoot).absoluteFilePath();
    if (!absRoot.isEmpty() && !zametti::NoteTreeModel::isStoreRoot(absRoot)) {
        if (!fromCommandLine) {
            // Хранилище прошлого сеанса или из конфига исчезло — не беда и не
            // повод отказываться работать: открываем пустое окно.
            std::fprintf(stderr, "the store is gone, opening empty: %s\n",
                         absRoot.toUtf8().constData());
            absRoot.clear();
        } else {
            std::fprintf(stderr, "does not look like a store (no .zametti): %s\n",
                         absRoot.toUtf8().constData());
            return 2;
        }
    }
    if (path.isEmpty() || absRoot.isEmpty() ||
        !QFileInfo(path).absoluteFilePath().startsWith(absRoot))
        path = QString();   // выберем свежую после построения дерева

    const QString wanted = path.isEmpty() ? QString() : QFileInfo(path).absoluteFilePath();

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
    // АРХИВНАЯ ЗАМЕТКА ПОКАЗЫВАЕТСЯ ВИДОМ, а не редактором: править её нельзя,
    // пока не вернут из архива, и редактор обещал бы правку, которой не будет.
    // Каретки в виде нет — этого человеку и достаточно.
    zametti::ReaderView archiveView(nullptr, zametti::ReaderView::Tint::Aged);
    // ДОКУМЕНТАЦИЯ — ТА ЖЕ ЧИТАЛКА, ДРУГИМ ТОНОМ. Заметки папки Info вшиты в
    // бинарник и правке не подлежат, но отложенным добром они не являются:
    // фон у них обычный (решение владельца), а всё остальное — как у архивной.
    zametti::ReaderView docView(nullptr, zametti::ReaderView::Tint::Plain);

    zametti::NoteEditor editor;

    // Хранилище открывает объект приложения; левая и средняя колонки — его
    // проекция (NotePanels: дерево папок, список заметок, показ открытой).
    // Корень известен всегда: без него мы сюда не доходим. Прежде здесь стоял
    // NoteTreeModel::rootFor(), выводивший корень подъёмом от открытой заметки, —
    // вместе с режимом «открой одну заметку» он и ушёл.
    zametti::NotePanels panels(zapp.openStorage(absRoot));
    zametti::NoteTreeModel& model = panels.model();
    // Синхронизация: контроллер живёт при окне, движок бегает в рабочем
    // потоке; каталог заметок обновляют сторожа хранилища и external-путь
    // редактора, поэтому окну от синка ничего не нужно, кроме статуса.
    // Люк обвязки, тот же, что у CLI: ключ в среде (ZAMETTI_SYNC_KEY) —
    // секреты из среды, keyring не трогается и диалог разблокировки не
    // выскакивает. Для человека — системный keyring, как и задумано.
    // Экземпляр ЯВНЫЙ и один: им же пользуется диалог хранилищ — добытые
    // секреты кладутся туда, откуда контроллер их потом достанет.
    std::shared_ptr<zametti::SecretStore> syncSecrets;
    if (!qEnvironmentVariable("ZAMETTI_SYNC_KEY").isEmpty())
        syncSecrets = std::make_shared<zametti::EnvSecrets>();
    else
        syncSecrets = std::make_shared<zametti::KeyringSecrets>();
    zametti::SyncController cloudSync(zapp.storage(), syncSecrets, &zapp.logs());
    // Той же связкой пользуется список хранилищ: факты строк («ключ есть?»)
    // спрашиваются атрибутами, без чтения секретов.
    zapp.setStoreSecrets(syncSecrets);
    zametti::NoteTreeView& tree = panels.tree();
    zametti::NoteListModel& list = panels.list();
    QListView& listView = panels.listView();


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
    // Правка настроек — четвёртая страница стека, на месте редактора.
    zametti::JsonEditView settingsView;
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
        textStack.addWidget(&archiveView);
        textStack.addWidget(&docView);
        textStack.addWidget(&historyView);
        textStack.addWidget(&markdownView);
        textStack.addWidget(&settingsView);
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
    findBar.setRegexOn(session.searchRegex());
    zametti::HistoryController history(editor, historyView);
    zametti::MarkdownController markdown(editor, markdownView);
    zametti::SettingsController settingsMode(editor, settingsView,
                                             std::make_shared<zametti::ZConfigFile>());

    // Облик применяется ОДНИМ местом — и на старте, и когда конфиг поправили
    // снаружи. Два места разошлись бы: половина настроек подхватывалась бы на
    // лету, половина только после перезапуска, и понять, какая именно, было бы
    // нельзя.
    const auto applyAppearance = [&] {
        // Цвета выделения — всей программе (контекстные меню и прочие виджеты
        // Qt рисуют подсветку палитрой приложения). Первым делом: виды ниже
        // ставят поверх свои палитры, и они должны лечь на уже верный фон.
        zametti::applySelectionPaletteToApp(zametti::settings().style());
        QFont font(zametti::settings().ui().sidebarFontFamily().isEmpty()
                       ? zametti::settings().style().fontFamily()
                       : zametti::settings().ui().sidebarFontFamily());
        font.setPointSizeF(zametti::settings().ui().sidebarFontPoint());
        panels.setSidebarFont(font);
        resultsView.setFont(font);
        historyView.list().setFont(font);

        zametti::applyPalette(editor);
        archiveView.refreshAppearance();
        docView.refreshAppearance();
        panels.refreshAppearance();
        zametti::applyPalette(resultsView);

        toolbar.refreshAppearance();
        statusBar.refreshAppearance();
        editor.refreshAppearance();
        history.refreshAppearance();
        markdown.refreshAppearance();
        settingsMode.refreshAppearance();

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
    // ПЕРЕЧИТАТЬ КОНФИГ И ПРИМЕНИТЬ — ОДНО МЕСТО: и когда файл поправили
    // снаружи (сторож выше), и когда его записал внутренний редактор настроек.
    // Дедуп по байтам: запись изнутри зовёт перечитывание сама, а следом
    // приходит сигнал сторожа о том же самом файле — второй раз пересобирать
    // документ незачем.
    QByteArray appliedConfigBytes;
    const auto reloadConfig = [&] {
        {
            QFile file(zametti::configPath());
            const QByteArray now =
                file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
            if (now == appliedConfigBytes) return;
            appliedConfigBytes = now;
        }
        // Несохранённое — на диск до перезагрузки облика: пересборка документа
        // проходит через всю модель, и терять правки на ней недопустимо.
        editor.save(true);

        QString error;
        QStringList unknown;
        // Через ZApp: он перечитывает настройки и заодно раздаёт бюджеты кэшам
        // (сторож прежде звал loadSettings напрямую и кэши не трогал).
        if (!zapp.reloadSettings(&error, &unknown)) {
            // Мусор в конфиге — это не повод перекрашивать окно наугад:
            // работаем на прежних значениях и говорим, что именно не так.
            std::fprintf(stderr, "config not accepted: %s\n", error.toUtf8().constData());
            // В полосе — причина, а не путь. Путь длиннее всей полосы, и в
            // первом же снимке многоточие съело ровно то, ради чего сообщение
            // и показывают: «object is missing after a comma».
            QString why = error;
            const QString prefix = zametti::configPath() + QStringLiteral(": ");
            if (why.startsWith(prefix)) why = why.mid(prefix.size());
            statusBar.setMessage(QStringLiteral("config not accepted: %1").arg(why));
            return;
        }
        applyAppearance();
        // Незнакомый ключ — это не «настройка не работает», а опечатка или
        // придуманное имя, и молчать о нём нельзя: владелец потерял вечер на
        // «caretColor» вместо «colors.caret».
        statusBar.setMessage(unknown.isEmpty()
                                 ? QString()
                                 : QStringLiteral("unrecognized in config: %1")
                                       .arg(unknown.join(QStringLiteral(", "))));
        if (!unknown.isEmpty())
            std::fprintf(stderr, "unrecognized in config: %s\n",
                         unknown.join(QStringLiteral(", ")).toUtf8().constData());
    };
    QObject::connect(&configSettle, &QTimer::timeout, &window, [&] {
        watchConfig();
        reloadConfig();
    });
    watchConfig();
    // Про конфиг, прочитанный на старте, сказать надо сразу, а не ждать, пока
    // человек его тронет: незнакомый ключ выглядит как «настройка не работает».
    if (!configUnknown.isEmpty()) {
        std::fprintf(stderr, "unrecognized in config: %s\n",
                     configUnknown.join(QStringLiteral(", ")).toUtf8().constData());
        statusBar.setMessage(QStringLiteral("unrecognized in config: %1")
                                 .arg(configUnknown.join(QStringLiteral(", "))));
    }

    // ТРИ СЕКЦИИ ВСЕГДА, а видимость средней — по тому, есть ли хранилище.
    // Раньше её добавляли только хранилищу, и это годилось, пока хранилище
    // открывалось один раз за запуск: вставить виджет в сплиттер на ходу
    // значит сменить ЧИСЛО секций, а splitterState в state.json помнит именно
    // его — ширины, снятые с трёх, не легли бы на две.
    splitter.addWidget(&panels.tree());
    splitter.addWidget(&panels.listPanel());
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
    // ПОДСКАЗКА — СКАЗАТЬ И ЗАБЫТЬ, и НИЧЕГО не запирать. Отдельно от
    // importStatus нарочно: тот запирает окно на время ввоза, и подсказка,
    // приехавшая по его проводам, гасила интерфейс навсегда (инцидент
    // 26.08.2026, нашёл владелец: буква на выбранной картинке).
    QObject::connect(&editor, &zametti::NoteEditor::hint, &window, [&](const QString& text) {
        statusBar.setMessage(text);
        if (text.isEmpty()) return;
        QTimer::singleShot(3000, &statusBar, [&statusBar] { statusBar.setMessage(QString()); });
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
                     QStringLiteral(" changed outside the app, and there are unsaved "
                                    "edits here."));
        QPushButton* mine =
            ask->addButton(QStringLiteral("Keep mine"), QMessageBox::AcceptRole);
        QPushButton* theirs =
            ask->addButton(QStringLiteral("Take external"), QMessageBox::DestructiveRole);
        ask->setDefaultButton(mine);
        ask->setInformativeText(
            QStringLiteral("Keep mine — the external version will be overwritten on "
                           "save. Take external — your edits can be brought back with undo."));
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
    if (!wanted.isEmpty() && session.lastFile() == wanted &&
        !zapp.state().knowsCaret(QFileInfo(wanted).completeBaseName()))
        zapp.state().rememberCaret(QFileInfo(wanted).completeBaseName(),
                                   {session.caret(), session.anchor(), 0});
    // Сама заметка откроется ниже, в attachStore: и старт, и переключение
    // хранилища идут одной дорогой, и «какую заметку показать» решается там.

    // Сохранение переписало файл — заголовок, начало текста и дата в строке
    // списка меняются вслед за ним: хранилище перечитывает заметку и говорит
    // дереву, дерево — списку.
    QObject::connect(&editor, &zametti::NoteEditor::fileSaved, &window,
                     [&](const QString& file) { model.refreshNote(file); });

    const auto shortcut = [&window](const QKeySequence& keys, auto&& slot) {
        QObject::connect(new QShortcut(keys, &window), &QShortcut::activated, &window, slot);
    };
    // У КАЖДОГО РЕЖИМА СВОЙ МАСШТАБ, И КЛАВИШИ ОДНОГО НЕ ТРОГАЮТ ДРУГОЙ
    // (решение владельца). Правило одно на все режимы, и держится оно тем, что
    // ветка выбирается ПО ТОМУ, ЧТО НА ВИДУ, а не по тому, кто как устроен.
    //
    // Заведено это было для исходника: Ctrl+= в нём молча увеличивал СКРЫТЫЙ
    // обычный вид — отжал [M], а заметка вдруг крупнее, хотя её масштаб не
    // трогали. 27.08.2026 владелец нашёл ту же беду у ИСТОРИИ: Ctrl+− на
    // разности уменьшал живую заметку, и это обнаруживалось только при выходе
    // из режима. Лечение то же, и число у режима своё (ZAppState::historyZoom).
    //
    // Архивная заметка и документация — НЕ режимы: это те же документы теми же
    // глазами, и масштаб у них общий с обычным видом.
    // ЧЕЙ СЕЙЧАС МАСШТАБ — спрашивается ОДНОЙ функцией (zoom_target.h), и оба
    // места ниже спрашивают именно её: «применить» и «шагнуть от текущего».
    const auto zoomTarget = [&] {
        return zametti::zoomTargetFor(settingsMode.active(), markdown.active(), history.active());
    };
    auto applyZoom = [&](qreal value) {
        switch (zoomTarget()) {
            case zametti::ZoomTarget::Plain:
                // ПЛОСКИЕ ВИДЫ ДЕРЖАТ ОДИН МАСШТАБ (решение владельца):
                // исходник и конфиг — один и тот же текст тем же шрифтом, и
                // открываться разного размера они не должны. Ставим обоим
                // сразу, какой бы из них ни был на виду.
                markdownView.applyZoom(value);
                settingsView.applyZoom(value);
                return;
            case zametti::ZoomTarget::History:
                historyView.textView().applyZoom(value);
                return;
            case zametti::ZoomTarget::Note:
                editor.applyZoom(value);
                // Архивная заметка и документация — те же документы теми же
                // глазами: масштаб у них общий с обычным видом, а не свой. Ради
                // этого документация и переехала из окна «о программе» в
                // дерево: там Ctrl+= не работал вовсе, и README читался всегда
                // в базовом кегле.
                archiveView.applyZoom(value);
                docView.applyZoom(value);
                return;
        }
    };
    auto stepZoom = [&](qreal factor) {
        // От чего шагаем — от масштаба ТОГО ЖЕ, кому и применим.
        const qreal now = zoomTarget() == zametti::ZoomTarget::Plain ? markdownView.zoom()
                          : zoomTarget() == zametti::ZoomTarget::History
                              ? historyView.textView().zoom()
                              : editor.zoom();

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
        // В РЕЖИМЕ ПРАВКИ НАСТРОЕК Ctrl+S пишет конфиг (и окно его применяет).
        if (settingsMode.active()) {
            settingsMode.save();
            return;
        }
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
        if (title.isEmpty()) title = QStringLiteral("Untitled");
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

    // КАКАЯ СТРАНИЦА СТЕКА НА ВИДУ — ОДНО МЕСТО НА ВСЕ РЕЖИМЫ, иначе каждый
    // режим решал бы это по-своему и они разошлись бы (так и было: из истории
    // человек возвращался в обычный вид, хотя ушёл из исходника). Порядок:
    // настройки > история > исходник > документация > архивная > редактор.
    //
    // Архивная стоит ПОД исходником сознательно: посмотреть её markdown человек
    // вправе (там она тоже только для чтения), а вот править — нет.
    //
    // Документация выше архивной, но ниже режимов, и вот почему это не спор:
    // войдя в неё, из режимов выходят (showDoc ниже), а их кнопки на ней
    // погашены. Признак — показанный документ, отдельного контроллера у
    // страницы нет: у неё нет ни своего состояния, ни правки, ни выхода
    // «наполовину».
    const auto showPage = [&] {
        const bool archived = editor.isArchivedNote();
        const bool doc = !docView.path().isEmpty();
        textStack.setCurrentWidget(settingsMode.active() ? static_cast<QWidget*>(&settingsView)
                                   : history.active()    ? static_cast<QWidget*>(&historyView)
                                   : markdown.active()   ? static_cast<QWidget*>(&markdownView)
                                   : doc                 ? static_cast<QWidget*>(&docView)
                                   : archived            ? static_cast<QWidget*>(&archiveView)
                                                         : static_cast<QWidget*>(&editor));
    };
    // ЧТО СЕЙЧАС НА СТРАНИЦЕ ЧТЕНИЯ. Она показывает два разных рода: вшитую
    // документацию (за ней нет файла в хранилище) и настоящую заметку под
    // меткой `access: read-only`. Отличаем не своим флагом рядом, а вопросом
    // хранилищу о показанном пути: флаг рядом с состоянием — это второе
    // состояние, которое однажды разойдётся с первым.
    const auto showingDoc = [&] {
        const std::shared_ptr<zametti::ZStorage> store = model.storage();
        return store != nullptr && !docView.path().isEmpty() &&
               store->virtualNoteAtPath(docView.path()) != nullptr;
    };

    // Что показывают страницы ЧТЕНИЯ — производное от открытой заметки, и
    // восстанавливает это ОДНА функция: её зовут все двери, через которые
    // заметка сменяется.
    const auto refreshReadingPages = [&] {
        if (editor.isArchivedNote()) {
            archiveView.showFile(editor.filePath());
            // Масштаб — общий с обычным видом: это тот же документ и те же
            // глаза. Ставится ПОСЛЕ показа: документ подменён, а масштаб несёт
            // шрифт документа.
            archiveView.applyZoom(editor.zoom());
        } else {
            archiveView.clear();
        }
        // ЗАПЕРТАЯ ЗАМЕТКА — ТОЖЕ ЧТЕНИЕ, но не отложенное: тон обычный, как у
        // документации. Впереди ввезённые книжки, у которых это норма.
        //
        // И здесь же: ОТКРЫЛИ ЗАМЕТКУ — ДОКУМЕНТАЦИЯ УХОДИТ С ДОРОГИ. Человек
        // попросил показать заметку, а не справку; так же ведёт себя режим
        // правки настроек. Правило одно и стоит здесь, потому что дверей
        // много: дерево, список, поиск, восстановление из истории, старт.
        if (editor.isReadOnlyNote() && !editor.isArchivedNote()) {
            docView.showFile(editor.filePath());
            docView.applyZoom(editor.zoom());
        } else if (!docView.path().isEmpty()) {
            docView.clear();
            statusBar.setMessage(QString());
        }
        showPage();
    };

    // СМЕНА ЗАМЕТКИ — единственный источник правды для страниц чтения.
    // Подписка идёт на fileChanged, потому что дверей много (дерево, список,
    // поиск, восстановление, старт), а правило одно.
    QObject::connect(&editor, &zametti::NoteEditor::fileChanged, &window,
                     [&](const QString&) { refreshReadingPages(); });

    // ТУЛБАР ИДЁТ ЗА ТЕМ, ЧТО ОТКРЫТО. Контроллер сам подписан на смену заметки
    // и на смену показанного документа — окну остаётся только отвечать, что у
    // него на виду.
    //
    // ЗАВОДИТСЯ ИМЕННО ЗДЕСЬ, ПОСЛЕ подписки страниц чтения, и это не вкус:
    // сигналы доставляются в порядке подписки, и пересчёт, вставший раньше,
    // считал бы кнопки по ЕЩЁ НЕ УБРАННОЙ документации. Ниже же стоят
    // обработчики режимов — им пересчёт тоже нужен, и отсюда он им виден.
    zametti::ToolbarController toolbarState(
        toolbar, editor, docView, [&] {
            zametti::ToolbarState state;
            state.store = model.isStore();
            // ДОКУМЕНТАЦИЯ — ЭТО ТА, ЧТО НА ВИДУ. Режим поверх неё (настройки,
            // история, исходник) закрывает её собой, и говорить про неё
            // «documentation is read-only», пока на экране другое, было бы
            // враньём.
            state.documentation = showingDoc() && !settingsMode.active() &&
                                  !markdown.active() && !history.active();
            state.readOnlyNote = editor.isReadOnlyNote();
            state.archivedNote = editor.isArchivedNote();
            state.cloudConfigured = cloudSync.configured();
            state.cloudStatus = cloudSync.statusText();
            return state;
        });

    QObject::connect(&history, &zametti::HistoryController::modeChanged, &window, [&](bool on) {
        // Кнопки — за режимом: вход в историю поверх документации закрывает её
        // собой, и запрет «documentation is read-only» с экрана уходит вместе с
        // ней.
        toolbarState.refresh();
        // Вид истории на месте редактора; таймлайн сбоку.
        showPage();
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
        showPage();
        toolbarState.refresh();   // кнопки — за тем, что на виду
        toolbar.setChecked(zametti::Toolbar::Button::MarkdownEdit, on);
        if (settingsMode.active()) return;   // на виду настройки — фокус их
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
                            ? QStringLiteral("the source contains a note header — remove it")
                            : QStringLiteral("the edit failed to apply and was undone — this is a defect"));
    });

    // РЕЖИМ ПРАВКИ НАСТРОЕК — четвёртая страница стека. Кнопка-шестерёнка —
    // переключатель, как [M] и история.
    QObject::connect(&settingsMode, &zametti::SettingsController::modeChanged, &window,
                     [&](bool on) {
                         showPage();
                         toolbarState.refresh();   // кнопки — за тем, что на виду
                         toolbar.setChecked(zametti::Toolbar::Button::Settings, on);
                         if (on) {
                             settingsView.setFocus();
                             return;
                         }
                         // Вышли — фокус тому, кто снова на виду.
                         if (markdown.active()) markdownView.setFocus();
                         else if (history.active()) historyView.setFocus();
                         else editor.setFocus();
                     });

    // ЗАПИСАЛИ КОНФИГ ИЗНУТРИ — ПРИМЕНЯЕМ ТЕМ ЖЕ ПУТЁМ, что и внешнюю правку:
    // одно место (reloadConfig), одна жалоба в полосе, если файл не принят.
    QObject::connect(&settingsMode, &zametti::SettingsController::saved, &window,
                     [&](bool ok, const QString& error) {
                         if (!ok) {
                             std::fprintf(stderr, "config not written: %s\n",
                                          error.toUtf8().constData());
                             statusBar.setMessage(
                                 QStringLiteral("config not written: %1").arg(error));
                             return;
                         }
                         watchConfig();
                         reloadConfig();
                     });

    QObject::connect(&history, &zametti::HistoryController::indexChanged, &window,
                     [&](int) { showHistoryState(); });
    // Восстановление — одно на баннер и на кнопку тулбара; статус пишет окно.
    QObject::connect(&history, &zametti::HistoryController::restored, &window, [&](qint64 source) {
        findBar.setStatus(QStringLiteral("restored from snapshot %1")
                              .arg(zametti::historyMoment(source)));
    });
    QObject::connect(&history, &zametti::HistoryController::restoreWasCurrent, &window, [&] {
        findBar.setStatus(QStringLiteral("this snapshot is already the current version"));
    });


    // ПРАВКА ШАПКИ ЗАКРЫТОЙ ЗАМЕТКИ ЖАЛУЕТСЯ САМА. Отказ записи означал бы, что
    // папка на диске осталась заметкой, а перенесённая — неперенесённой, и оба
    // раза молча. Пишет хранилище (ZStorage::rename/move/setSortMark — штатный
    // путь записи и шаг журнала); здесь только слово человеку.
    const auto complain = [&window](const QString& file, const QString& why) {
        std::fprintf(stderr, "note edit failed: %s — %s\n",
                     file.toUtf8().constData(), why.toUtf8().constData());
        QMessageBox::warning(&window, QStringLiteral("zametti"),
                             QStringLiteral("Could not write %1: %2")
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
        statusBar.setMessage(QStringLiteral("store reloaded"));
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

    // ЗАПЕРТОЕ НЕ ПРАВИТСЯ, И ОБ ОТКАЗЕ ГОВОРЯТ ВСЛУХ. Одно место на все
    // команды окна: молчаливый отказ читается как поломка, а мы не
    // спрашиваем «вы уверены?» — значит обязаны хотя бы сказать, почему не
    // сделали. Хранилище откажет и само (заслоны в ZStorage), но человек
    // увидит только эту строку.
    const auto refuseLocked = [&](const QString& folderOrNote) {
        const std::shared_ptr<zametti::ZStorage> store = model.storage();
        if (store == nullptr || folderOrNote.isEmpty()) return false;
        if (!store->isReadOnly(folderOrNote)) return false;
        statusBar.setMessage(QStringLiteral("read-only — nothing is written here"));
        // Явный захват, а не [&]: лямбда переживёт этот вызов на три секунды, и
        // ссылаться она должна на полосу сведений, а не на чужой кадр.
        QTimer::singleShot(3000, &statusBar,
                           [&statusBar] { statusBar.setMessage(QString()); });
        return true;
    };

    const auto deleteNote = [&](const QString& noteId) {
        if (!model.isStore() || noteId.isEmpty()) return;
        if (refuseLocked(noteId)) return;
        // Сама строка «Архив» ничему не подлежит: файла за ней нет. Так же и
        // виртуальные папки: hasNote отвечает про каталог, а их в нём нет.
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
                    QStringLiteral("Delete \"%1\" permanently?")
                        .arg(model.titleOfId(noteId)));
                if (answer != QMessageBox::Yes) return;
            }
            // Если заметка открыта, сначала сохраняем: иначе последним слепком
            // в истории осталось бы состояние до последних правок, а человек
            // удаляет то, что видит. Само удаление — дело хранилища: надгробие
            // или журнал вместе с архивной, картинки следом.
            if (wasOpen) editor.save(false);
            QString deleteError;
            if (!zapp.storage()->remove(
                    noteId, zametti::deletedImageLimitsFrom(zametti::settings().images()), &deleteError)) {
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
                                 QStringLiteral("Not everything could be archived:\n%1")
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
                                 QStringLiteral("Not everything could be restored:\n%1")
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
        // В запертую папку не создают. Без этого заметка молча уехала бы в
        // корень (createNote гасит негодного родителя), и человек не понял бы,
        // куда она делась.
        if (refuseLocked(requestedParent)) return;
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
        if (refuseLocked(parentId)) return;
        const QStringList files = QFileDialog::getOpenFileNames(
            &window, QStringLiteral("Import notes"), QString(),
            QStringLiteral("Markdown notes (*.md *.markdown);;All files (*)"));
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
                QStringLiteral("Files not imported: %1\n\n%2")
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

        const QString markdown = QStringLiteral("Markdown with images (*.md)");
        const QString pdf = QStringLiteral("PDF (*.pdf)");

        // Диалог свой, а не getSaveFileName: тому нельзя сказать «сменили
        // фильтр — смени и расширение», а без этого человек, выбрав PDF,
        // сохранял файл с именем «Заметка.md» и получал markdown.
        QFileDialog dialog(&window, QStringLiteral("Export note"), exportDir);
        dialog.setAcceptMode(QFileDialog::AcceptSave);
        dialog.setNameFilters({markdown, pdf});
        dialog.setDefaultSuffix(QStringLiteral("md"));
        // СВОЙ ДИАЛОГ, А НЕ СИСТЕМНЫЙ: в системный виджет не вставить, а
        // галочка нужна именно здесь, рядом с именем файла. Qt в этом случае
        // молча ничего не показывает — поэтому DontUseNativeDialog стоит явно.
        dialog.setOption(QFileDialog::DontUseNativeDialog, true);
        auto* keepMetaBox =
            new QCheckBox(QStringLiteral("Keep name and metadata"), &dialog);
        keepMetaBox->setChecked(exportKeepMeta);
        keepMetaBox->setToolTip(QStringLiteral(
            "The file goes out as is: with the header and under its identifier name.\n"
            "Dropped into another zametti store, such a file lands in Lost & found.\n"
            "Unchecked, plain markdown goes out under a human-readable name."));
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
                                 QStringLiteral("Could not export the note.\n%1")
                                     .arg(report.error));
            return;
        }
        // Молчать нельзя ровно в двух случаях: что-то переименовано или чего-то
        // не нашлось. В остальных человек и так видит файл там, где просил.
        if (!report.notes.isEmpty())
            QMessageBox::information(&window, QStringLiteral("zametti"),
                                     QStringLiteral("Note exported to %1.\n\n%2")
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
        // Метка порядка — тоже правка шапки папки.
        if (refuseLocked(folderId)) return;
        if (folderId.isEmpty()) {
            const zametti::SortOrder chosen =
                order.value_or(zametti::defaultOrder(zametti::SortKey::Modified));
            panels.setRootSort(chosen);
            // И в шапку корня: порядок «всех заметок» принадлежит хранилищу.
            if (model.isStore()) {
                const QString rootNote = zapp.storage()->rootId();
                QString sortError;
                if (!rootNote.isEmpty() &&
                    !zapp.storage()->setSortMark(rootNote, chosen,
                                                 zametti::NoteEditor::historyRules(), &sortError))
                    std::fprintf(stderr, "root sort not saved: %s\n",
                                 sortError.toUtf8().constData());
            }
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
        QStringList parts = zametti::ZSystem::splitCommand(command);
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
        if (!zametti::ZSystem::startDetached(program, parts))
            QMessageBox::warning(&window, QStringLiteral("zametti"),
                                 QStringLiteral("Failed to start: %1").arg(command));
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
        ask->setText(QStringLiteral("The external edit of \"%1\" lost: %2.")
                         .arg(model.titleOfId(QFileInfo(file).completeBaseName()),
                              keys.join(QStringLiteral(", "))));
        ask->setInformativeText(
            QStringLiteral("Restore the previous values? Text edits will be kept, "
                           "and the restore can be undone (Ctrl+Z)."));
        QPushButton* restore =
            ask->addButton(QStringLiteral("Restore"), QMessageBox::AcceptRole);
        ask->addButton(QStringLiteral("Leave as is"), QMessageBox::RejectRole);
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
        menu.addAction(QStringLiteral("Refresh (F5)"), [&] { reloadStore(); });
        // ЗАПЕРТАЯ СТРОКА — МЕНЮ БЕЗ ПРАВЯЩИХ ПУНКТОВ, а не с погашенными:
        // серый пункт обещает «когда-нибудь можно», а здесь нельзя никогда,
        // пока не снята пометка. Обновление оставляем — оно ничего не пишет.
        if (model.isReadOnlyIndex(at)) {
            menu.exec(tree.viewport()->mapToGlobal(pos));
            return;
        }
        menu.addSeparator();
        menu.addAction(QStringLiteral("New note"),
                       [&] { createNote(model.folderIdFor(at), false); });
        menu.addAction(QStringLiteral("New folder"),
                       [&] { createNote(model.folderIdFor(at), true); });
        menu.addAction(QStringLiteral("Import…"),
                       [&] { importNotes(model.folderIdFor(at)); });

        // «Сортировать по» — вторая дверь туда же, куда ведут три кнопки
        // тулбара, и единственная, где можно СБРОСИТЬ метку: кнопками порядок
        // только задают. Пункт «умолчанию» есть у папки и нет у корня —
        // корневой переключатель не наследует ни от кого.
        {
            const QString folderId = model.folderIdFor(at);
            QMenu* sortMenu = menu.addMenu(QStringLiteral("Sort by"));
            bool fromMark = false;
            const zametti::SortOrder now =
                model.effectiveSortFor(folderId, panels.rootSort(), &fromMark);
            const bool own = model.explicitSortOf(folderId).has_value();
            if (!folderId.isEmpty()) {
                QAction* reset = sortMenu->addAction(QStringLiteral("default"), [&, folderId] {
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
            menu.addAction(QStringLiteral("Rename"), [&] { tree.edit(at); });
            if (model.inArchiveId(id))
                menu.addAction(QStringLiteral("Restore from archive"), [&] { restoreNote(id); });
            menu.addAction(model.inArchiveId(id) ? QStringLiteral("Delete permanently")
                                                 : QStringLiteral("Archive"),
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
        menu.addAction(QStringLiteral("Import…"),
                       [&] { importNotes(panels.currentFolderId()); });
        if (id.isEmpty()) {
            menu.exec(listView.viewport()->mapToGlobal(pos));
            return;
        }
        // ЗАПЕРТАЯ СТРОКА — ТОЛЬКО ЧИТАЮЩИЕ ПУНКТЫ. Вывоз читает и уносит
        // прочитанное, это не правка; всё остальное — правка, и её здесь нет.
        // «Открыть во внешнем редакторе» тоже нет: у вшитого документа файла
        // на диске не существует, и открывать нечего.
        if (model.isReadOnlyId(id)) {
            menu.addSeparator();
            menu.addAction(QStringLiteral("Export…"),
                           [&] { exportNote(model.pathOfId(id).isEmpty()
                                                ? list.pathAt(at)
                                                : model.pathOfId(id)); });
            menu.exec(listView.viewport()->mapToGlobal(pos));
            return;
        }
        menu.addSeparator();

        // Перенос: список папок плоским перечнем с отступами. Перетаскивание
        // работает и так, но мышью через всё дерево — не для длинного списка.
        QMenu* moveTo = menu.addMenu(QStringLiteral("Move to folder"));
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
        menu.addAction(QStringLiteral("Refresh (F5)"), [&] { reloadStore(); });
        menu.addSeparator();
        menu.addAction(QStringLiteral("Open in external editor"),
                       [&] { openExternally(model.pathOfId(id)); });
        menu.addAction(QStringLiteral("Export…"),
                       [&] { exportNote(model.pathOfId(id)); });
        menu.addSeparator();
        if (model.inArchiveId(id))
            menu.addAction(QStringLiteral("Restore from archive"), [&] { restoreNote(id); });
        menu.addAction(model.inArchiveId(id) ? QStringLiteral("Delete permanently")
                                             : QStringLiteral("Archive"),
                       [&] { deleteNote(id); });
        menu.exec(listView.viewport()->mapToGlobal(pos));
    });

    // --- поиск --------------------------------------------------------------
    //
    // Панель одна на три команды; какая из них действует, решает её вид. F3
    // отдаётся той панели, что открыта, — открытие одной закрывает другую по
    // построению: панель-то одна.
    const auto searchRoot = [&] { return model.nodePath(QModelIndex()); };

    // ГДЕ ИЩЕМ — ОДИН ВОПРОС НА ВСЕ РЕЖИМЫ. У страницы стека, которая сейчас
    // на виду, спрашивается одно: «ты искомое» (TextSearchTarget), и дальше
    // окно зовёт одни и те же глаголы. Прежде здесь ветвилось пять мест по
    // markdown.active(): у заметки найденное живёт при заметке и адресуется
    // блоками, у плоских видов — смещениями в тексте, но окну эта разница не
    // нужна (долг из отчёта девятой сессии).
    // ИСКОМОЕ — ТА ЖЕ СТРАНИЦА, ЧТО НА ВИДУ, и порядок здесь ОБЯЗАН совпадать с
    // showPage: иначе Ctrl+F ищет в одном, а человек смотрит в другое.
    //
    // Так и было у архивной заметки: страницу показывал archiveView, а искал
    // Ctrl+F в редакторе — то есть в спрятанном документе, и на экране не
    // происходило ничего. Страница документации наступила бы на те же грабли,
    // и чинится это одной строкой на обе.
    const auto searchTarget = [&]() -> zametti::TextSearchTarget& {
        if (settingsMode.active()) return settingsView;
        if (markdown.active()) return markdownView;
        if (history.active()) return historyView.textView();
        if (!docView.path().isEmpty()) return docView;
        if (editor.isArchivedNote()) return archiveView;
        return editor;
    };

    // СЧЁТЧИК — ОДНО МЕСТО НА ВСЕХ. Он же отвечает за «1000+»: поиск, упёршийся
    // в потолок, знает не всё, и точное число писать нельзя — оно было бы
    // враньём. Пока никуда не встали, и текущего нет, так и говорим: «1000+/?».
    const auto counterText = [](const zametti::TextSearchTarget& target) {
        if (target.matchCount() == 0) return QStringLiteral("no matches");
        const QString total = target.matchesCapped()
                                  ? QStringLiteral("%1+").arg(target.matchCount())
                                  : QString::number(target.matchCount());
        if (target.currentMatch() < 0)
            return target.matchesCapped() ? total + QStringLiteral("/?") : total;
        return QStringLiteral("%1/%2").arg(target.currentMatch() + 1).arg(total);
    };

    const auto updateInNoteSearch = [&](const QString& text) {
        const zametti::Query query = zametti::makeQuery(text, findBar.regexOn());
        // Недописанное выражение — красные буквы в поле, и ничего больше:
        // ни слова об ошибке, правка продолжается (решение владельца).
        findBar.setQueryUsable(query.valid);
        zametti::TextSearchTarget& target = searchTarget();
        if (!query.usable()) {
            target.clearMatches();
            findBar.setStatus(QString());
            return;
        }
        target.findMatches(query);
        findBar.setStatus(counterText(target));
    };

    // РЕЖИМ ИСТОРИИ: Ctrl+F ищет и по показанному слепку, и по всей истории
    // этой заметки (решение владельца). Подсветка и F3 остаются на слепке —
    // это то, на что человек смотрит, — а список внизу показывает, в каких ещё
    // слепках встречается искомое.
    const auto updateHistorySearch = [&](const QString& text) {
        updateInNoteSearch(text);   // подсветка в слепке и счётчик
        const zametti::Query query = zametti::makeQuery(text, findBar.regexOn());
        if (query.isEmpty() || query.tooShort()) {
            results.clear();
            resultsView.hide();
            if (!query.isEmpty()) findBar.setStatus(QStringLiteral("need two characters"));
            return;
        }
        const zametti::HistorySearchReport report = history.searchHistory(text);
        results.setResults(report.hits);
        resultsView.setVisible(!report.hits.isEmpty());
        // Счётчик слепка уже написан updateInNoteSearch; дописываем к нему
        // историю, иначе одно из двух чисел молча пропадёт.
        zametti::TextSearchTarget& target = searchTarget();
        const QString inSnapshot = target.matchCount() > 0
                                       ? counterText(target) + QStringLiteral(" in snapshot")
                                       : QStringLiteral("none in snapshot");
        findBar.setStatus(report.hits.isEmpty()
                              ? inSnapshot + QStringLiteral(", nothing in history")
                              : QStringLiteral("%1; in history %2 in %3 snapshots%4")
                                    .arg(inSnapshot)
                                    .arg(report.hits.size())
                                    .arg(report.withHits)
                                    .arg(report.truncated ? QStringLiteral(", not all shown")
                                                          : QString()));
    };

    const auto showCounter = [&] { findBar.setStatus(counterText(searchTarget())); };

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

    // СМЕНА РЕЖИМА ПРИ ОТКРЫТОМ ПОИСКЕ — ТОЖЕ ПОИСК ЗАНОВО (решение владельца).
    // Искомое то же, а текст другой: в исходнике видна разметка, которой в
    // вёрстке нет вовсе, — значит и число вхождений, и их места другие. Тем же
    // путём, что смена заметки: одна лямбда на оба повода.
    const auto researchOnModeChange = [&](bool) {
        if (findBar.isHidden()) return;
        if (findBar.mode() == zametti::FindBar::Mode::InNote ||
            findBar.mode() == zametti::FindBar::Mode::Replace)
            updateInNoteSearch(findBar.query());
    };
    QObject::connect(&markdown, &zametti::MarkdownController::modeChanged, &window,
                     researchOnModeChange);
    QObject::connect(&history, &zametti::HistoryController::modeChanged, &window,
                     researchOnModeChange);

    // Найденное пересчиталось само после правки исходника — полосе пора
    // показать новое число (иначе счётчик остался бы от прошлого поиска).
    QObject::connect(&editor, &zametti::NoteEditor::matchesChanged, &window,
                     [&] { showCounter(); });
    QObject::connect(&markdownView, &zametti::MarkdownEditView::matchesChanged, &window,
                     [&] { showCounter(); });
    QObject::connect(&settingsView, &zametti::JsonEditView::matchesChanged, &window,
                     [&] { showCounter(); });

    // КТО ИЩЕТ ПО НАБРАННОМУ — ОДНО МЕСТО. Его зовут и правка запроса, и щелчок
    // по тумблеру выражений: буквы те же, а смысл у них другой, и искать надо
    // заново тем же путём.
    const auto searchForQuery = [&](const QString& text) {
        if (findBar.mode() == zametti::FindBar::Mode::History) {
            updateHistorySearch(text);
            return;
        }
        if (findBar.mode() == zametti::FindBar::Mode::Global) {
            const zametti::Query query = zametti::makeQuery(text, findBar.regexOn());
            if (query.isEmpty() || query.tooShort()) {
                storeSearch.cancel();
                searchDebounce.stop();
                results.clear();
                findBar.setStatus(query.isEmpty() ? QString()
                                                  : QStringLiteral("need two characters"));
                return;
            }
            searchDebounce.start();
            return;
        }
        updateInNoteSearch(text);
    };
    QObject::connect(&findBar, &zametti::FindBar::queryChanged, &window, searchForQuery);
    QObject::connect(&findBar, &zametti::FindBar::regexToggled, &window, [&](bool) {
        if (!findBar.isHidden()) searchForQuery(findBar.query());
    });

    QObject::connect(&searchDebounce, &QTimer::timeout, &window, [&] {
        // РЕЖИМ ЭКРАНА ЗАДАЁТ СМЫСЛ ПОИСКА ПО БАЗЕ: в [M] ищем по markdown,
        // в обычном виде — по тексту. Один Ctrl+Shift+F ведёт себя так же, как
        // Ctrl+F в открытой заметке, и объяснять это не приходится.
        storeSearch.search(searchRoot(), findBar.query(), findBar.regexOn(), markdown.active());
    });

    QObject::connect(&storeSearch, &zametti::StoreSearch::found, &window,
                     [&](const QString& text, const QVector<zametti::SearchResult>& found,
                         bool truncated, qint64) {
        // Ответ мог прийти на запрос, который уже никому не нужен: пока он
        // бежал, в поле успели дописать. Отсеиваем по самому запросу.
        if (text != findBar.query()) return;
        results.setResults(found);
        resultsView.setVisible(findBar.mode() == zametti::FindBar::Mode::Global);
        int notes = 0;
        for (int row = 0; row < results.rowCount(); ++row)
            if (results.isHeader(results.index(row, 0))) ++notes;
        if (found.isEmpty()) {
            findBar.setStatus(QStringLiteral("nothing"));
            return;
        }
        // ВРЕМЕНИ ЗДЕСЬ НЕТ (решение владельца): «6 in 2, 245 ms» читается как
        // «6 вхождений за 2 245 мс» — запятая между числами обманывает глаз, а
        // сама цифра человеку не нужна. Замеры живут на стенде, а не в панели.
        findBar.setStatus(truncated ? QStringLiteral("%1+ in %2, not all shown")
                                          .arg(found.size())
                                          .arg(notes)
                                    : QStringLiteral("%1 in %2").arg(found.size()).arg(notes));
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
                               ? tl->journal().indexOf(stamp, digest)
                               : -1;
            if (at >= 0) history.enter(at);
            // Прыжок на N-е вхождение в СЛЕПКЕ — у вида разности напрямую:
            // goToMatch знает только он (у плоских видов такого понятия нет),
            // и мы уже в режиме истории, показан именно он.
            const zametti::Query query = zametti::makeQuery(findBar.query(), findBar.regexOn());
            zametti::NoteView& snapshot = historyView.textView();
            snapshot.findMatches(query);
            snapshot.goToMatch(ordinal);
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
        const zametti::Query query = zametti::makeQuery(findBar.query(), findBar.regexOn());

        // НАШЛОСЬ В MARKDOWN — ТУДА И ВЕДЁМ. В обычном виде этой строки может
        // не быть вовсе («# vector» — это решётки заголовка, а в тексте блока
        // их нет), и прыжок по номеру попал бы не туда или никуда.
        if (index.data(zametti::SearchResultsModel::InMarkdownRole).toBool()) {
            if (!markdown.active()) markdown.enter();
            markdownView.findMatches(query);
            markdownView.goToMatch(ordinal);
            return;
        }
        if (markdown.active()) markdown.leave();
        editor.findMatches(query);
        editor.goToMatch(ordinal);
    };
    QObject::connect(&resultsView, &QAbstractItemView::clicked, &window, openResult);
    QObject::connect(resultsView.selectionModel(), &QItemSelectionModel::currentChanged,
                     &window, [&](const QModelIndex& index, const QModelIndex&) {
                         openResult(index);
                     });

    const auto stepSearch = [&](int direction) {
        if (findBar.isHidden()) return;
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
        zametti::TextSearchTarget& target = searchTarget();
        target.stepMatch(direction);
        showCounter();
        // ШАГ ПО НАЙДЕННОМУ ОТДАЁТ ФОКУС ТЕКСТУ (решение владельца). Человек
        // ищет фразу, чтобы её ПРАВИТЬ: попав на вхождение, он жмёт Backspace —
        // и до этой правки буква стиралась в поле поиска, потому что фокус
        // оставался там. Каретка уже стоит на находке (её ставит сам шаг), так
        // что тексту остаётся только принять ввод.
        target.searchWidget().setFocus(Qt::OtherFocusReason);
    };
    QObject::connect(&findBar, &zametti::FindBar::findNext, &window, [&] { stepSearch(1); });
    QObject::connect(&findBar, &zametti::FindBar::findPrevious, &window,
                     [&] { stepSearch(-1); });

    // ЗАМЕНА — ТОЖЕ У ИСКОМОГО. В режиме исходника это обычная правка текста:
    // она ложится в СВОЙ буфер отмены режима, а в заметку попадёт одним куском
    // при выходе. В слепке истории заменять нельзя, и об этом говорит сам
    // слепок (canReplace), а не особый случай здесь.
    QObject::connect(&findBar, &zametti::FindBar::replaceOne, &window, [&] {
        zametti::TextSearchTarget& target = searchTarget();
        if (!target.canReplace()) return;
        if (target.currentMatch() < 0) target.stepMatch(1);
        target.replaceCurrentMatch(findBar.replacement());
        showCounter();
    });
    QObject::connect(&findBar, &zametti::FindBar::replaceAll, &window, [&] {
        const zametti::Query query = zametti::makeQuery(findBar.query(), findBar.regexOn());
        if (query.isEmpty()) return;
        zametti::TextSearchTarget& target = searchTarget();
        if (!target.canReplace()) return;
        findBar.setStatus(QStringLiteral("replaced: %1")
                              .arg(target.replaceAllMatches(query, findBar.replacement())));
    });

    QObject::connect(&findBar, &zametti::FindBar::closed, &window, [&] {
        editor.clearMatches();
        markdownView.clearMatches();
        settingsView.clearMatches();
        historyView.textView().clearMatches();
        storeSearch.cancel();
        searchDebounce.stop();
        resultsView.hide();
        results.clear();
        searchTarget().searchWidget().setFocus();
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
        QString preset = searchTarget().searchPreset();
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
                QStringLiteral("searching the history of all notes is not supported yet"));
            QTimer::singleShot(3000, &statusBar,
                               [&statusBar] { statusBar.setMessage(QString()); });
            return;
        }
        openFind(zametti::FindBar::Mode::Global);
    };
    shortcut(QKeySequence(QStringLiteral("Ctrl+Shift+F")), openStoreFind);
    // СИНК — ОДНА ДВЕРЬ на кнопку облака и на Ctrl+Shift+S (просьба владельца,
    // 28.08.2026; на маке Qt сам показывает Cmd+Shift+S): не идёт — запустить,
    // сбросив правки на диск (выравнивание читает файлы, п.12), идёт —
    // отменить. Кнопка подсвечивается от stateChanged контроллера, каким бы
    // путём прогон ни запустили. Не настроен — молчим, как погашенная кнопка.
    const auto toggleSync = [&] {
        if (!cloudSync.configured()) return;
        if (!cloudSync.running()) editor.save(false, true);
        cloudSync.toggle();
    };
    shortcut(QKeySequence(QStringLiteral("Ctrl+Shift+S")), toggleSync);
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

    // --- ДОСТУПНОСТЬ КНОПОК ------------------------------------------------
    //
    // САМО ПРАВИЛО ЖИВЁТ НЕ ЗДЕСЬ (app/toolbar_state.h), а пересчёт ведёт
    // ToolbarController: он подписан на смену заметки и на смену показанного
    // документа и потому не может «забыть» пересчитать — это и была беда
    // 27.08.2026, когда после документации кнопки оставались серыми.
    //
    // Здесь остаётся то, что принадлежит ОКНУ, а не тулбару: полоса сведений.
    // Без хранилища она подсказывает, что делать, — это единственный намёк,
    // который человек в пустом окне получит.
    const auto refreshToolbar = [&] {
        toolbarState.refresh();
        statusBar.setMessage(model.isStore()
                                 ? QString()
                                 : QStringLiteral("No storage open — press the database "
                                                  "button to open or create one"));
    };

    // ДОКУМЕНТАЦИЯ ОТКРЫВАЕТСЯ ЧИТАЛКОЙ, А НЕ РЕДАКТОРОМ, и это не украшение:
    // openFile завёл бы вшитому документу журнал в ЧУЖОМ хранилище
    // (history/README.log), сторожа на путь ресурса и место в кэше заметок.
    // Развилка одна и стоит у двери, через которую человек выбирает строку.
    const auto showDoc = [&](const QString& path) {
        const std::shared_ptr<zametti::ZStorage> store = model.storage();
        const zametti::ZStorage::VirtualNote* doc =
            store == nullptr ? nullptr : store->virtualNoteAtPath(path);
        if (doc == nullptr) return false;
        // Режимы уходят с дороги — тем же решением, что и у правки настроек:
        // человек попросил показать документ, а не остаться в режиме.
        if (settingsMode.active()) settingsMode.leave();
        if (markdown.active()) markdown.leave();
        if (history.active()) history.leave();
        if (!docView.showFile(doc->path, doc->id)) return false;
        // ПОСЛЕ показа: масштаб несёт шрифт документа, а документ только что
        // подменили.
        docView.applyZoom(editor.zoom());
        // Панели обязаны знать, что «открыто» теперь это: иначе повторный
        // щелчок по строке не сработает (openFromPanel сверяется с ней), а
        // наполнение списка снимет выделение.
        panels.setCurrentNote(doc->path);
        window.setWindowTitle(docView.title() + QStringLiteral(" — zametti"));
        showPage();
        // Кнопки — до сообщения: refreshToolbar чистит полосу сведений, и
        // сказать в неё своё надо ПОСЛЕ него, иначе скажем в пустоту.
        refreshToolbar();
        statusBar.setMessage(docView.title() + QStringLiteral(" — documentation (read-only)"));
        docView.setFocus();
        return true;
    };

    // Панели говорят, что человек выбрал; открывает документ читалка, заметку —
    // редактор, и решает это одна развилка.
    QObject::connect(&panels, &zametti::NotePanels::noteChosen, &window,
                     [&](const QString& file, bool takeFocus) {
                         if (showDoc(file)) return;
                         editor.openFile(file, takeFocus);
                     });

    // --- ХРАНИЛИЩЕ ПРИЦЕПЛЯЕТСЯ И ОТЦЕПЛЯЕТСЯ ОДНОЙ ФУНКЦИЕЙ ----------------
    //
    // Её зовут ОБА пути: старт программы и кнопка «открыть хранилище». Пустой
    // корень — законный вход: так выглядит окно без хранилища. Два пути к
    // одному состоянию — классический способ получить «свежее открытие рисует,
    // переключился-вернулся — хвост пропал», поэтому путь один.
    //
    // ЗАКРЫТИЕ ХРАНИЛИЩА — ЭТО СМЕРТЬ ЕГО ОБЪЕКТА: отдельного close() у
    // ZStorage нет и не нужно, всё разбирает деструктор. Значит вся работа
    // здесь — проследить, чтобы не осталось ни одной копии shared_ptr; забытая
    // копия тихо удержит замок, и следующее открытие того же каталога упрётся
    // в «уже открыто другой копией zametti», указывающее на нас самих.
    // ШИРИНЫ КОЛОНОК. Зовётся и на старте, и при каждой смене хранилища: у
    // хранилища колонок ТРИ, без него — две, и Qt, показывая среднюю впервые,
    // отбирает место у соседей как ей вздумается (видно глазами: дерево
    // схлопывалось в шестьдесят точек).
    //
    // Сохранённое состояние сплиттера — только когда колонок столько же,
    // сколько было при сохранении: оно кодирует их ЧИСЛО, и снятое с трёх на
    // две не ляжет.
    const auto applyStartWidths = [&] {
        const int sidebar = zametti::settings().ui().sidebarWidth();
        const int noteList = zametti::settings().ui().noteListWidth();
        if (model.isStore()) {
            if (!session.splitterState().isEmpty() && splitter.restoreState(session.splitterState()))
                return;
            splitter.setSizes({sidebar, noteList, qMax(400, window.width() - sidebar - noteList)});
        } else {
            // Средняя спрятана, но в раскладке есть — ей ноль, и Qt не отдаст
            // ей места.
            splitter.setSizes({sidebar, 0, qMax(400, window.width() - sidebar)});
        }
    };

    // Ключ --unlock тратится на первом же прицеплении: он про названный
    // каталог, а не про всякий следующий.
    bool unlockOnce = unlock;
    const auto attachStore = [&](const QString& root, const QString& preferred) {
        // --- отцепление ---------------------------------------------------
        std::weak_ptr<zametti::ZStorage> departing = zapp.storage();
        if (model.isStore()) {
            // Правки на диск и каретку в память — до всего остального: дальше
            // заметка закроется, и спрашивать будет некого.
            editor.save(false, true);
            editor.rememberCurrentCaretInApp();
            // Режимы держат вид на ПРЕЖНЕЙ заметке; истории вдобавок
            // принадлежит журнал, а он смотрит на хранилище сырым указателем.
            if (history.active()) history.leave();
            if (markdown.active()) markdown.leave();
            if (settingsMode.active()) settingsMode.leave();
            archiveView.clear();
            storeSearch.cancel();
            results.clear();
            resultsView.hide();
        }
        // Документация могла быть на виду и БЕЗ хранилища — потому и снаружи
        // условия: в пустом окне она единственное, что вообще показывают.
        docView.clear();
        showPage();
        // Поток прогона синка держит СВОЮ копию указателя — контроллер его
        // дождётся.
        cloudSync.setStorage(nullptr);
        // КЭШ ЗАМЕТОК РЕДАКТОРА — ГЛАВНАЯ ОПАСНОСТЬ: в нём лежат журналы, а
        // ZJournal смотрит на хранилище СЫРЫМ указателем. Не вычистить — и
        // первое же обращение пойдёт по мёртвому адресу.
        editor.clearNoteCache();
        editor.closeFile();
        editor.setStorage(nullptr);
        panels.setStorage(nullptr);
        zapp.openStorage(QString());   // прежнее хранилище отпущено здесь

        // СТОРОЖ ЗАБЫТОЙ КОПИИ. Молча удержанный замок — беда, которую заметят
        // через час и не там, где она случилась.
        if (!departing.expired()) {
            const QString why = QStringLiteral(
                "the previous store is still referenced after detaching: its lock stays held");
            std::fprintf(stderr, "%s\n", why.toUtf8().constData());
            zapp.logs().err(why);
            Q_ASSERT(!"забытая копия shared_ptr<ZStorage> держит замок");
        }

        // --- прицепление --------------------------------------------------
        if (root.isEmpty()) {
            // ОКНО БЕЗ ХРАНИЛИЩА — ЭТО ХРАНИЛИЩЕ С ПУСТЫМ КОРНЕМ, а не
            // отсутствие объекта: isStore() у него ложь, каталог пуст, панели
            // ведут себя как прежде, — но виртуальные папки он несёт, и папка
            // Info остаётся на месте. Иначе документацию негде было бы
            // прочитать ровно тому, кому она нужнее всех: человеку, который
            // только что запустил программу впервые.
            panels.setStorage(zapp.storage());
            applyStartWidths();
            refreshToolbar();
            window.setWindowTitle(QStringLiteral("zametti"));
            return true;
        }
        auto storage = zapp.openStorage(root);
        // --unlock снимает ЧУЖОЙ забытый замок — и только с того хранилища,
        // которое человек назвал ключом, то есть ровно один раз, при первом
        // прицеплении. Дальше по кнопке он не действует: человек просил снять
        // замок у названного каталога, а не у всякого следующего.
        if (unlockOnce) {
            unlockOnce = false;
            std::fprintf(stderr, "%s\n", storage->forceUnlock().note.toUtf8().constData());
        }
        zametti::ZStorage::LockReport locked = storage->lock();
        if (!locked.note.isEmpty())
            std::fprintf(stderr, "%s\n", locked.note.toUtf8().constData());
        if (!locked.locked) {
            // ЗАНЯТО — И ЭТО МЕСТО, ГДЕ ЧЕЛОВЕКУ НУЖНА ДВЕРЬ. Забытый замок
            // мёртвой копии на ЭТОЙ машине снимается сам (это делает и Qt в
            // tryLock, и ZStorage::lock), но три случая ей не по зубам и по
            // делу: хранилище на сетевой шаре, замок с чужого хоста и pid,
            // доставшийся другому живому процессу. Раньше в этих случаях
            // оставался только ключ командной строки --unlock, а человек,
            // открывающий хранилище кнопкой, терминала перед собой не имеет.
            //
            // Предохранитель физический, а не вежливый: живой замок Qt держит
            // без FILE_SHARE_DELETE, и forceUnlock честно ответит, что снять
            // не вышло, — увести хранилище у работающей копии этой кнопкой
            // нельзя.
            auto* ask = new QMessageBox(&window);
            ask->setAttribute(Qt::WA_DeleteOnClose);
            ask->setIcon(QMessageBox::Warning);
            ask->setWindowTitle(QStringLiteral("zametti"));
            ask->setText(
                QStringLiteral("This store is already open by another copy of zametti:\n  %1")
                    .arg(root));
            ask->setInformativeText(
                QStringLiteral("The lock is held by pid %1 on \"%2\".\n\n"
                               "Close that copy and press Retry. If it is long dead and the "
                               "lock stayed behind, Force unlock removes the lock file.")
                    .arg(locked.holderPid)
                    .arg(locked.holderHost));
            // ТРИ ОТВЕТА, И ВСЕ ТРИ НАСТОЯЩИЕ. Retry ничего не разрушает —
            // человек закрыл ту копию и просит попробовать снова; это самый
            // частый случай, и прятать его внутрь «force» незачем.
            //
            // Force unlock назван тем, что он делает, а не тем, что человек
            // хочет получить: под Windows живой замок Qt не отдаст (файл открыт
            // без FILE_SHARE_DELETE), а вот под POSIX снести чужой открытый
            // файл МОЖНО — и та копия останется в уверенности, что держит
            // замок. Две программы в одном хранилище — ровно то, ради чего
            // замок и заведён, так что слово должно предупреждать.
            QPushButton* retry = ask->addButton(QMessageBox::Retry);
            QPushButton* force =
                ask->addButton(QStringLiteral("Force unlock"), QMessageBox::DestructiveRole);
            ask->addButton(QMessageBox::Cancel);
            ask->setDefaultButton(retry);
            ask->exec();
            const bool forced = ask->clickedButton() == force;
            const bool again = ask->clickedButton() == retry;

            // Отказались — остаёмся БЕЗ хранилища, а не наполовину в нём.
            const auto giveUp = [&] {
                zapp.openStorage(QString());
                refreshToolbar();
                window.setWindowTitle(QStringLiteral("zametti"));
            };
            if (!forced && !again) {
                giveUp();
                return false;
            }
            const QString note = forced ? storage->forceUnlock().note : QString();
            if (!note.isEmpty()) std::fprintf(stderr, "%s\n", note.toUtf8().constData());
            // Пробуем ещё раз. ОДИН повтор, а не цикл: если и теперь занято,
            // значит замок живой, и повторять вопрос незачем — ответ не
            // изменится. Что именно не вышло, говорит note (у Force unlock) или
            // сам отказ замка (у Retry).
            storage = zapp.openStorage(root);
            locked = storage->lock();
            if (!locked.locked) {
                giveUp();
                QMessageBox::warning(
                    &window, QStringLiteral("zametti"),
                    note.isEmpty() ? QStringLiteral("The store is still open elsewhere:\n  %1")
                                         .arg(root)
                                   : note);
                return false;
            }
        }
        // РАЗОВЫЕ МИГРАЦИИ ХРАНИЛИЩА — здесь, до того как дерево кто-нибудь
        // увидит: старая корзина переезжает в архивную пометку, а корневая
        // заметка заводится, если её нет (третье санкционированное исключение
        // из «загрузка не пишет»: без корня у хранилища нет ни имени, ни
        // порядка «всех заметок», и спрашивать тут не о чем).
        for (const QString& line : storage->migrate())
            std::fprintf(stderr, "%s\n", line.toUtf8().constData());
        QString rootError;
        if (storage->ensureRootNote(&rootError).isEmpty())
            std::fprintf(stderr, "no root note: %s\n", rootError.toUtf8().constData());

        panels.setStorage(storage);
        editor.setStorage(storage);
        cloudSync.setStorage(storage);

        // ПОРЯДОК КОРНЯ. Метка в шапке корневой заметки старше state.json: она
        // про хранилище, а не про устройство, и едет вместе с ним. Метки нет, а
        // в state.json порядок был — хранилище прежней сборки, порядок
        // переезжает в шапку разово (setSortMark штампа не ставит).
        if (const auto saved = zametti::parseSortOrder(session.treeSort())) panels.setRootSort(*saved);
        const QString rootNote = storage->rootId();
        if (!rootNote.isEmpty()) {
            const zametti::ZStorage::NoteInfo* info = storage->info(rootNote);
            const std::optional<zametti::SortOrder> mark =
                info != nullptr ? info->sortMark() : std::nullopt;
            if (mark.has_value()) {
                panels.setRootSort(*mark);
            } else if (const auto saved = zametti::parseSortOrder(session.treeSort())) {
                QString sortError;
                if (!storage->setSortMark(rootNote, *saved, zametti::NoteEditor::historyRules(),
                                          &sortError))
                    std::fprintf(stderr, "root sort not migrated: %s\n",
                                 sortError.toUtf8().constData());
            }
        }

        // Какую заметку показать: названную (прошлый сеанс), иначе первую
        // ОТКРЫВАЕМУЮ поиском в глубину; пустое хранилище получает заметку тут
        // же — окно без единой заметки показывать нечем.
        //
        // ИСКЛЮЧЕНИЕ — пустое хранилище С ОБЛАКОМ (владелец, п.10 первого
        // живого прогона): настоящие заметки привезёт первый синк, а
        // заведённая здесь пустышка уезжала бы в облако сиротой, и человек
        // удалял её руками на каждом устройстве. Показываем КОРНЕВУЮ заметку —
        // она настоящая (имя хранилища) и правится как любая другая.
        QString show = preferred;
        if (show.isEmpty()) {
            QString first = model.firstNoteId();
            if (first.isEmpty() && !storage->cloudConfig().hasCloudAddress()) {
                QString newError;
                if (storage->createNote(QString(), false, &newError).isEmpty())
                    std::fprintf(stderr, "%s\n", newError.toUtf8().constData());
                first = model.firstNoteId();
            }
            show = first.isEmpty() ? storage->pathOf(storage->rootId())
                                   : model.pathOfId(first);
        }
        if (!show.isEmpty() && !editor.openFile(show))
            std::fprintf(stderr, "unreadable: %s\n", show.toUtf8().constData());

        // Курсор дерева — на папку открытой заметки: от него зависит СОСТАВ
        // средней колонки, и без него список остаётся пустым при полном
        // хранилище (видно глазами на первом же переключении).
        panels.showNote(editor.filePath(), /*primary=*/true);
        applyStartWidths();
        if (zametti::settings().store().watchStore()) storage->setWatching(true);
        // Строка списка хранилищ устройства — освежить фактом: имя и облако
        // могли смениться с прошлого раза. Это же пополняет список всяким
        // открытым хранилищем (ключ --root, прежний storeRoot) — миграция
        // прежних состояний бесплатна.
        {
            zametti::ZStorage::Config entry = storage->cloudConfig();
            entry.name = storage->localStoreName();
            zapp.storeManager()->remember(entry);
        }
        refreshToolbar();
        return true;
    };

    // УПРАВЛЕНИЕ ХРАНИЛИЩАМИ — дверь кнопки database (решение владельца,
    // 27.08.2026; прежде тут был голый системный выбор каталога — теперь он
    // живёт внутри диалога, у Browse). Список известных хранилищ устройства,
    // «+»/«−», переключение, и у каждого — облако: адрес, логин, пароли,
    // скачивание с нового устройства и сброс пароля шифрования. Все ветки
    // знакомства с папкой и облаком решает ядро; окно только показывает.
    const auto manageStores = [&] {
        // Правки — на диск при входе (просьба владельца, 28.08.2026): всё,
        // что диалог сделает с хранилищем — прогон, скачивание, переключение,
        // — обязано видеть на диске свежую заметку.
        editor.save(false, true);
        // Прогон синка, если он идёт, останавливается и дожидается: диалог
        // будет трогать подключение того же хранилища.
        cloudSync.setStorage(zapp.storage());
        zametti::StoreManagerDialog::Result verdict;
        {
            // Блок не косметика: диалог держит копию shared_ptr хранилища, и
            // умереть он обязан ДО attachStore — сторож забытой копии не спит.
            zametti::StoreManagerDialog dialog(
                &window, zapp.storeManager()->stores(),
                model.isStore() ? zapp.storage()->root() : QString(), zapp.storage(),
                syncSecrets);
            dialog.exec();
            verdict = dialog.result();
        }
        // Список применяется ВСЕГДА (и по Esc): добавленное хранилище — не
        // черновик. Замена через те же двери, что и всё остальное: прежние
        // строки забываются, итог диалога вспоминается по порядку.
        zametti::ZStorageManager& out = *zapp.storeManager();
        const QList<zametti::ZStorage::Config> before = out.stores();
        for (const zametti::ZStorage::Config& e : before) out.forget(e.root);
        for (const zametti::ZStorage::Config& e : verdict.stores) out.remember(e);

        if (!verdict.switchToRoot.isEmpty()) {
            const bool ok = attachStore(verdict.switchToRoot, QString());
            // Только что скачанное хранилище — это манифест и корень: остальное
            // обязан привезти первый прогон, и ждать sync.onStart тут нечего.
            if (ok && (verdict.downloadedNew ||
                       (zametti::settings().sync().onStart() && cloudSync.configured())))
                cloudSync.startFull(false);
            return;
        }
        if (verdict.cloudChangedForCurrent) {
            // Кнопка облака оживает тут же, не дожидаясь смены заметки; адрес
            // и секреты контроллер достанет заново при прогоне. Правки — на
            // диск до прогона (выравнивание читает файлы).
            refreshToolbar();
            editor.save(false, true);
            cloudSync.startFull(false);
        }
    };

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
        // ЧЕРЕЗ ТУЛБАР, А НЕ ПРЯМО В КНОПКУ: подпись у неё живая, и тулбар
        // обязан её помнить — иначе первый же пересчёт доступности соберёт
        // подпись заново из таблицы и напишет «Hide side panels» на кнопке,
        // которая панели показывает.
        toolbar.setTip(Button::Panels, visible ? QStringLiteral("Hide side panels")
                                               : QStringLiteral("Show side panels"));
    };
    {
        // Зовём ВСЕГДА, а не только когда панели спрятаны: кнопка обязана
        // показывать своё состояние с первой секунды, а не с первого нажатия.
        showPanels(!session.panelsHidden());

        // КНОПКА ОБЛАКА ОЖИЛА (m17, сессия 4). Не настроенный синк — не
        // поломка и не обещание «потом», а состояние: тултип говорит, что
        // сделать. Настроенный: клик — полный прогон, клик во время — отмена.
        // САМО СОСТОЯНИЕ считает refreshToolbar (одно место на все кнопки);
        // здесь только подписка, чтобы тултип и цвет шли за прогоном.
        {
            QObject::connect(&cloudSync, &zametti::SyncController::stateChanged, &toolbar, [&] {
                toolbar.setTip(Button::Cloud, cloudSync.statusText());
                toolbar.setAccent(Button::Cloud, cloudSync.running());
            });
            // Индикатор прогона — в статус-баре, как у импорта картинок:
            // движок в рабочем потоке, звёздочка ездит, «]» не дёргается.
            QObject::connect(&cloudSync, &zametti::SyncController::progress, &statusBar,
                             [&statusBar](const QString& line) { statusBar.setMessage(line); });
            // ПРЕДОХРАНИТЕЛЬ МАССОВОГО УДАЛЕНИЯ — один из двух вопросов всего
            // синка (решение владельца): прогон задержал удаления и ждёт.
            // Подтвердил — повторный прогон с allowMassDelete; отказал —
            // заметки объявляются живыми, облако лечится, вопрос не
            // повторяется, потому что изменилось состояние.
            QObject::connect(
                &cloudSync, &zametti::SyncController::pendingDeletes, &window,
                [&](const QStringList& ids) {
                    // ПОЛНЫЕ имена, не id и не голые заголовки: «удаляется
                    // TODO» без пути — вопрос, а не ответ. Карта строится на
                    // выброс из живого каталога — заметки ещё лежат локально.
                    const zametti::ZStorage::NoteTree tree =
                        model.storage()->subtreeFor(ids);
                    QStringList names;
                    for (const QString& id : ids) names.append(tree.qualifiedName(id));
                    // Естественный порядок: «№2» перед «№10», как в дереве.
                    QCollator collator;
                    collator.setNumericMode(true);
                    std::sort(names.begin(), names.end(), collator);
                    using Verdict = zametti::PendingDeletesDialog::Verdict;
                    // Правки — на диск до повторного прогона: человек мог
                    // печатать, пока шёл фоновый (выравнивание читает файлы).
                    switch (zametti::PendingDeletesDialog::ask(&window, names)) {
                        case Verdict::DeleteHere:
                            editor.save(false, true);
                            cloudSync.startFull(true);
                            break;
                        case Verdict::KeepAlive:
                            editor.save(false, true);
                            cloudSync.declareAliveAndFinish(ids);
                            break;
                        case Verdict::DecideLater:
                            // Пометки dirty живы — следующий прогон спросит снова.
                            break;
                    }
                });
        }

        // ХРАНИЛИЩЕ ОТКРЫВАЕТСЯ ЗДЕСЬ — той же функцией, что и по кнопке.
        // Раньше это делал десяток блоков выше по main(), до того как окно
        // собрано; теперь путь один, и «свежее открытие» с «переключился»
        // отличаются только тем, что было до.
        attachStore(absRoot, wanted);

        QObject::connect(&toolbar, &zametti::Toolbar::pressed, &window, [&](Button id) {
            switch (id) {
            case Button::OpenStore:
                manageStores();
                break;
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
                // Правки — на диск перед нетривиальной операцией (просьба
                // владельца, 28.08.2026): упади ввоз — набранное уцелело.
                editor.save(false, true);
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
                    statusBar.setMessage(QStringLiteral("this note has no history yet"));
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
            case Button::Settings:
                // ПЕРЕКЛЮЧАТЕЛЬ, как история и правка исходника: конфиг
                // правится ВНУТРИ программы (решение владельца, refactor3;
                // прежде кнопка открывала config.json во внешнем редакторе).
                // Нет файла — модель пишет шаблон со всеми параметрами.
                settingsMode.toggle();
                break;
            case Button::Export:
                // Тот же резон: вывоз (особенно PDF) — нетривиальный путь,
                // и правки обязаны лечь на диск до него.
                editor.save(false, true);
                exportNote(editor.filePath());
                break;
            case Button::Cloud:
                // Та же дверь, что у Ctrl+Shift+S (save перед стартом внутри).
                toggleSync();
                break;
            }
        });
    }
    // ПОЛНОЭКРАННЫЙ ПРОСМОТР КАРТИНОК (решение владельца): F11, когда каретка
    // стоит на снимке, показывает его на весь экран, а стрелки листают снимки
    // ЭТОЙ заметки. Двойной щелчок занят правкой подписи, поэтому дверь —
    // клавиша и пункт меню, а не жест мышью.
    zametti::ImageViewer imageViewer(&window);
    const auto viewShotAtCaret = [&] {
        if (markdown.active() || settingsMode.active() || history.active()) return false;
        const zametti::NoteEditor::ShotList shots = editor.noteShots();
        if (shots.atCaret < 0) return false;
        std::vector<zametti::ImageViewer::Shot> list;
        for (int i = 0; i < shots.paths.size(); ++i)
            list.push_back({shots.paths.at(i), shots.captions.value(i)});
        return imageViewer.show(std::move(list), shots.atCaret);
    };
    QObject::connect(&editor, &zametti::NoteEditor::fullscreenShotRequested, &window,
                     [&] { viewShotAtCaret(); });
    QObject::connect(&imageViewer, &zametti::ImageViewer::closed, &window, [&] {
        window.raise();
        window.activateWindow();
        editor.setFocus();
    });

    // ПОЛНОЭКРАННАЯ ПРАВКА (решение владельца): остаются текст и полоса
    // сведений, уходят тулбар, боковые панели и рамка окна — вместе с меню и
    // доком системы, это делает сам оконный менеджер по showFullScreen.
    //
    // Панели прячутся ТЕМ ЖЕ showPanels, что и кнопка тулбара: два способа
    // прятать одно и то же разошлись бы (у кнопки — память ширин, у нас бы её
    // не было). Что было видно до перехода, помним и возвращаем.
    bool panelsBeforeFullscreen = true;
    const auto setFullscreen = [&](bool on) {
        if (on == window.isFullScreen()) return;
        if (on) {
            panelsBeforeFullscreen = toolbar.isChecked(Button::Panels);
            showPanels(false);
            // СПЕРВА ОКНО, ПОТОМ ТУЛБАР: showFullScreen перепоказывает окно, и
            // спрятанный до него ребёнок успевал показаться обратно (замер
            // пробника: «тулбар=1» сразу после входа).
            window.showFullScreen();
            toolbar.hide();
        } else {
            window.showNormal();
            toolbar.show();
            showPanels(panelsBeforeFullscreen);
        }
        // Фокус — тому, в чём человек работает: полноэкранный режим просят
        // ради текста.
        if (settingsMode.active()) settingsView.setFocus();
        else if (markdown.active()) markdownView.setFocus();
        else if (history.active()) historyView.setFocus();
        else editor.setFocus();
    };
    for (const QKeySequence& keys :
         zametti::keySequencesOf(zametti::settings().editor().fullscreenKey()))
        shortcut(keys, [&] {
            // Каретка на снимке — во весь экран уходит СНИМОК, а не текст: это
            // то, на что человек смотрит. Не на снимке — обычная полноэкранная
            // правка.
            if (viewShotAtCaret()) return;
            setFullscreen(!window.isFullScreen());
        });

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
            // ПОЛНОЭКРАННЫЙ РЕЖИМ ЗАКРЫВАЕТСЯ ПЕРВЫМ: он самый верхний слой, и
            // человек, нажавший Esc, ждёт обратно окно, а не закрытую панель
            // поиска под ним. Дальше — общий порядок (escapeActionFor).
            if (window.isFullScreen() && findBar.isHidden() &&
                editor.codeLanguageEditor() == nullptr && !editor.caretInOpenObject()) {
                setFullscreen(false);
                return;
            }
            switch (zametti::escapeActionFor(editor.codeLanguageEditor() != nullptr,
                                             editor.caretInOpenObject(), !findBar.isHidden(),
                                             settingsMode.active())) {
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
                case zametti::EscapeAction::LeaveMode:
                    // Только правка настроек: из режима исходника Esc не
                    // выводит (решение владельца) — там выходят кнопкой [M].
                    settingsMode.leave();
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
    // ШИРИНЫ КОЛОНОК — ПОСЛЕ ПОКАЗА ОКНА, а не до него. Сплиттер раздаёт место
    // по тому, что видит СЕЙЧАС: до show() у него нет ни своей ширины, ни
    // померенных детей, и просимые 260/320 схлопывались до минимума — при
    // первом запуске (когда в state.json ещё нет своего splitterState) обе
    // панели открывались полосками в 70 px. Найдено пробником полноэкранного
    // режима: «запомнили ширины 70,70». Ставим и сразу, и очередью — первое
    // задаёт пропорции до первой отрисовки, второе выправляет их, когда окно
    // уже знает свой размер.
    applyStartWidths();
    // ПЕРВАЯ ЗАМЕТКА ОТКРЫВАЕТСЯ ДО ТОГО, как встают подписки, — значит про
    // страницу архива её надо спросить отдельно, здесь. Иначе архивная,
    // открытая при запуске, показывалась бы редактором до первого перехода на
    // другую заметку (так и было; поймано снимком окна под Xvfb, а не набором:
    // проводка окна наборами не покрыта).
    refreshReadingPages();
    window.show();
    QTimer::singleShot(0, &window, applyStartWidths);
    // СИНК НА СТАРТЕ (дефолт вкл): в фоне, с акцентом на входящие — программа
    // открывается сразу, прилетевшее материализуется по ходу; открытая заметка
    // получает чужую версию existing external-путём, Ctrl+Z возвращает своё.
    if (zametti::settings().sync().onStart() && cloudSync.configured())
        QTimer::singleShot(0, &window, [&] { cloudSync.startFull(false); });

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
            zametti::ZStorage storage(storePath);
            const zametti::ZJournal::ThinReport report =
                storage.thinAllJournals(QDateTime::currentMSecsSinceEpoch());
            for (const QString& name : report.trimmed)
                std::fprintf(stderr, "journal %s: torn tail trimmed\n",
                             name.toUtf8().constData());
            for (const QString& line : report.problems)
                std::fprintf(stderr, "journal not thinned: %s\n", line.toUtf8().constData());
            if (report.recordsBefore != report.recordsAfter)
                std::fprintf(stderr, "journals thinned: records %lld -> %lld, bytes %lld -> %lld\n",
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
    panels.showNote(editor.filePath(), /*primary=*/true);

    // Фокус — после show() и после того, как дерево показало текущую заметку: до
    // show() окно ещё не решило, кому его отдать, и наш выбор затёрся бы первым
    // же виджетом в разделителе. Каретка при запуске должна быть сразу в тексте:
    // дерево нужно, чтобы выбрать заметку, а не чтобы в нём находиться.
    editor.setFocus();

    // РЕЖИМ ПРАВКИ ИСХОДНИКА ПЕРЕЖИВАЕТ ПЕРЕЗАПУСК (решение владельца): вышли
    // из программы с нажатой [M] — вернулись в неё же. После открытия заметки и
    // после фокуса: входить в режим нечем, пока показывать нечего.
    markdownView.applyZoom(session.plainZoom());
    settingsView.applyZoom(session.plainZoom());
    historyView.textView().applyZoom(session.historyZoom());
    if (session.markdownMode()) markdown.enter();



    QObject::connect(&app, &QCoreApplication::aboutToQuit, &window, [&] {
        // ИСХОДНИК НАКЛАДЫВАЕМ ДО ЗАПИСИ. Пока идёт режим, истина живёт в тексте
        // вида, и заметка о ней не знает: записать её первой значило бы
        // потерять всё, что человек набрал перед выходом.
        if (settingsMode.active()) settingsMode.save();
        if (markdown.active()) markdown.saveWithoutLeaving();
        // На выходе окно с ошибкой показывать поздно: жалуемся в stderr.
        editor.save(false, true);   // выходим: пробуем записать, не спрашивая признак

        // PUSH-ONLY НА ВЫХОДЕ (дефолт вкл): только исходящее, бюджет времени
        // внутри движка; офлайн — тихий пропуск, dirty-set переживает.
        if (zametti::settings().sync().onExit()) cloudSync.pushOnExit();

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
        // Число одно на оба плоских вида — берём у любого из них.
        out.setPlainZoom(markdownView.zoom());
        out.setHistoryZoom(historyView.textView().zoom());
        out.setExpandedDirs(panels.expandedDirs());
        out.setSearchHistory(findBar.history());
        out.setSearchRegex(findBar.regexOn());
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
    // копии хранилища владельца, глазами, а не рассуждением),
    // ZAMETTI_PROBE_SETTINGS=1 — открыть правку настроек до снимка.
    if (const QByteArray quitAfter = qgetenv("ZAMETTI_PROBE_QUIT_MS"); !quitAfter.isEmpty()) {
        const int ms = qMax(0, quitAfter.toInt());
        // ZAMETTI_PROBE_FULLSCREEN=1 — полноэкранная правка (приёмка глазами).
        if (qEnvironmentVariableIsSet("ZAMETTI_PROBE_FULLSCREEN"))
            QTimer::singleShot(ms / 2, &window, [&] {
                setFullscreen(true);
                // Состояние строкой — ПОСЛЕ цикла событий: показ окна у
                // оконного менеджера не мгновенный, и сразу после вызова
                // «тулбар ещё виден» ничего не значит. По этой строке и судят
                // глазами: полноэкранно ли окно, ушли ли тулбар и панели,
                // осталась ли полоса сведений.
                QTimer::singleShot(200, &window, [&] {
                    std::fprintf(stderr,
                                 "пробник: полный экран=%d тулбар=%d панели=%d полоса=%d\n",
                                 int(window.isFullScreen()), int(toolbar.isVisible()),
                                 int(panels.tree().isVisible()), int(statusBar.isVisible()));
                });
            });
        // ZAMETTI_PROBE_SIZE=ШИРИНАxВЫСОТА — размер окна для снимка приёмки.
        // Правило UI-матрицы требует ШИРОКОГО окна (три бага класса «у агента
        // окно узкое, у владельца широкое»), а получить его в снимке было
        // нечем: окно без сохранённой геометрии всегда 1150×780, и снаружи его
        // не растянуть — под offscreen оконного менеджера нет вовсе.
        if (const QByteArray size = qgetenv("ZAMETTI_PROBE_SIZE"); !size.isEmpty()) {
            const QList<QByteArray> wh = size.split('x');
            if (wh.size() == 2)
                QTimer::singleShot(ms / 4, &window, [&, wh] {
                    window.resize(qMax(200, wh[0].toInt()), qMax(200, wh[1].toInt()));
                    // Ширины колонок пересчитываются вслед за окном: сплиттер
                    // растягивает секции пропорционально, и панели после
                    // растяжки уехали бы не туда, где им место.
                    applyStartWidths();
                });
        }
        // ZAMETTI_PROBE_STORE=<каталог> — переключиться на другое хранилище на
        // ходу. Кнопка ведёт через системный диалог выбора каталога, а его
        // приёмка руками не воспроизводится; сама смена хранилища — вот она, и
        // проверяется тем же кодом (attachStore), что зовёт кнопка.
        if (const QByteArray to = qgetenv("ZAMETTI_PROBE_STORE"); !to.isEmpty())
            QTimer::singleShot(ms / 2, &window, [&, to] {
                const QString dir = QString::fromLocal8Bit(to);
                const QString was = model.isStore() ? zapp.storage()->root() : QString();
                std::fprintf(stderr, "probe: switching to %s\n", to.constData());
                const bool ok = attachStore(dir, QString());
                std::fprintf(stderr, "probe: store=%s note=%s toolbar-newnote=%d\n",
                             ok ? "attached" : "REFUSED",
                             editor.filePath().toUtf8().constData(),
                             int(toolbar.isEnabled(Button::NewNote)));
                // ОТПУЩЕН ЛИ ЗАМОК ПРЕЖНЕГО. Спрашиваем тем же способом, каким
                // спросит вторая копия программы: берём его. Забытая где-то
                // копия shared_ptr проявится ровно здесь и никак иначе.
                if (!was.isEmpty())
                    std::fprintf(stderr, "probe: previous store lock is %s\n",
                                 zametti::ZStorage(was).lock().locked ? "free" : "STILL HELD");
            });
        // ZAMETTI_PROBE_SETTINGS=1 — открыть правку настроек (приёмка глазами).
        if (qEnvironmentVariableIsSet("ZAMETTI_PROBE_SETTINGS"))
            QTimer::singleShot(ms / 2, &window, [&] {
                std::fprintf(stderr, "probe: settings edit %s\n",
                             settingsMode.enter() ? "opened" : "did not open");
            });
        if (qEnvironmentVariableIsSet("ZAMETTI_PROBE_HISTORY"))
            QTimer::singleShot(ms / 2, &window, [&] {
                std::fprintf(stderr, "probe: history mode %s\n",
                             history.enter() ? "entered" : "did not enter");
            });
        if (const QByteArray shot = qgetenv("ZAMETTI_PROBE_SHOT"); !shot.isEmpty())
            QTimer::singleShot(ms * 3 / 4, &window, [&window, shot] {
                window.grab().save(QString::fromLocal8Bit(shot));
            });
        QTimer::singleShot(ms, &app, &QCoreApplication::quit);
    }
    return app.exec();
}
