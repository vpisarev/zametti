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
#include <QTextCursor>

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

Хвост заметки.
)";

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
}
}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    g_dir = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QDir::tempPath();
    QDir().mkpath(g_dir);
    shoot(QStringLiteral("таблица-широкое"), 1000, 700, kNote);
    shoot(QStringLiteral("таблица-узкое"), 620, 700, kNote);
    std::printf("снимки: %s\n", qPrintable(g_dir));
    return 0;
}
