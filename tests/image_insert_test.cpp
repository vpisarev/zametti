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
#include "exif.h"
#include <QDateTime>
#include "test_util.h"

#include <vector>
#include "testdata.h"

#include <QApplication>
#include <QElapsedTimer>
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
// Картинка, похожая на фотографию: плавный переход плюс шум.
//
// ЗЕРНО ОБЯЗАНО ЗАВИСЕТЬ ОТ РАЗМЕРА И ОТ НОМЕРА ВЫЗОВА. Раньше оно было
// зашито (12345), и два вызова с одними размерами давали ПОБАЙТОВО ОДНУ
// картинку. Дальше срабатывала штатная дедупликация хранилища — «одинаковое
// содержимое, один файл», — и проверка «прибавилось ровно три» падала: файлов
// прибавлялось два.
//
// Падало не всегда: двойник ищется среди файлов с тем же восьмизначным
// префиксом id, а это СЕКУНДА съёмки. Успел набор уложиться в секунду —
// дедупликация сработала, перешагнул границу — нет. Отсюда и «то падает, то
// нет» на пустом месте.
//
// Виноват был набор, а не хранилище: он сам подсовывал содержимое, которое
// уже лежало, и сам же требовал новый файл.
QImage photo(int w, int h) {
    QImage img(w, h, QImage::Format_RGB888);
    static unsigned calls = 0;
    unsigned seed = 12345u + 7919u * ++calls + 104729u * unsigned(w) + 1299709u * unsigned(h);
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

// Ввоз теперь фоновый, и всякий, кто ждёт его результата, обязан ДОЖДАТЬСЯ.
// Признак конца — редактор снова правится: на время ввоза он читалка.
void waitForImport(zametti::NoteEditor& editor) {
    QElapsedTimer waiting;
    waiting.start();
    while (editor.isReadOnly() && waiting.elapsed() < 600000)
        QApplication::processEvents(QEventLoop::AllEvents, 20);
    QApplication::processEvents();
}

// Имя вложения чеканится от ДАТЫ СЪЁМКИ, а не от «сейчас»: на этом стоит вся
// дедупликация — повторный ввоз того же снимка обязан дать тот же префикс id.
void checkNameCarriesShotDate() {
    // Снимок с EXIF: берём настоящий файл из корпуса, выдуманный EXIF проверял
    // бы наш же писатель, а не чужие файлы.
    const QString source = QStringLiteral("/home/vpisarev/Pictures/Wallpapers");
    QDir dir(source);
    const QStringList shots = dir.entryList({QStringLiteral("*.jpg")}, QDir::Files);
    if (shots.isEmpty()) return;   // нет корпуса — проверять нечего

    const QString path = dir.filePath(shots.first());
    const QString store = QDir(g_dir).filePath(QStringLiteral("дата"));
    QDir().mkpath(store);

    const zametti::StoredImage first =
        zametti::storeImageFile(path, store, zametti::limitsFromSettings());
    ZT_TRUE("снимок положен: " + first.error.toStdString(), first.ok());
    if (!first.ok()) return;

    // Тот же файл второй раз: префикс id (восемь знаков — секунды) обязан
    // совпасть, а случайный хвост — разойтись.
    const zametti::StoredImage second =
        zametti::storeImageFile(path, store, zametti::limitsFromSettings());
    ZT_TRUE("снимок положен второй раз", second.ok());
    if (!second.ok()) return;

    ZT_EQ("префикс имени тот же: он от даты съёмки",
          first.fileName.left(8).toStdString(), second.fileName.left(8).toStdString());
    // Двойник: второй ввоз того же файла НЕ заводит второго вложения, а
    // ссылается на первое. Это и есть дедупликация — часть ввоза, а не
    // надстройка: снаружи возвращается имя для ссылки, как и всегда.
    ZT_EQ("второй ввоз того же снимка дал то же вложение",
          first.fileName.toStdString(), second.fileName.toStdString());
    ZT_TRUE("и сказал, что это двойник", second.duplicate);
    ZT_TRUE("а первый двойником не был", !first.duplicate);
    ZT_EQ("файл в хранилище один", num(1),
          num(QDir(store).entryList({QStringLiteral("*.*")}, QDir::Files).size()));

    // И главное: префикс должен отвечать именно ДАТЕ СЪЁМКИ. Без этой сверки
    // проверка выше проходила бы и с датой создания файла, и даже с «сейчас» —
    // два ввоза подряд всё равно попали бы в одну секунду.
    QFile raw(path);
    if (!raw.open(QIODevice::ReadOnly)) return;
    const QByteArray head = raw.read(2 * 1024 * 1024);
    const zametti::ImageMeta meta =
        zametti::readImageMeta(std::string_view(head.constData(), size_t(head.size())));
    const QString when = QString::fromStdString(zametti::exifDateTaken(meta.exif));
    if (when.isEmpty()) return;   // у этого файла даты нет — сверять нечего

    const QDateTime taken = QDateTime::fromString(when, Qt::ISODate);
    const QString expected = QString::fromStdString(
        zametti::makeNoteId(std::uint64_t(taken.toSecsSinceEpoch()), 0));
    ZT_EQ("префикс id — это дата съёмки (" + when.toStdString() + ")",
          expected.left(8).toStdString(), first.fileName.left(8).toStdString());
}

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

    // Вторая такая же НЕ ложится второй копией: с появлением дедупликации тот
    // же самый файл возвращает имя уже лежащего вложения. Прежде здесь
    // проверялось обратное («и под другим именем») — и это была верная
    // проверка ровно до того дня, пока двойники не начали ловиться.
    const zametti::StoredImage again = zametti::storeImageFile(source, g_dir, limits);
    ZT_TRUE("вторая тоже дала имя для ссылки", again.ok());
    ZT_EQ("и это имя того же вложения", stored.fileName.toStdString(),
          again.fileName.toStdString());
    ZT_TRUE("отмечено как двойник", again.duplicate);
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
    waitForImport(editor);
    QTest::qWait(20);
    ZT_EQ("вставились все три", num(3), num(inserted));
    if (countAttachments() != attachmentsBefore + 3) {
        // НЕ СОШЛОСЬ — ПОКАЗАТЬ, ЧТО ИМЕННО ЛЕЖИТ. Каталог у набора временный
        // и убирается сам, так что после падения смотреть уже негде: печатаем
        // здесь. Одна картинка из трёх иногда не долетает, и без этого списка
        // причину не поймать.
        std::printf("НЕ СОШЛОСЬ. В каталоге %s:\n", qPrintable(g_dir));
        for (const QString& name : QDir(g_dir).entryList(QDir::Files, QDir::Name)) {
            const QFileInfo fi(QDir(g_dir).filePath(name));
            std::printf("  %-28s %8lld байт  создан %s\n", qPrintable(name),
                        (long long)fi.size(),
                        qPrintable(fi.birthTime().toString(QStringLiteral("HH:mm:ss.zzz"))));
        }
    }
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
// ВСТАВЛЕННАЯ КАРТИНКА ОБЯЗАНА ДОЖИТЬ ДО ФАЙЛА.
//
// Дыра была не в коде, а в матрице: все прежние проверки смотрели на документ
// в памяти — «три картинки, между ними пустые строки», — и ни одна не спросила,
// что окажется в .md после сохранения. Владелец наткнулся на это с другой
// стороны: вставил картинку, а после перечитывания заметки её не было.
void checkInsertSurvivesSave() {
    const QString source = writeSource("сохраняемая.png", 320, 240);
    const QString note = writeNote("с-картинкой.md", QStringLiteral("Начало\n"));

    zametti::NoteEditor editor;
    editor.resize(900, 700);
    editor.show();
    QTest::qWait(20);
    editor.openFile(note);
    QTest::qWait(20);

    QTextCursor at = editor.textCursor();
    at.movePosition(QTextCursor::End);
    editor.setTextCursor(at);

    ZT_EQ("картинка вставилась", num(1), num(editor.insertImageFiles({source})));
    waitForImport(editor);
    QTest::qWait(20);
    editor.save(true);
    QTest::qWait(20);

    QFile f(note);
    ZT_TRUE("заметка читается", f.open(QIODevice::ReadOnly));
    const QString saved = QString::fromUtf8(f.readAll());
    ZT_TRUE("текст на месте", saved.contains(QStringLiteral("Начало")));
    ZT_TRUE("ссылка на вложение дожила до файла:\n" + saved.toStdString(),
            saved.contains(QStringLiteral(".jxl")));

    // И обратно: перечитанная с диска заметка снова показывает картинку.
    zametti::NoteEditor again;
    again.resize(900, 700);
    again.show();
    QTest::qWait(20);
    again.openFile(note);
    QTest::qWait(20);
    ZT_EQ("после перечитывания картинка на месте", num(1), num(imageCount(again)));
}

// То же самое, но в НАСТОЯЩЕМ ХРАНИЛИЩЕ. Путь сохранения там другой: шапка с
// метаданными, журнал правок и правило «не писать, если не изменилось». Именно
// на нём владелец и потерял вставленную картинку, а проверка выше — нет,
// потому что работала с голым файлом вне хранилища.
void checkInsertSurvivesSaveInStore() {
    const QString root = QDir(g_dir).filePath(QStringLiteral("хранилище"));
    QDir().mkpath(root + QStringLiteral("/.zametti"));
    const QString note = QDir(root).filePath(QStringLiteral("00000000000001.md"));
    {
        QFile f(note);
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
            f.write(QStringLiteral("<!-- zametti\ncreated: 2026-08-01T10:00:00Z\n"
                                   "modified: 2026-08-08T10:00:00Z\n-->\n\n# Вставка\n\n"
                                   "Текст до.\n")
                        .toUtf8());
    }
    const QString source = writeSource("в-хранилище.png", 320, 240);

    zametti::NoteEditor editor;
    editor.setStoreRoot(root);
    editor.resize(900, 700);
    editor.show();
    QTest::qWait(20);
    editor.openFile(note);
    QTest::qWait(20);

    QTextCursor at = editor.textCursor();
    at.movePosition(QTextCursor::End);
    editor.setTextCursor(at);

    ZT_EQ("картинка вставилась в заметку хранилища", num(1),
          num(editor.insertImageFiles({source})));
    waitForImport(editor);
    QTest::qWait(20);
    editor.save(true);
    QTest::qWait(50);

    QFile f(note);
    ZT_TRUE("заметка хранилища читается", f.open(QIODevice::ReadOnly));
    const QString saved = QString::fromUtf8(f.readAll());
    ZT_TRUE("шапка на месте", saved.contains(QStringLiteral("<!-- zametti")));
    ZT_TRUE("ссылка на вложение дожила до файла хранилища:\n" + saved.toStdString(),
            saved.contains(QStringLiteral(".jxl")));
}

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

// Ввоз идёт В ФОНЕ: окно не замирает, порядок вставки — порядок выбора файлов,
// и вся пачка ложится ОДНИМ шагом истории.
void checkBackgroundImport() {
    const QString dir = g_dir + QStringLiteral("/фон");
    QDir().mkpath(dir);
    QStringList sources;
    for (int i = 0; i < 3; ++i) {
        QImage picture(120 + i * 10, 80, QImage::Format_RGB32);
        picture.fill(QColor(40 * i, 90, 200));
        const QString path = dir + QStringLiteral("/кадр-%1.png").arg(i);
        picture.save(path);
        sources << path;
    }

    const QString notePath = dir + QStringLiteral("/01n6cqev00imp1.md");
    QFile note(notePath);
    if (note.open(QIODevice::WriteOnly)) note.write("# Пачка\n\nТекст.\n");
    note.close();

    zametti::NoteEditor editor;
    editor.resize(600, 400);
    editor.show();
    editor.openFile(notePath);
    const int blocksBefore = editor.document()->blockCount();

    const int queued = editor.insertImageFiles(sources);
    // Ждать здесь НЕЛЬЗЯ: следующие три утверждения — про то, что управление
    // вернулось до конца ввоза. Скрипт, расставлявший ожидания по всему файлу,
    // дописал его и сюда, и проверка стала проверять обратное самой себе.
    ZT_TRUE("в работу принята вся пачка", queued == 3);
    // Управление вернулось СРАЗУ: до окончания ввоза в документе ничего нет.
    ZT_TRUE("вставки ещё нет: ввоз идёт в фоне",
            editor.document()->blockCount() == blocksBefore);
    ZT_TRUE("пока везём, править нельзя", editor.isReadOnly());

    // Цикл событий крутится — ровно то, чего не было раньше и из-за чего
    // система вешала «программа не отвечает».
    int spins = 0;
    QElapsedTimer waiting;
    waiting.start();
    while (editor.isReadOnly() && waiting.elapsed() < 30000) {
        QApplication::processEvents(QEventLoop::AllEvents, 20);
        ++spins;
    }
    ZT_TRUE("цикл событий работал во время ввоза", spins > 1);
    ZT_TRUE("после ввоза правка вернулась", !editor.isReadOnly());

    // Порядок вставки — порядок ВЫБОРА, а не готовности.
    QStringList inserted;
    for (QTextBlock block = editor.document()->begin(); block.isValid(); block = block.next()) {
        const zametti::BlockImageRef ref = zametti::blockImageRef(block);
        if (ref.valid) inserted << ref.alt;   // текста у объекта нет — есть подпись
    }
    ZT_EQ("порядок вставки — порядок выбора файлов", std::string("кадр-0, кадр-1, кадр-2"),
          inserted.join(QStringLiteral(", ")).toStdString());

    // Один шаг истории на всю пачку: Ctrl+Z убирает три картинки разом.
    editor.undo();
    QStringList afterUndo;
    for (QTextBlock block = editor.document()->begin(); block.isValid(); block = block.next())
        if (zametti::blockImageRef(block).valid) afterUndo << block.text();
    ZT_TRUE("одна отмена убрала всю пачку", afterUndo.isEmpty());
}

static int ztRunSuite(int argc, char** argv) {
    (void)argc;
    (void)argv;
    QTemporaryDir tmp;
    if (!tmp.isValid()) {
        std::printf("не завёлся временный каталог\n");
        return 2;
    }
    g_dir = tmp.path();

    checkNameCarriesShotDate();
    checkBackgroundImport();
    checkStoresUnderFreshName();
    checkRefusalLeavesNoTrash();
    checkMarkdownEscapesAlt();
    checkInsertSurvivesSave();
    checkInsertSurvivesSaveInStore();
    checkNoUpscale();
    checkPixelsFromClipboard();
    checkMimeAcceptance();
    checkMultiInsertIsOneUndo();

    return zt::report("вставка картинок");
}

// Набор целиком одним TEST: тело не тронуто, argv ему собран здесь.
// Дробить на отдельные проверки — отдельная работа, по одному набору.
TEST(ImageInsert, All) {
    std::vector<QByteArray> ztArgs{QByteArrayLiteral("image_insert_test")};
    std::vector<char*> ztArgv;
    for (QByteArray& a : ztArgs) ztArgv.push_back(a.data());
    EXPECT_EQ(0, ztRunSuite(int(ztArgv.size()), ztArgv.data()));
}
