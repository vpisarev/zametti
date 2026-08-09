// Снимки приёмки этапа 8: то, что владелец проверяет глазами.
//
// Критерий из брифа дословно: «портретное фото с телефона стоит, не лежит;
// скриншот читаем на 100%; панорама на широком окне резкая; пять файлов легли
// одна под другой, один Ctrl+Z убрал всё».
//
// Набор делает две вещи разом, и обе нужны:
//
//   * ПРОВЕРЯЕТ то, что вообще поддаётся проверке числом: соотношение сторон
//     показанной картинки, порядок блоков, вписанность в колонку. Такие вещи
//     глазами как раз ловятся плохо — «на глаз похоже» слишком часто означает
//     «повёрнуто и никто не заметил»;
//   * КЛАДЁТ СНИМКИ на диск. Всё остальное — резкость, читаемость, цвет —
//     числом не берётся, и здесь снимок и есть отчёт. Каталог печатается в
//     вывод, чтобы не искать.
//
// Окно берётся И ШИРОКОЕ, И УЗКОЕ. Все прежние тестовые окна были узкие, и
// целый класс расхождений был невидим: панорама в узком окне упирается в
// ширину колонки всегда, и вписана она или нет — не видно.

#include "doc_model.h"
#include "editor_widget.h"
#include "image_insert.h"
#include "settings.h"
#include "test_util.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QTest>
#include <QTextBlock>
#include <QAbstractTextDocumentLayout>
#include <QTextDocument>

#include <cmath>
#include <string>

namespace {

// Ввоз картинок стал фоновым: кто ждёт его результата, обязан дождаться.
// Признак конца — редактор снова правится (на время ввоза он читалка).
void waitForImport(zametti::NoteEditor& editor) {
    QElapsedTimer waiting;
    waiting.start();
    while (editor.isReadOnly() && waiting.elapsed() < 60000)
        QApplication::processEvents(QEventLoop::AllEvents, 20);
    QApplication::processEvents();
}

QString g_corpus;   // .testdata/images/originals
QString g_store;    // куда кладём заметку и вложения
QString g_shots;    // куда кладём снимки

std::string num(long long v) { return std::to_string(v); }

QString writeNote(const QString& name, const QString& text) {
    const QString path = QDir(g_store).filePath(name);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return {};
    f.write(text.toUtf8());
    f.close();
    return path;
}

// Снимок окна целиком: не viewport, а весь виджет — на приёмке смотрят на
// программу, а не на её внутренности.
void shoot(zametti::NoteEditor& editor, const QString& name) {
    // ВЫДЕЛЕНИЕ СНИМАЕМ и курсор уводим в начало. Иначе на снимке приёмки
    // фотография лежит под синей плёнкой выделения, и о её цвете по такому
    // снимку не скажешь ничего — а именно за цветом на приёмке и смотрят.
    QTextCursor at = editor.textCursor();
    at.setPosition(0);
    editor.setTextCursor(at);
    QTest::qWait(60);   // дать разметке и декоду дойти до конца
    const QImage shot = editor.grab().toImage();
    const QString path = QDir(g_shots).filePath(name + QStringLiteral(".png"));
    if (!shot.save(path)) std::printf("  НЕ СОХРАНИЛСЯ снимок %s\n", qPrintable(path));
}

// Геометрия картинки на экране. Отвечает на главный вопрос приёмки: как она
// ВЫГЛЯДИТ, а не что записано в файле.
struct Shown {
    QRectF rect;
    bool found = false;
};

// Геометрию берём У САМОГО ПОКАЗА (imageGeometry), а не blockBoundingRect.
// Первая редакция брала прямоугольник блока — и получала 810×20, высоту
// пустой строки: картинка рисуется поверх блока по своей геометрии, и блок о
// её размерах ничего не знает. Проверка на поворот на таких числах не значила
// бы ничего.
class Shots : public zametti::NoteEditor {
public:
    QRectF photoRect(const QTextBlock& block) { return imageGeometry(block).photo; }
    bool photoValid(const QTextBlock& block) { return imageGeometry(block).valid; }
};

Shown shownImage(Shots& editor, int which = 0) {
    int seen = 0;
    for (QTextBlock b = editor.document()->firstBlock(); b.isValid(); b = b.next()) {
        if (!zametti::blockImageRef(b).valid) continue;
        if (seen++ != which) continue;
        Shown out;
        out.found = editor.photoValid(b);
        out.rect = editor.photoRect(b);
        return out;
    }
    return {};
}

Shots* openWith(Shots& editor, int width, int height,
                              const QString& noteName) {
    const QString note = writeNote(noteName, QStringLiteral("Приёмка этапа 8\n"));
    editor.resize(width, height);
    editor.show();
    QTest::qWait(20);
    editor.openFile(note);
    QTest::qWait(20);
    QTextCursor at = editor.textCursor();
    at.movePosition(QTextCursor::End);
    editor.setTextCursor(at);
    return &editor;
}

// --- портретное фото: стоит, а не лежит ------------------------------------
//
// Тег Orientation=6 означает «повернуть на 90°». Файл при этом лежит
// горизонтально (3648×2736), а показаться обязан вертикально. Ошибка здесь
// самая обидная: конвейер поворачивает пиксели сам и обязан СБРОСИТЬ тег,
// иначе показ повернёт второй раз, и фото ляжет набок.
void checkPortraitStandsUp() {
    const QString source = g_corpus + QStringLiteral("/photo/orientation6.jpg");
    if (!QFile::exists(source)) return;

    Shots editor;
    openWith(editor, 1400, 900, QStringLiteral("портрет.md"));
    ZT_EQ("портретное фото вставилось", num(1), num(editor.insertImageFiles({source})));
    waitForImport(editor);
    shoot(editor, QStringLiteral("портрет-широкое-окно"));

    const Shown shown = shownImage(editor);
    ZT_TRUE("картинка на экране есть", shown.found);
    if (!shown.found) return;
    // Сам блок занимает ширину колонки; о повороте говорит соотношение сторон
    // САМОЙ картинки, а его видно по высоте блока: у стоящего фото она больше
    // ширины отведённого под картинку места.
    ZT_TRUE("фото СТОИТ, а не лежит (" + num(qRound(shown.rect.width())) + "×" +
                num(qRound(shown.rect.height())) + ")",
            shown.rect.height() > shown.rect.width());
}

// --- панорама: вписана в колонку, а не обрезана ----------------------------
void checkPanoramaFitsWide() {
    const QString source = g_corpus + QStringLiteral("/photo/panorama-4to1.jpg");
    if (!QFile::exists(source)) return;

    // ШИРОКОЕ окно — то самое, которого раньше не было в матрице.
    Shots wide;
    openWith(wide, 1900, 800, QStringLiteral("панорама-широкая.md"));
    ZT_EQ("панорама вставилась", num(1), num(wide.insertImageFiles({source})));
    waitForImport(wide);
    shoot(wide, QStringLiteral("панорама-широкое-окно"));

    const Shown w = shownImage(wide);
    ZT_TRUE("панорама показана", w.found);
    if (w.found) {
        ZT_TRUE("панорама вписана в окно (ширина " + num(qRound(w.rect.width())) + ")",
                w.rect.width() <= wide.viewport()->width());
        // 4:1 — она и должна остаться низкой; если бы её растянули по высоте,
        // это значило бы, что пропорции потеряны.
        ZT_TRUE("и осталась низкой (" + num(qRound(w.rect.width())) + "×" +
                    num(qRound(w.rect.height())) + ")",
                w.rect.width() > w.rect.height() * 2.0);
    }

    // И то же в УЗКОМ окне: картинка обязана ужаться, а не вылезти за край.
    Shots narrow;
    openWith(narrow, 620, 800, QStringLiteral("панорама-узкая.md"));
    narrow.insertImageFiles({source});
    waitForImport(narrow);
    shoot(narrow, QStringLiteral("панорама-узкое-окно"));
    const Shown n = shownImage(narrow);
    if (n.found)
        ZT_TRUE("в узком окне не вылезает за край (" + num(qRound(n.rect.width())) + " из " +
                    num(narrow.viewport()->width()) + ")",
                n.rect.width() <= narrow.viewport()->width());
}

// --- скриншот: пошёл без потерь и остался чётким ---------------------------
void checkScreenshotStaysSharp() {
    const QString small = g_corpus + QStringLiteral("/screen/screenshot-alpha.png");
    const QString big = g_corpus + QStringLiteral("/screen/screenshot-4k.png");
    if (!QFile::exists(small) || !QFile::exists(big)) return;

    // Лимиты берём УМОЛЧАНИЯ, а не конфиг машины: набор обязан давать один и
    // тот же ответ у всех, а конфиг у каждого свой.
    const zametti::ImportLimits limits;

    // Скриншот ЦЕЛОГО экрана идёт путём фото, и это осознанно. Замер: lossless
    // для него стоит 170 КБ против 212 у lossy на полном размере — казалось бы,
    // дешевле, но картинка всё равно уменьшается до бюджета, и после уменьшения
    // расклад другой. Рассуждение владельца: скриншот целого экрана в заметку
    // попадает редко, буквы плывут прежде всего от самого уменьшения, а
    // художественной ценности в нём нет. Читаются буквы — и довольно; за этим
    // и смотрим на снимке.
    const zametti::StoredImage stored = zametti::storeImageFile(small, g_store, limits);
    ZT_TRUE("скриншот принят: " + stored.error.toStdString(), stored.ok());

    const zametti::StoredImage huge = zametti::storeImageFile(big, g_store, limits);
    ZT_TRUE("скриншот 4k принят", huge.ok());
    if (huge.ok())
        ZT_TRUE("и заметно ужался (" + huge.message.toStdString() + ")",
                huge.width > 0 && huge.width < 3840);

    Shots editor;
    openWith(editor, 1500, 950, QStringLiteral("скриншот.md"));
    editor.insertImageFiles({small});
    waitForImport(editor);
    shoot(editor, QStringLiteral("скриншот-широкое-окно"));

    Shots big4k;
    openWith(big4k, 1500, 950, QStringLiteral("скриншот-4k.md"));
    big4k.insertImageFiles({big});
    waitForImport(big4k);
    shoot(big4k, QStringLiteral("скриншот-4k-широкое-окно"));
}

// --- пять подряд: одна под другой, один Ctrl+Z убирает всё -----------------
void checkFiveInARow() {
    const QStringList sources = {
        g_corpus + QStringLiteral("/photo/wallpaper-4mp.jpg"),
        g_corpus + QStringLiteral("/photo/portrait-phone.jpg"),
        g_corpus + QStringLiteral("/photo/orientation6.jpg"),
        g_corpus + QStringLiteral("/vivo/vivo-display-p3.jpg"),
        g_corpus + QStringLiteral("/photo/wallpaper-4mp.jpg"),
    };
    for (const QString& s : sources)
        if (!QFile::exists(s)) return;

    Shots editor;
    openWith(editor, 1400, 1000, QStringLiteral("пятёрка.md"));
    ZT_EQ("вставились все пять", num(5), num(editor.insertImageFiles(sources)));
    waitForImport(editor);
    shoot(editor, QStringLiteral("пять-подряд"));

    // ОДНА ПОД ДРУГОЙ: у каждой следующей верх ниже, чем у предыдущей. Это и
    // значит «легли одна под другой», а не «наложились».
    double previousTop = -1.0;
    int seen = 0;
    bool ordered = true;
    for (int i = 0; i < 5; ++i) {
        const Shown s = shownImage(editor, i);
        if (!s.found) break;
        ++seen;
        if (s.rect.top() <= previousTop) ordered = false;
        previousTop = s.rect.top();
    }
    ZT_EQ("на экране все пять", num(5), num(seen));
    ZT_TRUE("лежат одна под другой", ordered);

    editor.undo();
    QTest::qWait(30);
    int left = 0;
    for (QTextBlock b = editor.document()->firstBlock(); b.isValid(); b = b.next())
        if (zametti::blockImageRef(b).valid) ++left;
    ZT_EQ("один Ctrl+Z убрал все пять", num(0), num(left));
    shoot(editor, QStringLiteral("пять-после-отмены"));
}

// МНОГО КАРТИНОК ПОДРЯД: не наезжают ли они друг на друга.
//
// Ошибка, ради которой эта проверка написана, ловится только на длинной
// заметке и только при ПОВТОРНОМ открытии. Qt размечает документ лениво: у
// блоков за пределами показанной области lineCount() равен нулю, и высоты у
// них нет — расстояние до следующего даёт один лишь bottomMargin. Резерв же
// считался так, будто строка есть, и её высота вычиталась дважды: фотографии
// наезжали ровно на высоту строки.
//
// Сразу после вставки всё сходилось (документ уже размечен), и потому ошибка
// пряталась до следующего запуска программы.
void checkManyImagesDoNotOverlap(const QString& root) {
    const QStringList sources = {
        root + QStringLiteral("/photo/wallpaper-4mp.jpg"),
        root + QStringLiteral("/photo/portrait-phone.jpg"),
        root + QStringLiteral("/photo/orientation6.jpg"),
        root + QStringLiteral("/vivo/vivo-display-p3.jpg"),
    };
    for (const QString& s : sources)
        if (!QFile::exists(s)) return;

    // Окно НАМЕРЕННО НИЗКОЕ: за его краем и начинается неразмеченная часть,
    // где ошибка и жила. В высоком окне всё поместилось бы и проверка ничего
    // бы не поймала.
    Shots editor;
    openWith(editor, 900, 400, QStringLiteral("много-картинок.md"));
    QStringList many;
    // ШТУК ПОБОЛЬШЕ, и это не запас, а условие проверки: Qt размечает лениво,
    // и на коротком документе неразмеченной части просто не остаётся. Первая
    // редакция брала двенадцать картинок — и со снятой починкой оставалась
    // зелёной, то есть не проверяла ничего.
    for (int i = 0; i < 10; ++i) many += sources;   // сорок штук
    editor.insertImageFiles(many);
    waitForImport(editor);
    QTest::qWait(50);

    // Перечитываем файл: именно так ошибка и всплывала — при открытии, а не
    // при вставке.
    const QString path = QDir(g_store).filePath(QStringLiteral("много-картинок.md"));
    editor.save(false);
    Shots reopened;
    reopened.resize(900, 400);
    reopened.show();
    QTest::qWait(20);
    reopened.openFile(path);
    QTest::qWait(200);

    int seen = 0;
    int overlaps = 0;
    double previousBottom = -1e9;
    for (QTextBlock b = reopened.document()->firstBlock(); b.isValid(); b = b.next()) {
        if (!zametti::blockImageRef(b).valid) continue;
        ++seen;
        const QRectF r = reopened.photoRect(b);
        if (r.height() <= 0) continue;
        if (r.top() < previousBottom - 1.0) ++overlaps;
        previousBottom = r.bottom();
    }
    ZT_TRUE("картинки на месте после перечитывания (" + num(seen) + ")", seen >= 12);
    ZT_EQ("ни одна не наезжает на соседнюю", num(0), num(overlaps));
    shoot(reopened, QStringLiteral("много-картинок-после-перечитывания"));
}

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    if (argc < 3) {
        std::printf("image_shots_test <корпус> <куда класть снимки>\n");
        return 2;
    }
    g_corpus = QString::fromLocal8Bit(argv[1]);
    g_shots = QString::fromLocal8Bit(argv[2]);
    QDir().mkpath(g_shots);

    // Хранилище — отдельным каталогом рядом со снимками: вложения приёмки
    // должны быть под рукой, но в корпус их писать нельзя, он только на чтение.
    g_store = QDir(g_shots).filePath(QStringLiteral("хранилище"));
    QDir(g_store).removeRecursively();
    QDir().mkpath(g_store);

    if (!QDir(g_corpus).exists()) {
        std::printf("корпуса нет (%s) — снимки не делались\n", qPrintable(g_corpus));
        return 0;
    }

    checkPortraitStandsUp();
    checkPanoramaFitsWide();
    checkScreenshotStaysSharp();
    checkFiveInARow();
    checkManyImagesDoNotOverlap(g_corpus);

    std::printf("снимки сложены в %s\n", qPrintable(g_shots));
    return zt::report("снимки этапа 8");
}
