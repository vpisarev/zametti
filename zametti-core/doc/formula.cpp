#include "formula.h"

#include <QByteArray>
#include <QFile>
#include <QMutex>
#include <QMutexLocker>
#include <QPainter>
#include <QRegularExpression>

#include <cmath>
#include <memory>
#include <vector>

#include "graphic_qt.h"
#include "microtex.h"

namespace zametti {
namespace {

// Байты .clm2 держим живыми на всё время работы: FontSrcData хранит указатель,
// а не копию, и движок ходит по ним при каждой отрисовке.
std::vector<QByteArray>& fontBytes() {
    static std::vector<QByteArray> bytes;
    return bytes;
}

// РАЗБОР ПОД МЬЮТЕКСОМ (оговорка разведки). Он стоит 0.03 мс — потери
// нулевые, — а контекст у движка глобальный: статический Config*, общие
// сеттеры. Сегодня рендер синхронный и мьютекс избыточен; он стоит здесь,
// чтобы фоновый рендер, если он однажды понадобится, не начинался с гонки.
QMutex& engineLock() {
    static QMutex lock;
    return lock;
}

bool g_ready = false;
int g_renders = 0;
QString g_mathFont;

// Одна гарнитура из ресурсов: .clm2 отдаём байтами, .otf — именем.
//
// Qt-бэкенд движка читает шрифт сам и умеет ресурсы: не найдя файла на диске,
// он приписывает к имени ":/" (upstream/platform/qt/graphic_qt.cpp,
// Font_qt::loadFont). Поэтому распаковывать во временный каталог ничего не
// надо, и бинарник остаётся самодостаточным.
bool loadFontFromResources(const QString& stem, bool asMath, QString* error) {
    QFile clm(QStringLiteral(":/fonts/math/%1.clm2").arg(stem));
    if (!clm.open(QIODevice::ReadOnly)) {
        if (error != nullptr) *error = QStringLiteral("missing resource %1.clm2").arg(stem);
        return false;
    }
    fontBytes().push_back(clm.readAll());
    const QByteArray& data = fontBytes().back();

    // Имя .otf БЕЗ ведущего ":/" — его допишет сам бэкенд. С двоеточием в
    // начале QFile::exists ответит истиной, и путь уйдёт в
    // QFontDatabase::addApplicationFont дважды преобразованным.
    const std::string otf = QStringLiteral("fonts/math/%1.otf").arg(stem).toStdString();
    const microtex::FontSrcData src(size_t(data.size()),
                                    reinterpret_cast<const microtex::u8*>(data.constData()), otf);
    const microtex::FontMeta meta = asMath ? microtex::MicroTeX::init(src)
                                           : microtex::MicroTeX::addFont(src);
    if (!meta.isValid()) {
        if (error != nullptr) *error = QStringLiteral("font %1 rejected by the engine").arg(stem);
        return false;
    }
    if (asMath) g_mathFont = QString::fromStdString(meta.name);
    else if (!meta.family.empty()) microtex::MicroTeX::setDefaultMainFont(meta.family);
    return true;
}

}  // namespace

QString checkLatex(const QString& latex) {
    // Движок из десяти сломанных формул жалуется на три, остальные семь
    // дорисовывает огрызком и молчит (замер разведки). Поэтому три самые
    // частые поломки ловим сами — до вызова.
    int braces = 0;
    int lefts = 0;
    int envs = 0;
    for (int i = 0; i < latex.size(); ++i) {
        const QChar c = latex[i];
        // Экранированное не считается: `\{` это знак, а не скобка.
        if (c == QLatin1Char('\\')) {
            ++i;
            continue;
        }
        if (c == QLatin1Char('{')) ++braces;
        else if (c == QLatin1Char('}')) {
            if (--braces < 0) return QStringLiteral("extra closing brace }");
        }
    }
    if (braces > 0)
        return QStringLiteral("unclosed brace: missing %1 «}»").arg(braces);

    // \left…\right и \begin…\end считаем по вхождениям команд: разбирать
    // синтаксис целиком мы не собираемся, а несведённая пара — это ровно то,
    // что движок проглатывает молча.
    static const QRegularExpression command(QStringLiteral("\\\\(left|right|begin|end)\\b"));
    auto it = command.globalMatch(latex);
    while (it.hasNext()) {
        const QString name = it.next().captured(1);
        if (name == QLatin1String("left")) ++lefts;
        else if (name == QLatin1String("right")) --lefts;
        else if (name == QLatin1String("begin")) ++envs;
        else --envs;
        if (lefts < 0) return QStringLiteral("\\right without \\left");
        if (envs < 0) return QStringLiteral("\\end without \\begin");
    }
    if (lefts > 0) return QStringLiteral("\\left without \\right");
    if (envs > 0) return QStringLiteral("\\begin without \\end");
    return {};
}

bool Formulas::init(QString* error) {
    if (g_ready) return true;
    QMutexLocker guard(&engineLock());
    if (g_ready) return true;

    // Порядок важен: сперва фабрика платформы, потом init — тот уже читает
    // шрифт, а чтение идёт через PlatformFactory::get().
    // ЕДИНСТВЕННЫЙ unique_ptr в дереве, и он не наш выбор: подпись
    // registerFactory принадлежит microtex. Правило владельца — «умный
    // указатель у нас один, shared_ptr» — про наш код; чужому API отдаём то,
    // что он просит.
    microtex::PlatformFactory::registerFactory(
        "qt", std::unique_ptr<microtex::PlatformFactory_qt>(
                  new microtex::PlatformFactory_qt()));
    microtex::PlatformFactory::activate("qt");

    if (!loadFontFromResources(QStringLiteral("EulerMath"), true, error)) return false;
    // Текстовые начертания: без них \text{…} внутри математики рисуется
    // математическим шрифтом — курсивом вместо прямого.
    for (const QString& stem :
         {QStringLiteral("lmroman10-regular"), QStringLiteral("lmroman10-italic"),
          QStringLiteral("lmroman10-bold"), QStringLiteral("lmroman10-bolditalic")}) {
        QString why;
        if (!loadFontFromResources(stem, false, &why))
            std::fprintf(stderr, "formula text font failed to load: %s\n",
                         why.toUtf8().constData());
    }
    g_ready = true;

    // ПРОГРЕВ. Первый рендер в процессе стоит 26 мс против 4 мс у всех
    // следующих: 22 мс уходит на прогрев кэша глифов (замер разведки). Эти
    // миллисекунды не должны достаться первой формуле человека.
    guard.unlock();
    const int before = g_renders;
    render(QStringLiteral("x^2"), false, 15.0, Qt::black, 1.0);
    g_renders = before;   // холостой рендер в счёт не идёт
    return true;
}

bool Formulas::ready() { return g_ready; }

int Formulas::renders() { return g_renders; }

void Formulas::resetRenders() { g_renders = 0; }

QString Formulas::mathFontName() { return g_mathFont; }

namespace {

// Общая часть render и paintInto: предконтроль, чистка пробелов, доллары по
// роду, разбор под мьютексом. nullptr — не разобралось, причина в error.
std::shared_ptr<microtex::Render> parseFormula(const QString& latex, bool display,
                                               qreal enginePixels, const QColor& colour,
                                               QString* error) {
    if (!g_ready) {
        *error = QStringLiteral("formula engine failed to start");
        return nullptr;
    }
    const QString broken = checkLatex(latex);
    if (!broken.isEmpty()) {
        *error = broken;
        return nullptr;
    }

    // ПРОБЕЛЫ ЮНИКОДА — ОБЫЧНЫЕ ПРОБЕЛЫ. Внутри формулы неразрывный пробел
    // (U+00A0) и его родня — это пробел и ничего больше: так их понимают KaTeX
    // и MathJax, то есть всё, чем эти заметки читают на стороне. MicroTeX же
    // рисует их НАСТОЯЩИМИ пробелами, и матрица, у которой строки отступлены
    // неразрывными, разъезжается дырами между столбцами.
    //
    // Найдено на заметке владельца: он расставил неразрывные руками, чтобы
    // куски формул не уезжали в комментарии, — и это законный markdown, а
    // разъехалась от него именно наша отрисовка.
    //
    // Правится ТОЛЬКО ТО, ЧТО УХОДИТ В ДВИЖОК. Файл не меняется ни на байт:
    // показ остаётся чистым чтением.
    QString clean = latex;
    for (QChar& ch : clean) {
        const char16_t code = ch.unicode();
        const bool space = code == 0x00A0 ||                    // неразрывный
                           (code >= 0x2000 && code <= 0x200A) || // круглая, полукруглая, тонкие
                           code == 0x202F || code == 0x205F ||   // узкий неразрывный, средний
                           code == 0x3000 ||                     // идеографический
                           code == 0x0009 || code == 0x000B ||   // табуляция, вертикальная
                           code == 0x000C || code == 0x0085;
        if (space) ch = QLatin1Char(' ');
    }

    // Выключная формула отличается от строчной ровно тем, чем в TeX: стилем
    // вёрстки. Движок сам понимает `$$…$$`, и это единственный способ получить
    // выключный стиль (пределы над знаком суммы, крупные дроби).
    const std::string source =
        (display ? QStringLiteral("$$%1$$") : QStringLiteral("$%1$")).arg(clean).toStdString();

    microtex::Render* raw = nullptr;
    {
        QMutexLocker guard(&engineLock());
        try {
            // fillWidth=false — иначе вёрстка растягивается на всю заданную
            // ширину: формула шириной 43 точки приезжает шириной 779.
            // Ширина здесь — только предел переноса, и он заведомо велик:
            // перенос формул мы не делаем, их ширину меряет вызывающий.
            raw = microtex::MicroTeX::parse(source, 100000, float(enginePixels), 0.0f,
                                            colour.rgba(), /*fillWidth=*/false);
        } catch (const std::exception& e) {
            *error = QString::fromUtf8(e.what());
        } catch (...) {
            // У MicroTeX ex_tex наследует std::exception, но ловим и всё
            // прочее: падать из-за формулы в заметке программа не имеет права.
            *error = QStringLiteral("formula engine threw an unknown exception");
        }
    }
    if (raw == nullptr) {
        if (error->isEmpty()) *error = QStringLiteral("formula engine produced no layout");
        return nullptr;
    }
    return std::shared_ptr<microtex::Render>(raw);
}

}  // namespace

FormulaImage Formulas::render(const QString& latex, bool display, qreal pixelSize,
                              const QColor& colour, qreal dpr) {
    FormulaImage out;
    const std::shared_ptr<microtex::Render> render =
        parseFormula(latex, display, pixelSize * dpr, colour, &out.error);
    if (render == nullptr) return out;

    const qreal physicalWidth = render->getWidth();
    const qreal physicalHeight = render->getHeight();
    // ДОЛЯ, А НЕ ПИКСЕЛИ. getBaseline() отдаёт долю ascent от полной высоты.
    const qreal physicalBaseline = render->getBaseline() * physicalHeight;

    if (physicalWidth <= 0.0 || physicalHeight <= 0.0) {
        // Вырожденная вёрстка — тоже ошибка, а не «пустая формула»: так
        // выглядит проглоченная движком поломка.
        out.error = QStringLiteral("empty layout %1×%2")
                        .arg(physicalWidth)
                        .arg(physicalHeight);
        return out;
    }

    // КОРОБКА ДВИЖКА ЧЕРНИЛА НЕ ДЕРЖИТ. getHeight() — логическая высота
    // вёрстки, а глифы рисуются за неё: хвост «γ», скобки, знак корня, круглый
    // низ «e» и нижний индекс свисают ниже на пиксель-другой, и по левому и
    // правому краю бывает то же. Растр ровно по коробке эти хвосты СРЕЗАЛ —
    // владелец видел это как «на некоторых масштабах низ формул явно режется»
    // ($e^{-x}$, $x_0^2$ в «Typesetting Math in Markdown»); замер: из 190
    // вёрсток на кеглях 12…30 свисают 172.
    //
    // Поэтому рисуем на холсте с полями, находим настоящие границы чернил и
    // РАСШИРЯЕМ КОРОБКУ до них. Расширение согласованно уезжает в геометрию:
    // baseline растёт на добавленное сверху, depth — на добавленное снизу,
    // поэтому посадка на базовую линию строки остаётся прежней, а места под
    // формулу становится ровно столько, сколько она занимает чернилами.
    // Влево расширяем тоже: чернила выходят и за левый край (курсивные буквы,
    // знак корня). Формула при этом сдвигается вправо на добавленное — не
    // больше пикселя, — и это дешевле, чем срезанный край.
    const int boxWidth = int(std::ceil(physicalWidth));
    const int boxHeight = int(std::ceil(physicalHeight));
    const int pad = std::max(3, int(std::ceil(physicalHeight * 0.3)));
    QImage canvas(boxWidth + 2 * pad, boxHeight + 2 * pad, QImage::Format_ARGB32_Premultiplied);
    canvas.fill(Qt::transparent);
    {
        QPainter painter(&canvas);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setRenderHint(QPainter::TextAntialiasing, true);
        painter.translate(pad, pad);
        microtex::Graphics2D_qt g2(&painter);
        render->draw(g2, 0, 0);
    }
    int inkTop = -1;
    int inkBottom = -1;
    int inkLeft = -1;
    int inkRight = -1;
    for (int y = 0; y < canvas.height(); ++y) {
        const auto* row = reinterpret_cast<const QRgb*>(canvas.constScanLine(y));
        for (int x = 0; x < canvas.width(); ++x) {
            if (qAlpha(row[x]) == 0) continue;
            if (inkTop < 0) inkTop = y;
            inkBottom = y;
            if (inkLeft < 0 || x < inkLeft) inkLeft = x;
            if (x > inkRight) inkRight = x;
        }
    }
    const int extraTop = inkTop < 0 ? 0 : std::max(0, pad - inkTop);
    const int extraBottom = inkBottom < 0 ? 0 : std::max(0, inkBottom + 1 - (pad + boxHeight));
    const int extraLeft = inkLeft < 0 ? 0 : std::max(0, pad - inkLeft);
    const int extraRight = inkRight < 0 ? 0 : std::max(0, inkRight + 1 - (pad + boxWidth));
    QImage image = canvas.copy(QRect(pad - extraLeft, pad - extraTop, boxWidth + extraLeft + extraRight,
                                     boxHeight + extraTop + extraBottom));
    // ПЛОТНОСТЬ КАРТИНКЕ НЕ ПРОСТАВЛЯЕМ. С ней «логический» размер картинки
    // считает Qt, и рисование зависит от того, какой формой drawImage её
    // попросили нарисовать. Здесь картинка — просто пиксели в физических
    // точках: кто рисует, тот и задаёт прямоугольник в логических, а источник
    // в физических (см. note_view.cpp). Тогда ответ один при любой плотности и
    // не зависит от того, как Qt толкует пометку.
    out.image = image;
    out.width = (physicalWidth + extraLeft + extraRight) / dpr;
    out.height = (physicalHeight + extraTop + extraBottom) / dpr;
    out.depth = (render->getDepth() + extraBottom) / dpr;
    out.baseline = (physicalBaseline + extraTop) / dpr;
    // Насколько верх и левый край картинки выходят за коробку движка: вектору
    // на бумаге рисовать от того же угла, что и растру, значит его надо
    // сдвинуть на эти доли.
    out.padTop = extraTop / dpr;
    out.padLeft = extraLeft / dpr;
    ++g_renders;
    return out;
}

QString Formulas::paintInto(QPainter& painter, const QPointF& at, const QString& latex,
                            bool display, qreal pixelSize, const QColor& colour) {
    QString error;
    // Кегль ЛОГИЧЕСКИЙ, без плотности: вёрстка движка детерминирована от
    // кегля, и геометрия совпадает с той, по которой кэш мерил место.
    const std::shared_ptr<microtex::Render> render =
        parseFormula(latex, display, pixelSize, colour, &error);
    if (render == nullptr) return error;
    if (render->getWidth() <= 0 || render->getHeight() <= 0)
        return QStringLiteral("empty layout");

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    painter.translate(at);
    {
        microtex::Graphics2D_qt g2(&painter);
        render->draw(g2, 0, 0);
    }
    painter.restore();
    ++g_renders;
    return {};
}

}  // namespace zametti
