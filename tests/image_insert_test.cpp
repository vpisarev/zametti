// Вставка картинок в заметку: что попадает в хранилище и что — в текст.
//
// Проверяется не «функция вернула true», а сами инварианты брифа:
//
//   C. вставка N картинок — ОДИН шаг отмены, и вложения при отмене остаются;
//      плюс требование владельца: между картинками пустая строка, иначе текст
//      между ними не напечатать, не рискуя стереть саму картинку;
//   имена вложений бессмысленны и НИКОГДА не совпадают с именем исходника:
//      иначе два IMG_0001.jpg из разных мест затёрли бы друг друга;
//   при отказе в хранилище не остаётся мусора.
//
// Живое окно нужно: вставка идёт через тот же путь, что и Ctrl+V, а он живёт в
// виджете.

#include "doc_model.h"
#include "editor_widget.h"
#include "image_insert.h"
#include "note_id.h"
#include "settings.h"
#include "test_util.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QMimeData>
#include <QTemporaryDir>
#include <QTest>
#include <QTextBlock>
#include <QTextDocument>

#include <string>

namespace {

QString g_dir;

QString writeNote(const QString& name, const QString& text) {
    const QString path = QDir(g_dir).filePath(name);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return {};
    f.write(text.toUtf8());
    f.close();
    return path;
}

// Картинка с настоящим содержимым: на однотонной заплате конвейер вправе
// пойти по пути lossless, и проверялся бы не тот путь, который нужен.
QImage photo(int w, int h) {
    QImage img(w, h, QImage::Format_RGB888);
    unsigned seed = 12345;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            seed = seed * 1664525u + 1013904223u;
            uchar* p = img.scanLine(y) + size_t(x) * 3;
            // Плавный переход плюс немного шума: и сжимается как фотография, и
            // отличается от соседней картинки.
            p[0] = uchar((255 * x / w + (seed >> 28)) & 0xff);
            p[1] = uchar((255 * y / h + (seed >> 24)) & 0xff);
            p[2] = uchar(128 + (seed >> 26));
        }
    return img;
}

QString writeSource(const QString& name, int w, int h) {
    const QString path = QDir(g_dir).filePath(name);
    if (!photo(w, h).save(path)) return {};
    return path;
}

// Считаем ТОЛЬКО вложения — файлы, чьё имя есть id. По расширению считать
// нельзя: в том же временном каталоге лежат и сами исходники, и первая
// редакция этой функции их учитывала, отчего проверка «мусор не принят»
// краснела на ровном месте. Признак вложения — именно бессмысленное имя.
int countAttachments() {
    int n = 0;
    for (const QString& name : QDir(g_dir).entryList(QDir::Files)) {
        const QString base = name.section(QLatin1Char('.'), 0, 0);
        if (zametti::isValidNoteId(base.toStdString())) ++n;
    }
    return n;
}

std::string num(long long v) { return std::to_string(v); }

int imageCount(const zametti::NoteEditor& editor) {
    int n = 0;
    for (QTextBlock b = editor.document()->firstBlock(); b.isValid(); b = b.next())
        if (zametti::blockImageRef(b).valid) ++n;
    return n;
}

// Приём mime-данных у QTextEdit защищённый — и правильно: звать его снаружи
// незачем никому, кроме Qt. Тесту нужен ровно его ответ, поэтому подкласс.
class Probe : public zametti::NoteEditor {
public:
    bool accepts(const QMimeData* data) const { return canInsertFromMimeData(data); }
};

// --- сама вставка ----------------------------------------------------------

void checkStoresUnderFreshName() {
    const QString source = writeSource("исходник.png", 640, 480);
    zametti::ImportLimits limits;
    const zametti::StoredImage stored = zametti::storeImageFile(source, g_dir, limits);

    ZT_TRUE("картинка положена: " + stored.error.toStdString(), stored.ok());
    if (!stored.ok()) return;

    // ИМЯ БЕССМЫСЛЕННО. Это не придирка: совпади оно с исходником, вторая
    // картинка с тем же именем затёрла бы первую.
    const QString base = stored.fileName.section(QLatin1Char('.'), 0, 0);
    ZT_TRUE("имя не от исходника", !stored.fileName.contains(QStringLiteral("исходник")));
    ZT_TRUE("имя — правильный id (" + base.toStdString() + ")",
            zametti::isValidNoteId(base.toStdString()));
    ZT_TRUE("файл на месте", QFile::exists(QDir(g_dir).filePath(stored.fileName)));

    // А человеческое имя обязано уцелеть в alt — иначе оно потеряно навсегда.
    ZT_EQ("alt — имя исходника", std::string("исходник"), stored.alt.toStdString());

    // Вторая такая же ложится РЯДОМ, а не поверх.
    const zametti::StoredImage again = zametti::storeImageFile(source, g_dir, limits);
    ZT_TRUE("вторая тоже легла", again.ok());
    ZT_TRUE("и под другим именем", again.fileName != stored.fileName);
}

void checkRefusalLeavesNoTrash() {
    const int before = countAttachments();

    // Файл, который картинкой не является вовсе.
    const QString junk = writeNote("не-картинка.png", QStringLiteral("это просто текст"));
    zametti::ImportLimits limits;
    const zametti::StoredImage bad = zametti::storeImageFile(junk, g_dir, limits);
    ZT_TRUE("мусор не принят", !bad.ok());
    ZT_TRUE("и причина названа", !bad.error.isEmpty());

    // Главное: в хранилище не появилось ничего. Пустой файл под свежим id был
    // бы хуже отказа — он бы показался сломанной картинкой.
    ZT_EQ("вложений не прибавилось", num(before), num(countAttachments()));
}

void checkMarkdownEscapesAlt() {
    zametti::StoredImage s;
    s.fileName = QStringLiteral("01abcdefghijkl.jxl");
    s.alt = QStringLiteral("фото [1] в скобках");
    // Квадратные скобки в alt закрыли бы ссылку раньше времени, и ссылка
    // развалилась бы на текст.
    const QString md = zametti::imageMarkdown(s);
    ZT_EQ("скобки в alt обезврежены",
          std::string("![фото (1) в скобках](01abcdefghijkl.jxl)"), md.toStdString());
}

// --- инвариант C: один шаг отмены, вложения остаются -----------------------

void checkMultiInsertIsOneUndo() {
    const QString a = writeSource("первая.png", 320, 240);
    const QString b = writeSource("вторая.png", 400, 300);
    const QString c = writeSource("третья.png", 360, 270);
    const QString note = writeNote("заметка.md", QStringLiteral("Начало\n"));

    zametti::NoteEditor editor;
    editor.resize(900, 700);
    editor.show();
    QTest::qWait(20);
    editor.openFile(note);
    QTest::qWait(20);

    // Курсор в конец, чтобы вставлять после текста.
    QTextCursor at = editor.textCursor();
    at.movePosition(QTextCursor::End);
    editor.setTextCursor(at);

    const int attachmentsBefore = countAttachments();
    const int inserted = editor.insertImageFiles({a, b, c});
    QTest::qWait(20);
    ZT_EQ("вставились все три", num(3), num(inserted));
    ZT_EQ("и в хранилище прибавилось ровно три",
          num(attachmentsBefore + 3), num(countAttachments()));

    const QString withImages = editor.document()->toPlainText();
    ZT_TRUE("текст начала на месте", withImages.contains(QStringLiteral("Начало")));

    // ПУСТАЯ СТРОКА МЕЖДУ КАРТИНКАМИ (требование владельца). Смотрим не на
    // markdown, а на сам документ: между блоками с картинками обязан стоять
    // пустой блок, иначе встать между ними и написать текст можно только
    // вплотную к картинке, а это способ её случайно стереть.
    int blanksBetween = 0;
    bool prevWasImage = false;
    for (QTextBlock blk = editor.document()->firstBlock(); blk.isValid(); blk = blk.next()) {
        if (zametti::blockImageRef(blk).valid) {
            prevWasImage = true;
            continue;
        }
        if (blk.text().isEmpty() && prevWasImage) ++blanksBetween;
        prevWasImage = false;
    }
    ZT_EQ("в документе три картинки", num(3), num(imageCount(editor)));
    ZT_TRUE("после каждой картинки пустая строка (" + num(blanksBetween) + ")",
            blanksBetween >= 2);

    // ОДИН ШАГ ОТМЕНЫ на всю пачку.
    //
    // Считаем БЛОКИ С КАРТИНКАМИ, а не ищем ".jxl" в тексте: первая редакция
    // этой проверки смотрела на toPlainText, а там имён файлов нет вовсе —
    // картинка отрисована, а не написана. Проверка была зелёной всегда, в том
    // числе когда вставка шла по одной и отменялась по одной.
    editor.undo();
    QTest::qWait(20);
    ZT_EQ("один Ctrl+Z убрал все три", num(0), num(imageCount(editor)));
    ZT_TRUE("а текст начала остался",
            editor.document()->toPlainText().contains(QStringLiteral("Начало")));

    // А ВЛОЖЕНИЯ ОСТАЛИСЬ. Отменённая вставка часто повторяется, а
    // восстанавливать удалённый файл неоткуда.
    ZT_EQ("файлы вложений отмена не трогает",
          num(attachmentsBefore + 3), num(countAttachments()));
}

// Инвариант B в этом месте: вставка НЕ увеличивает картинку. Мелкая должна
// остаться собой — апскейл был бы выдумыванием пикселей.
void checkNoUpscale() {
    const QString small = writeSource("мелкая.png", 200, 150);
    const zametti::StoredImage stored =
        zametti::storeImageFile(small, g_dir, zametti::ImportLimits{});
    ZT_TRUE("мелкая принята", stored.ok());
    if (!stored.ok()) return;
    ZT_EQ("ширина не выросла", num(200), num(stored.width));
    ZT_EQ("высота не выросла", num(150), num(stored.height));
}

// Битмап из буфера обмена: имени у него нет, и выдумывать его нельзя.
void checkPixelsFromClipboard() {
    const zametti::StoredImage stored =
        zametti::storeImagePixels(photo(500, 400), g_dir, zametti::ImportLimits{});
    ZT_TRUE("битмап принят: " + stored.error.toStdString(), stored.ok());
    if (!stored.ok()) return;
    ZT_TRUE("alt у битмапа пуст", stored.alt.isEmpty());
    ZT_EQ("и markdown без подписи",
          "![](" + stored.fileName.toStdString() + ")",
          zametti::imageMarkdown(stored).toStdString());
}

// Перетаскивание и Ctrl+V идут через один и тот же приём mime-данных. Если он
// отвечает «не приму», Qt даже не позовёт вставку, и перетаскивание молча не
// сработает — поэтому проверяется именно ответ.
void checkMimeAcceptance() {
    const QString note = writeNote("для-mime.md", QStringLiteral("текст\n"));
    Probe editor;
    editor.resize(700, 500);
    editor.show();
    QTest::qWait(20);
    editor.openFile(note);
    QTest::qWait(20);

    QMimeData withImage;
    withImage.setImageData(photo(64, 48));
    ZT_TRUE("картинку в буфере принимаем", editor.accepts(&withImage));

    QMimeData withFile;
    withFile.setUrls({QUrl::fromLocalFile(writeSource("для-drop.png", 100, 80))});
    ZT_TRUE("файл перетаскиванием принимаем", editor.accepts(&withFile));

    QMimeData withText;
    withText.setText(QStringLiteral("обычный текст"));
    ZT_TRUE("текст по-прежнему принимаем", editor.accepts(&withText));

    QMimeData empty;
    ZT_TRUE("пустое не принимаем", !editor.accepts(&empty));
}

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QTemporaryDir tmp;
    if (!tmp.isValid()) {
        std::printf("не завёлся временный каталог\n");
        return 2;
    }
    g_dir = tmp.path();

    checkStoresUnderFreshName();
    checkRefusalLeavesNoTrash();
    checkMarkdownEscapesAlt();
    checkNoUpscale();
    checkPixelsFromClipboard();
    checkMimeAcceptance();
    checkMultiInsertIsOneUndo();

    return zt::report("вставка картинок");
}
