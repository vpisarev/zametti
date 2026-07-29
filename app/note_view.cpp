#include "note_view.h"

#include "doc_model.h"
#include "marker.h"
#include "settings.h"

#include <QAbstractTextDocumentLayout>
#include <QDir>
#include <QFontMetricsF>
#include <QPainter>
#include <QPaintEvent>
#include <QTextCursor>
#include <QTextFragment>
#include <QResizeEvent>
#include <QPalette>
#include <QScrollBar>
#include <QWidget>
#include <QTextBlock>
#include <QGuiApplication>
#include <QStyleHints>
#include <QTextDocument>
#include <QTextFrame>
#include <QTextLayout>
#include <QTextFrameFormat>

#include <cmath>

namespace zametti {
namespace {

QFont baseFontFor(qreal zoom) {
    QFont font{QString(appearance().fontFamily)};
    font.setPointSizeF(appearance().baseFontPoint * zoom);
    font.setStyleHint(QFont::Monospace);
    return font;
}

// Отбивка после фотографии (и перед ней, когда виден текст строки).
qreal imageGap(qreal zoom) { return 6.0 * zoom; }

}  // namespace

void applyPalette(QWidget& view) {
    QPalette palette = view.palette();
    palette.setColor(QPalette::Base, appearance().pageBackground);
    palette.setColor(QPalette::Highlight, appearance().selectionBackground);
    // Выделение светлое, поэтому текст в нём остаётся тёмным: белый по
    // умолчанию на таком фоне просто пропал бы.
    palette.setColor(QPalette::HighlightedText, palette.color(QPalette::Text));
    view.setPalette(palette);
}

QFont NoteView::baseFont() const { return baseFontFor(zoom_); }

void NoteView::setZoom(qreal zoom) {
    zoom_ = zoom;
}

NoteView::NoteView(QWidget* parent) : QTextBrowser(parent) {
    // Штатную каретку гасим: рисуем свою.
    setCursorWidth(0);
    caretBlink_.setInterval(
        qMax(250, QGuiApplication::styleHints()->cursorFlashTime() / 2));
    connect(&caretBlink_, &QTimer::timeout, this, [this] {
        caretOn_ = !caretOn_;
        viewport()->update(caretRect());
    });
    // Пока человек печатает или ведёт курсор, каретка горит ровно.
    connect(this, &QTextEdit::cursorPositionChanged, this, &NoteView::showCaret);
    connect(this, &QTextEdit::textChanged, this, &NoteView::showCaret);
    // Правка могла родить или убить строку с картинкой — место перемеряется
    // после каждой. Свои же выставления полей отсекает syncingImages_.
    connect(this, &QTextEdit::textChanged, this, &NoteView::syncImageSpace);
    // Тонировка выделенной фотографии — своя отрисовка, Qt про неё не знает;
    // перерисовка на каждой смене выделения (снятие позицию не двигает и
    // cursorPositionChanged не даёт).
    connect(this, &QTextEdit::selectionChanged, this,
            [this] { viewport()->update(); });
}

// Прямоугольник каретки с запасом: перерисовываем чуть больше, чем красим,
// иначе от неё остаётся след.
QRect NoteView::caretRect() const {
    QRect at = cursorRect();
    at.setWidth(qMax(1, qRound(appearance().caretWidth * zoom_)));
    return at.adjusted(-2, -2, 4, 2);
}

void NoteView::showCaret() {
    caretOn_ = true;
    if (hasFocus() && !isReadOnly()) caretBlink_.start();
    // Целиком, а не по прямоугольнику: курсор мог только что уехать, и на
    // прежнем месте осталась бы нарисованная каретка.
    viewport()->update();
}

void NoteView::focusInEvent(QFocusEvent* event) {
    QTextBrowser::focusInEvent(event);
    showCaret();
}

void NoteView::focusOutEvent(QFocusEvent* event) {
    QTextBrowser::focusOutEvent(event);
    caretBlink_.stop();
    caretOn_ = false;
    viewport()->update();
}

void NoteView::repaintOverNativeCaret(QPainter& painter) {
    if (isReadOnly()) return;
    QRect col = cursorRect();
    col = QRect(col.left() - 3, col.top() - 2, 10, col.height() + 4);

    painter.save();
    painter.translate(-horizontalScrollBar()->value(), -verticalScrollBar()->value());
    const QRectF clip(col.translated(horizontalScrollBar()->value(),
                                     verticalScrollBar()->value()));
    painter.setClipRect(clip);
    painter.fillRect(clip, appearance().pageBackground);
    paintCodeBackground(painter, clip);

    QAbstractTextDocumentLayout::PaintContext ctx;
    ctx.palette = palette();
    ctx.clip = clip;
    ctx.cursorPosition = -1;   // ради этого всё и затевалось
    if (textCursor().hasSelection()) {
        // Выделение — как его собрал бы сам Qt, иначе колонка выпадала бы из
        // подсветки.
        QAbstractTextDocumentLayout::Selection selection;
        selection.cursor = textCursor();
        selection.format.setBackground(palette().brush(QPalette::Highlight));
        selection.format.setForeground(palette().brush(QPalette::HighlightedText));
        ctx.selections.append(selection);
    }
    document()->documentLayout()->draw(&painter, ctx);
    painter.restore();
}

void NoteView::applyContentWidth() {
    // Поля и предел ширины заданы в ширинах "A" — той же мерой, что и в
    // сборщике документа, иначе при смене гарнитуры они разъехались бы.
    const qreal charUnit =
        QFontMetricsF(baseFontFor(zoom_)).horizontalAdvance(QLatin1Char('A'));
    const qreal side = appearance().sideMargin * charUnit;
    qreal margin = side;

    if (appearance().maxContentWidth > 0.0) {
        const qreal limit = appearance().maxContentWidth * charUnit;
        const qreal extra = (viewport()->width() - 2 * side - limit) / 2;
        if (extra > 0.0) margin = side + extra;
    }

    QTextFrame* root = document()->rootFrame();
    QTextFrameFormat format = root->frameFormat();
    // Сравнение с допуском, а не на равенство: иначе каждый вызов переразмечал
    // бы документ заново.
    if (std::fabs(format.leftMargin() - margin) < 0.01) {
        // Поля на месте, но вызывают нас и после пересборки документа — а
        // пересборка ставит нижние поля по нулям, и место под фотографии
        // надо вернуть.
        syncImageSpace();
        return;
    }
    format.setLeftMargin(margin);
    format.setRightMargin(margin);
    changingLayout_ = true;
    root->setFrameFormat(format);
    // Пустой документ от смены полей не переразмечается: размечать в нём нечего.
    // Каретка тогда остаётся у прежнего поля и кеглем по умолчанию — в широком
    // окне это выглядело как «в пустой заметке каретки нет вовсе». Просим
    // разметить блок явно.
    //
    // Именно здесь, а не при пересборке: при запуске заметка открывается ещё до
    // show(), в узком окне, и колонку двигает уже первое изменение размера —
    // пересборки при этом нет вовсе.
    document()->markContentsDirty(0, qMax(1, document()->characterCount()));
    changingLayout_ = false;

    // Ширина колонки сменилась — фотографии могли стать шире или уже колонки,
    // и место под них надо перемерить.
    syncImageSpace();
}

void NoteView::resizeEvent(QResizeEvent* event) {
    QTextBrowser::resizeEvent(event);
    applyContentWidth();
}

void NoteView::setImageBase(const QString& dir) {
    if (imageBase_ == dir) return;
    imageBase_ = dir;
    imageCache_.clear();
    syncImageSpace();
}

const QImage* NoteView::imageFor(const QString& path) {
    if (path.isEmpty()) return nullptr;
    QString abs = QDir::isAbsolutePath(path)
                      ? path
                      : (imageBase_.isEmpty() ? QString() : QDir(imageBase_).filePath(path));
    if (abs.isEmpty()) return nullptr;
    abs = QDir::cleanPath(abs);
    auto it = imageCache_.find(abs);
    if (it == imageCache_.end()) it = imageCache_.insert(abs, QImage(abs));
    return it->isNull() ? nullptr : &it.value();
}

QSizeF NoteView::imageDisplaySize(const QImage& image, qreal widthHint,
                                  const QTextBlock& block) const {
    if (image.width() <= 0 || image.height() <= 0) return {};
    // Своя ширина картинки — в физических пикселях; на экране она занимает
    // столько логических, чтобы пиксели легли один в один (HiDPI). Явная
    // ширина ("|315") — уже логическая, как её видит Obsidian.
    const qreal natural = image.width() / devicePixelRatioF();
    qreal width = (widthHint > 0.0 ? widthHint : natural) * zoom_;

    // Шире колонки фотографии не бывать.
    const QTextFrameFormat root = document()->rootFrame()->frameFormat();
    const qreal available = viewport()->width() - root.leftMargin() - root.rightMargin() -
                            block.blockFormat().leftMargin();
    if (available > 16.0 && width > available) width = available;
    if (width < 1.0) width = 1.0;
    return QSizeF(width, width * image.height() / image.width());
}

NoteView::ImageGeometry NoteView::imageGeometry(const QTextBlock& block) {
    const BlockImageRef ref = blockImageRef(block);
    if (!ref.valid) return {};
    const QImage* image = imageFor(ref.path);
    if (image == nullptr) return {};

    qreal widthHint = ref.widthHint;
    if (block.blockNumber() == imageDragBlock_ && imageDragWidth_ > 0.0)
        widthHint = imageDragWidth_;
    const QSizeF size = imageDisplaySize(*image, widthHint, block);
    if (size.isEmpty()) return {};

    const QTextLayout* layout = block.layout();
    if (layout == nullptr) return {};
    // layout->position() отдаёт координаты документа — те же, в которых рисует
    // paintEvent. Высота текста — по числу строк и назначенной высоте, как у
    // подложки кода: длинный путь вики-вложения переносится.
    const qreal assigned = block.blockFormat().lineHeight();
    const int lines = layout->lineCount() > 0 ? layout->lineCount() : 1;
    const qreal textHeight = assigned > 0
                                 ? qMax(layout->boundingRect().height(), lines * assigned)
                                 : layout->boundingRect().height();
    const QPointF textTop = layout->position();

    ImageGeometry geometry;
    geometry.valid = true;
    geometry.line = QRectF(textTop.x(), textTop.y(),
                           qMax(size.width(), layout->boundingRect().width()), textHeight);
    geometry.photo = QRectF(textTop, size);
    return geometry;
}

QRectF NoteView::imageRectInViewport(const QTextBlock& block) {
    const ImageGeometry geometry = imageGeometry(block);
    if (!geometry.valid) return {};
    return geometry.photo.translated(-horizontalScrollBar()->value(),
                                     -verticalScrollBar()->value());
}

void NoteView::setImageDragWidth(int blockNumber, qreal width) {
    if (imageDragBlock_ == blockNumber && std::fabs(imageDragWidth_ - width) < 0.01)
        return;
    imageDragBlock_ = width > 0.0 ? blockNumber : -1;
    imageDragWidth_ = width > 0.0 ? width : 0.0;
    syncImageSpace();
    viewport()->update();
}

void NoteView::syncImageSpace() {
    if (syncingImages_) return;
    syncingImages_ = true;
    const qreal gap = imageGap(zoom_);
    for (QTextBlock block = document()->begin(); block.isValid(); block = block.next()) {
        const ImageGeometry geometry = imageGeometry(block);
        qreal want = 0.0;
        if (geometry.valid) {
            // Фотография стоит на месте текста строки и торчит из него вниз.
            want = qMax(0.0, geometry.photo.height() + gap - geometry.line.height());
        }
        QTextBlockFormat format = block.blockFormat();
        // Нижнее поле всех прочих блоков — ноль по построению сборщика, так
        // что ненулевое поле здесь только наше. Сравнение с допуском: каждое
        // выставление формата переразмечает документ.
        if (std::fabs(format.bottomMargin() - want) < 0.5) continue;
        format.setBottomMargin(want);
        changingLayout_ = true;
        QTextCursor cursor(block);
        cursor.setBlockFormat(format);
        changingLayout_ = false;
    }
    syncingImages_ = false;
}

void NoteView::paintImage(QPainter& painter, const QTextBlock& block) {
    const ImageGeometry geometry = imageGeometry(block);
    if (!geometry.valid) return;
    const BlockImageRef ref = blockImageRef(block);
    const QImage* image = imageFor(ref.path);
    if (image == nullptr) return;

    painter.save();
    // Строка хитро-отрисованная: текст закрашивается фоном, фотография встаёт
    // на его место. Каретка рисуется позже и поверх — ей можно.
    painter.fillRect(geometry.line.adjusted(-2, 0, 2, 0), appearance().pageBackground);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.drawImage(geometry.photo, *image);

    // Выделение, задевшее строку, — это выделенная фотография, а не вскрытая
    // разметка: тонировка цветом выделения поверх, примерно 50/50. Каретка,
    // вставшая на строку, — то же самое: мигающая полоска в углу фотографии
    // человеку ничего не говорит, выбранная фотография — говорит (сама
    // полоска гасится в paintEvent).
    const QTextCursor cursor = textCursor();
    const bool selected =
        cursor.hasSelection()
            ? qMin(cursor.anchor(), cursor.position()) <
                      block.position() + block.length() &&
                  qMax(cursor.anchor(), cursor.position()) > block.position()
            : cursor.block() == block;
    if (selected) {
        QColor tint = appearance().selectionBackground;
        tint.setAlpha(128);
        painter.fillRect(geometry.photo, tint);
    }
    painter.restore();
}

void NoteView::paintCodeBackground(QPainter& painter, const QRectF& visible) {
    const QAbstractTextDocumentLayout* layout = document()->documentLayout();
    const int firstVisible = layout->hitTest(QPointF(0, visible.top()), Qt::FuzzyHit);

    painter.setPen(Qt::NoPen);
    painter.setBrush(appearance().codeBackground);
    for (QTextBlock block = document()->findBlock(firstVisible); block.isValid();
         block = block.next()) {
        const QRectF rect = layout->blockBoundingRect(block);
        if (rect.top() > visible.bottom()) break;
        if (rect.bottom() < visible.top()) continue;
        if (isRawBlock(block) || kindOf(block) != Kind::Code) continue;

        // Высоту считаем по числу строк и назначенной высоте строки, а не по
        // прямоугольнику блока: прямоугольник отдаёт естественную высоту, а
        // шаг идёт по назначенной, и разница между ними — та самая полоса.
        // Строк в блоке может быть больше одной: длинная строка кода переносится.
        const qreal assigned = block.blockFormat().lineHeight();
        const int lines = block.layout() != nullptr ? block.layout()->lineCount() : 1;
        const qreal height =
            assigned > 0 ? qMax(rect.height(), lines * assigned) : rect.height();

        // Блок кода внутри пункта списка начинается не от края колонки, а от
        // колонки своего пункта: иначе подложка вылезала бы левее маркера и
        // разрезала список надвое. Собственный отступ кода при этом не в счёт —
        // на верхнем уровне подложка как шла почти во всю колонку, так и идёт.
        const qreal charUnit =
            QFontMetricsF(baseFontFor(zoom_)).horizontalAdvance(QLatin1Char('A'));
        const qreal shift = qMax(0.0, block.blockFormat().leftMargin() -
                                          appearance().codeIndent * charUnit);
        painter.drawRect(
            QRectF(rect.left() + shift, rect.top(), rect.width() - shift, height));
    }
}

void NoteView::paintEvent(QPaintEvent* event) {
    {
        // Рисуем до текста: сам виджет виден только там, где Qt уже стёр фон, а
        // текст ляжет поверх нашей заливки.
        QPainter painter(viewport());
        painter.translate(-horizontalScrollBar()->value(), -verticalScrollBar()->value());
        const QRectF visible(horizontalScrollBar()->value() + event->rect().x(),
                             verticalScrollBar()->value() + event->rect().y(),
                             event->rect().width(), event->rect().height());
        paintCodeBackground(painter, visible);
    }
    QTextBrowser::paintEvent(event);

    const QFont base = baseFontFor(zoom_);
    QPainter painter(viewport());
    repaintOverNativeCaret(painter);
    painter.translate(-horizontalScrollBar()->value(), -verticalScrollBar()->value());

    const QRectF visible(horizontalScrollBar()->value() + event->rect().x(),
                         verticalScrollBar()->value() + event->rect().y(),
                         event->rect().width(), event->rect().height());

    // К первому видимому блоку идём поиском по раскладке, а не обходом от
    // начала документа: обход стоит тем дороже, чем ниже прокрутка, и на
    // заметке в тысячу блоков это уже заметно.
    const QAbstractTextDocumentLayout* layout = document()->documentLayout();
    const int firstVisible = layout->hitTest(QPointF(0, visible.top()), Qt::FuzzyHit);
    for (QTextBlock block = document()->findBlock(firstVisible); block.isValid();
         block = block.next()) {
        const QRectF rect = layout->blockBoundingRect(block);
        if (rect.top() > visible.bottom()) break;
        // Фотография живёт в нижнем поле блока и может быть видна, когда сама
        // строка уже уехала вверх, — поэтому отсечение с запасом на поле.
        if (rect.bottom() + block.blockFormat().bottomMargin() < visible.top()) continue;
        paintMarker(painter, block, base);
        paintDivider(painter, block, rect, zoom_);
        paintImage(painter, block);
    }

    // Каретка — последней и без сдвига на прокрутку: cursorRect уже отдаёт
    // координаты вьюпорта. При выделении не рисуется вовсе: там видно и так, а
    // мигающая полоска на краю выделения только мешает. На строке-фотографии
    // тоже: там выбор показывает тонировка, а не полоска в углу картинки.
    painter.resetTransform();
    if (caretOn_ && hasFocus() && !isReadOnly() && !textCursor().hasSelection() &&
        !imageGeometry(textCursor().block()).valid) {
        QRect at = cursorRect();
        at.setWidth(qMax(1, qRound(appearance().caretWidth * zoom_)));
        painter.fillRect(at, appearance().caretColor);
    }
}

}  // namespace zametti
