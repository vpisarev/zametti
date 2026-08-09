// Полоса сведений: что она показывает и откуда берёт.
//
// Проверяется ТЕКСТ, а не расположение. Расположение — дело снимков под Xvfb,
// а текст — это правила: что показывается вместо неизвестного числа слов, что
// показывается, когда каретка стоит на картинке, и что — когда вложение
// потерялось.

#include "editor_widget.h"
#include "image_facts.h"
#include "status_bar.h"
#include "test_util.h"

#include <QApplication>
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
    checkHas(leftText(bar), QStringLiteral("WEBP"), "формат картинки");
    checkHas(leftText(bar), QStringLiteral("Drawing"), "подпись картинки");
    check(!leftText(bar).contains(QStringLiteral(".md")),
          "пока каретка на картинке, заметка уступает ей место");
    // Правая половина остаётся про заметку: число слов от картинки не зависит.
    checkHas(rightText(bar), QStringLiteral("100"), "слова заметки на месте");

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

    // Второй спрос той же картинки файл не перечитывает: кэш на месте.
    zametti::clearImageFactsCache();
    editor.caretImage();
    const int afterFirst = zametti::imageFactsCacheSize();
    editor.caretImage();
    check(afterFirst == 1 && zametti::imageFactsCacheSize() == 1,
          "картинка читается один раз, дальше из кэша");

    // Вложение пропало — сведения обязаны это сказать, а не соврать старым.
    zametti::clearImageFactsCache();
    check(QFile::remove(imagePath), "вложение удалено");
    const zametti::ImageFacts gone = editor.caretImage();
    check(gone.valid && !gone.exists, "пропавшее вложение видно как пропавшее");
}

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
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
