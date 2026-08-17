// МАСШТАБ: Ctrl+= и Ctrl+− обязаны менять то, на что человек смотрит.
//
// Набор написан по жалобе владельца «зум не работает вообще» и держит ровно то,
// чего не держал ни один из пяти прежних вопросов про масштаб (editor_test:297,
// 339, 766, 915 и diff_view_test:572). Все пятеро спрашивали, что масштаб НЕ
// портит текст и НЕ заводит шага истории; ни один не спросил, стало ли хоть
// что-нибудь крупнее. Проверка-пустышка ровно того класса, который у меня
// записан в уроках.
//
// Что здесь спрашивается, тремя вопросами:
//
//   1. РАСТЁТ ЛИ ТЕКСТ. Шрифт документа и высота вёрстки — от них зависит
//      всё остальное, и мерить надо их, а не наши намерения.
//   2. ОДИН ЛИ ИСТОЧНИК МАСШТАБА. Маркер списка рисует ВИД, текст верстает Qt;
//      если мера у них разная, при 200 % маркер останется от прежнего кегля.
//      Меряется отношением, а не размером: оно обязано не зависеть от масштаба.
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

#include "test_util.h"
#include "testdata.h"

#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QFontMetricsF>
#include <QScrollBar>
#include <QTest>
#include <QTextBlock>
#include <QTextDocument>

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

// Ширина колонки маркера у первого списочного блока — то, что рисует ВИД.
qreal markerWidth(const zametti::NoteEditor& editor) {
    const QTextDocument* doc = editor.document();
    for (QTextBlock block = doc->begin(); block.isValid(); block = block.next()) {
        if (!zametti::isListBlock(block)) continue;
        return zametti::markerColumn(zametti::markerOf(block), zametti::ordinalOf(block),
                                     zametti::levelOf(block), editor.baseFont());
    }
    return 0.0;
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
    const QString path = writeNote(dir, QStringLiteral("масштаб.md"),
                                   QStringLiteral("# Заголовок\n\n"
                                                  "- первый пункт\n"
                                                  "- второй пункт\n\n"
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
    const qreal markerAt100 = markerWidth(editor);
    const std::string skeleton = skeletonOf(editor);
    const QString text = editor.document()->toPlainText();

    ZT_TRUE("на 100 % есть что мерить: высота строки", unitAt100 > 1.0);
    ZT_TRUE("на 100 % есть что мерить: высота документа", heightAt100 > 1.0);
    ZT_TRUE("на 100 % есть что мерить: колонка маркера", markerAt100 > 1.0);
    shoot(editor, dir, QStringLiteral("масштаб-100.png"));

    // --- 1. РАСТЁТ ЛИ ТЕКСТ ------------------------------------------------
    editor.applyZoom(step);
    QTest::qWait(20);

    shoot(editor, dir, QStringLiteral("масштаб-110.png"));
    const qreal unitBig = textUnit(editor);
    const qreal heightBig = documentHeight(editor);
    ZT_TRUE("Ctrl+=: шрифт документа стал крупнее", unitBig > unitAt100 * 1.02);
    ZT_TRUE("Ctrl+=: документ стал выше", heightBig > heightAt100 * 1.02);

    // --- 2. ОДИН ЛИ ИСТОЧНИК МАСШТАБА --------------------------------------
    //
    // Маркер рисует вид, текст верстает Qt. Отношение их мер обязано быть
    // одним и тем же на любом масштабе — иначе маркер и буквы меряются разным.
    const qreal markerBig = markerWidth(editor);
    const qreal ratio100 = markerAt100 / unitAt100;
    const qreal ratioBig = markerBig / unitBig;
    ZT_TRUE("маркер меряется тем же, чем текст: " + std::to_string(ratio100) + " против " +
                std::to_string(ratioBig),
            std::fabs(ratio100 - ratioBig) < 0.02 * ratio100);

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

    // Крайние ступени — тоже на снимок: ритм полей виден только на них.
    editor.applyZoom(1.5);
    QTest::qWait(20);
    shoot(editor, dir, QStringLiteral("масштаб-150.png"));
    editor.applyZoom(2.0);
    QTest::qWait(20);
    shoot(editor, dir, QStringLiteral("масштаб-200.png"));

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

    return zt::report("zoom");
}

// Набор целиком одним TEST — как и у соседей.
TEST(Zoom, All) {
    std::vector<QByteArray> ztArgs{QByteArrayLiteral("zoom_test")};
    std::vector<char*> ztArgv;
    for (QByteArray& a : ztArgs) ztArgv.push_back(a.data());
    EXPECT_EQ(0, ztRunSuite(int(ztArgv.size()), ztArgv.data()));
}
