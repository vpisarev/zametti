// DPI шрифтов не делает масштаб экрана дробным.
//
// На маке кегль переводится в пиксели через 72 dpi, и программа задаёт
// QT_FONT_DPI=96 (main.cpp). Qt 6 применяет чужой DPI не к шрифтам, а ко всему
// масштабу: 96/72 ложится множителем поверх масштаба экрана, dpr становится
// дробным (2,667 на ретине), и на границах областей перерисовки остаются
// столбики в один физический пиксель — «след каретки» владельца (04.09.2026,
// подробности в font_dpi.h). prepareFontDpi гасит это политикой округления:
// множитель округляется до целого, остаток уходит в логический DPI.
//
// Проверяется дочерним процессом: политику Qt читает при создании приложения, а
// в общем бинарнике наборов оно создано в main. Ребёнок получает
// ZAMETTI_TEST_FONT_DPI=128 — вход наборов зовёт с ним ту же prepareFontDpi, что
// main.cpp; на оффскрине (96 dpi) множитель 128/96 = 4/3 — та же дробь, что у
// мака, и без политики dpr тут же становится 1,333.

#include "font_dpi.h"

#include "test_util.h"

#include <QCoreApplication>
#include <QFont>
#include <QFontInfo>
#include <QGuiApplication>
#include <QProcess>
#include <QProcessEnvironment>
#include <QScreen>

#include <cmath>

namespace {

constexpr const char* kChildFlag = "ZAMETTI_FONT_DPI_CHILD";

}  // namespace

TEST(FontDpi, IntegerScale) {
    if (qEnvironmentVariableIsSet(kChildFlag)) {
        const QScreen* screen = QGuiApplication::primaryScreen();
        ASSERT_NE(nullptr, screen);
        const qreal dpr = screen->devicePixelRatio();
        const qreal logical = screen->logicalDotsPerInch();
        ZT_TRUE("масштаб экрана целый при чужом DPI шрифтов (dpr " + std::to_string(dpr) + ")",
                std::fabs(dpr - std::round(dpr)) < 1e-6);
        ZT_TRUE("логический DPI — тот, что просили (" + std::to_string(logical) + ")",
                std::fabs(logical - 128.0) < 0.5);
        // И кегль действительно идёт через него: 12 pt при 128 dpi — 21 px.
        QFont font;
        font.setPointSizeF(12.0);
        const int px = QFontInfo(font).pixelSize();
        ZT_TRUE("12 pt при 128 dpi — 21 px (" + std::to_string(px) + ")", px == 21);
        EXPECT_EQ(0, zt::report("DPI шрифтов и целый масштаб экрана"));
        return;
    }
    QProcess child;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QString::fromLatin1(kChildFlag), QStringLiteral("1"));
    env.insert(QStringLiteral("ZAMETTI_TEST_FONT_DPI"), QStringLiteral("128"));
    env.remove(QStringLiteral("QT_FONT_DPI"));
    env.remove(QStringLiteral("QT_SCALE_FACTOR_ROUNDING_POLICY"));
    env.remove(QStringLiteral("QT_SCALE_FACTOR"));
    // Ровно оффскрин: у него 96 dpi, и 128 даёт ту же дробь 4/3, что мак.
    env.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
    child.setProcessEnvironment(env);
    child.setProcessChannelMode(QProcess::ForwardedChannels);
    child.start(QCoreApplication::applicationFilePath(), {QStringLiteral("--gtest_filter=FontDpi.IntegerScale")});
    ASSERT_TRUE(child.waitForStarted(10000));
    ASSERT_TRUE(child.waitForFinished(60000));
    EXPECT_EQ(QProcess::NormalExit, child.exitStatus());
    EXPECT_EQ(0, child.exitCode());
}
