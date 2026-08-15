// СТОИТ ЛИ ВСТАВКА ОДНОГО И ТОГО ЖЕ НА ЗАМЕТКЕ ЛЮБОГО РАЗМЕРА.
//
// Главное правило проекта: цена нажатия клавиши зависит от объёма ПОКАЗАННОГО,
// а не от размера заметки. До базиса вставка его нарушала — кончалась обходом
// всего документа (piecesOf) и заплаткой поверх. Теперь заметка приводит к
// канону только шов, и это утверждение здесь и меряется.
//
// Стенд, а не набор: у него ответ — число, на которое надо смотреть.

#include "document.h"

#include <QElapsedTimer>
#include <QTextCursor>

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
    return 0;
}
