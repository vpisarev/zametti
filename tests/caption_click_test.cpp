// ЩЕЛЧОК ПО ПОДПИСИ КАРТИНКИ ОТКРЫВАЕТ ЕЁ ПРАВКУ (просьба владельца), и
// каретка в этом поле — та же, что в заметке: цвет и толщина из настроек.
//
// Спрашивается настоящим нажатием мыши: между «функция открывает поле» и «по
// щелчку открывается поле» лежит вся обработка мыши, и ломалось у нас именно
// там (тот же довод, что у кнопки копирования в CodeShots).

#include "caption_editor.h"
#include "doc_model.h"
#include "editor_widget.h"
#include "settings.h"

#include "test_util.h"

#include <QDir>
#include <QFile>
#include <QImage>
#include <QScrollBar>
#include <QTemporaryDir>
#include <QTest>
#include <QTextBlock>

#include <string>

namespace {

std::string s(const QString& text) { return text.toStdString(); }

int blockOfPhoto(const QTextDocument& doc) {
    for (QTextBlock block = doc.begin(); block.isValid(); block = block.next())
        if (zametti::blockImageRef(block).valid) return block.blockNumber();
    return -1;
}

// Вид с настоящей картинкой и подписью под ней.
class Peek : public zametti::NoteEditor {
public:
    QRectF captionRect(const QTextBlock& block) { return imageCaptionRectInViewport(block); }
    QRectF photoRect(const QTextBlock& block) { return imageRectInViewport(block); }
    void clickAt(const QPointF& at) {
        QTest::mouseClick(viewport(), Qt::LeftButton, Qt::NoModifier, at.toPoint());
        QTest::qWait(20);
    }
};

}  // namespace

TEST(CaptionClick, All) {
    QTemporaryDir tmp;
    const QString dir = tmp.path();
    QImage picture(160, 100, QImage::Format_RGB32);
    picture.fill(Qt::darkCyan);
    ZT_TRUE("картинка записана", picture.save(QDir(dir).filePath(QStringLiteral("kadr.png"))));

    const QString path = QDir(dir).filePath(QStringLiteral("заметка.md"));
    {
        QFile file(path);
        ZT_TRUE("заметка записана", file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write("# Заголовок\n\n![Вид с балкона](kadr.png)\n\nхвост\n");
    }

    Peek editor;
    editor.resize(700, 600);
    editor.show();
    QTest::qWait(20);
    editor.openFile(path);
    QTest::qWait(40);

    const int number = blockOfPhoto(*editor.document());
    ZT_TRUE("картинка нашлась", number >= 0);
    const QTextBlock block = editor.document()->findBlockByNumber(number);
    ZT_EQ("подпись показана", std::string("Вид с балкона"),
          s(zametti::blockImageRef(block).shownCaption()));

    const QRectF caption = editor.captionRect(block);
    const QRectF photo = editor.photoRect(block);
    ZT_TRUE("место подписи известно", !caption.isEmpty());
    ZT_TRUE("подпись под снимком", caption.top() >= photo.bottom() - 0.5);

    // 1. Щелчок ПО СНИМКУ поля не открывает: там выбор картинки.
    editor.clickAt(photo.center());
    ZT_TRUE("щелчок по снимку поля не открыл", editor.imageCaptionEditor() == nullptr);
    ZT_EQ("но картинку выбрал", std::to_string(number),
          std::to_string(editor.textCursor().blockNumber()));

    // 2. Щелчок ПО ПОДПИСИ открывает поле, и в нём — та самая подпись.
    editor.clickAt(caption.center());
    zametti::CaptionEditor* field = editor.imageCaptionEditor();
    ZT_TRUE("щелчок по подписи открыл поле", field != nullptr);
    if (field == nullptr) return;
    ZT_EQ("в поле — подпись картинки", std::string("Вид с балкона"), s(field->text()));
    ZT_TRUE("и поле взяло фокус", field->hasFocus());

    // 3. КАРЕТКА В ПОЛЕ — ЦВЕТА caretColor И ТОЛЩИНОЙ caretWidth (та же, что в
    // заметке). Снимок поля сразу после того, как каретку разбудили ходом.
    field->setCursorPosition(3);
    QCoreApplication::processEvents();
    const QColor want = zametti::settings().style().caretColor();
    const int wide = qMax(1, qRound(zametti::settings().style().caretWidth()));
    const QImage shot = field->grab().toImage();
    // Место каретки ищем по снимку: cursorRect() у QLineEdit закрыт, а искать
    // пиксели нужного цвета честнее — так проверяется то, что человек ВИДИТ.
    int run = 0;
    int found = 0;
    const int y = shot.height() / 2;
    for (int x = 0; x < shot.width(); ++x) {
        if (shot.pixelColor(x, y) != want) {
            if (run > 0) break;
            continue;
        }
        ++run;
        found = x;
    }
    (void)found;
    ZT_EQ("каретка цвета caretColor и толщиной caretWidth", std::to_string(wide),
          std::to_string(run));

    // 3а. И ЧУЖОЙ КАРЕТКИ РЯДОМ НЕТ — НИ В ОДНОЙ ФАЗЕ МИГАНИЯ. Владелец
    // увидел в первой редакции ровно это: «мигают обе, тонкая штатная и
    // цветная, с рассинхроном». Поле сделано на QPlainTextEdit ради
    // setCursorWidth(0), а колонка каретки ещё и перерисовывается поверх —
    // на дробном масштабе экрана нулевая ширина становится физическим
    // пикселем. Смотрим серию кадров: тёмных пикселей в поле нет ни разу.
    {
        int frames = 0;
        int litFrames = 0;
        int strangers = 0;
        for (int i = 0; i < 12; ++i) {
            QTest::qWait(120);
            const QImage frame = field->grab().toImage();
            const int row = frame.height() / 2;
            int lit = 0;
            int dark = 0;
            for (int x = 0; x < frame.width(); ++x) {
                const QColor c = frame.pixelColor(x, row);
                if (c == want) ++lit;
                else if (c.lightness() < 90) ++dark;
            }
            ++frames;
            if (lit > 0) ++litFrames;
            strangers += dark;
        }
        ZT_EQ("чужой каретки нет ни в одном кадре", std::string("0"), std::to_string(strangers));
        ZT_TRUE("а своя мигает: горит не всегда и не никогда",
                litFrames > 0 && litFrames < frames);
    }

    // 4. Подпись правится и уходит в заметку — поле работает, а не только видно.
    field->setText(QStringLiteral("Балкон, вечер"));
    QTest::keyClick(field, Qt::Key_Return);
    QTest::qWait(20);
    ZT_TRUE("поле закрылось", editor.imageCaptionEditor() == nullptr);
    const QTextBlock after = editor.document()->findBlockByNumber(number);
    ZT_EQ("подпись в заметке новая", std::string("Балкон, вечер"),
          s(zametti::blockImageRef(after).shownCaption()));
}
