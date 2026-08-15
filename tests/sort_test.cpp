// Сортировки этапа 13: три ключа, два направления, метка в шапке папки.
//
// Набор из трёх частей, и все три про одно — что порядок в дереве получается
// из меток так же, как человек их задавал.
//
//   1. Чистые правила: круг «строка → порядок → строка», чужие значения,
//      правило нажатия кнопки, правка шапки.
//   2. Дневниковая фикстура: заголовки «1 августа»… «12 августа» с честными
//      created. По имени такое не сортируется вовсе (нет ведущих нулей), по
//      правке разваливается от первой же опечатки — по created порядок верен.
//   3. Наследование по трём уровням и инварианты брифа: смена порядка не
//      меняет файлы, метка переживает перезапуск, modified не поднимается.

#include "note_tree.h"
#include "pieces.h"
#include "sort_order.h"

#include "test_util.h"

#include <vector>
#include "testdata.h"

#include <QApplication>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <limits>
#include <optional>
#include <string>

using zametti::NoteTreeModel;
using zametti::SortKey;
using zametti::SortOrder;

namespace {

// Пометка порядка — глагол самой заметки.
void setSort(zametti::ZDocument& note, std::optional<zametti::SortOrder> order) {
    note.setSortOrder(order);
}

QString g_root;

std::string s(const QString& q) { return q.toStdString(); }

void note(const QString& id, const QString& meta, const QString& body) {
    QFile f(g_root + QLatin1Char('/') + id + QStringLiteral(".md"));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return;
    QString text = QStringLiteral("<!-- zametti\n") + meta + QStringLiteral("-->\n");
    if (!body.isEmpty()) text += QStringLiteral("\n") + body;
    f.write(text.toUtf8());
}

// Заголовки поддерева в том порядке, в каком их отдаёт средняя колонка.
//
// Папка ищется ПО ID каждый раз, а не берётся готовым индексом: смена порядка
// перестраивает модель целиком, и запомненный QModelIndex после неё указывает
// на освобождённый узел. Я на этом и попался — набор падал в отладчике на
// третьем прогоне из десяти, и падал он в моём же тесте, а не в модели.
QStringList listOrder(const NoteTreeModel& model, const QString& folderId) {
    QStringList out;
    const QModelIndex folder = model.indexForPath(model.pathOfId(folderId));
    for (const zametti::NoteRow& row : model.notesInSubtree(folder)) out << row.title;
    return out;
}

QString readFile(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QString::fromUtf8(f.readAll());
}

// Слепок всего хранилища: путь → хеш содержимого. Инвариант A брифа —
// «сортировка не меняет ни одного файла» — иначе не спросить: сравнивать надо
// ВСЕ файлы, а не тот, о котором вспомнил.
QMap<QString, QString> storeHashes() {
    QMap<QString, QString> out;
    for (const QFileInfo& info :
         QDir(g_root).entryInfoList({QStringLiteral("*.md")}, QDir::Files)) {
        QFile f(info.filePath());
        if (!f.open(QIODevice::ReadOnly)) continue;
        out.insert(info.fileName(),
                   QString::fromLatin1(
                       QCryptographicHash::hash(f.readAll(), QCryptographicHash::Sha256)
                           .toHex()));
    }
    return out;
}

// --- 1. чистые правила -------------------------------------------------------

void checkRoundTrip() {
    const SortOrder all[] = {
        {SortKey::Name, true},     {SortKey::Name, false},    {SortKey::Modified, true},
        {SortKey::Modified, false}, {SortKey::Created, true}, {SortKey::Created, false},
    };
    for (const SortOrder order : all) {
        const QString text = zametti::sortOrderToString(order);
        ZT_TRUE("порядок пишется непусто: " + s(text), !text.isEmpty());
        const auto back = zametti::parseSortOrder(text);
        ZT_TRUE("круг строка→порядок→строка: " + s(text),
                back.has_value() && *back == order);
        ZT_TRUE("название непустое: " + s(zametti::sortOrderTitle(order)),
                !zametti::sortOrderTitle(order).isEmpty());
    }

    // Умолчания брифа: имя — по возрастанию, даты — новые сверху.
    ZT_TRUE("имя по умолчанию А→Я", zametti::defaultAscending(SortKey::Name));
    ZT_TRUE("правка по умолчанию новые сверху", !zametti::defaultAscending(SortKey::Modified));
    ZT_TRUE("создание по умолчанию новые сверху", !zametti::defaultAscending(SortKey::Created));

    // Краткая запись рукой: ключ без направления берёт умолчание своего ключа.
    const auto bare = zametti::parseSortOrder(QStringLiteral("created"));
    ZT_TRUE("«created» без направления читается",
            bare.has_value() && *bare == zametti::defaultOrder(SortKey::Created));
    const auto spaced = zametti::parseSortOrder(QStringLiteral("  Name-Desc  "));
    const SortOrder nameDesc{SortKey::Name, false};
    ZT_TRUE("пробелы и регистр не мешают", spaced.has_value() && *spaced == nameDesc);

    // ЧУЖОЕ ЗНАЧЕНИЕ — не ошибка программы: хранилище правится чужими руками и
    // приезжает синхронизацией. Метка из будущей версии обязана быть просто
    // непонятой, и папка тогда наследует порядок.
    for (const char* junk : {"size-asc", "created-sideways", "-desc", "asc", "created-",
                             "созданию", "created desc"})
        ZT_TRUE(std::string("непонятное значение не разбирается: ") + junk,
                !zametti::parseSortOrder(QString::fromUtf8(junk)).has_value());
    ZT_TRUE("пустое значение — не метка", !zametti::parseSortOrder(QString()).has_value());
}

void checkPressRule() {
    // Нажали кнопку ДРУГОГО ключа — включается он в направлении по умолчанию.
    for (const SortKey pressed : {SortKey::Name, SortKey::Modified, SortKey::Created})
        for (const SortKey now : {SortKey::Name, SortKey::Modified, SortKey::Created})
            for (const bool ascending : {true, false}) {
                const SortOrder got = zametti::pressedSort(SortOrder{now, ascending}, pressed);
                if (now != pressed) {
                    ZT_TRUE("чужая кнопка включает умолчание",
                            got == zametti::defaultOrder(pressed));
                    continue;
                }
                // Нажали действующую — направление перевернулось, ключ тот же.
                ZT_TRUE("своя кнопка переворачивает направление",
                        got.key == pressed && got.ascending != ascending);
            }
}

void checkMetaMark() {
    const std::string source =
        "<!-- zametti\n"
        "created: 2020-01-01T00:00:00Z\n"
        "modified: 2021-02-03T04:05:06Z\n"
        "чужое: не трогать\n"
        "-->\n\n# Дневник\n";

    zametti::ZDocument doc = noteOf(source);
    setSort(doc, SortOrder{SortKey::Created, false});
    const std::string marked = doc.toMarkdown();
    ZT_TRUE("метка записалась", marked.find("sort: created-desc") != std::string::npos);

    const zametti::ZDocument back = noteOf(marked);
    ZT_EQ("modified от пометки не изменился", "2021-02-03T04:05:06Z", head(back, "modified"));
    ZT_EQ("created от пометки не изменился", "2020-01-01T00:00:00Z", head(back, "created"));
    ZT_EQ("чужой ключ пережил правку", "не трогать", head(back, "чужое"));
    ZT_TRUE("текст заметки на месте", marked.find("# Дневник") != std::string::npos);

    // Сброс убирает ключ и не трогает остальное.
    zametti::ZDocument reset = noteOf(marked);
    setSort(reset, std::nullopt);
    const std::string cleared = reset.toMarkdown();
    ZT_TRUE("сброс убрал ключ", cleared.find("sort:") == std::string::npos);
    ZT_EQ("сброс вернул файл к исходному", source, cleared);
}

// --- 2. дневник --------------------------------------------------------------

void buildDiary() {
    QDir(g_root).removeRecursively();
    QDir().mkpath(g_root + QStringLiteral("/.zametti"));

    // Год → месяц → двенадцать записей. Заголовки нарочно «человеческие»: без
    // ведущих нулей, со словесным месяцем — по имени они сортируются как
    // попало, и в этом весь смысл третьего ключа.
    note("00000000000d01", "created: 2026-01-01T00:00:00Z\nmodified: 2026-01-01T00:00:00Z\n",
         "# Дневник\n");
    note("00000000000d02",
         "parent: 00000000000d01\ncreated: 2026-08-01T00:00:00Z\n"
         "modified: 2026-08-01T00:00:00Z\n",
         "# 2026\n");
    note("00000000000d03",
         "parent: 00000000000d02\ncreated: 2026-08-01T00:00:00Z\n"
         "modified: 2026-08-01T00:00:00Z\n",
         "# август\n");
    for (int day = 1; day <= 12; ++day) {
        const QString id = QStringLiteral("00000000000e%1").arg(day, 2, 10, QLatin1Char('0'));
        const QString when = QStringLiteral("2026-08-%1T09:00:00Z")
                                 .arg(day, 2, 10, QLatin1Char('0'));
        note(id,
             QStringLiteral("parent: 00000000000d03\ncreated: %1\nmodified: %1\n").arg(when),
             QStringLiteral("# %1 августа 2026г\n").arg(day));
    }
    // Две записи с человеческими названиями — так дневник и живёт: не всякая
    // запись зовётся датой. По имени они уезжают в конец алфавита, по
    // созданию встают на свои дни.
    note("00000000000f01",
         "parent: 00000000000d03\ncreated: 2026-08-05T21:00:00Z\n"
         "modified: 2026-08-05T21:00:00Z\n",
         "# Итоги недели\n");
    note("00000000000f02",
         "parent: 00000000000d03\ncreated: 2026-08-09T21:00:00Z\n"
         "modified: 2026-08-09T21:00:00Z\n",
         "# Мысль на полях\n");
}

void checkDiaryOrder() {
    buildDiary();
    NoteTreeModel model(g_root);
    const QString month = QStringLiteral("00000000000d03");
    ZT_TRUE("папка месяца найдена",
            model.indexForPath(model.pathOfId(month)).isValid());

    model.setRootSort(SortOrder{SortKey::Created, true});
    QStringList expected;
    for (int day = 1; day <= 12; ++day)
        expected << QStringLiteral("%1 августа 2026г").arg(day);
    expected.insert(5, QStringLiteral("Итоги недели"));    // создана 5 августа вечером
    expected.insert(10, QStringLiteral("Мысль на полях"));  // 9 августа вечером
    ZT_EQ("по созданию, сначала старые — календарный порядок",
          s(expected.join(QLatin1Char('|'))), s(listOrder(model, month).join(QLatin1Char('|'))));

    model.setRootSort(SortOrder{SortKey::Created, false});
    QStringList reversed = expected;
    std::reverse(reversed.begin(), reversed.end());
    ZT_EQ("по созданию, новые сверху — обратный календарный",
          s(reversed.join(QLatin1Char('|'))), s(listOrder(model, month).join(QLatin1Char('|'))));

    // ПО ИМЕНИ ДНЕВНИК НЕ СОБИРАЕТСЯ. Числа в заголовках наш коллятор понимает
    // (numericMode), и «1, 2, … 12 августа» он бы упорядочил верно — но стоит
    // появиться записи с человеческим названием, и она уезжает в конец
    // алфавита, а не встаёт на свой день. Именно за этим и нужен третий ключ,
    // и проверять надо это, а не выдуманную беду с ведущими нулями.
    model.setRootSort(SortOrder{SortKey::Name, true});
    const QStringList byName = listOrder(model, month);
    ZT_TRUE("по имени хронология ломается",
            byName.join(QLatin1Char('|')) != expected.join(QLatin1Char('|')));
    ZT_TRUE("запись с человеческим названием уезжает из своего дня",
            byName.indexOf(QStringLiteral("Итоги недели")) != expected.indexOf(
                                                                  QStringLiteral("Итоги недели")));

    // ПРАВКА СТАРОЙ ЗАПИСИ. Опечатка в записи за первое августа поднимает
    // modified до сегодняшнего — в порядке по правке она улетает наверх, а в
    // порядке по созданию не двигается ни на строку. Ради этого всё и затевалось.
    note("00000000000e01",
         QStringLiteral("parent: 00000000000d03\ncreated: 2026-08-01T09:00:00Z\n"
                        "modified: 2026-12-31T23:59:00Z\n"),
         QStringLiteral("# 1 августа 2026г\n"));
    NoteTreeModel edited(g_root);

    edited.setRootSort(SortOrder{SortKey::Modified, false});
    ZT_EQ("по правке поправленная запись всплыла наверх", "1 августа 2026г",
          s(listOrder(edited, month).value(0)));

    edited.setRootSort(SortOrder{SortKey::Created, false});
    ZT_EQ("по созданию поправленная запись осталась на своём месте", "12 августа 2026г",
          s(listOrder(edited, month).value(0)));
    ZT_EQ("и внизу по-прежнему первое августа", "1 августа 2026г",
          s(listOrder(edited, month).last()));
}

// --- 3. наследование и инварианты --------------------------------------------

void checkInheritance() {
    buildDiary();
    // Метку ставим руками в файл — ровно так, как её пишет программа.
    const QString monthFile = g_root + QStringLiteral("/00000000000d03.md");
    {
        zametti::ZDocument doc = noteOf(readFile(monthFile).toStdString());
        setSort(doc, SortOrder{SortKey::Created, true});
        QFile f(monthFile);
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            const std::string out = doc.toMarkdown();
            f.write(out.data(), qint64(out.size()));
        }
    }

    NoteTreeModel model(g_root);
    const SortOrder root = zametti::defaultOrder(SortKey::Modified);
    const QString diary = QStringLiteral("00000000000d01");
    const QString year = QStringLiteral("00000000000d02");
    const QString month = QStringLiteral("00000000000d03");

    const SortOrder createdAsc{SortKey::Created, true};
    const std::optional<SortOrder> monthMark = model.explicitSortOf(month);
    ZT_TRUE("своя метка читается из шапки",
            monthMark.has_value() && *monthMark == createdAsc);
    ZT_TRUE("у предков метки нет", !model.explicitSortOf(year).has_value() &&
                                       !model.explicitSortOf(diary).has_value());

    bool fromMark = false;
    ZT_TRUE("помеченная папка идёт по своей метке",
            model.effectiveSortFor(month, root, &fromMark) == createdAsc);
    ZT_TRUE("и это видно как «задано меткой»", fromMark);
    ZT_TRUE("папка без метки идёт по переключателю",
            model.effectiveSortFor(year, root, &fromMark) == root);
    ZT_TRUE("и это видно как «общий порядок»", !fromMark);
    ZT_TRUE("корень идёт по переключателю",
            model.effectiveSortFor(QString(), root, &fromMark) == root);

    // ПОМЕТИЛИ ПРЕДКА — наследует и потомок. Заодно спрашиваем, что файлы
    // ДЕТЕЙ при этом не тронуты: метка живёт в одном файле, а не размножается.
    const QMap<QString, QString> before = storeHashes();
    const QString yearFile = g_root + QStringLiteral("/00000000000d02.md");
    {
        zametti::ZDocument doc = noteOf(readFile(yearFile).toStdString());
        setSort(doc, SortOrder{SortKey::Name, false});   // NOLINT
        QFile f(yearFile);
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            const std::string out = doc.toMarkdown();
            f.write(out.data(), qint64(out.size()));
        }
    }
    const QMap<QString, QString> after = storeHashes();
    int changed = 0;
    for (auto it = after.constBegin(); it != after.constEnd(); ++it)
        if (before.value(it.key()) != it.value()) ++changed;
    ZT_TRUE("пометка предка изменила ровно один файл (" + std::to_string(changed) + ")",
            changed == 1);

    NoteTreeModel marked(g_root);
    ZT_TRUE("месяц по-прежнему идёт по СВОЕЙ метке",
            marked.effectiveSortFor(month, root) == createdAsc);
    ZT_TRUE("дневник — по переключателю: помечен год, а не он",
            marked.effectiveSortFor(diary, root) == root);

    // Сброс метки месяца — и он наследует от года.
    {
        zametti::ZDocument doc = noteOf(readFile(monthFile).toStdString());
        setSort(doc, std::nullopt);
        QFile f(monthFile);
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            const std::string out = doc.toMarkdown();
            f.write(out.data(), qint64(out.size()));
        }
    }
    NoteTreeModel reset(g_root);
    const SortOrder yearMark{SortKey::Name, false};
    ZT_TRUE("после сброса месяц наследует метку года",
            reset.effectiveSortFor(month, root) == yearMark);
    ZT_TRUE("своей метки у месяца больше нет", !reset.explicitSortOf(month).has_value());

    // МЕТКА НЕ ПОДНИМАЕТ modified. Пометка — правка организационная, как
    // перенос: всплывать наверх списка папка от неё не должна (правило этапа 7).
    ZT_TRUE("modified помеченного года не изменился",
            readFile(yearFile).contains(QStringLiteral("modified: 2026-08-01T00:00:00Z")));
    ZT_TRUE("modified сброшенного месяца не изменился",
            readFile(monthFile).contains(QStringLiteral("modified: 2026-08-01T00:00:00Z")));
}

// Инвариант A брифа: смена порядка в корне не меняет ни одного файла.
void checkSortingWritesNothing() {
    buildDiary();
    NoteTreeModel model(g_root);
    const QMap<QString, QString> before = storeHashes();
    for (const SortKey key : {SortKey::Name, SortKey::Modified, SortKey::Created})
        for (const bool ascending : {true, false}) {
            model.setRootSort(SortOrder{key, ascending});
            (void)model.notesInSubtree(QModelIndex());
        }
    model.refresh();
    ZT_TRUE("шесть переключений сортировки не тронули ни одного файла",
            storeHashes() == before);
}

// Непонятная метка: программа не падает, жалуется и показывает по наследству.
void checkJunkMark() {
    buildDiary();
    note("00000000000d03",
         "parent: 00000000000d02\ncreated: 2026-08-01T00:00:00Z\n"
         "modified: 2026-08-01T00:00:00Z\nsort: по-настроению\n",
         "# август\n");
    NoteTreeModel model(g_root);
    const SortOrder root = zametti::defaultOrder(SortKey::Created);
    ZT_TRUE("непонятная метка не читается как метка",
            !model.explicitSortOf(QStringLiteral("00000000000d03")).has_value());
    bool fromMark = true;
    ZT_TRUE("папка с непонятной меткой наследует",
            model.effectiveSortFor(QStringLiteral("00000000000d03"), root, &fromMark) == root);
    ZT_TRUE("и это не считается меткой", !fromMark);
    ZT_TRUE("заметки папки на месте",
            listOrder(model, QStringLiteral("00000000000d03")).size() == 14);
}

// --- замер --------------------------------------------------------------
//
// Цена сортировки на большом хранилище. Считается не «сколько миллисекунд
// вообще», а ДВЕ разницы: сколько стоит скан с чтением новых ключей и сколько
// — пересортировка по каждому из трёх ключей. Рядом печатается эталон —
// постоянный счётный цикл: пока он стоит намертво, разброс в замерах говорит
// о коде, а поехал он — поехала машина, и числам верить нельзя.
void bench(int notes) {
    QDir(g_root).removeRecursively();
    QDir().mkpath(g_root + QStringLiteral("/.zametti"));
    note("00000000000d01", "created: 2020-01-01T00:00:00Z\nmodified: 2020-01-01T00:00:00Z\n",
         "# Корень\n");
    for (int i = 0; i < notes; ++i) {
        const QString id = QStringLiteral("0000000001%1").arg(i, 4, 36, QLatin1Char('0'));
        const QString when = QStringLiteral("20%1-%2-%3T09:00:00Z")
                                 .arg(10 + i % 20, 2, 10, QLatin1Char('0'))
                                 .arg(1 + i % 12, 2, 10, QLatin1Char('0'))
                                 .arg(1 + i % 28, 2, 10, QLatin1Char('0'));
        note(id,
             QStringLiteral("parent: 00000000000d01\ncreated: %1\nmodified: %1\n").arg(when),
             QStringLiteral("# Заметка %1\n\nТекст для сниппета, чтобы разбор был честным.\n")
                 .arg(i));
    }

    const auto micros = [](auto&& work) {
        qint64 best = std::numeric_limits<qint64>::max();
        for (int run = 0; run < 5; ++run) {
            QElapsedTimer timer;
            timer.start();
            work();
            best = qMin(best, timer.nsecsElapsed() / 1000);
        }
        return best;
    };

    volatile double sink = 0;
    const qint64 yard = micros([&sink] {
        double acc = 0;
        for (int i = 1; i < 3000000; ++i) acc += 1.0 / double(i);
        sink = acc;
    });

    const qint64 scan = micros([&] { NoteTreeModel probe(g_root); });
    NoteTreeModel model(g_root);
    std::printf("замер сортировки: %d заметок\n", notes);
    std::printf("  эталон (счётный цикл)          %6lld мкс\n", (long long)yard);
    std::printf("  скан хранилища с разбором      %6lld мкс\n", (long long)scan);
    for (const SortKey key : {SortKey::Name, SortKey::Modified, SortKey::Created})
        for (const bool ascending : {false, true}) {
            const SortOrder order{key, ascending};
            const qint64 sorted = micros([&] {
                model.setRootSort(order);
                // Смена порядка на ТОТ ЖЕ ничего не делает (ранний возврат),
                // поэтому между замерами порядок сбрасывается.
                model.setRootSort(SortOrder{SortKey::Name, !ascending});
                model.setRootSort(order);
            });
            std::printf("  порядок %-14s         %6lld мкс (две пересборки)\n",
                        zametti::sortOrderToString(order).toUtf8().constData(),
                        (long long)sorted);
        }
    QDir(g_root).removeRecursively();
}

// СЛУЖЕБНЫЕ ПАПКИ ВНИЗУ И В СВОЁМ ПОРЯДКЕ (просьба владельца): всё живое,
// под ним бюро находок, в самом низу Архив. Проверяется при обоих направлениях
// и по всем трём ключам: служебные не участвуют в сортировке вовсе, и
// переворот направления не должен поднимать их наверх.
void checkSpecialFoldersStayAtBottom() {
    QDir(g_root).removeRecursively();
    QDir().mkpath(g_root + QStringLiteral("/.zametti"));
    note("00000000000a01", "role: folder\ncreated: 2026-01-01T00:00:00+03:00\n"
                           "modified: 2026-01-01T00:00:00+03:00\n", "# Ада\n");
    note("00000000000a02", "role: folder\ncreated: 2026-06-01T00:00:00+03:00\n"
                           "modified: 2026-06-01T00:00:00+03:00\n", "# Яна\n");
    // Бюро заведено раньше всех и правлено позже всех — по любому ключу оно
    // просилось бы то вверх, то вниз.
    note("00000000000a03", "role: lost\ncreated: 2020-01-01T00:00:00+03:00\n"
                           "modified: 2026-12-31T00:00:00+03:00\n", "# Бюро находок\n");
    note("00000000000a04", "parent: 00000000000a03\ncreated: 2026-02-02T00:00:00+03:00\n"
                           "modified: 2026-02-02T00:00:00+03:00\n", "# Найдёныш\n");
    note("00000000000a05", "archived: yes\ncreated: 2026-03-03T00:00:00+03:00\n"
                           "modified: 2026-03-03T00:00:00+03:00\n", "# Убранная\n");

    NoteTreeModel model(g_root);
    for (const SortKey key : {SortKey::Name, SortKey::Modified, SortKey::Created})
        for (const bool ascending : {true, false}) {
            model.setRootSort(SortOrder{key, ascending});
            const QModelIndex all = model.index(0, 0, QModelIndex());
            const int rows = model.rowCount(all);
            QStringList titles;
            for (int row = 0; row < rows; ++row)
                titles << model.data(model.index(row, 0, all), Qt::DisplayRole).toString();
            ZT_EQ("служебные внизу и в своём порядке (" +
                      s(zametti::sortOrderToString(SortOrder{key, ascending})) + ")",
                  std::string("Бюро находок|Архив"),
                  s(titles.mid(rows - 2).join(QLatin1Char('|'))));
        }
    QDir(g_root).removeRecursively();
}

// ПОРЯДОК ПРИНАДЛЕЖИТ ПАПКЕ, А НЕ ВЫБРАННОЙ СТРОКЕ.
//
// Проверка родилась из беды, которую нашёл владелец: он щёлкал по папке,
// помеченной «по дате создания», и ВСЯ левая панель перекладывалась — строка
// под курсором оказывалась чужой. Причина была в устройстве: порядок считался
// свойством точки обзора, один на всё дерево.
//
// Теперь у каждой папки свой: её метка → метка предка → переключатель корня. В
// одной и той же сборке дерева помеченная папка идёт по-своему, соседняя — по
// корневому порядку, а смена переключателя помеченную не трогает вовсе.
void checkEachFolderSortsItsOwnChildren() {
    QDir(g_root).removeRecursively();
    QDir().mkpath(g_root + QStringLiteral("/.zametti"));

    // Имена и даты нарочно ПРОТИВОПОЛОЖНЫ: по имени «Ася, Боря, Витя», по
    // правке — наоборот. Так порядок виден по первому же заголовку.
    const auto family = [&](const QString& prefix, const QString& parent) {
        int day = 1;
        for (const QString& name :
             {QStringLiteral("Ася"), QStringLiteral("Боря"), QStringLiteral("Витя")}) {
            const QString when = QStringLiteral("2026-0%1-01T10:00:00+03:00").arg(day);
            note(prefix + QStringLiteral("%1").arg(day),
                 QStringLiteral("parent: %1\ncreated: %2\nmodified: %2\n").arg(parent, when),
                 QStringLiteral("# %1\n").arg(name));
            ++day;
        }
    };

    note("00000000000m01", "role: folder\nsort: name-asc\n"
                           "created: 2026-01-01T00:00:00+03:00\n"
                           "modified: 2026-01-01T00:00:00+03:00\n", "# Помеченная\n");
    family(QStringLiteral("00000000000m1"), QStringLiteral("00000000000m01"));
    note("00000000000m02", "role: folder\ncreated: 2026-01-01T00:00:00+03:00\n"
                           "modified: 2026-01-01T00:00:00+03:00\n", "# Обычная\n");
    family(QStringLiteral("00000000000m2"), QStringLiteral("00000000000m02"));
    // Подпапка ВНУТРИ помеченной, без своей метки: наследует не от корня, а от
    // неё.
    note("00000000000m03", "role: folder\nparent: 00000000000m01\n"
                           "created: 2026-01-01T00:00:00+03:00\n"
                           "modified: 2026-01-01T00:00:00+03:00\n", "# Внутри помеченной\n");
    family(QStringLiteral("00000000000m3"), QStringLiteral("00000000000m03"));

    NoteTreeModel model(g_root);
    model.setRootSort(SortOrder{SortKey::Modified, false});   // корень: свежие сверху

    const auto children = [&model](const QString& folderId) {
        QStringList out;
        const QModelIndex folder = model.indexForPath(model.pathOfId(folderId));
        for (int row = 0; row < model.rowCount(folder); ++row)
            out << model.data(model.index(row, 0, folder), Qt::DisplayRole).toString();
        return out.join(QLatin1Char('|'));
    };

    ZT_EQ("помеченная папка идёт по СВОЕЙ метке", std::string("Внутри помеченной|Ася|Боря|Витя"),
          s(children(QStringLiteral("00000000000m01"))));
    ZT_EQ("соседняя без метки — по корневому порядку", std::string("Витя|Боря|Ася"),
          s(children(QStringLiteral("00000000000m02"))));
    ZT_EQ("подпапка наследует от помеченной, а не от корня",
          std::string("Ася|Боря|Витя"), s(children(QStringLiteral("00000000000m03"))));

    // СМЕНА ПЕРЕКЛЮЧАТЕЛЯ КОРНЯ помеченную папку не трогает.
    model.setRootSort(SortOrder{SortKey::Created, true});
    ZT_EQ("после смены корневого порядка помеченная стоит как стояла",
          std::string("Внутри помеченной|Ася|Боря|Витя"),
          s(children(QStringLiteral("00000000000m01"))));
    ZT_EQ("а соседняя без метки перевернулась вместе с корнем",
          std::string("Ася|Боря|Витя"), s(children(QStringLiteral("00000000000m02"))));

    QDir(g_root).removeRecursively();
}

}  // namespace

static int ztRunSuite(int argc, char** argv) {
    g_root = QDir::tempPath() + QStringLiteral("/zametti-sort-test");

    // Замер по просьбе — в набор он не входит: две тысячи файлов на каждом
    // прогоне ctest никому не нужны.
    if (argc > 1 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("--bench")) {
        bench(argc > 2 ? QString::fromLocal8Bit(argv[2]).toInt() : 2000);
        return 0;
    }

    checkRoundTrip();
    checkPressRule();
    checkMetaMark();
    checkDiaryOrder();
    checkInheritance();
    checkSortingWritesNothing();
    checkJunkMark();
    checkSpecialFoldersStayAtBottom();
    checkEachFolderSortsItsOwnChildren();

    QDir(g_root).removeRecursively();
    return zt::report("сортировки");
}

// Набор целиком одним TEST: тело не тронуто, argv ему собран здесь.
// Дробить на отдельные проверки — отдельная работа, по одному набору.
TEST(Sort, All) {
    std::vector<QByteArray> ztArgs{QByteArrayLiteral("sort_test")};
    std::vector<char*> ztArgv;
    for (QByteArray& a : ztArgs) ztArgv.push_back(a.data());
    EXPECT_EQ(0, ztRunSuite(int(ztArgv.size()), ztArgv.data()));
}
