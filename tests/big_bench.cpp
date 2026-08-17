// СКОЛЬКО СТОЯТ МЕСТНЫЕ ДЕЙСТВИЯ НА БОЛЬШОЙ ЗАМЕТКЕ — И РАСТУТ ЛИ ОНИ С НЕЙ.
//
// Правило проекта: цена нажатия, хода каретки, выделения и кадра зависит от
// объёма ПОКАЗАННОГО, а не от размера заметки. Владелец назвал случай, где это
// нарушено грубо: «Братья Карамазовы» (1.9 МБ), выделение текста у самого
// конца — программа виснет так, что система предлагает её убить.
//
// Стенд открывает заметку в живом NoteEditor трёх размеров — целиком, половина,
// десятая часть — и меряет одни и те же действия. Читать надо СТЕПЕНЬ: если
// число стоит на месте — O(1); растёт вдесятеро от десятой части к целому —
// O(N); в сто раз — O(N²). Стенд, а не набор: ответ — таблица, на которую надо
// смотреть.
//
//   zametti-bench big <заметка.md>

#include "document.h"
#include "editor_widget.h"

#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QKeyEvent>
#include <QScrollBar>
#include <QTest>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

namespace {

double micros(const std::function<void()>& body) {
    QElapsedTimer timer;
    timer.start();
    body();
    return double(timer.nsecsElapsed()) / 1000.0;
}

void key(QWidget& w, int k, Qt::KeyboardModifiers mods = Qt::NoModifier, const QString& text = {}) {
    QKeyEvent press(QEvent::KeyPress, k, mods, text);
    QApplication::sendEvent(&w, &press);
    QKeyEvent release(QEvent::KeyRelease, k, mods, text);
    QApplication::sendEvent(&w, &release);
}

// Нажатие вместе с тем, что оно тянет за собой до следующего кадра: сигналы,
// отложенные вызовы, перерисовка. Именно это чувствует человек.
double keyAndFrame(zametti::NoteEditor& e, int k, Qt::KeyboardModifiers mods = Qt::NoModifier,
                   const QString& text = {}) {
    return micros([&] {
        key(e, k, mods, text);
        QApplication::processEvents(QEventLoop::AllEvents);
        e.viewport()->repaint();
    });
}

// Голова файла: первые blocks логических блоков, чтобы «половина» и «десятая»
// были той же заметкой, а не другой.
std::string headOf(const std::string& source, double fraction) {
    if (fraction >= 1.0) return source;
    size_t cut = size_t(double(source.size()) * fraction);
    // до конца абзаца, чтобы не резать посреди строки
    size_t nl = source.find("\n\n", cut);
    if (nl == std::string::npos) return source;
    return source.substr(0, nl + 1);
}

struct Row {
    std::string what;
    std::vector<double> values;   // по размерам
};

}  // namespace

int ztBigBench(int argc, char** argv) {
    if (argc < 2) {
        std::printf("zametti-bench big <заметка.md>\n");
        return 2;
    }
    const QString srcPath = QString::fromLocal8Bit(argv[1]);
    // Режим ПЕТЛИ: zametti-bench big <заметка> loop <сценарий> <повторов> —
    // одно действие на целой заметке много раз подряд, чтобы снимать стеки
    // (perf на этой машине заперт, остаётся gdb по таймеру).
    const bool loopMode = argc >= 5 && std::string(argv[2]) == "loop";
    std::string source;
    {
        QFile f(srcPath);
        if (!f.open(QIODevice::ReadOnly)) {
            std::printf("не читается: %s\n", argv[1]);
            return 1;
        }
        const QByteArray bytes = f.readAll();
        source.assign(bytes.constData(), size_t(bytes.size()));
    }

    const QString dir = QStringLiteral("/tmp/zametti-big-bench");
    QDir(dir).removeRecursively();
    QDir().mkpath(dir);
    // Вторая, маленькая заметка — для «переключиться и вернуться».
    const QString otherPath = dir + QStringLiteral("/other.md");
    {
        QFile f(otherPath);
        if (f.open(QIODevice::WriteOnly)) f.write("# Другая\n\nкоротко\n");
    }

    if (loopMode) {
        const std::string scenario = argv[3];
        const int repeat = std::atoi(argv[4]);
        const QString path = dir + QStringLiteral("/full.md");
        {
            QFile f(path);
            if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
                f.write(QByteArray(source.data(), qsizetype(source.size())));
        }
        zametti::NoteEditor editor;
        editor.resize(1000, 700);
        editor.show();
        QTest::qWait(20);
        editor.openFile(path);
        QTest::qWait(100);
        keyAndFrame(editor, Qt::Key_End, Qt::ControlModifier);
        QTest::qWait(100);
        std::printf("петля «%s» × %d — снимайте стеки\n", scenario.c_str(), repeat);
        std::fflush(stdout);
        QElapsedTimer total;
        total.start();
        for (int i = 0; i < repeat; ++i) {
            if (scenario == "type") keyAndFrame(editor, Qt::Key_X, Qt::NoModifier, QStringLiteral("x"));
            else if (scenario == "shift-up") {
                keyAndFrame(editor, Qt::Key_Up, Qt::ShiftModifier);
                if (i % 5 == 4) keyAndFrame(editor, Qt::Key_End, Qt::ControlModifier);
            } else if (scenario == "drag") {
                QTextCursor c = editor.textCursor();
                c.clearSelection();
                editor.setTextCursor(c);
                const QRect r = editor.cursorRect();
                const QPoint from = editor.viewport()->mapToGlobal(r.center());
                QTest::mousePress(editor.viewport(), Qt::LeftButton, Qt::NoModifier, r.center());
                for (int j = 1; j <= 3; ++j) {
                    QMouseEvent move(QEvent::MouseMove, QPointF(r.center() - QPoint(0, 20 * j)),
                                     QPointF(from), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
                    QApplication::sendEvent(editor.viewport(), &move);
                    QApplication::processEvents(QEventLoop::AllEvents);
                    editor.viewport()->repaint();
                }
                QTest::mouseRelease(editor.viewport(), Qt::LeftButton, Qt::NoModifier,
                                    r.center() - QPoint(0, 60));
                QApplication::processEvents(QEventLoop::AllEvents);
            } else if (scenario == "end") {
                keyAndFrame(editor, Qt::Key_Home, Qt::ControlModifier);
                keyAndFrame(editor, Qt::Key_End, Qt::ControlModifier);
            } else if (scenario == "return") {
                editor.openFile(otherPath);
                QApplication::processEvents(QEventLoop::AllEvents);
                editor.openFile(path);
                QApplication::processEvents(QEventLoop::AllEvents);
                editor.viewport()->repaint();
            } else if (scenario == "return-dirty") {
                keyAndFrame(editor, Qt::Key_X, Qt::NoModifier, QStringLiteral("x"));
                editor.openFile(otherPath);
                QApplication::processEvents(QEventLoop::AllEvents);
                QElapsedTimer back; back.start();
                editor.openFile(path);
                const double t1 = back.nsecsElapsed() / 1e6;
                QApplication::processEvents(QEventLoop::AllEvents);
                const double t2 = back.nsecsElapsed() / 1e6;
                editor.viewport()->repaint();
                const double t3 = back.nsecsElapsed() / 1e6;
                std::printf("  openFile %.0f ms, +processEvents %.0f ms, +repaint %.0f ms\n", t1, t2 - t1, t3 - t2);
            } else if (scenario == "reopen-end") {
                // Каретка в конце, заметка с хвостовой пустой строкой: кэш её не
                // берёт, и возврат идёт полной пересборкой с кареткой в конце.
                keyAndFrame(editor, Qt::Key_End, Qt::ControlModifier);
                keyAndFrame(editor, Qt::Key_Return);
                editor.openFile(otherPath);
                QApplication::processEvents(QEventLoop::AllEvents);
                QElapsedTimer back; back.start();
                editor.openFile(path);
                const double t1 = back.nsecsElapsed() / 1e6;
                QApplication::processEvents(QEventLoop::AllEvents);
                editor.viewport()->repaint();
                std::printf("  openFile %.0f ms, всего %.0f ms\n", t1, back.nsecsElapsed() / 1e6);
                std::fflush(stdout);
            } else if (scenario == "leave") {
                editor.openFile(otherPath);
                QApplication::processEvents(QEventLoop::AllEvents);
                editor.openFile(path);
                QApplication::processEvents(QEventLoop::AllEvents);
            } else if (scenario == "search") {
                editor.findMatches(QStringLiteral("the"), false);
                editor.clearMatches();
            } else if (scenario == "open") {
                zametti::NoteEditor fresh;
                fresh.resize(1000, 700);
                fresh.show();
                fresh.openFile(path);
                QApplication::processEvents(QEventLoop::AllEvents);
                fresh.viewport()->repaint();
            } else {
                std::printf("неизвестный сценарий: %s\n", scenario.c_str());
                return 2;
            }
        }
        std::printf("итого %.0f мс, %.1f мс за повтор\n", double(total.elapsed()),
                    double(total.elapsed()) / double(repeat));
        return 0;
    }

    const double fractions[] = {0.1, 0.5, 1.0};
    std::vector<Row> rows;
    auto put = [&](const std::string& what, size_t col, double v) {
        for (Row& r : rows)
            if (r.what == what) { r.values.resize(3, 0.0); r.values[col] = v; return; }
        Row r; r.what = what; r.values.assign(3, 0.0); r.values[col] = v; rows.push_back(r);
    };

    std::printf("\nБОЛЬШАЯ ЗАМЕТКА: %s (%zu КБ)\n", argv[1], source.size() / 1024);
    std::vector<int> blocksOf(3, 0);
    std::vector<size_t> bytesOf(3, 0);

    for (size_t col = 0; col < 3; ++col) {
        const std::string part = headOf(source, fractions[col]);
        bytesOf[col] = part.size();
        const QString path = dir + QStringLiteral("/part%1.md").arg(int(col));
        {
            QFile f(path);
            if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
                f.write(QByteArray(part.data(), qsizetype(part.size())));
        }

        zametti::NoteEditor editor;
        editor.resize(1000, 700);
        editor.show();
        QTest::qWait(20);

        put("открыть заметку", col, micros([&] {
            editor.openFile(path);
            QApplication::processEvents(QEventLoop::AllEvents);
            editor.viewport()->repaint();
        }));
        blocksOf[col] = editor.document()->blockCount();
        QTest::qWait(50);

        // Каретка в конец.
        put("Ctrl+End (в конец)", col, keyAndFrame(editor, Qt::Key_End, Qt::ControlModifier));
        QTest::qWait(50);

        // Кадр без изменений: прокрутка на строку у конца.
        put("кадр у конца (repaint)", col, micros([&] { editor.viewport()->repaint(); }));

        // Ход каретки без выделения.
        {
            double sum = 0;
            for (int i = 0; i < 10; ++i) sum += keyAndFrame(editor, Qt::Key_Left);
            put("Left ×1 у конца", col, sum / 10.0);
        }
        // Выделение — то, что виснет у владельца.
        {
            double sum = 0;
            for (int i = 0; i < 10; ++i) sum += keyAndFrame(editor, Qt::Key_Left, Qt::ShiftModifier);
            put("Shift+Left ×1 у конца", col, sum / 10.0);
        }
        {
            double sum = 0;
            for (int i = 0; i < 5; ++i) sum += keyAndFrame(editor, Qt::Key_Up, Qt::ShiftModifier);
            put("Shift+Up ×1 у конца", col, sum / 5.0);
        }
        // Мышью: тащим выделение на несколько строк — как QTextEdit сам делает.
        put("выделение мышью (drag 3 строки)", col, micros([&] {
            QTextCursor c = editor.textCursor();
            c.clearSelection();
            editor.setTextCursor(c);
            const QRect r = editor.cursorRect();
            const QPoint from = editor.viewport()->mapToGlobal(r.center());
            QTest::mousePress(editor.viewport(), Qt::LeftButton, Qt::NoModifier, r.center());
            for (int i = 1; i <= 3; ++i) {
                QMouseEvent move(QEvent::MouseMove, QPointF(r.center() - QPoint(0, 20 * i)),
                                 QPointF(from), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
                QApplication::sendEvent(editor.viewport(), &move);
                QApplication::processEvents(QEventLoop::AllEvents);
                editor.viewport()->repaint();
            }
            QTest::mouseRelease(editor.viewport(), Qt::LeftButton, Qt::NoModifier,
                                r.center() - QPoint(0, 60));
            QApplication::processEvents(QEventLoop::AllEvents);
        }));
        // Снять выделение.
        keyAndFrame(editor, Qt::Key_End, Qt::ControlModifier);

        // Ctrl+C маленького выделения у конца.
        {
            for (int i = 0; i < 5; ++i) key(editor, Qt::Key_Left, Qt::ShiftModifier);
            QApplication::processEvents(QEventLoop::AllEvents);
            put("Ctrl+C (5 знаков у конца)", col, keyAndFrame(editor, Qt::Key_C, Qt::ControlModifier));
            keyAndFrame(editor, Qt::Key_End, Qt::ControlModifier);
        }

        // Набор в конце.
        {
            double sum = 0;
            for (int i = 0; i < 10; ++i)
                sum += keyAndFrame(editor, Qt::Key_X, Qt::NoModifier, QStringLiteral("x"));
            put("набор ×1 у конца", col, sum / 10.0);
        }
        // Enter в конце.
        put("Enter у конца", col, keyAndFrame(editor, Qt::Key_Return));

        // Ctrl+A и Ctrl+C всего.
        put("Ctrl+A", col, keyAndFrame(editor, Qt::Key_A, Qt::ControlModifier));
        put("Ctrl+C после Ctrl+A", col, keyAndFrame(editor, Qt::Key_C, Qt::ControlModifier));
        keyAndFrame(editor, Qt::Key_End, Qt::ControlModifier);

        // В начало и Shift+Ctrl+End.
        keyAndFrame(editor, Qt::Key_Home, Qt::ControlModifier);
        put("Shift+Ctrl+End из начала", col,
            keyAndFrame(editor, Qt::Key_End, Qt::ControlModifier | Qt::ShiftModifier));
        keyAndFrame(editor, Qt::Key_Home, Qt::ControlModifier);

        // Прокрутка: кадр в середине.
        put("прокрутка в середину + кадр", col, micros([&] {
            QScrollBar* sb = editor.verticalScrollBar();
            sb->setValue(sb->maximum() / 2);
            QApplication::processEvents(QEventLoop::AllEvents);
            editor.viewport()->repaint();
        }));

        // Поиск.
        put("поиск слова", col, micros([&] { editor.findMatches(QStringLiteral("the"), false); }));
        editor.clearMatches();

        // Переключиться на другую заметку и вернуться.
        put("уйти на другую заметку", col, micros([&] {
            editor.openFile(otherPath);
            QApplication::processEvents(QEventLoop::AllEvents);
        }));
        put("вернуться (из кэша)", col, micros([&] {
            editor.openFile(path);
            QApplication::processEvents(QEventLoop::AllEvents);
            editor.viewport()->repaint();
        }));
        QTest::qWait(20);
    }

    std::printf("   %-34s %14s %14s %14s\n", "", "десятая", "половина", "целиком");
    std::printf("   %-34s %14zu %14zu %14zu   КБ\n", "размер", bytesOf[0] / 1024, bytesOf[1] / 1024,
                bytesOf[2] / 1024);
    std::printf("   %-34s %14d %14d %14d   блоков\n", "", blocksOf[0], blocksOf[1], blocksOf[2]);
    for (const Row& r : rows) {
        std::printf("   %-34s %14.0f %14.0f %14.0f   мкс", r.what.c_str(), r.values[0], r.values[1],
                    r.values[2]);
        const double ratio = r.values[0] > 0 ? r.values[2] / r.values[0] : 0.0;
        if (ratio > 50) std::printf("   ×%.0f  КВАДРАТ?", ratio);
        else if (ratio > 4) std::printf("   ×%.0f  ЛИНЕЙНО", ratio);
        else std::printf("   ×%.1f", ratio);
        std::printf("\n");
    }
    std::printf("\n");
    return 0;
}
