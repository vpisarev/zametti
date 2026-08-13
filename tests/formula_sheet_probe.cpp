// Контрольный лист этапа: корпус разведки как заметка, снимки страница за
// страницей.
//
// Приёмочная фикстура №1 из брифа: `Typesetting Math in Texts.md` открывается
// живым окном, и все его формулы проверяются глазами против листов разведки.
// Числами здесь спрашивается только то, что числами и проверяется: сколько
// формул показано вёрсткой и сколько блоков осталось дословными.
//
// Пробник, а не набор: он ничего не утверждает, он показывает. Утверждения
// живут в formula_shots_test — там их и проверяют.

#include "doc_model.h"
#include "editor_widget.h"
#include "formula.h"

#include <QApplication>
#include <QDir>
#include <QImage>
#include <QScrollBar>
#include <QTest>
#include <QTextBlock>
#include <QTextDocument>

#include <cstdio>

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    if (argc < 3) {
        std::fprintf(stderr, "использование: formula_sheet_probe <заметка.md> <каталог>\n");
        return 2;
    }
    const QString note = QString::fromLocal8Bit(argv[1]);
    const QString dir = QString::fromLocal8Bit(argv[2]);
    QDir().mkpath(dir);

    QString error;
    if (!zametti::Formulas::init(&error)) {
        std::fprintf(stderr, "движок формул не поднялся: %s\n", qPrintable(error));
        return 1;
    }

    zametti::NoteEditor editor;
    editor.resize(1000, 900);
    editor.show();
    QTest::qWait(30);
    editor.openFile(note);
    QTest::qWait(250);

    int raw = 0;
    int rendered = 0;
    int broken = 0;
    for (QTextBlock b = editor.document()->firstBlock(); b.isValid(); b = b.next()) {
        if (zametti::isRawBlock(b)) ++raw;
        if (const zametti::FormulaRender* r = editor.formulaAt(b.blockNumber())) {
            ++rendered;
            if (!r->error.isEmpty()) ++broken;
        }
    }
    std::printf("формул вёрсткой %d (из них битых %d), дословных блоков %d\n", rendered, broken,
                raw);
    std::printf("рендеров движка %d\n", zametti::Formulas::renders());

    // Страница за страницей, с перекрытием в четверть экрана: так ни одна
    // формула не оказывается разрезанной между снимками во всех кадрах сразу.
    QScrollBar* bar = editor.verticalScrollBar();
    const int step = int(editor.viewport()->height() * 0.75);
    int page = 0;
    for (int at = 0; at <= bar->maximum(); at += step, ++page) {
        bar->setValue(at);
        QTest::qWait(60);
        const QString name = QStringLiteral("лист-%1.png").arg(page, 2, 10, QLatin1Char('0'));
        if (!editor.grab().toImage().save(QDir(dir).filePath(name)))
            std::fprintf(stderr, "не сохранился %s\n", qPrintable(name));
        if (bar->value() >= bar->maximum()) break;
    }
    std::printf("листов: %d, каталог: %s\n", page + 1, qPrintable(dir));
    return 0;
}
