// Снимки таблиц: то, что владелец проверяет глазами.
#include "editor_widget.h"
#include "note_view.h"
#include "settings.h"
#include "test_util.h"

#include <QApplication>
#include <QDir>
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

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    g_dir = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QDir::tempPath();
    QDir().mkpath(g_dir);
    shoot(QStringLiteral("таблица-широкое"), 1000, 700, kNote);
    shoot(QStringLiteral("таблица-узкое"), 620, 700, kNote);
    std::printf("снимки: %s\n", qPrintable(g_dir));
    return zt::report("снимки таблиц");
}
