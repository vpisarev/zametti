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

int ztSourceBench(int argc, char** argv) {
    QStringList paths;
    for (int i = 2; i < argc; ++i) paths << QString::fromLocal8Bit(argv[i]);
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
