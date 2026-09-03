// ЛЕЧЕНИЕ — ОДИН РАЗ, ДАЛЬШЕ — НЕПОДВИЖНАЯ ТОЧКА (требование владельца,
// 03.09.2026). Программа вправе причесать чужую заметку при первом открытии,
// ввозе, правке во внешнем редакторе — но не вправе приумножать хаос при
// каждом следующем открытии: у владельца такие заметки переписывались на
// каждом визите, со штампом и слепком в журнал. Байтовой эквивалентности «до и
// после лечения» не требуется; требуется, чтобы записанное ОДИН раз читалось и
// записывалось назад собой.
//
// Для каждого файла корпуса: w1 = канон(чтение(файл)), w2 = канон(чтение(w1)).
// Провал — w1 != w2. «Вылечено» (w1 != файл) — печатается числом, не провал.

#include "znote.h"

#include "test_util.h"
#include "testdata.h"

#include <QByteArray>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QString>

#include <string>

namespace {

// Как пишет программа: шапка своя, тело каноном.
std::string canonOf(const std::string& source) {
    zametti::ZNote note;
    note.load(source);
    return note.toMarkdown();
}

std::string firstDiffLine(const std::string& a, const std::string& b) {
    const QList<QByteArray> x = QByteArray::fromStdString(a).split('\n');
    const QList<QByteArray> y = QByteArray::fromStdString(b).split('\n');
    for (int i = 0; i < qMax(x.size(), y.size()); ++i)
        if (x.value(i) != y.value(i))
            return "строка " + std::to_string(i + 1) + ": [" + x.value(i).left(70).toStdString() +
                   "] -> [" + y.value(i).left(70).toStdString() + "]";
    return {};
}

}  // namespace

TEST(Heal, CorpusConvergesInOneStep) {
    const QString root = zt::TestData::corpus(QStringLiteral("corpus"));
    ZT_SKIP_NO_CORPUS(root, "корпус заметок");
    int files = 0;
    int healed = 0;
    int diverged = 0;
    QDirIterator it(root, {QStringLiteral("*.md")}, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) continue;
        const std::string source = f.readAll().toStdString();
        ++files;
        const std::string w1 = canonOf(source);
        const std::string w2 = canonOf(w1);
        if (w1 != w2) {
            ++diverged;
            std::fprintf(stderr, "не сходится: %s\n  %s\n", path.toUtf8().constData(),
                         firstDiffLine(w1, w2).c_str());
        }
        if (w1 != source) {
            ++healed;
            if (healed <= 3)
                std::fprintf(stderr, "вылечено: %s\n  %s\n", path.toUtf8().constData(),
                             firstDiffLine(source, w1).c_str());
        }
    }
    std::fprintf(stderr, "лечение: файлов %d, вылечено при первом чтении %d, не сходится %d\n",
                 files, healed, diverged);
    EXPECT_TRUE(files > 0);
    EXPECT_EQ(0, diverged) << "лечение обязано сходиться за один шаг";
}
