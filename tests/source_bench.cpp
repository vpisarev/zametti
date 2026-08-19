// ЦЕНА ВЫХОДА ИЗ РЕЖИМА ПРАВКИ ИСХОДНИКА: сколько стоит наложить правленый
// текст на заметку — и от чего это зависит.
//
// Стенд, а не набор: у него число, на которое надо смотреть. Мерить есть что —
// наложение честно O(N) от размера заметки, и не однажды: канон нынешнего,
// разбор нового, канон нового, построчное сравнение и сверка на выходе. Это
// законно (предмет операции — вся заметка, и зовут её на явном действии
// человека), но названо вслух должно быть числом, а не словом «недорого».
//
//   zametti-bench source            заметки владельца из .testdata/owner-copy
//   zametti-bench source <файл.md>  названный файл

#include "document.h"

#include "testdata.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>

#include <cstdio>
#include <cstring>
#include <string>

namespace {

using zametti::ZDocument;

struct Row {
    QString name;
    qint64 bytes = 0;
    int blocks = 0;
    double nothing = 0.0;   // наложить свой же канон: «ничего не менялось»
    double oneWord = 0.0;   // одно слово в середине
    double whole = 0.0;     // всё другое
    int hunks = 0;
};

double msOf(ZDocument& doc, const QString& text, int* hunks = nullptr) {
    QElapsedTimer clock;
    clock.start();
    const int applied = doc.applySourceText(text);
    const double ms = double(clock.nsecsElapsed()) / 1e6;
    if (hunks != nullptr) *hunks = applied;
    return ms;
}

Row measure(const QString& path) {
    Row row;
    row.name = QFileInfo(path).fileName();
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return row;
    const QByteArray bytes = file.readAll();
    row.bytes = bytes.size();

    ZDocument doc;
    doc.loadMarkdown(std::string(bytes.constData(), size_t(bytes.size())));
    doc.setUndoEnabled(true);
    row.blocks = doc.blockCount();

    const QString canonical = doc.toMarkdownText();
    row.nothing = msOf(doc, canonical);

    // Одно слово в середине: правим первую строку, в которой есть буквы.
    QStringList lines = canonical.split(QLatin1Char('\n'));
    int at = lines.size() / 2;
    while (at < lines.size() && lines[at].trimmed().isEmpty()) ++at;
    if (at < lines.size()) lines[at] += QStringLiteral(" ещё");
    row.oneWord = msOf(doc, lines.join(QLatin1Char('\n')), &row.hunks);

    ZDocument again;
    again.loadMarkdown(std::string(bytes.constData(), size_t(bytes.size())));
    again.setUndoEnabled(true);
    row.whole = msOf(again, QStringLiteral("совсем другое содержимое\n"));
    return row;
}

}  // namespace

// СЖАТИЕ СЛУЧАЯ. Фаззер даёт пару файлов «было/стало», на которой наложение
// разошлось с каноном; руками искать в них виноватую строку — это искать иголку.
// Убираем куски из обеих сторон, пока расхождение держится: что осталось, то и
// есть случай для матрицы.
//
//   zametti-bench source --reduce было.md стало.md
int reduceCase(const QString& beforePath, const QString& afterPath) {
    const auto read = [](const QString& path) {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) return QString();
        return QString::fromUtf8(f.readAll());
    };
    const auto stillBroken = [](const QStringList& before, const QStringList& after) {
        ZDocument doc;
        doc.loadMarkdown(before.join(QLatin1Char('\n')).toStdString());
        doc.setUndoEnabled(true);
        return doc.applySourceText(after.join(QLatin1Char('\n'))) == -2;
    };

    QStringList before = read(beforePath).split(QLatin1Char('\n'));
    QStringList after = read(afterPath).split(QLatin1Char('\n'));
    if (!stillBroken(before, after)) {
        std::printf("случай не воспроизводится: наложение не даёт -2\n");
        return 1;
    }
    std::printf("до сжатия: %lld и %lld строк\n", (long long)before.size(),
                (long long)after.size());

    // Грубо к тонкому: сперва половинами, потом по строке.
    for (int chunk = qMax(1, int(before.size()) / 2); chunk >= 1; chunk /= 2) {
        bool moved = true;
        while (moved) {
            moved = false;
            for (int at = 0; at + chunk <= before.size() && at + chunk <= after.size();) {
                QStringList b = before;
                QStringList a = after;
                b.remove(at, chunk);
                a.remove(at, chunk);
                if (!b.isEmpty() && !a.isEmpty() && stillBroken(b, a)) {
                    before = b;
                    after = a;
                    moved = true;
                } else {
                    at += chunk;
                }
            }
        }
        if (chunk == 1) break;
    }
    std::printf("после сжатия: %lld и %lld строк\n\n=== было ===\n%s\n=== стало ===\n%s\n",
                (long long)before.size(), (long long)after.size(),
                before.join(QLatin1Char('\n')).toUtf8().constData(),
                after.join(QLatin1Char('\n')).toUtf8().constData());
    return 0;
}

// ПОКАЗАТЬ СЛУЧАЙ ЦЕЛИКОМ: что было, что должно было выйти и что вышло.
//
//   zametti-bench source --case было.md стало.md
int showCase(const QString& beforePath, const QString& afterPath) {
    const auto read = [](const QString& path) {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) return QString();
        return QString::fromUtf8(f.readAll());
    };
    const QString before = read(beforePath);
    const QString after = read(afterPath);

    ZDocument doc;
    doc.loadMarkdown(before.toStdString());
    doc.setUndoEnabled(true);
    const QString canonBefore = doc.toMarkdownText();

    ZDocument probe;
    probe.loadMarkdown(after.toStdString());
    const QString want = probe.toMarkdownText();

    const int stepsBefore = doc.undoSteps();
    const int hunks = doc.applySourceText(after);
    std::printf("=== канон ДО ===\n%s=== ждали ===\n%s=== кусков: %d, шагов отмены %d -> %d ===\n"
                "=== вышло ===\n%s",
                canonBefore.toUtf8().constData(), want.toUtf8().constData(), hunks, stepsBefore,
                doc.undoSteps(), doc.toMarkdownText().toUtf8().constData());
    // Проба: возврат СРАЗУ после отмены, без единого чтения между ними.
    {
        ZDocument probe2;
        probe2.loadMarkdown(before.toStdString());
        probe2.setUndoEnabled(true);
        probe2.applySourceText(after);
        const bool u = probe2.undo();
        const bool r = probe2.redo();
        std::printf("=== проба: отмена %s, возврат сразу %s ===\n", u ? "да" : "НЕТ",
                    r ? "да" : "НЕТ");
    }
    const bool undone = doc.undo();
    std::printf("=== отмена: %s, шагов %d, вернула ===\n%s", undone ? "да" : "НЕТ",
                doc.undoSteps(), doc.toMarkdownText().toUtf8().constData());
    const bool redone = doc.redo();
    std::printf("=== возврат: %s, вернул ===\n%s", redone ? "да" : "НЕТ",
                doc.toMarkdownText().toUtf8().constData());
    return 0;
}

int ztSourceBench(int argc, char** argv) {
    if (argc >= 4 && std::strcmp(argv[1], "--case") == 0)
        return showCase(QString::fromLocal8Bit(argv[2]), QString::fromLocal8Bit(argv[3]));
    // Доводы приходят так, будто стенд звали напрямую: argv[0] — имя, дальше
    // всё, что стояло после подкоманды.
    if (argc >= 4 && std::strcmp(argv[1], "--reduce") == 0)
        return reduceCase(QString::fromLocal8Bit(argv[2]), QString::fromLocal8Bit(argv[3]));
    QStringList paths;
    for (int i = 1; i < argc; ++i) paths << QString::fromLocal8Bit(argv[i]);
    if (paths.isEmpty()) {
        const QString dir = zt::TestData::corpus(QStringLiteral("owner-copy"));
        if (dir.isEmpty()) {
            std::printf("нет корпуса owner-copy — назовите файл доводом\n");
            return 1;
        }
        QDir d(dir);
        // Самые крупные: на них и видно цену.
        for (const QString& name : d.entryList({QStringLiteral("*.md")}, QDir::Files, QDir::Size))
            paths << d.filePath(name);
        while (paths.size() > 8) paths.removeLast();
    }

    std::printf("%-24s %9s %7s %9s %9s %9s %6s\n", "заметка", "байт", "блоков", "ничего, мс",
                "слово, мс", "всё, мс", "кусков");
    for (const QString& path : paths) {
        const Row row = measure(path);
        std::printf("%-24s %9lld %7d %9.1f %9.1f %9.1f %6d\n",
                    row.name.left(24).toUtf8().constData(), (long long)row.bytes, row.blocks,
                    row.nothing, row.oneWord, row.whole, row.hunks);
    }
    return 0;
}
