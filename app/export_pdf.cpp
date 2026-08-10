#include "export_pdf.h"

#include "doc_model.h"
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

#include <QUrl>

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

// Заголовок → имя якоря, к которому ведут ссылки вида "#имя". Правило то же,
// каким пользуется markdown-мир: строчные буквы, пробелы в дефис. Отличаются
// эти правила в мелочи — что делать со знаками препинания, — поэтому имён
// заводим ДВА: со знаками и без. Стоит это ничего, а ссылка попадает в цель
// при обоих написаниях.
QStringList anchorNamesFor(const QString& heading) {
    QString kept;
    QString stripped;
    for (const QChar ch : heading.simplified().toLower()) {
        const bool word = ch.isLetterOrNumber() || ch == QLatin1Char('-') ||
                          ch == QLatin1Char('_');
        if (ch.isSpace()) {
            kept.append(QLatin1Char('-'));
            stripped.append(QLatin1Char('-'));
        } else if (word) {
            kept.append(ch);
            stripped.append(ch);
        } else {
            kept.append(ch);   // знак препинания остаётся только в первом
        }
    }
    QStringList out{kept};
    if (stripped != kept && !stripped.isEmpty()) out << stripped;
    return out;
}

}  // namespace

void prepareForPaper(QTextDocument& doc) {
    QTextCursor caret(&doc);
    caret.beginEditBlock();
    // КОММЕНТАРИЙ ОТДЕЛЬНЫМ БЛОКОМ. Род Html — это «понятый HTML-блок», и
    // единственный понятый вид у нас сегодня как раз комментарий (см. HtmlKind
    // в ir.h). Непонятый HTML остаётся дословным куском и на бумагу попадает:
    // это уже не записка себе, а текст, который человек написал руками.
    //
    // Вместе с комментарием уходит и пустая строка за ним — иначе на её месте
    // осталась бы дыра там, где ничего не было видно и раньше.
    std::vector<QTextBlock> comments;
    for (QTextBlock block = doc.begin(); block.isValid(); block = block.next()) {
        if (isRawBlock(block) || kindOf(block) != Kind::Html) continue;
        comments.push_back(block);
        const QTextBlock after = block.next();
        if (after.isValid() && isVSpaceBlock(after)) {
            comments.push_back(after);
            block = after;
        }
    }
    // С конца: удаление блока сдвигает позиции всего, что за ним.
    for (auto it = comments.rbegin(); it != comments.rend(); ++it) {
        QTextCursor kill(*it);
        kill.select(QTextCursor::BlockUnderCursor);
        kill.removeSelectedText();
        // BlockUnderCursor не забирает разделитель у САМОГО ПЕРВОГО блока —
        // от него осталась бы пустая строка.
        if (kill.atStart() && kill.block().text().isEmpty() && !kill.atEnd()) kill.deleteChar();
    }

    for (QTextBlock block = doc.begin(); block.isValid(); block = block.next()) {
        // ЗАГОЛОВОК СТАНОВИТСЯ ЦЕЛЬЮ. Внутренние ссылки Qt пишет в PDF как
        // ссылку на именованную цель, а самих целей в файле не было вовсе:
        // заголовки ничем не помечены, и все 49 внутренних ссылок Ficus
        // Tutorial вели в никуда.
        if (!isRawBlock(block) && kindOf(block) == Kind::Heading) {
            QTextCursor mark(block);
            mark.select(QTextCursor::BlockUnderCursor);
            QTextCharFormat anchor;
            anchor.setAnchor(true);
            anchor.setAnchorNames(anchorNamesFor(block.text()));
            mark.mergeCharFormat(anchor);
        }

        // КОММЕНТАРИИ В СТРОКЕ — первым проходом и по одному, с перезапуском
        // обхода: правка делает итератор фрагментов недействительным. Первая
        // редакция после удаления просто выходила из блока — и ссылка, стоящая
        // ЗА комментарием, оставалась неисправленной.
        for (bool again = true; again;) {
            again = false;
            for (QTextBlock::iterator it = block.begin(); !it.atEnd(); ++it) {
                const QTextFragment fragment = it.fragment();
                if (!fragment.isValid()) continue;
                if ((fragment.charFormat().intProperty(SpanStyleProperty) & SpanComment) == 0)
                    continue;

                int from = fragment.position();
                int to = from + fragment.length();
                // «Текст с <!-- х --> внутри» оставил бы двойной пробел: один
                // перед комментарием, другой за ним. Забираем один — но только
                // ОБЫЧНЫЙ: неразрывный у нас значит отступ или выравнивание, и
                // трогать его нельзя.
                const QString line = block.text();
                const int left = from - block.position() - 1;
                const int right = to - block.position();
                if (left >= 0 && left < line.size() && line.at(left) == QLatin1Char(' ') &&
                    right < line.size() && line.at(right) == QLatin1Char(' '))
                    ++to;

                QTextCursor kill(&doc);
                kill.setPosition(from);
                kill.setPosition(to, QTextCursor::KeepAnchor);
                kill.removeSelectedText();
                again = true;
                break;
            }
        }

        for (QTextBlock::iterator it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            if (!fragment.isValid()) continue;
            QTextCharFormat format = fragment.charFormat();
            const int style = format.intProperty(SpanStyleProperty);
            if (format.anchorHref().isEmpty()) continue;

            QTextCursor at(&doc);
            at.setPosition(fragment.position());
            at.setPosition(fragment.position() + fragment.length(), QTextCursor::KeepAnchor);

            QTextCharFormat fixed = format;
            if ((style & SpanImage) != 0) {
                // Строку с фотографией закрывает сама фотография, а ссылка под
                // ней остаётся кликабельной — щелчок по снимку вёл бы на файл
                // вложения, которого рядом с PDF нет.
                //
                // Гасится ТОЛЬКО признак ссылки, а адрес остаётся на месте:
                // именно по нему вид и узнаёт строку-фотографию. Первый заход
                // стирал адрес — и снимок пропадал со страницы вовсе.
                fixed.setAnchor(false);
            } else {
                // URI Qt пишет в файл однобайтовым, и всё, что вне ASCII,
                // превращается в "?" — так "Red–black tree" в Ficus Tutorial
                // стал "Red?black tree". Приводим к процентной записи сами.
                //
                // Но ТОЛЬКО внешние адреса. Ссылка "#заголовок" — это не адрес,
                // а имя цели внутри файла, и оно обязано совпасть с тем именем,
                // которым помечен заголовок, знак в знак. Процентная запись
                // сделала бы из "сборка-и-запуск" нечто вроде "%D1%81%D0%B1…",
                // и ссылка перестала бы попадать в цель (проверено на файле).
                const QString href = format.anchorHref();
                if (!href.startsWith(QLatin1Char('#'))) {
                    const QUrl url(href);
                    if (!url.isEmpty())
                        fixed.setAnchorHref(QString::fromLatin1(url.toEncoded()));
                }
            }
            at.setCharFormat(fixed);
        }
    }
    caret.endEditBlock();
}

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

    // ОБЛИК НА ВРЕМЯ ВЫВОЗА ПОДМЕНЯЕТСЯ. Сборщик документа и вид спрашивают
    // шрифты у глобального appearance(), и другого способа сказать им «сейчас
    // мы на бумаге» нет — кроме как протащить облик параметром через десяток
    // мест, которые о бумаге знать не должны.
    //
    // Подменяются ТОЛЬКО шрифты и кегли: цвета, поля, ритм страницы у бумаги
    // те же, что на экране, — она и должна выглядеть как то, что человек
    // видит. Возвращается облик на месте, чем бы вывоз ни кончился.
    struct PaperLook {
        Appearance saved = appearance();
        ~PaperLook() { appearance() = saved; }
    } look;
    {
        Appearance& a = appearance();
        const Appearance::Pdf& paper = look.saved.pdf;
        if (!paper.fontFamily.isEmpty()) a.fontFamily = paper.fontFamily;
        if (paper.pointSize > 0.0) a.baseFontPoint = paper.pointSize;
        if (!paper.codeFamily.isEmpty()) a.codeFamily = paper.codeFamily;
        if (paper.codePointSize > 0.0) a.codePointSize = paper.codePointSize;
        a.headingScale = paper.headingScale;
    }
    const Appearance::Pdf& paper = look.saved.pdf;

    QPdfWriter writer(targetPath);
    writer.setPageSize(QPageSize(options.page));
    const qreal margin = options.marginMm > 0.0 ? options.marginMm : paper.marginMm;
    writer.setPageMargins(QMarginsF(margin, margin, margin, margin), QPageLayout::Millimeter);
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
    prepareForPaper(*view.document());
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
    const qreal marginPx = margin / 25.4 * kLayoutDpi;
    const int dpi = options.imageDpi > 0 ? options.imageDpi : paper.imageDpi;
    const qreal imageRatio = qMax(1.0, qreal(dpi) / kLayoutDpi);

    for (size_t i = 0; i + 1 < cuts.size(); ++i) {
        if (i > 0) writer.newPage();
        const qreal top = cuts[i];
        const qreal bottom = qMin(cuts[i + 1], top + pageHeight);
        painter.save();
        // ФОН — НА ВЕСЬ ЛИСТ, а не только на поле набора. Начало координат у
        // painter'а стоит в углу поля набора, поэтому заливка идёт с запасом в
        // отрицательные координаты: иначе страница выглядела бы как цветная
        // карточка на белом листе.
        painter.fillRect(QRectF(-marginPx, -marginPx, pageWidth + 2 * marginPx,
                                pageHeight + 2 * marginPx),
                         appearance().pageBackground);
        painter.translate(0.0, -top);
        view.renderSlice(painter, QRectF(0.0, top, pageWidth, bottom - top), imageRatio,
                         paper.maxExportedImageSize);
        painter.restore();
    }
    painter.end();

    report.pages = int(cuts.size()) - 1;
    return report;
}

}  // namespace zametti
