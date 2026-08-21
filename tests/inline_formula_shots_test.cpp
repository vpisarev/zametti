// Снимки СТРОЧНЫХ формул: заметка владельца «Typesetting Math in Markdown» в
// настоящем окне (приёмка сессии — он просил отрендерить именно её), посадка,
// дедупликация движка, флип клавишами и мышью, зум, симметрия «вернулся ==
// открыл заново», бумага.
//
// Матрица окон — широкое И узкое (правило проекта: три бага класса «у агента
// окно узкое, у владельца широкое»).

#include "doc_model.h"
#include "editor_widget.h"
#include "formula.h"
#include "formula_object.h"
#include "settings.h"
#include "test_util.h"
#include "testdata.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QPainter>
#include <QScrollBar>
#include <QSet>
#include <QTest>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFragment>

#include <string>

namespace {

QString g_dir;

zametti::NoteEditor* openNote(const QString& name, int width, int height, const QByteArray& text) {
    const QString path = QDir(g_dir).filePath(name + QStringLiteral(".md"));
    QFile file(path);
    if (file.open(QIODevice::WriteOnly)) file.write(text);
    file.close();

    auto* editor = new zametti::NoteEditor;
    editor->resize(width, height);
    editor->show();
    QTest::qWait(20);
    editor->openFile(path);
    QTest::qWait(80);
    QTextCursor at = editor->textCursor();
    at.setPosition(0);
    editor->setTextCursor(at);
    QTest::qWait(60);
    return editor;
}

// Позиции знаков строчных формул документа.
std::vector<int> inlineObjectPositions(const QTextDocument& doc) {
    std::vector<int> out;
    for (QTextBlock block = doc.begin(); block.isValid(); block = block.next())
        for (QTextBlock::iterator it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment f = it.fragment();
            if (!f.isValid() || f.charFormat().objectType() != zametti::InlineFormulaObject)
                continue;
            for (int n = 0; n < f.length(); ++n) out.push_back(f.position() + n);
        }
    return out;
}

// Сколько РАЗНЫХ вёрсток нужно заметке: разные исходники строчных плюс разные
// исходники выключных (ключ кэша учитывает род — считаем так же).
int distinctRenders(const QTextDocument& doc) {
    QSet<QString> keys;
    for (QTextBlock block = doc.begin(); block.isValid(); block = block.next()) {
        const zametti::BlockFormulaRef ref = zametti::blockFormulaRef(block);
        if (ref.valid && ref.display) keys.insert(QStringLiteral("D:") + ref.source);
        for (QTextBlock::iterator it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment f = it.fragment();
            if (f.isValid() && f.charFormat().objectType() == zametti::InlineFormulaObject)
                keys.insert(QStringLiteral("i:") +
                            f.charFormat().property(zametti::ObjectSourceProperty).toString());
        }
    }
    return keys.size();
}

int inkIn(const QImage& shot, const QRectF& box, qreal dpr) {
    int dark = 0;
    const QRectF at(box.left() * dpr, box.top() * dpr, box.width() * dpr, box.height() * dpr);
    for (int x = qMax(0, int(at.left())); x < int(at.right()) && x < shot.width(); ++x)
        for (int y = qMax(0, int(at.top())); y < int(at.bottom()) && y < shot.height(); ++y)
            if (qGray(shot.pixel(x, y)) < 128) ++dark;
    return dark;
}

// --- приёмка: заметка владельца в окне ---------------------------------------

void checkOwnersNoteRendered(const QByteArray& note, int width, const QString& name) {
    zametti::NoteEditor* editor = openNote(name, width, 800, note);
    const QImage shot = editor->grab().toImage();
    if (!shot.save(QDir(g_dir).filePath(name + QStringLiteral(".png"))))
        std::printf("НЕ СОХРАНИЛСЯ снимок %s\n", qPrintable(name));

    const std::vector<int> objects = inlineObjectPositions(*editor->document());
    ZT_TRUE("строчных формул десятки: " + std::to_string(objects.size()),
            objects.size() >= 50);

    // Вёрстка стоит на месте знаков: у видимых объектов прямоугольник не пуст
    // и в нём есть чернила.
    const QPoint origin = editor->viewport()->mapTo(editor, QPoint(0, 0));
    const qreal dpr = shot.devicePixelRatio();
    int checked = 0;
    int inked = 0;
    for (const int position : objects) {
        const QRectF box = zametti::inlineFormulaRect(*editor->document(), position);
        if (box.isEmpty()) continue;
        const QRectF inViewport = box.translated(origin.x(), origin.y() - 0.0);
        if (inViewport.bottom() >= editor->viewport()->height()) break;   // ниже окна
        ++checked;
        if (inkIn(shot, inViewport, dpr) > 0) ++inked;
    }
    ZT_TRUE("видимых формул в окне немало: " + std::to_string(checked), checked >= 8);
    ZT_EQ("чернила есть у каждой видимой", std::to_string(checked), std::to_string(inked));

    delete editor;
}

// --- дедупликация: движок зовётся по числу РАЗНЫХ формул -----------------------

void checkEngineDedup(const QByteArray& note) {
    zametti::Formulas::resetRenders();
    zametti::NoteEditor* editor = openNote(QStringLiteral("дедуп"), 1000, 760, note);

    // Прокрутить заметку до конца, рисуя каждую страницу: ленивому кэшу
    // достанется каждый объект.
    QScrollBar* bar = editor->verticalScrollBar();
    for (int value = 0; value <= bar->maximum(); value += 600) {
        bar->setValue(value);
        QTest::qWait(10);
        (void)editor->grab();
    }
    const int distinct = distinctRenders(*editor->document());
    const int called = zametti::Formulas::renders();
    std::printf("движок: %d вызовов на %d разных формул\n", called, distinct);
    ZT_EQ("вызовов движка — по числу РАЗНЫХ формул (порог брифа)",
          std::to_string(distinct), std::to_string(called));
    delete editor;
}

// --- симметрия: вернулся == открыл заново --------------------------------------

void checkSwitchAndReturn(const QByteArray& note) {
    zametti::NoteEditor* editor = openNote(QStringLiteral("сим-а"), 900, 700, note);
    const QImage fresh = editor->grab().toImage();

    const QString other = QDir(g_dir).filePath(QStringLiteral("сим-другая.md"));
    QFile file(other);
    if (file.open(QIODevice::WriteOnly)) file.write("# Другая\n\nПросто текст.\n");
    file.close();
    editor->openFile(other);
    QTest::qWait(60);
    editor->openFile(QDir(g_dir).filePath(QStringLiteral("сим-а.md")));
    QTest::qWait(80);
    QTextCursor at = editor->textCursor();
    at.setPosition(0);
    editor->setTextCursor(at);
    QTest::qWait(60);
    const QImage back = editor->grab().toImage();

    ZT_TRUE("switch-and-return == fresh open (снимки пиксель в пиксель)", back == fresh);
    if (back != fresh) {
        fresh.save(QDir(g_dir).filePath(QStringLiteral("сим-свежий.png")));
        back.save(QDir(g_dir).filePath(QStringLiteral("сим-возврат.png")));
    }
    delete editor;
}

// --- флип клавишами и мышью ------------------------------------------------------

void checkFlipKeys() {
    zametti::NoteEditor* editor = openNote(
        QStringLiteral("флип"), 800, 400,
        QByteArray("До \x24x^2\x24 после и хвост.\n\nВторой абзац, куда уходит каретка.\n"));
    QTextDocument* doc = editor->document();
    const std::vector<int> objects = inlineObjectPositions(*doc);
    ZT_EQ("объект один", "1", std::to_string(objects.size()));
    const int knob = objects.front();

    // Enter на ВЫДЕЛЕННОМ знаке — раскрыть.
    QTextCursor select(doc);
    select.setPosition(knob);
    select.setPosition(knob + 1, QTextCursor::KeepAnchor);
    editor->setTextCursor(select);
    QTest::keyClick(editor, Qt::Key_Return);
    QTest::qWait(30);
    ZT_TRUE("Enter раскрыл исходник",
            zametti::hasOpenInlineFormula(doc->findBlock(knob)));
    ZT_TRUE("исходник в тексте блока",
            doc->findBlock(knob).text().contains(QStringLiteral("$x^2$")));

    // Набор продолжает раскрытый кусок; Esc сворачивает судьёй.
    QTextCursor inside(doc);
    inside.setPosition(knob + 4);   // за «2», перед закрывающим долларом
    editor->setTextCursor(inside);
    QTest::keyClicks(editor, QStringLiteral("1"));
    QTest::qWait(20);
    QTest::keyClick(editor, Qt::Key_Escape);
    QTest::qWait(30);
    const std::vector<int> closed = inlineObjectPositions(*doc);
    ZT_EQ("Esc свернул в объект", "1", std::to_string(closed.size()));
    {
        QTextCursor probe(doc);
        probe.setPosition(closed.front() + 1);
        ZT_EQ("правка в исходнике", "$x^21$",
              probe.charFormat().property(zametti::ObjectSourceProperty).toString()
                  .toStdString());
    }

    // Ctrl+Z после Esc возвращает раскрытую.
    QTest::keyClick(editor, Qt::Key_Z, Qt::ControlModifier);
    QTest::qWait(30);
    ZT_TRUE("отмена вернула раскрытую",
            zametti::hasOpenInlineFormula(doc->findBlock(knob)));

    // Уход каретки из блока сворачивает сам.
    QTextCursor away(doc);
    away.setPosition(doc->lastBlock().position());
    editor->setTextCursor(away);
    QTest::qWait(30);
    ZT_EQ("уход каретки свернул", "1",
          std::to_string(inlineObjectPositions(*doc).size()));
    delete editor;
}

void checkDoubleClickOpens() {
    zametti::NoteEditor* editor = openNote(QStringLiteral("дважды"), 800, 400,
                                           QByteArray("Тут \x24\\alpha\x24 стоит.\n"));
    QTextDocument* doc = editor->document();
    const std::vector<int> objects = inlineObjectPositions(*doc);
    ZT_EQ("объект один", "1", std::to_string(objects.size()));
    const QRectF box = zametti::inlineFormulaRect(*doc, objects.front());
    ZT_TRUE("вёрстка на месте", !box.isEmpty());
    const QPoint at = editor->viewport()->mapTo(
        editor, box.center().toPoint() -
                    QPoint(editor->horizontalScrollBar()->value(),
                           editor->verticalScrollBar()->value()));
    QTest::mouseDClick(editor->viewport(), Qt::LeftButton, Qt::NoModifier,
                       editor->viewport()->mapFrom(editor, at));
    QTest::qWait(30);
    ZT_TRUE("двойной щелчок раскрыл",
            zametti::hasOpenInlineFormula(doc->firstBlock()));
    delete editor;
}

// --- зум и бумага ------------------------------------------------------------------

void checkZoomAndPaper() {
    zametti::NoteEditor* editor = openNote(QStringLiteral("зум"), 800, 400,
                                           QByteArray("Тут \x24\\frac{a}{b}\x24 дробь.\n"));
    QTextDocument* doc = editor->document();
    const std::vector<int> objects = inlineObjectPositions(*doc);
    ZT_EQ("объект один", "1", std::to_string(objects.size()));
    const QRectF before = zametti::inlineFormulaRect(*doc, objects.front());
    ZT_TRUE("вёрстка посчитана", !before.isEmpty());

    editor->setZoom(2.0);
    QTest::qWait(60);
    const QRectF scaled = zametti::inlineFormulaRect(*doc, objects.front());
    ZT_TRUE("на 200 % вёрстка выросла примерно вдвое: " +
                std::to_string(scaled.width() / qMax(1.0, before.width())),
            scaled.width() > before.width() * 1.6 && scaled.width() < before.width() * 2.4);
    editor->setZoom(1.0);
    QTest::qWait(60);

    // Бумага: renderSlice рисует строчную формулу через drawObject — чернила
    // на месте, без окна и выделения.
    const QRectF box = zametti::inlineFormulaRect(*doc, objects.front());
    QImage paper(600, 200, QImage::Format_RGB32);
    paper.fill(Qt::white);
    {
        QPainter painter(&paper);
        editor->renderSlice(painter, QRectF(0, 0, 600, 200), 1.0);
    }
    ZT_TRUE("на бумаге у формулы есть чернила", inkIn(paper, box, 1.0) > 0);
    delete editor;
}

// Случай владельца дословно: формулы, приклеенные к буквам, — «$\Pi$иф$\alpha$гор».
void checkGluedRendered() {
    zametti::NoteEditor* editor =
        openNote(QStringLiteral("пифагор"), 700, 300,
                 QByteArray("\x24\\Pi\x24\xD0\xB8\xD1\x84\x24\\alpha\x24\xD0\xB3\xD0\xBE\xD1\x80"
                            " \xE2\x80\x94 \xD1\x82\xD0\xB5\xD0\xBE\xD1\x80\xD0\xB5\xD0\xBC\xD0"
                            "\xB0.\n"));
    QTextDocument* doc = editor->document();
    const std::vector<int> objects = inlineObjectPositions(*doc);
    ZT_EQ("две формулы среди букв", "2", std::to_string(objects.size()));
    const QImage shot = editor->grab().toImage();
    shot.save(QDir(g_dir).filePath(QStringLiteral("пифагор.png")));
    const QPoint origin = editor->viewport()->mapTo(editor, QPoint(0, 0));
    for (const int position : objects) {
        const QRectF box = zametti::inlineFormulaRect(*doc, position);
        ZT_TRUE("вёрстка на месте", !box.isEmpty());
        ZT_TRUE("и с чернилами",
                inkIn(shot, box.translated(origin), shot.devicePixelRatio()) > 0);
    }
    delete editor;
}

}  // namespace

static int ztRunSuite(int argc, char** argv) {
    g_dir = argc > 1 ? QString::fromLocal8Bit(argv[1])
                     : zt::TestData::outDir(QStringLiteral("inline-formula-shots"));
    QDir().mkpath(g_dir);

    QString error;
    if (!zametti::Formulas::init(&error)) {
        std::printf("formula engine failed to start: %s\n", qPrintable(error));
        ++zt::g_checks;
        ++zt::g_failures;
        return zt::report("снимки строчных формул");
    }

    checkFlipKeys();
    checkDoubleClickOpens();
    checkZoomAndPaper();
    checkGluedRendered();

    // Приёмка на копии заметки владельца — если корпус на месте.
    const QString ownPath = zt::TestData::file(QStringLiteral("typesetting-math.md"));
    if (ownPath.isEmpty()) {
        std::printf("ПРОПУСК: нет .testdata/typesetting-math.md — снимков заметки "
                    "владельца не будет\n");
    } else {
        QFile file(ownPath);
        QByteArray note;
        if (file.open(QIODevice::ReadOnly)) note = file.readAll();
        checkOwnersNoteRendered(note, 1600, QStringLiteral("typesetting-широкое"));
        checkOwnersNoteRendered(note, 700, QStringLiteral("typesetting-узкое"));
        checkEngineDedup(note);
        checkSwitchAndReturn(note);
    }

    std::printf("снимки: %s\n", qPrintable(g_dir));
    return zt::report("снимки строчных формул");
}

TEST(InlineFormulaShots, All) {
    std::vector<QByteArray> ztArgs{QByteArrayLiteral("inline_formula_shots_test")};
    std::vector<char*> ztArgv;
    for (QByteArray& a : ztArgs) ztArgv.push_back(a.data());
    EXPECT_EQ(0, ztRunSuite(int(ztArgv.size()), ztArgv.data()));
}
