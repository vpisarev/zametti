// Чего стоит история: сколько раз мы трогаем диск и сколько почти одинаковых
// копий заметки при этом заводим.
//
// В обязательный набор НЕ входит: это замер, а не проверка. Смотрят на него
// тогда, когда решают, что менять в укладе записи.
//
//   taskset -c 0 ./tests/history_bench [заметка.md ...]
//
// Меряется ровно то, что делает программа на автосохранении: сериализация
// документа, запись файла заметки через QSaveFile и дозапись слепка в журнал.
// Разбирать это на части бессмысленно — человек платит за всё сразу.
//
// РАБОЧИЙ КАТАЛОГ — НА ДИСКЕ, А НЕ В tmpfs. QSaveFile внутри себя зовёт
// fdatasync (проверено strace), и на tmpfs он стоит ноль — числа вышли бы
// честные и бесполезные. Поэтому хранилище замера живёт в .testdata/.
//
// ЭТАЛОН рядом с каждым кругом: постоянный счётный цикл. Сам по себе он не
// нужен, нужен его разброс — пока он стоит намертво, числа замера про код, а
// не про машину.
//
// ЧТО ПОКАЗАЛ ЗАМЕР (10.08.2026, ext4, taskset -c 0, минимум по трём запускам).
// Минута непрерывного набора — это 40 автосохранений:
//
//   заметка   размер   в файл заметки   журнал   одно сохранение   без журнала
//   малая      300 Б         14.5 КБ    3.6 КБ       5.74 мс         5.54 мс
//   большая  233.4 КБ         9.12 МБ  166.8 КБ      16.26 мс        15.44 мс
//
// Отсюда три вывода, и все три против первой догадки.
//
// ДИСК ЖГЁТ НЕ ИСТОРИЯ, А САМА ЗАМЕТКА. За минуту работы в большой заметке в
// файл уходит 9.12 МБ против 167 КБ журнала: каждое автосохранение переписывает
// заметку ЦЕЛИКОМ, и внутри QSaveFile сидит fdatasync.
//
// ЖУРНАЛ СТОИТ ПОЧТИ НИЧЕГО: 0.8 мс из 16.3 (5%). Дельта-сжатие поколениями
// работает как обещано — 9.35 МБ слепков ложатся в 167 КБ (1.7%).
//
// СВАЛКИ В ХРАНИЛИЩЕ НЕТ: у владельца 65 журналов и 100 записей всего, у самой
// богатой заметки — 8. Много почти одинаковых записей копится только ВНУТРИ
// сеанса: шкала прореживания держит последний час целиком, а само прореживание
// заходит раз в запуск программы.

#include "editor_widget.h"
#include "journal.h"
#include "settings.h"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace {

// Сколько правок делает человек за минуту работы. Автосохранение ходит раз в
// 1.5 с, значит минута непрерывного набора — это 40 записей на диск.
constexpr int kSavesPerMinute = 40;

struct Sample {
    QString name;
    qint64 noteBytes = 0;      // размер самой заметки
    int saves = 0;
    qint64 journalBytes = 0;   // сколько занял журнал
    qint64 noteWritten = 0;    // сколько байт ушло в файл заметки
    double medianSaveMs = 0;
    double worstSaveMs = 0;
    qint64 records = 0;
    qint64 plainTotal = 0;     // сумма слепков ДО сжатия — «сколько копий»
};

double yardstick() {
    QElapsedTimer bar;
    bar.start();
    double acc = 0;
    for (int i = 1; i < 200000; ++i) acc += 1.0 / double(i);
    if (acc < 0) std::printf("этого не бывает\n");
    return double(bar.nsecsElapsed()) / 1e6;
}

std::string readAll(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    const QByteArray bytes = file.readAll();
    return std::string(bytes.constData(), size_t(bytes.size()));
}

// Хранилище замера: пустое, на диске, с одной заметкой внутри.
QString makeStore(const QString& root, const QString& sourceNote, QString* noteId) {
    QDir().mkpath(root + QStringLiteral("/.zametti"));
    QDir().mkpath(root + QStringLiteral("/.rescue"));
    QDir().mkpath(root + QStringLiteral("/history"));
    *noteId = QStringLiteral("01zzzzzzzzzzzz");
    const QString target = root + QLatin1Char('/') + *noteId + QStringLiteral(".md");
    QFile::remove(target);
    QFile::remove(root + QStringLiteral("/history/") + *noteId + QStringLiteral(".log"));

    std::string text = readAll(sourceNote);
    // Шапка нужна: без неё заметка не заметка.
    if (text.compare(0, 4, "<!--") != 0)
        text = "<!-- zametti\ncreated: 2020-01-01T00:00:00Z\n-->\n\n" + text;
    QFile out(target);
    if (out.open(QIODevice::WriteOnly)) out.write(text.data(), qint64(text.size()));
    return target;
}

Sample measure(const QString& root, const QString& sourceNote, int saves) {
    Sample sample;
    sample.name = QFileInfo(sourceNote).fileName();

    QString noteId;
    const QString path = makeStore(root, sourceNote, &noteId);
    sample.noteBytes = QFileInfo(path).size();

    zametti::NoteEditor editor;
    // Ключ ZAMETTI_BENCH_NOHISTORY отключает журнал: без корня хранилища
    // recordHistory уходит сразу. Разница двух прогонов и есть цена истории,
    // и меряется она тем же кодом, а не отдельной прикидкой.
    if (qEnvironmentVariableIsEmpty("ZAMETTI_BENCH_NOHISTORY")) editor.setStoreRoot(root);
    editor.resize(900, 700);
    editor.openFile(path);

    std::vector<double> times;
    times.reserve(size_t(saves));
    for (int i = 0; i < saves; ++i) {
        // Одна правка — один знак, как при наборе. В конец документа: так
        // делает человек, дописывающий заметку.
        QTextCursor caret(editor.document());
        caret.movePosition(QTextCursor::End);
        caret.insertText(QStringLiteral("а"));

        QElapsedTimer step;
        step.start();
        editor.save(false);
        times.push_back(double(step.nsecsElapsed()) / 1e6);
        sample.noteWritten += QFileInfo(path).size();
    }
    sample.saves = saves;

    const QString log = root + QStringLiteral("/history/") + noteId + QStringLiteral(".log");
    sample.journalBytes = QFileInfo(log).size();

    zametti::journal::History history(root);
    zametti::journal::Journal read;
    QString error;
    if (history.read(noteId, &read, &error)) {
        sample.records = read.entries.size();
        for (const zametti::journal::Entry& entry : read.entries)
            sample.plainTotal += entry.plainSize;
    }

    std::sort(times.begin(), times.end());
    if (!times.empty()) {
        sample.medianSaveMs = times[times.size() / 2];
        sample.worstSaveMs = times.back();
    }
    return sample;
}

QString human(qint64 bytes) {
    if (bytes < 1024) return QStringLiteral("%1 Б").arg(bytes);
    if (bytes < 1024 * 1024) return QStringLiteral("%1 КБ").arg(double(bytes) / 1024.0, 0, 'f', 1);
    return QStringLiteral("%1 МБ").arg(double(bytes) / (1024.0 * 1024.0), 0, 'f', 2);
}

}  // namespace

int ztHistoryBench(int argc, char** argv) {

    const QString root = QStringLiteral(ZAMETTI_TESTDATA) + QStringLiteral("/history-bench");
    QDir(root).removeRecursively();

    std::vector<QString> notes;
    for (int i = 1; i < argc; ++i) notes.push_back(QString::fromLocal8Bit(argv[i]));
    if (notes.empty()) {
        std::printf("нужны заметки: history_bench заметка.md ...\n");
        return 2;
    }

    // Эталон греется первым вызовом: на холодной частоте он показывает втрое
    // больше. Поэтому первый ответ выбрасываем.
    (void)yardstick();
    std::printf("эталон до замера: %.3f мс\n", yardstick());
    std::printf("%-28s %9s %6s %10s %10s %9s %9s\n", "заметка", "размер", "записей", "журнал",
                "в файл", "медиана", "худшая");

    for (const QString& note : notes) {
        const Sample s = measure(root, note, kSavesPerMinute);
        std::printf("%-28s %9s %6d %10s %10s %7.2f мс %6.2f мс\n",
                    s.name.left(28).toUtf8().constData(), human(s.noteBytes).toUtf8().constData(),
                    s.saves, human(s.journalBytes).toUtf8().constData(),
                    human(s.noteWritten).toUtf8().constData(), s.medianSaveMs, s.worstSaveMs);
        // Главное число разговора: сколько всего «копий заметки» лежит в
        // журнале до сжатия. Оно и есть та куча почти одинаковых слепков.
        std::printf("%-28s копий в журнале %s, сжато до %s (%.1f%%)\n", "",
                    human(s.plainTotal).toUtf8().constData(),
                    human(s.journalBytes).toUtf8().constData(),
                    s.plainTotal > 0 ? 100.0 * double(s.journalBytes) / double(s.plainTotal) : 0.0);
    }
    std::printf("эталон после замера: %.3f мс\n", yardstick());
    std::printf("\nминута непрерывного набора — это %d записей на диск\n", kSavesPerMinute);
    return 0;
}
