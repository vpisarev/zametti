// Лист для выбора кегля ВЫКЛЮЧНОЙ формулы: одна и та же заметка при разных
// formulas.displayScale. Выбирает владелец глазами — так же, как выбирал
// inlineScale по листу сравнения.
#include "editor_widget.h"
#include "formula.h"
#include "settings.h"
#include "settings_hook.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QTest>

#include <cstdio>

int ztDisplayScaleProbe(int argc, char** argv) {
    const QString dir = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QDir::tempPath();
    QDir().mkpath(dir);
    QString error;
    if (!zametti::Formulas::init(&error)) {
        std::fprintf(stderr, "движок формул: %s\n", qPrintable(error));
        return 1;
    }

    const char* note =
        "# Выключные формулы\n\nОбычный текст рядом, чтобы было с чем сравнивать глазом.\n\n"
        "$$\\frac{a}{b}$$\n\nСтрока текста между формулами.\n\n"
        "$$\\sum_{k=0}^\\infty \\frac{x^k}{k!} \\not= \\prod_{j=1}^{10} \\frac{j}{j+1}$$\n\n"
        "И ещё одна строка текста.\n\n"
        "$$\\lim_{h\\to0} \\frac{\\sin(x+h) - \\sin(x)}{h} = \\cos x$$\n\nХвост.\n";
    const QString path = QDir(dir).filePath(QStringLiteral("масштабы.md"));
    QFile file(path);
    if (file.open(QIODevice::WriteOnly)) file.write(note);
    file.close();

    for (const qreal scale : {1.00, 1.10, 1.25}) {
        zametti::mutableSettingsForTests().formulas().setDisplayScale(scale);
        zametti::NoteEditor editor;
        editor.resize(900, 620);
        editor.show();
        QTest::qWait(20);
        editor.openFile(path);
        QTest::qWait(120);
        const QString name = QStringLiteral("выключные-%1.png").arg(int(scale * 100));
        if (!editor.grab().toImage().save(QDir(dir).filePath(name)))
            std::fprintf(stderr, "не сохранился %s\n", qPrintable(name));
        std::printf("%s\n", qPrintable(name));
    }
    return 0;
}
