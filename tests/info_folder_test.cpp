// ПАПКА INFO И МЕТКА `access: read-only` — то, что видно человеку, и то, что
// стережёт его данные.
//
// Проверяется три уровня защиты сразу, и ни один не заменяет другие:
//
//   * ЯДРО — хранилище отказывается писать запертое (переименовать, перенести,
//     убрать в архив, удалить, поставить метку порядка) и говорит об этом;
//   * МОДЕЛЬ ОКНА — у запертой строки нет флагов правки и перетаскивания,
//     значит F2, drag и приём drop умирают, не доходя до команд;
//   * ВИД — читалка без каретки: нажатия не меняют документ и не поднимают
//     признак «изменено».
//
// И отдельно — то, ради чего всё затевалось: строка Info в дереве есть ВСЕГДА,
// в том числе когда хранилища нет вовсе (первый запуск программы), стоит
// последней, а её содержимое идёт в порядке файлов docs/info/, а не по датам,
// которых у вшитых документов не бывает.

#include "note_panels.h"
#include "note_tree.h"
#include "reader_view.h"
#include "resources.h"
#include "test_util.h"
#include "testdata.h"
#include "zapp.h"
#include "zstorage.h"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QKeyEvent>
#include <QModelIndex>
#include <QScrollBar>
#include <QStackedWidget>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextFragment>
#include <QAbstractTextDocumentLayout>
#include <QTemporaryDir>
#include <QTest>

#include <memory>
#include <string>
#include <vector>

namespace {

using zametti::NotePanels;
using zametti::NoteTreeModel;
using zametti::ReaderView;
using zametti::ZStorage;

std::string s(const QString& q) { return q.toStdString(); }
std::string n(long long v) { return std::to_string(v); }

zametti::ZJournal::Rules rules() { return zametti::ZJournal::Rules{}; }

QModelIndex byTitle(const NoteTreeModel& model, const QModelIndex& parent, const QString& title) {
    for (int row = 0; row < model.rowCount(parent); ++row) {
        const QModelIndex at = model.index(row, 0, parent);
        if (model.titleOf(at) == title) return at;
        const QModelIndex deeper = byTitle(model, at, title);
        if (deeper.isValid()) return deeper;
    }
    return {};
}

QStringList listTitles(const zametti::NoteListModel& list) {
    QStringList out;
    for (int row = 0; row < list.rowCount(); ++row)
        out << list.data(list.index(row, 0), zametti::NoteListModel::TitleRole).toString();
    return out;
}

// Имена вшитых документов в порядке файлов — то, чего мы ждём от средней
// колонки. Считается по тем же ресурсам, по которым строится папка: второго
// списка имён в программе нет, и в наборе его тоже быть не должно.
QStringList docFileNames() {
    QStringList out;
    for (const QString& path : zametti::embeddedDocs()) out << QFileInfo(path).completeBaseName();
    return out;
}

}  // namespace

static int ztRunSuite(int, char**) {
    // --- 1. БЕЗ ХРАНИЛИЩА: строка Info всё равно есть ------------------------
    //
    // Пустой корень — это законное хранилище, а не отсутствие объекта. Ради
    // этого случая механизм и живёт в ZStorage: человеку, впервые запустившему
    // программу, документация нужнее всех, а хранилища у него ещё нет.
    qint64 firstSetup = 0;
    {
        auto empty = std::make_shared<ZStorage>(QString());
        QElapsedTimer clock;
        clock.start();
        zametti::ZApp::instance().addInfoFolder(*empty);
        firstSetup = clock.nsecsElapsed() / 1000;
        std::printf("  заведение папки Info: %lld мкс на %d документов\n", (long long)firstSetup,
                    int(zametti::embeddedDocs().size()));
        NoteTreeModel model(empty);
        ZT_TRUE("без хранилища это не хранилище", !model.isStore());
        ZT_EQ("без хранилища видна одна строка — Info", "1", n(model.rowCount(QModelIndex())));
        const QModelIndex info = model.index(0, 0, QModelIndex());
        ZT_EQ("и это она", "Info", s(model.titleOf(info)));
        ZT_TRUE("она виртуальная", model.isVirtualFolder(info));
        ZT_TRUE("и запертая", model.isReadOnlyIndex(info));
        ZT_EQ("в ней столько строк, сколько вшито документов",
              n(zametti::embeddedDocs().size()), n(model.rowCount(info)));
    }

    // --- 2. С ХРАНИЛИЩЕМ: Info последняя, на уровне корня --------------------
    QTemporaryDir home;
    const QString root = home.path() + QStringLiteral("/store");
    QString error;
    ZT_TRUE("хранилище заведено: " + s(error), ZStorage(root).init(&error));

    auto storage = std::make_shared<ZStorage>(root);
    storage->reload();
    // ЗАГОЛОВКИ СЧИТАЮТСЯ ОДИН РАЗ НА ПРОЦЕСС. Первое заведение стоит разбора
    // документов (40 мс на два наших в Debug), и это цена, которую платят
    // однажды; хранилище же открывают ещё и на каждом ПЕРЕКЛЮЧЕНИИ, а
    // документы вшиты и за время работы не меняются. Проверяем не слова, а
    // время: второе заведение обязано быть на порядок дешевле первого.
    QElapsedTimer again;
    again.start();
    zametti::ZApp::instance().addInfoFolder(*storage);
    const qint64 second = again.nsecsElapsed() / 1000;
    std::printf("  второе заведение: %lld мкс (первое — %lld)\n", (long long)second,
                (long long)firstSetup);
    // Оговорка названа вслух, а не спрятана в условие: наборы идут одним
    // процессом, и если кэш заголовков кто-то согрел до нас, сравнивать нечего.
    // Молчаливый пропуск превратил бы проверку в пустышку.
    if (firstSetup < 2000)
        std::printf("  ПРОПУЩЕНО: кэш заголовков был тёплым (первое заведение %lld мкс)\n",
                    (long long)firstSetup);
    else
        ZT_TRUE("второе заведение папки дешевле первого на порядок: " + n(second) + " против " +
                    n(firstSetup),
                second * 10 < firstSetup);

    const QString work = storage->createNote(QString(), true, &error);
    const QString locked = storage->createNote(QString(), true, &error);
    ZT_TRUE("папки созданы", !work.isEmpty() && !locked.isEmpty());
    ZT_TRUE("имена папок", storage->rename(work, QStringLiteral("Работа"), rules(), &error) &&
                               storage->rename(locked, QStringLiteral("Книги"), rules(), &error));
    const QString alpha = storage->createNote(work, false, &error);
    const QString book = storage->createNote(locked, false, &error);
    ZT_TRUE("заметки созданы", !alpha.isEmpty() && !book.isEmpty());
    ZT_TRUE("имена заметок", storage->rename(alpha, QStringLiteral("Альфа"), rules(), &error) &&
                                 storage->rename(book, QStringLiteral("Книга"), rules(), &error));

    NotePanels panels(storage);
    NoteTreeModel& model = panels.model();
    ZT_TRUE("это хранилище", panels.isStore());

    const int top = model.rowCount(QModelIndex());
    ZT_EQ("верхних строк две: хранилище и Info", "2", n(top));
    const QModelIndex infoRow = model.index(top - 1, 0, QModelIndex());
    ZT_EQ("Info — ПОСЛЕДНЯЯ строка", "Info", s(model.titleOf(infoRow)));
    ZT_TRUE("Info — на уровне корня, а не внутри него",
            !model.parent(infoRow).isValid());
    ZT_TRUE("значок у неё свой",
            !model.data(infoRow, Qt::DecorationRole).isNull());

    // --- 3. Средняя колонка Info: порядок файлов, а не дат -------------------
    zametti::NoteListModel& list = panels.list();
    panels.tree().setAttribute(Qt::WA_DontShowOnScreen);
    panels.tree().resize(300, 400);
    panels.tree().show();
    panels.listView().setAttribute(Qt::WA_DontShowOnScreen);
    panels.listView().resize(300, 400);
    panels.listView().show();

    panels.tree().setCurrentIndex(infoRow);
    QCoreApplication::processEvents();
    const QStringList docs = listTitles(list);
    ZT_EQ("в списке — все вшитые документы", n(zametti::embeddedDocs().size()), n(docs.size()));
    ZT_TRUE("порядок задан списком, а не сортировкой", list.fixedOrder());

    // Порядок держится при смене переключателя сортировки: у вшитых документов
    // дат нет вовсе, и сортировка по ним расставляла бы их как попало.
    panels.setRootSort(zametti::SortOrder{zametti::SortKey::Name, false});
    panels.tree().setCurrentIndex(infoRow);
    QCoreApplication::processEvents();
    ZT_EQ("порядок не изменился от переключателя", s(docs.join(QLatin1Char('|'))),
          s(listTitles(list).join(QLatin1Char('|'))));

    // И этот порядок — порядок ФАЙЛОВ docs/info/, а не что придётся.
    ZT_EQ("файлы идут по именам", s(docFileNames().join(QLatin1Char('|'))),
          s([&] {
              QStringList names;
              for (int row = 0; row < list.rowCount(); ++row)
                  names << QFileInfo(list.pathAt(list.index(row, 0))).completeBaseName();
              return names.join(QLatin1Char('|'));
          }()));

    // Панели просят открыть документ ТОЙ ЖЕ дверью, что и заметку: сигналом с
    // путём. Развилка «читалка или редактор» стоит в окне, у самой двери, и
    // ей нужен путь ресурса — проверяем, что он оттуда и приходит.
    {
        QStringList chosen;
        QObject::connect(&panels, &NotePanels::noteChosen, &panels,
                         [&chosen](const QString& file, bool) { chosen << file; });
        panels.tree().setCurrentIndex(model.index(model.rowCount(QModelIndex()) - 1, 0,
                                                  QModelIndex()));
        QCoreApplication::processEvents();
        panels.listView().setCurrentIndex(list.index(0, 0));
        QCoreApplication::processEvents();
        ZT_TRUE("панели попросили открыть документ", !chosen.isEmpty());
        if (!chosen.isEmpty())
            ZT_TRUE("и путь у него — путь ресурса: " + s(chosen.last()),
                    chosen.last().startsWith(QLatin1Char(':')));
        QObject::disconnect(&panels, &NotePanels::noteChosen, &panels, nullptr);
    }

    // --- 4. Документация не лезет в чужой список -----------------------------
    const QModelIndex workRow = byTitle(model, QModelIndex(), QStringLiteral("Работа"));
    ZT_TRUE("папка «Работа» в дереве", workRow.isValid());
    panels.tree().setCurrentIndex(workRow);
    QCoreApplication::processEvents();
    for (const QString& title : listTitles(list))
        ZT_TRUE("документа нет в списке заметок: " + s(title), !docs.contains(title));
    {
        const std::vector<zametti::NoteRow> all = model.notesInSubtree(QModelIndex());
        for (const zametti::NoteRow& row : all)
            ZT_TRUE("документа нет и во «всех заметках»: " + s(row.title),
                    !ZStorage::isVirtualId(row.id));
    }

    // Команды окна целятся в ПАПКУ — и папка обязана ответить, что заперта:
    // на этом стоит отказ создания, импорта и метки порядка (refuseLocked в
    // main.cpp). Проверяем ровно то, что спрашивает окно.
    ZT_EQ("папка под курсором — Info", "info", s(model.folderIdFor(infoRow)));
    ZT_TRUE("и она заперта", storage->isReadOnly(model.folderIdFor(infoRow)));
    // В КАТАЛОГЕ ХРАНИЛИЩА её при этом нет: строка в дереве есть, файла и
    // записи каталога — нет.
    ZT_TRUE("в каталоге хранилища её нет", !storage->has(QStringLiteral("info")));

    // --- 5. Флаги: F2, перетаскивание, приём -------------------------------
    const Qt::ItemFlags infoFlags = model.flags(infoRow);
    ZT_TRUE("Info не переименовать", !(infoFlags & Qt::ItemIsEditable));
    ZT_TRUE("Info не утащить", !(infoFlags & Qt::ItemIsDragEnabled));
    ZT_TRUE("в Info не бросить", !(infoFlags & Qt::ItemIsDropEnabled));
    ZT_TRUE("правка Info отвергается моделью",
            !model.setData(infoRow, QStringLiteral("Другое имя"), Qt::EditRole));

    // --- 6. `access: read-only` у настоящей папки — на всё, что внутри ------
    ZT_TRUE("метка поставлена", storage->setReadOnly(locked, true, rules(), &error));
    ZT_TRUE("папка заперта", storage->isReadOnly(locked));
    ZT_TRUE("и заметка внутри — тоже (наследование вниз)", storage->isReadOnly(book));
    ZT_TRUE("соседняя папка не заперта", !storage->isReadOnly(work));
    ZT_TRUE("и её заметка тоже", !storage->isReadOnly(alpha));

    // Заслоны ядра. Каждый обязан вернуть ложь И объяснить, почему.
    error.clear();
    ZT_TRUE("запертую не переименовать",
            !storage->rename(book, QStringLiteral("Другое"), rules(), &error));
    ZT_TRUE("и сказано, почему: " + s(error), !error.isEmpty());
    ZT_TRUE("запертую не перенести", !storage->move(book, work, rules(), &error));
    ZT_TRUE("запертой не поставить метку порядка",
            !storage->setSortMark(locked, zametti::SortOrder{zametti::SortKey::Name, true},
                                  rules(), &error));
    {
        QStringList failed;
        ZT_TRUE("запертую не убрать в архив", !storage->archive(book, rules(), &failed));
        ZT_TRUE("и сказано, почему", !failed.isEmpty());
    }
    ZT_TRUE("запертую не удалить", !storage->remove(book, zametti::ImportLimits{}, &error));
    // В ЗАПЕРТОЙ ПАПКЕ НЕ ЗАВОДЯТ. Ядро не отказывает, а СНОСИТ негодного
    // родителя — так же, как для архива: заметка появится, но в корне.
    // Молчаливым это не остаётся: окно спрашивает про замок раньше и говорит
    // человеку, почему не создало (refuseLocked в main.cpp).
    {
        const QString stray = storage->createNote(locked, false, &error);
        ZT_TRUE("заметка создалась", !stray.isEmpty());
        const ZStorage::NoteInfo* meta = storage->info(stray);
        ZT_TRUE("но не в запертой папке", meta != nullptr && meta->parent().isEmpty());
        ZT_TRUE("и сама она не заперта", !storage->isReadOnly(stray));
    }

    // Замок снимается — иначе он был бы односторонним, то есть ловушкой.
    ZT_TRUE("замок снимается", storage->setReadOnly(locked, false, rules(), &error));
    ZT_TRUE("и после этого заметка правится", !storage->isReadOnly(book));
    ZT_TRUE("переименование прошло",
            storage->rename(book, QStringLiteral("Книга 2"), rules(), &error));
    ZT_TRUE("замок ставится обратно", storage->setReadOnly(locked, true, rules(), &error));

    // Модель окна знает то же самое — и по индексу, и по id.
    storage->reload();
    {
        const QModelIndex booksRow = byTitle(model, QModelIndex(), QStringLiteral("Книги"));
        ZT_TRUE("папка «Книги» в дереве", booksRow.isValid());
        ZT_TRUE("модель видит замок", model.isReadOnlyIndex(booksRow));
        ZT_TRUE("и по id тоже", model.isReadOnlyId(book));
        const Qt::ItemFlags f = model.flags(booksRow);
        ZT_TRUE("запертую папку не переименовать", !(f & Qt::ItemIsEditable));
        ZT_TRUE("и не бросить в неё", !(f & Qt::ItemIsDropEnabled));
    }

    return zt::report("папка Info и метка доступа");
}

// ЧИТАЛКА: документ показан, каретки нет, нажатия его не трогают, место чтения
// переживает уход и возврат.
static int ztRunReader() {
    const QStringList docs = zametti::embeddedDocs();
    ZT_TRUE("вшитая документация есть", !docs.isEmpty());
    for (const QString& path : docs) {
        QFile file(path);
        ZT_TRUE("документ читается: " + s(path), file.open(QIODevice::ReadOnly));
        ZT_TRUE("и он не заглушка: " + s(path), file.size() > 500);
    }

    ReaderView view(nullptr, ReaderView::Tint::Plain);
    view.setAttribute(Qt::WA_DontShowOnScreen);
    view.resize(900, 700);
    view.show();

    const QString first = docs.first();
    ZT_TRUE("документ показан", view.showFile(first, QStringLiteral("info:first")));
    ZT_TRUE("вид только для чтения", view.isReadOnly());
    ZT_TRUE("заголовок взят из самого документа", !view.title().isEmpty());
    const QString shown = view.document()->toPlainText();
    ZT_TRUE("текст непуст", shown.size() > 500);
    // Собран НАШИМ сборщиком, а не показан как есть: решёток заголовка в
    // тексте документа не остаётся.
    ZT_TRUE("разметки в тексте нет", !shown.startsWith(QLatin1Char('#')));

    // Каретки нет — её рисует только редактор. Нажатия ничего не меняют.
    const QString before = view.document()->toPlainText();
    view.document()->setModified(false);
    // Латиницей: QTest::keyClicks переводит знак в код клавиши и на
    // кириллице падает ассертом внутри Qt (qasciikey.cpp).
    QTest::keyClicks(&view, QStringLiteral("edit"));
    QTest::keyClick(&view, Qt::Key_Return);
    QTest::keyClick(&view, Qt::Key_Backspace);
    QTest::keyClick(&view, Qt::Key_Tab);
    QTest::keyClick(&view, Qt::Key_V, Qt::ControlModifier);
    QTest::keyClick(&view, Qt::Key_Z, Qt::ControlModifier);
    ZT_EQ("нажатия документ не изменили", s(before), s(view.document()->toPlainText()));
    ZT_TRUE("и признак «изменено» не поднялся", !view.document()->isModified());

    // МЕСТО ЧТЕНИЯ. Уходим со второго документа и возвращаемся — прокрутка та
    // же. Классический баг проекта: «свежее открытие рисуется, вернулся —
    // не то».
    if (docs.size() > 1) {
        view.showFile(docs.at(1), QStringLiteral("info:second"));
        view.verticalScrollBar()->setValue(view.verticalScrollBar()->maximum() / 2);
        const int spot = view.verticalScrollBar()->value();
        ZT_TRUE("документ длиннее экрана — есть чему прокручиваться", spot > 0);
        view.showFile(first, QStringLiteral("info:first"));
        view.showFile(docs.at(1), QStringLiteral("info:second"));
        ZT_EQ("место чтения вернулось", n(spot), n(view.verticalScrollBar()->value()));
        // И оно же переживает полное закрытие страницы.
        view.clear();
        view.showFile(docs.at(1), QStringLiteral("info:second"));
        ZT_EQ("и после ухода со страницы — тоже", n(spot),
              n(view.verticalScrollBar()->value()));
    }

    // ЩЕЛЧОК ПО ССЫЛКЕ НЕ ОПУСТОШАЕТ СТРАНИЦУ. Беда настоящая и найдена
    // владельцем: QTextBrowser в режиме чтения по щелчку грузит адрес В СЕБЯ,
    // не может — и остаётся пустым насовсем. Проверка переехала сюда из набора
    // «о программе» вместе с самой документацией: ссылки живут там, где текст.
    //
    // Сигнал руками тут не годится: переход заведён на СВОЙ разбор нажатия, и
    // «испущенный» anchorClicked ничего не ломает — такая проверка была бы
    // пустышкой. Нужен настоящий щелчок мышью.
    ZT_TRUE("вид не ходит по ссылкам сам", !view.openLinks());
    view.showFile(first, QStringLiteral("info:first"));
    QTextCursor anchor;
    for (QTextBlock block = view.document()->begin(); block.isValid() && anchor.isNull();
         block = block.next()) {
        for (QTextBlock::iterator it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            if (!fragment.isValid() || !fragment.charFormat().isAnchor()) continue;
            anchor = QTextCursor(view.document());
            anchor.setPosition(fragment.position() + fragment.length() / 2);
            break;
        }
    }
    ZT_TRUE("в документе есть хотя бы одна ссылка", !anchor.isNull());
    if (!anchor.isNull()) {
        view.setTextCursor(anchor);
        view.ensureCursorVisible();
        QApplication::processEvents();
        const QString whole = view.document()->toPlainText();
        const QRect box = view.cursorRect(anchor);
        QTest::mouseClick(view.viewport(), Qt::LeftButton, Qt::NoModifier, box.center());
        QApplication::processEvents();
        ZT_EQ("после щелчка по ссылке страница цела", s(whole),
              s(view.document()->toPlainText()));
    }

    // ПОИСК ВНУТРИ ДОКУМЕНТА (Ctrl+F) работает: читалка — такое же «искомое»
    // (TextSearchTarget), как редактор и вид исходника. Замена при этом
    // невозможна, и об этом говорит сама страница, а не особый случай в окне.
    view.showFile(first, QStringLiteral("info:first"));
    ZT_TRUE("в документе есть что искать", view.findMatches(zametti::makeQuery(QStringLiteral("markdown"))) > 0);
    ZT_TRUE("замена в документе невозможна", !view.canReplace());
    view.clearMatches();

    // ТАБЛИЦА В ДОКУМЕНТЕ РИСУЕТСЯ СРАЗУ, А НЕ ПОСЛЕ ЩЕЛЧКА (нашёл владелец на
    // «Storage Organization»: на месте таблицы пустое место, и она появляется,
    // только если туда ткнуть). Таблица — объект, и высоту ей даёт вёрстка;
    // первая вёрстка случается ВНУТРИ setDocument, и всё, что объекту нужно,
    // обязано быть на месте до неё.
    //
    // Спрашиваем не картинку, а геометрию: у блока таблицы высота должна быть
    // больше строки текста сразу после показа, без единого события мыши.
    for (const QString& path : docs) {
        view.showFile(path, QStringLiteral("info:probe"));
        QTextDocument* shown = view.document();
        int tables = 0;
        qreal thinnest = -1;
        for (QTextBlock block = shown->begin(); block.isValid(); block = block.next()) {
            if (!block.text().contains(QChar::ObjectReplacementCharacter)) continue;
            const QRectF box = shown->documentLayout()->blockBoundingRect(block);
            ++tables;
            if (thinnest < 0 || box.height() < thinnest) thinnest = box.height();
        }
        if (tables == 0) continue;
        std::printf("  %s: объектов %d, самый низкий %.1f\n",
                    QFileInfo(path).completeBaseName().toUtf8().constData(), tables,
                    double(thinnest));
        ZT_TRUE("объект в документе имеет высоту сразу: " + s(path), thinnest > 20.0);
    }

    // Снимок места таблицы: её видно или там пусто.
    {
        const QString dir = zt::TestData::outDir(QStringLiteral("info-table"));
        for (const QString& path : docs) {
            view.showFile(path, QStringLiteral("info:shot"));
            QTextDocument* shown = view.document();
            for (QTextBlock block = shown->begin(); block.isValid(); block = block.next()) {
                if (!block.text().contains(QChar::ObjectReplacementCharacter)) continue;
                const QRectF box = shown->documentLayout()->blockBoundingRect(block);
                view.verticalScrollBar()->setValue(int(box.top()) - 40);
                QApplication::processEvents();
                const QString shot = dir + QLatin1Char('/') +
                                     QFileInfo(path).completeBaseName() + QStringLiteral(".png");
                view.grab().toImage().save(shot);
                std::printf("  снимок таблицы: %s\n", shot.toUtf8().constData());
                break;
            }
        }
    }

    // СНИМОК — АРТЕФАКТ ПРИЁМКИ: то, как документация выглядит на самом деле.
    // Глазами смотрит человек; набор лишь кладёт файл на место и говорит, куда.
    {
        const QString dir = zt::TestData::outDir(QStringLiteral("info"));
        view.showFile(first, QStringLiteral("info:first"));
        view.verticalScrollBar()->setValue(0);
        QApplication::processEvents();
        const QString path = dir + QStringLiteral("/README.png");
        if (!view.grab().toImage().save(path))
            std::printf("  НЕ СОХРАНИЛСЯ снимок %s\n", path.toUtf8().constData());
        else
            std::printf("  снимок: %s\n", path.toUtf8().constData());
    }

    return zt::report("читалка документации");
}

// ТАБЛИЦА В ДОКУМЕНТЕ ТОЙ ЖЕ ДОРОГОЙ, ЧТО И В ОКНЕ.
//
// Владелец: «открываю Storage Organization — таблички не видно, вместо неё
// пустое место; ткнёшь — появляется». В голом виде это не воспроизводится,
// значит дело в ПОРЯДКЕ, каким показывает окно: страница стека ещё не на виду,
// когда ей отдают документ, масштаб ставится ПОСЛЕ показа, и только потом
// страницу поднимают наверх.
static int ztRunTableInStack() {
    const QStringList docs = zametti::embeddedDocs();
    if (docs.isEmpty()) return 0;

    QStackedWidget stack;
    auto* other = new QWidget;
    auto* reader = new ReaderView(nullptr, ReaderView::Tint::Plain);
    stack.addWidget(other);
    stack.addWidget(reader);
    stack.setCurrentWidget(other);
    stack.setAttribute(Qt::WA_DontShowOnScreen);
    stack.resize(1000, 760);
    stack.show();
    QApplication::processEvents();

    // Тот же порядок, что в main.cpp (showDoc). Берём ИМЕННО тот документ, на
    // котором споткнулся владелец: в README первым объектом идёт строчная
    // формула, а речь про таблицу.
    QString withTable;
    for (const QString& path : docs)
        if (path.contains(QStringLiteral("storage"))) withTable = path;
    if (withTable.isEmpty()) withTable = docs.last();
    reader->showFile(withTable, QStringLiteral("info:stack"));
    // ЛЖЁТ ЛИ ВЁРСТКА СРАЗУ ПОСЛЕ ПОДМЕНЫ ДОКУМЕНТА — спрашиваем ДО единого
    // другого обращения к ней: любое (blockBoundingRect, прокрутка, событие)
    // достраивает раскладку и ложь прячет. В живом окне ответ был «4727»
    // (блок 60, верх 4130) при видимой полосе 0..705 — и наложение обрывалось.
    {
        // Люк к защищённому помощнику: набору нужен ровно он, а наружу вид его
        // не выпускает.
        struct Probe : ReaderView {
            using ReaderView::blockAtHeight;
        };
        auto* probe = static_cast<Probe*>(reader);
        const int at = reader->document()->documentLayout()->hitTest(QPointF(0, 0), Qt::FuzzyHit);
        const QTextBlock lied = reader->document()->findBlock(at);
        std::printf("  hitTest(0,0) сразу после показа: %d → блок %d\n", at,
                    lied.isValid() ? lied.blockNumber() : -1);

        // ВОТ САМА БЕДА: вёрстка отвечает блоком глубоко внутри документа, хотя
        // спросили про его верх. Наложение (сетка таблиц, плашки кода) уходило
        // от этого блока ВНИЗ и не рисовало ничего, что видно на экране.
        const QTextBlock honest = probe->blockAtHeight(0);
        ZT_TRUE("первый видимый блок накрывает верх документа, а не лежит ниже",
                honest.isValid() && honest.blockNumber() == 0);
        // И проверка не пустышка: если бы вёрстка не врала, ловить было бы
        // нечего — говорим вслух, когда так вышло.
        if (lied.isValid() && lied.blockNumber() == 0)
            std::printf("  (вёрстка на этот раз не соврала — проверка прошла вхолостую)\n");
    }

    // МАСШТАБ СТАВИТСЯ ПОСЛЕ ПОКАЗА — так делает окно (showDoc), и у владельца
    // он не единица: в его state.json 1.21. Именно этим его случай и отличался
    // от моего первого пробника, где всё рисовалось.
    reader->applyZoom(1.21);
    stack.setCurrentWidget(reader);
    QApplication::processEvents();

    // Прокрутить к первой таблице и посмотреть, что нарисовано.
    QTextDocument* shown = reader->document();
    QRectF box;
    int number = -1;
    for (QTextBlock block = shown->begin(); block.isValid(); block = block.next()) {
        if (!block.text().contains(QChar::ObjectReplacementCharacter)) continue;
        box = shown->documentLayout()->blockBoundingRect(block);
        number = block.blockNumber();
        break;
    }
    reader->verticalScrollBar()->setValue(int(box.top()) - 30);
    QApplication::processEvents();

    const QString dir = zt::TestData::outDir(QStringLiteral("info-stack"));
    const QImage before = reader->grab().toImage();
    before.save(dir + QStringLiteral("/до-щелчка.png"));
    std::printf("  снимок до щелчка: %s/до-щелчка.png (блок %d, высота полосы %.1f)\n",
                dir.toUtf8().constData(), number, double(box.height()));

    // Щелчок по месту таблицы — то самое, что помогает владельцу.
    QTest::mouseClick(reader->viewport(), Qt::LeftButton, Qt::NoModifier,
                      QPoint(reader->viewport()->width() / 2, 60));
    QApplication::processEvents();
    const QImage after = reader->grab().toImage();
    after.save(dir + QStringLiteral("/после-щелчка.png"));
    std::printf("  снимок после щелчка: %s/после-щелчка.png\n", dir.toUtf8().constData());

    // Снимки — артефакт приёмки, их смотрит человек. СРАВНИВАТЬ ИХ МЕЖДУ СОБОЙ
    // НЕЛЬЗЯ, и это не лень: grab() сам по себе перерисовывает виджет целиком,
    // то есть «до щелчка» в наборе уже не первая отрисовка. Беду ловит
    // проверка выше — вопрос к вёрстке до единого обращения к ней.
    (void)before;
    (void)after;
    return zt::report("таблица в стеке");
}

TEST(InfoFolder, All) {
    std::vector<QByteArray> ztArgs{QByteArrayLiteral("info_folder_test")};
    std::vector<char*> ztArgv;
    for (QByteArray& a : ztArgs) ztArgv.push_back(a.data());
    EXPECT_EQ(0, ztRunSuite(int(ztArgv.size()), ztArgv.data()));
    EXPECT_EQ(0, ztRunReader());
    EXPECT_EQ(0, ztRunTableInStack());
}
