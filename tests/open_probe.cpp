#include "editor_widget.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QTest>
#include <cstdio>
int ztOpenProbe(int argc, char** argv) {
    (void)argc;
    zametti::NoteEditor editor;
    editor.resize(1000, 800);
    editor.show();
    QTest::qWait(30);
    QElapsedTimer t; t.start();
    editor.openFile(QString::fromLocal8Bit(argv[1]));
    std::printf("openFile: %lld мс\n", (long long)t.restart());
    QTest::qWait(50);
    editor.grab();
    std::printf("первый кадр: %lld мс\n", (long long)t.restart());
    return 0;
}
