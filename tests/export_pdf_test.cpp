// Заметка на бумагу.
//
// Проверять тут нечего из того, что и так проверяет экран: PDF рисует ТОТ ЖЕ
// код, что и окно. Бумага добавляет ровно три вещи, и спрашивается только про
// них.
//
// РАЗРЕЗ. Он единственный ничем больше не покрыт, и у него есть вырожденные
// случаи: кусок ровно на кромке, кусок выше целой страницы, документ короче
// страницы. Разрез — чистая арифметика, поэтому проверяется прямо, функцией,
// а не «посмотрим на получившийся файл».
//
// РАЗМЕР КАРТИНКИ В ФАЙЛЕ. Правило владельца: уменьшенная мышью фотография
// обязана уехать уменьшенной. Проверяется ВЕСОМ ФАЙЛА при одной и той же
// картинке и разной заданной ширине: если бы в PDF ложились исходные пиксели,
// оба файла весили бы одинаково. Это единственная проверка, которая ловит
// именно то, о чём просил владелец.
//
// ВШИТЫЕ ШРИФТЫ. Без них страница поедет у любого, у кого нет IBM Plex, — а
// нет её почти у всех. Спрашивается у самого файла: у вшитого шрифта в PDF
// есть дескриптор с потоком, у невшитого — нет.

#include "document_builder.h"
#include "pieces.h"
#include "doc_model.h"
#include "export_pdf.h"
#include "formula.h"
#include "settings.h"
#include "test_util.h"
#include "testdata.h"

#include <QApplication>
#include <QProcess>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTextDocument>

#include <string>
#include <vector>

namespace {

QString g_dir;

void put(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    if (file.open(QIODevice::WriteOnly)) file.write(bytes);
}

// --- разрез -------------------------------------------------------------

std::string show(const std::vector<qreal>& cuts) {
    std::string out;
    for (const qreal at : cuts) out += std::to_string(int(at)) + " ";
    return out;
}

void checkCuts() {
    // Документ короче страницы: одна страница, и резать нечего.
    ZT_EQ("короткий документ — одна страница", std::string("0 300 "),
          show(zametti::pageCuts({}, 300.0, 1000.0)));

    // Пусто: режем ровно по кромкам.
    ZT_EQ("без неделимых кусков режем по кромкам", std::string("0 100 200 250 "),
          show(zametti::pageCuts({}, 250.0, 100.0)));

    // Строка сидит на кромке — разрез уезжает к её верху.
    ZT_EQ("строка на кромке уезжает на следующую страницу", std::string("0 95 195 250 "),
          show(zametti::pageCuts({{95.0, 110.0}, {195.0, 210.0}}, 250.0, 100.0)));

    // Кусок касается кромки снизу, но её не пересекает: трогать не надо.
    ZT_EQ("кусок вплотную к кромке не двигает разрез", std::string("0 100 200 250 "),
          show(zametti::pageCuts({{80.0, 100.0}}, 250.0, 100.0)));

    // Фотография выше целой страницы. Разрез уезжает к её верху — то есть она
    // НАЧИНАЕТСЯ С НОВОЙ СТРАНИЦЫ, а дальше режется как придётся: целиком ей не
    // влезть никуда. Первая страница получается короткой, и это правильно:
    // лучше пустое место, чем срезанный верх фотографии.
    ZT_EQ("кусок выше страницы начинается с новой и дальше режется как есть",
          std::string("0 10 110 210 300 "),
          show(zametti::pageCuts({{10.0, 250.0}}, 300.0, 100.0)));

    // Тот же кусок, но от самого верха: двигать разрез некуда, и зациклиться
    // на этом нельзя.
    ZT_EQ("кусок выше страницы от самого верха не зацикливает разрез",
          std::string("0 100 200 300 "),
          show(zametti::pageCuts({{0.0, 250.0}}, 300.0, 100.0)));

    // Порядок кусков на входе не важен: сортировка — забота самой функции.
    ZT_EQ("порядок кусков на входе не важен", std::string("0 95 195 250 "),
          show(zametti::pageCuts({{195.0, 210.0}, {95.0, 110.0}}, 250.0, 100.0)));
}

// --- настоящий файл -----------------------------------------------------

QString makeNote(const QString& name, const std::string& body) {
    const QString path = QDir(g_dir).filePath(name);
    put(path, QByteArray(("<!-- zametti\ncreated: 2020-01-01T00:00:00Z\n-->\n\n" + body).c_str()));
    return path;
}

qint64 exportAndSize(const QString& note, const QString& pdfName, int* pages) {
    const QString out = QDir(g_dir).filePath(pdfName);
    const zametti::ExportReport report = zametti::exportPdf(note, out);
    ZT_TRUE("вывоз в PDF удался: " + report.error.toStdString(), report.ok());
    if (pages != nullptr) *pages = report.pages;
    return QFileInfo(out).size();
}

// Картинка, которую нельзя ужать вхолостую: шум сжимается плохо, и разница в
// весе файла будет от размера, а не от удачи кодека.
void makePicture(const QString& path, int side) {
    QImage image(side, side, QImage::Format_RGB32);
    unsigned seed = 12345;
    for (int y = 0; y < side; ++y) {
        for (int x = 0; x < side; ++x) {
            seed = seed * 1103515245u + 12345u;
            image.setPixel(x, y, seed >> 8);
        }
    }
    image.save(path, "PNG");
}

void checkRealFile() {
    std::string body = "# Заметка на бумагу\n\n";
    for (int i = 0; i < 120; ++i)
        body += "Строка " + std::to_string(i) +
                ": текст, которого хватит на несколько страниц подряд.\n\n";
    const QString note = makeNote(QStringLiteral("01aaaaaaaaaaaa.md"), body);

    int pages = 0;
    const qint64 size = exportAndSize(note, QStringLiteral("текст.pdf"), &pages);
    ZT_TRUE("файл не пуст", size > 1000);
    ZT_TRUE("страниц вышло несколько, а не " + std::to_string(pages), pages > 1);

    QFile file(QDir(g_dir).filePath(QStringLiteral("текст.pdf")));
    ZT_TRUE("файл читается", file.open(QIODevice::ReadOnly));
    const QByteArray bytes = file.readAll();
    ZT_TRUE("это PDF", bytes.startsWith("%PDF"));
    // Вшитый шрифт в PDF — это FontFile/FontFile2/FontFile3 в дескрипторе.
    // Невшитый описан одним именем, и потока при нём нет.
    ZT_TRUE("шрифты вшиты в файл", bytes.contains("FontFile"));

    // НИЧЕГО НЕ РИСУЕТСЯ ПОЛУПРОЗРАЧНЫМ. Проверка про чужую кисть, забытую в
    // painter'е: подложка кода красится цветом с альфой 14 из 255, и на экране
    // оставленная кисть безвредна — растровый painter при drawImage на неё не
    // смотрит. PDF смотрит: Qt складывает альфу кисти в состояние картинки, и
    // фотография уехала на бумагу пятипроцентной тенью. В файле это выглядит
    // как состояние с "ca" меньше единицы.
    for (qsizetype at = bytes.indexOf("/ca "); at >= 0; at = bytes.indexOf("/ca ", at + 1)) {
        QByteArray value;
        for (qsizetype i = at + 4; i < bytes.size(); ++i) {
            const char ch = bytes.at(i);
            if ((ch < '0' || ch > '9') && ch != '.') break;
            value.append(ch);
        }
        ZT_TRUE("в файле нет полупрозрачных состояний, а найдено ca=" + value.toStdString(),
                value.toDouble() >= 1.0);
    }
}

// ГЛАВНАЯ ПРОВЕРКА: уменьшенная картинка едет уменьшенной.
void checkShrunkImage() {
    makePicture(QDir(g_dir).filePath(QStringLiteral("01bbbbbbbbbbbb.png")), 1200);

    const QString big = makeNote(QStringLiteral("01ccccccccccc1.md"),
                                 "# Во всю ширину\n\n![шум](01bbbbbbbbbbbb.png)\n");
    // "#w=120" — та самая запись, которую ставит перетаскивание угла мышью.
    const QString small = makeNote(QStringLiteral("01ccccccccccc2.md"),
                                   "# Уменьшенная\n\n![шум](01bbbbbbbbbbbb.png#w=120)\n");

    const qint64 bigSize = exportAndSize(big, QStringLiteral("большая.pdf"), nullptr);
    const qint64 smallSize = exportAndSize(small, QStringLiteral("малая.pdf"), nullptr);

    ZT_TRUE("уменьшенная картинка весит в файле заметно меньше: " +
                std::to_string(bigSize) + " против " + std::to_string(smallSize),
            smallSize * 4 < bigSize);
}

// ОБЪЕКТЫ ДОЛЖНЫ ДОЕХАТЬ ДО БУМАГИ: вёрстка формулы и пиксели снимка.
//
// Владелец: «сейчас ни картинки ни формулы не экспортируются нормально в PDF».
// Причина была одна на обоих: вывоз собирал документ и сразу печатал, ни разу
// не позвав подготовку объектов, — фотографиям не раздавались пиксели, формулы
// не считались вовсе, и на страницу уезжали пустое место и рамка «не посчитано».
//
// Спрашиваем PDF о КАРТИНКАХ ВНУТРИ НЕГО. И вёрстка формулы, и фотография
// ложатся на страницу растром, то есть объектом «/Subtype /Image»; у страницы с
// одним текстом его нет ни одного. Это прямее и надёжнее, чем мерить вес файла:
// вес зависит от сжатия, а наличие картинки — нет.
// Есть ли в PDF такой текст. Глифы в файле лежат номерами, поэтому спрашиваем
// внешний pdftotext — он есть всюду, где есть poppler. Нет его — проверка
// честно пропускается, а не притворяется пройденной.
bool pdfHasText(const QString& path, const QString& what) {
    QProcess tool;
    tool.start(QStringLiteral("pdftotext"), {path, QStringLiteral("-")});
    if (!tool.waitForStarted(2000)) {
        std::printf("  pdftotext не найден — проверку текста пропускаем\n");
        return true;
    }
    tool.waitForFinished(10000);
    return QString::fromUtf8(tool.readAllStandardOutput()).contains(what);
}

int imagesInPdf(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return -1;
    const QByteArray bytes = file.readAll();
    int found = 0;
    int at = 0;
    while ((at = int(bytes.indexOf("/Subtype/Image", at))) >= 0) {
        ++found;
        at += 14;
    }
    // Qt пишет и с пробелом между ключом и значением — считаем обе формы.
    at = 0;
    while ((at = int(bytes.indexOf("/Subtype /Image", at))) >= 0) {
        ++found;
        at += 15;
    }
    return found;
}

// СТРОКИ НА БУМАГЕ НЕ ОБРЕЗАЮТСЯ.
//
// Владелец: «при экспорте в PDF строки обрезаются, причём иногда довольно
// сильно». Причина была в ширине вёрстки: подгонка вьюпорта растила ВИДЖЕТ на
// ширину его полей, applyContentWidth эти поля тут же обнуляла, и виджет
// оставался шире страницы — вёрстка шла по 765 при странице 679, а всё, что не
// влезло, страница обрезала на полуслове.
//
// Проверяется по САМОМУ PDF: если строка обрезана, её хвоста в файле нет вовсе.
// Текст в PDF лежит глифами, поэтому спрашиваем не байты, а ширину: сколько
// места заняла бы строка при нынешней вёрстке. Прямее — сравнить ширину
// разметки со страницей: она обязана быть НЕ БОЛЬШЕ.
void checkLinesFitPage() {
    // УСЛОВИЕ БЕДЫ ЗАДАЁТСЯ ЯВНО. Строки обрезались не всегда, а когда колонка
    // упирается в ПОТОЛОК СВОЕЙ ШИРИНЫ: тогда вид заводит боковые поля вьюпорта,
    // подгонка растит под них виджет, и он остаётся шире страницы. С потолком по
    // умолчанию (90 знаков) на A4 запаса нет, полей не заводится, и проверка
    // была бы пустышкой — я на этом и попался, пока не задал число сам.
    const qreal savedWidth = zametti::settings().look().maxContentWidth();
    zametti::editSettings().look().setMaxContentWidth(40.0);
    struct Restore {
        qreal width;
        ~Restore() { zametti::editSettings().look().setMaxContentWidth(width); }
    } restore{savedWidth};

    std::string source = "# Длинная\n\n";
    for (int i = 1; i <= 12; ++i) {
        source += "Абзац номер " + std::to_string(i) +
                  ". Он достаточно длинный, чтобы занять пару строк на странице и "
                  "довести содержимое до разреза между страницами.\n\n";
    }
    const QString note = makeNote(QStringLiteral("01ffffffffff01.md"), source.c_str());
    const QString pdf = QDir(g_dir).filePath(QStringLiteral("длинная.pdf"));

    // ПОЛЯ ПОУЖЕ — СТРАНИЦА ПОШИРЕ, и колонка перестаёт помещаться в потолок
    // ширины (maxContentWidth). Именно тогда вид и заводит боковые поля
    // вьюпорта, ради которых подгонка растит виджет, — то есть ровно то
    // условие, при котором строки и обрезались. С полями по умолчанию беда не
    // всплывает вовсе, и проверка была бы пустышкой.
    zametti::PdfOptions options;
    const zametti::ExportReport report = zametti::exportPdf(note, pdf, options);
    ZT_TRUE("длинная заметка вывезена", report.ok());
    ZT_TRUE("страниц больше одной: разрез действительно случился", report.pages >= 1);

    // Хвост последней строки абзаца обязан быть в файле. Обрезанная строка
    // кончается на полуслове, и этих слов в PDF просто нет.
    // ВЁРСТКА ОБЯЗАНА ИДТИ РОВНО ПО СТРАНИЦЕ. Спрашивать сам PDF бесполезно:
    // отсечённые глифы в файл всё равно попадают, и pdftotext показывает целый
    // текст даже там, где на бумаге строка кончается на полуслове. Я на этом и
    // попался: первая редакция проверки читала текст и не краснела вовсе.
    ZT_TRUE("вёрстка идёт по ширине страницы: " + std::to_string(int(report.layoutWidth)) +
                " при странице " + std::to_string(int(report.pageWidth)),
            report.ok() && std::fabs(report.layoutWidth - report.pageWidth) < 1.0);
    // И текст доезжает до бумаги целиком.
    ZT_TRUE("хвост абзаца на месте",
            report.ok() && pdfHasText(pdf, QStringLiteral("между страницами")));
}

void checkObjectsOnPaper() {
    if (!zametti::Formulas::ready()) {
        QString error;
        zametti::Formulas::init(&error);
    }

    makePicture(QDir(g_dir).filePath(QStringLiteral("01ddddddddddd1.png")), 300);
    const QString plain = makeNote(QStringLiteral("01eeeeeeeeee01.md"),
                                   "# Только текст\n\nАбзац без объектов.\n");
    const QString withImage = makeNote(QStringLiteral("01eeeeeeeeee02.md"),
                                       "# Со снимком\n\n![вид](01ddddddddddd1.png)\n");
    const QString withMath = makeNote(QStringLiteral("01eeeeeeeeee03.md"),
                                      "# С формулой\n\n$$\\frac{a}{b}$$\n");

    const QString plainPdf = QDir(g_dir).filePath(QStringLiteral("текст.pdf"));
    const QString imagePdf = QDir(g_dir).filePath(QStringLiteral("снимок.pdf"));
    const QString mathPdf = QDir(g_dir).filePath(QStringLiteral("формула.pdf"));
    ZT_TRUE("текст вывезен", zametti::exportPdf(plain, plainPdf).ok());
    ZT_TRUE("снимок вывезен", zametti::exportPdf(withImage, imagePdf).ok());
    ZT_TRUE("формула вывезена", zametti::exportPdf(withMath, mathPdf).ok());

    ZT_EQ("у страницы с одним текстом картинок внутри нет", "0",
          std::to_string(imagesInPdf(plainPdf)));
    ZT_TRUE("фотография попала на страницу растром: " +
                std::to_string(imagesInPdf(imagePdf)),
            imagesInPdf(imagePdf) > 0);
    ZT_TRUE("вёрстка формулы попала на страницу растром: " +
                std::to_string(imagesInPdf(mathPdf)),
            imagesInPdf(mathPdf) > 0);
}

// Что бумага делает с документом до отрисовки. Проверяется здесь, а не по
// готовому PDF: текст в PDF лежит номерами глифов, и «нет ли там комментария»
// у файла не спросишь.
void checkPaperPrep() {
    const std::string source =
        "# Первый раздел\n\n"
        "<!-- записка себе -->\n\n"
        "Комментарий <!-- сперва --> и только потом ссылка "
        "[внутрь](#первый-раздел), наружу "
        "[в вики](https://ru.wikipedia.org/wiki/Красно–чёрное_дерево) и "
        "ещё <!-- в строке --> внутри.\n\n"
        "![снимок](01n6vwnr03mxzq.jxl)\n";

    QTextDocument doc;
    zametti::buildDocument(pieces(source), doc);
    zametti::prepareForPaper(doc);

    QString all;
    bool headingMarked = false;
    bool imageKept = false;
    bool imageLinked = true;
    QStringList hrefs;
    for (QTextBlock block = doc.begin(); block.isValid(); block = block.next()) {
        all += block.text() + QLatin1Char('\n');
        if (!zametti::isRawBlock(block) && zametti::kindOf(block) == zametti::Kind::Heading) {
            for (QTextBlock::iterator it = block.begin(); !it.atEnd(); ++it)
                if (it.fragment().isValid() &&
                    it.fragment().charFormat().anchorNames().contains(
                        QStringLiteral("первый-раздел")))
                    headingMarked = true;
        }
        for (QTextBlock::iterator it = block.begin(); !it.atEnd(); ++it) {
            if (!it.fragment().isValid()) continue;
            const QTextCharFormat format = it.fragment().charFormat();
            if ((format.intProperty(zametti::SpanStyleProperty) & zametti::SpanImage) != 0) {
                imageKept = !format.anchorHref().isEmpty();
                imageLinked = format.isAnchor();
            } else if (!format.anchorHref().isEmpty()) {
                hrefs << format.anchorHref();
            }
        }
    }

    ZT_TRUE("комментарий блоком не попал на бумагу: " + all.toStdString(),
            !all.contains(QStringLiteral("записка себе")));
    ZT_TRUE("комментарий в строке не попал на бумагу: " + all.toStdString(),
            !all.contains(QStringLiteral("в строке")));
    ZT_TRUE("а сам текст на месте: " + all.toStdString(),
            all.contains(QStringLiteral("ссылка")) && all.contains(QStringLiteral("внутри")));
    // На месте вырезанного комментария не остаётся дыры из двух пробелов.
    ZT_TRUE("двойного пробела на месте комментария нет: " + all.toStdString(),
            !all.contains(QStringLiteral("  ")));

    // Заголовок стал ЦЕЛЬЮ. Без этого внутренние ссылки в PDF ведут в никуда:
    // Qt пишет их как ссылку на именованную цель, а целей в файле нет.
    ZT_TRUE("заголовок помечен именем цели", headingMarked);

    // Ссылка под фотографией погашена, но АДРЕС ОСТАЛСЯ: по нему вид и узнаёт
    // строку-фотографию. Первый заход стирал адрес — снимок пропадал вовсе.
    ZT_TRUE("адрес фотографии на месте", imageKept);
    ZT_TRUE("а ссылка под фотографией погашена", !imageLinked);

    bool inner = false;
    bool outer = false;
    for (const QString& href : hrefs) {
        // Внутренняя остаётся как есть: имя цели обязано совпасть знак в знак.
        if (href == QStringLiteral("#первый-раздел")) inner = true;
        // Внешняя приводится к процентной записи: Qt пишет URI однобайтовым, и
        // "Красно–чёрное" превратилось бы в вереницу "?".
        if (href.startsWith(QStringLiteral("https://")) && !href.contains(QChar(0x043A)) &&
            href.contains(QStringLiteral("%D")))
            outer = true;
    }
    ZT_TRUE("внутренняя ссылка не тронута: " + hrefs.join(QLatin1Char(' ')).toStdString(), inner);
    ZT_TRUE("внешняя приведена к процентной записи: " +
                hrefs.join(QLatin1Char(' ')).toStdString(),
            outer);
}

// Шрифт бумаги — из раздела pdf конфига, а не экранный.
void checkPaperFont() {
    const QString note = makeNote(QStringLiteral("01gggggggggggg.md"),
                                  "# Заголовок\n\nАбзац текста.\n\n```\nкод\n```\n");
    zametti::editSettings().pdf().setFontFamily(QStringLiteral("IBM Plex Sans"));
    zametti::editSettings().pdf().setCodeFamily(QStringLiteral("IBM Plex Mono"));
    exportAndSize(note, QStringLiteral("шрифты.pdf"), nullptr);

    QFile file(QDir(g_dir).filePath(QStringLiteral("шрифты.pdf")));
    ZT_TRUE("файл читается", file.open(QIODevice::ReadOnly));
    const QByteArray bytes = file.readAll();
    ZT_TRUE("текст набран бумажной гарнитурой", bytes.contains("IBMPlexSans"));
    ZT_TRUE("а код — своей", bytes.contains("IBMPlexMono"));
    // Облик возвращается на место: подмена живёт только внутри вывоза.
    ZT_TRUE("экранная гарнитура не тронута вывозом",
            zametti::settings().look().fontFamily() != QStringLiteral("IBM Plex Sans"));
}

}  // namespace

static int ztRunSuite(int argc, char** argv) {
    QTemporaryDir tmp;
    if (!tmp.isValid()) {
        std::printf("не завёлся временный каталог\n");
        return 2;
    }
    g_dir = tmp.path();

    // С путём к заметке набор превращается в инструмент: вывозит её рядом и
    // молчит. Глазами на страницу смотреть всё равно приходится — ни один
    // текстовый ответ не скажет, поехала вёрстка или нет.
    if (argc > 1) {
        const QString out = QString::fromUtf8(argv[1]) + QStringLiteral(".pdf");
        const zametti::ExportReport report = zametti::exportPdf(QString::fromUtf8(argv[1]), out);
        std::printf("%s: %s, страниц %d\n", out.toUtf8().constData(),
                    report.ok() ? "готово" : report.error.toUtf8().constData(), report.pages);
        return report.ok() ? 0 : 1;
    }

    checkLinesFitPage();
    checkCuts();
    checkPaperPrep();
    checkPaperFont();
    checkRealFile();
    checkShrunkImage();
    checkObjectsOnPaper();

    return zt::report("заметка на бумагу");
}

// Набор целиком одним TEST: тело не тронуто, argv ему собран здесь.
// Дробить на отдельные проверки — отдельная работа, по одному набору.
TEST(ExportPdf, All) {
    std::vector<QByteArray> ztArgs{QByteArrayLiteral("export_pdf_test")};
    std::vector<char*> ztArgv;
    for (QByteArray& a : ztArgs) ztArgv.push_back(a.data());
    EXPECT_EQ(0, ztRunSuite(int(ztArgv.size()), ztArgv.data()));
}
