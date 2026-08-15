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
//   Ctrl+B   блок кода: построчно ↔ ОДНИМ QTextBlock (опыт, см. ниже)
//   Ctrl+=   крупнее      Ctrl+−   мельче      Ctrl+0   сто процентов
//
// Ctrl+R и есть главный переключатель. «Шрифтом» — это один setDefaultFont:
// документ не трогается вовсе, но геометрия (маркеры, отступы, плашка кода)
// собрана в единице и с места не сходит. «Пересборкой» — сегодняшний путь:
// едет всё, но документ собирается заново на каждый шаг масштаба.
//
// ОПЫТ ПРО БЛОК КОДА (Ctrl+B). Обычно литеральное содержимое режется по
// QTextBlock на строку — так набор внутри длинного блока кода не заставляет Qt
// переразмечать его целиком. В просмотрщике набора нет вовсе, и вопрос стоит
// обратный: во что обходятся тысячи блоков вместо сотен при вёрстке, прокрутке
// и Ctrl+=. С ключом блок кода становится ОДНИМ QTextBlock, а переводы строк
// внутри — разделителями U+2028, теми же, какими живёт мягкий перенос в абзаце.
//
// Числа печатаются в stdout на каждую пересборку и на каждый шаг масштаба.
//
// Способ и масштаб написаны в заголовке окна.
//
// Программа НЕ входит в приёмку и ничего не проверяет. Это инструмент, и он
// умрёт вместе с вопросом, ради которого заведён.

#include "note_view.h"

#include "document_builder.h"
#include "document_pieces.h"
#include "editor_ops.h"
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

#include <QAbstractTextDocumentLayout>
#include <QElapsedTimer>
#include <QScrollBar>
#include <QTextBlock>

#include <algorithm>

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
                case Qt::Key_B:
                    codeAsOneBlock_ = !codeAsOneBlock_;
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

        // ЗАМЕР ЦЕНЫ ШАГА МАСШТАБА. Отдельно шрифт (он же полная переразметка
        // документа) и отдельно то, что делаем мы поверх: ширина колонки,
        // сетки таблиц, места формул.
        QElapsedTimer timer;
        timer.start();
        document()->setDefaultFont(base);
        const qreal size = document()->documentLayout()->documentSize().height();
        const qint64 fontUs = timer.nsecsElapsed() / 1000;
        timer.restart();
        setZoom(1.0);   // геометрия собрана в единице и за шрифтом не идёт
        applyContentWidth();
        syncTables();
        syncFormulas();
        const qint64 afterUs = timer.nsecsElapsed() / 1000;
        std::printf("масштаб %3d %%: шрифт+вёрстка %6lld мкс, наше поверх %6lld мкс, "
                    "высота %.0f\n",
                    int(scale_ * 100.0 + 0.5), static_cast<long long>(fontUs),
                    static_cast<long long>(afterUs), double(size));
        std::fflush(stdout);
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
        std::vector<zametti::Piece> blocks;
        zametti::NoteHeader ignored;

        QElapsedTimer timer;
        timer.start();
        zametti::parsePieces(source, blocks, ignored);
        const qint64 parseUs = timer.nsecsElapsed() / 1000;

        timer.restart();
        zametti::buildDocument(blocks, *fresh, {codeAsOneBlock_});
        const qint64 buildUs = timer.nsecsElapsed() / 1000;
        if (canonical_.empty()) canonical_ = zametti::writePieces(blocks);

        setDocument(fresh);
        setZoom(byRebuild_ ? scale_ : 1.0);
        applyContentWidth();
        syncTables();
        syncFormulas();

        timer.restart();
        const qreal height = document()->documentLayout()->documentSize().height();
        const qint64 layoutUs = timer.nsecsElapsed() / 1000;

        std::printf("--- блок кода %s: логических блоков %zu, QTextBlock %d, знаков %d\n"
                    "    разбор %6lld мкс, сборка %6lld мкс, вёрстка %6lld мкс, высота %.0f\n",
                    codeAsOneBlock_ ? "ОДНИМ QTextBlock" : "построчно", blocks.size(),
                    document()->blockCount(), document()->characterCount(),
                    static_cast<long long>(parseUs), static_cast<long long>(buildUs),
                    static_cast<long long>(layoutUs), double(height));
        std::fflush(stdout);

        showState();
        rebuilding_ = false;
    }

    void showState() {
        setWindowTitle(
            QStringLiteral("%1 — высота строки: %2 — зум: %3 — код: %4 — масштаб %5 %")
                .arg(QFileInfo(path_).fileName())
                .arg(QString::fromUtf8(modeName(zametti::appearance().lineHeightMode)))
                .arg(byRebuild_ ? QStringLiteral("пересборкой") : QStringLiteral("шрифтом"))
                .arg(codeAsOneBlock_ ? QStringLiteral("одним блоком")
                                     : QStringLiteral("построчно"))
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

    // ГЛАВНОЕ, А НЕ ЧИСЛА: пережил ли круг «документ → файл» смену способа.
    // Разделители U+2028 помечены BreakSourceProperty, и читатель обязан
    // вернуть из них перевод строки — то есть байты файла не меняются вовсе.
    // ЦЕНА ВТОРОЙ КОПИИ. Обход живого документа сам по себе против обхода с
    // укладыванием блоков в вектор: разница и есть то, что стоит Piece как
    // значение, а не как ручка.
    void weighPieces() {
        QElapsedTimer timer;
        qint64 walkUs = 0;
        qint64 materialiseUs = 0;
        size_t bytes = 0;
        size_t count = 0;
        for (int round = 0; round < 5; ++round) {
            timer.restart();
            size_t seen = 0;
            zametti::walkPieces(*document(), [&](const zametti::Piece& piece) {
                seen += piece.text.size();   // трогаем, чтобы обход не выбросили
                return true;
            });
            const qint64 bare = timer.nsecsElapsed() / 1000;

            timer.restart();
            std::vector<zametti::Piece> all;
            zametti::walkPieces(*document(), [&](const zametti::Piece& piece) {
                all.push_back(piece);
                return true;
            });
            const qint64 full = timer.nsecsElapsed() / 1000;

            if (round == 0 || bare < walkUs) walkUs = bare;
            if (round == 0 || full < materialiseUs) materialiseUs = full;
            count = all.size();
            bytes = all.size() * sizeof(zametti::Piece);
            for (const zametti::Piece& one : all) {
                bytes += one.text.capacity() + one.info.capacity() +
                         one.runs.capacity() * sizeof(zametti::Run);
                for (const zametti::Run& r : one.runs) bytes += r.href.capacity() + r.title.capacity();
            }
            (void)seen;
        }
        std::printf("    обход без копии %5lld мкс, с укладкой в вектор %5lld мкс "
                    "(+%lld %%), блоков %zu, вторая копия %zu КБ\n",
                    static_cast<long long>(walkUs), static_cast<long long>(materialiseUs),
                    static_cast<long long>(walkUs > 0 ? (materialiseUs - walkUs) * 100 / walkUs : 0),
                    count, bytes / 1024);
        std::fflush(stdout);
    }

    void checkRoundTrip() {
        std::vector<zametti::Piece> back;
        zametti::walkPieces(*document(), [&](const zametti::Piece& piece) {
            back.push_back(piece);
            return true;
        });
        const std::string written = zametti::writePieces(back);
        if (written == canonical_) {
            std::printf("    круг «документ → файл» сходится побайтово (%zu Б)\n",
                        written.size());
            return;
        }
        size_t at = 0;
        while (at < written.size() && at < canonical_.size() && written[at] == canonical_[at]) ++at;
        std::printf("    КРУГ РАЗОШЁЛСЯ на байте %zu: ждали [%s] вышло [%s]\n", at,
                    canonical_.substr(at, 40).c_str(), written.substr(at, 40).c_str());
    }

    // Снимок в память — чтобы сравнить два способа попиксельно.
    QImage frameNow() {
        QImage frame(size(), QImage::Format_ARGB32);
        frame.fill(Qt::white);
        render(&frame);
        return frame;
    }

    // ПОПИКСЕЛЬНАЯ СВЕРКА ДВУХ СПОСОБОВ. Числа числами, а вопрос владельца был
    // про показ: одинаково ли выглядит. Идём по документу экранами и на каждом
    // рисуем оба способа в картинку одного размера.
    void compareModes(const QString& dir) {
        int worstAt = -1;
        int worstChannel = 0;
        qint64 worstPixels = 0;
        qint64 totalDiff = 0;
        int frames = 0;

        codeAsOneBlock_ = false;
        rebuild();
        const int step = qMax(1, height() - 40);
        const int last = verticalScrollBar()->maximum();

        for (int at = 0; at <= last; at += step) {
            codeAsOneBlock_ = false;
            rebuild();
            verticalScrollBar()->setValue(at);
            QCoreApplication::processEvents();
            const QImage a = frameNow();

            codeAsOneBlock_ = true;
            rebuild();
            verticalScrollBar()->setValue(at);
            QCoreApplication::processEvents();
            const QImage b = frameNow();

            ++frames;
            qint64 differing = 0;
            int maxChannel = 0;
            for (int y = 0; y < a.height() && y < b.height(); ++y)
                for (int x = 0; x < a.width() && x < b.width(); ++x) {
                    const QRgb p = a.pixel(x, y);
                    const QRgb q = b.pixel(x, y);
                    if (p == q) continue;
                    ++differing;
                    maxChannel = qMax(maxChannel,
                                      qMax(qAbs(qRed(p) - qRed(q)),
                                           qMax(qAbs(qGreen(p) - qGreen(q)),
                                                qAbs(qBlue(p) - qBlue(q)))));
                }
            if (differing > 0)
                std::printf("  прокрутка %6d: точек %5lld, наибольшее отличие канала %d\n", at,
                            static_cast<long long>(differing), maxChannel);
            totalDiff += differing;
            // Худшим считаем экран с самым ЗАМЕТНЫМ отличием, а не с самым
            // частым: сотня точек, разошедшихся на один уровень серого, — это
            // сглаживание, а десяток на сто десять видно глазом.
            if (maxChannel > worstChannel ||
                (maxChannel == worstChannel && differing > worstPixels)) {
                worstChannel = maxChannel;
                worstPixels = differing;
                worstAt = at;
                if (!dir.isEmpty()) {
                    a.save(dir + QStringLiteral("/построчно.png"));
                    b.save(dir + QStringLiteral("/одним-блоком.png"));
                }
            }
        }
        const qint64 area = qint64(width()) * height();
        std::printf("экранов %d, точек на экране %lld: расхождений всего %lld; "
                    "самое заметное — канал %d, точек %lld (прокрутка %d)\n",
                    frames, static_cast<long long>(area), static_cast<long long>(totalDiff),
                    worstChannel, static_cast<long long>(worstPixels), worstAt);
        std::fflush(stdout);
    }

    void selectAll() {
        QTextCursor whole(document());
        whole.movePosition(QTextCursor::End, QTextCursor::KeepAnchor);
        setTextCursor(whole);
    }

    // ЗАМЕР ОБОИХ СПОСОБОВ НА ОДНОМ ФАЙЛЕ. Меряется то, за что платит
    // просмотрщик: сборка, вёрстка, шаг масштаба и КАДР ПРОКРУТКИ.
    //
    // Кадр — это render() в картинку того же размера, что окно: рисует ровно
    // тот код, что и на экране. Прокрутка идёт сверху вниз по одному экрану,
    // чтобы ленивая разметка Qt успела коснуться всего документа.
    void bench(bool oneBlock) {
        codeAsOneBlock_ = oneBlock;
        setScale(1.0);
        rebuild();
        checkRoundTrip();
        weighPieces();

        // Шаги масштаба: пять вверх и пять вниз, по одному замеру на шаг —
        // печатает сам setScale.
        std::printf("    шаги масштаба:\n");
        for (int i = 0; i < 5; ++i) setScale(scale_ * zametti::appearance().zoomStep);
        for (int i = 0; i < 5; ++i) setScale(scale_ / zametti::appearance().zoomStep);
        setScale(1.0);

        // Кадры прокрутки.
        QImage frame(size(), QImage::Format_ARGB32);
        QScrollBar* bar = verticalScrollBar();
        const int step = qMax(1, height() - 40);
        const int last = bar->maximum();
        QList<qint64> frames;
        for (int at = 0; at <= last; at += step) {
            bar->setValue(at);
            QCoreApplication::processEvents();
            frame.fill(Qt::white);
            QElapsedTimer timer;
            timer.start();
            render(&frame);
            frames.append(timer.nsecsElapsed() / 1000);
        }
        bar->setValue(0);
        if (frames.isEmpty()) return;
        std::sort(frames.begin(), frames.end());
        qint64 total = 0;
        for (qint64 one : frames) total += one;
        std::printf("    кадров %lld: медиана %lld мкс, худший %lld мкс, всего %lld мс\n",
                    static_cast<long long>(frames.size()),
                    static_cast<long long>(frames.at(frames.size() / 2)),
                    static_cast<long long>(frames.last()),
                    static_cast<long long>(total / 1000));
        std::fflush(stdout);
    }

private:
    QString path_;
    // Канон файла, как его пишет обычный путь: с ним сверяется круг после
    // смены способа сборки.
    std::string canonical_;
    qreal scale_ = 1.0;
    // Кегль облика, как он записан в настройках. Зум пересборкой его двигает,
    // и без запомненного значения масштаб копился бы сам на себе.
    qreal basePoint_ = zametti::appearance().baseFontPoint;
    bool byRebuild_ = false;
    bool codeAsOneBlock_ = false;
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
                     "  Ctrl+L         чем задана высота строки\n"
                     "  Ctrl+R         зум шрифтом ↔ пересборкой\n"
                     "  Ctrl+B         блок кода построчно ↔ одним QTextBlock\n"
                     "  --bench        замер обоих способов и выход\n");
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

    // Замером и выйти: zametti-peek файл.md --bench
    if (argc >= 3 && QString::fromLocal8Bit(argv[2]) == QLatin1String("--bench")) {
        peek.show();
        QCoreApplication::processEvents();
        for (bool oneBlock : {false, true, false, true}) peek.bench(oneBlock);
        return 0;
    }

    // Сверкой показа и выйти: zametti-peek файл.md --compare [каталог]
    if (argc >= 3 && QString::fromLocal8Bit(argv[2]) == QLatin1String("--compare")) {
        peek.show();
        QCoreApplication::processEvents();
        peek.compareModes(argc >= 4 ? QString::fromLocal8Bit(argv[3]) : QString());
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
