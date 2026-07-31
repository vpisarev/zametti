#include "note_view.h"

#include "doc_model.h"
#include "marker.h"
#include "settings.h"

#include <QAbstractTextDocumentLayout>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QImageReader>
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
    // Кэш не чистим: ключ в нём — абсолютный путь, и от смены каталога, от
    // которого разрешаются относительные пути, уже разжатые картинки не
    // портятся. Раньше чистили, и это была единственная его уборка вовсе.
    syncImageSpace();
}

namespace {
int g_imageDecodes = 0;
qint64 g_imageDecodeMicros = 0;
}  // namespace

int NoteView::imageDecodes() { return g_imageDecodes; }
qint64 NoteView::imageDecodeMicros() { return g_imageDecodeMicros; }

void NoteView::resetImageDecodeCounters() {
    g_imageDecodes = 0;
    g_imageDecodeMicros = 0;
}

QString NoteView::absoluteImagePath(const QString& path) const {
    if (path.isEmpty()) return {};
    const QString abs = QDir::isAbsolutePath(path)
                            ? path
                            : (imageBase_.isEmpty() ? QString() : QDir(imageBase_).filePath(path));
    return abs.isEmpty() ? QString() : QDir::cleanPath(abs);
}

void NoteView::touchImage(const QString& key) {
    const qsizetype at = imageOrder_.indexOf(key);
    if (at <= 0) return;   // уже свежайшая или её нет
    imageOrder_.move(at, 0);
}

// Порядок именно такой: сначала добавили, потом убираем лишнее. Спрашивать
// «сколько она весит» до разжатия негде — вес узнаётся из самого декода.
//
// Картинки открытой заметки не вытесняются никогда, поэтому потолок мягкий: на
// одну заметку кэша хватает всегда, пусть она одна и больше бюджета. Так решил
// владелец, и это правило, а не следствие реализации.
void NoteView::trimImageCache(const QString& keep) {
    const qint64 budget = qint64(qMax(8, appearance().imageCacheSizeMb)) * 1024 * 1024;
    for (qsizetype i = imageOrder_.size() - 1; i >= 0 && imageCacheBytes_ > budget; --i) {
        const QString& key = imageOrder_.at(i);
        if (key == keep || currentNoteImages_.contains(key)) continue;
        const auto it = imageCache_.constFind(key);
        if (it != imageCache_.constEnd()) imageCacheBytes_ -= it->bytes;
        imageCache_.remove(key);
        imageOrder_.removeAt(i);
    }
}

const NoteView::CachedImage* NoteView::cachedImage(const QString& path) {
    const QString abs = absoluteImagePath(path);
    if (abs.isEmpty()) return nullptr;
    // Всякий спрос идёт от блока открытой заметки — значит эта картинка её.
    currentNoteImages_.insert(abs);

    auto it = imageCache_.find(abs);
    if (it != imageCache_.end() && it->limit == loadedImageSizeLimit()) {
        touchImage(abs);
        return &it.value();
    }
    if (it != imageCache_.end()) {
        // Предел сменили в конфиге: копия в памяти ужата не так, как надо.
        imageCacheBytes_ -= it->bytes;
        imageCache_.erase(it);
        imageOrder_.removeAll(abs);
    }

    QElapsedTimer decode;
    decode.start();
    // Размеры — из заголовка файла, до всякого разжатия: по ним и решается,
    // ужимать ли, а сами они остаются НАСТОЯЩИМИ размерами картинки и держат
    // вёрстку. Копия в памяти может быть мельче, и это на место не влияет.
    QImageReader reader(abs);
    const QSize declared = reader.size();
    const int limit = loadedImageSizeLimit();
    if (!declared.isEmpty() && limit > 0 &&
        (declared.width() > limit || declared.height() > limit)) {
        // Предел держит ОБЕ стороны. Только вниз: картинка мельче предела
        // остаётся собой. Просим об этом сам читатель — иные форматы умеют
        // разжимать сразу в нужный размер и полной копии не заводят вовсе.
        QSize scaled = declared.scaled(limit, limit, Qt::KeepAspectRatio);
        // У вырожденной картинки (1x1000000) короткая сторона уходит в ноль, а
        // картинки нулевой ширины не бывает.
        scaled.setWidth(qMax(1, scaled.width()));
        scaled.setHeight(qMax(1, scaled.height()));
        reader.setScaledSize(scaled);
    }
    QImage image = reader.read();
    ++g_imageDecodes;
    g_imageDecodeMicros += decode.nsecsElapsed() / 1000;

    CachedImage entry;
    entry.limit = limit;
    if (image.isNull()) {
        // Пустой результат на файле с известными размерами — это отказ Qt
        // разжимать: картинка больше потолка (см. settings.cpp). Отличаем от
        // «файла нет» и запоминаем, чтобы не спрашивать заново на каждом кадре.
        if (!declared.isEmpty()) {
            entry.declared = declared;
            entry.tooBig = true;
        } else {
            return nullptr;
        }
    } else {
        entry.declared = declared.isEmpty() ? image.size() : declared;
        entry.bytes = qint64(image.sizeInBytes());
        entry.image = std::move(image);
    }

    imageCacheBytes_ += entry.bytes;
    imageCache_.insert(abs, std::move(entry));
    imageOrder_.prepend(abs);
    trimImageCache(abs);
    // Заново: вытеснение меняло QHash, а ссылки в нём этого не переживают.
    const auto found = imageCache_.constFind(abs);
    return found == imageCache_.constEnd() ? nullptr : &found.value();
}

const QImage* NoteView::imageFor(const QString& path) {
    const CachedImage* entry = cachedImage(path);
    if (entry == nullptr || entry->image.isNull()) return nullptr;
    return &entry->image;
}

QSizeF NoteView::imageDisplaySize(QSize natural_, qreal widthHint,
                                  const QTextBlock& block) const {
    if (natural_.width() <= 0 || natural_.height() <= 0) return {};
    // Своя ширина картинки — в физических пикселях; на экране она занимает
    // столько логических, чтобы пиксели легли один в один (HiDPI). Явная
    // ширина ("|315") — уже логическая, как её видит Obsidian.
    const qreal natural = natural_.width() / devicePixelRatioF();
    qreal width = (widthHint > 0.0 ? widthHint : natural) * zoom_;

    // Шире колонки фотографии не бывать.
    const QTextFrameFormat root = document()->rootFrame()->frameFormat();
    const qreal available = viewport()->width() - root.leftMargin() - root.rightMargin() -
                            block.blockFormat().leftMargin();
    if (available > 16.0 && width > available) width = available;
    if (width < 1.0) width = 1.0;

    qreal height = width * natural_.height() / natural_.width();
    // Выше нескольких экранов картинку всё равно не рассмотреть, а вырожденная
    // разносит документ: замер на 1x20000 — 19984 px поля под одну строку,
    // документ высотой в двадцать тысяч пикселей; при 1x1000000 это миллион.
    // Ужимаем ОБЕ стороны сразу — картинка остаётся собой, только мельче, и
    // пропорции целы. Правило одно на фотографию и на рамку-заглушку: иначе
    // вёрстка прыгала бы при смене потолка разжатия.
    const qreal tallest = 4.0 * (viewport()->height() > 0 ? viewport()->height() : 1000);
    if (height > tallest) {
        width *= tallest / height;
        height = tallest;
    }
    // Обе стороны — не меньше пикселя: у ленты 20000x1 высота уходила под
    // пиксель, а у 1x1000000 после ужатия по высоте так же уходит ширина.
    return QSizeF(qMax(1.0, width), qMax(1.0, height));
}

NoteView::ImageGeometry NoteView::imageGeometry(const QTextBlock& block) {
    const BlockImageRef ref = blockImageRef(block);
    if (!ref.valid) return {};
    // Слишком большая картинка место занимает наравне с показанной: на её
    // месте стоит рамка с надписью, и вёрстка не прыгнет, если потолок в
    // конфиге поднимут.
    const CachedImage* entry = cachedImage(ref.path);
    if (entry == nullptr) return {};

    qreal widthHint = ref.widthHint;
    if (block.blockNumber() == imageDragBlock_ && imageDragWidth_ > 0.0)
        widthHint = imageDragWidth_;
    const QSizeF size = entry->tooBig ? tooBigBoxSize(block, *entry)
                                      : imageDisplaySize(entry->declared, widthHint, block);
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

    // Выравнивание в колонке. Умолчание — по центру: страница с фотографиями
    // посередине выглядит по-книжному, и ради этого умолчания в файл ничего
    // писать не надо.
    const QTextFrameFormat root = document()->rootFrame()->frameFormat();
    const qreal available = viewport()->width() - root.leftMargin() - root.rightMargin() -
                            block.blockFormat().leftMargin();
    qreal shift = 0.0;
    if (available > size.width()) {
        switch (ref.align) {
            case ImageAlign::Center: shift = (available - size.width()) / 2.0; break;
            case ImageAlign::Right: shift = available - size.width(); break;
            case ImageAlign::Left: break;
        }
    }

    ImageGeometry geometry;
    geometry.valid = true;
    // Строка закрашивается во всю колонку: фотография съехала вбок, а текст
    // под ней остался у левого края, и без этого он выглядывал бы рядом.
    geometry.line = QRectF(textTop.x(), textTop.y(),
                           qMax(available, layout->boundingRect().width()), textHeight);
    geometry.photo = QRectF(textTop + QPointF(shift, 0.0), size);
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
    // Набор незащищаемых от вытеснения собирается заново: он про ТУ заметку,
    // что в документе сейчас. Наполнит его сам обход ниже — cachedImage
    // зовётся ровно для картинок этого документа.
    currentNoteImages_.clear();
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

QString NoteView::tooBigText(const QTextBlock& block, const CachedImage& entry) const {
    return QStringLiteral("%1:\na big %2x%3 image")
        .arg(QFileInfo(blockImageRef(block).path).fileName())
        .arg(entry.declared.width())
        .arg(entry.declared.height());
}

// Небольшой прямоугольник по размеру самой надписи, с полем вокруг. Ни
// пропорций картинки, ни её размеров он не наследует: 1x1000000 растянуло бы
// рамку на миллион пикселей, а показывать в ней всё равно нечего.
QSizeF NoteView::tooBigBoxSize(const QTextBlock& block, const CachedImage& entry) const {
    const QFontMetricsF metrics(baseFont());
    const qreal padding = metrics.height();
    QSizeF box = metrics.boundingRect(QRectF(0, 0, 1e6, 1e6), Qt::AlignLeft | Qt::TextWordWrap,
                                      tooBigText(block, entry))
                     .size();
    box += QSizeF(2 * padding, 2 * padding);

    // Ни шире колонки, ни выше экрана — обе стороны, как и у фотографии.
    const QTextFrameFormat root = document()->rootFrame()->frameFormat();
    const qreal available = viewport()->width() - root.leftMargin() - root.rightMargin() -
                            block.blockFormat().leftMargin();
    if (available > 16.0 && box.width() > available) box.setWidth(available);
    const qreal tallest = viewport()->height() > 0 ? viewport()->height() : 1000;
    if (box.height() > tallest) box.setHeight(tallest);
    return box;
}

// Картинка, которую Qt разжимать отказался: она больше потолка, выведенного из
// бюджета кэша. Показать вместо неё нечего, но и молчать нельзя — человек
// должен увидеть, что случайно положил в хранилище здоровенный файл и его надо
// уменьшить. Поэтому рамка читается как «тут что-то не так»: пунктир, цвет
// непонятого, имя файла и настоящие размеры из его заголовка.
void NoteView::paintTooBigImage(QPainter& painter, const QTextBlock& block,
                                const ImageGeometry& geometry, const CachedImage& entry) {
    QPen pen(appearance().rawColor);
    pen.setStyle(Qt::DashLine);
    pen.setWidthF(qMax(1.0, 1.5 * zoom_));
    painter.setPen(pen);
    painter.drawRect(geometry.photo.adjusted(0.5, 0.5, -0.5, -0.5));

    painter.setFont(baseFont());
    painter.setPen(appearance().rawColor);
    painter.drawText(geometry.photo, Qt::AlignCenter | Qt::TextWordWrap,
                     tooBigText(block, entry));
}

void NoteView::paintImage(QPainter& painter, const QTextBlock& block) {
    const ImageGeometry geometry = imageGeometry(block);
    if (!geometry.valid) return;
    const BlockImageRef ref = blockImageRef(block);
    const CachedImage* entry = cachedImage(ref.path);
    if (entry == nullptr) return;

    painter.save();
    // Строка хитро-отрисованная: текст закрашивается фоном, фотография встаёт
    // на его место. Каретка рисуется позже и поверх — ей можно.
    painter.fillRect(geometry.line.adjusted(-2, 0, 2, 0), appearance().pageBackground);
    if (entry->tooBig) {
        paintTooBigImage(painter, block, geometry, *entry);
    } else {
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.drawImage(geometry.photo, entry->image);
    }

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
    QTextBlock start = document()->findBlock(firstVisible);
    // Шаг назад: когда в кадр сверху въехала только фотография (нижнее поле
    // блока), hitTest по верхней кромке отдаёт уже следующий блок — и без
    // шага картинка пропадала бы целиком, стоило её верху выйти из кадра.
    // Дальше одного блока поле не тянется: следующий блок начинается под ним.
    if (start.isValid() && start.previous().isValid()) start = start.previous();
    for (QTextBlock block = start; block.isValid(); block = block.next()) {
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
