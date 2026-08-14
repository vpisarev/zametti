// ПРОСМОТРЩИК ДЛЯ ГЛАЗ. Отдельная маленькая программа: markdown-файл из
// командной строки, окно, прокрутка, Ctrl+= / Ctrl+−. Ни дерева, ни поиска, ни
// правки — только показ.
//
// Зачем она есть. Кегли в документе стали ступенями от шрифта документа, и весь
// шрифтовой зум свёлся к одному setDefaultFont. Что при этом происходит с
// вертикальным ритмом — вопрос вкуса, а не чисел, и решать его владельцу
// глазами. Программа даёт сравнить три способа задать высоту строки, не
// перезапускаясь:
//
//   Ctrl+L   доля от естественной → естественная → пиксели → доля …
//   Ctrl+R   зум шрифтом ↔ зум пересборкой (как сегодня в программе)
//   Ctrl+=   крупнее      Ctrl+−   мельче      Ctrl+0   сто процентов
//
// Ctrl+R и есть главный переключатель. «Шрифтом» — это один setDefaultFont:
// документ не трогается вовсе, но геометрия (маркеры, отступы, плашка кода)
// собрана в единице и с места не сходит. «Пересборкой» — сегодняшний путь:
// едет всё, но документ собирается заново на каждый шаг масштаба.
//
// Способ и масштаб написаны в заголовке окна.
//
// Программа НЕ входит в приёмку и ничего не проверяет. Это инструмент, и он
// умрёт вместе с вопросом, ради которого заведён.

#include "note_view.h"

#include "document_builder.h"
#include "editor_ops.h"
#include "parser.h"
#include "serializer.h"
#include "resources.h"
#include "settings.h"

#include <QApplication>
#include <QFile>
#include <QFileInfo>
#include <QKeyEvent>
#include <QImage>
#include <QTextCursor>
#include <QTextDocument>

#include <cstdio>
#include <string>

namespace {

using zametti::Appearance;

const char* modeName(Appearance::LineHeight mode) {
    switch (mode) {
        case Appearance::LineHeight::Proportional: return "доля от естественной";
        case Appearance::LineHeight::Natural: return "естественная";
        case Appearance::LineHeight::Pixels: return "пиксели";
    }
    return "?";
}

class Peek : public zametti::NoteView {
public:
    explicit Peek(QString path) : path_(std::move(path)) {
        setImageBase(QFileInfo(path_).absolutePath());
        rebuild();
    }

protected:
    void keyPressEvent(QKeyEvent* event) override {
        if ((event->modifiers() & Qt::ControlModifier) != 0) {
            switch (event->key()) {
                case Qt::Key_Equal:
                case Qt::Key_Plus:
                    setScale(scale_ * zametti::appearance().zoomStep);
                    return;
                case Qt::Key_Minus:
                    setScale(scale_ / zametti::appearance().zoomStep);
                    return;
                case Qt::Key_0:
                    setScale(1.0);
                    return;
                case Qt::Key_L:
                    cycleLineHeight();
                    return;
                case Qt::Key_R:
                    byRebuild_ = !byRebuild_;
                    rebuild();
                    return;
                default:
                    break;
            }
        }
        zametti::NoteView::keyPressEvent(event);
    }

private:
    // ЗУМ — ЭТО ОДИН setDefaultFont, и больше ничего. Документ не пересобирается:
    // абсолютных кеглей в нём нет, каждый знак задан ступенью от шрифта
    // документа. Ровно это здесь и проверяется глазами.
    void setScale(qreal scale) {
        Appearance& look = zametti::appearance();
        scale_ = qBound(look.zoomMin, scale, look.zoomMax);
        if (byRebuild_) {
            // Сегодняшний путь: кегль облика умножается на масштаб, и документ
            // собирается заново целиком. Едет всё, включая геометрию.
            look.baseFontPoint = basePoint_ * scale_;
            rebuild();
            return;
        }
        look.baseFontPoint = basePoint_;
        QFont base{QString(look.fontFamily)};
        base.setPointSizeF(basePoint_ * scale_);
        base.setStyleHint(QFont::Monospace);
        document()->setDefaultFont(base);
        setZoom(1.0);   // геометрия собрана в единице и за шрифтом не идёт
        applyContentWidth();
        syncTables();
        syncFormulas();
        showState();
    }

    void cycleLineHeight() {
        Appearance& look = zametti::appearance();
        switch (look.lineHeightMode) {
            case Appearance::LineHeight::Proportional:
                look.lineHeightMode = Appearance::LineHeight::Natural;
                break;
            case Appearance::LineHeight::Natural:
                look.lineHeightMode = Appearance::LineHeight::Pixels;
                break;
            case Appearance::LineHeight::Pixels:
                look.lineHeightMode = Appearance::LineHeight::Proportional;
                break;
        }
        rebuild();
    }

    void rebuild() {
        if (rebuilding_) return;
        rebuilding_ = true;
        Appearance& look = zametti::appearance();
        look.baseFontPoint = byRebuild_ ? basePoint_ * scale_ : basePoint_;

        QFile file(path_);
        if (!file.open(QIODevice::ReadOnly)) {
            std::fprintf(stderr, "не открылось: %s\n", qPrintable(path_));
            return;
        }
        const QByteArray bytes = file.readAll();
        const std::string source = zametti::normaliseSpaces(
            std::string_view(bytes.constData(), size_t(bytes.size())));

        auto* fresh = new QTextDocument(this);
        zametti::buildDocument(zametti::parse(source), *fresh);
        setDocument(fresh);
        setZoom(byRebuild_ ? scale_ : 1.0);
        applyContentWidth();
        syncTables();
        syncFormulas();
        showState();
        rebuilding_ = false;
    }

    void showState() {
        setWindowTitle(
            QStringLiteral("%1 — высота строки: %2 — зум: %3 — масштаб %4 %")
                .arg(QFileInfo(path_).fileName())
                .arg(QString::fromUtf8(modeName(zametti::appearance().lineHeightMode)))
                .arg(byRebuild_ ? QStringLiteral("пересборкой") : QStringLiteral("шрифтом"))
                .arg(int(scale_ * 100.0 + 0.5)));
    }

    // Снимок текущего вида. Нужен, чтобы можно было посмотреть на все три
    // способа рядом, не переключая руками, — и чтобы было что приложить к
    // разговору.
public:
    void shoot(const QString& file) {
        QImage frame(size(), QImage::Format_ARGB32);
        frame.fill(Qt::white);
        render(&frame);
        frame.save(file);
    }

    void setScaleForShot(qreal scale) { setScale(scale); }

    void setZoomByRebuild(bool on) {
        byRebuild_ = on;
        rebuild();
    }

    void setMode(Appearance::LineHeight mode) {
        zametti::appearance().lineHeightMode = mode;
        rebuild();
    }

    void deselect() {
        QTextCursor start(document());
        setTextCursor(start);
    }

    // Столбец пикселей сверху вниз, слитыми прогонами одного цвета. Нужен, чтобы
    // разглядеть швы между заливками: глазом «полоска в один пиксель» видна, а
    // светлее она или темнее соседей — уже нет.
    void scanColumn(int x) {
        QImage frame(size(), QImage::Format_ARGB32);
        frame.fill(Qt::white);
        render(&frame);
        if (x < 0 || x >= frame.width()) return;
        QRgb previous = 0;
        int from = 0;
        for (int y = 0; y <= frame.height(); ++y) {
            const QRgb here = y < frame.height() ? frame.pixel(x, y) : 0u;
            if (y > 0 && (y == frame.height() || here != previous)) {
                std::printf("  %4d..%-4d  %3d,%3d,%3d\n", from, y - 1, qRed(previous),
                            qGreen(previous), qBlue(previous));
                from = y;
            }
            previous = here;
        }
    }

    void selectAll() {
        QTextCursor whole(document());
        whole.movePosition(QTextCursor::End, QTextCursor::KeepAnchor);
        setTextCursor(whole);
    }

private:
    QString path_;
    qreal scale_ = 1.0;
    // Кегль облика, как он записан в настройках. Зум пересборкой его двигает,
    // и без запомненного значения масштаб копился бы сам на себе.
    qreal basePoint_ = zametti::appearance().baseFontPoint;
    bool byRebuild_ = false;
    bool rebuilding_ = false;
};

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    zametti::loadEmbeddedFonts();

    if (argc < 2) {
        std::fprintf(stderr,
                     "как звать: zametti-peek <файл.md>\n"
                     "  Ctrl+=/Ctrl+−  масштаб, Ctrl+0 — сто процентов\n"
                     "  Ctrl+L         чем задана высота строки\n");
        return 2;
    }

    Peek peek{QString::fromLocal8Bit(argv[1])};
    peek.resize(900, 700);

    // Снимками и выйти: zametti-peek файл.md --shot каталог
    if (argc >= 4 && QString::fromLocal8Bit(argv[2]) == QLatin1String("--shot")) {
        const QString dir = QString::fromLocal8Bit(argv[3]);
        peek.show();
        QCoreApplication::processEvents();
        peek.setZoomByRebuild(QString::fromLocal8Bit(argc >= 5 ? argv[4] : "") ==
                              QLatin1String("--rebuild"));
        struct Named { Appearance::LineHeight mode; const char* file; };
        const Named modes[] = {
            {Appearance::LineHeight::Proportional, "доля"},
            {Appearance::LineHeight::Natural, "естественная"},
            {Appearance::LineHeight::Pixels, "пиксели"},
        };
        for (const Named& one : modes) {
            peek.setMode(one.mode);
            for (int percent : {100, 200}) {
                peek.setScaleForShot(percent / 100.0);
                peek.deselect();
                QCoreApplication::processEvents();
                peek.shoot(QStringLiteral("%1/%2-%3.png").arg(dir).arg(
                    QString::fromUtf8(one.file)).arg(percent));
                peek.selectAll();
                QCoreApplication::processEvents();
                peek.shoot(QStringLiteral("%1/%2-%3-выделено.png").arg(dir).arg(
                    QString::fromUtf8(one.file)).arg(percent));
            }
        }
        std::printf("снимки: %s\n", qPrintable(dir));
        return 0;
    }

    // Столбцом и выйти: zametti-peek файл.md --scan <x>
    if (argc >= 4 && QString::fromLocal8Bit(argv[2]) == QLatin1String("--scan")) {
        peek.show();
        QCoreApplication::processEvents();
        std::printf("столбец x=%s\n", argv[3]);
        peek.scanColumn(QString::fromLocal8Bit(argv[3]).toInt());
        return 0;
    }

    peek.show();
    return app.exec();
}
