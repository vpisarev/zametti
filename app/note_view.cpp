#include "note_view.h"

#include "block_object.h"
#include "doc_model.h"
#include "formula.h"
#include "icons.h"
#include "import_limits.h"
#include "marker.h"
#include "settings.h"
#include "table.h"

#include <QApplication>
#include <QWheelEvent>
#include <QAbstractTextDocumentLayout>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include "image_read.h"

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
#include <QDateTime>
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

// Сколько места занимает формула на экране: вёрстка или рамка ошибки. Одним
// местом — иначе резерв и отрисовка разойдутся, и рамка налезет на текст под
// собой (я это и получил: рамка в две строки на блоке высотой в одну).
qreal formulaBoxHeight(const FormulaRender& render, qreal naturalLine) {
    if (!render.error.isEmpty() || render.image.isNull()) return 2.4 * naturalLine;
    return render.height;
}


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

// Первая строка блока кода, в котором лежит эта. Нужна там, где мы пришли к
// блоку с конца: полоска висит у последней строки, а зовётся блок по первой.
QTextBlock startOfCodeBlock(const QTextBlock& block) {
    QTextBlock at = block;
    while (isContinuationBlock(at)) {
        const QTextBlock before = at.previous();
        if (!before.isValid() || isRawBlock(before) || kindOf(before) != Kind::Code) break;
        at = before;
    }
    return at;
}

// Собственное нижнее поле блока — то, которое стоит в нём НЕ ради картинки.
// Пока такое одно: полоска с языком и кнопкой у последней строки блока кода.
qreal ownBottomMargin(const QTextBlock& block, const CodePlate& plate) {
    if (isRawBlock(block) || kindOf(block) != Kind::Code) return 0.0;
    return codeContinues(block) ? 0.0 : plate.strip;
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

bool caretShouldBeDrawn(bool focused, bool readOnly, bool hasSelection, bool onDrawnObject) {
    if (!focused || readOnly) return false;
    if (hasSelection) return false;
    if (onDrawnObject) return false;
    return true;
}

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
    // СТРАНИЦА ЗАНИМАЕТ ВСЁ ОКНО, а не только вьюпорт. Лишнюю ширину широкого
    // окна мы теперь отдаём полям вьюпорта (см. applyContentWidth), и полоски
    // по краям рисует уже не документ, а сам виджет — своим фоном. Без этой
    // строки они вылезали серыми, и белая колонка выглядела листом, положенным
    // на стол.
    palette.setColor(QPalette::Window, palette.color(QPalette::Base));
    view.setPalette(palette);
    // Заливать фон виджет обязан САМ: по умолчанию у полосы прокрутки красит
    // только вьюпорт, а рамка вокруг него остаётся стилю — и там проступает
    // серый цвет окна.
    view.setBackgroundRole(QPalette::Base);
    view.setAutoFillBackground(true);
}

QColor NoteView::pageColour() const { return palette().color(QPalette::Base); }

QFont NoteView::baseFont() const {
    // У документа, а не из zoom_: см. довод в заголовке. Второй меры масштаба
    // не существует.
    return document() != nullptr ? document()->defaultFont() : baseFontFor(1.0);
}

qreal NoteView::displayScale() const {
    const qreal base = appearance().baseFontPoint;
    if (base <= 0.0) return 1.0;
    const qreal shown = baseFont().pointSizeF();
    return shown > 0.0 ? shown / base : 1.0;
}

void NoteView::setZoom(qreal zoom) {
    zoom_ = zoom;
    if (document() == nullptr) return;
    // ВОТ ЗДЕСЬ МАСШТАБ И ПРИМЕНЯЕТСЯ — единственным местом на всю программу.
    // Раньше его не применял никто: сборщик ставил документу базовый кегль без
    // масштаба, а сюда число только записывалось.
    //
    // Кегль строится ОТ ОБЛИКА (baseFontFor), а не от нынешнего шрифта
    // документа: baseFont() отдаёт как раз его, и сравнение вышло бы с самим
    // собой — масштаб не менялся бы никогда.
    const QFont want = baseFontFor(zoom_);
    if (document()->defaultFont() == want) return;
    // ПОД ФЛАГОМ ОБЛИКА. Смена шрифта документа переразмечает его целиком, и Qt
    // шлёт contentsChanged — документу она неотличима от набора. Без этой
    // пометки Ctrl+= заводил бы шаг истории (поймано набором Editor: после
    // зума в цепочке отмены становилось на шаг больше).
    const LayoutChange mark(this);
    document()->setDefaultFont(want);
}

NoteView::NoteView(QWidget* parent) : QTextBrowser(parent) {
    // Свой документ у вида уже есть — его завела Qt; объекты в нём тоже надо
    // уметь показывать (в него собирает вывоз на бумагу).
    attachObjectHandlers(document());
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

    // ШТАТНОГО QScroller ЗДЕСЬ НЕТ, И ЭТО РЕШЕНИЕ, А НЕ ЗАБЫВЧИВОСТЬ.
    //
    // Он ловит НАСТОЯЩИЕ КАСАНИЯ. Тачпад ими не приходит — он присылает те же
    // колёсные события, только точными пикселями, — так что пользы от QScroller
    // на ноутбуке ноль. Зато он включает виджету приём касаний и меняет разбор
    // ввода: владелец увидел, что прокрутка вниз уводит за собой каретку, та
    // уезжает за окно, и прокрутка перестаёт работать. Синтетическими событиями
    // это не воспроизводится — значит трогать ввод без нужды нельзя тем более.
    //
    // Инерцию тачпаду делает wheelEvent. Тач-экран вернём отдельно и с прибором.

    // БРОСОК ПОСЛЕ ТОГО, КАК ПАЛЬЦЫ ОТНЯЛИ. Ждём тишины: фазы прокрутки под X11
    // не приходят вовсе, и «пальцы убрали» узнаётся только по тому, что новых
    // событий больше нет.
    glideStart_.setSingleShot(true);
    connect(&glideStart_, &QTimer::timeout, this, [this] {
        // Скорость меньше пикселя за кадр — это не бросок, а остановка.
        if (systemGlides_ || std::fabs(glideSpeed_) * 16.0 < 1.0) {
            glideSpeed_ = 0.0;
            return;
        }
        scrollGlide_.start();
    });

    // ЗАТУХАНИЕ. Кадр за кадром скорость тает, и текст останавливается сам —
    // оттого движение и ощущается броском, а не рывком. За smoothScrollMs
    // скорость падает вчетверо с лишним.
    scrollGlide_.setInterval(16);   // примерно кадр экрана
    connect(&scrollGlide_, &QTimer::timeout, this, [this] {
        QScrollBar* bar = verticalScrollBar();
        if (bar == nullptr) {
            stopGlide();
            return;
        }
        const int move = int(glideSpeed_ * scrollGlide_.interval());
        if (move == 0) {
            stopGlide();
            return;
        }
        const int now = bar->value();
        bar->setValue(now + move);
        // Упёрлись в край — ехать больше некуда.
        if (bar->value() == now) {
            stopGlide();
            return;
        }
        const qreal tau = qMax(1, appearance().smoothScrollMs);
        glideSpeed_ *= std::exp(-scrollGlide_.interval() / tau);
    });

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
    at.setWidth(qMax(1, qRound(appearance().caretWidth * displayScale())));
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
    painter.fillRect(clip, pageColour());
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
    // ЦЕНТРИРОВАНИЕ КОЛОНКИ — ПОЛЯМИ ВЬЮПОРТА, А НЕ ДОКУМЕНТА.
    //
    // Боковое поле ставит СБОРЩИК, один раз (rootFrame, sideMargin × ширина
    // «A»), — это настройка облика, и переделывать её незачем. Виду остаётся
    // только лишняя ширина широкого окна, и класть её в документ нельзя: всякая
    // запись формата попадает в штатный стек отмены (замерено: и setFrameFormat,
    // и setDocumentMargin стоят одного нажатия Ctrl+Z), а перекладка окна шагом
    // отмены быть не имеет права.
    //
    // Поля вьюпорта документа не касаются вовсе: он просто получает меньше
    // места. Формула «сколько досталось колонке» от этого не меняется — в ней
    // и так стоит ширина вьюпорта.
    const qreal charUnit = QFontMetricsF(baseFont()).horizontalAdvance(QLatin1Char('A'));
    // Поле, которое колонке ПОЛОЖЕНО сейчас, — от нынешнего шрифта: оно обязано
    // расти вместе с масштабом, иначе на 200 % текст прижимается к краю окна.
    const qreal want = appearance().sideMargin * charUnit;
    // И то, которое уже даёт документ: его поставил сборщик, один раз, базовым
    // кеглем. Переписывать его нельзя — запись формата попадает в стек отмены.
    const qreal fromDocument = document()->rootFrame()->frameFormat().leftMargin();

    // Полная ширина, из которой раздаётся место: нынешний вьюпорт плюс то, что
    // мы у него уже отняли. Считать по width() виджета нельзя — там ещё рамка и
    // полоса прокрутки, и вышла бы обратная связь.
    const int room = viewport()->width() + viewportMargin_ * 2;
    qreal margin = qMax(0.0, want - fromDocument);
    if (appearance().maxContentWidth > 0.0) {
        const qreal limit = appearance().maxContentWidth * charUnit;
        const qreal spare = (room - 2 * want - limit) / 2;
        if (spare > 0.0) margin += spare;
    }

    const int wanted = int(margin);
    if (wanted != viewportMargin_) {
        viewportMargin_ = wanted;
        setViewportMargins(wanted, 0, wanted, 0);
        // ШИРИНУ ВЁРСТКИ ДОСЫЛАЕМ САМИ. Поля вьюпорта сузили окно, а документ
        // остаётся свёрстан по прежней ширине: QTextEdit пересчитывает её на
        // своём resizeEvent, а тот приходит позже нас. При запуске это и
        // выглядело как горизонтальная полоса прокрутки, пропадавшая от первого
        // же изменения размера окна (жалоба владельца).
        //
        // setTextWidth в стек отмены не попадает — замерено (zametti-bench zoom).
        document()->setTextWidth(viewport()->width());
        // Пустой документ от смены полей не переразмечается: размечать в нём
        // нечего. Каретка тогда остаётся у прежнего поля — в широком окне это
        // выглядело как «в пустой заметке каретки нет вовсе».
        document()->markContentsDirty(0, qMax(1, document()->characterCount()));
    }

    // Ширина колонки сменилась — фотографии могли стать шире или уже колонки,
    // и место под них надо перемерить.
    syncImageSpace();
}

void NoteView::wheelEvent(QWheelEvent* event) {
    QScrollBar* bar = verticalScrollBar();
    // ИНЕРЦИЯ НУЖНА ТАЧПАДУ, а не колесу. Колесо приходит рывками по «щелчку»
    // (angleDelta) и своей плавностью владельца устраивает; тачпад присылает
    // ТОЧНЫЕ ПИКСЕЛИ (pixelDelta) — текст едет за пальцами и встаёт колом,
    // стоит их отнять.
    //
    // Тач-экран сюда не приходит вовсе: он идёт касаниями, и ему заведён
    // QScroller (см. конструктор).
    // ФАЗА ВАЖНЕЕ СДВИГА. Событие «пальцы убрали» приходит с НУЛЕВЫМ сдвигом, и
    // судить по одному pixelDelta нельзя: приняв его за колесо, мы гасили бы
    // бросок ровно там, где он должен начинаться (поймал набор).
    const bool sequence = event->phase() != Qt::NoScrollPhase;
    const bool touchpad = sequence || !event->pixelDelta().isNull();
    if (!appearance().smoothScroll || !touchpad || bar == nullptr) {
        stopGlide();
        QTextBrowser::wheelEvent(event);
        return;
    }

    // СИСТЕМА, УМЕЮЩАЯ САМА, — не трогаем. macOS и Wayland присылают фазу
    // «инерция» и досылают события уже после того, как пальцы убрали; своя
    // поверх неё удвоила бы разгон.
    if (event->phase() == Qt::ScrollMomentum) {
        systemGlides_ = true;
        stopGlide();
        QTextBrowser::wheelEvent(event);
        return;
    }
    if (event->phase() == Qt::ScrollBegin) systemGlides_ = false;

    // КОНЕЦ ЖЕСТА — ЭТО МАРКЕР, А НЕ ДВИЖЕНИЕ. Он приходит с нулевым сдвигом, и
    // мерить по нему скорость нельзя: она обнулилась бы ровно в тот миг, ради
    // которого копилась.
    if (event->phase() == Qt::ScrollEnd) {
        glideStart_.start(0);
        event->accept();
        return;
    }

    // Само движение — как всегда, за пальцами. Мы только запоминаем СКОРОСТЬ:
    // на сколько пикселей ушёл текст и за сколько времени.
    const int before = bar->value();
    QTextBrowser::wheelEvent(event);
    const int moved = bar->value() - before;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 passed = glideStamp_ > 0 ? now - glideStamp_ : 0;
    glideStamp_ = now;
    if (passed > 0 && passed < 100) {
        // Скорость сглаживаем: одно случайное дрожание пальца не должно
        // определять весь бросок. Замерший палец шлёт нули — и скорость тает
        // сама, так что «остановился и убрал» броском не станет.
        const qreal fresh = qreal(moved) / qreal(passed);   // пикселей на мс
        glideSpeed_ = glideSpeed_ * 0.4 + fresh * 0.6;
    }

    // На X11 фаз нет вовсе, и «пальцы убрали» узнаётся только тишиной.
    glideStart_.start(50);
    event->accept();
}

void NoteView::stopGlide() {
    scrollGlide_.stop();
    glideStart_.stop();
    glideSpeed_ = 0.0;
    glideStamp_ = 0;
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
    // xmpWithFileName в exif.h). У вики-вложения alt нет: исходником там стоит
    // сама запись "![[путь]]", и показывать её вместо подписи незачем.
    //
    // Берём её у САМОЙ СПРАВКИ, а не у текста блока: текста у объекта нет —
    // в блоке стоит один знак U+FFFC. Пока брали текст, в панель уезжал он.
    if (!ref.wiki) facts.caption = ref.alt.trimmed();
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
    const QSize declared = probeImageFile(abs).size;
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
    DecodeRequest request;
    if (limit > 0 && (declared.width() > limit || declared.height() > limit)) {
        // Предел держит ОБЕ стороны. Только вниз: картинка мельче предела
        // остаётся собой. Просим об этом сам читатель — иные форматы умеют
        // разжимать сразу в нужный размер и полной копии не заводят вовсе.
        QSize scaled = declared.scaled(limit, limit, Qt::KeepAspectRatio);
        // У вырожденной картинки (1x1000000) короткая сторона уходит в ноль, а
        // картинки нулевой ширины не бывает.
        scaled.setWidth(qMax(1, scaled.width()));
        scaled.setHeight(qMax(1, scaled.height()));
        request.maxSize = scaled;
    }
    // Глубина показу не нужна: экран восьмибитный, а шестнадцать бит стоят
    // четверти времени и вдвое больше памяти в кэше. Ввоз просит их отдельно.
    QImage image = decodeImageFile(key, request);
    // Читатель отдал не меньше просимого (у JPEG размер идёт восьмыми долями) —
    // доводим до предела сами.
    if (!request.maxSize.isEmpty() && image.size() != request.maxSize &&
        (image.width() > request.maxSize.width() || image.height() > request.maxSize.height())) {
        image = image.scaled(request.maxSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }
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

// --- ОБЪЕКТ-ФОТОГРАФИЯ ------------------------------------------------------

ImageObjectHandler::ImageObjectHandler(NoteView* view) : QObject(view), view_(view) {}

QSizeF ImageObjectHandler::intrinsicSize(QTextDocument* doc, int posInDocument,
                                         const QTextFormat& format) {
    Q_UNUSED(format);
    if (view_ == nullptr || doc == nullptr) return {};
    const NoteView::ImageBox box = view_->imageBoxFor(doc->findBlock(posInDocument));
    return box.valid ? box.band : QSizeF();
}

void ImageObjectHandler::drawObject(QPainter* painter, const QRectF& rect, QTextDocument* doc,
                                    int posInDocument, const QTextFormat& format) {
    Q_UNUSED(format);
    if (view_ == nullptr || doc == nullptr || painter == nullptr) return;
    const QTextBlock block = doc->findBlock(posInDocument);
    NoteView::ImageBox box = view_->imageBoxFor(block);
    if (!box.valid) return;
    // Полоса приезжает от Qt уже поставленной на место — переносим в неё своё
    // внутреннее устройство.
    box.caption.translate(rect.topLeft());

    // ЗДЕСЬ РИСУЕТСЯ ТОЛЬКО ПОДПИСЬ, а сам снимок — позже, поверх готовой
    // страницы. Причина замерена: выделение Qt кладёт НА ОБЪЕКТ своим проходом,
    // уже после drawObject, и выбранная фотография выцветала — красный квадрат
    // 220,30,30 превращался в 134,85,114. А за цветами на снимок чаще всего и
    // смотрят (правило владельца: помечаем уголками, а не заливкой).
    //
    // Подпись при этом остаётся здесь: она текст, и выделению её красить можно
    // и нужно — ровно как всякий другой текст.
    view_->paintImageCaption(*painter, box);
}

// --- ОБЪЕКТ-ФОРМУЛА ---------------------------------------------------------

FormulaObjectHandler::FormulaObjectHandler(NoteView* view) : QObject(view), view_(view) {}

QSizeF FormulaObjectHandler::intrinsicSize(QTextDocument* doc, int posInDocument,
                                           const QTextFormat& format) {
    Q_UNUSED(format);
    if (view_ == nullptr || doc == nullptr) return {};
    return view_->formulaBandFor(doc->findBlock(posInDocument));
}

void FormulaObjectHandler::drawObject(QPainter* painter, const QRectF& rect, QTextDocument* doc,
                                      int posInDocument, const QTextFormat& format) {
    Q_UNUSED(painter);
    Q_UNUSED(rect);
    Q_UNUSED(doc);
    Q_UNUSED(posInDocument);
    Q_UNUSED(format);
    // ЗДЕСЬ НЕ РИСУЕТСЯ НИЧЕГО, и это не забывчивость: объект только ДЕРЖИТ
    // МЕСТО, а сама вёрстка ложится поверх готовой страницы (paintFormulaMarks).
    // Замер тот же, что у фотографии: выделение Qt кладёт на объект своим
    // проходом, уже после drawObject, и выбранная формула тонула в заливке.
}

// ПОЛОСА ФОРМУЛЫ — ВО ВСЮ ШИРИНУ КОЛОНКИ, как у фотографии: вёрстку внутри неё
// мы ставим сами, по центру. Высота — высота вёрстки плюс воздух сверху и
// снизу, чтобы формула не липла к соседним строкам.
QSizeF NoteView::formulaBandFor(const QTextBlock& block) {
    const BlockFormulaRef ref = blockFormulaRef(block);
    if (!ref.valid || !ref.display) return {};
    const qreal band = columnWidth(block);
    const qreal natural = QFontMetricsF(baseFont()).height();
    const FormulaRender* render = formulaAt(block.blockNumber());
    // Вёрстки ещё нет (движок не позвали, заметку только открыли) — держим
    // место по естественной высоте строки: пустоты вместо формулы быть не
    // должно, а как только вёрстка появится, полоса перемерится.
    const qreal box = render != nullptr ? formulaBoxHeight(*render, natural) : natural;
    return QSizeF(band, box + imageGap(displayScale()));
}

qreal NoteView::columnWidth(const QTextBlock& block) const {
    const QTextFrameFormat root = document()->rootFrame()->frameFormat();
    qreal width = document()->textWidth() - root.leftMargin() - root.rightMargin() -
                  block.blockFormat().leftMargin();
    // Документ, которому ширину ещё не задали, отдаёт −1: пока её нет, меряем
    // окном. Это случается ровно один раз — до первой раскладки.
    if (width < 16.0) width = viewport()->width() - block.blockFormat().leftMargin();
    return qMax(16.0, width);
}

QFont NoteView::captionFont() const {
    QFont font(appearance().imageCaptionFamily);
    font.setPointSizeF(qMax(1.0, appearance().imageCaptionPoints * displayScale()));
    return font;
}

NoteView::ImageBox NoteView::imageBoxFor(const QTextBlock& block) {
    ImageBox box;
    const BlockImageRef ref = blockImageRef(block);
    if (!ref.valid) return box;
    const CachedImage* entry = imageInfo(ref.path);
    if (entry == nullptr) return box;

    // ПОЛОСА — ВО ВСЮ ШИРИНУ КОЛОНКИ. Уголки выбранной фотографии рисуются
    // снаружи её края, а всё, что вылезло за прямоугольник объекта, Qt
    // отсекает; поэтому сам снимок живёт внутри полосы с отступом на вылет.
    const qreal over = imageCornerOverhang();
    const qreal band = columnWidth(block);
    const qreal room = qMax(1.0, band - 2 * over);

    qreal widthHint = ref.widthHint;
    if (block.blockNumber() == imageDragBlock_ && imageDragWidth_ > 0.0)
        widthHint = imageDragWidth_;
    QSizeF photo = entry->framed() ? frameBoxSize(block, *entry)
                                   : imageDisplaySize(entry->declared, widthHint, block);
    if (photo.isEmpty()) return box;
    if (photo.width() > room) {
        photo.setHeight(qMax(1.0, photo.height() * room / photo.width()));
        photo.setWidth(room);
    }

    // ВЫРАВНИВАНИЕ — НАШЕ ДЕЛО, а не Qt: полоса всегда во всю ширину, и снимок
    // мы ставим в ней сами. Не убрались — снимок занял всю полосу, и вопрос
    // выравнивания отпал сам.
    qreal shift = 0.0;
    if (room > photo.width()) {
        switch (ref.align) {
            case ImageAlign::Center: shift = (room - photo.width()) / 2.0; break;
            case ImageAlign::Right: shift = room - photo.width(); break;
            case ImageAlign::Left: break;
        }
    }
    box.photo = QRectF(over + shift, over, photo.width(), photo.height());

    // ПОДПИСЬ. У рамки «файл не найден» её нет: рамка сама и есть надпись.
    // Переносится по словам в пределах снимка и может занять несколько строк —
    // её высота входит в высоту полосы.
    qreal captionHeight = 0.0;
    if (appearance().imageCaption && !entry->framed() && !ref.alt.isEmpty()) {
        box.text = ref.alt;
        box.flags = Qt::TextWordWrap |
                    (ref.align == ImageAlign::Right ? Qt::AlignRight : Qt::AlignLeft);
        const QFontMetricsF metrics(captionFont());
        const qreal width = box.photo.width();
        const qreal height =
            metrics.boundingRect(QRectF(0, 0, width, 1e6), box.flags, box.text).height();
        const qreal gap = appearance().imageCaptionGap * displayScale();
        box.caption = QRectF(box.photo.left(), box.photo.bottom() + gap, width, height);
        captionHeight = gap + height;
    }

    box.band = QSizeF(band, over + photo.height() + captionHeight + over +
                                imageGap(displayScale()));
    box.valid = true;
    return box;
}

QSizeF NoteView::imageDisplaySize(QSize natural_, qreal widthHint,
                                  const QTextBlock& block) const {
    if (natural_.width() <= 0 || natural_.height() <= 0) return {};
    // Своя ширина картинки — в физических пикселях; на экране она занимает
    // столько логических, чтобы пиксели легли один в один (HiDPI). Явная
    // ширина ("|315") — уже логическая, как её видит Obsidian.
    const qreal natural = natural_.width() / devicePixelRatioF();
    qreal width = (widthHint > 0.0 ? widthHint : natural) * displayScale();

    // Шире колонки фотографии не бывать.
    const qreal available = columnWidth(block);
    if (width > available) width = available;
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
    const qreal assigned = assignedLineHeight(block);
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
    const QRectF box = imageObjectRect(block);
    if (box.isEmpty()) return {};
    return box.translated(-horizontalScrollBar()->value(), -verticalScrollBar()->value());
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
    if (document() == nullptr || document()->documentLayout() == nullptr) return;

    // РАЗМЕР ОБЪЕКТА ЗАВИСИТ ОТ ТОГО, ЧЕГО В ДОКУМЕНТЕ НЕТ: ширины колонки,
    // масштаба, кэша пикселей. Изменилось это — вёрстке надо сказать
    // перемерить; САМ ДОКУМЕНТ ПРИ ЭТОМ НЕ МЕНЯЕТСЯ, и в том вся выгода:
    // markContentsDirty не пишет ни свойства, ни текста, а значит и стек отмены
    // не трогает. Ради этого объекты и заводились.
    //
    // Помечаем ТОЛЬКО ТРОНУТОЕ, когда границы правки нам сказали: пометка на
    // весь документ означала бы полную переразметку на каждое нажатие клавиши.
    //
    // А ЕСЛИ ГРАНИЦ НЕТ И ПОЛНОГО ПЕРЕСЧЁТА НЕ ПРОСИЛИ — НЕ ПОМЕЧАЕМ НИЧЕГО.
    // textChanged приходит и без правки: Qt испускает contentsChanged на каждом
    // закрытии скобки правки, в том числе на пустой скобке, которой
    // NoteEditor::onContentsChanged приклеивает уборку к набранному знаку.
    // Прежде такой вызов считался «без границ — значит полный» и помечал
    // грязным ВЕСЬ документ: на «Братьях Карамазовых» (1.9 МБ) каждое нажатие
    // стоило 328 мс полной перевёрстки (стенд zametti-bench big, стеки под
    // gdb: markContentsDirty → doLayout на всю заметку). Настоящая правка
    // всегда приходит с границами (contentsChange → markImageRegion), а вёрстка
    // о ней знает и сама; помечать нам нужно только то, чего вёрстка не видит, —
    // ширину колонки, масштаб, кэш пикселей, — и это whole.
    {
        const bool partial = !whole && !imageDirty_.isNull();
        const int last = qMax(0, document()->characterCount() - 1);
        const int from = partial ? qBound(0, imageDirty_.selectionStart(), last) : 0;
        const int to = partial ? qBound(from, imageDirty_.selectionEnd(), last) : last;
        // ПОЛНАЯ ПОМЕТКА — ТОЛЬКО ЕСЛИ ИЗМЕНИЛОСЬ ТО, ОТ ЧЕГО ЗАВИСИТ РАЗМЕР
        // ОБЪЕКТА: сам документ, ширина колонки, шрифт (масштаб), плотность.
        // Полный пересчёт просят часто и на всякий случай — на каждую
        // перекладку окна, на подмену документа, на возврат к заметке, — а
        // пометить весь документ значит заново сверстать его целиком при
        // первом же вопросе о геометрии (замер: 155 мс на «Карамазовых» на
        // каждый возврат к заметке из кэша, при том что ни ширина, ни шрифт не
        // менялись).
        bool mark = partial;
        if (whole) {
            const WholeSyncKey key{document(), viewport()->width(), baseFont(), devicePixelRatioF()};
            if (!(key == lastWholeSync_)) {
                lastWholeSync_ = key;
                mark = true;
            }
        }
        if (mark) document()->markContentsDirty(from, qMax(1, to - from));
        if (whole) {
            // Набор незащищаемых от вытеснения собирается заново — но только
            // при полном обходе: при частичном мы видим не все картинки
            // заметки, и очистив набор, отдали бы остальные на вытеснение.
            //
            // Картинки заметки перечисляем САМИ, обходом блоков, а не ждём,
            // когда о них спросит вёрстка: она размечает лениво и о хвосте
            // документа может не спросить вовсе, а раздавать пиксели надо по
            // всей заметке. Обход стоит числа блоков и идёт только на полном
            // пересчёте — при смене заметки, ширины окна или масштаба.
            currentNoteImages_.clear();
            for (QTextBlock b = document()->begin(); b.isValid(); b = b.next())
                if (const BlockImageRef ref = blockImageRef(b); ref.valid) imageInfo(ref.path);
        }
    }

    // Кому из картинок заметки достанутся пиксели — решаем на КАЖДОМ пересчёте,
    // а не только на полном: заметка растёт правкой, и вставленная картинка
    // обязана попасть в раздачу сразу, а не после следующего открытия.
    planNoteImages();
    imageDirty_ = QTextCursor();

    // Вёрстка формул считается ВСЕГДА: формула — объект, её размер называет она
    // сама, и места в документе ей не резервируют. Считать надо до раскладки:
    // именно у вёрстки объект спросит свою высоту.
    syncFormulas();

    // ВРЕМЕННЫЙ ШАГ НАЗАД (kObjectsShown в doc_model.h): резерв места под
    // ТАБЛИЦУ вид всё ещё держит полями блоков, и пока держит, показывать её
    // нельзя. У фотографии и у формулы этого резерва больше нет: их размер
    // знают они сами.
    if (!kObjectsShown) return;
    if (syncingImages_) return;
    syncingImages_ = true;
    // Пометка на весь вызов: всё, что здесь пишется, — резерв места под объекты,
    // то есть облик. Узкие пометки вокруг отдельных записей оставляли щели,
    // в которые проваливался сигнал от предыдущей записи.
    const LayoutChange mark(this);

    // Таблицы прячут блоки и меняют высоты — резерв считается уже по новой
    // раскладке.
    syncTables();

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

    const qreal gap = imageGap(displayScale());
    const CodePlate plate = codePlate();
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
        // Место под вёрстку формулы здесь больше не считается: формула —
        // объект, её размер называет она сама (formulaBandFor). Прежде тут
        // стояли две ручки — ужатие высоты строки и добор нижним полем, — и обе
        // были записями вида в живой документ.
        const qreal wantLine = -1.0;
        // Резерв под сетку таблицы: он висит на ПОСЛЕДНЕЙ её строке — перед
        // спрятанными блоками Qt поле игнорирует (пробник). Высота берётся
        // из раскладки, а из неё вычитается то, что блок занимает сам.
        for (const TableRender& table : std::as_const(tables_)) {
            if (block.blockNumber() != table.last) continue;
            const qreal allotted =
                document()->documentLayout()->blockBoundingRect(block).height();
            want += qMax(0.0, table.layout.height - allotted);
            break;
        }
        // Сравнение с допуском: каждое выставление формата переразмечает
        // документ.
        const bool marginSame = std::fabs(format.bottomMargin() - want) < 0.5;
        const bool lineSame = wantLine < 0.0 || std::fabs(format.lineHeight() - wantLine) < 0.5;
        if (marginSame && lineSame) continue;
        format.setBottomMargin(want);
        if (wantLine >= 0.0) format.setLineHeight(wantLine, QTextBlockFormat::FixedHeight);
        // ВЫСОТА БЛОКА ЦЕЛИКОМ НАША, а не «строка плюс поле». Иначе она
        // складывается из двух слагаемых, одно из которых считает Qt, — и
        // стоит ему дать неразмеченному блоку ноль вместо высоты строки, как
        // сумма разъезжается: фотографии наезжают друг на друга ровно на эту
        // высоту. Проявлялось при ПОВТОРНОМ открытии длинной заметки: Qt
        // размечает лениво, и у хвоста разметки ещё нет.
        QTextCursor cursor(block);
        cursor.setBlockFormat(format);
    }

    // Нижнее поле ПОСЛЕДНЕГО блока Qt в высоту документа не берёт вовсе —
    // замер: поле 500 на последнем блоке даёт +0, а такое же поле рамки даёт
    // +500. Фотография в последней строке из-за этого не пролезала под нижнюю
    // кромку: прокрутка кончалась раньше, чем она. Недостачу добавляем полем
    // рамки — единственным, которое Qt считает.
    const QTextBlock last = document()->lastBlock();
    const qreal missing = last.isValid() ? last.blockFormat().bottomMargin() : 0.0;
    const qreal want =
        appearance().verticalMargin * QFontMetricsF(baseFont()).height() + missing;
    QTextFrameFormat frame = document()->rootFrame()->frameFormat();
    // С допуском: каждое выставление формата рамки переразмечает документ.
    if (std::fabs(frame.bottomMargin() - want) >= 0.5) {
        frame.setBottomMargin(want);
        document()->rootFrame()->setFrameFormat(frame);
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
    const qreal available = columnWidth(block);
    if (box.width() > available) box.setWidth(available);
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
    pen.setWidthF(qMax(1.0, 1.5 * displayScale()));
    painter.setPen(pen);
    painter.drawRect(geometry.photo.adjusted(0.5, 0.5, -0.5, -0.5));

    painter.setFont(baseFont());
    painter.setPen(appearance().rawColor);
    painter.drawText(geometry.photo, Qt::AlignCenter | Qt::TextWordWrap,
                     frameText(block, entry));
}

void NoteView::paintImageCaption(QPainter& painter, const ImageBox& geometry) {
    if (!geometry.valid || geometry.caption.isEmpty() || geometry.text.isEmpty()) return;
    // ПОДПИСЬ ПОД СНИМКОМ: своим шрифтом и приглушённым цветом — это справка о
    // снимке, а не содержание заметки. Место под неё уже отведено, оно вошло в
    // высоту полосы; добирать полями блока ничего не нужно.
    painter.save();
    painter.setFont(captionFont());
    painter.setPen(appearance().imageCaptionColor);
    painter.drawText(geometry.caption, geometry.flags, geometry.text);
    painter.restore();
}

void NoteView::paintImageObject(QPainter& painter, const ImageBox& geometry,
                                const QTextBlock& block) {
    const BlockImageRef ref = blockImageRef(block);
    if (!ref.valid || !geometry.valid) return;
    const CachedImage* entry = imageInfo(ref.path);
    if (entry == nullptr) return;

    const QRectF box = geometry.photo;
    painter.save();
    // Закрашивать строку под фотографией больше не нужно: текста под ней нет
    // вовсе — объект и есть тот единственный знак, который стоит в строке.
    const QImage* pixels = entry->framed() ? nullptr : pixelsFor(absoluteImagePath(ref.path));
    if (pixels == nullptr) {
        // Заново: разжатие могло сменить состояние записи, а ссылки в QHash
        // этого не переживают.
        const CachedImage* fresh = imageInfo(ref.path);
        if (fresh != nullptr) {
            painter.fillRect(box, pageColour());
            QPen pen(appearance().rawColor);
            pen.setStyle(Qt::DashLine);
            pen.setWidthF(qMax(1.0, 1.5 * displayScale()));
            painter.setPen(pen);
            painter.drawRect(box.adjusted(0.5, 0.5, -0.5, -0.5));
            painter.setFont(baseFont());
            painter.setPen(appearance().rawColor);
            painter.drawText(box, Qt::AlignCenter | Qt::TextWordWrap, frameText(block, *fresh));
        }
    } else {
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        if (exportRatio_ > 0.0) {
            // НА БУМАГУ КАРТИНКА ЕДЕТ ТОГО РАЗМЕРА, КАКИМ ЕЁ ВИДНО (довод — у
            // прежнего paintImage, там же и замер).
            int want = qMax(1, qRound(box.width() * exportRatio_));
            if (exportImageBudget_ > 0) {
                ImportLimits budget;
                budget.maxSize = exportImageBudget_;
                const int height = qMax(1, qRound(want * qreal(pixels->height()) /
                                                  qreal(qMax(1, pixels->width()))));
                want = qMin(want, targetSize({want, height}, budget).width);
            }
            painter.drawImage(box, want < pixels->width()
                                       ? pixels->scaledToWidth(want, Qt::SmoothTransformation)
                                       : *pixels);
        } else {
            painter.drawImage(box, *pixels);
        }
    }
    painter.restore();
}

// Уголки выбранной фотографии рисуются НЕ в обработчике объекта, а поверх
// готовой страницы. Причина замерена: выделение Qt кладёт на объект СВОИМ
// проходом, уже после drawObject, и уголки под ним пропадали.
void NoteView::paintImageMarks(QPainter& painter, const QTextBlock& block) {
    const ImageBox box = imageBoxFor(block);
    if (!box.valid) return;
    const QTextLayout* layout = block.layout();
    if (layout == nullptr || layout->lineCount() == 0) return;
    const QTextLine line = layout->lineAt(0);
    ImageBox placed = box;
    const QPointF at = layout->position() + QPointF(line.x(), line.y());
    placed.photo.translate(at);
    placed.caption.translate(at);

    // САМ СНИМОК — ЗДЕСЬ, поверх выделения (см. довод в drawObject).
    paintImageObject(painter, placed, block);
    if (exportRatio_ > 0.0) return;

    // Уголки — выбранной фотографии. Выделение, задевшее объект, и каретка,
    // вставшая на его строку, — это одно и то же: «вот эта фотография».
    const QTextCursor caret = textCursor();
    const bool selected =
        caret.hasSelection()
            ? qMin(caret.anchor(), caret.position()) < block.position() + block.length() &&
                  qMax(caret.anchor(), caret.position()) > block.position()
            : caret.block() == block;
    if (!selected) return;
    painter.save();
    // УГОЛКИ ОХВАТЫВАЮТ И ПОДПИСЬ: снимок и подпись — один объект, и выбраны
    // они вместе. Нижняя пара уголков уходит под подпись, верхняя остаётся у
    // верхнего края снимка (решение владельца).
    QRectF marks = placed.photo;
    if (!placed.caption.isEmpty()) marks |= placed.caption;
    paintImageCorners(painter, marks);
    painter.restore();
}

// Место фотографии в координатах ДОКУМЕНТА. Спрашиваем у вёрстки: объект стоит
// в строке одним знаком, и где он оказался, знает Qt.
QRectF NoteView::imageObjectRect(const QTextBlock& block) {
    const ImageBox box = imageBoxFor(block);
    if (!box.valid) return {};
    const QTextLayout* layout = block.layout();
    if (layout == nullptr || layout->lineCount() == 0) return {};
    // Полоса объекта стоит на своей строке; где именно — знает Qt. Внутреннее
    // устройство полосы считает imageBoxFor, и второй копии у него нет.
    const QTextLine line = layout->lineAt(0);
    return box.photo.translated(layout->position() + QPointF(line.x(), line.y()));
}

// ОБРАБОТЧИКИ ОБЪЕКТОВ ЖИВУТ У ВЁРСТКИ, а вёрстка — у документа. Значит
// регистрировать их надо у КАЖДОГО документа, который вид показывает: и у
// подменённого, и у того, который Qt завела виду сама.
//
// Второе оказалось важнее, чем кажется: вывоз на бумагу собирает документ прямо
// в свой вид, не подменяя его, — и без этого объекты на странице РИСОВАЛИСЬ, но
// МЕСТА НЕ ЗАНИМАЛИ, наезжая на текст под собой (владелец: «ни картинки ни
// формулы не экспортируются нормально в PDF»).
void NoteView::attachObjectHandlers(QTextDocument* doc) {
    if (doc == nullptr || doc->documentLayout() == nullptr) return;
    if (imageObjects_ == nullptr) imageObjects_ = new ImageObjectHandler(this);
    doc->documentLayout()->registerHandler(ImageObject, imageObjects_);
    if (formulaObjects_ == nullptr) formulaObjects_ = new FormulaObjectHandler(this);
    doc->documentLayout()->registerHandler(FormulaObject, formulaObjects_);
}

void NoteView::setDocument(QTextDocument* doc) {
    QTextBrowser::setDocument(doc);
    attachObjectHandlers(doc);
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
    painter.fillRect(geometry.line.adjusted(-2, 0, 2, 0), pageColour());
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

    paintTables(painter, documentRect);

    // Дальше — слово в слово то же, что в paintEvent: маркеры, черты,
    // фотографии. Разошлись бы эти два обхода — бумага перестала бы совпадать
    // с экраном, а заметить это можно было бы только глазами.
    const QFont base = baseFont();
    const QAbstractTextDocumentLayout* layout = document()->documentLayout();
    const int firstVisible = layout->hitTest(QPointF(0, documentRect.top()), Qt::FuzzyHit);
    QTextBlock start = document()->findBlock(firstVisible);
    if (start.isValid() && start.previous().isValid()) start = start.previous();
    for (QTextBlock block = start; block.isValid(); block = block.next()) {
        const QRectF rect = layout->blockBoundingRect(block);
        if (rect.top() > documentRect.bottom()) break;
        if (rect.bottom() + block.blockFormat().bottomMargin() < documentRect.top()) continue;
        paintMarker(painter, block, base);
        paintDivider(painter, block, rect, displayScale());
        // Фотографию рисует обработчик объектов (ImageObjectHandler): Qt зовёт
        // его сама, отведя объекту место в строке. Здесь остаются только уголки
        // выбранной — они ложатся поверх выделения — и те объекты, которые ещё
        // не переведены.
        paintImageMarks(painter, block);
        paintFormulaMarks(painter, block);
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
    const CodePlate plate = codePlate();

    QVector<CodeBand> bands;
    int started = -1;   // начало блока кода, в котором мы сейчас идём
    // Начинаем с блока ВЫШЕ первого видимого: плашка вылезает за прямоугольник
    // своего блока — вверх на воздух, вниз на полоску, — и блок, чей текст уже
    // уехал вверх, вполне может показывать сюда свою полоску.
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
        if (rect.top() - plate.padTop > visible.bottom()) break;
        if (rect.bottom() + plate.strip < visible.top()) continue;
        if (isRawBlock(block) || kindOf(block) != Kind::Code) continue;

        // Высоту считаем по числу строк и назначенной высоте строки, а не по
        // прямоугольнику блока: прямоугольник отдаёт естественную высоту, а
        // шаг идёт по назначенной, и разница между ними — та самая полоса.
        // Строк в блоке может быть больше одной: длинная строка кода переносится.
        const qreal assigned = assignedLineHeight(block);
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
        // Начало блока запоминаем на ходу: обход идёт сверху вниз, и первая его
        // строка уже прошла — кроме случая, когда обход начался ПОСРЕДИ блока
        // (длинный блок кода поперёк всего окна). Тогда, и только тогда, идём
        // назад — один раз на блок, а не на каждую его строку.
        if (band.first) started = band.blockNumber;
        if (started < 0) started = startOfCodeBlock(block).blockNumber();
        band.firstBlockNumber = started;
        if (band.last) started = -1;
        bands.push_back(band);
    }
    return bands;
}

// Кнопка копирования — в правом нижнем углу плашки, то есть у ПОСЛЕДНЕЙ полосы
// блока: полоска висит в её нижнем поле.
QRectF NoteView::copyButtonRect(const CodeBand& band) const {
    const CodePlate plate = codePlate();
    if (!band.last || plate.strip <= 0.0) return {};
    const qreal side = qMin(plate.strip * 0.62, 18.0 * displayScale());
    const qreal gap = plate.padLeft + plate.stripPadding;
    return QRectF(band.rect.right() - gap - side,
                  band.rect.bottom() + (plate.strip - side) / 2.0, side, side);
}

void NoteView::setEditedCodeLanguage(int firstBlockNumber) {
    if (editedCodeLanguage_ == firstBlockNumber) return;
    editedCodeLanguage_ = firstBlockNumber;
    viewport()->update();
}

// --- выключные формулы -------------------------------------------------------
//
// Третье воплощение слоя объекта, и устроено оно как показ картинки: строка
// исходника закрашивается фоном, вёрстка встаёт на её место, место резервирует
// нижнее поле блока. Прятать блок, как у таблицы, здесь нельзя и не нужно:
// многострочная выключная формула живёт в ОДНОМ блоке — переносы внутри абзаца
// его не рвут.
//
// Движок зовётся здесь, а не в отрисовке: рендер стоит миллисекунды, а кадров
// в секунду шестьдесят.
void NoteView::syncFormulas() {
    QHash<int, FormulaRender> fresh;
    if (Formulas::ready()) {
        const QFont base = baseFont();
        // Кегль движку нужен В ПИКСЕЛЯХ, и спрашивать его надо у Qt: она знает,
        // во сколько пикселей превратился кегль в пунктах на этом экране.
        // Пункты сюда передавать нельзя — формула выйдет на треть мельче текста
        // (обжёгся на этом в пробнике).
        const qreal pixelSize = QFontInfo(base).pixelSize() * appearance().formulas.displayScale;
        // Цвет — ПЕРОМ ИЗ ПАЛИТРЫ, а не инверсией картинки: в тёмной теме
        // формула обязана быть набрана светлым, а не вывернутой наизнанку.
        const QColor colour = palette().color(QPalette::Text);
        const qreal dpr = devicePixelRatioF();

        for (QTextBlock block = document()->begin(); block.isValid(); block = block.next()) {
            // ВЁРСТКА СЧИТАЕТСЯ ТОЛЬКО ОБЪЕКТУ. Раскрытая на правку формула —
            // это обычный абзац с исходником (Kind::Paragraph), и рисовать
            // поверх него нечего: человек правит то, что видит. Признака «его
            // сейчас правят» больше нет — род блока говорит всё сам.
            if (isRawBlock(block) || kindOf(block) != Kind::Math) continue;
            const BlockFormulaRef ref = blockFormulaRef(block);
            if (!ref.valid || !ref.display) continue;

            const int number = block.blockNumber();
            FormulaRender render;
            // КЭШ ВЁРСТКИ ВЫКЛЮЧЕН — по решению владельца, чтобы проверить
            // догадку: не от него ли пропадающие строки и прочие странности,
            // которые набор не ловит. Пока выключен, движок зовётся на каждую
            // сверку; это дорого (миллисекунды на формулу), и включать его
            // обратно надо вместе с проверкой, которая эту догадку закрывает.
            //
            // Пометка на завтра: если догадка не подтвердится, кэш вернуть — но
            // ключом, в котором перечислено ВСЁ, от чего зависит картинка
            // (исходник, кегль, цвет, плотность), и с проверкой «повторная
            // сверка движок не зовёт».
            const FormulaRender* had = nullptr;
            if (had != nullptr && had->source == ref.source && had->colour == colour &&
                qFuzzyCompare(had->pixelSize, pixelSize) && qFuzzyCompare(had->dpr, dpr)) {
                render = *had;   // ничего не изменилось — движок звать незачем
            } else {
                render.source = ref.source;
                render.pixelSize = pixelSize;
                render.colour = colour;
                render.dpr = dpr;
                // Предконтроль ДО движка: он молчалив и семь сломанных формул из
                // десяти дорисовывает огрызком без единой жалобы.
                render.error = checkLatex(ref.latex);
                if (render.error.isEmpty()) {
                    const FormulaImage drawn =
                        Formulas::render(ref.latex, true, pixelSize, colour, dpr);
                    if (drawn.ok()) {
                        render.image = drawn.image;
                        render.width = drawn.width;
                        render.height = drawn.height;
                    } else {
                        render.error = drawn.error;
                    }
                }
            }
            render.block = number;
            fresh.insert(number, render);
        }
    }
    formulas_ = fresh;

    // ИСХОДНИК ГАСИТЬ БОЛЬШЕ НЕЧЕМ И НЕЗАЧЕМ: его в тексте блока нет вовсе —
    // там стоит объект, а исходник живёт в свойстве его формата. Прежде здесь
    // стоял проход, красивший исходник прозрачным и возвращавший ему цвет на
    // время правки; это была запись ВИДА в живой документ, и ровно из-за неё
    // показ формул был выключен.
}

// ОДИН ВОПРОС НА ВСЕ ОБЪЕКТЫ. Показан ли объект вместо своего исходника —
// решает слой объекта плюс признак «его сейчас правят». Разводить это по веткам
// у каждого вида нельзя: ровно так каретка и осталась мигать сперва в таблице,
// а потом, слово в слово, у формулы.
bool NoteView::caretOnDrawnObject() {
    const QTextBlock block = textCursor().block();
    const BlockObject object = objectOf(block);
    switch (object.kind) {
        case ObjectKind::None:
            return false;
        case ObjectKind::Image:
            return imageGeometry(block).valid;
        case ObjectKind::Table:
            return object.first != editedTable_;
        case ObjectKind::Formula:
            // Формула-ОБЪЕКТ показана вёрсткой всегда: раскрытая на правку
            // формула объектом уже не является (objectOf спрашивает род блока),
            // и сюда не попадает вовсе. Признак «её сейчас правят» больше не
            // нужен — и не спрашивается.
            //
            // Каретку здесь гасить обязательно: объект занимает полосу во всю
            // ширину колонки и высотой в саму формулу, и штатная мигающая
            // полоска рисуется во весь этот рост (владелец: «сбоку появляется
            // огромный мигающий курсор»).
            return true;
    }
    return false;
}

const FormulaRender* NoteView::formulaAt(int blockNumber) const {
    const auto it = formulas_.constFind(blockNumber);
    return it == formulas_.constEnd() ? nullptr : &it.value();
}


// Где стоит вёрстка: по центру колонки, сразу под верхом строки исходника.
QRectF NoteView::formulaRect(int blockNumber) const {
    const FormulaRender* render = formulaAt(blockNumber);
    if (render == nullptr || render->image.isNull()) return {};
    const QTextBlock block = document()->findBlockByNumber(blockNumber);
    const QTextLayout* layout = block.isValid() ? block.layout() : nullptr;
    if (layout == nullptr || layout->lineCount() == 0) return {};

    // ГЕОМЕТРИЯ ОДНА С ОТРИСОВКОЙ: полоса во всю ширину колонки, вёрстка по
    // центру внутри неё. Второй копии этого расчёта быть не должно — разойдётся,
    // и уголки окажутся не там, где формула.
    const qreal band = columnWidth(block);
    const qreal shift = band > render->width ? (band - render->width) / 2.0 : 0.0;
    const QTextLine line = layout->lineAt(0);
    return QRectF(layout->position() + QPointF(line.x() + shift, line.y()),
                  QSizeF(render->width, render->height));
}

// ВЁРСТКА И УГОЛКИ ВЫБРАННОЙ ФОРМУЛЫ — ПОВЕРХ ГОТОВОЙ СТРАНИЦЫ. Объект только
// держит место; рисуем здесь, потому что выделение Qt кладёт на объект своим
// проходом, уже после drawObject (замер тот же, что у фотографии).
void NoteView::paintFormulaMarks(QPainter& painter, const QTextBlock& block) {
    // Только ОБЪЕКТ: раскрытая на правку формула — обычный абзац, и рисовать
    // поверх него нечего.
    if (isRawBlock(block) || kindOf(block) != Kind::Math) return;
    const BlockFormulaRef ref = blockFormulaRef(block);
    if (!ref.valid || !ref.display) return;
    const QTextLayout* layout = block.layout();
    if (layout == nullptr || layout->lineCount() == 0) return;

    painter.save();
    const FormulaRender* render = formulaAt(block.blockNumber());
    const qreal natural = QFontMetricsF(baseFont()).height();
    if (render == nullptr || !render->error.isEmpty() || render->image.isNull()) {
        // Битая формула — рамка с исходником, родня рамки «файл не найден»:
        // молчаливый огрызок хуже честной ошибки.
        const QTextLine line = layout->lineAt(0);
        const QRectF frame(layout->position().x() + line.x(), layout->position().y() + line.y(),
                           columnWidth(block),
                           qMax(natural, formulaBoxHeight(render == nullptr ? FormulaRender()
                                                                            : *render,
                                                          natural)));
        painter.fillRect(frame, pageColour());
        QPen pen(appearance().rawColor);
        pen.setStyle(Qt::DashLine);
        pen.setWidthF(qMax(1.0, 1.5 * displayScale()));
        painter.setPen(pen);
        painter.drawRect(frame.adjusted(0.5, 0.5, -0.5, -0.5));
        painter.setFont(baseFont());
        painter.setPen(appearance().rawColor);
        const QString what = render == nullptr ? QString() : render->error;
        painter.drawText(frame.adjusted(6, 4, -6, -4), Qt::AlignLeft | Qt::TextWordWrap,
                         what.isEmpty() ? ref.source : ref.source + QLatin1Char('\n') + what);
        painter.restore();
        return;
    }

    // ПО ЦЕНТРУ ПОЛОСЫ (решение владельца). Прямоугольник — в логических
    // точках, источник — в физических, и никакой плотности у самой картинки
    // (см. formula.cpp): размер вёрстки не зависит ни от плотности экрана, ни
    // от того, как Qt толкует её пометку.
    // Закрашивать под вёрсткой нечего: исходника в тексте блока нет вовсе — там
    // стоит объект. Выделение при этом остаётся видно вокруг формулы, ровно как
    // вокруг фотографии: так и читается «выбрано».
    const QRectF box = formulaRect(block.blockNumber());
    painter.drawImage(box, render->image, QRectF(QPointF(0, 0), QSizeF(render->image.size())));

    // Уголки — выбранной. С небольшим отступом наружу: впритык обнимающие дробь
    // читаются как часть формулы, а не как «выбрано».
    if (exportRatio_ <= 0.0) {
        const QTextCursor caret = textCursor();
        const bool selected =
            caret.hasSelection()
                ? qMin(caret.anchor(), caret.position()) < block.position() + block.length() &&
                      qMax(caret.anchor(), caret.position()) > block.position()
                : caret.block() == block;
        if (selected) {
            const qreal pad = imageCornerOverhang();
            paintImageCorners(painter, box.adjusted(-pad, -pad, pad, pad));
        }
    }
    painter.restore();
}

int NoteView::formulaAtPoint(const QPointF& documentPoint) const {
    for (const FormulaRender& render : std::as_const(formulas_)) {
        const QRectF rect = formulaRect(render.block);
        if (!rect.isEmpty() && rect.contains(documentPoint)) return render.block;
    }
    return -1;
}

// --- таблицы ----------------------------------------------------------------
//
// Показ таблицы устроен ровно как показ картинки: исходные строки прячутся,
// место резервируется полем блока, а сетка рисуется по геометрии поверх. Три
// вещи выяснены пробником, а не документацией:
//
//   * QTextBlock::setVisible(false) действительно убирает блок из раскладки:
//     высота документа падает, блок получает нулевой прямоугольник;
//   * НИЖНЕЕ ПОЛЕ ПЕРЕД СПРЯТАННЫМИ БЛОКАМИ Qt ИГНОРИРУЕТ. Поле в 120 пикселей
//     на блоке, за которым идут невидимые, не сдвинуло следующий видимый ни на
//     пиксель. Поэтому резерв висит на ПОСЛЕДНЕЙ строке таблицы (за ней идёт
//     видимый блок), а прячутся все, кроме неё;
//   * каретка по спрятанным блокам ходит: стрелка вниз с видимой строки
//     приводит её в невидимую. Это придётся ловить отдельно — на слое объекта.
void NoteView::syncTables() {
    // ВРЕМЕННЫЙ ШАГ НАЗАД (kObjectsShown): таблица показана строками исходника,
    // сетки поверх нет — и прятать строки не надо.
    if (!kObjectsShown) return;
    // Место, в которое вписывается таблица, считается ОТ ЕЁ ЛЕВОГО КРАЯ.
    //
    // Таблица стоит там же, где начинается колонка текста, — то есть уже
    // отступив левое поле рамки. Первая редакция брала полную ширину вьюпорта,
    // и в узком окне таблица уезжала за правый край ровно на это поле: текст
    // обрезался на «в какую коло…». Видно только глазами и только в узком
    // окне — то самое, ради чего в матрице обязательны оба размера.
    const QTextFrameFormat frame = document()->rootFrame()->frameFormat();
    const qreal columnWidth = document()->textWidth() > 0
                                  ? document()->textWidth() - frame.leftMargin() -
                                        frame.rightMargin()
                                  : viewport()->width();
    const qreal margin = qMax(0.0, qreal(viewport()->width()) - frame.leftMargin() - 8);
    const qreal fullWidth = qMax(columnWidth, margin);

    QHash<int, TableRender> fresh;
    for (QTextBlock block = document()->begin(); block.isValid(); block = block.next()) {
        const BlockObject object = objectOf(block);
        if (object.kind != ObjectKind::Table || object.first != block.blockNumber()) continue;
        // Правят исходником — показываем как есть: строки на месте, сетки нет.
        if (object.first == editedTable_) continue;

        // Исходник таблицы: строки блоков, как они есть.
        QString source;
        for (int number = object.first; number <= object.last; ++number) {
            source += document()->findBlockByNumber(number).text();
            source += QLatin1Char('\n');
        }

        TableRender render;
        const TableRender* had = tables_.contains(object.first) ? &tables_[object.first] : nullptr;
        if (had != nullptr && had->source == source && qFuzzyCompare(had->width, fullWidth) &&
            qFuzzyCompare(had->zoom, displayScale())) {
            render = *had;   // ничего не изменилось — считать заново незачем
        } else {
            TableSpace space;
            space.columnWidth = columnWidth;
            space.fullWidth = fullWidth;
            space.zoom = displayScale();
            render.layout = layoutTable(parseTable(source.toStdString()), space);
            render.source = source;
            render.width = fullWidth;
            render.zoom = displayScale();
        }
        render.first = object.first;
        render.last = object.last;
        if (render.layout.rows <= 0) continue;
        fresh.insert(object.first, render);
    }
    tables_ = fresh;

    // Прячем всё, кроме последней строки: на ней висит резерв (см. выше).
    //
    // ПРИЗНАК ПЕРЕКЛАДКИ ВОЗВРАЩАЕМ, А НЕ ГАСИМ. Он уже мог быть поднят выше
    // по стеку — пересборкой документа, — и слепое `= false` в конце гасило
    // чужой признак посреди чужой работы: правки, шедшие следом, записывались
    // в историю как настоящие, и одного Ctrl+Z переставало хватать. Поймал
    // набор редактора («смена облика шагов истории не заводит»), и это ровно
    // тот случай, когда беда видна только в чужом наборе.
    const LayoutChange mark(this);
    int dirtyFrom = -1;
    int dirtyTo = -1;
    for (QTextBlock block = document()->begin(); block.isValid(); block = block.next()) {
        bool hide = false;
        for (const TableRender& table : std::as_const(tables_)) {
            if (block.blockNumber() >= table.first && block.blockNumber() < table.last) {
                hide = true;
                break;
            }
        }
        if (block.isVisible() == !hide) continue;
        block.setVisible(!hide);
        if (dirtyFrom < 0) dirtyFrom = block.position();
        dirtyTo = block.position() + block.length();
    }

    // ПОСЛЕ СМЕНЫ ВИДИМОСТИ БЛОКИ НАДО ПОМЕТИТЬ ГРЯЗНЫМИ.
    //
    // setVisible(true) возвращает блок в документ, но раскладку ему Qt сама не
    // пересчитывает: он остаётся с нулевой высотой, то есть невидимым на
    // экране. Владелец увидел ровно это — «щёлкаю по таблице, жму Enter, а
    // показывается только последняя строка»: строки возвращались, высоты у них
    // не было. В моём пробнике markContentsDirty стоял, и потому пробник этой
    // беды не показал — а в syncTables я его не перенёс.
    if (dirtyFrom >= 0) document()->markContentsDirty(dirtyFrom, dirtyTo - dirtyFrom);
}

void NoteView::paintTables(QPainter& painter, const QRectF& visible) {
    // ВРЕМЕННЫЙ ШАГ НАЗАД (kObjectsShown): сетки нет, видны строки исходника.
    if (!kObjectsShown) return;
    if (tables_.isEmpty()) return;
    const QAbstractTextDocumentLayout* layout = document()->documentLayout();
    const Appearance::Tables& look = appearance().tables;

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, false);
    for (const TableRender& table : std::as_const(tables_)) {
        const QTextBlock last = document()->findBlockByNumber(table.last);
        if (!last.isValid()) continue;
        const QRectF anchorRect = layout->blockBoundingRect(last);
        const QRectF area(anchorRect.left(), anchorRect.top(), table.layout.width,
                          table.layout.height);
        if (area.bottom() < visible.top() || area.top() > visible.bottom()) continue;

        // Под сеткой осталась видимой последняя строка исходника — закрываем её
        // фоном страницы, как фотография закрывает текст своей строки. Фон
        // берётся у палитры вида, а не у облика: справка в окне About показана
        // тем же вьюером, но палитру ей не правят, и «страничный» цвет вылезал
        // там молочным пятном под каждой таблицей (заметил владелец).
        painter.fillRect(QRectF(anchorRect.left(), area.top(), qMax(area.width(), anchorRect.width()),
                                area.height()),
                         pageColour());

        // Заливки: тело, зебра, шапка. Прозрачные по умолчанию — тогда просто
        // ничего не рисуется.
        const auto fill = [&painter](const QRectF& rect, const QColor& colour) {
            if (colour.alpha() > 0) painter.fillRect(rect, colour);
        };
        fill(area, look.tableColor);
        qreal y = area.top();
        for (int row = 0; row < table.layout.rows; ++row) {
            const qreal height = table.layout.rowHeight.at(row);
            const QRectF band(area.left(), y, area.width(), height);
            if (row == 0) fill(band, look.headerColor);
            else if ((row % 2) == 0) fill(band, look.altTableColor);
            y += height;
        }

        // Линии. Толщины и цвет — из конфига; ноль означает «не рисовать».
        const auto line = [&painter, &look, this](const QRectF& rect, qreal width) {
            if (width <= 0.0) return;
            painter.fillRect(QRectF(rect.left(), rect.top(), rect.width(),
                                    qMax(1.0, width * displayScale())),
                             look.borderColor);
        };
        const auto column = [&painter, &look, this](qreal x, qreal top, qreal height,
                                                    qreal width) {
            if (width <= 0.0) return;
            painter.fillRect(QRectF(x, top, qMax(1.0, width * displayScale()), height),
                             look.borderColor);
        };

        line(QRectF(area.left(), area.top(), area.width(), 0), look.horizontalBorder);
        line(QRectF(area.left(), area.bottom() - look.horizontalBorder * displayScale(), area.width(), 0),
             look.horizontalBorder);
        y = area.top();
        for (int row = 0; row < table.layout.rows; ++row) {
            y += table.layout.rowHeight.at(row);
            if (row == 0)
                line(QRectF(area.left(), y - look.headerSeparator * displayScale() / 2, area.width(), 0),
                     look.headerSeparator);
            else if (row + 1 < table.layout.rows)
                line(QRectF(area.left(), y, area.width(), 0), look.rowSeparator);
        }
        column(area.left(), area.top(), area.height(), look.verticalBorder);
        column(area.right() - look.verticalBorder * displayScale(), area.top(), area.height(),
               look.verticalBorder);
        qreal x = area.left();
        for (int col = 0; col + 1 < table.layout.columns; ++col) {
            x += table.layout.columnWidth.at(col);
            column(x, area.top(), area.height(), look.columnSeparator);
        }

        // Выбранная таблица — с уголками-мишенями, как выбранная фотография:
        // объект под кареткой человек видит одинаково, чем бы объект ни был.
        const QTextCursor caret = textCursor();
        const bool selected = !caret.hasSelection() &&
                              caret.blockNumber() >= table.first &&
                              caret.blockNumber() <= table.last;
        if (selected && exportRatio_ <= 0.0) paintImageCorners(painter, area);

        // Текст ячеек. Выравнивание — из :---: разбора; по умолчанию влево,
        // как в GitHub.
        const qreal padX = tableCellPadX(table.layout.scale);
        const qreal padY = tableCellPadY(table.layout.scale);
        for (const TableCellBox& cell : table.layout.cells) {
            if (cell.text == nullptr) continue;
            const QRectF box = cell.rect.translated(area.topLeft());
            const qreal textWidth = cell.textWidth;
            qreal x = box.left() + padX;
            if (cell.align == TableAlign::Right)
                x = box.right() - padX - textWidth;
            else if (cell.align == TableAlign::Center)
                x = box.left() + (box.width() - textWidth) / 2;
            painter.setPen(palette().color(QPalette::Text));
            cell.text->draw(&painter, QPointF(x, box.top() + padY));
        }
    }
    painter.restore();
}

int NoteView::tableAtPoint(const QPointF& documentPoint) const {
    for (const TableRender& table : std::as_const(tables_)) {
        const QRectF area = tableRect(table.first);
        if (!area.isEmpty() && area.contains(documentPoint)) return table.first;
    }
    return -1;
}

int NoteView::sourcePositionAt(const QPointF& documentPoint) const {
    const int first = tableAtPoint(documentPoint);
    if (first < 0) return -1;
    const TableRender* table = tableAt(first);
    const QRectF area = tableRect(first);
    if (table == nullptr || area.isEmpty()) return -1;

    // РЯД по вертикали щелчка, КОЛОНКА по горизонтали. Ряд заголовка — первая
    // строка исходника, ряды тела — со второй, потому что между ними стоит
    // строка-разделитель, которой на экране нет.
    int row = 0;
    qreal y = area.top();
    for (; row + 1 < table->layout.rows; ++row) {
        if (documentPoint.y() < y + table->layout.rowHeight.at(row)) break;
        y += table->layout.rowHeight.at(row);
    }
    int column = 0;
    qreal x = area.left();
    for (; column + 1 < table->layout.columns; ++column) {
        if (documentPoint.x() < x + table->layout.columnWidth.at(column)) break;
        x += table->layout.columnWidth.at(column);
    }

    const int line = table->first + (row == 0 ? 0 : row + 1);
    const QTextBlock block = document()->findBlockByNumber(line);
    if (!block.isValid()) return -1;

    // Начало ячейки в ИСХОДНОЙ строке: считаем неэкранированные палки. Первая
    // палка строки — оформление, а не пустая ячейка.
    const QString source = block.text();
    int at = 0;
    while (at < source.size() && source.at(at).isSpace()) ++at;
    if (at < source.size() && source.at(at) == QLatin1Char('|')) ++at;
    int seen = 0;
    int cellStart = at;
    for (; at < source.size() && seen < column; ++at) {
        if (source.at(at) == QLatin1Char('\\')) { ++at; continue; }
        if (source.at(at) != QLatin1Char('|')) continue;
        ++seen;
        cellStart = at + 1;
    }
    if (seen < column) cellStart = source.size();
    // Пробел после палки в позицию ячейки не входит: человек целился в текст.
    while (cellStart < source.size() && source.at(cellStart) == QLatin1Char(' ')) ++cellStart;

    // Позиция ВНУТРИ ячейки — по x щелчка. Точна для ячеек без разметки: там
    // показанный текст и есть исходный. В ячейке с разметкой показанное и
    // исходное — разные строки (звёздочек на экране нет), и честного отображения
    // одной в другую у нас пока нет: встаём в начало ячейки, а не наугад.
    int inside = 0;
    if (const TableCellBox* cell = table->layout.at(row, column)) {
        if (cell->text != nullptr && cell->text->lineCount() > 0) {
            const QString shown = cell->text->text();
            const QString raw = source.mid(cellStart);
            const bool plain = raw.startsWith(shown);
            if (plain) {
                const qreal padX = tableCellPadX(table->layout.scale);
                const QPointF local(documentPoint.x() - (area.left() + cell->rect.left()) - padX,
                                    documentPoint.y() - (area.top() + cell->rect.top()));
                const QTextLine textLine = cell->text->lineForTextPosition(0);
                inside = textLine.isValid() ? textLine.xToCursor(local.x()) : 0;
                inside = qBound(0, inside, shown.size());
            }
        }
    }
    return block.position() + qMin(cellStart + inside, block.length() - 1);
}

void NoteView::mouseDoubleClickEvent(QMouseEvent* event) {
    // Двойной щелчок по сетке — правка исходника, каретка в ту ячейку, куда
    // целились. Одинарный только выбирает таблицу (см. mousePressEvent).
    if (event->button() == Qt::LeftButton && !isReadOnly()) {
        const QPointF at = toDocument(event->position().toPoint());
        // ПО ФОРМУЛЕ — ТО ЖЕ САМОЕ. Без этой ветки двойной щелчок доставался
        // QTextBrowser, и тот выделял «слово» в погашенном исходнике: на экране
        // поперёк вёрстки ложилась синяя полоса выделения, а буква, набранная
        // следом, уходила прямо в LaTeX — запрет на объекте выделение снимает.
        // По формуле щелчок больше ничего не делает ЗДЕСЬ: раскрыть её —
        // правка документа, а вид документ не правит. Это дело редактора
        // (NoteEditor::mouseDoubleClickEvent → ZDocument::openFormula).
        
        const int position = sourcePositionAt(at);
        if (position >= 0) {
            const int first = tableAtPoint(at);
            setEditedTable(first);
            QTextCursor caret(document());
            caret.setPosition(qBound(0, position, document()->characterCount() - 1));
            setTextCursor(caret);
            event->accept();
            return;
        }
    }
    QTextBrowser::mouseDoubleClickEvent(event);
}

void NoteView::setEditedTable(int firstBlockNumber) {
    if (editedTable_ == firstBlockNumber) return;
    editedTable_ = firstBlockNumber;
    syncImageSpace(true);   // перемерить: строки то прячутся, то возвращаются
    viewport()->update();
}

int NoteView::tableNearCaret() const {
    const QTextBlock block = textCursor().block();
    const BlockObject own = objectOf(block);
    if (own.kind == ObjectKind::Table) return own.first;

    // Пустая строка сразу за таблицей — ещё «в таблице»: её заводит Enter,
    // когда человек добавляет ряд.
    if (block.text().trimmed().isEmpty()) {
        const BlockObject above = objectOf(block.previous());
        if (above.kind == ObjectKind::Table) return above.first;
    }

    // ТАБЛИЦА МОЖЕТ ВРЕМЕННО ПЕРЕСТАТЬ БЫТЬ ТАБЛИЦЕЙ, и это нормально.
    //
    // Enter внутри исходника заводит пустую строку, а пустая строка кончает
    // таблицу — то есть на миг между двумя нажатиями кусок перестаёт быть
    // таблицей вовсе (проверено: все блоки становятся абзацами). Обрывать
    // правку в этот миг значит выкидывать человека из таблицы ровно тогда,
    // когда он добавляет ряд. Поэтому пока каретка среди строк с палками —
    // правка продолжается, и показывается исходник, а не сетка.
    const auto looksLikeRow = [](const QTextBlock& line) {
        return line.isValid() && line.text().contains(QLatin1Char('|'));
    };
    if (looksLikeRow(block) || looksLikeRow(block.previous()) || looksLikeRow(block.next()))
        return editedTable_;

    return -1;
}

QRectF NoteView::tableRect(int firstBlockNumber) const {
    const TableRender* table = tableAt(firstBlockNumber);
    if (table == nullptr) return {};
    const QTextBlock last = document()->findBlockByNumber(table->last);
    if (!last.isValid()) return {};
    const QRectF anchorRect = document()->documentLayout()->blockBoundingRect(last);
    return QRectF(anchorRect.left(), anchorRect.top(), table->layout.width,
                  table->layout.height);
}

bool NoteView::snapCaretOutOfHiddenTable() {
    QTextCursor caret = textCursor();
    if (caret.hasSelection()) return false;
    const QTextBlock block = caret.block();
    if (!block.isValid() || block.isVisible()) return false;

    // Спрятанные строки бывают только у таблицы, показанной сеткой: ставим
    // каретку на её ВИДИМУЮ строку — ту, на которой висит резерв. Дальше
    // таблица считается выбранной, как выбрана картинка под кареткой.
    const BlockObject object = objectOf(block);
    if (object.kind != ObjectKind::Table) return false;
    QTextCursor at(document()->findBlockByNumber(object.last));
    setTextCursor(at);
    return true;
}

const TableRender* NoteView::tableAt(int firstBlockNumber) const {
    const auto it = tables_.constFind(firstBlockNumber);
    return it == tables_.constEnd() ? nullptr : &it.value();
}

// Место под имя языка: вся полоска слева от кнопки. Надпись в нём прижата
// ВПРАВО, к кнопке, а прямоугольник остаётся широким нарочно — по нему ловится
// щелчок, и у блока без языка целиться человеку было бы некуда.
QRectF NoteView::languageRect(const CodeBand& band) const {
    const CodePlate plate = codePlate();
    if (!band.last || plate.strip <= 0.0) return {};
    const qreal left = band.rect.left() + plate.padLeft;
    const QRectF button = copyButtonRect(band);
    const qreal right = button.isEmpty() ? band.rect.right() : button.left() - plate.langGap;
    if (right <= left) return {};
    return QRectF(left, band.rect.bottom(), right - left, plate.strip);
}

// НЕПРОЗРАЧНЫЙ ЦВЕТ ПЛАШКИ. В настройках подложка кода задана ТОНИРОВКОЙ —
// чёрным с альфой 14, — и это правильно: она обязана одинаково ложиться и на
// обычную страницу, и на пожелтевший фон режима истории. Но рисовать
// полупрозрачным нельзя: две заливки внахлёст дают удвоенную плотность, а две
// сомкнувшиеся сглаженные кромки, наоборот, недобирают её — замер столбцом
// показал светлый шов в один пиксель каждые 23 (плашка стояла на дробных
// высотах строк). Поэтому тонировку смешиваем с цветом страницы ОДИН РАЗ и
// дальше кладём готовый непрозрачный цвет.
QColor NoteView::plateColour() const {
    const QColor page = pageColour();
    const QColor tint = appearance().codeBackground;
    const qreal a = tint.alphaF();
    return QColor::fromRgbF(page.redF() * (1 - a) + tint.redF() * a,
                            page.greenF() * (1 - a) + tint.greenF() * a,
                            page.blueF() * (1 - a) + tint.blueF() * a);
}

// ОДНА ЗАЛИВКА НА ВЕСЬ БЛОК КОДА, а не на строку.
//
// Блок кода лежит в документе по QTextBlock на строку (так задумано: Qt
// переразмечает блок целиком, и длинный блок кода делал набор внутри себя
// ощутимо медленным). Плашка же — одна фигура, и рисовать её надо одной: пока
// каждая строка красилась своим прямоугольником, между ними оставался шов.
void NoteView::paintCodeBackground(QPainter& painter, const QRectF& visible) {
    const CodePlate plate = codePlate();
    const QVector<CodeBand> bands = codeBands(visible);

    // СОСТОЯНИЕ ВОЗВРАЩАЕМ. Оставленная в painter'е кисть на экране безвредна —
    // растровый painter при drawImage на неё не смотрит. А вот PDF смотрит: Qt
    // складывает альфу кисти в состояние картинки, и фотография уехала на
    // бумагу с прозрачностью 5% — бледной тенью. Найдено глазами по вывезенной
    // странице, в самом файле это выглядело как "/ca 0.054901960" перед
    // вставкой снимка.
    painter.save();
    painter.setPen(Qt::NoPen);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QColor fill = plateColour();

    for (int i = 0; i < bands.size();) {
        // Прогон строк одного блока: от этой до той, у которой стоит last.
        // Соседство проверяем и по номеру блока — окно могло открыться прямо
        // посреди блока кода, и тогда first у первой полосы не стоит.
        int j = i;
        QRectF whole = bands[i].rect;
        while (j + 1 < bands.size() && !bands[j].last &&
               bands[j + 1].blockNumber == bands[j].blockNumber + 1) {
            ++j;
            whole |= bands[j].rect;
        }

        // Воздух сверху и полоска снизу — это ПОЛЯ БЛОКА, зарезервированные
        // сборщиком документа теми же величинами (codePlate в settings.h).
        // Прямоугольник блока полей не включает — замерено пробником, а не
        // взято из документации.
        const qreal top = whole.top() - (bands[i].first ? plate.padTop : 0.0);
        const qreal bottom = whole.bottom() + (bands[j].last ? plate.strip : 0.0);
        const QRectF box(whole.left(), top, whole.width(), bottom - top);
        const QPainterPath path = platePath(box, plate.radius, bands[i].first, bands[j].last);

        painter.save();
        painter.setClipPath(path, Qt::IntersectClip);
        painter.fillRect(box, fill);
        painter.restore();

        // На бумаге содержимого полоски нет вовсе (решение владельца): ни имени
        // языка, ни кнопки. Скруглённые углы и поля остаются — плашка на
        // странице выглядит как на экране, только без органов управления.
        if (bands[j].last && exportRatio_ <= 0.0) paintCodeStrip(painter, bands[j]);
        i = j + 1;
    }
    painter.restore();
}

// Содержимое полоски: имя языка и кнопка копирования, оба в ПРАВОМ НИЖНЕМ углу
// плашки (решение владельца). Надпись прижата вправо, к кнопке: слева от них —
// пустое поле полоски, и оно же служит мишенью для щелчка по имени языка.
void NoteView::paintCodeStrip(QPainter& painter, const CodeBand& band) {
    const CodePlate plate = codePlate();
    if (plate.strip <= 0.0) return;
    // Язык лежит на КАЖДОЙ строке блока (сборщик ставит его всем строкам), и
    // последняя знает его не хуже первой — а рисуем мы именно у последней.
    const QRectF where = languageRect(band);

    painter.save();
    if (!band.info.isEmpty() && !where.isEmpty() &&
        band.firstBlockNumber != editedCodeLanguage_) {
        painter.setFont(codeLangFont());
        painter.setPen(appearance().codeLangColor);
        painter.drawText(where, Qt::AlignVCenter | Qt::AlignRight, band.info);
    }

    // Кнопка видна ВСЕГДА, а не по наведению (решение владельца): слежение за
    // мышью ради значка — это перерисовка вьюпорта на каждое движение, а
    // выигрыш только в том, что серого значка не видно, пока он не нужен.
    // На бумаге кнопки нет: нажимать там нечего.
    if (exportRatio_ <= 0.0) {
        const QRectF box = copyButtonRect(band);
        const bool done = band.firstBlockNumber == copiedCodeBlock_;
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
            // Полоска — у ПОСЛЕДНЕЙ строки блока, там же и оба органа
            // управления; зовётся блок по первой (band.firstBlockNumber).
            if (!band.last) continue;
            const QRectF box = copyButtonRect(band);
            if (!box.isEmpty() && box.contains(at)) {
                copyCodeBlock(band.firstBlockNumber);
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
            emit codeStripClicked(band.firstBlockNumber, inViewport);
            event->accept();
            return;
        }

        // Щелчок по сетке таблицы — выбрать её целиком. Пускать сюда Qt нельзя:
        // про резерв места она не знает и ставит каретку по своим правилам,
        // случайно попадая то в спрятанную строку, то в соседний абзац.
        const int table = tableAtPoint(at);
        if (table >= 0 && table != editedTable_) {
            const TableRender* render = tableAt(table);
            if (render != nullptr) {
                setTextCursor(QTextCursor(document()->findBlockByNumber(render->last)));
                event->accept();
                return;
            }
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

    const QFont base = baseFont();
    QPainter painter(viewport());
    repaintOverNativeCaret(painter);
    painter.translate(-horizontalScrollBar()->value(), -verticalScrollBar()->value());

    const QRectF visible(horizontalScrollBar()->value() + event->rect().x(),
                         verticalScrollBar()->value() + event->rect().y(),
                         event->rect().width(), event->rect().height());

    // Сетка таблиц — поверх текста: под ней остаётся видимой последняя строка
    // исходника, и закрыть её может только то, что нарисовано после.
    paintTables(painter, visible);

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
        paintDivider(painter, block, rect, displayScale());
        // Фотографию рисует обработчик объектов (ImageObjectHandler): Qt зовёт
        // его сама, отведя объекту место в строке. Здесь остаются только уголки
        // выбранной — они ложатся поверх выделения — и те объекты, которые ещё
        // не переведены.
        paintImageMarks(painter, block);
        paintFormulaMarks(painter, block);
    }

    // Каретка — последней и без сдвига на прокрутку: cursorRect уже отдаёт
    // координаты вьюпорта. При выделении не рисуется вовсе: там видно и так, а
    // мигающая полоска на краю выделения только мешает. На строке-фотографии
    // тоже: там выбор показывает тонировка, а не полоска в углу картинки.
    //
    // И В НАРИСОВАННОЙ ТАБЛИЦЕ ЕЁ НЕТ. Каретка стоит на спрятанной строке
    // исходника, и Qt отдаёт под неё огрызок высотой в ничто — владелец увидел
    // «крохотный курсор, мигающий внутри таблицы». Выбранную таблицу показывают
    // уголки, как и выбранную фотографию, а не полоска между ячейками.
    painter.resetTransform();
    if (caretOn_ && caretShouldBeDrawn(hasFocus(), isReadOnly(), textCursor().hasSelection(),
                                       caretOnDrawnObject())) {
        QRect at = cursorRect();
        at.setWidth(qMax(1, qRound(appearance().caretWidth * displayScale())));
        painter.fillRect(at, appearance().caretColor);
    }
}

}  // namespace zametti
