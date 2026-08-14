// Одна и та же формула, отрисованная много раз: одинакова ли она?
#include "formula.h"
#include "resources.h"
#include <QApplication>
#include <QImage>
#include <cstdio>
int ztMatrixProbe(int argc, char** argv) {
    (void)argc;
    zametti::loadEmbeddedFonts();   // заодно поднимает ресурсы qrc
    QString error;
    if (!zametti::Formulas::init(&error)) { std::printf("движок: %s\n", qPrintable(error)); return 1; }
    const QString matrix = QStringLiteral(
        "D = \\left[\n    \\begin{matrix}\n      \\lambda_1 & 0 & 0 & \\cdots & 0 \\\\\n"
        "      0 & \\lambda_2 & 0 & \\cdots & 0 \\\\\n      0 & 0 & \\lambda_3 & \\cdots & 0 \\\\\n"
        "      \\vdots & \\vdots & \\vdots & \\ddots & \\vdots \\\\\n"
        "      0 & 0 & 0 & \\cdots & \\lambda_n\n    \\end{matrix}\n    \\right].");
    const QString other = QStringLiteral("\\sum_{k=0}^\\infty \\frac{x^k}{k!} \\not= \\prod_{j=1}^{10} \\frac{j}{j+1}");
    // Та же матрица, но отступы — НЕРАЗРЫВНЫМИ пробелами: ровно так она лежит
    // в заметке владельца. Раньше движок рисовал их настоящими пробелами, и
    // матрица разъезжалась дырами.
    QString nbsp = matrix;
    nbsp.replace(QLatin1Char(' '), QChar(0x00A0));
    const zametti::FormulaImage n = zametti::Formulas::render(nbsp, true, 16.0, Qt::black, 1.0);
    n.image.save(QStringLiteral("%1/матрица-неразрывные.png").arg(argv[1]));

    QString flat = matrix;
    flat.replace(QStringLiteral("\n      "), QStringLiteral("\n"));
    flat.replace(QStringLiteral("\n    "), QStringLiteral("\n"));
    const zametti::FormulaImage a = zametti::Formulas::render(matrix, true, 16.0, Qt::black, 1.0);
    const zametti::FormulaImage b = zametti::Formulas::render(flat, true, 16.0, Qt::black, 1.0);
    std::printf("с отступами: %.1f x %.1f; без отступов: %.1f x %.1f; неразрывные: %.1f x %.1f\n",
                a.width, a.height, b.width, b.height, n.width, n.height);
    a.image.save(QStringLiteral("%1/матрица-отступы.png").arg(argv[1]));
    b.image.save(QStringLiteral("%1/матрица-ровно.png").arg(argv[1]));

    for (int i = 0; i < 6; ++i) {
        // Через раз рисуем чужую формулу — вдруг движок хранит состояние.
        if (i % 2 == 1) zametti::Formulas::render(other, true, 16.0, Qt::black, 1.0);
        const zametti::FormulaImage m = zametti::Formulas::render(matrix, true, 16.0, Qt::black, 1.0);
        std::printf("заход %d: %.1f x %.1f (картинка %dx%d) %s\n", i, m.width, m.height,
                    m.image.width(), m.image.height(), qPrintable(m.error));
        if (i == 0 || i == 5) m.image.save(QStringLiteral("%1/матрица-%2.png").arg(argv[1]).arg(i));
    }
    return 0;
}
