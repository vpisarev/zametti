// СКОЛЬКО СТОИТ ПЕРЕКЛЮЧЕНИЕ ЗАМЕТКИ В НАСТОЯЩЕМ ХРАНИЛИЩЕ.
//
// Владелец: «переключение между заметками без правок перестало быть
// мгновенным». Стенд big меряет уход-возврат на редакторе БЕЗ хранилища — то
// есть без журнала, без канона файла на диске и без всего, что окно делает по
// fileChanged. Этот стенд открывает НАСТОЯЩИЙ ZStorage (на копии хранилища
// владельца — оригинал не трогать) и ходит по заметкам, печатая разрез
// openFile по фазам (NoteEditor::OpenTrace): первый круг — холодный, второй —
// из кэша. Отдельной колонкой — записал ли уход файл: чистое переключение не
// имеет права писать ни файл, ни журнал.
//
//   zametti-bench switch <корень-хранилища> [N | id...]
//
// Без id берутся N самых больших заметок (по умолчанию 12) плюс три самых
// маленьких: интересен и размер, и то, что от размера не зависит.

#include "editor_widget.h"
#include "zstorage.h"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QTest>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

struct Row {
    QString id;
    qint64 bytes = 0;
    zametti::NoteEditor::OpenTrace cold;
    zametti::NoteEditor::OpenTrace warm;
    qint64 coldFrame = 0;   // + processEvents + перерисовка после открытия
    qint64 warmFrame = 0;
};

qint64 frameAfterOpen(zametti::NoteEditor& editor) {
    QElapsedTimer t;
    t.start();
    QCoreApplication::processEvents();
    editor.viewport()->repaint();
    return t.nsecsElapsed() / 1000;
}

void printRow(const char* label, const Row& r, const zametti::NoteEditor::OpenTrace& t, qint64 frame) {
    std::printf("%-5s %-15s %8lld | %7lld %7lld %6lld %7lld %7lld %7lld %7lld %6lld %7lld | %7lld %7lld %s\n",
                label, r.id.toUtf8().constData(), (long long)r.bytes, (long long)t.save,
                (long long)t.stash, (long long)t.read, (long long)t.canon, (long long)t.journal,
                (long long)t.load, (long long)t.install, (long long)t.land, (long long)t.activate,
                (long long)t.total, (long long)frame,
                t.saveOutcome == 3 ? "WROTE" : t.saveOutcome == 2 ? "unchanged" : t.saveOutcome == 4 ? "FAILED" : "");
}

}  // namespace

int ztSwitchBench(int argc, char** argv) {
    if (argc < 2) {
        std::printf("zametti-bench switch <корень-хранилища> [N | id...]\n");
        return 2;
    }
    const QString root = QDir(QString::fromLocal8Bit(argv[1])).absolutePath();
    auto storage = std::make_shared<zametti::ZStorage>(root);
    storage->reload();
    if (!storage->isStore()) {
        std::printf("не хранилище: %s\n", argv[1]);
        return 1;
    }

    std::vector<Row> rows;
    {
        QStringList ids;
        for (int i = 2; i < argc; ++i) {
            const QString arg = QString::fromLocal8Bit(argv[i]);
            bool number = false;
            arg.toInt(&number);
            if (!number) ids.append(arg);
        }
        if (ids.isEmpty()) {
            const int n = argc >= 3 ? std::max(1, std::atoi(argv[2])) : 12;
            std::vector<std::pair<qint64, QString>> sized;
            for (const QFileInfo& info :
                 QDir(root).entryInfoList({QStringLiteral("*.md")}, QDir::Files))
                sized.push_back({info.size(), info.completeBaseName()});
            std::sort(sized.begin(), sized.end(),
                      [](const auto& a, const auto& b) { return a.first > b.first; });
            for (size_t i = 0; i < sized.size() && int(i) < n; ++i) ids.append(sized[i].second);
            for (size_t i = sized.size(); i-- > 0 && ids.size() < n + 3;)
                if (!ids.contains(sized[i].second)) ids.append(sized[i].second);
        }
        for (const QString& id : ids) {
            Row r;
            r.id = id;
            r.bytes = QFileInfo(storage->pathOf(id)).size();
            rows.push_back(r);
        }
    }
    if (rows.size() < 2) {
        std::printf("нужно хотя бы две заметки\n");
        return 1;
    }

    zametti::NoteEditor editor;
    editor.setStorage(storage);
    editor.resize(1000, 800);
    editor.show();
    QTest::qWait(50);

    // Холодный круг: каждая заметка открывается первый раз; уход — из
    // предыдущей (её save/stash ложатся в строку ТЕКУЩЕЙ: openFile меряет
    // уход и приход вместе, как их чувствует человек).
    for (Row& r : rows) {
        editor.openFile(storage->pathOf(r.id));
        r.cold = editor.lastOpenTrace();
        r.coldFrame = frameAfterOpen(editor);
        QTest::qWait(30);
    }
    // Тёплый круг: те же заметки из кэша (если влезли в бюджет кэша).
    for (Row& r : rows) {
        editor.openFile(storage->pathOf(r.id));
        r.warm = editor.lastOpenTrace();
        r.warmFrame = frameAfterOpen(editor);
        QTest::qWait(30);
    }

    std::printf("хранилище %s, заметок %zu; микросекунды\n", root.toUtf8().constData(), rows.size());
    std::printf("%-5s %-15s %8s | %7s %7s %6s %7s %7s %7s %7s %6s %7s | %7s %7s\n", "круг", "id",
                "байт", "save", "stash", "read", "canon", "journal", "load", "install", "land",
                "activ", "ИТОГО", "+кадр");
    for (const Row& r : rows) printRow("cold", r, r.cold, r.coldFrame);
    std::printf("\n");
    for (const Row& r : rows)
        printRow(r.warm.fromCache ? "cache" : "warm", r, r.warm, r.warmFrame);
    return 0;
}
