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

#include "scratch_files.h"
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QKeyEvent>
#include <QTest>

#include <QElapsedTimer>
#include <QTextBlock>
#include <QTextCursor>
#include <QPlainTextEdit>
#include <QTextEdit>
#include <QTextDocument>

#include <algorithm>
#include <cstdio>
#include <string>
#include <utility>
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
    // Enter идёт через глагол заметки: правит локально и приводит к канону
    // только шов. До перевода он платил обходом ВСЕГО документа и заплаткой
    // поверх — 344 / 1488 / 12729 мкс на тех же трёх размерах.
    std::printf("ЦЕНА Enter (через глагол заметки)\n");
    std::printf("   %-10s %-10s %-12s %-12s\n", "блоков", "КБ", "Enter, мкс", "на блок, нс");

    const QString dir = QStringLiteral("/tmp/zametti-paste-bench");
    zt::dropTree(dir);
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

    // --- ЦЕНА ОБЫЧНОГО НАБОРА -----------------------------------------------
    //
    // Фраза без единой автозамены. Меряется путь целиком: нажатие → insertTyped
    // → вставка знака → contentsChanged → починка инвариантов и подметание. Это
    // самая частая работа программы, и её цена важнее всех прочих.
    //
    // Два случая нарочно. «Одним абзацем» — вырожденный: Qt переразмечает
    // ЦЕЛИКОМ тот блок, в который пишут, и цена растёт вместе с длиной абзаца.
    // «Абзацами» — как пишут люди, и вот это настоящее число.
    std::printf("ЦЕНА ОБЫЧНОГО НАБОРА\n");
    std::printf("   %-16s %-10s %-14s %-14s %-14s\n", "как набирают", "знаков", "всё, мкс/знак",
                "чистый Qt", "наше поверх");

    const QString phrase = QStringLiteral("the quick brown fox jumps over a lazy dog ");
    for (const bool paragraphs : {false, true}) {
        const QString path = dir + (paragraphs ? QStringLiteral("/набор-абзацами.md")
                                              : QStringLiteral("/набор-одним.md"));
        {
            QFile file(path);
            if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) file.write("# Набор\n\n");
        }
        // Сколько раз повторить фразу. Вырожденный случай квадратичен, и тысяча
        // повторов в нём — две с половиной минуты; берём меньше, характер и так
        // виден.
        const int times = paragraphs ? 1000 : 100;

        // 1. ПУТЬ ЦЕЛИКОМ — настоящими нажатиями.
        double whole = 0.0;
        qint64 chars = 0;
        int blocks = 0;
        {
            zametti::NoteEditor editor;
            editor.resize(800, 600);
            editor.show();
            QTest::qWait(20);
            editor.openFile(path);
            QTest::qWait(30);
            QTextCursor at(editor.document());
            at.movePosition(QTextCursor::End);
            editor.setTextCursor(at);

            QElapsedTimer timer;
            timer.start();
            for (int i = 0; i < times; ++i) {
                for (const QChar ch : phrase) {
                    QKeyEvent press(QEvent::KeyPress, Qt::Key_unknown, Qt::NoModifier, QString(ch));
                    QApplication::sendEvent(&editor, &press);
                    ++chars;
                }
                if (paragraphs) {
                    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
                    QApplication::sendEvent(&editor, &enter);
                    QApplication::sendEvent(&editor, &enter);
                }
            }
            whole = double(timer.nsecsElapsed()) / 1000.0;
            blocks = editor.document()->blockCount();
        }

        // 2. ЧИСТЫЙ Qt — тот же текст, но вставкой прямо в документ, без нашей
        // обвязки. Разница между этим и путём целиком и есть наша доля.
        double bare = 0.0;
        {
            QTextDocument plain;
            QTextCursor at(&plain);
            QElapsedTimer timer;
            timer.start();
            for (int i = 0; i < times; ++i) {
                for (const QChar ch : phrase) at.insertText(QString(ch));
                if (paragraphs) at.insertBlock();
            }
            bare = double(timer.nsecsElapsed()) / 1000.0;
        }

        std::printf("   %-16s %-10lld %-14.1f %-14.1f %-14.1f\n",
                    paragraphs ? "абзацами" : "одним абзацем",
                    static_cast<long long>(chars), whole / double(chars), bare / double(chars),
                    (whole - bare) / double(chars));
        std::printf("       блоков к концу: %d\n", blocks);
    }
    std::printf("   «одним абзацем» — вырожденный случай: Qt переразмечает блок,\n"
                "   в который пишут, ЦЕЛИКОМ, и цена растёт вместе с его длиной.\n\n");

    // РАСТЁТ ЛИ ЦЕНА НАЖАТИЯ ВМЕСТЕ С ЗАМЕТКОЙ — главный вопрос правил проекта.
    std::printf("ЦЕНА НАЖАТИЯ ПРОТИВ РАЗМЕРА ЗАМЕТКИ (набор в середину)\n");
    std::printf("   %-10s %-10s %-14s\n", "блоков", "КБ", "мкс/знак");
    for (const int blocks : {50, 500, 5000}) {
        const std::string source = noteOf(blocks);
        const QString path = dir + QStringLiteral("/набор%1.md").arg(blocks);
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

        QTextCursor at(editor.document());
        at.setPosition(editor.document()
                           ->findBlockByNumber(editor.document()->blockCount() / 2)
                           .position());
        editor.setTextCursor(at);

        // Лучшее из трёх заходов: разброс на этой машине доходит до пятой части,
        // и одиночный замер сравнивать не с чем.
        // КАК ПИШУТ ЛЮДИ: фразы, а каждые десять — Enter. Без него набор
        // выродился бы в один растущий абзац, а это отдельная беда (Qt
        // переразмечает блок, в который пишут, целиком) и мерить надо не её.
        double each = 1e18;
        for (int run = 0; run < 3; ++run) {
            qint64 chars = 0;
            QElapsedTimer timer;
            timer.start();
            for (int i = 0; i < 20; ++i) {
                for (const QChar ch : phrase) {
                    QKeyEvent press(QEvent::KeyPress, Qt::Key_unknown, Qt::NoModifier,
                                    QString(ch));
                    QApplication::sendEvent(&editor, &press);
                    ++chars;
                }
                if ((i + 1) % 10 == 0) {
                    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
                    QApplication::sendEvent(&editor, &enter);
                }
            }
            each = std::min(each, double(timer.nsecsElapsed()) / 1000.0 / double(chars));
        }
        std::printf("   %-10d %-10.0f %-14.1f\n", blocks, double(source.size()) / 1024.0, each);
    }
    std::printf("   Обязано СТОЯТЬ: цена нажатия не должна зависеть от размера заметки.\n\n");

    // А СКОЛЬКО ИЗ ЭТОГО — САМ Qt.
    //
    // Сравнивать надо с ЖИВЫМ виджетом, а не с документом без вёрстки: у того
    // вёрстка выключена вовсе, и он «дёшев» ровно потому, что ничего не считает.
    // Здесь обычный QTextEdit с тем же текстом, тех же размеров и тоже
    // показанный: разница между ним и нашим редактором и есть наша доля.
    std::printf("ТО ЖЕ, НО В ОБЫЧНОМ QTextEdit (Qt со своей вёрсткой)\n");
    std::printf("   %-10s %-10s %-14s\n", "блоков", "КБ", "мкс/знак");
    for (const int blocks : {50, 500, 5000}) {
        const std::string source = noteOf(blocks);
        QTextEdit plain;
        plain.setPlainText(QString::fromUtf8(source.data(), qsizetype(source.size())));
        plain.resize(800, 600);
        plain.show();
        QTest::qWait(30);

        QTextCursor at(plain.document());
        at.setPosition(
            plain.document()->findBlockByNumber(plain.document()->blockCount() / 2).position());
        plain.setTextCursor(at);

        double each = 1e18;
        for (int run = 0; run < 3; ++run) {
            qint64 chars = 0;
            QElapsedTimer timer;
            timer.start();
            for (int i = 0; i < 20; ++i) {
                for (const QChar ch : phrase) {
                    QKeyEvent press(QEvent::KeyPress, Qt::Key_unknown, Qt::NoModifier,
                                    QString(ch));
                    QApplication::sendEvent(&plain, &press);
                    ++chars;
                }
                if ((i + 1) % 10 == 0) {
                    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
                    QApplication::sendEvent(&plain, &enter);
                }
            }
            each = std::min(each, double(timer.nsecsElapsed()) / 1000.0 / double(chars));
        }
        std::printf("   %-10d %-10.0f %-14.1f\n", blocks, double(source.size()) / 1024.0, each);
    }


    // И ТО ЖЕ В QPlainTextEdit (мысль владельца). У него ДРУГАЯ ВЁРСТКА —
    // QPlainTextDocumentLayout, писанная под большие простые документы: она
    // считает высоту лениво и не перекладывает весь документ на каждую правку.
    // Если рост живёт именно там, здесь его быть не должно.
    std::printf("И ТО ЖЕ В QPlainTextEdit (ленивая вёрстка Qt)\n");
    std::printf("   %-10s %-10s %-14s\n", "блоков", "КБ", "мкс/знак");
    for (const int blocks : {50, 500, 5000}) {
        const std::string source = noteOf(blocks);
        QPlainTextEdit plain;
        plain.setPlainText(QString::fromUtf8(source.data(), qsizetype(source.size())));
        plain.resize(800, 600);
        plain.show();
        QTest::qWait(30);

        QTextCursor at(plain.document());
        at.setPosition(
            plain.document()->findBlockByNumber(plain.document()->blockCount() / 2).position());
        plain.setTextCursor(at);

        double each = 1e18;
        for (int run = 0; run < 3; ++run) {
            qint64 chars = 0;
            QElapsedTimer timer;
            timer.start();
            for (int i = 0; i < 20; ++i) {
                for (const QChar ch : phrase) {
                    QKeyEvent press(QEvent::KeyPress, Qt::Key_unknown, Qt::NoModifier,
                                    QString(ch));
                    QApplication::sendEvent(&plain, &press);
                    ++chars;
                }
                if ((i + 1) % 10 == 0) {
                    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
                    QApplication::sendEvent(&plain, &enter);
                }
            }
            each = std::min(each, double(timer.nsecsElapsed()) / 1000.0 / double(chars));
        }
        std::printf("   %-10d %-10.0f %-14.1f\n", blocks, double(source.size()) / 1024.0, each);
    }

    // ГДЕ ИМЕННО ПРАВИШЬ — ВАЖНО ЛИ ЭТО.
    //
    // Вопрос решающий для выбора лечения. Если цена растёт с числом блоков
    // НИЖЕ места правки, значит Qt разносит геометрию по всему хвосту, и
    // лечится это индексом высот (docs/zametti-fast-layout.md). Если цена
    // одинакова везде — дело в другом, и своя вёрстка не поможет.
    {
        const std::string source = noteOf(5000);
        std::printf("ЗАВИСИТ ЛИ ЦЕНА ОТ МЕСТА ПРАВКИ (QTextEdit, 490 КБ, 5000 блоков)\n");
        std::printf("   %-22s %-14s\n", "где правим", "мкс/знак");
        for (const auto& where : {std::pair<const char*, double>{"в самом начале", 0.0},
                                  {"в середине", 0.5},
                                  {"в самом конце", 0.999}}) {
            QTextEdit plain;
            plain.setPlainText(QString::fromUtf8(source.data(), qsizetype(source.size())));
            plain.resize(800, 600);
            plain.show();
            QTest::qWait(30);

            const int block = int(where.second * (plain.document()->blockCount() - 1));
            QTextCursor at(plain.document());
            at.setPosition(plain.document()->findBlockByNumber(block).position());
            plain.setTextCursor(at);

            double each = 1e18;
            for (int run = 0; run < 3; ++run) {
                qint64 chars = 0;
                QElapsedTimer timer;
                timer.start();
                for (int i = 0; i < 10; ++i)
                    for (const QChar ch : phrase) {
                        QKeyEvent press(QEvent::KeyPress, Qt::Key_unknown, Qt::NoModifier,
                                        QString(ch));
                        QApplication::sendEvent(&plain, &press);
                        ++chars;
                    }
                each = std::min(each, double(timer.nsecsElapsed()) / 1000.0 / double(chars));
            }
            std::printf("   %-22s %-14.1f\n", where.first, each);
        }
        std::printf("   Падение к концу означает: платим за блоки НИЖЕ правки.\n\n");
    }

    // САМЫЙ ДЕШЁВЫЙ ПРОБНИК ПЕРЕД СВОЕЙ ВЁРСТКОЙ: подсунуть НАШЕМУ документу,
    // со всеми его форматами, штатную ленивую вёрстку. Рисовать она будет не то,
    // что нам нужно, — но ответить на вопрос «сколько стоит правка, если
    // геометрия не разносится по хвосту» она может уже сейчас.
    {
        const std::string source = noteOf(5000);
        std::printf("НАШ ДОКУМЕНТ ПОД ЛЕНИВОЙ ВЁРСТКОЙ (QPlainTextDocumentLayout)\n");
        zametti::ZDocument note;
        note.loadMarkdown(source);

        QPlainTextEdit host;
        host.resize(800, 600);
        // Документ отдаём тот самый, который собрал наш сборщик, — со ступенями
        // кеглей, полями блоков и всеми свойствами.
        QTextDocument* doc = note.getDocument();
        doc->setDocumentLayout(new QPlainTextDocumentLayout(doc));
        host.setDocument(doc);
        host.show();
        QTest::qWait(30);

        QTextCursor at(doc);
        at.setPosition(doc->findBlockByNumber(doc->blockCount() / 2).position());
        host.setTextCursor(at);

        double each = 1e18;
        for (int run = 0; run < 3; ++run) {
            qint64 chars = 0;
            QElapsedTimer timer;
            timer.start();
            for (int i = 0; i < 10; ++i)
                for (const QChar ch : phrase) {
                    QKeyEvent press(QEvent::KeyPress, Qt::Key_unknown, Qt::NoModifier,
                                    QString(ch));
                    QApplication::sendEvent(&host, &press);
                    ++chars;
                }
            each = std::min(each, double(timer.nsecsElapsed()) / 1000.0 / double(chars));
        }
        std::printf("   в середине 490 КБ: %.1f мкс/знак\n", each);
        std::printf("   для сравнения: та же правка под QTextDocumentLayout — 3308 мкс\n");
        host.setDocument(nullptr);
    }

    std::printf("\n");
    return 0;
}
