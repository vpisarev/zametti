// Архивация ОТКРЫТОЙ заметки: то, что делает человек, а не только хранилище.
//
// Набор появился по следу владельца: «попытка архивировать Typesetting Math
// каждый раз добавляет огромное количество символов \». Хранилищный набор
// (archive_test) при этом был зелёным, потому что спрашивал хранилище, а беда
// жила на стыке: заметку перед архивацией сохраняет РЕДАКТОР, а его круг
// «документ → IR» терял признак формулы, и сериализатор экранировал косые
// заново. Каждый заход удваивал их.
//
// Поэтому здесь проверяется весь путь целиком, ровно так, как он идёт в окне:
// открыли → сохранили → в архив → перечитали → сохранили ещё раз →
// вернули из архива. И так три раза подряд: беда была не в первом заходе, а в
// накоплении.

#include "zstorage.h"
#include "reader_view.h"
#include "jxl_encoder.h"
#include "editor_widget.h"
#include "journal.h"

#include "test_util.h"

#include <vector>
#include "testdata.h"

#include "scratch_files.h"
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QTest>

#include <string>

namespace {

QString g_root;

QString notePath(const QString& id) { return g_root + QLatin1Char('/') + id + QStringLiteral(".md"); }

QString readFile(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QString::fromUtf8(f.readAll());
}

void writeFile(const QString& path, const QString& text) {
    QFile f(path);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) f.write(text.toUtf8());
}

zametti::ZJournal::Rules rules() {
    zametti::ZJournal::Rules r;
    r.mergeChars = 100;
    r.mergeHours = 24;
    return r;
}

// В архив и обратно — теми же глаголами хранилища, что и окно. Хранилище
// читает каталог заново: файлы здесь пишутся руками.
bool archiveNote(const QString& id, QString* why) {
    zametti::ZStorage storage(g_root);
    storage.reload();
    QStringList failed;
    const bool ok = storage.archive(id, rules(), &failed);
    if (why != nullptr) *why = failed.join(QLatin1Char('\n'));
    return ok;
}

bool restoreNote(const QString& id, QString* why) {
    zametti::ZStorage storage(g_root);
    storage.reload();
    QStringList failed;
    const bool ok = storage.restore(id, &failed);
    if (why != nullptr) *why = failed.join(QLatin1Char('\n'));
    return ok;
}

const char* kNote =
    "<!-- zametti\n"
    "id: 01f0rmxa0test0\n"
    "created: 2026-01-01T00:00:00+03:00\n"
    "modified: 2026-01-02T00:00:00+03:00\n"
    "-->\n"
    "\n"
    "# Formulas\n"
    "\n"
    "Greek letters are typed using commands such as $\\gamma$ and $\\Gamma$.\n"
    "\n"
    "The integral $\\int_0^1 x^2 \\, dx$ has a thin space before $dx$.\n"
    "\n"
    "$$\\sum_{k=0}^\\infty \\frac{x^k}{k!} \\not= \\prod_{j=1}^{10} \\frac{j}{j+1}$$\n"
    "\n"
    "Roots: $\\sqrt{3}$, $\\sqrt[3]{12}$, $\\sqrt{1+\\sqrt{2}}$.\n";

}  // namespace

static int ztRunSuite(int argc, char** argv) {
    g_root = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QDir::tempPath();
    zt::dropTree(g_root);
    QDir().mkpath(g_root + QStringLiteral("/.zametti"));
    QDir().mkpath(g_root + QStringLiteral("/history"));

    const QString id = QStringLiteral("01f0rmxa0test0");
    const QString path = notePath(id);
    writeFile(path, QString::fromUtf8(kNote));

    zametti::NoteEditor editor;
    editor.resize(900, 700);
    editor.show();
    QTest::qWait(20);

    // Открытие КАНОНИЗИРУЕТ заметку и пишет её обратно, если она не канонична.
    // Именно поэтому беда была видна без единой правки: человек просто открывал
    // заметку, а файл менялся. Канон снимаем один раз и дальше сравниваем с ним.
    editor.openFile(path);
    QTest::qWait(60);
    const QString canon = readFile(path);
    ZT_TRUE("канон непуст", !canon.isEmpty());
    ZT_TRUE("формулы в каноне целы: \\gamma не удвоился",
            canon.contains(QStringLiteral("$\\gamma$")) &&
                !canon.contains(QStringLiteral("$\\\\gamma$")));
    ZT_TRUE("тонкий пробел на месте", canon.contains(QStringLiteral("x^2 \\, dx")));
    ZT_TRUE("индекс не экранирован", canon.contains(QStringLiteral("\\sum_{k=0}")));

    for (int round = 1; round <= 3; ++round) {
        const std::string tag = " (заход " + std::to_string(round) + ")";

        // 1. Открыли и сохранили — так делает окно перед архивацией.
        editor.openFile(path);
        QTest::qWait(40);
        editor.save(false);
        QTest::qWait(20);
        ZT_TRUE("открытие и запись файл не меняют" + tag, readFile(path) == canon);

        // 2. В архив.
        QString why;
        ZT_TRUE("архивация прошла" + tag, archiveNote(id, &why));
        const QString marked = readFile(path);
        ZT_TRUE("файл помечен архивным, а тело осталось" + tag,
                marked.contains(QStringLiteral("archived: yes")) &&
                    marked.contains(QStringLiteral("\\gamma")));

        // 3. Перечитали и сохранили — окно делает ровно это, а закрытие
        //    заметки зовёт запись принудительно.
        editor.openFile(path);
        QTest::qWait(40);
        editor.save(false, true);
        QTest::qWait(20);
        const QString afterSave = readFile(path);
        ZT_TRUE("пометка архива пережила запись" + tag,
                afterSave.contains(QStringLiteral("archived: yes")));
        ZT_TRUE("тело не потерялось от перезаписи" + tag,
                afterSave.contains(QStringLiteral("\\gamma")));

        // 4. Вернули из архива — тело обязано вернуться БАЙТ В БАЙТ.
        ZT_TRUE("возврат прошёл" + tag, restoreNote(id, &why));
        const QString back = readFile(path);
        ZT_TRUE("тело вернулось побайтово" + tag, back == canon);
        if (back != canon) {
            // Место расхождения — в вывод: на заметке в килобайт «не совпало»
            // ничего не говорит.
            int i = 0;
            while (i < back.size() && i < canon.size() && back.at(i) == canon.at(i)) ++i;
            std::printf("  разошлось на %d: ждали [%s] вышло [%s]\n", i,
                        qPrintable(canon.mid(i, 40)), qPrintable(back.mid(i, 40)));
        }
    }

    // БИТУЮ ЗАМЕТКУ ТОЖЕ УБИРАЕМ. Правило владельца: пусть файл испорчен чем
    // угодно — правкой в чужом редакторе, сбоем, нашей же ошибкой, — раз шапка
    // на месте, архивация обязана пройти, и разбирать тело для этого не надо.
    {
        const QString brokenId = QStringLiteral("01br0ken00test");
        const QString brokenPath = notePath(brokenId);
        QString body = QStringLiteral(
            "<!-- zametti\nid: 01br0ken00test\ncreated: 2026-01-01T00:00:00+03:00\n"
            "modified: 2026-01-02T00:00:00+03:00\n-->\n\n# Битая\n\n");
        // Всё, чем markdown можно сломать: незакрытый забор, оборванная
        // таблица, гора косых, нулевой байт в тексте.
        body += QStringLiteral("```\nне закрыт забор\n\n| a | b\n|---\n");
        body += QString(2000, QLatin1Char('\\'));
        body += QStringLiteral("\n$$\\frac{a}{b\n\n<!-- не закрытый комментарий\n");
        writeFile(brokenPath, body);

        QString why;
        ZT_TRUE("битая заметка убирается в архив", archiveNote(brokenId, &why));
        const QString marked = readFile(brokenPath);
        ZT_TRUE("она помечена архивной", marked.contains(QStringLiteral("archived: yes")));
        ZT_TRUE("и содержит заголовок", marked.contains(QStringLiteral("# Битая")));
        // ТЕЛО ЦЕЛО ПОБАЙТОВО, И ЦЕЛО В ФАЙЛЕ: битую заметку архивация обязана
        // убрать, ни во что её не разбирая и ничего в ней не поправляя.
        ZT_TRUE("и тело битой заметки на месте, как было",
                marked.contains(QStringLiteral("не закрыт забор")));

        // ГОЛОВА ЖУРНАЛА РАВНА ФАЙЛУ — и у битой тоже.
        zametti::ZStorage history(g_root);
        zametti::ZJournal read;
        QByteArray head;
        if (history.readJournal(brokenId, &read, &why) && !read.isEmpty())
            history.journalSnapshot(brokenId, read.lastSnapshotIndex(), &head, &why);
        ZT_TRUE("голова журнала — помеченный файл байт в байт",
                QString::fromUtf8(head) == marked);

        // И ВОЗВРАТ ОТДАЁТ ТЕ ЖЕ БАЙТЫ.
        ZT_TRUE("возврат битой прошёл", restoreNote(brokenId, &why));
        ZT_TRUE("битая вернулась байт в байт", readFile(brokenPath) == body);
    }


    // --- АРХИВНУЮ ПОКАЗЫВАЕТ ВИД, А НЕ РЕДАКТОР ------------------------------
    //
    // Тело её лежит в файле, как у живой, и документ у неё обычный — собранный
    // тем же кодом. Разница в том, кому он отдан: виду, где каретки нет вовсе,
    // а не редактору, который обещал бы правку.
    {
        const QString id = QStringLiteral("01n7arcv1ew000");
        const QString path = g_root + QLatin1Char('/') + id + QStringLiteral(".md");
        writeFile(path, QStringLiteral("<!-- zametti\nversion: 1\n-->\n\n# Убранная\n\n"
                                       "Тело её никуда не делось.\n"));
        QString why;
        ZT_TRUE("архивация прошла", archiveNote(id, &why));

        editor.openFile(path);
        QTest::qWait(40);
        ZT_TRUE("редактор знает, что заметка архивная", editor.isArchivedNote());

        zametti::ReaderView view;
        ZT_TRUE("вид показал файл", view.showFile(path));
        ZT_TRUE("и в нём тело заметки",
                view.toPlainText().contains(QStringLiteral("Тело её никуда не делось")));
        ZT_TRUE("вид — только для чтения", view.isReadOnly());
        // ПОЛЕ ТОНИРОВАНО КАК ПРОШЛОЕ: тем же серым, что и режим истории.
        ZT_EQ("фон — серый истории",
              zametti::settings().style().historyBackground().name().toStdString(),
              view.palette().color(QPalette::Base).name().toStdString());

        // ЖИВАЯ ЗАМЕТКА ВИДОМ НЕ ПОКАЗЫВАЕТСЯ: признак обязан различать их.
        const QString alive = g_root + QStringLiteral("/01n7arclive00.md");
        writeFile(alive, QStringLiteral("<!-- zametti\nversion: 1\n-->\n\n# Живая\n"));
        editor.openFile(alive);
        QTest::qWait(40);
        ZT_TRUE("живая архивной не считается", !editor.isArchivedNote());

        // ВЕРНУЛИСЬ К АРХИВНОЙ — признак тот же: производное состояние
        // восстанавливают ОБА пути открытия, и возврат из кэша тоже.
        editor.openFile(path);
        QTest::qWait(40);
        ZT_TRUE("после возврата признак прежний", editor.isArchivedNote());

        // ЗАМЕТКА ИЗ ОДНИХ СНИМКОВ — вот на чём вид и попался, и вот чем это
        // приколото. Ссылка в тексте ОТНОСИТЕЛЬНАЯ, и виджет обязан знать, от
        // какого каталога её считать. Редактору это говорят при открытии; вид
        // архива той же функции не звал — база была пуста, картинки не
        // рисовались, и заметка выглядела как один заголовок.
        //
        // Проверка идёт ПО ПИКСЕЛЯМ, а не по путям: спрашивать «правильно ли
        // выставлена база» значит проверять свою же реализацию, а спрашивать
        // «нарисовалась ли картинка» — то, что видит человек.
        {
            const QString shotId = QStringLiteral("01n7arcsh0t000");
            const QString shotPath = g_root + QLatin1Char('/') + shotId + QStringLiteral(".md");
            // Настоящий файл вложения рядом с заметкой: ярко-красный квадрат,
            // которого в тексте нет и быть не может.
            QImage picture(160, 120, QImage::Format_RGB32);
            picture.fill(qRgb(220, 30, 40));
            zametti::EncodeOptions options;
            options.quality = 90;
            options.effort = 3;
            QString encodeError;
            const QByteArray jxl = zametti::encodeJxl(picture, options, {}, &encodeError);
            ZT_TRUE("вложение закодировано", !jxl.isEmpty());
            {
                QFile f(g_root + QStringLiteral("/01jd7f0kq2m8xa.jxl"));
                ZT_TRUE("вложение записано", f.open(QIODevice::WriteOnly));
                f.write(jxl);
            }
            writeFile(shotPath,
                      QStringLiteral("<!-- zametti\nversion: 1\n-->\n\n# Со снимком\n\n"
                                     "![вид](01jd7f0kq2m8xa.jxl)\n"));
            ZT_TRUE("архивация прошла", archiveNote(shotId, &why));

            zametti::ReaderView shotView;
            shotView.resize(700, 500);
            shotView.show();
            ZT_TRUE("вид показал файл", shotView.showFile(shotPath));
            QTest::qWait(300);   // картинки читаются в другом потоке
            const QImage rendered = shotView.grab().toImage();
            int red = 0;
            for (int y = 0; y < rendered.height(); ++y)
                for (int x = 0; x < rendered.width(); ++x) {
                    const QColor c = rendered.pixelColor(x, y);
                    if (c.red() > 150 && c.green() < 100 && c.blue() < 100) ++red;
                }
            ZT_TRUE("снимок заметки НАРИСОВАН: красных точек " + std::to_string(red),
                    red > 1000);
        }

        // ЗАМЕТКА ИЗ ОДНИХ СНИМКОВ — ДВЕ ОШИБКИ РАЗОМ, и приколоты они порознь.
        //
        // Первая: вид не звал общую функцию «что вид обязан знать о заметке», и
        // база относительных ссылок оставалась пустой. Вторая, опаснее:
        // неразрешённый путь рисовался НИЧЕМ — вся отрисовка молча пропускала
        // объект, поэтому первую ошибку не было видно вовсе. Правило проекта
        // «нет файла — рамка с именем» обязано работать и здесь.
        {
            const QString shotId = QStringLiteral("01n7arcsh0t000");
            const QString shotPath = g_root + QLatin1Char('/') + shotId + QStringLiteral(".md");
            // Настоящий файл вложения рядом с заметкой: ярко-красный квадрат,
            // которого в тексте нет и быть не может.
            QImage picture(200, 150, QImage::Format_RGB32);
            picture.fill(qRgb(220, 30, 40));
            zametti::EncodeOptions encodeOptions;
            encodeOptions.quality = 90;
            encodeOptions.effort = 3;
            QString encodeError;
            const QByteArray jxl = zametti::encodeJxl(picture, encodeOptions, {}, &encodeError);
            ZT_TRUE("вложение закодировано", !jxl.isEmpty());
            {
                QFile f(g_root + QStringLiteral("/01jd7f0kq2m8xa.jxl"));
                ZT_TRUE("вложение записано", f.open(QIODevice::WriteOnly));
                f.write(jxl);
            }
            writeFile(shotPath,
                      QStringLiteral("<!-- zametti\nversion: 1\n-->\n\n# Со снимком\n\n"
                                     "![вид](01jd7f0kq2m8xa.jxl)\n"));
            ZT_TRUE("архивация прошла", archiveNote(shotId, &why));

            const auto redPixels = [](const QImage& image) {
                int count = 0;
                for (int y = 0; y < image.height(); ++y)
                    for (int x = 0; x < image.width(); ++x) {
                        const QColor c = image.pixelColor(x, y);
                        if (c.red() > 150 && c.green() < 110 && c.blue() < 110) ++count;
                    }
                return count;
            };

            zametti::ReaderView shotView;
            shotView.resize(700, 500);
            shotView.show();
            ZT_TRUE("вид показал файл", shotView.showFile(shotPath));
            QTest::qWait(400);   // картинки читаются в другом потоке
            ZT_TRUE("СНИМОК ЗАМЕТКИ НАРИСОВАН", redPixels(shotView.grab().toImage()) > 2000);

            // ТРЕТЬЯ ОШИБКА, найденная владельцем: щелчок по картинке опустошал
            // заметку. NoteView — это QTextBrowser, а браузер в режиме
            // только-для-чтения активирует ссылки САМ: адрес картинки в
            // документе и есть ссылка, и «переход» по ней подменял документ
            // пустым. В редакторе не видно — в редактируемом виджете Qt ссылок
            // не активирует вовсе.
            {
                const QString before = shotView.toPlainText();
                ZT_TRUE("до щелчка заметка не пуста", !before.trimmed().isEmpty());
                QTest::mouseClick(shotView.viewport(), Qt::LeftButton, {},
                                  QPoint(shotView.viewport()->width() / 2,
                                         shotView.viewport()->height() / 2));
                QTest::qWait(200);
                ZT_EQ("щелчок по картинке заметку не опустошает", before.toStdString(),
                      shotView.toPlainText().toStdString());
            }

            // ЩЕЛЧОК ПО КАРТИНКЕ РИСУЕТ ТО ЖЕ, ЧТО И В ЖИВОЙ ЗАМЕТКЕ. Жалоба
            // владельца: в архивной вокруг выбранного снимка появляется вторая
            // рамка, которой в обычной заметке нет. Сравниваем не с описанием,
            // а с самим редактором — на той же заметке, тем же щелчком.
            {
                // Сравниваем с ТЕМ ЖЕ редактором, что и работает в наборе:
                // заводить второй значило бы сравнивать со своей же догадкой о
                // том, как он настроен.
                zametti::NoteEditor& twin = editor;
                // ПАЛИТРУ СТАВИТ ОКНО, а не сам вид: в живой программе это
                // делает applyAppearance. Без неё редактор в наборе стоял на
                // системной палитре — белый лист и чёрные буквы вместо наших
                // #fefefb и #1a1a1a, — и сравнение с архивным видом (у него
                // палитра своя) сравнивало заодно и это.
                zametti::applyPalette(twin);
                twin.resize(shotView.size());
                twin.show();
                twin.openFile(shotPath);
                QTest::qWait(400);
                const QPoint at(shotView.viewport()->width() / 2,
                                shotView.viewport()->height() / 2);
                QTest::mouseClick(shotView.viewport(), Qt::LeftButton, {}, at);
                QTest::mouseClick(twin.viewport(), Qt::LeftButton, {}, at);
                QTest::qWait(300);
                const QImage archived = shotView.grab().toImage();
                const QImage live = twin.grab().toImage();
                int differ = 0;
                for (int y = 0; y < qMin(archived.height(), live.height()); ++y)
                    for (int x = 0; x < qMin(archived.width(), live.width()); ++x) {
                        const QColor a = archived.pixelColor(x, y);
                        const QColor b = live.pixelColor(x, y);
                        if (qAbs(a.red() - b.red()) > 20 || qAbs(a.green() - b.green()) > 20 ||
                            qAbs(a.blue() - b.blue()) > 20)
                            ++differ;
                    }
                if (differ >= 400) {
                    const QString dir = zt::TestData::outDir(QStringLiteral("архив-щелчок"));
                    archived.save(QDir(dir).filePath(QStringLiteral("архивная.png")));
                    live.save(QDir(dir).filePath(QStringLiteral("живая.png")));
                    std::fprintf(stderr, "DBG снимки в %s\n", dir.toUtf8().constData());
                }
                ZT_TRUE("выбранная картинка в архиве рисуется как в живой заметке: "
                        "разошлось точек " + std::to_string(differ),
                        differ < 400);
            }

            // ВТОРАЯ ОШИБКА ПОРОЗНЬ: ссылка, чей путь не разрешился, обязана
            // стать РАМКОЙ «файл не найден», а не пустотой. Спрашиваем у вида
            // напрямую — пиксели тут не годятся: пустота и рамка отличаются
            // десятком точек, и такая проверка была бы пустышкой (она ею и
            // оказалась с первого раза).
            struct Probe : zametti::ReaderView {
                // true — вид нарисует РАМКУ (файла нет или путь не разрешился).
                bool framed(const QString& path) {
                    const CachedImage* entry = imageInfo(path);
                    return entry != nullptr && entry->framed();
                }
                bool known(const QString& path) { return imageInfo(path) != nullptr; }
            };
            Probe blind;
            ZT_TRUE("неразрешённый путь даёт запись, а не пустоту",
                    blind.known(QStringLiteral("01jd7f0kq2m8xa.jxl")));
            ZT_TRUE("и запись эта — рамка", blind.framed(QStringLiteral("01jd7f0kq2m8xa.jxl")));
        }

        // СНИМОК ДЛЯ ПРИЁМКИ ГЛАЗАМИ: серое поле, тело на месте, каретки нет.
        // Под Xvfb это артефакт приёмки; под offscreen — просто картинка.
        view.resize(700, 420);
        view.show();
        QTest::qWait(60);
        const QString shots = zt::TestData::outDir(QStringLiteral("архив"));
        const QImage shot = view.grab().toImage();
        ZT_TRUE("снимок сделан", !shot.isNull());
        shot.save(QDir(shots).filePath(QStringLiteral("архивная-заметка.png")));
    }

    return zt::report("архивация открытой заметки");
}

// Набор целиком одним TEST: тело не тронуто, argv ему собран здесь.
// Дробить на отдельные проверки — отдельная работа, по одному набору.
TEST(ArchiveEditor, All) {
    std::vector<QByteArray> ztArgs{QByteArrayLiteral("archive_editor_test")};
    ztArgs.push_back((zt::TestData::outDir(QStringLiteral("archive-editor"))).toLocal8Bit());
    std::vector<char*> ztArgv;
    for (QByteArray& a : ztArgs) ztArgv.push_back(a.data());
    EXPECT_EQ(0, ztRunSuite(int(ztArgv.size()), ztArgv.data()));
}
