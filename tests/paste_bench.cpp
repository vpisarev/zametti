// СТОИТ ЛИ ВСТАВКА ОДНОГО И ТОГО ЖЕ НА ЗАМЕТКЕ ЛЮБОГО РАЗМЕРА.
//
// Главное правило проекта: цена нажатия клавиши зависит от объёма ПОКАЗАННОГО,
// а не от размера заметки. До базиса вставка его нарушала — кончалась обходом
// всего документа (piecesOf) и заплаткой поверх. Теперь заметка приводит к
// канону только шов, и это утверждение здесь и меряется.
//
// Стенд, а не набор: у него ответ — число, на которое надо смотреть.

#include "document.h"
#include "editor_widget.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QKeyEvent>
#include <QTest>

#include <QElapsedTimer>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace {

// Заметка из блоков разного рода: обычные абзацы, список, блок кода. Так шов
// попадает в осмысленное окружение, а не в однородную простыню.
std::string noteOf(int blocks) {
    std::string out = "# Заметка\n\n";
    for (int i = 0; i < blocks; ++i) {
        const int kind = i % 8;
        if (kind == 3) out += "- пункт списка номер " + std::to_string(i) + "\n";
        else if (kind == 6) out += "```\nx = " + std::to_string(i) + "\n```\n\n";
        else
            out += "Абзац номер " + std::to_string(i) +
                   ", в нём достаточно слов, чтобы вёрстка была не пустой.\n\n";
    }
    return out;
}

}  // namespace

int ztPasteBench(int argc, char** argv) {
    (void)argc;
    (void)argv;

    std::printf("\nЦЕНА ВСТАВКИ ПРОТИВ РАЗМЕРА ЗАМЕТКИ\n");
    std::printf("   %-10s %-10s %-12s %-12s\n", "блоков", "КБ", "вставка, мкс", "на блок, нс");

    for (const int blocks : {50, 500, 5000}) {
        const std::string source = noteOf(blocks);
        zametti::ZDocument note;
        note.loadMarkdown(source);

        // Вставляем В СЕРЕДИНУ: там у шва соседи с обеих сторон, и работа для
        // приведения к канону настоящая.
        double best = 1e18;
        for (int run = 0; run < 7; ++run) {
            zametti::ZDocument fresh;
            fresh.loadMarkdown(source);
            QTextCursor at = fresh.caretAtBlock(fresh.blockCount() / 2);

            QElapsedTimer timer;
            timer.start();
            fresh.replaceRange(at, QStringLiteral("вставленный кусок\n"));
            best = std::min(best, double(timer.nsecsElapsed()) / 1000.0);
        }
        std::printf("   %-10d %-10.0f %-12.1f %-12.1f\n", blocks, double(source.size()) / 1024.0,
                    best, best * 1000.0 / blocks);
    }
    std::printf("   «на блок» обязано ПАДАТЬ: если вставка стоит одного и того же,\n"
                "   на большой заметке доля каждого блока меньше.\n\n");

    // --- А ТЕПЕРЬ ТО, ЧТО ЕЩЁ НЕ НА БАЗИСЕ ----------------------------------
    //
    // Enter и Backspace идут прежним путём: операция правит документ, а следом
    // редактор обходит его ЦЕЛИКОМ (piecesOf) и накладывает заплатку. Здесь
    // видно, чего это стоит и что даст перевод на базис.
    std::printf("ЦЕНА Enter ПРЕЖНИМ ПУТЁМ (операция + обход всего документа)\n");
    std::printf("   %-10s %-10s %-12s %-12s\n", "блоков", "КБ", "Enter, мкс", "на блок, нс");

    const QString dir = QStringLiteral("/tmp/zametti-paste-bench");
    QDir(dir).removeRecursively();
    QDir().mkpath(dir);
    for (const int blocks : {50, 500, 5000}) {
        const std::string source = noteOf(blocks);
        const QString path = dir + QStringLiteral("/n%1.md").arg(blocks);
        {
            QFile file(path);
            if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) continue;
            file.write(QByteArray(source.data(), qsizetype(source.size())));
        }

        zametti::NoteEditor editor;
        editor.resize(800, 600);
        editor.show();
        QTest::qWait(20);
        editor.openFile(path);
        QTest::qWait(30);

        double best = 1e18;
        for (int run = 0; run < 7; ++run) {
            QTextCursor at(editor.document());
            at.setPosition(
                editor.document()->findBlockByNumber(editor.document()->blockCount() / 2).position());
            editor.setTextCursor(at);
            QKeyEvent press(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
            QElapsedTimer timer;
            timer.start();
            QApplication::sendEvent(&editor, &press);
            best = std::min(best, double(timer.nsecsElapsed()) / 1000.0);
        }
        std::printf("   %-10d %-10.0f %-12.1f %-12.1f\n", blocks, double(source.size()) / 1024.0,
                    best, best * 1000.0 / blocks);
    }
    std::printf("   Здесь «на блок» обязано СТОЯТЬ: цена растёт вместе с заметкой.\n\n");
    return 0;
}
