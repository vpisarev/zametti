#include "export_pdf.h"

#include "document_builder.h"
#include "note_view.h"
#include "parser.h"
#include "resources.h"
#include "settings.h"

#include <QAbstractTextDocumentLayout>
#include <QFile>
#include <QFileInfo>
#include <QPageLayout>
#include <QPainter>
#include <QPdfWriter>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextLayout>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace zametti {
namespace {

// Единицы вёрстки — те же, что на экране: логический пиксель при 96 точках на
// дюйм. Так страница получается той же меры, что и окно, и всё, что в облике
// задано в пикселях (поля, толщина черты, отступы), значит на бумаге ровно то
// же самое. В этих же единицах живёт и само устройство PDF (см. setResolution
// ниже) — так что пересчитывать координаты не приходится вовсе.
constexpr qreal kLayoutDpi = 96.0;

// Виджет нужен только затем, чтобы добраться до imageGeometry: где на странице
// лежит фотография, знает отрисовка, и никто больше. Наследуемся вместо того,
// чтобы открывать метод всем: бумага — не повод менять договор вида.
class PaperView : public NoteView {
public:
    using NoteView::ImageGeometry;
    using NoteView::imageGeometry;
    using NoteView::renderSlice;
};

std::vector<Unbreakable> collectUnbreakables(PaperView& view) {
    std::vector<Unbreakable> out;
    const QTextDocument* doc = view.document();
    const QAbstractTextDocumentLayout* layout = doc->documentLayout();
    for (QTextBlock block = doc->begin(); block.isValid(); block = block.next()) {
        // РАЗМЕТКУ БЛОКА ПРИХОДИТСЯ ПРОСИТЬ ЯВНО. Qt размечает блоки лениво —
        // тот, до которого ещё не дошли, строк не имеет вовсе, и разрез про
        // него ничего не узнает. documentSize() тут не помощник: он
        // возвращается за 0.0 мс, ничего не размечая (замер отдельный, при
        // разборе рывков прокрутки). А blockBoundingRect размечает.
        (void)layout->blockBoundingRect(block);
        const QTextLayout* lines = block.layout();
        if (lines != nullptr) {
            // Якорь — position() САМОЙ РАСКЛАДКИ БЛОКА, а не верх его
            // blockBoundingRect: первое даёт координаты документа, второе —
            // прямоугольник вместе с полями, и строки от него уезжают. Первая
            // редакция считала от второго, и на каждой странице верхняя строка
            // оказывалась срезанной пополам.
            const qreal anchor = lines->position().y();
            // Кусок строки тянется ДО НАЧАЛА СЛЕДУЮЩЕЙ, а не до конца букв.
            // Между строками зазора нет: там живут подложки — фон строки кода,
            // фон кода в строке — и разрез, прошедший по такому зазору, оставлял
            // на верхней кромке следующей страницы полоску чужого фона. У
            // последней строки хвост считается до низа блока по той же причине.
            const QRectF rect = layout->blockBoundingRect(block);
            for (int i = 0; i < lines->lineCount(); ++i) {
                const qreal top = anchor + lines->lineAt(i).y();
                const qreal bottom = i + 1 < lines->lineCount()
                                         ? anchor + lines->lineAt(i + 1).y()
                                         : qMax(rect.bottom(), top + lines->lineAt(i).height());
                out.push_back({top, bottom});
            }
        }
        const PaperView::ImageGeometry photo = view.imageGeometry(block);
        // Фотография живёт в нижнем поле блока и в blockBoundingRect не входит
        // — её кромки приходится спрашивать отдельно.
        if (photo.valid) out.push_back({photo.photo.top(), photo.photo.bottom()});
    }
    return out;
}

}  // namespace

std::vector<qreal> pageCuts(std::vector<Unbreakable> hard, qreal docHeight, qreal pageHeight) {
    std::sort(hard.begin(), hard.end(),
              [](const Unbreakable& a, const Unbreakable& b) { return a.top < b.top; });
    std::vector<qreal> cuts{0.0};
    if (pageHeight <= 0.0) {
        cuts.push_back(docHeight);
        return cuts;
    }
    qreal top = 0.0;
    // Потолок на число страниц: заметка конечна, но ошибка в поиске разреза не
    // должна оборачиваться бесконечным файлом.
    for (int guard = 0; guard < 10000; ++guard) {
        if (top + pageHeight >= docHeight) break;
        qreal cut = top + pageHeight;
        for (const Unbreakable& piece : hard) {
            if (piece.top >= cut) break;             // отсортированы по верху
            if (piece.bottom > cut) cut = piece.top;
        }
        // Кусок выше целой страницы (фотография во весь лист): режем как есть,
        // иначе он не влез бы никогда.
        if (cut <= top) cut = top + pageHeight;
        cuts.push_back(cut);
        top = cut;
    }
    cuts.push_back(docHeight);
    return cuts;
}

namespace {

bool readAll(const QString& path, std::string& out) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return false;
    const QByteArray bytes = file.readAll();
    out.assign(bytes.constData(), size_t(bytes.size()));
    return true;
}

}  // namespace

ExportReport exportPdf(const QString& notePath, const QString& targetPath,
                       const PdfOptions& options) {
    ExportReport report;

    std::string text;
    if (!readAll(notePath, text)) {
        report.error = QStringLiteral("не прочитать заметку: %1").arg(notePath);
        return report;
    }

    QPdfWriter writer(targetPath);
    writer.setPageSize(QPageSize(options.page));
    writer.setPageMargins(QMarginsF(options.marginMm, options.marginMm, options.marginMm,
                                    options.marginMm),
                          QPageLayout::Millimeter);
    // РАЗРЕШЕНИЕ PDF РАВНО ЕДИНИЦАМ ВЁРСТКИ, и это не мелочь оформления.
    //
    // Кегли у нас заданы в ПУНКТАХ. Документ разметился под виджет и свои
    // размеры уже запомнил, а вот наши собственные painter.setFont (маркер
    // списка, надпись в рамке непоказанной картинки) переводят пункты в
    // пиксели по устройству, на котором рисуют. При 1200 точках на дюйм маркер
    // выходил ровно в 12.5 раза больше — чёрная дуга во всю страницу поверх
    // текста. Ровно во столько, во сколько 1200 больше 96.
    //
    // Отсюда: устройство должно жить в тех же 96 точках на дюйм, что и вёрстка.
    // Ни точности, ни качества это не стоит — текст и черты в PDF векторные, а
    // координаты вещественные; разрешение картинок задаётся отдельно и от этого
    // числа не зависит.
    writer.setResolution(int(kLayoutDpi));
    writer.setTitle(options.title.isEmpty() ? QFileInfo(targetPath).completeBaseName()
                                            : options.title);
    writer.setCreator(QStringLiteral("zametti"));

    const QRectF paint = writer.pageLayout().paintRectPixels(writer.resolution());
    const qreal pageWidth = paint.width();
    const qreal pageHeight = paint.height();
    if (pageWidth < 1.0 || pageHeight < 1.0) {
        report.error = QStringLiteral("поля больше самой страницы");
        return report;
    }

    // Вид собирается ровно так же, как в редакторе: разбор, сборка документа,
    // ширина колонки. Всё остальное — облик, шрифты, подложки — уже внутри
    // сборщика и вида, и повторять это здесь незачем.
    // Шрифты — из ресурсов, а не «какие найдутся». Программа регистрирует их
    // при запуске, но вывоз не вправе на это полагаться: без них Qt молча
    // подставит что-нибудь своё, и страница поедет целиком. Повторный вызов
    // ничего не делает.
    loadEmbeddedFonts();

    PaperView view;
    view.setFrameShape(QFrame::NoFrame);
    view.setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    view.setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    applyPalette(view);
    view.setImageBase(QFileInfo(notePath).absolutePath());
    // Вьюпорт обязан стать РОВНО страницей: по нему считаются и ширина колонки,
    // и место под фотографии. У виджета же есть рамка и полосы прокрутки, и он
    // всегда шире своего вьюпорта — на сколько именно, спрашиваем у него.
    // ВИДЖЕТ ПРИХОДИТСЯ ПОКАЗАТЬ — но не на экран. Спрятанному виджету Qt
    // событие о новом размере не шлёт, а откладывает до show(): вьюпорт
    // остаётся прежним, и всё, что от него считается (ширина колонки, место под
    // фотографии), берётся с потолка. Замер: после resize(680) вьюпорт отдавал
    // 638, а каждый следующий заход подгонки прибавлял ещё 42.
    //
    // WA_DontShowOnScreen даёт ровно то, что нужно: виджет считается видимым и
    // раскладывается сразу, но окна не заводит и на экране не мелькает.
    const int wantWidth = int(std::lround(pageWidth));
    const int wantHeight = int(std::lround(pageHeight));
    view.setAttribute(Qt::WA_DontShowOnScreen);
    view.show();
    view.resize(wantWidth, wantHeight);
    // Полосы прокрутки погашены, рамки нет — вьюпорт обязан совпасть со
    // страницей. Если вдруг нет, подгоняем: лучше кривая страница, чем текст,
    // уехавший за её край.
    if (view.viewport()->width() != wantWidth || view.viewport()->height() != wantHeight)
        view.resize(view.width() + wantWidth - view.viewport()->width(),
                    view.height() + wantHeight - view.viewport()->height());

    const Document ir = parse(text);
    buildDocument(ir, *view.document(), 1.0);
    view.applyContentWidth();
    // Ширину разметки ставим явно, хотя показанный виджет ставит её и сам:
    // разметка обязана идти по ширине страницы, и полагаться тут на побочное
    // действие show() не стоит.
    view.document()->setTextWidth(view.viewport()->width());
    // Разметка целиком нужна ЗДЕСЬ по-настоящему: без неё у блоков нет ни
    // строк, ни высоты, а по ним и ищется место разреза.
    const qreal docHeight = view.document()->documentLayout()->documentSize().height();
    if (docHeight <= 0.0) {
        report.error = QStringLiteral("в заметке нечего печатать");
        return report;
    }

    const std::vector<qreal> cuts = pageCuts(collectUnbreakables(view), docHeight, pageHeight);

    QPainter painter;
    if (!painter.begin(&writer)) {
        report.error = QStringLiteral("не создать PDF: %1").arg(targetPath);
        return report;
    }
    // Сколько пикселей самой картинки приходится на логическую единицу
    // страницы. От разрешения устройства это не зависит: там, где вёрстка
    // отмерила два сантиметра, фотография ляжет своими пикселями, и их число
    // задаётся здесь и только здесь.
    const qreal imageRatio = qMax(1.0, qreal(options.imageDpi) / kLayoutDpi);

    for (size_t i = 0; i + 1 < cuts.size(); ++i) {
        if (i > 0) writer.newPage();
        const qreal top = cuts[i];
        const qreal bottom = qMin(cuts[i + 1], top + pageHeight);
        painter.save();
        painter.translate(0.0, -top);
        view.renderSlice(painter, QRectF(0.0, top, pageWidth, bottom - top), imageRatio);
        painter.restore();
    }
    painter.end();

    report.pages = int(cuts.size()) - 1;
    return report;
}

}  // namespace zametti
