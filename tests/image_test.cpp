// Показ картинок в просмотре.
//
// Модель текста картинок не знает: строка остаётся строкой (подпись
// image-спана или дословное вики-вложение "![[путь|ширина]]"), но рисуется
// на её месте сама фотография — строка хитро-отрисованная, как черта. Текст
// показывается, только когда строку задевает выделение. Место резервирует
// syncImageSpace нижним полем блока: скрытая строка — фото минус высота
// строки, показанная — фото целиком под текстом.
//
// Ресайз: угол фотографии тянется мышью, отпускание записывает ширину
// операцией — вики-вложению в "|ширину", image-спану в "#w=" пути.

#include "caption_editor.h"
#include "doc_model.h"
#include "document_builder.h"
#include "pieces.h"
#include "editor_ops.h"
#include "editor_widget.h"
#include "zapp.h"
#include "settings.h"
#include "settings_hook.h"

#include "keys.h"
#include "test_util.h"

#include <vector>
#include "testdata.h"

#include <QApplication>
#include <QClipboard>
#include <QGuiApplication>
#include <QImage>
#include <QImageReader>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollBar>
#include <QTest>
#include <QAbstractTextDocumentLayout>
#include <QTextBlock>
#include <QTextCursor>

#include <cmath>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace {

int countReddish(const QImage& shot) {
    int hits = 0;
    for (int y = 0; y < shot.height(); ++y)
        for (int x = 0; x < shot.width(); ++x) {
            const QColor c = shot.pixelColor(x, y);
            if (c.red() > 180 && c.green() < 90 && c.blue() < 90) ++hits;
        }
    return hits;
}

// Та же отбивка, что у вида (imageGap при масштабе 1).
const qreal kGap = 6.0;
// Вылет уголков выбора закладывается в резерв ВСЕГДА, сверху и снизу: иначе
// при прокрутке их откусывало бы чужой перерисовкой. Берём числа из настроек,
// а не переписываем сюда: разойтись им тогда негде.
qreal cornersRoom() {
    return 2.0 * (qMax(0.0, zametti::settings().style().imageCornerOffset()) +
                  qMax(0.5, zametti::settings().style().imageCornerWidth()));
}

// Сколько высоты добирает ПОДПИСЬ под снимком. Числа берём из настроек и
// меряем тем же шрифтом, что и вид: переписывать сюда формулу вёрстки нельзя —
// проверка стала бы копией реализации и перестала бы что-либо утверждать.
// Здесь утверждается другое: подпись занимает столько, сколько занимает её
// текст, и ни строкой больше.
qreal captionRoom(const QString& alt, qreal photoWidth) {
    const zametti::ZDocStyle& look = zametti::settings().style();
    if (!look.imageCaption() || alt.isEmpty()) return 0.0;
    QFont font(look.imageCaptionFamily());
    font.setPointSizeF(look.imageCaptionPoints());
    const QFontMetricsF metrics(font);
    return look.imageCaptionGap() +
           metrics.boundingRect(QRectF(0, 0, photoWidth, 1e6),
                                Qt::TextWordWrap | Qt::AlignLeft, alt)
               .height();
}

}  // namespace


// Готовый WebP: 120x80, сплошной тёмно-зелёный, без потерь. 42 байта, сделан
// внешним cwebp и положен сюда БАЙТАМИ НАРОЧНО.
//
// Раньше набор изготавливал этот файл сам — `QImage::save(".../*.webp")`. Так
// делать нельзя по двум причинам, и вторая нашлась дорого.
//
// Первая: САМА ПРОГРАММА WEBP НЕ ПИШЕТ НИКОГДА. Пишет она ровно один формат —
// JPEG XL, а webp только читает, своим вендоренным декодером (у которого
// энкодер выключен нарочно, см. 3rdparty/libwebp/update.sh). Набор, пишущий
// webp, проверял путь, которого у программы нет.
//
// Вторая: запись шла ЧУЖИМ плагином Qt (libqwebp), а он необязателен. В
// динамическом Qt из Ubuntu он есть, и набор был зелёным; в статической
// переносимой сборке его нет — Qt не собирает webp без системной libwebp, а
// её мы в sysroot не кладём нарочно. `save()` там молча вернул false, и набор
// покраснел тремя проверками подряд, ни одна из которых про webp не говорила.
static const unsigned char kGreenWebP[] = {
      0x52, 0x49, 0x46, 0x46, 0x22, 0x00, 0x00, 0x00, 0x57, 0x45, 0x42, 0x50,
      0x56, 0x50, 0x38, 0x4c, 0x16, 0x00, 0x00, 0x00, 0x2f, 0x77, 0xc0, 0x13,
      0x00, 0x07, 0x50, 0xc0, 0x88, 0xfe, 0x87, 0x01, 0x48, 0x08, 0xff, 0xf7,
      0x4b, 0x11, 0xfd, 0x4f, 0x5d, 0x01
};

// Пропавшее вложение — рамка, а не пустота.
//
// Случай не выдуманный: файл могли удалить руками, он мог не приехать с
// синхронизацией, а в слепке истории ссылка на давно удалённое — вообще норма.
// Байты ссылки в заметке при этом неприкосновенны: вернётся файл — вернётся и
// картинка, и это тоже проверяется.
void checkMissingAttachment() {
    const fs::path dir = fs::temp_directory_path() / "zametti-нет-вложения";
    fs::remove_all(dir);
    fs::create_directories(dir);

    const QString picture = QString::fromStdString((dir / "01n6cqevh7bbfr.webp").string());
    const QString note = QString::fromStdString((dir / "заметка.md").string());
    {
        QFile f(note);
        ZT_TRUE("заметка записана", f.open(QIODevice::WriteOnly));
        f.write(QStringLiteral("# Заметка\n\n![вид](01n6cqevh7bbfr.webp#w=300)\n").toUtf8());
    }

    zametti::NoteEditor editor;
    editor.resize(800, 600);
    editor.show();
    QTest::qWait(20);
    editor.openFile(note);
    QTest::qWait(50);

    ZT_EQ("файла нет — картинка не показана", std::string("0"),
          std::to_string(editor.shownImageCount()));
    ZT_EQ("вместо неё рамка", std::string("1"),
          std::to_string(editor.framedImageCount()));

    // Место под рамку держится: строка не схлопнулась в обычную.
    const QTextBlock block = editor.document()->findBlockByNumber(2);
    ZT_TRUE("строка осталась строкой-фотографией", zametti::blockImageRef(block).valid);
    // Место — это ВЫСОТА БЛОКА: у объекта нет полей, его размер знает он сам.
    // Мерка — высота ОБЫЧНОЙ строки этой же заметки: рамка обязана быть выше,
    // иначе строка схлопнулась в текстовую и пропажа выглядит как пустота.
    // Спрашивать assignedLineHeight у самого блока нельзя: у блока с объектом
    // Qt назначает высоту строки по объекту, и мерка сравнилась бы сама с собой.
    const auto heightOf = [&](int n) {
        return editor.document()->documentLayout()
            ->blockBoundingRect(editor.document()->findBlockByNumber(n)).height();
    };
    ZT_TRUE("и под неё отведено место", heightOf(2) > 2 * heightOf(0));

    // Байты ссылки не тронуты.
    const std::string text = markdownOf(blocksOf(*editor.document()));
    ZT_TRUE("ссылка в заметке цела",
            text.find("01n6cqevh7bbfr.webp#w=300") != std::string::npos);

    // Файл вернулся — вернулась и картинка. Кладём ГОТОВЫЕ байты (см.
    // kGreenWebP выше), а не изготавливаем webp через Qt.
    {
        QFile f(picture);
        ZT_TRUE("вложение вернулось", f.open(QIODevice::WriteOnly));
        ZT_EQ("вложение записано целиком", std::to_string(sizeof(kGreenWebP)),
              std::to_string(f.write(reinterpret_cast<const char*>(kGreenWebP),
                                     qint64(sizeof(kGreenWebP)))));
    }
    editor.openFile(note);
    QTest::qWait(50);
    ZT_EQ("рамки больше нет", std::string("0"),
          std::to_string(editor.framedImageCount()));
    ZT_EQ("картинка показана", std::string("1"),
          std::to_string(editor.shownImageCount()));

    fs::remove_all(dir);
}

// Исходник строки-фотографии у любого редактора: текста у объекта нет, а то,
// что уйдёт в файл, лежит в свойствах формата.
QString sourceIn(const zametti::NoteEditor& editor, int number) {
    const QTextBlock block = editor.document()->findBlockByNumber(number);
    for (QTextBlock::iterator it = block.begin(); !it.atEnd(); ++it) {
        const QTextCharFormat f = it.fragment().charFormat();
        if (f.objectType() == zametti::ImageObject)
            return f.property(zametti::ObjectSourceProperty).toString();
    }
    return block.text();
}

// ФОТОГРАФИЯ — ОБЪЕКТ, И U+FFFC НЕ ПОКИДАЕТ ДОКУМЕНТ.
//
// В живом документе фотография занимает один знак U+FFFC, а её исходник лежит
// рядом, в свойствах формата. Наружу — в файл, в буфер обмена, в журнал —
// обязан уходить исходник и только он. Правило железное и куплено дорого:
// инцидент №15, заглушка «удалено: 2 строки» в живой заметке владельца.
//
// Снимите починку (перестаньте отдавать исходник в markdown_writer) — и здесь
// в файл поедет U+FFFC.
void checkImageObjectRoundTrip() {
    const char* const cases[] = {
        "текст\n\n![подпись](фото.jxl)\n\nхвост\n",
        "![подпись](фото.jxl#w=560&align=left)\n",
        "![[вложение.png|300]]\n",
        // ПУСТАЯ ПОДПИСЬ ЗАКОННА: у картинки содержимое — сам снимок, а не
        // подпись, и к иным снимкам она просто не имеет смысла (правило
        // владельца). Прежде такая строка деградировала в дословный кусок —
        // в документ уезжали десять знаков «![](фото.jxl)» текстом, и
        // фотография не рисовалась вовсе.
        "![](фото.jxl)\n",
        "![](фото.jxl#w=560&align=left)\n",
        "![](а.png)\n\n![](б.png)\n",
        // Картинка ПОСРЕДИ строки объектом не бывает: она живёт внутри текста.
        "текст с ![встроенной](в.png) картинкой внутри строки\n",
    };
    for (const char* source : cases) {
        zametti::ZDocument note;
        note.loadMarkdown(source);
        const std::string out = note.toMarkdown();
        ZT_EQ(std::string("круг с фотографией: ") + source, std::string(source), out);
        ZT_TRUE(std::string("U+FFFC не уходит в файл: ") + source,
                out.find("\xef\xbf\xbc") == std::string::npos);
    }
}

// Пустая подпись: снимок рисуется, а места под подпись не отводится вовсе.
void checkEmptyCaption() {
    const fs::path dir = fs::temp_directory_path() / "zametti-пустая-подпись";
    fs::remove_all(dir);
    fs::create_directories(dir);

    QImage square(64, 64, QImage::Format_RGB32);
    square.fill(QColor(220, 30, 30));
    ZT_TRUE("картинка записана",
            square.save(QString::fromStdString((dir / "img.png").string())));
    {
        std::ofstream out(dir / "н.md", std::ios::binary);
        out << "![](img.png)\n\n![подпись](img.png)\n";
    }

    zametti::NoteEditor editor;
    editor.resize(600, 500);
    editor.show();
    QTest::qWait(20);
    editor.openFile(QString::fromStdString((dir / "н.md").string()));
    QTest::qWait(50);

    const auto blockAt = [&](int n) { return editor.document()->findBlockByNumber(n); };
    const auto takenBy = [&](int n) {
        return editor.document()->documentLayout()->blockBoundingRect(blockAt(n)).height();
    };
    ZT_TRUE("снимок без подписи — всё равно фотография",
            zametti::blockImageRef(blockAt(0)).valid);
    ZT_TRUE("и он нарисован, а не показан текстом",
            !editor.imageRectInViewport(blockAt(0)).isEmpty());
    ZT_TRUE("под пустую подпись места не отведено (" + std::to_string(int(takenBy(0))) + ")",
            std::fabs(takenBy(0) - (64.0 + kGap + cornersRoom())) < 1.5);
    ZT_TRUE("а под настоящую — отведено",
            takenBy(2) > takenBy(0) + 8.0);
}

// БЕЗЫМЯННАЯ ПОДПИСЬ ПОД СНИМКОМ НЕ ПОКАЗЫВАЕТСЯ и места не занимает — ни имя
// от камеры («IMG_1234»), ни спрятанная знаком («~подпись»); настоящая —
// показывается. Решение владельца: «хорошие имена показывать, дурацкие
// скрывать». В файле все три остаются как есть.
void checkNonameCaption() {
    const fs::path dir = fs::temp_directory_path() / "zametti-безымянная-подпись";
    fs::remove_all(dir);
    fs::create_directories(dir);

    QImage square(64, 64, QImage::Format_RGB32);
    square.fill(QColor(220, 30, 30));
    ZT_TRUE("картинка записана",
            square.save(QString::fromStdString((dir / "img.png").string())));
    const char* const source =
        "![IMG_1234](img.png)\n\n![~подпись](img.png)\n\n![подпись](img.png)\n";
    {
        std::ofstream out(dir / "н.md", std::ios::binary);
        out << source;
    }

    zametti::NoteEditor editor;
    editor.resize(600, 500);
    editor.show();
    QTest::qWait(20);
    editor.openFile(QString::fromStdString((dir / "н.md").string()));
    QTest::qWait(50);

    const auto blockAt = [&](int n) { return editor.document()->findBlockByNumber(n); };
    const auto takenBy = [&](int n) {
        return editor.document()->documentLayout()->blockBoundingRect(blockAt(n)).height();
    };
    const qreal bare = 64.0 + kGap + cornersRoom();
    ZT_TRUE("имя от камеры под снимком места не занимает (" +
                std::to_string(int(takenBy(0))) + ")",
            std::fabs(takenBy(0) - bare) < 1.5);
    ZT_TRUE("спрятанная знаком — тоже (" + std::to_string(int(takenBy(2))) + ")",
            std::fabs(takenBy(2) - bare) < 1.5);
    ZT_TRUE("а настоящая — занимает", takenBy(4) > bare + 8.0);
    ZT_EQ("в файле все три подписи целы", std::string(source), editor.note().toMarkdown());
}

// ПРАВКА ПОДПИСИ ПО ENTER: поле ввода встаёт под снимок, Enter принимает
// подпись глаголом заметки, Esc отменяет; сочетание переключения (Ctrl+D)
// прячет подпись знаком «~» и возвращает обратно; Ctrl+Z отменяет и то, и другое.
void checkCaptionEditing() {
    const fs::path dir = fs::temp_directory_path() / "zametti-правка-подписи";
    fs::remove_all(dir);
    fs::create_directories(dir);

    QImage square(64, 64, QImage::Format_RGB32);
    square.fill(QColor(220, 30, 30));
    ZT_TRUE("картинка записана",
            square.save(QString::fromStdString((dir / "img.png").string())));
    {
        std::ofstream out(dir / "н.md", std::ios::binary);
        out << "текст\n\n![Вид](img.png)\n\nхвост\n";
    }

    zametti::NoteEditor editor;
    editor.resize(600, 500);
    editor.show();
    QTest::qWait(20);
    editor.openFile(QString::fromStdString((dir / "н.md").string()));
    QTest::qWait(50);

    const auto imageBlock = [&] {
        for (QTextBlock b = editor.document()->begin(); b.isValid(); b = b.next())
            if (zametti::blockImageRef(b).valid) return b.blockNumber();
        return -1;
    };
    const auto markdown = [&] { return editor.note().toMarkdown(); };
    const auto takenBy = [&](int n) {
        return editor.document()->documentLayout()->blockBoundingRect(
            editor.document()->findBlockByNumber(n)).height();
    };
    const int photo = imageBlock();
    ZT_TRUE("фотография найдена", photo >= 0);
    if (photo < 0) return;
    const qreal shownRoom = takenBy(photo);

    // Enter на снимке — поле с подписью как она есть.
    editor.setTextCursor(QTextCursor(editor.document()->findBlockByNumber(photo)));
    QTest::keyClick(&editor, Qt::Key_Return);
    QTest::qWait(20);
    ZT_TRUE("Enter открыл поле подписи", editor.imageCaptionEditor() != nullptr);
    if (editor.imageCaptionEditor() == nullptr) return;
    ZT_EQ("в поле — нынешняя подпись", "Вид", editor.imageCaptionEditor()->text().toStdString());
    ZT_TRUE("поле стоит под снимком, а не поверх него",
            editor.imageCaptionEditor()->geometry().top() + 1 >=
                editor.imageRectInViewport(editor.document()->findBlockByNumber(photo)).bottom());
    ZT_EQ("заметка пока не тронута", "текст\n\n![Вид](img.png)\n\nхвост\n", markdown());

    // Латиница нарочно: QTest::keyClicks знает только ASCII.
    QTest::keyClicks(editor.imageCaptionEditor(), QStringLiteral(" 2"));
    QTest::keyClick(editor.imageCaptionEditor(), Qt::Key_Return);
    QTest::qWait(20);
    ZT_TRUE("Enter в поле закрыл его", editor.imageCaptionEditor() == nullptr);
    ZT_EQ("подпись записана глаголом заметки", "текст\n\n![Вид 2](img.png)\n\nхвост\n",
          markdown());
    ZT_EQ("каретка осталась на снимке", std::to_string(photo),
          std::to_string(editor.textCursor().blockNumber()));
    ZT_TRUE("снимок нарисован по-прежнему",
            !editor.imageRectInViewport(editor.document()->findBlockByNumber(photo)).isEmpty());

    // Esc — отмена: заметка как была.
    QTest::keyClick(&editor, Qt::Key_Return);
    QTest::qWait(20);
    ZT_TRUE("поле открыто снова", editor.imageCaptionEditor() != nullptr);
    if (editor.imageCaptionEditor() != nullptr) {
        QTest::keyClicks(editor.imageCaptionEditor(), QStringLiteral("zzz"));
        QTest::keyClick(editor.imageCaptionEditor(), Qt::Key_Escape);
        QTest::qWait(20);
    }
    ZT_TRUE("Esc закрыл поле", editor.imageCaptionEditor() == nullptr);
    ZT_EQ("и ничего не записал", "текст\n\n![Вид 2](img.png)\n\nхвост\n", markdown());
    ZT_TRUE("место под подпись после правки — как у показанной (" +
                std::to_string(int(takenBy(photo))) + " против " +
                std::to_string(int(shownRoom)) + ")",
            std::fabs(takenBy(photo) - shownRoom) < 1.5);

    // Сочетание переключения на снимке — спрятать подпись знаком; место под
    // неё исчезает, файл хранит подпись со знаком.
    // СПИСОК, А НЕ ОДНО СОЧЕТАНИЕ: настройка допускает несколько через точку с
    // запятой («Ctrl+D; Ctrl+SPACE»), и разбирает её listFromString — ровно так
    // же, как в бою (editor_widget.cpp). QKeySequence(QString) точки с запятой
    // не знает и молча отдавал мусор: набор жал несуществующую клавишу.
    const QString toggle = zametti::settings().editor().toggleTaskKey();
    ZT_TRUE("хоткей переключателя разобран", !zt::firstKey(toggle).isEmpty());
    if (zt::firstKey(toggle).isEmpty()) return;
    zt::pressKey(&editor, toggle);
    QTest::qWait(30);
    ZT_EQ("подпись спрятана знаком", "текст\n\n![~Вид 2](img.png)\n\nхвост\n", markdown());
    ZT_TRUE("под спрятанную места не отведено (" + std::to_string(int(takenBy(photo))) + ")",
            takenBy(photo) < shownRoom - 8.0);
    ZT_TRUE("снимок на месте",
            !editor.imageRectInViewport(editor.document()->findBlockByNumber(photo)).isEmpty());
    zt::pressKey(&editor, toggle);
    QTest::qWait(30);
    ZT_EQ("и возвращена", "текст\n\n![Вид 2](img.png)\n\nхвост\n", markdown());
    ZT_TRUE("место под подпись вернулось", std::fabs(takenBy(photo) - shownRoom) < 1.5);

    // Отмена — штатная: шаг за шагом назад.
    editor.undo();
    QTest::qWait(10);
    ZT_EQ("Ctrl+Z: снова спрятана", "текст\n\n![~Вид 2](img.png)\n\nхвост\n", markdown());
    editor.undo();
    QTest::qWait(10);
    ZT_EQ("Ctrl+Z: снова показана", "текст\n\n![Вид 2](img.png)\n\nхвост\n", markdown());
    editor.undo();
    QTest::qWait(10);
    ZT_EQ("Ctrl+Z: прежняя подпись", "текст\n\n![Вид](img.png)\n\nхвост\n", markdown());

    // Пустая подпись законна: поле очистили — картинка осталась. Отмена
    // могла увести каретку — ставим её на снимок заново.
    editor.setTextCursor(QTextCursor(editor.document()->findBlockByNumber(photo)));
    QTest::keyClick(&editor, Qt::Key_Return);
    QTest::qWait(20);
    ZT_TRUE("поле открыто и после отмены", editor.imageCaptionEditor() != nullptr);
    if (editor.imageCaptionEditor() != nullptr) {
        editor.imageCaptionEditor()->clear();
        QTest::keyClick(editor.imageCaptionEditor(), Qt::Key_Return);
        QTest::qWait(20);
    }
    ZT_EQ("пустая подпись — картинка цела", "текст\n\n![](img.png)\n\nхвост\n", markdown());
    ZT_TRUE("и по-прежнему объект", imageBlock() == photo);
    // Прятать пустую нечего — сочетание молчит, файл не меняется.
    zt::pressKey(&editor, toggle);
    QTest::qWait(20);
    ZT_EQ("знак перед пустой подписью не ставится", "текст\n\n![](img.png)\n\nхвост\n",
          markdown());
}

// Картинка в конце заметки: Ctrl+Enter заводит абзац под ней, и НАБОР В НЁМ
// картинку не трогает.
//
// Владелец наткнулся ровно так: «нажимаю Ctrl-Enter, курсор переезжает на новый
// параграф после картинки; нажимаю любую букву — картинка пропадает». Причина:
// починка после набора сливала соседей, которые в файле слиплись бы, — а
// фотография атом, слияние делает её строкой обычного текста.
void checkTypingAfterImage() {
    const fs::path dir = fs::temp_directory_path() / "zametti-набор-под-фото";
    fs::remove_all(dir);
    fs::create_directories(dir);

    QImage square(64, 64, QImage::Format_RGB32);
    square.fill(QColor(220, 30, 30));
    ZT_TRUE("картинка записана",
            square.save(QString::fromStdString((dir / "img.png").string())));
    const QString note = QString::fromStdString((dir / "н.md").string());
    {
        std::ofstream out(dir / "н.md", std::ios::binary);
        out << "текст\n\n![вид](img.png)\n";
    }

    zametti::NoteEditor editor;
    editor.resize(600, 500);
    editor.show();
    QTest::qWait(20);
    editor.openFile(note);
    QTest::qWait(50);

    const auto imageBlock = [&] {
        for (QTextBlock b = editor.document()->begin(); b.isValid(); b = b.next())
            if (zametti::blockImageRef(b).valid) return b.blockNumber();
        return -1;
    };
    const int photo = imageBlock();
    ZT_TRUE("фотография найдена", photo >= 0);
    if (photo < 0) return;

    // Ctrl+Enter на фотографии: абзац ПОД ней, каретка в нём.
    QTextCursor on(editor.document()->findBlockByNumber(photo));
    editor.setTextCursor(on);
    QTest::keyClick(&editor, Qt::Key_Return, Qt::ControlModifier);
    QTest::qWait(20);
    ZT_TRUE("каретка уехала под фотографию",
            editor.textCursor().blockNumber() > photo);

    // Латиница нарочно: QTest::keyClicks знает только ASCII и на кириллице
    // падает ассертом внутри самой Qt.
    QTest::keyClicks(&editor, QStringLiteral("x"));
    QTest::qWait(20);
    ZT_TRUE("после набора фотография на месте", imageBlock() >= 0);
    ZT_TRUE("и она по-прежнему нарисована",
            !editor.imageRectInViewport(
                     editor.document()->findBlockByNumber(imageBlock())).isEmpty());
    ZT_TRUE("набранное на месте", editor.toPlainText().contains(QStringLiteral("x")));

    // BACKSPACE НА ПУСТОЙ СТРОКЕ НАД ОБЪЕКТОМ. Владелец: «курсор на пустой
    // строке между текстом и картинкой, нажимаю BACKSPACE — картинка исчезает,
    // подпись становится гиперссылкой». Пустая строка там ОБЯЗАТЕЛЬНА (без неё
    // абзацы слиплись бы в файле), а слияние соседей губит объект.
    //
    // Правило то же, что у пустой строки ПОД объектом: отказ и шаг.
    {
        const int photoNow = imageBlock();
        ZT_TRUE("фотография на месте перед проверкой", photoNow > 0);
        if (photoNow > 0) {
            QTextCursor gap(editor.document()->findBlockByNumber(photoNow - 1));
            editor.setTextCursor(gap);
            QTest::keyClick(&editor, Qt::Key_Backspace);
            QTest::qWait(20);
            ZT_TRUE("после Backspace над фотографией она цела", imageBlock() >= 0);
            ZT_TRUE("и осталась нарисованной",
                    !editor.imageRectInViewport(
                             editor.document()->findBlockByNumber(imageBlock())).isEmpty());
            ZT_TRUE("подпись не стала текстом строки",
                    !editor.toPlainText().contains(QStringLiteral("вид")));
        }
    }
}

static int ztRunSuite(int argc, char** argv) {
    (void)argc;
    (void)argv;
    zametti::loadSettings(nullptr);

    const fs::path dir = fs::temp_directory_path() / "zametti-image-test";
    fs::create_directories(dir);

    // Красный квадрат 64×64 — его пиксели потом ищутся в кадре.
    QImage square(64, 64, QImage::Format_RGB32);
    square.fill(QColor(220, 30, 30));
    ZT_TRUE("картинка записана",
            square.save(QString::fromStdString((dir / "img.png").string())));

    const char* source =
        "![фото](img.png)\n"
        "\n"
        "![[img.png|40]]\n"
        "\n"
        "![нет такой](пропала.png)\n"
        "\n"
        "просто текст\n";
    {
        std::ofstream out(dir / "н.md", std::ios::binary);
        out << source;
    }

    zametti::NoteEditor editor;
    editor.resize(600, 500);
    editor.show();
    QTest::qWait(20);
    editor.openFile(QString::fromStdString((dir / "н.md").string()));
    QTest::qWait(20);

    const auto blockAt = [&](int n) { return editor.document()->findBlockByNumber(n); };
    // ИСХОДНИК СТРОКИ-ФОТОГРАФИИ. Текста у неё больше нет: в документе стоит
    // объект (один знак U+FFFC), а то, что уйдёт в файл, лежит в свойствах его
    // формата. Спрашиваем ровно его — это и есть обещание, а не представление.
    const auto sourceOf = [&](int n) {
        for (QTextBlock::iterator it = blockAt(n).begin(); !it.atEnd(); ++it) {
            const QTextCharFormat f = it.fragment().charFormat();
            if (f.objectType() == zametti::ImageObject)
                return f.property(zametti::ObjectSourceProperty).toString();
        }
        return blockAt(n).text();
    };
    const auto marginOf = [&](int n) { return blockAt(n).blockFormat().bottomMargin(); };
    // СКОЛЬКО МЕСТА БЛОК ЗАНЯЛ НА САМОМ ДЕЛЕ — отведённая Qt высота плюс наше
    // поле. Проверять надо это, а не формулу резерва: формула — реализация, а
    // занятое место — обещание. Прежняя редакция сверяла «фото + отбивка минус
    // ЗАПРОШЕННАЯ высота строки», и когда выяснилось, что Qt отводит другую,
    // проверки покраснели, хотя вёрстка стала правильнее.
    const auto takenBy = [&](int n) {
        return editor.document()->documentLayout()->blockBoundingRect(blockAt(n)).height() +
               marginOf(n);
    };

    // Скрытая строка: фото стоит на месте текста и торчит из него вниз.
    ZT_TRUE("под фотографию 64 px занято ровно столько, сколько нужно (" +
                std::to_string(int(takenBy(0))) + ")",
            std::fabs(takenBy(0) - (64.0 + kGap + cornersRoom() +
                                    captionRoom(QStringLiteral("фото"), 64.0))) < 1.5);
    ZT_TRUE("вики-вложение шириной 40 заняло своё (" +
                std::to_string(int(takenBy(2))) + ")",
            std::fabs(takenBy(2) - (40.0 + kGap + cornersRoom())) < 1.5);
    // Файла нет — но место есть: с этапа 7 вместо пропавшего вложения рисуется
    // рамка «файл не найден», и под неё резервируется место. Прежде строка
    // схлопывалась в обычную, и пропажа выглядела как будто картинки тут
    // никогда и не было.
    // Место меряем ЗАНЯТЫМ МЕСТОМ, а не нижним полем блока: поля у объекта нет
    // вовсе — его размер спрашивает Qt, и в этом весь смысл перевода.
    ZT_TRUE("под пропавший файл держится место для рамки", takenBy(4) > takenBy(6) + 8.0);
    ZT_TRUE("под обычный текст места нет", marginOf(6) == 0.0);

    // Цвет в центре фотографии блока: по нему видно, трогает ли выбор сами
    // пиксели снимка. Не должен трогать — за цветами на снимок и смотрят.
    const auto shadeOf = [&](int n) {
        QImage frame(editor.viewport()->size(), QImage::Format_RGB32);
        frame.fill(Qt::white);
        QPainter painter(&frame);
        editor.viewport()->render(&painter);
        const QRectF photo = editor.imageRectInViewport(blockAt(n));
        return frame.pixelColor(photo.center().toPoint());
    };
    const auto sameShade = [](const QColor& a, const QColor& b) {
        return a.red() == b.red() && a.green() == b.green() && a.blue() == b.blue();
    };
    const auto caretTo = [&](int n) {
        editor.setTextCursor(QTextCursor(blockAt(n)));
        QTest::qWait(10);
    };

    // Сколько точек цвета каретки видно в углу фотографии: выбор помечается
    // четырьмя уголками, как мишень в видоискателе. Считаем именно их, а не
    // «стало не так, как было»: посчитанные точки говорят, что нарисовано
    // ровно то и ровно там.
    const auto cornerMarks = [&](int n) {
        QImage frame(editor.viewport()->size(), QImage::Format_RGB32);
        frame.fill(Qt::white);
        QPainter painter(&frame);
        editor.viewport()->render(&painter);
        const QRectF photo = editor.imageRectInViewport(blockAt(n));
        const QColor want = zametti::settings().style().caretColor();
        int marks = 0;
        // Полоса вдоль верхнего края, захватывающая и то, что СНАРУЖИ: уголки
        // вынесены за край фотографии, чтобы не сливаться с её содержимым.
        for (int x = int(photo.left()) - 12; x < int(photo.right()) + 12; ++x)
            for (int y = int(photo.top()) - 12; y < int(photo.top()) + 4 && y < frame.height();
                 ++y) {
                if (x < 0 || y < 0 || x >= frame.width()) continue;
                const QColor at = frame.pixelColor(x, y);
                if (std::abs(at.red() - want.red()) < 24 &&
                    std::abs(at.green() - want.green()) < 24 &&
                    std::abs(at.blue() - want.blue()) < 24)
                    ++marks;
            }
        return marks;
    };

    // Выделение — это выбранная фотография, а не вскрытая разметка: текст не
    // показывается, резерв не дёргается, а по углам встают уголки. Сами
    // пиксели снимка при этом не трогаются вовсе — прежняя заливка их красила.
    {
        caretTo(6);   // каретка в стороне: она сама по себе помечает фото
        const QColor plain = shadeOf(2);
        ZT_TRUE("невыбранная фотография уголков не имеет", cornerMarks(2) == 0);

        QTextCursor cursor(blockAt(2));
        cursor.movePosition(QTextCursor::Right, QTextCursor::KeepAnchor, 3);
        editor.setTextCursor(cursor);
        QTest::qWait(10);
        ZT_TRUE("выделение не тронуло резерв",
                std::fabs(takenBy(2) - (40.0 + kGap + cornersRoom())) < 1.5);
        ZT_TRUE("у выделенной фотографии есть уголки", cornerMarks(2) > 0);
        ZT_TRUE("цвета самого снимка не тронуты", sameShade(plain, shadeOf(2)));

        caretTo(6);
        ZT_TRUE("уголки сняты вместе с выделением", cornerMarks(2) == 0);

        // Каретка, вставшая на строку-фотографию, — та же выбранная
        // фотография: уголки без всякого выделения.
        caretTo(2);
        ZT_TRUE("каретка на фотографии помечает её уголками", cornerMarks(2) > 0);
        caretTo(6);
        ZT_TRUE("каретка ушла — уголки сняты", cornerMarks(2) == 0);
    }

    // Фотографии действительно в кадре: красных пикселей не меньше, чем в
    // самих картинках (64×64 + 40×40), с запасом на сглаживание краёв.
    QImage shot(editor.viewport()->size(), QImage::Format_RGB32);
    shot.fill(Qt::white);
    {
        QPainter painter(&shot);
        editor.viewport()->render(&painter);
    }
    ZT_TRUE("фотографии нарисованы", countReddish(shot) > 64 * 64 + 40 * 40 - 600);

    // Ресайз мышью: взяться за нижний правый угол, потянуть вправо, отпустить
    // — ширина записывается в "|ширину" вики-вложения, отдельным шагом истории.
    {
        const QRectF photo = editor.imageRectInViewport(blockAt(2));
        ZT_TRUE("фото вики-вложения имеет прямоугольник", !photo.isEmpty());
        const QPointF grip(photo.right() - 4.0, photo.bottom() - 4.0);
        const QPointF pulled = grip + QPointF(26.0, 9.0);
        // Математика дельтой: насколько уехала мышь, настолько выросла ширина.
        const int expected = qRound(photo.width() + 26.0);

        QTest::mousePress(editor.viewport(), Qt::LeftButton, {}, grip.toPoint());
        QMouseEvent drag(QEvent::MouseMove, pulled, editor.viewport()->mapToGlobal(pulled),
                         Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(editor.viewport(), &drag);
        QTest::mouseRelease(editor.viewport(), Qt::LeftButton, {}, pulled.toPoint());
        QTest::qWait(10);

        ZT_EQ("ширина записана в вики-вложение",
              QStringLiteral("![[img.png|%1]]").arg(expected).toStdString(),
              sourceOf(2).toStdString());
        ZT_TRUE("резерв пересчитан под новую ширину",
                std::fabs(takenBy(2) - (expected + kGap + cornersRoom())) < 1.5);

        QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
        QTest::qWait(10);
        ZT_EQ("Ctrl-Z вернул прежнюю ширину", std::string("![[img.png|40]]"),
              sourceOf(2).toStdString());
    }

    // Esc посреди жеста — отмена без записи: ни текста, ни шага истории.
    {
        const std::string before = sourceOf(2).toStdString();
        const qreal roomBefore = marginOf(2);
        const QRectF photo = editor.imageRectInViewport(blockAt(2));
        const QPointF grip(photo.right() - 4.0, photo.bottom() - 4.0);
        const QPointF pulled = grip + QPointF(40.0, 14.0);

        QTest::mousePress(editor.viewport(), Qt::LeftButton, {}, grip.toPoint());
        QMouseEvent drag(QEvent::MouseMove, pulled, editor.viewport()->mapToGlobal(pulled),
                         Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(editor.viewport(), &drag);
        QTest::qWait(10);
        ZT_TRUE("во время жеста фото и правда тянется",
                editor.imageRectInViewport(blockAt(2)).width() > photo.width() + 20.0);

        QTest::keyClick(&editor, Qt::Key_Escape);
        QTest::qWait(10);
        ZT_TRUE("Esc вернул примерочную ширину на место",
                std::fabs(editor.imageRectInViewport(blockAt(2)).width() - photo.width()) < 1.5);

        QTest::mouseRelease(editor.viewport(), Qt::LeftButton, {}, pulled.toPoint());
        QTest::qWait(10);
        ZT_EQ("после Esc отпускание ничего не записывает", before,
              sourceOf(2).toStdString());
        ZT_TRUE("и резерв остался прежним", std::fabs(marginOf(2) - roomBefore) < 1.5);
    }

    // Запись ширины image-спана — во фрагмент пути "#w=". Операция напрямую:
    // мышь уже проверена на вики-форме, путь тот же.
    {
        QTextCursor cursor(blockAt(0));
        ZT_TRUE("ширина image-спана записана",
                editor.note().setImageWidth(cursor, 50));
        QTest::qWait(10);
        const std::vector<zametti::Piece> ir = blocksOf(*editor.document());
        const std::string out = markdownOf(ir);
        ZT_TRUE("в файл уходит путь с #w=50",
                out.find("![фото](img.png#w=50)") != std::string::npos);
        ZT_TRUE("резерв ужался до 50",
                std::fabs(takenBy(0) - (50.0 + kGap + cornersRoom() +
                                        captionRoom(QStringLiteral("фото"), 50.0))) < 1.5);
    }

    // Правка ломает путь вики-вложения — резерв обязан сняться.
    QTextCursor cursor(blockAt(2));
    cursor.movePosition(QTextCursor::EndOfBlock);
    editor.setTextCursor(cursor);
    editor.insertPlainText(QStringLiteral("х"));
    QTest::qWait(10);
    ZT_TRUE("правка сняла резерв: вложение испорчено", marginOf(2) == 0.0);

    // И возврат правкой же — резерв возвращается.
    QTest::keyClick(&editor, Qt::Key_Backspace);
    QTest::qWait(10);
    ZT_TRUE("место вернулось после починки строки (" +
                std::to_string(int(takenBy(2))) + ")",
            std::fabs(takenBy(2) - (40.0 + kGap + cornersRoom())) < 1.5);

    // Ctrl+C/Ctrl+X/Ctrl+V: каретка на картинке — выбранная картинка. В
    // клипборд идёт текстовое представление строки, вставка идёт через полный
    // парсер ядра и встаёт своей строкой.
    {
        QClipboard* clipboard = QGuiApplication::clipboard();
        caretTo(2);
        QTest::keyClick(&editor, Qt::Key_C, Qt::ControlModifier);
        ZT_EQ("Ctrl+C кладёт текст вложения", std::string("![[img.png|40]]"),
              clipboard->text().toStdString());

        const int blocksBefore = editor.document()->blockCount();
        QTest::keyClick(&editor, Qt::Key_X, Qt::ControlModifier);
        QTest::qWait(10);
        ZT_TRUE("Ctrl+X убрал строку целиком",
                editor.document()->blockCount() == blocksBefore - 1 &&
                    !editor.toPlainText().contains(QStringLiteral("img.png|40")));
        ZT_EQ("клипборд после Ctrl+X цел", std::string("![[img.png|40]]"),
              clipboard->text().toStdString());

        // Вставка в конец: фотография встаёт своей строкой, не вклеивается.
        QTextCursor end(editor.document());
        end.movePosition(QTextCursor::End);
        editor.setTextCursor(end);
        QTest::keyClick(&editor, Qt::Key_V, Qt::ControlModifier);
        QTest::qWait(10);
        int pasted = -1;
        for (QTextBlock b = editor.document()->begin(); b.isValid(); b = b.next())
            if (sourceOf(b.blockNumber()) == QStringLiteral("![[img.png|40]]"))
                pasted = b.blockNumber();
        ZT_TRUE("Ctrl+V вернул строку-фотографию", pasted >= 0);
        if (pasted >= 0) {
            ZT_TRUE("вставленная строка — своя, не вклейка",
                    sourceOf(pasted) == QStringLiteral("![[img.png|40]]"));
            ZT_TRUE("у вставленной место есть (" +
                        std::to_string(int(takenBy(pasted))) + ")",
                    std::fabs(takenBy(pasted) - (40.0 + kGap + cornersRoom())) < 1.5);
        }

        // Откат: вставка и вырезание — по своему шагу истории.
        QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
        QTest::qWait(10);
        ZT_TRUE("Ctrl-Z убрал вставленную",
                !editor.toPlainText().contains(QStringLiteral("img.png|40")));
        QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
        QTest::qWait(10);
        ZT_EQ("Ctrl-Z вернул вырезанную на место", std::string("![[img.png|40]]"),
              sourceOf(2).toStdString());
    }

    // Канон image-спана из клипборда тоже встаёт фотографией: полный парсер
    // ядра превращает текст в спан, ширина — из "#w=".
    {
        QGuiApplication::clipboard()->setText(QStringLiteral("![пейзаж](img.png#w=33)"));
        QTextCursor end(editor.document());
        end.movePosition(QTextCursor::End);
        editor.setTextCursor(end);
        QTest::keyClick(&editor, Qt::Key_V, Qt::ControlModifier);
        QTest::qWait(10);
        int pasted = -1;
        for (QTextBlock b = editor.document()->begin(); b.isValid(); b = b.next())
            if (zametti::blockImageRef(b).alt == QStringLiteral("пейзаж"))
                pasted = b.blockNumber();
        ZT_TRUE("канон вставился image-спаном", pasted >= 0);
        if (pasted >= 0) {
            const zametti::BlockImageRef ref = zametti::blockImageRef(blockAt(pasted));
            ZT_TRUE("вставленный спан — фотография с шириной из #w=",
                    ref.valid && !ref.wiki && qRound(ref.widthHint) == 33);
        }
    }

    // Прокрутка: верх фотографии ушёл за кадр — остальная часть обязана
    // остаться на экране (hitTest по верхней кромке отдаёт следующий блок,
    // без шага назад картинка пропадала целиком).
    {
        QTextCursor cursor(blockAt(0));
        editor.setTextCursor(cursor);
        editor.note().setImageWidth(cursor, 400);
        QTest::qWait(20);
        caretTo(6);   // тонировка от каретки не должна мешать замеру красного

        QRectF photo = editor.imageRectInViewport(blockAt(0));
        ZT_TRUE("фотография раздута до 400", photo.height() > 300.0);
        // Прокрутить так, чтобы верх фото был выше кадра на треть высоты.
        editor.verticalScrollBar()->setValue(
            editor.verticalScrollBar()->value() + int(photo.top() + photo.height() / 3));
        QTest::qWait(20);
        photo = editor.imageRectInViewport(blockAt(0));
        ZT_TRUE("верх фото действительно за кадром",
                photo.top() < 0.0 && photo.bottom() > 0.0);

        QImage frame(editor.viewport()->size(), QImage::Format_RGB32);
        frame.fill(Qt::white);
        {
            QPainter painter(&frame);
            editor.viewport()->render(&painter);
        }
        ZT_TRUE("хвост фотографии в кадре дорисован",
                countReddish(frame) > int(photo.width() * photo.bottom() * 0.8));
    }

    // Левый верхний угол: тянем ВЛЕВО — фотография растёт (дельта от центра).
    {
        caretTo(6);
        const QRectF photo = editor.imageRectInViewport(blockAt(2));
        const QPointF grip(photo.left() + 3.0, photo.top() + 3.0);
        const QPointF pulled = grip + QPointF(-15.0, -5.0);
        const int expected = qRound(photo.width() + 15.0);

        QTest::mousePress(editor.viewport(), Qt::LeftButton, {}, grip.toPoint());
        QMouseEvent drag(QEvent::MouseMove, pulled, editor.viewport()->mapToGlobal(pulled),
                         Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(editor.viewport(), &drag);
        QTest::mouseRelease(editor.viewport(), Qt::LeftButton, {}, pulled.toPoint());
        QTest::qWait(10);
        ZT_EQ("левый угол растит ширину",
              QStringLiteral("![[img.png|%1]]").arg(expected).toStdString(),
              sourceOf(2).toStdString());
        QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
        QTest::qWait(10);
        ZT_EQ("Ctrl-Z вернул ширину после левого угла", std::string("![[img.png|40]]"),
              sourceOf(2).toStdString());
    }

    // Фотография — атом: свежая заметка, чтобы соседство было предсказуемым.
    {
        const char* atomSource =
            "строка\n"
            "\n"
            "![[img.png|40]]\n"
            "\n"
            "хвост\n";
        {
            std::ofstream out(dir / "а.md", std::ios::binary);
            out << atomSource;
        }
        editor.document()->setModified(false);
        editor.openFile(QString::fromStdString((dir / "а.md").string()));
        QTest::qWait(20);

        // ВНУТРЬ ФОТОГРАФИИ КАРЕТКЕ ПОПАСТЬ БОЛЬШЕ НЕ ЧЕРЕЗ ЧТО: объект — один
        // знак, и «середины» у него нет. Рядом с ним осталось одно место —
        // сразу за объектом; каретка, поставленная туда не шагом вправо,
        // сводится к началу строки.
        {
            QTextCursor inside(editor.document());
            inside.setPosition(blockAt(2).position() + 1);
            editor.setTextCursor(inside);
            QTest::qWait(10);
            ZT_TRUE("каретка за объектом сведена к началу строки",
                    editor.textCursor().position() == blockAt(2).position());
        }
        // Вправо с начала — прыжок через всю строку; влево обратно — к началу.
        QTest::keyClick(&editor, Qt::Key_Right);
        ZT_TRUE("шаг вправо перепрыгивает фото",
                editor.textCursor().position() == blockAt(3).position());
        QTest::keyClick(&editor, Qt::Key_Left);
        ZT_TRUE("шаг влево возвращает к началу фото",
                editor.textCursor().position() == blockAt(2).position());

        // Backspace на самой строке: атом гибнет целиком, огрызков не остаётся.
        QTest::keyClick(&editor, Qt::Key_Backspace);
        QTest::qWait(10);
        ZT_TRUE("Backspace на фото убрал строку без огрызков",
                !editor.toPlainText().contains(QStringLiteral("img.png")) &&
                    !editor.toPlainText().contains(QStringLiteral("[[")));
        QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
        QTest::qWait(10);
        ZT_EQ("Ctrl-Z вернул фото-атом", std::string("![[img.png|40]]"),
              sourceOf(2).toStdString());

        // Backspace из-под фотографии: пустую строку под фото удалить нельзя
        // (склеила бы текст со скрытой подписью) — отказ и шаг: каретка встаёт
        // на фотографию, следующий Backspace убирает её целиком.
        {
            QTextCursor below(editor.document());
            below.setPosition(blockAt(4).position());
            editor.setTextCursor(below);
        }
        QTest::keyClick(&editor, Qt::Key_Backspace);
        ZT_TRUE("отказ и шаг: каретка встала на фото, документ цел",
                editor.textCursor().position() == blockAt(2).position() &&
                    sourceOf(2) == QStringLiteral("![[img.png|40]]"));
        QTest::keyClick(&editor, Qt::Key_Backspace);
        QTest::qWait(10);
        ZT_TRUE("Backspace снизу: фото ушло атомом, соседи целы",
                !editor.toPlainText().contains(QStringLiteral("img.png")) &&
                    editor.toPlainText().contains(QStringLiteral("хвост")) &&
                    editor.toPlainText().contains(QStringLiteral("строка")));
        QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
        QTest::qWait(10);
        ZT_EQ("история вернула фото", std::string("![[img.png|40]]"),
              sourceOf(2).toStdString());

        // Delete сверху: то же правило зеркально — отказ и шаг, затем атом.
        {
            QTextCursor above(editor.document());
            above.setPosition(blockAt(0).position() + blockAt(0).length() - 1);
            editor.setTextCursor(above);
        }
        QTest::keyClick(&editor, Qt::Key_Delete);
        ZT_TRUE("Delete сверху: отказ и шаг на фото",
                editor.textCursor().position() == blockAt(2).position());
        QTest::keyClick(&editor, Qt::Key_Delete);
        QTest::qWait(10);
        ZT_TRUE("Delete сверху: фото ушло атомом",
                !editor.toPlainText().contains(QStringLiteral("img.png")) &&
                    editor.toPlainText().contains(QStringLiteral("строка")));

        // Щелчок по фотографии выбирает её: каретка в начале строки.
        QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
        QTest::qWait(10);
        // Щелчок по фотографии выбирает её: каретка в начале строки.
        QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
        QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
        QTest::qWait(10);
        const QRectF photo = editor.imageRectInViewport(blockAt(2));
        ZT_TRUE("фото на месте перед щелчком", !photo.isEmpty());
        QTest::mouseClick(editor.viewport(), Qt::LeftButton, {},
                          photo.center().toPoint());
        QTest::qWait(10);
        ZT_TRUE("щелчок по фото поставил каретку в начало его строки",
                editor.textCursor().position() == blockAt(2).position() &&
                    !editor.textCursor().hasSelection());
    }

    // --- кэш картинок: потолок, вытеснение, защита от бомбы ---
    //
    // Кэш живёт всю сессию, а не заметку, поэтому у него есть бюджет. Правила
    // владельца: сначала добавляем, потом убираем; картинки ОТКРЫТОЙ заметки
    // не вытесняем никогда.
    //
    // Ниже восьми мегабайт бюджет не опускается: кэш на одну картинку смысла
    // не имеет. Числа ниже подобраны под это, а не взяты с потолка.
    {
        const fs::path cacheDir = dir / "кэш";
        fs::create_directories(cacheDir);
        // Восемнадцать картинок по мегабайту разжатыми: 512x512 RGB32.
        const int kSide = 512;
        const qint64 kOne = qint64(kSide) * kSide * 4;
        for (int i = 0; i < 20; ++i) {
            QImage tile(kSide, kSide, QImage::Format_RGB32);
            tile.fill(QColor(10 * i, 40, 200));
            ZT_TRUE("картинка кэша записана",
                    tile.save(QString::fromStdString(
                        (cacheDir / ("к" + std::to_string(i) + ".png")).string())));
        }
        // Десять заметок по две картинки.
        for (int n = 0; n < 10; ++n) {
            std::ofstream out(cacheDir / ("з" + std::to_string(n) + ".md"), std::ios::binary);
            out << "![[к" << (n * 2) << ".png]]\n\n![[к" << (n * 2 + 1) << ".png]]\n";
        }

        // Свой редактор: кэш у каждого вида собственный, и картинки проверок
        // выше сбивали бы счёт.
        zametti::NoteEditor cacheEditor;
        cacheEditor.resize(600, 500);
        cacheEditor.show();
        QTest::qWait(20);
        // Пиксели берутся лениво, по первому рисованию: чтобы разжались все
        // картинки заметки, её надо прокрутить до конца — ровно как человек.
        const auto showAll = [](zametti::NoteEditor& view) {
            for (int at = 0; at <= view.verticalScrollBar()->maximum();
                 at += qMax(1, view.viewport()->height() / 2)) {
                view.verticalScrollBar()->setValue(at);
                view.repaint();
                QTest::qWait(0);
            }
            view.verticalScrollBar()->setValue(view.verticalScrollBar()->maximum());
            view.repaint();
            QTest::qWait(0);
        };
        const auto openNote = [&](int n) {
            cacheEditor.openFile(QString::fromStdString(
                (cacheDir / ("з" + std::to_string(n) + ".md")).string()));
            QTest::qWait(20);
            showAll(cacheEditor);
        };

        const int savedBudget = zametti::settings().cache().imageCacheSizeMb();
        // Бюджет 16 МБ: в кэш влезает шестнадцать мегабайтных картинок,
        // а всего их двадцать.
        zametti::mutableSettingsForTests().cache().setImageCacheSizeMb(16);
        zametti::ZApp::instance().applySettingsToCaches();
        // Кэш один на программу: другие проверки уже клали в него своё.
        zametti::ZApp::instance().images().clear();
        zametti::applyImageAllocationLimit();

        zametti::NoteView::resetImageDecodeCounters();
        openNote(0);
        ZT_EQ("показанные картинки разжаты", std::to_string(2),
              std::to_string(zametti::NoteView::imageDecodes()));
        ZT_EQ("в кэше две записи", std::to_string(2),
              std::to_string(cacheEditor.cachedImageCount()));
        ZT_EQ("вес кэша — две картинки", std::to_string(2 * kOne),
              std::to_string(cacheEditor.imageCacheBytes()));

        // Повторное открытие той же заметки не разжимает ничего.
        zametti::NoteView::resetImageDecodeCounters();
        openNote(0);
        ZT_EQ("повторное открытие не разжимает ничего", std::to_string(0),
              std::to_string(zametti::NoteView::imageDecodes()));

        for (int n = 1; n < 9; ++n) openNote(n);
        ZT_TRUE("кэш уложился в бюджет",
                cacheEditor.imageCacheBytes() <= 16 * 1024 * 1024);
        ZT_TRUE("вытеснение случилось: записей меньше, чем картинок",
                cacheEditor.cachedImageCount() < 18);
        zametti::NoteView::resetImageDecodeCounters();
        cacheEditor.repaint();
        QTest::qWait(0);
        ZT_EQ("картинки открытой заметки не вытеснены", std::to_string(0),
              std::to_string(zametti::NoteView::imageDecodes()));

        // Возврат в самую старую заметку: её картинки успели вытесниться.
        zametti::NoteView::resetImageDecodeCounters();
        openNote(0);
        ZT_EQ("вытесняется самое старое", std::to_string(2),
              std::to_string(zametti::NoteView::imageDecodes()));

        // Заметка тяжелее всего бюджета: показывается столько картинок,
        // сколько влезло, остальные — рамки с надписью. Грузятся они от самой
        // мелкой, поэтому влезает их столько, сколько вообще возможно.
        // Ниже восьми мегабайт бюджет не опускается — кэш на одну картинку
        // смысла не имеет, — поэтому заметка берётся из десяти.
        {
            std::ofstream out(cacheDir / "тяжёлая.md", std::ios::binary);
            for (int i = 10; i < 20; ++i) out << "![[к" << i << ".png]]\n\n";
        }
        zametti::mutableSettingsForTests().cache().setImageCacheSizeMb(1);   // упрётся в нижние 8 МБ
        zametti::ZApp::instance().applySettingsToCaches();
        // Кэш один на программу: другие проверки уже клали в него своё, а
        // прежний редактор с открытой заметкой защищает свои картинки от
        // вытеснения — прячем его и отпускаем его защиту, чтобы бюджет
        // достался одной заметке.
        cacheEditor.hide();
        zametti::ZApp::instance().images().forget(&cacheEditor);
        zametti::ZApp::instance().images().clear();
        zametti::NoteEditor heavy;
        heavy.resize(600, 500);
        heavy.show();
        QTest::qWait(20);
        heavy.openFile(QString::fromStdString((cacheDir / "тяжёлая.md").string()));
        QTest::qWait(20);
        ZT_EQ("все десять картинок заметки известны", std::to_string(10),
              std::to_string(heavy.cachedImageCount()));
        // Решение принято по размерам из заголовков, ДО всякого разжатия: две
        // самые крупные объявлены рамками, и разжимать их никто не станет.
        ZT_EQ("что не влезло в бюджет — рамки", std::to_string(2),
              std::to_string(heavy.framedImageCount()));
        for (int at = 0; at <= heavy.verticalScrollBar()->maximum(); at += 200) {
            heavy.verticalScrollBar()->setValue(at);
            heavy.repaint();
            QTest::qWait(0);
        }
        ZT_EQ("показано столько, сколько влезло", std::to_string(8),
              std::to_string(heavy.shownImageCount()));
        ZT_TRUE("и бюджет при этом не превышен",
                heavy.imageCacheBytes() <= 8 * 1024 * 1024);
        zametti::NoteView::resetImageDecodeCounters();
        heavy.repaint();
        QTest::qWait(0);
        ZT_EQ("и ни одна не разжимается заново", std::to_string(0),
              std::to_string(zametti::NoteView::imageDecodes()));

        zametti::mutableSettingsForTests().cache().setImageCacheSizeMb(savedBudget);
        zametti::ZApp::instance().applySettingsToCaches();
        // Кэш один на программу: другие проверки уже клали в него своё.
        zametti::ZApp::instance().images().clear();
        zametti::applyImageAllocationLimit();
    }

    // Картинка больше потолка: Qt её не разжимает, вместо неё рамка с
    // надписью. Место она занимает ровно то же, что заняла бы сама картинка —
    // иначе вёрстка прыгала бы от правки потолка в конфиге. Это и проверяем:
    // один и тот же файл при высоком потолке и при низком.
    {
        const fs::path bombDir = dir / "бомба";
        fs::create_directories(bombDir);
        QImage huge(1024, 768, QImage::Format_RGB32);
        huge.fill(QColor(30, 200, 30));
        ZT_TRUE("большая картинка записана",
                huge.save(QString::fromStdString((bombDir / "б.png").string())));
        {
            std::ofstream out(bombDir / "з.md", std::ios::binary);
            out << "![[б.png]]\n";
        }
        const QString note = QString::fromStdString((bombDir / "з.md").string());
        const int savedBudget = zametti::settings().cache().imageCacheSizeMb();
        // Порог высокий: картинка разжимается, место меряется по ней.
        zametti::mutableSettingsForTests().cache().setImageCacheSizeMb(512);
        zametti::ZApp::instance().applySettingsToCaches();
        // Кэш один на программу: другие проверки уже клали в него своё.
        zametti::ZApp::instance().images().clear();
        zametti::applyImageAllocationLimit();
        zametti::NoteEditor shown;
        shown.resize(600, 500);
        shown.show();
        QTest::qWait(20);
        shown.openFile(note);
        QTest::qWait(20);
        // Место меряем ВЫСОТОЙ БЛОКА, а не его нижним полем: полей у объекта
        // нет вовсе — его размер знает он сам, и в этом весь смысл перевода.
        const qreal roomForImage = shown.document()->documentLayout()->blockBoundingRect(
            shown.document()->findBlockByNumber(0)).height();
        ZT_TRUE("картинка разжалась и заняла место", roomForImage > 0.0);
        ZT_TRUE("и заняла вес в кэше", shown.imageCacheBytes() > 0);

        // Порог низкий: та же картинка отвергнута, место то же самое.
        // Четверть от восьми мегабайт — два, а картинка весит три.
        zametti::mutableSettingsForTests().cache().setImageCacheSizeMb(8);
        zametti::ZApp::instance().applySettingsToCaches();
        // Кэш один на программу: другие проверки уже клали в него своё.
        zametti::ZApp::instance().images().clear();
        zametti::applyImageAllocationLimit();
        zametti::NoteEditor refused;
        refused.resize(600, 500);
        refused.show();
        QTest::qWait(20);
        zametti::NoteView::resetImageDecodeCounters();
        refused.openFile(note);
        QTest::qWait(20);
        const qreal roomForFrame = refused.document()->documentLayout()->blockBoundingRect(
            refused.document()->findBlockByNumber(0)).height();
        // Рамка — не картинка: пропорций оригинала она не повторяет, а занимает
        // ровно столько, сколько нужно надписи. Показывать в ней всё равно
        // нечего, а 1x1000000 растянуло бы её на миллион пикселей.
        ZT_TRUE("рамка меньше самой картинки", roomForFrame < roomForImage);
        // Мерка — обычная строка текста, а не назначенная высота строки самого
        // блока: у блока с объектом Qt назначает её по объекту, и проверка
        // сравнивалась бы сама с собой.
        ZT_TRUE("но не вырождается: надпись в неё помещается",
                roomForFrame > 2 * QFontMetricsF(zametti::layoutBaseFont()).height());
        ZT_EQ("веса в кэше она не занимает", std::to_string(0),
              std::to_string(refused.imageCacheBytes()));

        const int decodes = zametti::NoteView::imageDecodes();
        refused.repaint();
        QTest::qWait(0);
        ZT_EQ("отказ не повторяется на каждом кадре", std::to_string(decodes),
              std::to_string(zametti::NoteView::imageDecodes()));

        zametti::mutableSettingsForTests().cache().setImageCacheSizeMb(savedBudget);
        zametti::ZApp::instance().applySettingsToCaches();
        // Кэш один на программу: другие проверки уже клали в него своё.
        zametti::ZApp::instance().images().clear();
        zametti::applyImageAllocationLimit();
    }

    // Вырожденные пропорции. Предел стороны держит ОБЕ стороны, поэтому худший
    // случай в кэше — квадрат limit x limit, а не лента: 1x1000000 занимает
    // столько же, сколько 1x1024. И место в документе тоже ограничено — без
    // этого замер на 1x20000 давал 19984 px поля под одну строку и документ
    // высотой в двадцать тысяч пикселей.
    {
        const fs::path thinDir = dir / "вырожденные";
        fs::create_directories(thinDir);
        QImage thin(1, 20000, QImage::Format_RGB32);
        thin.fill(QColor(200, 40, 40));
        ZT_TRUE("тонкая картинка записана",
                thin.save(QString::fromStdString((thinDir / "тонкая.png").string())));
        QImage wide(20000, 1, QImage::Format_RGB32);
        wide.fill(QColor(40, 200, 40));
        ZT_TRUE("широкая картинка записана",
                wide.save(QString::fromStdString((thinDir / "широкая.png").string())));
        {
            std::ofstream out(thinDir / "з.md", std::ios::binary);
            out << "![[тонкая.png]]\n\n![[широкая.png]]\n";
        }

        const int savedLimit = zametti::settings().cache().maxLoadedImageSize();
        zametti::mutableSettingsForTests().cache().setMaxLoadedImageSize(1024);
        zametti::ZApp::instance().applySettingsToCaches();
        // Кэш один на программу: другие проверки уже клали в него своё.
        zametti::ZApp::instance().images().clear();
        zametti::NoteEditor thinEditor;
        thinEditor.resize(600, 500);
        thinEditor.show();
        QTest::qWait(20);
        thinEditor.openFile(QString::fromStdString((thinDir / "з.md").string()));
        QTest::qWait(20);

        const qreal thinRoom = thinEditor.document()->documentLayout()->blockBoundingRect(
            thinEditor.document()->findBlockByNumber(0)).height();
        ZT_TRUE("под ленту 1x20000 отведено место, а не двадцать тысяч пикселей",
                thinRoom > 0.0 && thinRoom < 4.0 * 500 + 50);
        // Худший случай в кэше — квадрат стороной в предел, и обе вырожденные
        // вместе до него не дотягивают.
        ZT_TRUE("вырожденные картинки в кэше — не лента, а мелочь",
                thinEditor.imageCacheBytes() < qint64(1024) * 1024 * 4);

        zametti::mutableSettingsForTests().cache().setMaxLoadedImageSize(savedLimit);
        zametti::ZApp::instance().applySettingsToCaches();
        // Кэш один на программу: другие проверки уже клали в него своё.
        zametti::ZApp::instance().images().clear();
    }

    // Выравнивание фотографии в колонке. Умолчание — по центру, и в файл ради
    // него не пишется ничего: заметка не обязана хранить незаданное.
    {
        const fs::path alignDir = dir / "выравнивание";
        fs::create_directories(alignDir);
        QImage small(80, 60, QImage::Format_RGB32);
        small.fill(QColor(40, 40, 220));
        ZT_TRUE("картинка выравнивания записана",
                small.save(QString::fromStdString((alignDir / "м.png").string())));
        {
            std::ofstream out(alignDir / "з.md", std::ios::binary);
            out << "![[м.png]]\n\n![подпись](м.png)\n";
        }

        zametti::NoteEditor aligned;
        aligned.resize(700, 500);
        aligned.show();
        QTest::qWait(20);
        aligned.openFile(QString::fromStdString((alignDir / "з.md").string()));
        QTest::qWait(20);
        const auto blockOf = [&](int n) { return aligned.document()->findBlockByNumber(n); };
        const auto photoOf = [&](int n) { return aligned.imageRectInViewport(blockOf(n)); };
        const auto setAlign = [&](int n, zametti::ImageAlign to) {
            QTextCursor cursor(blockOf(n));
            const bool changed = aligned.note().setImageAlign(cursor, to);
            QTest::qWait(10);
            return changed;
        };

        // Умолчание: фотография стоит посередине колонки, а не у левого края.
        const QRectF centred = photoOf(0);
        ZT_TRUE("по умолчанию фотография по центру", centred.left() > 40.0);
        ZT_EQ("и в тексте про выравнивание ничего не написано",
              std::string("![[м.png]]"), sourceIn(aligned, 0).toStdString());

        ZT_TRUE("выравнивание влево принято", setAlign(0, zametti::ImageAlign::Left));
        const QRectF left = photoOf(0);
        ZT_TRUE("слева фотография уехала левее центра", left.left() < centred.left() - 20.0);
        ZT_EQ("выравнивание записано рядом с шириной",
              std::string("![[м.png|align=left]]"), sourceIn(aligned, 0).toStdString());

        ZT_TRUE("выравнивание вправо принято", setAlign(0, zametti::ImageAlign::Right));
        const QRectF right = photoOf(0);
        ZT_TRUE("справа фотография уехала правее центра", right.left() > centred.left() + 20.0);

        // ПОЛОСА ОБЪЕКТА — ВО ВСЮ ШИРИНУ КОЛОНКИ, и в этом весь замысел: Qt
        // отводит фотографии всю ширину, а куда её поставить внутри полосы,
        // решаем мы сами.
        //
        // Спрашиваем об этом САМУ Qt: naturalTextWidth строки — это ширина
        // того единственного знака, который в ней стоит, то есть объявленный
        // нами размер объекта. Меряя вместо этого положение снимка, проверка
        // спрашивала бы наше же вычисление и не могла бы покраснеть вовсе —
        // на чём я и попался: подменил ширину полосы, а набор не заметил.
        const QTextBlock block = blockOf(0);
        const QTextLayout* layout = block.layout();
        const qreal claimed =
            layout != nullptr && layout->lineCount() > 0 ? layout->lineAt(0).naturalTextWidth()
                                                         : 0.0;
        const qreal column =
            aligned.document()->documentLayout()->blockBoundingRect(block).width();
        ZT_TRUE("объект занял всю ширину колонки (" + std::to_string(int(claimed)) + " из " +
                    std::to_string(int(column)) + ")",
                claimed > 1.0 && std::fabs(claimed - column) < 2.0);
        ZT_EQ("и записано так же", std::string("![[м.png|align=right]]"),
              sourceIn(aligned, 0).toStdString());

        // Возврат к умолчанию стирает запись, а не пишет "align=center".
        ZT_TRUE("возврат к центру принят", setAlign(0, zametti::ImageAlign::Center));
        ZT_EQ("умолчание в файл не пишется", std::string("![[м.png]]"),
              sourceIn(aligned, 0).toStdString());
        ZT_TRUE("и фотография вернулась на середину",
                std::fabs(photoOf(0).left() - centred.left()) < 1.5);

        // Ширина и выравнивание уживаются рядом и не сбивают друг друга.
        {
            QTextCursor cursor(blockOf(0));
            ZT_TRUE("ширина записана", aligned.note().setImageWidth(cursor, 50));
        }
        QTest::qWait(10);
        setAlign(0, zametti::ImageAlign::Right);
        ZT_EQ("ширина и выравнивание стоят рядом",
              std::string("![[м.png|50|align=right]]"), sourceIn(aligned, 0).toStdString());
        {
            QTextCursor cursor(blockOf(0));
            ZT_TRUE("ширина меняется, выравнивание цело",
                    aligned.note().setImageWidth(cursor, 60));
        }
        QTest::qWait(10);
        ZT_EQ("обе величины на месте", std::string("![[м.png|60|align=right]]"),
              sourceIn(aligned, 0).toStdString());

        // Image-спан: то же самое, но во фрагменте пути.
        setAlign(2, zametti::ImageAlign::Left);
        {
            const std::vector<zametti::Piece> ir = blocksOf(*aligned.document());
            const std::string out = markdownOf(ir);
            ZT_TRUE("у image-спана выравнивание уходит во фрагмент пути",
                    out.find("![подпись](м.png#align=left)") != std::string::npos);
        }
        {
            QTextCursor cursor(blockOf(2));
            aligned.note().setImageWidth(cursor, 70);
        }
        QTest::qWait(10);
        {
            const std::vector<zametti::Piece> ir = blocksOf(*aligned.document());
            const std::string out = markdownOf(ir);
            ZT_TRUE("ширина встаёт рядом с выравниванием",
                    out.find("![подпись](м.png#w=70&align=left)") != std::string::npos);
        }
    }

    // Картинка в ПОСЛЕДНЕЙ строке заметки: нижнее поле последнего блока Qt в
    // высоту документа не берёт вовсе (замер: поле 500 даёт +0, а такое же
    // поле рамки даёт +500). Прокрутка из-за этого кончалась раньше картинки,
    // и снизу было видно только её верхушку.
    {
        const fs::path tailDir = dir / "последняя";
        fs::create_directories(tailDir);
        QImage tall(300, 900, QImage::Format_RGB32);
        tall.fill(QColor(220, 120, 40));
        ZT_TRUE("высокая картинка записана",
                tall.save(QString::fromStdString((tailDir / "в.png").string())));
        {
            std::ofstream out(tailDir / "з.md", std::ios::binary);
            out << "# заметка\n\nтекст\n\n![[в.png]]\n";
        }

        zametti::NoteEditor tailEditor;
        tailEditor.resize(600, 400);
        tailEditor.show();
        QTest::qWait(20);
        tailEditor.openFile(QString::fromStdString((tailDir / "з.md").string()));
        QTest::qWait(30);

        const QTextBlock lastBlock = tailEditor.document()->lastBlock();
        ZT_TRUE("под картинку в последней строке отведено место",
                tailEditor.document()->documentLayout()->blockBoundingRect(lastBlock).height() >
                    2 * QFontMetricsF(zametti::layoutBaseFont()).height());

        tailEditor.verticalScrollBar()->setValue(tailEditor.verticalScrollBar()->maximum());
        QTest::qWait(20);
        const QRectF photo = tailEditor.imageRectInViewport(lastBlock);
        ZT_TRUE("прокрутив до упора, картинку видно целиком",
                !photo.isEmpty() && photo.bottom() <= tailEditor.viewport()->height() + 1.0);
    }

    checkMissingAttachment();
    checkEmptyCaption();
    checkNonameCaption();
    checkCaptionEditing();
    checkTypingAfterImage();

    return zt::report("картинки в просмотре");
}

// Набор целиком одним TEST: тело не тронуто, argv ему собран здесь.
// Дробить на отдельные проверки — отдельная работа, по одному набору.
// КРУГ С ОБЪЕКТОМ ОТ ПОКАЗА НЕ ЗАВИСИТ, и потому идёт всегда: рисуют ли
// фотографию на экране — дело вида, а вот то, что в файл уходит исходник, а не
// U+FFFC, обязано проверяться при любой настройке.
TEST(Image, ObjectRoundTrip) {
    checkImageObjectRoundTrip();
    EXPECT_EQ(0, zt::g_failures);
}

TEST(Image, All) {
    std::vector<QByteArray> ztArgs{QByteArrayLiteral("image_test")};
    std::vector<char*> ztArgv;
    for (QByteArray& a : ztArgs) ztArgv.push_back(a.data());
    EXPECT_EQ(0, ztRunSuite(int(ztArgv.size()), ztArgv.data()));
}
