// Подпись картинки: какую показывать, какую прятать, как править.
//
// Решение владельца (17–18.08.2026): «хорошие имена показывать, дурацкие
// скрывать/переименовывать в то, что не показывается». Три предмета:
//   * ПРАВИЛО isNonameCaption — матрица имён от камеры, спрятанных знаком и
//     настоящих подписей;
//   * ГЛАГОЛЫ заметки setImageCaption / toggleImageCaption — правят объект, а
//     в файл уходит «![подпись](путь)», и картинка не теряется ни при пустой
//     подписи, ни при спрятанной;
//   * СЛОЙ ОБЪЕКТОВ — сочетание переключения на картинке значит «спрятать
//     подпись», в тексте слой молчит.

#include "block_object.h"
#include "doc_model.h"
#include "document.h"
#include "settings.h"
#include "test_util.h"

#include <QTextBlock>
#include <QTextCursor>

#include <string>
#include <vector>

namespace {

using zametti::ObjectAction;
using zametti::ObjectContext;
using zametti::ZDocument;

std::string s(const QString& text) { return text.toStdString(); }

// --- правило -----------------------------------------------------------------

void checkRule() {
    // Безымянные: имя от камеры, телефона, буфера обмена; спрятанные знаком;
    // пустые. Регистр не важен.
    const char* const noname[] = {
        "",           "   ",         "image",     "Image 1",   "image 12",
        "IMG_1234",   "img_20240101_123456", "DSC343453_DXO", "DSC_0042.JPG",
        "dsc0042",    "PXL_20240101_123456789.jpg", "photo_2024-01-01_12-00-00",
        "Изображение", "изображение 2", "Снимок экрана 2024-01-01 в 12.00.00",
        "Screenshot 2024-01-01 at 12.00.00", "Screen Shot 2024-01-01 at 12.00.00",
        "Pasted image 20240101120000", "pic", "picture 2", "untitled", "Unnamed", "clipboard",
        "~Вид на море", "~", "-старая подпись", "~IMG_1234",
        // Имя от камеры без служебного слова и голое число из мессенджера.
        "0A5A0229_DxO", "P1010234", "DJI_0042", "GOPR1234.JPG", "MVI_1234", "6850343268",
        "VID_20240101_120000",
    };
    for (const char* text : noname)
        ZT_TRUE(std::string("безымянная: «") + text + "»",
                zametti::isNonameCaption(QString::fromUtf8(text)));

    // Настоящие подписи: слово живого языка после служебного, своё имя.
    const char* const real[] = {
        "Вид на море",   "image of the dog", "photo from Paris", "Схема установки",
        "picture 3 — закат", "IMG_1234 у причала", "фотография 3", "images", "imaginary",
        "Домик", "2024",  "1", "Screenshot of the bug", "фото Маши", "dscription",
        "iPhone15", "Nokia3310", "room101", "Red-Black Tree", "intro-scalable-arch",
        "— тире",        "Море ~ волны",
        // Русские слова человек пишет сам — это подпись, а не имя от машины
        // (на «фото» и «снимок» стоят и другие наборы).
        "фото", "снимок", "картинка", "Фото 7", "Картинка 3", "снимок с балкона",
    };
    for (const char* text : real)
        ZT_TRUE(std::string("настоящая: «") + text + "»",
                !zametti::isNonameCaption(QString::fromUtf8(text)));

    // Переключение знаком: «подпись» ⇄ «~подпись»; спрятанная любым из двух
    // знаков возвращается снятием знака.
    ZT_EQ("спрятать", "~Вид", s(zametti::captionWithHidingToggled(QStringLiteral("Вид"))));
    ZT_EQ("вернуть", "Вид", s(zametti::captionWithHidingToggled(QStringLiteral("~Вид"))));
    ZT_EQ("вернуть спрятанную минусом", "Вид",
          s(zametti::captionWithHidingToggled(QStringLiteral("-Вид"))));
    ZT_EQ("спрятать минусом", "-Вид",
          s(zametti::captionWithHidingToggled(QStringLiteral("Вид"), QLatin1Char('-'))));

    // Регэксп из настроек: битый образец правило не меняет — проверяется на
    // чтении конфига, а здесь — что подмена образца правило и меняет.
    const QRegularExpression saved = zametti::appearance().imageNonameCaption;
    zametti::appearance().imageNonameCaption =
        QRegularExpression(QStringLiteral("^дурацк.*$"), QRegularExpression::CaseInsensitiveOption);
    ZT_TRUE("свой регэксп: «дурацкая» безымянна",
            zametti::isNonameCaption(QStringLiteral("Дурацкая подпись")));
    ZT_TRUE("свой регэксп: «IMG_1234» больше не безымянна",
            !zametti::isNonameCaption(QStringLiteral("IMG_1234")));
    ZT_TRUE("знак спереди прячет и при своём регэкспе",
            zametti::isNonameCaption(QStringLiteral("~IMG_1234")));
    zametti::appearance().imageNonameCaption = saved;
}

// --- глаголы -----------------------------------------------------------------

int imageBlockOf(ZDocument& note) {
    for (int i = 0; i < note.blockCount(); ++i)
        if (zametti::blockImageRef(note.caretAtBlock(i).block()).valid) return i;
    return -1;
}

zametti::BlockImageRef imageAt(ZDocument& note, int block) {
    return zametti::blockImageRef(note.caretAtBlock(block).block());
}

void checkVerbs() {
    ZDocument note;
    note.loadMarkdown("текст\n\n![Вид на море](x.png)\n\nхвост\n");
    const int block = imageBlockOf(note);
    ZT_TRUE("картинка найдена", block >= 0);
    if (block < 0) return;

    QTextCursor at = note.caretAtBlock(block);
    ZT_TRUE("подпись правится", note.setImageCaption(at, QStringLiteral("Море")));
    ZT_EQ("новая подпись в файле", "текст\n\n![Море](x.png)\n\nхвост\n", note.toMarkdown());
    ZT_EQ("каретка осталась на картинке", std::to_string(block), std::to_string(at.blockNumber()));
    ZT_TRUE("та же подпись — не правка", !note.setImageCaption(at, QStringLiteral("Море")));

    // ПУСТАЯ ПОДПИСЬ ЗАКОННА: картинка остаётся картинкой (владелец: «![](…)
    // мы в любом случае обязаны сохранять»).
    ZT_TRUE("пустая подпись ставится", note.setImageCaption(at, QString()));
    ZT_EQ("картинка с пустой подписью в файле", "текст\n\n![](x.png)\n\nхвост\n",
          note.toMarkdown());
    ZT_TRUE("и она по-прежнему объект-картинка", imageAt(note, block).valid);
    ZT_TRUE("прятать пустую нечего", !note.toggleImageCaption(at));
    ZT_EQ("файл не тронут", "текст\n\n![](x.png)\n\nхвост\n", note.toMarkdown());

    // Перевод строки в подписи становится пробелом: иначе «![a\nb](x)» в файле
    // развалил бы картинку.
    ZT_TRUE("подпись с переводом строки", note.setImageCaption(at, QStringLiteral("две\nстроки")));
    ZT_EQ("перевод строки стал пробелом", "текст\n\n![две строки](x.png)\n\nхвост\n",
          note.toMarkdown());

    // Спрятать и вернуть: знак `~` спереди, подпись цела.
    ZT_TRUE("спрятать", note.toggleImageCaption(at));
    ZT_EQ("спрятанная — в файле со знаком", "текст\n\n![~две строки](x.png)\n\nхвост\n",
          note.toMarkdown());
    ZT_TRUE("под снимком её не показывают", imageAt(note, block).shownCaption().isEmpty());
    ZT_TRUE("вернуть", note.toggleImageCaption(at));
    ZT_EQ("возвращённая — как была", "текст\n\n![две строки](x.png)\n\nхвост\n",
          note.toMarkdown());
    ZT_EQ("и снова показана", "две строки", s(imageAt(note, block).shownCaption()));

    // Не на картинке — ложь и ничего не трогает.
    QTextCursor text = note.caretAtBlock(0);
    ZT_TRUE("на тексте подписи нет", !note.setImageCaption(text, QStringLiteral("x")));
    ZT_TRUE("и прятать нечего", !note.toggleImageCaption(text));

    // Вики-вложение подписи не имеет.
    ZDocument wiki;
    wiki.loadMarkdown("![[в.png|300]]\n");
    QTextCursor onWiki = wiki.caretAtBlock(0);
    ZT_TRUE("вики-вложение опознано", imageAt(wiki, 0).valid);
    ZT_TRUE("у вики-вложения подписи нет", !wiki.setImageCaption(onWiki, QStringLiteral("x")));
    ZT_EQ("вики-вложение не тронуто", "![[в.png|300]]\n", wiki.toMarkdown());

    // Круг через файл: спрятанная подпись читается назад спрятанной.
    ZDocument again;
    again.loadMarkdown("![~IMG_1234](x.png)\n");
    ZT_EQ("спрятанная подпись переживает чтение", "![~IMG_1234](x.png)\n", again.toMarkdown());
    ZT_TRUE("и прочитана как спрятанная", imageAt(again, 0).shownCaption().isEmpty());
    ZT_EQ("alt при этом цел", "~IMG_1234", s(imageAt(again, 0).alt));
}

// --- слой объектов -----------------------------------------------------------

void checkObjectRule() {
    // Сочетание переключения на объекте — спрятать подпись; вне объекта слой
    // молчит (в тексте оно переключает задачу — не его дело); при выделении
    // молчит всегда.
    int cells = 0;
    int toggles = 0;
    for (int mask = 0; mask < 512; ++mask) {
        ObjectContext where;
        where.onObject = (mask & 1) != 0;
        where.hasSelection = (mask & 2) != 0;
        where.atBlockStart = (mask & 4) != 0;
        where.atBlockEnd = (mask & 8) != 0;
        where.objectAbove = (mask & 16) != 0;
        where.objectBelow = (mask & 32) != 0;
        where.objectAboveGap = (mask & 64) != 0;
        where.objectBelowGap = (mask & 128) != 0;
        where.onGap = (mask & 256) != 0;
        where.toggleKey = true;
        const ObjectAction action = zametti::actionFor(Qt::Key_Space, Qt::ControlModifier, where);
        ++cells;
        if (where.hasSelection || !where.onObject)
            ZT_TRUE("переключение вне объекта (или при выделении) — не дело слоя",
                    action == ObjectAction::None);
        else
            ZT_TRUE("переключение на объекте — подпись", action == ObjectAction::ToggleCaption);
        if (action == ObjectAction::ToggleCaption) ++toggles;
    }
    ZT_EQ("клеток", "512", std::to_string(cells));
    ZT_EQ("подпись переключается ровно на объекте без выделения", "128", std::to_string(toggles));

    // Без признака сочетания та же клавиша на объекте — ничего.
    ObjectContext on;
    on.onObject = true;
    ZT_TRUE("Ctrl+Space без признака — не подпись",
            zametti::actionFor(Qt::Key_Space, Qt::ControlModifier, on) == ObjectAction::None);
}

}  // namespace

TEST(Caption, All) {
    checkRule();
    checkVerbs();
    checkObjectRule();
    EXPECT_EQ(0, zt::g_failures);
}
