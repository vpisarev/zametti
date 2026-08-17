// Полоса сведений: что она показывает и откуда берёт.
//
// Проверяется ТЕКСТ, а не расположение. Расположение — дело снимков под Xvfb,
// а текст — это правила: что показывается вместо неизвестного числа слов, что
// показывается, когда каретка стоит на картинке, и что — когда вложение
// потерялось.

#include "doc_model.h"
#include "editor_widget.h"
#include "image_facts.h"
#include "status_bar.h"
#include "test_util.h"

#include <vector>
#include "testdata.h"

#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QScrollBar>
#include <QTest>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QLabel>
#include <QLayout>
#include <QTextBlock>
#include <QTextCursor>

#include <string>

namespace {

QString g_dir;

void check(bool ok, const std::string& what) {
    ++zt::g_checks;
    if (ok) return;
    ++zt::g_failures;
    std::printf("провал: %s\n", what.c_str());
}

void checkHas(const QString& text, const QString& part, const std::string& what) {
    ++zt::g_checks;
    if (text.contains(part)) return;
    ++zt::g_failures;
    std::printf("провал: %s\n  ждали внутри: %s\n  вышло:        %s\n", what.c_str(),
                part.toUtf8().constData(), text.toUtf8().constData());
}

// Полоса устроена двумя надписями; берём их по порядку, как они и лежат.
QString leftText(const zametti::StatusBar& bar) {
    const QList<QLabel*> labels = bar.findChildren<QLabel*>();
    return labels.isEmpty() ? QString() : labels.at(0)->text();
}
QString rightText(const zametti::StatusBar& bar) {
    const QList<QLabel*> labels = bar.findChildren<QLabel*>();
    return labels.size() < 2 ? QString() : labels.at(1)->text();
}

QString writeFile(const QString& name, const QString& text) {
    const QString path = g_dir + QLatin1Char('/') + name;
    QFile file(path);
    if (file.open(QIODevice::WriteOnly)) file.write(text.toUtf8());
    return path;
}

void checkHumanNumbers() {
    check(zametti::humanBytes(0).startsWith(QStringLiteral("0")), "ноль байт");
    check(zametti::humanBytes(847).contains(QStringLiteral("847")), "меньше килобайта — байты");
    check(zametti::humanBytes(2048).contains(QStringLiteral("2.0")), "два килобайта");
    check(zametti::humanBytes(5 * 1024 * 1024).contains(QStringLiteral("5.0")), "мегабайты");
    // Разряды разделяются НЕРАЗРЫВНЫМ пробелом: обычный порвал бы число
    // переносом строки пополам.
    const QString big = zametti::humanCount(36828);
    check(big.contains(QChar(0x00A0)), "разряды разделены неразрывным пробелом");
    check(!big.contains(QLatin1Char(' ')), "обычного пробела в числе нет");
    check(zametti::humanCount(999) == QStringLiteral("999"), "три знака не делятся");
}

void checkNoteLine() {
    zametti::StatusBar bar;
    bar.resize(1200, 24);
    bar.show();
    if (bar.layout() != nullptr) bar.layout()->activate();

    zametti::StatusBar::NoteInfo note;
    note.valid = true;
    note.path = QStringLiteral("/tmp/хранилище/01n6r08s8wy52h.md");
    note.bytes = 3891;
    note.created = QDateTime(QDate(2019, 3, 14), QTime(9, 26));
    note.modified = QDateTime(QDate(2026, 8, 9), QTime(12, 39));
    note.words = 36828;
    note.lines = 4278;
    note.wordsKnown = true;
    bar.setNote(note);
    bar.setCaret(12, 5);

    checkHas(leftText(bar), QStringLiteral("01n6r08s8wy52h.md"), "имя файла в панели");
    check(!leftText(bar).contains(QStringLiteral("/tmp/")),
          "пути в панели нет: хранилище плоское");
    checkHas(leftText(bar), QStringLiteral("14.03.2019"), "дата создания");
    checkHas(rightText(bar), QStringLiteral("36"), "число слов");
    checkHas(rightText(bar), QStringLiteral("4"), "число строк");
    checkHas(rightText(bar), QStringLiteral("12"), "строка каретки");

    // Красная звёздочка у имени: самопроверка при записи не сошлась. Окна с
    // вопросом больше нет — оно повторялось на каждом автосохранении и потому
    // выключалось вместе с самим сигналом.
    check(!leftText(bar).contains(QStringLiteral("*")), "без беды звёздочки нет");
    note.suspect = true;
    bar.setNote(note);
    checkHas(leftText(bar), QStringLiteral("*"), "звёздочка появилась");
    checkHas(leftText(bar), QStringLiteral("color:"), "и она покрашена, а не просто знак");
    checkHas(leftText(bar), QStringLiteral("01n6r08s8wy52h.md"), "имя заметки на месте");
    note.suspect = false;
    bar.setNote(note);
    check(!leftText(bar).contains(QStringLiteral("*")), "и гаснет, когда запись сошлась");

    // Неизвестное число слов — «?», а не старое число молча.
    note.wordsKnown = false;
    bar.setNote(note);
    checkHas(rightText(bar), QStringLiteral("?"), "неизвестное число слов — вопрос");
    check(!rightText(bar).contains(QStringLiteral("36")),
          "устаревшее число не показывается вовсе");
}

void checkImageLine() {
    zametti::StatusBar bar;
    bar.resize(1200, 24);
    bar.show();
    if (bar.layout() != nullptr) bar.layout()->activate();

    zametti::StatusBar::NoteInfo note;
    note.valid = true;
    note.path = QStringLiteral("/tmp/хранилище/01n6r08s8wy52h.md");
    note.words = 100;
    note.wordsKnown = true;
    bar.setNote(note);

    zametti::StatusBar::ImageInfo image;
    image.valid = true;
    image.exists = true;
    image.name = QStringLiteral("01n6cqev0s0h1c.webp");
    image.caption = QStringLiteral("Drawing");
    image.format = QStringLiteral("webp");
    image.size = QSize(1920, 1080);
    image.bytes = 240000;
    bar.setImage(image);

    checkHas(leftText(bar), QStringLiteral("1920×1080"), "разрешение картинки");
    // Формата в строке нет намеренно: расширение в имени файла говорит о нём
    // однозначно, и «01n6….webp · WEBP» — одно слово дважды.
    check(!leftText(bar).contains(QStringLiteral("WEBP")),
          "формат отдельным словом не пишется: он в имени файла");
    checkHas(leftText(bar), QStringLiteral(".webp"), "расширение видно в имени");
    // ПОДПИСЬ В ПАНЕЛИ НЕ ДУБЛИРУЕТСЯ: она видна под самим снимком. Панель
    // берёт её на себя только тогда, когда подпись под снимком выключена, —
    // иначе человеку негде было бы её увидеть вовсе.
    check(!leftText(bar).contains(QStringLiteral("Drawing")),
          "подпись не повторяется: она под снимком");
    {
        const bool saved = zametti::settings().look.imageCaption;
        zametti::editSettings().look.imageCaption = false;
        // Панель не перерисовывает то, что не менялось; здесь поменялась
        // настройка, а не сведения, — сбрасываем её показом другого.
        bar.setImage(zametti::StatusBar::ImageInfo());
        bar.setImage(image);
        checkHas(leftText(bar), QStringLiteral("Drawing"),
                 "с выключенной подписью под снимком её берёт панель");
        zametti::editSettings().look.imageCaption = saved;
        bar.setImage(zametti::StatusBar::ImageInfo());
        bar.setImage(image);
    }
    check(!leftText(bar).contains(QStringLiteral(".md")),
          "пока каретка на картинке, заметка уступает ей место");
    // Правая половина остаётся про заметку: число слов от картинки не зависит.
    checkHas(rightText(bar), QStringLiteral("100"), "слова заметки на месте");

    // КОГДА СНЯТО. Пишется тем же словом и в том же виде, что дата заметки —
    // «создана DD.MM.YYYY HH:MM», — а правка снимка не пишется вовсе: у
    // вложения это время файла, к содержимому снимка отношения не имеющее.
    check(!leftText(bar).contains(QStringLiteral("создана")),
          "без метаданных дата не выдумывается");
    image.taken = QDateTime(QDate(2019, 3, 14), QTime(9, 26));
    bar.setImage(image);
    checkHas(leftText(bar), QStringLiteral("создана 14.03.2019 09:26"), "дата съёмки в панели");
    check(!leftText(bar).contains(QStringLiteral("правлена")),
          "правку снимка не пишем");
    image.taken = QDateTime();
    bar.setImage(image);

    // Вложение потерялось — молчать нельзя.
    image.exists = false;
    bar.setImage(image);
    checkHas(leftText(bar), QStringLiteral("вложения нет"), "пропавшее вложение названо");

    zametti::StatusBar::ImageInfo none;
    bar.setImage(none);
    checkHas(leftText(bar), QStringLiteral("01n6r08s8wy52h.md"),
             "каретка ушла с картинки — вернулась заметка");
}

// Каретка на картинке: сведения берутся из заголовка файла и из подписи в
// заметке. Здесь же проверяется, что чтение не повторяется.
void checkCaretImage() {
    QImage picture(320, 200, QImage::Format_RGB32);
    picture.fill(Qt::darkCyan);
    const QString imagePath = g_dir + QStringLiteral("/01n6cqev0s0h1c.png");
    check(picture.save(imagePath), "тестовая картинка записана");

    const QString notePath =
        writeFile(QStringLiteral("с-картинкой.md"),
                  QStringLiteral("Текст до\n\n![Снимок с телефона](01n6cqev0s0h1c.png)\n\n"
                                 "Текст после\n"));

    zametti::NoteEditor editor;
    editor.resize(700, 500);
    editor.show();
    editor.openFile(notePath);

    // Каретка в самом начале — картинки под ней нет.
    QTextCursor caret = editor.textCursor();
    caret.setPosition(0);
    editor.setTextCursor(caret);
    check(!editor.caretImage().valid, "в начале заметки каретка не на картинке");

    // Ставим её в блок картинки.
    bool landed = false;
    for (QTextBlock block = editor.document()->begin(); block.isValid(); block = block.next()) {
        if (!zametti::blockImageRef(block).valid) continue;
        caret.setPosition(block.position());
        editor.setTextCursor(caret);
        landed = true;
        break;
    }
    check(landed, "блок с картинкой в документе есть");

    const zametti::ImageFacts facts = editor.caretImage();
    check(facts.valid, "каретка на картинке — сведения есть");
    check(facts.exists, "файл вложения найден");
    check(facts.size == QSize(320, 200), "разрешение из заголовка файла");
    check(facts.format == QStringLiteral("png"), "формат из заголовка файла");
    check(facts.bytes > 0, "размер файла известен");
    check(facts.caption == QStringLiteral("Снимок с телефона"),
          "подпись берётся из заметки, а не из файла");

    // Сведения живут в кэше картинок вида, своего кэша у них нет: второй
    // спрос обязан дать ровно то же, не перечитывая файл.
    const zametti::ImageFacts again = editor.caretImage();
    check(again.size == facts.size && again.bytes == facts.bytes &&
              again.format == facts.format,
          "повторный спрос даёт то же самое");

    // Настоящий путь человека: он не ставит каретку из кода, он ЩЁЛКАЕТ по
    // фотографии. Щелчок проходит через snapCaretOffImage и может оставить
    // каретку с выделением, чей position стоит уже в следующем блоке.
    {
        editor.resize(700, 500);
        editor.show();
        (void)QTest::qWaitForWindowExposed(&editor);
        QTest::qWait(50);

        QTextBlock photo;
        for (QTextBlock block = editor.document()->begin(); block.isValid();
             block = block.next())
            if (zametti::blockImageRef(block).valid) photo = block;
        check(photo.isValid(), "блок с фотографией найден");

        // Середина фотографии в координатах виджета.
        const QAbstractTextDocumentLayout* layout = editor.document()->documentLayout();
        const QRectF box = layout->blockBoundingRect(photo);
        const QPoint at = QPoint(int(box.center().x()),
                                 int(box.center().y()) - editor.verticalScrollBar()->value()) +
                          QPoint(editor.viewport()->x(), editor.viewport()->y());
        QTest::mouseClick(editor.viewport(), Qt::LeftButton, Qt::NoModifier, at);
        QTest::qWait(50);

        const zametti::ImageFacts clicked = editor.caretImage();
        check(clicked.valid, "щёлкнули по фотографии — сведения о ней есть");
        if (!clicked.valid)
            std::printf("  каретка: блок %d, выделение %d..%d, блок фотографии %d\n",
                        editor.textCursor().blockNumber(),
                        editor.textCursor().selectionStart(),
                        editor.textCursor().selectionEnd(), photo.blockNumber());
    }

    // Вложение пропало — сведения обязаны это сказать, а не соврать старым.
    // Редактор новый: у прежнего в кэше лежат и пиксели, и сведения об уже
    // прочитанном файле, и он законно продолжает показывать то, что показывает.
    check(QFile::remove(imagePath), "вложение удалено");
    zametti::NoteEditor fresh;
    fresh.resize(700, 500);
    fresh.show();
    fresh.openFile(notePath);
    for (QTextBlock block = fresh.document()->begin(); block.isValid(); block = block.next()) {
        if (!zametti::blockImageRef(block).valid) continue;
        QTextCursor place = fresh.textCursor();
        place.setPosition(block.position());
        fresh.setTextCursor(place);
        break;
    }
    const zametti::ImageFacts gone = fresh.caretImage();
    check(gone.valid && !gone.exists, "пропавшее вложение видно как пропавшее");
}

}  // namespace

static int ztRunSuite(int argc, char** argv) {
    if (argc < 2) {
        std::printf("использование: status_bar_test <каталог для временных файлов>\n");
        return 2;
    }
    g_dir = QString::fromLocal8Bit(argv[1]) + QStringLiteral("/status-bar-data");
    QDir(g_dir).removeRecursively();
    if (!QDir().mkpath(g_dir)) {
        std::printf("не создать каталог %s\n", g_dir.toUtf8().constData());
        return 2;
    }

    checkHumanNumbers();
    checkNoteLine();
    checkImageLine();
    checkCaretImage();

    return zt::report("status-bar");
}

// Набор целиком одним TEST: тело не тронуто, argv ему собран здесь.
// Дробить на отдельные проверки — отдельная работа, по одному набору.
TEST(StatusBar, All) {
    std::vector<QByteArray> ztArgs{QByteArrayLiteral("status_bar_test")};
    ztArgs.push_back((zt::TestData::outDir(QStringLiteral("status-bar"))).toLocal8Bit());
    std::vector<char*> ztArgv;
    for (QByteArray& a : ztArgs) ztArgv.push_back(a.data());
    EXPECT_EQ(0, ztRunSuite(int(ztArgv.size()), ztArgv.data()));
}
