// Variable fonts under Qt, measured before they are embedded (07.09.2026).
//
//   zametti-bench fonts <file.ttf>...
//
// Registers the files, and for every family they bring: the styles the font
// database enumerates (the named instances of a variable font must be among
// them), what QFontInfo resolves for weights 400/500/600/700 upright and
// italic, and whether the weight is really drawn — the advance of a sample
// line and the ink of a rendered glyph must grow with the weight. A family
// whose default instance is not Regular (Source Code Pro: ExtraLight) is the
// case that decides whether the named instances are honoured at all.

#include <QFont>
#include <QFontDatabase>
#include <QFontInfo>
#include <QFontMetricsF>
#include <QImage>
#include <QPainter>
#include <QString>
#include <QStringList>

#include <cstdio>

namespace {

// Dark pixels of the sample drawn at the given weight: a coarse measure of
// stroke thickness that does not depend on the font's own metrics.
qint64 inkOf(const QFont& font) {
    QImage image(600, 60, QImage::Format_Grayscale8);
    image.fill(255);
    QPainter painter(&image);
    painter.setFont(font);
    painter.drawText(QRect(0, 0, 600, 60), Qt::AlignVCenter | Qt::AlignLeft,
                     QStringLiteral("Hamburgefonstiv Щукин"));
    painter.end();
    qint64 ink = 0;
    for (int y = 0; y < image.height(); ++y) {
        const uchar* line = image.constScanLine(y);
        for (int x = 0; x < image.width(); ++x) ink += 255 - line[x];
    }
    return ink;
}

void probeFamily(const QString& family) {
    std::printf("family %s\n  styles: %s\n", family.toUtf8().constData(),
                QFontDatabase::styles(family).join(QStringLiteral(" | ")).toUtf8().constData());
    for (const bool italic : {false, true}) {
        qint64 previousInk = 0;
        for (const int weight : {400, 500, 600, 700}) {
            QFont font(family);
            font.setPointSizeF(24.0);
            font.setWeight(QFont::Weight(weight));
            font.setItalic(italic);
            const QFontInfo info(font);
            const qreal advance = QFontMetricsF(font).horizontalAdvance(QStringLiteral("Hamburgefonstiv"));
            const qint64 ink = inkOf(font);
            std::printf("  %s %3d -> family '%s' style '%s' weight %d italic %d exact %d | advance %.1f "
                        "ink %lld%s\n",
                        italic ? "italic " : "upright", weight, info.family().toUtf8().constData(),
                        info.styleName().toUtf8().constData(), info.weight(), int(info.italic()),
                        int(info.exactMatch()), advance, (long long)ink,
                        previousInk != 0 && ink <= previousInk ? "  <-- NOT HEAVIER" : "");
            previousInk = ink;
        }
    }
}

}  // namespace

int ztFontProbe(int argc, char** argv) {
    if (argc < 2) {
        std::printf("zametti-bench fonts <file.ttf>...\n");
        return 2;
    }
    QStringList families;
    for (int i = 1; i < argc; ++i) {
        const QString path = QString::fromLocal8Bit(argv[i]);
        const int id = QFontDatabase::addApplicationFont(path);
        if (id < 0) {
            std::printf("NOT LOADED: %s\n", path.toUtf8().constData());
            continue;
        }
        const QStringList got = QFontDatabase::applicationFontFamilies(id);
        std::printf("loaded %s -> %s\n", path.toUtf8().constData(),
                    got.join(QStringLiteral(", ")).toUtf8().constData());
        for (const QString& family : got)
            if (!families.contains(family)) families << family;
    }
    for (const QString& family : families) probeFamily(family);
    // A family named "<X> Var" (IBM's variable cuts) is asked for as "<X>" by
    // the settings: does QFont::insertSubstitution serve it — by family() and
    // by families()? The two doors of QFont resolve differently, and the
    // documents use the second.
    for (const QString& family : families) {
        if (!family.endsWith(QStringLiteral(" Var"))) continue;
        const QString base = family.chopped(4);
        QFont::insertSubstitution(base, family);
        QFont byFamily(base);
        byFamily.setWeight(QFont::DemiBold);
        QFont byFamilies;
        byFamilies.setFamilies({base});
        byFamilies.setWeight(QFont::DemiBold);
        QFont real(family);
        real.setWeight(QFont::DemiBold);
        std::printf("substitution: advance by the real family %.1f, by family() %.1f, by families() %.1f\n",
                    QFontMetricsF(real).horizontalAdvance(QStringLiteral("Hamburgefonstiv")),
                    QFontMetricsF(byFamily).horizontalAdvance(QStringLiteral("Hamburgefonstiv")),
                    QFontMetricsF(byFamilies).horizontalAdvance(QStringLiteral("Hamburgefonstiv")));
        std::printf("substitution %s -> %s: family() resolves to '%s' (%s, %d); families() resolves to "
                    "'%s' (%s, %d)\n",
                    base.toUtf8().constData(), family.toUtf8().constData(),
                    QFontInfo(byFamily).family().toUtf8().constData(),
                    QFontInfo(byFamily).styleName().toUtf8().constData(), QFontInfo(byFamily).weight(),
                    QFontInfo(byFamilies).family().toUtf8().constData(),
                    QFontInfo(byFamilies).styleName().toUtf8().constData(),
                    QFontInfo(byFamilies).weight());
    }
    return 0;
}
