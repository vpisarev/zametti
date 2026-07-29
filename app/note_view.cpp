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

// Отбивка между строкой-подписью и фотографией и после фотографии.
qreal imageGap(qreal zoom) { return 6.0 * zoom; }

// Картинка, которую блок просит показать. Модель текста об этом не знает:
// блок остаётся обычным абзацем, а фотография — дело вида.
struct BlockImageRef {
    QString path;
    qreal widthHint = 0.0;   // 0 — своя ширина картинки
    bool valid = false;
};

BlockImageRef blockImageRef(const QTextBlock& block) {
    if (!block.isValid() || isRawBlock(block)) return {};
    if (kindOf(block) != Kind::Paragraph) return {};

    // Image-спан целым абзацем: каждый кусок помечен SpanImage с одним путём.
    // Картинка в середине текста фотографией не показывается — только стилем.
    QString href;
    bool whole = true;
    for (QTextBlock::iterator it = block.begin(); !it.atEnd(); ++it) {
        const QTextFragment fragment = it.fragment();
        if (!fragment.isValid() || fragment.text().isEmpty()) continue;
        const QTextCharFormat format = fragment.charFormat();
        const bool image = (format.intProperty(SpanStyleProperty) & SpanImage) != 0 &&
                           !format.anchorHref().isEmpty();
        if (!image || (!href.isEmpty() && href != format.anchorHref())) {
            whole = false;
            break;
        }
        href = format.anchorHref();
    }
    if (whole && !href.isEmpty()) return {href, 0.0, true};

    // Вики-вложение Obsidian: строка целиком "![[путь]]" или "![[путь|ширина]]".
    // Модель хранит его дословным текстом абзаца (см. бриф: wikilinks не
    // переписываются), но фотографию по нему показать можно и нужно.
    const QString text = block.text().trimmed();
    if (!text.startsWith(QStringLiteral("![[")) || !text.endsWith(QStringLiteral("]]")))
        return {};
    QString inner = text.mid(3, text.size() - 5);
    if (inner.isEmpty() || inner.contains(QStringLiteral("]]"))) return {};
    qreal width = 0.0;
    const qsizetype bar = inner.lastIndexOf(QLatin1Char('|'));
    if (bar >= 0) {
        // После черты либо ширина, либо подпись (Obsidian допускает обе);
        // подпись фотографии не мешает — просто остаётся своя ширина.
        bool ok = false;
        const double w = inner.mid(bar + 1).trimmed().toDouble(&ok);
        if (ok && w > 0.0) width = w;
        inner = inner.left(bar);
    }
    inner = inner.trimmed();
    if (inner.isEmpty()) return {};
    return {inner, width, true};
}

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

void NoteView::syncImageSpace() {
    if (syncingImages_) return;
    syncingImages_ = true;
    const qreal gap = imageGap(zoom_);
    for (QTextBlock block = document()->begin(); block.isValid(); block = block.next()) {
        const BlockImageRef ref = blockImageRef(block);
        qreal want = 0.0;
        if (ref.valid) {
            if (const QImage* image = imageFor(ref.path))
                want = imageDisplaySize(*image, ref.widthHint, block).height() + 2.0 * gap;
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

void NoteView::paintImage(QPainter& painter, const QTextBlock& block, const QRectF& rect) {
    const qreal reserved = block.blockFormat().bottomMargin();
    if (reserved <= 1.0) return;   // место не резервировали — и рисовать нечего
    const BlockImageRef ref = blockImageRef(block);
    if (!ref.valid) return;
    const QImage* image = imageFor(ref.path);
    if (image == nullptr) return;

    const QSizeF size = imageDisplaySize(*image, ref.widthHint, block);
    if (size.isEmpty()) return;

    // Якорь — низ собственно текста: layout->position() отдаёт координаты
    // документа, те же, в которых рисует и весь paintEvent.
    const QTextLayout* layout = block.layout();
    if (layout == nullptr) return;
    const qreal top = layout->position().y() + layout->boundingRect().height() +
                      imageGap(zoom_);
    const qreal left = rect.left() + block.blockFormat().leftMargin();

    painter.save();
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.drawImage(QRectF(QPointF(left, top), size), *image);
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
        paintImage(painter, block, rect);
    }

    // Каретка — последней и без сдвига на прокрутку: cursorRect уже отдаёт
    // координаты вьюпорта. При выделении не рисуется вовсе: там видно и так, а
    // мигающая полоска на краю выделения только мешает.
    painter.resetTransform();
    if (caretOn_ && hasFocus() && !isReadOnly() && !textCursor().hasSelection()) {
        QRect at = cursorRect();
        at.setWidth(qMax(1, qRound(appearance().caretWidth * zoom_)));
        painter.fillRect(at, appearance().caretColor);
    }
}

}  // namespace zametti
