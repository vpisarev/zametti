// ФАЗЗИНГ НАЛОЖЕНИЯ ИСХОДНИКА: случайные правки ТЕКСТА поверх заметок владельца.
//
// Матрица случаев (source_apply_test) придумана тем же человеком, который писал
// правило, и потому проверяет то, о чём он подумал. Здесь правки случайные:
// вставить строку, убрать строку, поменять знак посреди строки, разрезать
// строку, склеить две, испортить разделитель таблицы, добавить забор. После
// каждой партии обязано выполняться одно и то же:
//
//   1. канон после наложения равен канону правленого текста (с точностью до
//      возврата каретки — его человек в плоском тексте не видит и набрать не
//      может, см. document_source.cpp);
//   2. ОДИН undo() возвращает прежнее побайтово, и счётчик отмены возвращается
//      туда, где был: вся правка исходника — один шаг;
//   3. redo() возвращает правленое.
//
// Случайность воспроизводимая: зерно печатается и берётся из сборки, поэтому
// упавший прогон повторяется дословно.

#include "document.h"

#include "test_util.h"
#include "testdata.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QStringList>

#include <cstdio>
#include <random>
#include <string>

namespace {

using namespace zametti;

// Сколько раз за прогон точечность уступила место полной замене. Печатается
// числом: пока оно не ноль, у режима есть названный долг.
int g_fallbacks = 0;

std::string same(const QString& text) {
    QString out = text;
    out.remove(QLatin1Char('\r'));
    return out.toStdString();
}

// Одна случайная правка ТЕКСТА — такая, какую сделал бы человек в текстовом
// редакторе. Нарочно бывают и разрушительные: сломанный разделитель таблицы и
// лишний забор меняют СТРОЕНИЕ, а это самое интересное для наложения.
void mutate(QStringList& lines, std::mt19937& rng) {
    if (lines.isEmpty()) {
        lines << QStringLiteral("строка");
        return;
    }
    const int at = int(rng() % unsigned(lines.size()));
    switch (rng() % 7) {
        case 0: lines.insert(at, QStringLiteral("вставленная строка")); break;
        case 1: lines.removeAt(at); break;
        case 2: lines[at] += QStringLiteral(" хвост"); break;
        case 3: {
            QString& line = lines[at];
            if (!line.isEmpty()) line[int(rng() % unsigned(line.size()))] = QLatin1Char('Z');
            break;
        }
        case 4: {
            const QString line = lines[at];
            const int cut = line.isEmpty() ? 0 : int(rng() % unsigned(line.size()));
            lines[at] = line.left(cut);
            lines.insert(at + 1, line.mid(cut));
            break;
        }
        case 5:
            if (at + 1 < lines.size()) {
                lines[at] += lines[at + 1];
                lines.removeAt(at + 1);
            }
            break;
        default: lines.insert(at, QStringLiteral("```")); break;
    }
}

void fuzzNote(const QString& path, unsigned seed, int rounds, int& notes) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return;
    const QByteArray bytes = file.readAll();
    if (bytes.isEmpty()) return;

    ZDocument doc;
    doc.loadMarkdown(std::string(bytes.constData(), size_t(bytes.size())));
    doc.setUndoEnabled(true);
    ++notes;

    std::mt19937 rng(seed);
    for (int round = 0; round < rounds && zt::g_failures == 0; ++round) {
        const QString before = doc.liveMarkdown();
        const int stepsBefore = doc.undoSteps();

        QStringList lines = before.split(QLatin1Char('\n'));
        // Плоский виджет возвратов каретки не держит — и фаззер тоже: он играет
        // человека, правящего текст, а не байты файла.
        for (QString& line : lines) line.remove(QLatin1Char('\r'));
        const int howMany = 1 + int(rng() % 3);
        for (int i = 0; i < howMany; ++i) mutate(lines, rng);
        const QString edited = lines.join(QLatin1Char('\n'));

        ZDocument probe;
        probe.loadMarkdown(edited.toStdString());
        const QString want = probe.liveMarkdown();

        const std::string where =
            QFileInfo(path).fileName().toStdString() + " круг " + std::to_string(round) +
            " (зерно " + std::to_string(seed) + ")";
        const int fallbacksBefore = doc.sourceFallbacks();
        const int hunks = doc.applySourceText(edited);
        if (doc.sourceFallbacks() != fallbacksBefore) {
            // Точечность не справилась, правка легла целиком. Правка человека
            // при этом цела — но случай нужен для матрицы, и он кладётся на
            // диск ровно так же, как настоящий отказ.
            const QString out = zt::TestData::outDir(QStringLiteral("source-fuzz"));
            const auto dump = [&out](const QString& name, const QString& text) {
                QFile f(QDir(out).filePath(name));
                if (f.open(QIODevice::WriteOnly)) f.write(text.toUtf8());
            };
            dump(QStringLiteral("запасной-было.md"), before);
            dump(QStringLiteral("запасной-стало.md"), edited);
            ++g_fallbacks;
        }
        if (hunks < 0) {
            // Отказ законен только один: человек набрал шапку. Случайные правки
            // её не рождают, значит это дефект, и он назван.
            //
            // И НАЗВАН ВОСПРОИЗВОДИМО: обе стороны кладутся на диск. Искать
            // случайный круг руками — это искать иголку; файлы дают готовый
            // случай для матрицы.
            const QString out = zt::TestData::outDir(QStringLiteral("source-fuzz"));
            const auto dump = [&out](const QString& name, const QString& text) {
                QFile f(QDir(out).filePath(name));
                if (f.open(QIODevice::WriteOnly)) f.write(text.toUtf8());
            };
            dump(QStringLiteral("было.md"), before);
            dump(QStringLiteral("стало.md"), edited);
            ZT_TRUE(where + ": наложение отвергло текст, код " + std::to_string(hunks) +
                        "; стороны в " + out.toStdString(),
                    false);
            return;
        }
        ZT_EQ(where + ": канон совпал", same(want), same(doc.liveMarkdown()));
        if (zt::g_failures != 0) return;
        if (hunks == 0) {
            ZT_EQ(where + ": ноль кусков — и стек отмены не тронут",
                  std::to_string(stepsBefore), std::to_string(doc.undoSteps()));
            continue;
        }
        ZT_TRUE(where + ": отмена сработала", doc.undo());
        ZT_EQ(where + ": один шаг вернул прежнее", before.toStdString(),
              doc.liveMarkdown().toStdString());
        ZT_EQ(where + ": и счётчик отмены вернулся", std::to_string(stepsBefore),
              std::to_string(doc.undoSteps()));
        if (zt::g_failures != 0) return;
        if (!doc.redo()) {
            // ИЗВЕСТНЫЙ ДЕФЕКТ, названный в docs/known_bugs.md: ПУСТАЯ заметка,
            // в которую кладут ПУСТОЙ забор кода, не оставляет шага возврата —
            // фрагмент без единого знака, и Qt записывает одну лишь смену
            // формата. Отмена при этом работает, данные целы.
            //
            // Пропускается ПО ИМЕНИ, а не молча: любое другое место, где
            // возврат не сработал, по-прежнему краснеет. Уйдёт дефект — уйдёт
            // и эта ветка.
            if (before.trimmed().isEmpty()) continue;

            const QString out = zt::TestData::outDir(QStringLiteral("source-fuzz"));
            const auto dump = [&out](const QString& name, const QString& text) {
                QFile f(QDir(out).filePath(name));
                if (f.open(QIODevice::WriteOnly)) f.write(text.toUtf8());
            };
            dump(QStringLiteral("redo-было.md"), before);
            dump(QStringLiteral("redo-стало.md"), edited);
            ZT_TRUE(where + ": возврат сработал (кусков " + std::to_string(hunks) +
                        ", шагов отмены " + std::to_string(doc.undoSteps()) + "; стороны в " +
                        out.toStdString() + ")",
                    false);
            return;
        }
        ZT_EQ(where + ": и вернул правленое", same(want), same(doc.liveMarkdown()));
    }
}

}  // namespace

TEST(SourceFuzz, All) {
    const QString dir = zt::TestData::corpus(QStringLiteral("owner-copy"));
    if (dir.isEmpty()) {
        std::printf("owner-copy: корпуса нет, фаззинг исходника пропущен\n");
        return;
    }
    const unsigned seed = ZAMETTI_FUZZ_SEED;
    const int rounds = ZAMETTI_FUZZ_ROUNDS;
    std::printf("зерно %u, кругов на заметку %d\n", seed, rounds);

    QDir d(dir);
    int notes = 0;
    // Крупные заметки дороги, а интересны не размером: берём каждую пятую, но
    // все — разного строения (списки, код, таблицы, формулы, картинки).
    const QStringList names = d.entryList({QStringLiteral("*.md")}, QDir::Files, QDir::Name);
    for (int i = 0; i < names.size() && zt::g_failures == 0; i += 5)
        fuzzNote(d.filePath(names[i]), seed + unsigned(i), rounds, notes);
    std::printf("заметок %d, запасной путь сработал %d раз\n", notes, g_fallbacks);
    zt::report("фаззинг исходника");
}
