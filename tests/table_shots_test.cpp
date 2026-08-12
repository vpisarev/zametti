// Снимки таблиц: то, что владелец проверяет глазами.
#include "editor_widget.h"
#include "note_view.h"
#include "settings.h"
#include "test_util.h"

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QImage>
#include <QTest>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>

namespace {
QString g_dir;

const char* kNote = R"(# Таблицы

Обычный абзац перед таблицей.

| деталь | цена | наличие |
|:---|---:|:---:|
| болт | 10 | да |
| гайка с длинным именем | 1000 | нет |
| шайба | 5 | да |

Абзац между таблицами.

| что | зачем |
|---|---|
| **жир** | видно ли |
| `код` | и он тоже |
| [ссылка](https://example.com) | цветом |

| колонка | очень длинное описание |
|---|---|
| раз | очень длинное описание детали, которое ни в какую колонку целиком не поместится и обязано перенестись по словам, а не раздуть таблицу |
| два | коротко |

Хвост заметки.
)";

// В КАЖДОЙ КОЛОНКЕ ЧТО-ТО НАРИСОВАНО.
//
// Проверка появилась после того, как моя же оптимизация раскладки унесла текст
// колонок с выравниванием вправо и по центру на километр за экран: ширину
// текста я спрашивал у boundingRect() раскладки, а там стояла ширина строки —
// бесконечная, потому что при измерении ячейку кладут в бесконечную ширину.
// Поймал снимок, глазами. Теперь ловит набор.
void checkEveryColumnDrawn(zametti::NoteEditor& editor, const QImage& shot) {
    const zametti::TableRender* table = nullptr;
    for (QTextBlock b = editor.document()->firstBlock(); b.isValid(); b = b.next()) {
        const zametti::TableRender* found = editor.tableAt(b.blockNumber());
        if (found != nullptr) { table = found; break; }
    }
    if (table == nullptr) {
        ++zt::g_failures;
        std::printf("провал: ни одной таблицы не показано сеткой\n");
        return;
    }

    const QRectF area = editor.tableRect(table->first);
    const int scroll = editor.verticalScrollBar()->value();

    // Считаем тёмные точки ВНУТРИ РЯДОВ, отступя от их границ. Первая редакция
    // считала по всей высоте колонки — и находила линии сетки, которые идут
    // через все колонки насквозь: проверка оставалась зелёной при пустых
    // колонках. Пустышка страшнее отсутствия проверки (правило проекта).
    qreal x = area.left();
    for (int column = 0; column < table->layout.columns; ++column) {
        const qreal width = table->layout.columnWidth.at(column);
        int dark = 0;
        qreal y = area.top();
        for (int row = 0; row < table->layout.rows; ++row) {
            const qreal height = table->layout.rowHeight.at(row);
            for (int px = int(x) + 2; px < int(x + width) - 2 && px < shot.width(); ++px)
                for (int py = int(y - scroll) + 4; py < int(y + height - scroll) - 4 &&
                                                  py < shot.height(); ++py)
                    if (py >= 0 && qGray(shot.pixel(px, py)) < 128) ++dark;
            y += height;
        }
        ++zt::g_checks;
        if (dark == 0) {
            ++zt::g_failures;
            std::printf("провал: в колонке %d ничего не нарисовано\n", column);
        }
        x += width;
    }
}

void shoot(const QString& name, int width, int height, const char* text) {
    const QString path = QDir(g_dir).filePath(name + QStringLiteral(".md"));
    QFile file(path);
    if (file.open(QIODevice::WriteOnly)) file.write(text);
    file.close();

    zametti::NoteEditor editor;
    editor.resize(width, height);
    editor.show();
    QTest::qWait(20);
    editor.openFile(path);
    QTest::qWait(80);
    QTextCursor at = editor.textCursor();
    at.setPosition(0);
    editor.setTextCursor(at);
    QTest::qWait(60);
    const QImage shot = editor.grab().toImage();
    if (!shot.save(QDir(g_dir).filePath(name + QStringLiteral(".png"))))
        std::printf("НЕ СОХРАНИЛСЯ снимок %s\n", qPrintable(name));
    checkEveryColumnDrawn(editor, shot);
}
}  // namespace

// Флип: Enter на таблице показывает исходник, уход каретки — снова сетку.
// Клавиши идут через слой объекта, но проводка к редактору своя, и без этой
// проверки она держалась бы только на моём слове.
void checkFlip() {
    const QString path = QDir(g_dir).filePath(QStringLiteral("флип.md"));
    QFile file(path);
    if (file.open(QIODevice::WriteOnly))
        file.write("до\n\n| a | b |\n|---|---|\n| 1 | 2 |\n\nпосле\n");
    file.close();

    zametti::NoteEditor editor;
    editor.resize(900, 700);
    editor.show();
    QTest::qWait(20);
    editor.openFile(path);
    QTest::qWait(60);

    const auto tableFirst = [&editor] {
        for (QTextBlock b = editor.document()->firstBlock(); b.isValid(); b = b.next())
            if (editor.tableAt(b.blockNumber()) != nullptr) return b.blockNumber();
        return -1;
    };
    const int first = tableFirst();
    ++zt::g_checks;
    if (first < 0) {
        ++zt::g_failures;
        std::printf("провал: таблица не показана сеткой\n");
        return;
    }

    // Строки исходника спрятаны — кроме последней, на которой висит резерв.
    int hidden = 0;
    for (QTextBlock b = editor.document()->firstBlock(); b.isValid(); b = b.next())
        if (!b.isVisible()) ++hidden;
    ++zt::g_checks;
    if (hidden != 2) {
        ++zt::g_failures;
        std::printf("провал: спрятано строк %d, а не 2\n", hidden);
    }

    // Каретка на таблицу — и Enter показывает исходник.
    QTextCursor at(editor.document()->findBlockByNumber(first + 2));
    editor.setTextCursor(at);
    QTest::qWait(20);
    QTest::keyClick(&editor, Qt::Key_Return);
    QTest::qWait(40);
    ++zt::g_checks;
    if (editor.editedTable() != first) {
        ++zt::g_failures;
        std::printf("провал: Enter не открыл исходник (правится %d, ждали %d)\n",
                    editor.editedTable(), first);
    }
    int visibleNow = 0;
    for (QTextBlock b = editor.document()->firstBlock(); b.isValid(); b = b.next())
        if (!b.isVisible()) ++visibleNow;
    ++zt::g_checks;
    if (visibleNow != 0) {
        ++zt::g_failures;
        std::printf("провал: в правке остались спрятанные строки (%d)\n", visibleNow);
    }

    // Увели каретку наружу — снова сетка.
    editor.setTextCursor(QTextCursor(editor.document()->firstBlock()));
    QTest::qWait(60);
    ++zt::g_checks;
    if (editor.editedTable() != -1) {
        ++zt::g_failures;
        std::printf("провал: уход каретки не вернул сетку\n");
    }
    ++zt::g_checks;
    if (editor.tableAt(first) == nullptr) {
        ++zt::g_failures;
        std::printf("провал: сетка не вернулась\n");
    }
}

// ИНВАРИАНТ A ИЗ БРИФА: показ не меняет файл ни на байт.
//
// Открываем копии настоящих заметок владельца, даём виду их отрисовать, водим
// кареткой по таблице и сравниваем байты. Показ — чистое чтение, и это
// единственный способ убедиться в этом, а не понадеяться.
void checkFilesUntouched(const QStringList& sources) {
    for (const QString& source : sources) {
        QFile in(source);
        if (!in.open(QIODevice::ReadOnly)) continue;
        const QByteArray before = in.readAll();
        in.close();

        const QString copy = QDir(g_dir).filePath(QFileInfo(source).fileName());
        QFile out(copy);
        if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) continue;
        out.write(before);
        out.close();

        zametti::NoteEditor editor;
        editor.resize(900, 700);
        editor.show();
        QTest::qWait(20);
        editor.openFile(copy);
        QTest::qWait(80);

        // Ходим кареткой по всему документу: рендер, флип, снимок мест —
        // всё это не имеет права тронуть файл.
        for (int i = 0; i < 40; ++i) {
            QTest::keyClick(&editor, Qt::Key_Down);
            QTest::qWait(2);
        }
        for (int i = 0; i < 10; ++i) {
            QTest::keyClick(&editor, Qt::Key_Up);
            QTest::qWait(2);
        }
        QTest::qWait(50);

        // Снимок живой заметки — на него смотрит владелец.
        QTextCursor top = editor.textCursor();
        top.setPosition(0);
        editor.setTextCursor(top);
        QTest::qWait(60);
        const QImage live = editor.grab().toImage();
        live.save(QDir(g_dir).filePath(QFileInfo(source).completeBaseName() +
                                       QStringLiteral(".png")));

        QFile after(copy);
        ++zt::g_checks;
        if (!after.open(QIODevice::ReadOnly)) {
            ++zt::g_failures;
            std::printf("провал: копия %s не читается\n", qPrintable(copy));
            continue;
        }
        const QByteArray now = after.readAll();
        after.close();
        if (now == before) continue;
        ++zt::g_failures;
        std::printf("провал: файл изменился при показе — %s (было %lld байт, стало %lld)\n",
                    qPrintable(QFileInfo(source).fileName()), (long long)before.size(),
                    (long long)now.size());
    }
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    g_dir = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QDir::tempPath();
    QDir().mkpath(g_dir);
    shoot(QStringLiteral("таблица-широкое"), 1000, 700, kNote);
    shoot(QStringLiteral("таблица-узкое"), 620, 700, kNote);
    checkFlip();

    QStringList sources;
    for (int i = 2; i < argc; ++i) sources << QString::fromLocal8Bit(argv[i]);
    if (!sources.isEmpty()) checkFilesUntouched(sources);

    std::printf("снимки: %s\n", qPrintable(g_dir));
    return zt::report("снимки таблиц");
}
