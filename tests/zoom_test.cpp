// МАСШТАБ: Ctrl+= и Ctrl+− обязаны менять то, на что человек смотрит.
//
// Набор написан по жалобе владельца «зум не работает вообще» и держит ровно то,
// чего не держал ни один из пяти прежних вопросов про масштаб (editor_test:297,
// 339, 766, 915 и history_view_test — зум слепка). Все пятеро спрашивали, что масштаб НЕ
// портит текст и НЕ заводит шага истории; ни один не спросил, стало ли хоть
// что-нибудь крупнее. Проверка-пустышка ровно того класса, который у меня
// записан в уроках.
//
// Что здесь спрашивается, тремя вопросами:
//
//   1. РАСТЁТ ЛИ ТЕКСТ. Шрифт документа и высота вёрстки — от них зависит
//      всё остальное, и мерить надо их, а не наши намерения.
//   2. МАРКЕР ПРИ ЗУМЕ НЕПОДВИЖЕН ЦЕЛИКОМ (владелец, две живые пробы 31.08:
//      сначала «буллеты съезжают по горизонтали», затем — про первую починку,
//      прибившую только правый край растущего глифа, — «пять раз Ctrl+− — и
//      уезжают вправо, Ctrl+= — влево»). Левый край текста при зуме
//      неподвижен в пикселях (leftMargin не переписывается), значит
//      неподвижен и маркер — ВСЕМ ТЕЛОМ: левый край, правый край, ширина —
//      для всех родов: буллеты трёх уровней, номер, буква, скобка, чекбокс.
//      Меряется дважды: своей геометрией (markerBoxOf) и ЧУЖИМ СУДЬЁЙ —
//      пикселями снимка (левейший столбец чернил строки пункта — это маркер).
//      Прежние редакции вопроса не могли покраснеть на живой беде: отношение
//      markerColumn/высота строки константно по построению, а «зазор
//      постоянен» молчал о теле глифа, ездившем вокруг прибитого края.
//   3. МАСШТАБ — НЕ ПЕРЕСБОРКА. Содержимое и строение обязаны остаться теми же
//      до последнего блока: масштаб это облик, а не правка.
//
// Замер, на котором стоит выбранный механизм (zametti-bench zoom):
// setDefaultFont в стек отмены НЕ ПОПАДАЕТ, а шаг масштаба на 4000 блоках
// стоит 34.7 мс против 151 мс полной пересборки.

#include "doc_model.h"
#include "editor_widget.h"
#include "marker.h"
#include "settings.h"
#include "zoom_target.h"

#include "test_util.h"
#include "testdata.h"

#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QFontDatabase>
#include <QFontInfo>
#include <QFontMetricsF>
#include <QScrollBar>
#include <QTest>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextLayout>

#include <cmath>
#include <string>
#include <vector>

namespace {

QString writeNote(const QString& dir, const QString& name, const QString& text) {
    QDir().mkpath(dir);
    const QString path = dir + QLatin1Char('/') + name;
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return {};
    file.write(text.toUtf8());
    file.close();
    return path;
}

// Высота строки текста, как её видит вёрстка. Мера, с которой сравнивается всё
// остальное.
qreal textUnit(const zametti::NoteEditor& editor) {
    return QFontMetricsF(editor.document()->defaultFont()).height();
}

// Чернила маркера каждого списочного блока — той геометрией, какой маркер
// нарисован (markerBoxOf), относительно левого края текста своего блока.
struct MarkerInk {
    qreal gap = 0;     // текст.left − маркер.right: прибитый зазор
    qreal width = 0;   // ширина чернил
    qreal left = 0;    // левый край чернил в координатах документа
};

std::vector<MarkerInk> markerInks(const zametti::NoteEditor& editor) {
    std::vector<MarkerInk> out;
    const QTextDocument* doc = editor.document();
    for (QTextBlock block = doc->begin(); block.isValid(); block = block.next()) {
        if (!zametti::isListBlock(block)) continue;
        const QRectF box = zametti::markerBoxOf(block);
        if (box.isNull()) continue;
        const qreal textLeft =
            block.layout()->position().x() + block.blockFormat().leftMargin();
        out.push_back({textLeft - box.right(), box.width(), box.left()});
    }
    return out;
}

// ЧУЖОЙ СУДЬЯ: положение маркера ОТНОСИТЕЛЬНО ТЕКСТА своей строки — ПО
// ПИКСЕЛЯМ СНИМКА, без нашей геометрии. В первой строке блока чернила лежат
// двумя прогонами: маркер, зазор (≥ 6 px в базовом шрифте), текст. Судья
// возвращает textLeft − markerLeft — расстояние, которое и «съезжало» в обеих
// жалобах владельца. Абсолютные координаты не годятся: вид при зуме двигает
// ВСЮ колонку (центровка), и текст с маркером едут вместе — это не съезд.
// Прокрутка ставится в ноль, строка обязана влезть во вьюпорт — иначе −2.
int markerToTextPx(zametti::NoteEditor& editor, int blockNumber) {
    editor.verticalScrollBar()->setValue(0);
    QTest::qWait(20);
    const QImage shot = editor.grab().toImage();
    const QTextBlock block = editor.document()->findBlockByNumber(blockNumber);
    const QTextLayout* layout = block.layout();
    if (layout == nullptr || layout->lineCount() == 0) return -1;
    const QTextLine line = layout->lineAt(0);
    const QPointF origin = layout->position();
    const QPoint off = editor.viewport()->mapTo(&editor, QPoint(0, 0));
    const int top = off.y() + int(origin.y() + line.y()) + 1;
    const int bottom = off.y() + int(origin.y() + line.y() + line.height()) - 1;
    if (top < 0 || bottom >= off.y() + editor.viewport()->height()) return -2;
    const QRgb paper = shot.pixel(off.x() + 1, off.y() + 1);
    const auto inkAt = [&](int x) {
        for (int y = top; y <= bottom; ++y) {
            const QRgb p = shot.pixel(x, y);
            if (qAbs(qRed(p) - qRed(paper)) + qAbs(qGreen(p) - qGreen(paper)) +
                    qAbs(qBlue(p) - qBlue(paper)) >
                90)
                return true;
        }
        return false;
    };
    int markerLeft = -1;
    int lastInk = -1;
    for (int x = off.x(); x < shot.width(); ++x) {
        if (!inkAt(x)) {
            // Зазор в 4+ пустых столбца после начала чернил — это уже зазор
            // маркер–текст: внутри маркера просветы не шире 2–3 px.
            if (markerLeft >= 0 && lastInk >= 0 && x - lastInk >= 4) {
                for (int t = x; t < shot.width(); ++t)
                    if (inkAt(t)) return t - markerLeft;
                return -1;
            }
            continue;
        }
        if (markerLeft < 0) markerLeft = x;
        lastInk = x;
    }
    return -1;
}

// Строение документа одной строкой: род и уровень каждого блока. Масштаб не
// имеет права его тронуть.
std::string skeletonOf(const zametti::NoteEditor& editor) {
    std::string out;
    const QTextDocument* doc = editor.document();
    for (QTextBlock block = doc->begin(); block.isValid(); block = block.next()) {
        out += std::to_string(int(zametti::kindOf(block))) + ":" +
               std::to_string(zametti::levelOf(block)) + "\n";
    }
    return out;
}

qreal documentHeight(const zametti::NoteEditor& editor) {
    return editor.document()->documentLayout()->documentSize().height();
}

// Снимок как артефакт приёмки. Числа отвечают на вопрос «поехало ли», но не на
// вопрос «хорошо ли это выглядит»: поля блоков испечены в единице и за шрифтом
// не идут, и решать, беда это или нет, владелец будет глазами.
void shoot(zametti::NoteEditor& editor, const QString& dir, const QString& name) {
    const QImage shot = editor.grab().toImage();
    const QString path = dir + QLatin1Char('/') + name;
    if (!shot.save(path)) std::printf("  НЕ СОХРАНИЛСЯ снимок %s\n", qPrintable(path));
    else std::printf("  снимок: %s\n", qPrintable(path));
}

}  // namespace

static int ztRunSuite(int argc, char** argv) {
    (void)argc;
    (void)argv;
    zametti::loadSettings(nullptr);

    const QString dir = zt::TestData::outDir(QStringLiteral("zoom"));
    // ВСЕ РОДА МАРКЕРОВ: буллеты трёх уровней (диск, кружок, квадрат), номер,
    // буква (уровень 1), скобка (уровень 2), чекбокс — съезд у каждого рода
    // был свой, потому что зазоры у родов разные.
    const QString path = writeNote(dir, QStringLiteral("масштаб.md"),
                                   QStringLiteral("# Заголовок\n\n"
                                                  "- первый пункт\n"
                                                  "- второй пункт\n"
                                                  "  - вложенный\n"
                                                  "    - третий уровень\n\n"
                                                  "1. номер\n"
                                                  "   1. буква\n"
                                                  "      1. скобка\n"
                                                  "10. широкий номер\n\n"
                                                  "- [ ] задача\n\n"
                                                  "Обычный абзац, в котором достаточно слов, "
                                                  "чтобы вёрстка была не пустой.\n"));

    zametti::NoteEditor editor;
    editor.resize(800, 600);
    editor.show();
    QTest::qWait(20);
    editor.openFile(path);
    QTest::qWait(20);

    const qreal step = zametti::settings().ui().zoomStep();
    ZT_TRUE("шаг масштаба задан и больше единицы", step > 1.0);

    const qreal unitAt100 = textUnit(editor);
    const qreal heightAt100 = documentHeight(editor);
    const std::vector<MarkerInk> inks100 = markerInks(editor);
    const std::string skeleton = skeletonOf(editor);
    const QString text = editor.document()->toPlainText();

    ZT_TRUE("на 100 % есть что мерить: высота строки", unitAt100 > 1.0);
    ZT_TRUE("на 100 % есть что мерить: высота документа", heightAt100 > 1.0);
    // 4 буллета + 4 нумерованных + чекбокс.
    ZT_EQ("маркеры всех родов на месте", std::string("9"),
          std::to_string(inks100.size()));
    shoot(editor, dir, QStringLiteral("масштаб-100.png"));

    // Пиксельный судья: расстояние маркер→текст на снимке 100 % — буллет
    // верхнего уровня и чекбокс. Блоки ищутся родом, чтобы не окаменеть.
    const auto blockOfKind = [&](zametti::Marker kind, int skip) {
        int seen = 0;
        const QTextDocument* doc = editor.document();
        for (QTextBlock b = doc->begin(); b.isValid(); b = b.next())
            if (zametti::isListBlock(b) && zametti::markerOf(b).marker == kind &&
                seen++ == skip)
                return b.blockNumber();
        return -1;
    };
    const int bulletBlock = blockOfKind(zametti::Marker::Bullet, 0);
    const int taskBlock = blockOfKind(zametti::Marker::Task, 0);
    ZT_TRUE("буллет и чекбокс найдены", bulletBlock >= 0 && taskBlock >= 0);
    const int bulletInk100 = markerToTextPx(editor, bulletBlock);
    const int taskInk100 = markerToTextPx(editor, taskBlock);
    ZT_TRUE("маркер и текст различимы на снимке 100 %: буллет " +
                std::to_string(bulletInk100) + ", чекбокс " +
                std::to_string(taskInk100),
            bulletInk100 > 0 && taskInk100 > 0);

    // Неподвижность маркера ЦЕЛИКОМ на масштабе z против 100 %: зазор, ширина
    // и левый край — одни и те же В ПИКСЕЛЯХ. Допуск 0.1 px новый, мой:
    // величины аналитические (один и тот же базовый шрифт), запас — на float.
    const auto checkPinned = [&](qreal, const char* label) {
        const std::vector<MarkerInk> inks = markerInks(editor);
        ZT_EQ(std::string(label) + ": маркеров столько же",
              std::to_string(inks100.size()), std::to_string(inks.size()));
        if (inks.size() != inks100.size()) return;
        for (size_t i = 0; i < inks.size(); ++i) {
            ZT_TRUE(std::string(label) + ": зазор маркера #" + std::to_string(i) +
                        " неподвижен: " + std::to_string(inks100[i].gap) + " → " +
                        std::to_string(inks[i].gap),
                    std::fabs(inks[i].gap - inks100[i].gap) < 0.1);
            ZT_TRUE(std::string(label) + ": ширина глифа #" + std::to_string(i) +
                        " неподвижна: " + std::to_string(inks100[i].width) +
                        " → " + std::to_string(inks[i].width),
                    std::fabs(inks[i].width - inks100[i].width) < 0.1);
            ZT_TRUE(std::string(label) + ": левый край глифа #" +
                        std::to_string(i) + " неподвижен: " +
                        std::to_string(inks100[i].left) + " → " +
                        std::to_string(inks[i].left),
                    std::fabs(inks[i].left - inks100[i].left) < 0.1);
        }
    };

    // --- 1. РАСТЁТ ЛИ ТЕКСТ ------------------------------------------------
    editor.applyZoom(step);
    QTest::qWait(20);

    shoot(editor, dir, QStringLiteral("масштаб-110.png"));
    const qreal unitBig = textUnit(editor);
    const qreal heightBig = documentHeight(editor);
    ZT_TRUE("Ctrl+=: шрифт документа стал крупнее", unitBig > unitAt100 * 1.02);
    ZT_TRUE("Ctrl+=: документ стал выше", heightBig > heightAt100 * 1.02);

    // --- 2. МАРКЕР ПРИ ЗУМЕ НЕПОДВИЖЕН ЦЕЛИКОМ ------------------------------
    //
    // Обе прежние редакции вопроса не краснели на живой беде (см. шапку);
    // честная редакция: всё тело маркера — зазор, ширина, левый край — стоит
    // на месте в пикселях, и это же подтверждают чернила снимка.
    checkPinned(step, "110 %");

    // --- 3. МАСШТАБ — НЕ ПЕРЕСБОРКА ---------------------------------------
    ZT_EQ("масштаб не тронул строение", skeleton, skeletonOf(editor));
    ZT_EQ("масштаб не тронул текст", text.toStdString(),
          editor.document()->toPlainText().toStdString());

    // --- и обратно ---------------------------------------------------------
    editor.applyZoom(1.0);
    QTest::qWait(20);

    ZT_TRUE("Ctrl+0 вернул прежнюю высоту строки",
            std::fabs(textUnit(editor) - unitAt100) < 0.01);
    ZT_TRUE("Ctrl+0 вернул прежнюю высоту документа",
            std::fabs(documentHeight(editor) - heightAt100) < 1.0);
    ZT_EQ("после возврата строение то же", skeleton, skeletonOf(editor));

    // --- уменьшение тоже работает ------------------------------------------
    editor.applyZoom(1.0 / step);
    QTest::qWait(20);
    ZT_TRUE("Ctrl+−: шрифт документа стал мельче", textUnit(editor) < unitAt100 * 0.98);

    // Крайние ступени — прибивка числом и снимок глазам.
    editor.applyZoom(1.5);
    QTest::qWait(20);
    checkPinned(1.5, "150 %");
    shoot(editor, dir, QStringLiteral("масштаб-150.png"));
    editor.applyZoom(2.0);
    QTest::qWait(20);
    checkPinned(2.0, "200 %");
    shoot(editor, dir, QStringLiteral("масштаб-200.png"));

    // Пиксельный судья на 200 %: маркер стоит от текста там же, где на 100 %.
    // Допуск 3 px: чернила первой буквы начинаются с её бокового выноса, а он
    // растёт с кеглем (замер: 2 px на 200 %); прежний съезд был 8–9 px и в
    // 3 px не спрячется. Ровно жест владельца «нажать Ctrl+= и смотреть».
    // Окно на время замера выше: на 200 % чекбокс не влезает в 600 px.
    {
        editor.resize(800, 1300);
        QTest::qWait(40);
        const int bulletInk = markerToTextPx(editor, bulletBlock);
        const int taskInk = markerToTextPx(editor, taskBlock);
        ZT_TRUE("буллет на 200 % не съехал от текста: " +
                    std::to_string(bulletInk100) + " → " + std::to_string(bulletInk),
                std::abs(bulletInk - bulletInk100) <= 3);
        ZT_TRUE("чекбокс на 200 % не съехал от текста: " +
                    std::to_string(taskInk100) + " → " + std::to_string(taskInk),
                std::abs(taskInk - taskInk100) <= 3);
        editor.resize(800, 600);
        QTest::qWait(40);
    }

    // Мишень щелчка чекбокса — та же геометрия, что у отрисовки: на 200 %
    // щелчок по центру рамки обязан попасть в свой блок (мишень раньше уезжала
    // вместе с якорем).
    {
        const QTextDocument* doc = editor.document();
        bool found = false;
        for (QTextBlock block = doc->begin(); block.isValid(); block = block.next()) {
            if (zametti::markerOf(block).marker != zametti::Marker::Task) continue;
            const QRectF box = zametti::checkboxRect(block);
            ZT_TRUE("рамка чекбокса на 200 % не пуста", !box.isNull());
            const QTextBlock hit = zametti::blockAtCheckbox(*doc, box.center());
            ZT_TRUE("щелчок по центру рамки на 200 % попадает в свой блок",
                    hit.isValid() && hit.blockNumber() == block.blockNumber());
            found = true;
            break;
        }
        ZT_TRUE("чекбокс в заметке есть", found);
    }

    // Узкое окно + 200 %: маркер не выходит ни за левый край колонки, ни на
    // текст своего пункта (базовый глиф в базовой колонке — по построению).
    editor.resize(500, 600);
    QTest::qWait(40);
    {
        const std::vector<MarkerInk> inks = markerInks(editor);
        for (size_t i = 0; i < inks.size(); ++i) {
            ZT_TRUE("узко+200 %: маркер #" + std::to_string(i) +
                        " не за левым краем: left=" + std::to_string(inks[i].left),
                    inks[i].left >= -0.5);
            ZT_TRUE("узко+200 %: маркер #" + std::to_string(i) +
                        " не налез на текст: зазор " + std::to_string(inks[i].gap),
                    inks[i].gap > -0.1);
        }
    }
    shoot(editor, dir, QStringLiteral("ширина-500-масштаб-200.png"));
    editor.resize(800, 600);
    QTest::qWait(40);

    editor.applyZoom(1.0);
    QTest::qWait(20);

    // --- 4. ПРОКРУТКА ДЕРЖИТСЯ ---------------------------------------------
    //
    // Жалоба владельца: «при Ctrl+= скроллинг уезжает куда-то». Высота документа
    // от смены кегля меняется, а прокрутка задана пикселями — и текст уезжает
    // тем сильнее, чем ниже по заметке человек стоял. Правило: строка, бывшая в
    // середине экрана, там и остаётся.
    {
        QString big = QStringLiteral("# Длинная\n\n");
        for (int i = 0; i < 200; ++i)
            big += QStringLiteral("Абзац номер %1, в нём достаточно слов.\n\n").arg(i);
        const QString longPath = writeNote(dir, QStringLiteral("длинная.md"), big);
        zametti::NoteEditor scrolled;
        scrolled.resize(800, 600);
        scrolled.show();
        QTest::qWait(20);
        scrolled.openFile(longPath);
        QTest::qWait(30);

        scrolled.verticalScrollBar()->setValue(scrolled.verticalScrollBar()->maximum() / 2);
        QTest::qWait(20);

        const auto blockInMiddle = [&scrolled] {
            const QAbstractTextDocumentLayout* layout = scrolled.document()->documentLayout();
            const int middle =
                scrolled.verticalScrollBar()->value() + scrolled.viewport()->height() / 2;
            return scrolled.document()
                ->findBlock(layout->hitTest(QPointF(0, middle), Qt::FuzzyHit))
                .blockNumber();
        };

        const int before = blockInMiddle();
        ZT_TRUE("прокрутили в середину длинной заметки: блок " + std::to_string(before),
                before > 20);

        scrolled.applyZoom(1.5);
        QTest::qWait(30);
        const int afterIn = blockInMiddle();
        ZT_TRUE("после Ctrl+= в середине тот же блок: было " + std::to_string(before) +
                    ", стало " + std::to_string(afterIn),
                std::abs(afterIn - before) <= 2);

        scrolled.applyZoom(1.0);
        QTest::qWait(30);
        const int afterOut = blockInMiddle();
        ZT_TRUE("и после Ctrl+- тоже: " + std::to_string(afterOut),
                std::abs(afterOut - before) <= 2);
    }

    // Ширина колонки: узкое окно, широкое и очень широкое.
    editor.resize(500, 600);
    QTest::qWait(40);
    shoot(editor, dir, QStringLiteral("ширина-500.png"));
    editor.resize(1000, 600);
    QTest::qWait(40);
    shoot(editor, dir, QStringLiteral("ширина-1000.png"));
    editor.resize(1600, 600);
    QTest::qWait(40);
    shoot(editor, dir, QStringLiteral("ширина-1600.png"));
    std::printf("  вьюпорт x=%d ширина=%d при окне 1600\n", editor.viewport()->x(),
                editor.viewport()->width());

    // ГОРИЗОНТАЛЬНОЙ ПОЛОСЫ ПРОКРУТКИ НЕ БЫВАЕТ. Владелец увидел её при запуске:
    // поля вьюпорта уже отняли ширину, а документ ещё считает себя прежним.
    {
        // ПОРЯДОК КАК В ЖИЗНИ: приложение открывает заметку ДО show(), в окне
        // ещё не своего размера, и только потом окно раскрывается. Прежняя
        // редакция открывала после show и беды не показывала вовсе.
        zametti::NoteEditor fresh;
        fresh.openFile(path);
        fresh.resize(1600, 600);
        fresh.show();
        QTest::qWait(50);
        std::printf("  при запуске: полоса до %d, textWidth %.0f, вёрстка %.0f, вьюпорт %d\n",
                    fresh.horizontalScrollBar()->maximum(),
                    double(fresh.document()->textWidth()),
                    double(fresh.document()->documentLayout()->documentSize().width()),
                    fresh.viewport()->width());
        // ДОКУМЕНТ ВЁРСТАН ПО ВЬЮПОРТУ. Полоса прокрутки — лишь следствие; мерить
        // надо причину, иначе проверка молчит там, где беда уже есть.
        ZT_TRUE("ширина вёрстки не больше вьюпорта",
                fresh.document()->textWidth() <= fresh.viewport()->width() + 1);
        ZT_TRUE("при запуске горизонтальной полосы прокрутки нет",
                fresh.horizontalScrollBar()->maximum() == 0);
    }

    // КОМУ ДОСТАЮТСЯ КЛАВИШИ МАСШТАБА — правило чистой функцией, и спрашиваем
    // её прямо. Дважды эта развилка ошибалась на живых людях, и оба раза
    // одинаково: она забывала режим и отдавала клавиши скрытому обычному виду.
    // Владелец находил это только при выходе из режима — «а заметка вдруг
    // другого размера».
    {
        using zametti::ZoomTarget;
        using zametti::zoomTargetFor;
        ZT_TRUE("без режимов — обычный вид",
                zoomTargetFor(false, false, false) == ZoomTarget::Note);
        ZT_TRUE("исходник — плоским видам",
                zoomTargetFor(false, true, false) == ZoomTarget::Plain);
        ZT_TRUE("настройки — плоским видам",
                zoomTargetFor(true, false, false) == ZoomTarget::Plain);
        // Вот эта строка и краснела бы при беде 27.08.2026: клавиши в истории
        // доставались обычному виду, и живая заметка меняла кегль втихую.
        ZT_TRUE("история — разности, а НЕ заметке",
                zoomTargetFor(false, false, true) == ZoomTarget::History);
        // Порядок тот же, что у выбора страницы стека: настройки выше истории,
        // история выше исходника.
        ZT_TRUE("настройки поверх истории",
                zoomTargetFor(true, false, true) == ZoomTarget::Plain);
        ZT_TRUE("история поверх исходника",
                zoomTargetFor(false, true, true) == ZoomTarget::History);
    }

    // --- 5. ГАРНИТУРА НОМЕРА ПРИБИТА К ОСНОВНОМУ ШРИФТУ ЗАМЕТКИ ------------
    //
    // В base может приехать шрифт другого режима (у основного и markdown-
    // режимов шрифты разные — слова владельца 31.08); цифры номера обязаны
    // остаться шрифтом заметки. base нарочно ЖИВОЙ другой гарнитуры: на
    // выдуманной подделку не отличить от честного фолбэка.
    {
        const zametti::ZDocStyle& look = zametti::settings().style();
        const QString wanted = QFontInfo(QFont(look.fontFamily())).family();
        QString other;
        for (const QString& family : QFontDatabase::families()) {
            if (QFontInfo(QFont(family)).family() == wanted) continue;
            other = family;
            break;
        }
        ZT_TRUE("нашлась другая живая гарнитура: " + other.toStdString(),
                !other.isEmpty());
        QFont base(other);
        base.setPointSizeF(23.5);
        zametti::MarkerStyle ordered;
        ordered.marker = zametti::Marker::Ordered;
        const QFont font = zametti::markerFontFor(ordered, base, look);
        ZT_EQ("гарнитура номера — основной шрифт заметки", wanted.toStdString(),
              QFontInfo(font).family().toStdString());
        ZT_TRUE("кегль номера идёт за base (зумом)",
                std::fabs(font.pointSizeF() - 23.5) < 0.01);
        // Буллет-глиф и чекбокс здесь не проверяются: кружочки и рамки
        // рисуются вручную намеренно (решение владельца, CLAUDE.md).
    }

    return zt::report("zoom");
}

// Набор целиком одним TEST — как и у соседей.
TEST(Zoom, All) {
    std::vector<QByteArray> ztArgs{QByteArrayLiteral("zoom_test")};
    std::vector<char*> ztArgv;
    for (QByteArray& a : ztArgs) ztArgv.push_back(a.data());
    EXPECT_EQ(0, ztRunSuite(int(ztArgv.size()), ztArgv.data()));
}
