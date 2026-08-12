#include "note_view.h"

#include "doc_model.h"
#include "icons.h"
#include "import_limits.h"
#include "marker.h"
#include "settings.h"

#include <QAbstractTextDocumentLayout>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QImageReader>
#include <QFontMetricsF>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QTextCursor>
#include <QTextFragment>
#include <QResizeEvent>
#include <QPalette>
#include <QScrollBar>
#include <QWidget>
#include <QTextBlock>
#include <QClipboard>
#include <QGuiApplication>
#include <QMouseEvent>
#include <QStringList>
#include <QStyleHints>
#include <QTextDocument>
#include <QTextFrame>
#include <QTextLayout>
#include <QTextFrameFormat>

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

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

// --- плашка блока кода ------------------------------------------------------
//
// Рисуется по геометрии блоков, а не свойствами формата: заливка по
// прямоугольнику блока оставляла бы между строками кода незакрашенные полосы
// (см. paintCodeBackground ниже), а полоски с языком в тексте нет вовсе —
// каретка в неё не попадает, выделение её не берёт, поиск её не видит.
//
// Одна ПОЛОСА на строку кода, а не один прямоугольник на блок: строки блока —
// это отдельные QTextBlock (ContinuationProperty), и обходить их назад до
// начала блока на каждом кадре значило бы платить длиной блока за прокрутку.
// Полосы собираются за один проход сверху вниз, и уже по ним видно, где блок
// начался и где кончился.
// Следующая строка того же блока кода?
bool codeContinues(const QTextBlock& block) {
    const QTextBlock next = block.next();
    return next.isValid() && !isRawBlock(next) && kindOf(next) == Kind::Code &&
           isContinuationBlock(next);
}

// Собственное нижнее поле блока — то, которое стоит в нём НЕ ради картинки.
// Пока такое одно: поле под скругление у последней строки блока кода.
qreal ownBottomMargin(const QTextBlock& block, const CodePlate& plate) {
    if (isRawBlock(block) || kindOf(block) != Kind::Code) return 0.0;
    return codeContinues(block) ? 0.0 : plate.padBottom;
}

QPainterPath platePath(const QRectF& rect, qreal radius, bool roundTop, bool roundBottom) {
    QPainterPath path;
    if (radius <= 0.0 || (!roundTop && !roundBottom)) {
        path.addRect(rect);
        return path;
    }
    const qreal r = qMin(radius, qMin(rect.width(), rect.height()) / 2.0);
    path.moveTo(rect.left(), rect.top() + (roundTop ? r : 0.0));
    if (roundTop) {
        path.arcTo(QRectF(rect.left(), rect.top(), 2 * r, 2 * r), 180, -90);
        path.lineTo(rect.right() - r, rect.top());
        path.arcTo(QRectF(rect.right() - 2 * r, rect.top(), 2 * r, 2 * r), 90, -90);
    } else {
        path.lineTo(rect.left(), rect.top());
        path.lineTo(rect.right(), rect.top());
    }
    if (roundBottom) {
        path.lineTo(rect.right(), rect.bottom() - r);
        path.arcTo(QRectF(rect.right() - 2 * r, rect.bottom() - 2 * r, 2 * r, 2 * r), 0, -90);
        path.lineTo(rect.left() + r, rect.bottom());
        path.arcTo(QRectF(rect.left(), rect.bottom() - 2 * r, 2 * r, 2 * r), 270, -90);
    } else {
        path.lineTo(rect.right(), rect.bottom());
        path.lineTo(rect.left(), rect.bottom());
    }
    path.closeSubpath();
    return path;
}

}  // namespace

void applyPalette(QWidget& view, bool history) {
    QPalette palette = view.palette();
    // В режиме истории поле тонируется: слегка пожелтевший от времени фон
    // (решение владельца). Прошлое видно ещё до того, как человек прочтёт
    // баннер, а совпадение historyBackground с pageBackground выключает
    // тонировку — это законная настройка, а не поломка.
    palette.setColor(QPalette::Base, history ? appearance().historyBackground
                                             : appearance().pageBackground);
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
    connect(this, &QTextEdit::textChanged, this, [this] { syncImageSpace(false); });
    // Тонировка выделенной фотографии — своя отрисовка, Qt про неё не знает;
    // перерисовка на каждой смене выделения (снятие позицию не двигает и
    // cursorPositionChanged не даёт).
    connect(this, &QTextEdit::selectionChanged, this,
            [this] { viewport()->update(); });

    // Галочка «скопировано» гаснет сама: подтверждение, которое не гаснет,
    // через минуту врёт.
    copiedFade_.setSingleShot(true);
    copiedFade_.setInterval(900);
    connect(&copiedFade_, &QTimer::timeout, this, [this] {
        copiedCodeBlock_ = -1;
        viewport()->update();
    });
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

bool NoteView::event(QEvent* e) {
    // ПЛОТНОСТЬ ЭКРАНА МЕНЯЕТСЯ НА ЛЕТУ, и это ломало вёрстку фотографий.
    //
    // Размер фотографии считается как «своя ширина в физических пикселях,
    // делённая на devicePixelRatio»: так пиксели ложатся один в один. Но у
    // виджета этот коэффициент не постоянен — при запуске РАСПАХНУТОГО окна Qt
    // сначала отдаёт 2.00, а когда окно оказывается на экране с дробным
    // масштабом, становится 1.67 (замер на машине владельца).
    //
    // resizeEvent при этом НЕ приходит: логический размер окна тот же. Резерв
    // остаётся посчитанным по старому коэффициенту, а рисуются фотографии по
    // новому — то есть крупнее отведённого места, и наезжают друг на друга.
    //
    // Отсюда и вся картина: ошибка только при запуске, только на распахнутом
    // окне, лечится выходом из полноэкранного режима или переключением заметки
    // — всем, что вызывает полный пересчёт.
    if (e->type() == QEvent::DevicePixelRatioChange) {
        const bool handled = QTextBrowser::event(e);
        applyContentWidth();
        syncImageSpace();
        viewport()->update();
        return handled;
    }
    return QTextBrowser::event(e);
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
qint64 NoteView::budgetBytes() {
    return qint64(qMax(8, appearance().imageCacheSizeMb)) * 1024 * 1024;
}

void NoteView::trimImageCache(const QString& keep, qint64 need) {
    const qint64 budget = budgetBytes();
    // need — вес того, что вот-вот добавят. Отдельного прохода по уборке нет и
    // не надо: вытеснение живёт внутри добавления и срабатывает только когда
    // места не хватает под КОНКРЕТНУЮ картинку. С need = 0 условие вырождается
    // в прежнее «пока не уложились».
    for (qsizetype i = imageOrder_.size() - 1; i >= 0 && imageCacheBytes_ + need > budget; --i) {
        const QString& key = imageOrder_.at(i);
        if (key == keep || currentNoteImages_.contains(key)) continue;
        const auto it = imageCache_.constFind(key);
        if (it != imageCache_.constEnd()) imageCacheBytes_ -= it->bytes;
        imageCache_.remove(key);
        imageOrder_.removeAt(i);
    }
}

int NoteView::shownImageCount() const {
    int count = 0;
    for (const CachedImage& entry : imageCache_)
        if (entry.state == ImageState::Shown) ++count;
    return count;
}

int NoteView::framedImageCount() const {
    int count = 0;
    for (const CachedImage& entry : imageCache_)
        if (entry.framed()) ++count;
    return count;
}

qint64 NoteView::decodedBytes(QSize declared, int limit) {
    QSize shown = declared;
    if (limit > 0 && (shown.width() > limit || shown.height() > limit)) {
        shown = shown.scaled(limit, limit, Qt::KeepAspectRatio);
        shown.setWidth(qMax(1, shown.width()));
        shown.setHeight(qMax(1, shown.height()));
    }
    // Четыре байта на точку: столько занимает разжатая копия в памяти.
    return qint64(shown.width()) * shown.height() * 4;
}

ImageFacts NoteView::caretImage() {
    const QTextBlock block = textCursor().block();
    const BlockImageRef ref = blockImageRef(block);
    if (!ref.valid) return {};
    // Спрашиваем тот же кэш, что и показ: он уже читал заголовок этого файла,
    // а если картинку успели разжать — знает и цвет с глубиной.
    const CachedImage* entry = imageInfo(ref.path);
    ImageFacts facts;
    if (entry != nullptr) facts = entry->facts;
    else readImageFacts(absoluteImagePath(ref.path), facts);
    // Подпись — это alt картинки, и в ней живёт имя исходного файла (см.
    // xmpWithFileName в exif.h). У вики-вложения alt нет: текстом абзаца там
    // стоит сама запись "![[путь]]", и показывать её вместо подписи незачем.
    if (!ref.wiki) facts.caption = block.text().trimmed();
    return facts;
}

const NoteView::CachedImage* NoteView::imageInfo(const QString& path) {
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

    // Только заголовок: размеры есть, пикселей нет и не надо. Место под
    // фотографию считается по НАСТОЯЩИМ размерам, а не по размеру копии.
    const QSize declared = QImageReader(abs).size();
    if (declared.isEmpty()) {
        // Файла нет — рисуем рамку и держим под неё место. Так человек видит,
        // что вложение пропало, а не пустоту; байты ссылки в заметке при этом
        // не трогаются вовсе: вернётся файл — вернётся картинка. То же самое
        // штатно бывает в слепке истории, который ссылается на давно удалённое.
        //
        // Файл, который есть, но картинкой не является, — не наше дело: строка
        // остаётся строкой, как и всякий текст, который мы не поняли.
        if (QFileInfo::exists(abs)) return nullptr;
        CachedImage gone;
        readImageFacts(abs, gone.facts);
        gone.limit = loadedImageSizeLimit();
        gone.state = ImageState::Missing;
        imageCache_.insert(abs, std::move(gone));
        imageOrder_.prepend(abs);
        const auto lost = imageCache_.constFind(abs);
        return lost == imageCache_.constEnd() ? nullptr : &lost.value();
    }

    CachedImage entry;
    entry.declared = declared;
    readImageFacts(abs, entry.facts);
    entry.limit = loadedImageSizeLimit();
    entry.state = ImageState::Pending;
    imageCache_.insert(abs, std::move(entry));
    imageOrder_.prepend(abs);
    const auto found = imageCache_.constFind(abs);
    return found == imageCache_.constEnd() ? nullptr : &found.value();
}

void NoteView::planNoteImages() {
    // Пропавшие файлы перепроверяем: рамка «файл не найден» не имеет права
    // застыть навсегда. Вернулся файл — вернётся и картинка, а стоит проверка
    // одного обращения к файловой системе на картинку, и то не на каждый кадр,
    // а на пересчёт места.
    for (auto it = imageCache_.begin(); it != imageCache_.end();) {
        if (it->state == ImageState::Missing && QFileInfo::exists(it.key())) {
            imageOrder_.removeAll(it.key());
            it = imageCache_.erase(it);
        } else {
            ++it;
        }
    }

    const qint64 budget = qint64(qMax(8, appearance().imageCacheSizeMb)) * 1024 * 1024;
    const int limit = loadedImageSizeLimit();

    // Все картинки этой заметки — от самой лёгкой к самой тяжёлой. Порядок
    // именно такой: так их покажется больше всего.
    std::vector<std::pair<qint64, QString>> mine;
    for (const QString& key : currentNoteImages_) {
        const auto it = imageCache_.constFind(key);
        if (it == imageCache_.constEnd()) continue;
        mine.push_back({decodedBytes(it->declared, limit), key});
    }
    std::sort(mine.begin(), mine.end());

    qint64 taken = 0;
    for (const auto& [cost, key] : mine) {
        auto it = imageCache_.find(key);
        if (it == imageCache_.end()) continue;
        // Отказ Qt разжимать и уже принятое решение «рамка» не пересматриваем:
        // иначе одна и та же картинка мигала бы туда-сюда при каждой правке.
        if (it->state == ImageState::TooBig || it->state == ImageState::Crowded) continue;

        // Потолок Qt на разжатие — тоже по заголовку, до всякого чтения
        // пикселей. Считается он по ПОЛНОМУ размеру: предел стороны тут не
        // помощник, ужимать Qt всё равно будет уже разжатое. Решить это здесь
        // важно: место под картинку резервируется до первой отрисовки, и
        // узнав об отказе только при ней, мы бы держали дырку в тексте
        // размером с несостоявшуюся фотографию.
        const qint64 full = qint64(it->declared.width()) * it->declared.height() * 4;
        if (full > qint64(QImageReader::allocationLimit()) * 1024 * 1024) {
            it->state = ImageState::TooBig;
            continue;
        }

        taken += cost;
        if (taken > budget) it->state = ImageState::Crowded;
    }
}

// Пиксели — лениво, по первому рисованию. Разжимать всю заметку при открытии
// незачем: замер на 25 снимках дал 2.7 с, а видно из них один-два.
const QImage* NoteView::pixelsFor(const QString& key) {
    auto it = imageCache_.find(key);
    if (it == imageCache_.end()) return nullptr;
    if (it->state == ImageState::Shown) return &it->image;
    if (it->state != ImageState::Pending) return nullptr;

    const QSize declared = it->declared;
    const int limit = loadedImageSizeLimit();
    const qint64 cost = decodedBytes(declared, limit);
    // Место под неё — за счёт чужих заметок: свои не трогаем никогда.
    trimImageCache(key, cost);
    if (imageCacheBytes_ + cost > budgetBytes()) {
        // Не влезла даже после вытеснения — рамка. Спрашиваем ДО разжатия:
        // иначе платили бы памятью ровно за то, чего решили не показывать.
        it = imageCache_.find(key);
        if (it != imageCache_.end()) it->state = ImageState::Crowded;
        return nullptr;
    }

    QElapsedTimer decode;
    decode.start();
    QImageReader reader(key);
    if (limit > 0 && (declared.width() > limit || declared.height() > limit)) {
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

    it = imageCache_.find(key);
    if (it == imageCache_.end()) return nullptr;
    if (image.isNull()) {
        // Отказ Qt разжимать: картинка больше потолка (см. settings.cpp).
        // Запоминаем, чтобы не спрашивать заново на каждом кадре.
        it->state = ImageState::TooBig;
        return nullptr;
    }
    it->bytes = qint64(image.sizeInBytes());
    it->image = std::move(image);
    it->state = ImageState::Shown;
    // Раз уж картинку всё равно разжали — забираем заодно и то, что видно
    // только у разжатой копии: цветовое пространство и глубину. Второй раз за
    // ними никто не пойдёт: они лежат рядом с пикселями, в той же записи кэша.
    addDecodedFacts(it->image, it->facts);
    imageCacheBytes_ += it->bytes;
    touchImage(key);
    return &it->image;
}

const QImage* NoteView::imageFor(const QString& path) {
    const CachedImage* entry = imageInfo(path);
    if (entry == nullptr) return nullptr;
    return pixelsFor(absoluteImagePath(path));
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
    const CachedImage* entry = imageInfo(ref.path);
    if (entry == nullptr) return {};

    qreal widthHint = ref.widthHint;
    if (block.blockNumber() == imageDragBlock_ && imageDragWidth_ > 0.0)
        widthHint = imageDragWidth_;
    const QSizeF size = entry->framed() ? frameBoxSize(block, *entry)
                                        : imageDisplaySize(entry->declared, widthHint, block);
    if (size.isEmpty()) return {};

    const QTextLayout* layout = block.layout();
    if (layout == nullptr) return {};
    // layout->position() отдаёт координаты документа — те же, в которых рисует
    // paintEvent. Высота текста — по числу строк и назначенной высоте, как у
    // подложки кода: длинный путь вики-вложения переносится.
    // ДВЕ РАЗНЫЕ ВЫСОТЫ, и путать их нельзя — на этом я и обжёгся.
    //
    // textHeight — высота ТЕКСТА строки. Ею закрашивается строка под
    // фотографией: в блоке лежит "![alt](файл.jxl)", отрисованный цветом
    // ссылки, и он обязан скрыться целиком. Возьмёшь её больше или меньше —
    // текст проступит синей полосой (владелец увидел ровно это).
    //
    // allotted — высота, которую блоку ОТВЁЛ Qt. От неё считается резерв:
    // «сколько добрать сверх того, что блок и так занимает». Она не равна
    // запрошенной: при lineHeight = 22 Qt отводит 20, а неразмеченному блоку —
    // и вовсе ноль, потому что размечает лениво. Вычитая запрошенные 22, мы
    // получали наслоение фотографий: на два пикселя у размеченных блоков и на
    // все двадцать два у хвоста документа.
    //
    // Проявлялось при ПОВТОРНОМ открытии длинной заметки — сразу после вставки
    // документ уже размечен, и всё сходилось.
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
    geometry.allotted = document()->documentLayout()->blockBoundingRect(block).height();
    // Строка закрашивается во всю колонку: фотография съехала вбок, а текст
    // под ней остался у левого края, и без этого он выглядывал бы рядом.
    geometry.line = QRectF(textTop.x(), textTop.y(),
                           qMax(available, layout->boundingRect().width()), textHeight);
    // Фотография опускается на вылет уголков: они рисуются снаружи её края, и
    // без этого верхние уходили бы в полосу предыдущего блока, где их
    // откусывает чужая перерисовка при прокрутке.
    geometry.photo = QRectF(textTop + QPointF(shift, imageCornerOverhang()), size);
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

void NoteView::markImageRegion(int position, int charsAdded) {
    if (syncingImages_) return;   // свои же правки полей перемерять незачем
    const int last = qMax(0, document()->characterCount() - 1);
    const int from = qBound(0, position, last);
    const int to = qBound(from, position + charsAdded, last);
    if (imageDirty_.isNull() || imageDirty_.document() != document()) {
        imageDirty_ = QTextCursor(document());
        imageDirty_.setPosition(from);
        imageDirty_.setPosition(to, QTextCursor::KeepAnchor);
        return;
    }
    imageDirty_.setPosition(qMin(imageDirty_.selectionStart(), from));
    imageDirty_.setPosition(qMax(imageDirty_.selectionEnd(), to), QTextCursor::KeepAnchor);
}

void NoteView::syncImageSpace(bool whole) {
    if (syncingImages_) return;
    syncingImages_ = true;

    // Полный обход — когда меняется не текст, а всё сразу: ширина колонки,
    // масштаб, сам документ. Частичный — когда правка задела кусок и её
    // границы нам сказали. Без границ полный: верно всегда, просто дороже.
    const bool partial = !whole && !imageDirty_.isNull();
    int first = 0;
    int afterLast = document()->blockCount() - 1;
    if (partial) {
        // Соседний блок с каждой стороны — про запас, как и в уборке: правка
        // на границе блоков сливает и делит их.
        first = qMax(0, document()->findBlock(imageDirty_.selectionStart()).blockNumber() - 1);
        afterLast = qMin(afterLast,
                         document()->findBlock(imageDirty_.selectionEnd()).blockNumber() + 1);
    }
    imageDirty_ = QTextCursor();

    // РАЗМЕТКУ ДОВОДИМ ДО КОНЦА ПЕРЕД ОБХОДОМ. Резерв считается от высоты,
    // которую Qt блоку ОТВЁЛ, а размечает он лениво — и на свежем документе
    // отдал бы устаревшие числа либо нули.
    //
    // Ошибка от этого выходила плавающая, «через раз»: при запуске заметка
    // открывается ещё ДО show(), в узком окне, и колонку двигает уже первое
    // изменение размера. Успела разметка обновиться до нашего обхода — всё
    // сходилось; не успела — фотографии наезжали друг на друга. Переключение
    // на другую заметку чинило картину, потому что там обход шёл уже в готовом
    // окне.
    //
    // documentSize() именно доводит разметку, а не спрашивает готовое: у
    // QPlainTextDocumentLayout это единственный дешёвый способ.
    document()->documentLayout()->documentSize();

    // Набор незащищаемых от вытеснения собирается заново — но только при
    // полном обходе: при частичном мы видим не все картинки заметки, и
    // очистив набор, отдали бы остальные на вытеснение.
    if (!partial) currentNoteImages_.clear();
    const qreal gap = imageGap(zoom_);
    const CodePlate plate = codePlate(zoom_);
    QTextBlock block = document()->findBlockByNumber(first);
    for (int number = first; number <= afterLast && block.isValid();
         ++number, block = block.next()) {
        // РЕЗЕРВ СЧИТАЕТСЯ БЕЗ РАСКЛАДКИ. Высота фотографии выводится из
        // размеров файла и ширины колонки — ни то, ни другое к QTextLayout
        // отношения не имеет. А imageGeometry без раскладки возвращает
        // пустоту, потому что ей нужно ещё и ПОЛОЖЕНИЕ строки, нужное для
        // отрисовки, но не для резерва.
        //
        // Я связал эти две вещи, и вышло вот что: при распахивании окна на
        // весь экран колонка выросла с 67 до 268, но раскладку успели получить
        // не все блоки — резерв поправился у 91 картинки из 118, а остальные
        // остались с полем от узкого окна и наехали друг на друга. Следующий
        // полный обход считал, что всё уже верно, и не трогал ничего;
        // переключение заметки чинило, потому что там документ строился заново
        // в уже готовом окне.
        qreal want = 0.0;
        const BlockImageRef ref = blockImageRef(block);
        if (ref.valid) {
            if (const CachedImage* entry = imageInfo(ref.path)) {
                qreal widthHint = ref.widthHint;
                if (block.blockNumber() == imageDragBlock_ && imageDragWidth_ > 0.0)
                    widthHint = imageDragWidth_;
                const QSizeF size = entry->framed()
                                        ? frameBoxSize(block, *entry)
                                        : imageDisplaySize(entry->declared, widthHint, block);
                if (!size.isEmpty()) {
                    // Отведённая высота у неразмеченного блока — ноль, и это
                    // не беда: как только Qt его разметит, поле останется
                    // верным, а лишняя строка добавится к зазору, а не съест
                    // фотографию.
                    const qreal allotted =
                        document()->documentLayout()->blockBoundingRect(block).height();
                    // Вылет уголков закладывается сверху и снизу — всегда, а
                    // не только у выбранной: резерв не должен зависеть от того,
                    // куда сейчас поставили каретку.
                    want = qMax(0.0, size.height() + 2 * imageCornerOverhang() + gap -
                                         allotted);
                }
            }
        }
        QTextBlockFormat format = block.blockFormat();
        // СОБСТВЕННОЕ нижнее поле блока прибавляется к резерву, а не стирается
        // им. Прежде здесь стоял ноль и рядом уговор «нижнее поле всех прочих
        // блоков — ноль по построению сборщика»; уговор кончился вместе с
        // плашкой кода, у последней строки которой поле своё. Обход стирал его
        // на первой же правке — блок кода терял нижнее поле, и плашка
        // обрезалась по последней строке. Поймал набор, а не глаз.
        want += ownBottomMargin(block, plate);
        // Сравнение с допуском: каждое выставление формата переразмечает
        // документ.
        if (std::fabs(format.bottomMargin() - want) < 0.5) continue;
        format.setBottomMargin(want);
        // ВЫСОТА БЛОКА ЦЕЛИКОМ НАША, а не «строка плюс поле». Иначе она
        // складывается из двух слагаемых, одно из которых считает Qt, — и
        // стоит ему дать неразмеченному блоку ноль вместо высоты строки, как
        // сумма разъезжается: фотографии наезжают друг на друга ровно на эту
        // высоту. Проявлялось при ПОВТОРНОМ открытии длинной заметки: Qt
        // размечает лениво, и у хвоста разметки ещё нет.
        changingLayout_ = true;
        QTextCursor cursor(block);
        cursor.setBlockFormat(format);
        changingLayout_ = false;
    }

    // Обход закончен: известны все картинки этой заметки и их размеры. Теперь
    // решается, каким достанутся пиксели; само разжатие — лениво, по первому
    // рисованию.
    planNoteImages();

    // Нижнее поле ПОСЛЕДНЕГО блока Qt в высоту документа не берёт вовсе —
    // замер: поле 500 на последнем блоке даёт +0, а такое же поле рамки даёт
    // +500. Фотография в последней строке из-за этого не пролезала под нижнюю
    // кромку: прокрутка кончалась раньше, чем она. Недостачу добавляем полем
    // рамки — единственным, которое Qt считает.
    const QTextBlock last = document()->lastBlock();
    const qreal missing = last.isValid() ? last.blockFormat().bottomMargin() : 0.0;
    const qreal want =
        appearance().verticalMargin * QFontMetricsF(baseFontFor(zoom_)).height() + missing;
    QTextFrameFormat frame = document()->rootFrame()->frameFormat();
    // С допуском: каждое выставление формата рамки переразмечает документ.
    if (std::fabs(frame.bottomMargin() - want) >= 0.5) {
        frame.setBottomMargin(want);
        changingLayout_ = true;
        document()->rootFrame()->setFrameFormat(frame);
        changingLayout_ = false;
    }
    syncingImages_ = false;
}

QString NoteView::frameText(const QTextBlock& block, const CachedImage& entry) const {
    const QString name = QFileInfo(blockImageRef(block).path).fileName();
    if (entry.state == ImageState::Missing)
        return QStringLiteral("%1:\nфайл не найден").arg(name);
    return QStringLiteral("%1:\na big %2x%3 image")
        .arg(name)
        .arg(entry.declared.width())
        .arg(entry.declared.height());
}

// Небольшой прямоугольник по размеру самой надписи, с полем вокруг. Ни
// пропорций картинки, ни её размеров он не наследует: 1x1000000 растянуло бы
// рамку на миллион пикселей, а показывать в ней всё равно нечего.
QSizeF NoteView::frameBoxSize(const QTextBlock& block, const CachedImage& entry) const {
    const QFontMetricsF metrics(baseFont());
    const qreal padding = metrics.height();
    QSizeF box = metrics.boundingRect(QRectF(0, 0, 1e6, 1e6), Qt::AlignLeft | Qt::TextWordWrap,
                                      frameText(block, entry))
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
                     frameText(block, entry));
}

void NoteView::paintImage(QPainter& painter, const QTextBlock& block) {
    const ImageGeometry geometry = imageGeometry(block);
    if (!geometry.valid) return;
    const BlockImageRef ref = blockImageRef(block);
    const CachedImage* entry = imageInfo(ref.path);
    if (entry == nullptr) return;

    painter.save();
    // Строка хитро-отрисованная: текст закрашивается фоном, фотография встаёт
    // на его место. Каретка рисуется позже и поверх — ей можно.
    painter.fillRect(geometry.line.adjusted(-2, 0, 2, 0), appearance().pageBackground);
    // Пиксели берутся здесь и только здесь: рисуем — значит нужны.
    const QImage* pixels = entry->framed() ? nullptr : pixelsFor(absoluteImagePath(ref.path));
    if (pixels == nullptr) {
        // Заново: разжатие могло сменить состояние записи, а ссылки в QHash
        // этого не переживают.
        const CachedImage* fresh = imageInfo(ref.path);
        if (fresh != nullptr) paintTooBigImage(painter, block, geometry, *fresh);
    } else {
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        if (exportRatio_ > 0.0) {
            // НА БУМАГУ КАРТИНКА ЕДЕТ ТОГО РАЗМЕРА, КАКИМ ЕЁ ВИДНО. Qt вложила
            // бы в PDF исходные пиксели целиком, сколько бы их ни было: она
            // просто масштабирует при отрисовке, а в файл кладёт то, что дали.
            // Фотография, уменьшенная мышью до трети, весила бы в файле как
            // полная — а именно этого владелец и просил не делать.
            //
            // Только ВНИЗ: растянуть мелкую картинку до разрешения печати
            // нельзя, пикселей взять неоткуда, и файл вырос бы ни за что.
            int want = qMax(1, qRound(geometry.photo.width() * exportRatio_));
            if (exportImageBudget_ > 0) {
                // Потолок считает ТОТ ЖЕ targetSize, что и ввоз: бюджет
                // площади S², потолок стороны 3S, никогда вверх. Второй
                // реализации того же правила быть не должно — разойдутся.
                ImportLimits budget;
                budget.maxSize = exportImageBudget_;
                const int height = qMax(1, qRound(want * qreal(pixels->height()) /
                                                  qreal(qMax(1, pixels->width()))));
                want = qMin(want, targetSize({want, height}, budget).width);
            }
            painter.drawImage(geometry.photo,
                              want < pixels->width()
                                  ? pixels->scaledToWidth(want, Qt::SmoothTransformation)
                                  : *pixels);
        } else {
            painter.drawImage(geometry.photo, *pixels);
        }
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
    if (selected && exportRatio_ <= 0.0) paintImageCorners(painter, geometry.photo);
    painter.restore();
}

void NoteView::renderSlice(QPainter& painter, const QRectF& documentRect, qreal pixelRatio,
                           int imageBudget) {
    exportRatio_ = qMax(0.0, pixelRatio);
    exportImageBudget_ = qMax(0, imageBudget);
    painter.save();
    painter.setClipRect(documentRect);
    // Фон рисуем сами: у бумаги его нет, а подложка кода и цвет текста заданы
    // относительно него. Белая страница с нашими цветами текста читалась бы
    // иначе, чем то, что человек видит в окне.
    painter.fillRect(documentRect, appearance().pageBackground);
    paintCodeBackground(painter, documentRect);

    // Текст — тем же слоем, что и на экране, только без каретки и выделения:
    // PaintContext отдаём пустой, cursorPosition = -1 по умолчанию.
    QAbstractTextDocumentLayout::PaintContext context;
    context.palette = palette();
    context.clip = documentRect;
    document()->documentLayout()->draw(&painter, context);

    // Дальше — слово в слово то же, что в paintEvent: маркеры, черты,
    // фотографии. Разошлись бы эти два обхода — бумага перестала бы совпадать
    // с экраном, а заметить это можно было бы только глазами.
    const QFont base = baseFontFor(zoom_);
    const QAbstractTextDocumentLayout* layout = document()->documentLayout();
    const int firstVisible = layout->hitTest(QPointF(0, documentRect.top()), Qt::FuzzyHit);
    QTextBlock start = document()->findBlock(firstVisible);
    if (start.isValid() && start.previous().isValid()) start = start.previous();
    for (QTextBlock block = start; block.isValid(); block = block.next()) {
        const QRectF rect = layout->blockBoundingRect(block);
        if (rect.top() > documentRect.bottom()) break;
        if (rect.bottom() + block.blockFormat().bottomMargin() < documentRect.top()) continue;
        paintMarker(painter, block, base);
        paintDivider(painter, block, rect, zoom_);
        paintImage(painter, block);
    }

    painter.restore();
    exportRatio_ = 0.0;
    exportImageBudget_ = 0;
}

// Четыре уголка по краям фотографии — как мишень в видоискателе. Заливка
// поверх снимка красила его собственные цвета, а именно за цветами на него
// чаще всего и смотрят; уголки стоят СНАРУЖИ пикселей и не трогают ни один.
qreal NoteView::imageCornerOverhang() {
    const Appearance& a = appearance();
    return qMax(0.0, a.imageCornerOffset) + qMax(0.5, a.imageCornerWidth);
}

void NoteView::paintImageCorners(QPainter& painter, const QRectF& photo) {
    if (photo.isEmpty()) return;
    const Appearance& a = appearance();
    const qreal shortSide = qMin(photo.width(), photo.height());
    // Доля от ПОКАЗАННОГО размера, а не от размера файла: уголки — это про то,
    // что человек видит на экране. Пол — чтобы на маленькой картинке уголок не
    // выродился в точку, потолок — сама короткая сторона: длиннее ему негде.
    const qreal length =
        qMin(shortSide, qMax(shortSide * qMax(0.0, a.imageCornerShare),
                             qreal(a.imageCornerMinLength)));
    const qreal thick = qMax(0.5, a.imageCornerWidth);
    if (length <= 0.0) return;

    // Каждый уголок — ОДИН многоугольник, а не две линии. Двумя линиями в
    // самом углу выходил заметный артефакт: два прямоугольника накладывались
    // под прямым углом, и стык был виден ступенькой.
    const qreal out = qMax(0.0, a.imageCornerOffset);
    const QRectF box = photo.adjusted(-out - thick, -out - thick, out + thick, out + thick);

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    painter.setBrush(a.caretColor);
    for (int corner = 0; corner < 4; ++corner) {
        const bool right = corner == 1 || corner == 2;
        const bool bottom = corner >= 2;
        const QPointF at(right ? box.right() : box.left(), bottom ? box.bottom() : box.top());
        const qreal dx = right ? -1.0 : 1.0;
        const qreal dy = bottom ? -1.0 : 1.0;
        const QPointF points[6] = {
            at,
            at + QPointF(dx * length, 0),
            at + QPointF(dx * length, dy * thick),
            at + QPointF(dx * thick, dy * thick),
            at + QPointF(dx * thick, dy * length),
            at + QPointF(0, dy * length),
        };
        painter.drawPolygon(points, 6);
    }
    painter.restore();
}

QVector<CodeBand> NoteView::codeBands(const QRectF& visible) const {
    const QAbstractTextDocumentLayout* layout = document()->documentLayout();
    const int firstVisible = layout->hitTest(QPointF(0, visible.top()), Qt::FuzzyHit);
    const CodePlate plate = codePlate(zoom_);

    QVector<CodeBand> bands;
    // Начинаем с блока ВЫШЕ первого видимого: плашка вылезает за прямоугольник
    // своего блока — вверх на полоску, вниз на поле, — и блок, чей текст ещё не
    // виден, вполне может показывать сюда свою полоску.
    QTextBlock start = document()->findBlock(firstVisible);
    if (start.isValid() && start.previous().isValid()) start = start.previous();

    for (QTextBlock block = start; block.isValid(); block = block.next()) {
        const QRectF rect = layout->blockBoundingRect(block);
        // ГРАНИЦЫ С ЗАПАСОМ НА ПЛАШКУ. Qt при прокрутке перерисовывает только
        // открывшуюся полосу, и она запросто попадает целиком в ПОЛЕ блока —
        // туда, где нарисована полоска, а не текст. По голому прямоугольнику
        // блока такой кусок оказывался «невидимым», и полоска не рисовалась
        // вовсе: владелец увидел, что при прокрутке она то есть, то нет. Та же
        // беда была у рамок вокруг картинок и лечится тем же — запасом на то,
        // что блок рисует за своими краями.
        if (rect.top() - plate.strip > visible.bottom()) break;
        if (rect.bottom() + plate.padBottom < visible.top()) continue;
        if (isRawBlock(block) || kindOf(block) != Kind::Code) continue;

        // Высоту считаем по числу строк и назначенной высоте строки, а не по
        // прямоугольнику блока: прямоугольник отдаёт естественную высоту, а
        // шаг идёт по назначенной, и разница между ними — та самая полоса.
        // Строк в блоке может быть больше одной: длинная строка кода переносится.
        const qreal assigned = block.blockFormat().lineHeight();
        const int lines = block.layout() != nullptr ? block.layout()->lineCount() : 1;
        const qreal height =
            assigned > 0 ? qMax(rect.height(), lines * assigned) : rect.height();

        // Левый край плашки — поле блока минус внутреннее поле кода. Внутри
        // пункта списка поле блока уже включает колонку пункта, и плашка едет
        // вместе с ним: иначе она вылезала бы левее маркера и разрезала список
        // надвое (проверено на снимке этапа 11).
        const qreal left = rect.left() + block.blockFormat().leftMargin() - plate.padLeft;
        CodeBand band;
        band.rect = QRectF(left, rect.top(), rect.right() - left, height);
        band.blockNumber = block.blockNumber();
        band.first = !isContinuationBlock(block);
        band.last = !codeContinues(block);
        band.info = block.blockFormat().stringProperty(InfoProperty);
        bands.push_back(band);
    }
    return bands;
}

QRectF NoteView::copyButtonRect(const CodeBand& band) const {
    const CodePlate plate = codePlate(zoom_);
    if (!band.first || plate.strip <= 0.0) return {};
    const qreal side = qMin(plate.strip * 0.62, 18.0 * zoom_);
    const qreal gap = plate.padLeft + plate.stripPadding;
    return QRectF(band.rect.right() - gap - side,
                  band.rect.top() - plate.strip + (plate.strip - side) / 2.0, side, side);
}

void NoteView::setEditedCodeLanguage(int firstBlockNumber) {
    if (editedCodeLanguage_ == firstBlockNumber) return;
    editedCodeLanguage_ = firstBlockNumber;
    viewport()->update();
}

QRectF NoteView::languageRect(const CodeBand& band) const {
    const CodePlate plate = codePlate(zoom_);
    if (!band.first || plate.strip <= 0.0) return {};
    const qreal left = band.rect.left() + plate.padLeft + plate.stripPadding;
    const QRectF button = copyButtonRect(band);
    const qreal right = button.isEmpty() ? band.rect.right() : button.left() - plate.stripPadding;
    if (right <= left) return {};
    return QRectF(left, band.rect.top() - plate.strip, right - left, plate.strip);
}

void NoteView::paintCodeBackground(QPainter& painter, const QRectF& visible) {
    const CodePlate plate = codePlate(zoom_);
    const QVector<CodeBand> bands = codeBands(visible);

    // СОСТОЯНИЕ ВОЗВРАЩАЕМ. Цвет подложки кода полупрозрачен (альфа 14 из 255),
    // и оставленная в painter'е кисть на экране безвредна — растровый painter
    // при drawImage на неё не смотрит. А вот PDF смотрит: Qt складывает альфу
    // кисти в состояние картинки, и фотография уехала на бумагу с прозрачностью
    // 5% — бледной тенью. Найдено глазами по вывезенной странице, в самом файле
    // это выглядело как "/ca 0.054901960" перед вставкой снимка.
    painter.save();
    painter.setPen(Qt::NoPen);
    painter.setRenderHint(QPainter::Antialiasing, true);

    for (const CodeBand& band : bands) {
        // Полоска сверху и поле снизу — это ПОЛЯ БЛОКА, зарезервированные
        // сборщиком документа теми же величинами (codePlate в settings.h).
        // Прямоугольник блока полей не включает — замерено пробником, а не
        // взято из документации.
        const qreal top = band.rect.top() - (band.first ? plate.strip : 0.0);
        const qreal bottom = band.rect.bottom() + (band.last ? plate.padBottom : 0.0);
        const QRectF whole(band.rect.left(), top, band.rect.width(), bottom - top);
        const QPainterPath path = platePath(whole, plate.radius, band.first, band.last);
        const qreal stripHeight = band.first ? plate.strip : 0.0;

        // ДВЕ ЗАЛИВКИ, НЕ НАКЛАДЫВАЮЩИЕСЯ. Полоска и подложка полупрозрачны
        // (по умолчанию обе — чернота с прозрачностью 14), и нарисованные одна
        // поверх другой они дают удвоенную плотность: владелец увидел это как
        // «цвет полоски всё ещё чуть-чуть отличается». Красим каждую область
        // ровно один раз, а скруглённые углы держит клип по контуру.
        painter.save();
        painter.setClipPath(path, Qt::IntersectClip);
        if (stripHeight > 0.0)
            painter.fillRect(QRectF(whole.left(), whole.top(), whole.width(), stripHeight),
                             appearance().codeStripBackground);
        painter.fillRect(QRectF(whole.left(), whole.top() + stripHeight, whole.width(),
                                whole.height() - stripHeight),
                         appearance().codeBackground);
        painter.restore();
        if (!band.first) continue;

        // Черта НЕ во всю ширину: слева начинается от отступа буквы, справа не
        // доходит полбуквы до края. Иначе она читается как рамка, а нужна
        // граница между надписью и кодом.
        // На бумаге полоски нет вовсе (решение владельца): ни черты, ни имени
        // языка, ни кнопки. Скруглённые углы и поля остаются — плашка на
        // странице выглядит как на экране, только без органов управления.
        if (exportRatio_ > 0.0) continue;
        if (plate.ruleWidth > 0.0 && plate.strip > 0.0) {
            const qreal left = whole.left() + plate.padLeft;
            const qreal right = whole.right() - plate.ruleInset;
            if (right > left)
                painter.fillRect(QRectF(left, band.rect.top() - plate.ruleWidth,
                                        right - left, plate.ruleWidth),
                                 appearance().codeStripRule);
        }
        paintCodeStrip(painter, band);
    }
    painter.restore();
}

// Содержимое полоски: имя языка слева, кнопка копирования справа.
//
// Кнопка появляется при наведении на блок — держать её на виду всегда значило
// бы, что на каждом блоке кода висит серый значок, которого человек не просил.
// На бумаге её нет вовсе: нажимать там нечего.
void NoteView::paintCodeStrip(QPainter& painter, const CodeBand& band) {
    const CodePlate plate = codePlate(zoom_);
    if (plate.strip <= 0.0) return;
    const QRectF strip(band.rect.left(), band.rect.top() - plate.strip, band.rect.width(),
                       plate.strip);

    painter.save();
    if (!band.info.isEmpty() && band.blockNumber != editedCodeLanguage_) {
        painter.setFont(codeLangFont(zoom_));
        painter.setPen(appearance().codeLangColor);
        painter.drawText(strip.adjusted(plate.padLeft + plate.stripPadding, 0, 0, 0),
                         Qt::AlignVCenter | Qt::AlignLeft, band.info);
    }

    // Кнопка видна ВСЕГДА, а не по наведению (решение владельца): слежение за
    // мышью ради значка — это перерисовка вьюпорта на каждое движение, а
    // выигрыш только в том, что серого значка не видно, пока он не нужен.
    // На бумаге кнопки нет: нажимать там нечего.
    if (exportRatio_ <= 0.0) {
        const QRectF box = copyButtonRect(band);
        const bool done = band.blockNumber == copiedCodeBlock_;
        const QPixmap icon = toolbarIcon(
            done ? QStringLiteral("check") : QStringLiteral("copy"),
            int(std::round(box.width())), appearance().codeLangColor, devicePixelRatioF());
        painter.drawPixmap(box.topLeft(), icon);
    }
    painter.restore();
}

QPointF NoteView::toDocument(const QPoint& viewportPoint) const {
    return QPointF(viewportPoint.x() + horizontalScrollBar()->value(),
                   viewportPoint.y() + verticalScrollBar()->value());
}

void NoteView::mousePressEvent(QMouseEvent* event) {
    // Кнопка копирования перехватывает нажатие целиком: она нарисована в поле
    // блока, но для Qt это обычное место документа, и без перехвата щелчок
    // ставил бы туда каретку.
    if (event->button() == Qt::LeftButton) {
        const QPointF at = toDocument(event->position().toPoint());
        const QRectF visible(0, verticalScrollBar()->value(), viewport()->width(),
                             viewport()->height());
        for (const CodeBand& band : codeBands(visible)) {
            if (!band.first) continue;
            const QRectF box = copyButtonRect(band);
            if (!box.isEmpty() && box.contains(at)) {
                copyCodeBlock(band.blockNumber);
                event->accept();
                return;
            }
            // Щелчок по месту языка — включая пустое: у блока без языка его
            // как раз и надо задать, а целиться человеку некуда.
            const QRectF where = languageRect(band);
            if (where.isEmpty() || !where.contains(at)) continue;
            const QRect inViewport =
                where.translated(-horizontalScrollBar()->value(), -verticalScrollBar()->value())
                    .toRect();
            emit codeStripClicked(band.blockNumber, inViewport);
            event->accept();
            return;
        }
    }
    QTextBrowser::mousePressEvent(event);
}

QString NoteView::codeTextFrom(int firstBlockNumber) const {
    QTextBlock block = document()->findBlockByNumber(firstBlockNumber);
    if (!block.isValid() || isRawBlock(block) || kindOf(block) != Kind::Code) return {};
    QStringList lines;
    for (;;) {
        lines << block.text();
        if (!codeContinues(block)) break;
        block = block.next();
    }
    return lines.join(QLatin1Char('\n'));
}

void NoteView::copyCodeBlock(int firstBlockNumber) {
    const QString code = codeTextFrom(firstBlockNumber);
    if (code.isEmpty()) return;
    QGuiApplication::clipboard()->setText(code);
    copiedCodeBlock_ = firstBlockNumber;
    copiedFade_.start();
    viewport()->update();
}

// Подложка кода ПОВЕРХ выделения.
//
// Порядок отрисовки таков: сначала наша серая подложка, потом Qt рисует текст
// и заливает выделение своим непрозрачным цветом — и подложка под выделением
// пропадает целиком. Ctrl+E на выделенном тексте не менял на экране ровным
// счётом ничего, и владелец на это наткнулся.
//
// Чиним не выделением, а подложкой: цвет кода у нас и так полупрозрачный
// (codeBackground с малой альфой), и повторить его сверху — значит подкрасить
// синеву выделения в серо-голубой, ничего больше не трогая. Альфа у самого
// выделения не нужна вовсе: Qt её в QPalette::Highlight не соблюдает.
void NoteView::paintCodeOverSelection(QPainter& painter, const QRectF& visible) {
    const QTextCursor caret = textCursor();
    if (!caret.hasSelection()) return;
    const int from = caret.selectionStart();
    const int to = caret.selectionEnd();

    const QAbstractTextDocumentLayout* layout = document()->documentLayout();
    painter.setPen(Qt::NoPen);
    painter.setBrush(appearance().codeBackground);
    for (QTextBlock block = document()->findBlock(from); block.isValid();
         block = block.next()) {
        if (block.position() > to) break;
        const QRectF rect = layout->blockBoundingRect(block);
        if (rect.top() > visible.bottom()) break;
        if (rect.bottom() < visible.top()) continue;
        const QTextLayout* text = block.layout();
        if (text == nullptr) continue;

        // Кода в блоке бывает два рода, и оба надо подкрасить: ЦЕЛЫЙ блок в
        // тройных кавычках и СТРОЧНЫЙ код внутри обычного абзаца (Ctrl+E на
        // выделении делает именно его — на этом я и попался, починив сперва
        // только блоки).
        const bool whole = !isRawBlock(block) && kindOf(block) == Kind::Code;

        // Отрезки блока, которые надо подкрасить, в координатах блока.
        std::vector<std::pair<int, int>> runs;
        const int start = qMax(0, from - block.position());
        const int end = qMin(block.length() - 1, to - block.position());
        if (start >= end) continue;
        if (whole) {
            runs.emplace_back(start, end);
        } else {
            // Строчный код: берём куски, помеченные фоном кода, и пересекаем с
            // выделением. Спрашиваем именно фон, а не «моноширинный шрифт»:
            // подкрашиваем ровно то, что и было подкрашено до выделения.
            for (QTextBlock::iterator it = block.begin(); !it.atEnd(); ++it) {
                const QTextFragment fragment = it.fragment();
                if (!fragment.isValid()) continue;
                if (fragment.charFormat().background().style() == Qt::NoBrush) continue;
                const int at = fragment.position() - block.position();
                const int lo = qMax(start, at);
                const int hi = qMin(end, at + fragment.length());
                if (lo < hi) runs.emplace_back(lo, hi);
            }
        }
        if (runs.empty()) continue;

        for (int i = 0; i < text->lineCount(); ++i) {
            const QTextLine line = text->lineAt(i);
            for (const auto& run : runs) {
                const int lineFrom = qMax(run.first, line.textStart());
                const int lineTo = qMin(run.second, line.textStart() + line.textLength());
                if (lineFrom >= lineTo) continue;
                const qreal x0 = line.cursorToX(lineFrom);
                const qreal x1 = line.cursorToX(lineTo);
                painter.drawRect(QRectF(rect.left() + qMin(x0, x1), rect.top() + line.y(),
                                        std::fabs(x1 - x0), line.height()));
            }
        }
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

    {
        // Повторяем подложку кода поверх выделения: Qt только что закрасило её
        // своим непрозрачным цветом.
        QPainter painter(viewport());
        painter.translate(-horizontalScrollBar()->value(), -verticalScrollBar()->value());
        const QRectF visible(horizontalScrollBar()->value(), verticalScrollBar()->value(),
                             viewport()->width(), viewport()->height());
        paintCodeOverSelection(painter, visible);
    }

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
