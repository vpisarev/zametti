// Редактирование списков — целиком, от клавиши до файла.
//
// Списки в заметках правят чаще всего остального, и ошибка здесь дороже любой
// другой. Проверка идёт живым набором в настоящем редакторе, а не вызовом
// операций: одна и та же правка операции может быть верной, а через клавишу
// давать не то — курсор оказывается не там, обработчик зовёт не ту операцию,
// пересборка сбивает место. Ровно так и вышло с Enter в начале пункта: сама
// операция была разумной, а вставку пункта между двумя она сломала.
//
// Каждый случай — это: заметка, куда встать, что нажать, что должно выйти и где
// оказаться курсору. Проверяется и содержимое, и место курсора: половина
// правил про списки — это как раз про то, куда он встаёт.

#include "doc_model.h"
#include "document_reader.h"
#include "document_saver.h"
#include "editor_widget.h"
#include "parser.h"
#include "serializer.h"
#include "settings.h"

#include "test_util.h"

#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QKeySequence>
#include <QScrollBar>
#include <QTest>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

fs::path g_dir;

QString writeNote(const std::string& name, const QString& text) {
    const fs::path path = g_dir / name;
    std::ofstream out(path, std::ios::binary);
    const QByteArray bytes = text.toUtf8();
    out.write(bytes.constData(), bytes.size());
    return QString::fromStdString(path.string());
}

QString textOf(const zametti::NoteEditor& editor) {
    return QString::fromStdString(
        zametti::serialize(zametti::readDocument(*editor.document())));
}

// Нажатие в виде «что человек делает»: сочетание клавиш или набор текста.
struct Press {
    const char* keys;    // как в настройках: "Return", "Ctrl+3", "Shift+Tab"
    const char* typed;   // либо просто набрать это
};

Press key(const char* keys) { return {keys, nullptr}; }
Press type(const char* text) { return {nullptr, text}; }

struct Case {
    const char* what;
    const char* source;
    int block;            // куда встать: номер блока
    int offset;           // и смещение в нём
    std::vector<Press> presses;
    const char* expected;
    int cursorBlock;      // в каком блоке обязан оказаться курсор; -1 — не проверяем
};

void run(const Case& c) {
    const QString path = writeNote(std::string(c.what) + ".md", QString::fromUtf8(c.source));

    zametti::NoteEditor editor;
    editor.resize(700, 500);
    editor.show();
    QTest::qWait(20);
    editor.setFocus();
    editor.openFile(path);
    QTest::qWait(20);

    QTextCursor cursor = editor.textCursor();
    const QTextBlock start = editor.document()->findBlockByNumber(c.block);
    if (!start.isValid()) {
        ZT_TRUE(std::string(c.what) + ": блока " + std::to_string(c.block) + " нет", false);
        return;
    }
    cursor.setPosition(start.position() + qMin(c.offset, start.length() - 1));
    editor.setTextCursor(cursor);

    for (const Press& press : c.presses) {
        if (press.typed != nullptr) {
            editor.insertPlainText(QString::fromUtf8(press.typed));
        } else {
            const QKeySequence sequence(QString::fromLatin1(press.keys),
                                        QKeySequence::PortableText);
            const QKeyCombination combo = sequence[0];
            QTest::keyClick(&editor, combo.key(), combo.keyboardModifiers());
        }
        QTest::qWait(10);
    }

    ZT_EQ(c.what, std::string(c.expected), textOf(editor).toStdString());
    if (c.cursorBlock >= 0)
        ZT_EQ(std::string(c.what) + ": курсор в блоке", std::to_string(c.cursorBlock),
              std::to_string(editor.textCursor().blockNumber()));
}

// Вставка и выход. Здесь важнее всего место курсора: печатать человек будет
// сразу после нажатия, и если курсор не там, правка уедет не туда.
const Case kEnterCases[] = {
    {"пункт между двумя",
     "- раз\n- два\n- три\n",
     1, 0, {key("Return"), type("новый")},
     "- раз\n- новый\n- два\n- три\n", 1},

    {"пункт перед первым",
     "- раз\n- два\n",
     0, 0, {key("Return"), type("ноль")},
     "- ноль\n- раз\n- два\n", 0},

    {"пункт после текущего",
     "- раз\n- два\n",
     0, 5, {key("Return"), type("полтора")},
     "- раз\n- полтора\n- два\n", 1},

    {"разрез пункта посередине",
     "- разъдва\n",
     0, 4, {key("Return")},
     "- разъ\n- два\n", 1},

    {"выход из списка в конце",
     "- раз\n- два\n",
     1, 5, {key("Return"), key("Return"), type("абзац")},
     "- раз\n- два\n\nабзац\n", 3},

    // Разрезает абзац только Enter на ПУСТОЙ строке — пустой целиком, и до
    // курсора, и после. В начале строки с текстом он переносит строку, и десять
    // нажатий дают десять пустых строк, а не пять абзацев.
    {"Enter в конце абзаца и на пустой строке разрезает",
     "текст\n",
     0, 5, {key("Return"), key("Return"), type("второй")},
     "текст\n\nвторой\n", 2},

    // Три пустые строки над текстом, и всё это один абзац: сравнение идёт по
    // содержимому документа, а в файле такие строки станут неразрывным пробелом.
    {"Enter в начале строки с текстом только переносит",
     "текст\n",
     0, 0, {key("Return"), key("Return"), key("Return")},
     "\n\n\nтекст\n", 0},

    // Разлепить два слипшихся списка: выйти из списка и набрать разделяющий
    // абзац. Соседей при этом никто не трогает — раньше пустой пункт снимал
    // маркер со СЛЕДУЮЩЕГО, и два Enter в середине списка отнимали чекбокс у
    // пункта, которого человек не касался.
    //
    // Пустой абзац для разрыва не годится, и это проверено на ядре: пустая
    // строка между пунктами не разделяет ничего. Разделяет только абзац с
    // содержимым — потому в случае и набирается текст.
    {"разрыв списка посередине",
     "- раз\n- два\n- три\n",
     1, 0, {key("Return"), key("Return"), type("разрыв")},
     "- раз\n\nразрыв\n- два\n- три\n", 2},

    {"разрыв держится после записи",
     "- раз\n- задачки:\n- [ ] дело\n",
     1, 0, {key("Return"), key("Return"), type("разрыв"), key("Ctrl+S")},
     "- раз\n\nразрыв\n- задачки:\n- [ ] дело\n", 2},

    {"новая задача невыполненная",
     "- [x] сделано\n",
     0, 8, {key("Return"), type("ещё")},
     "- [x] сделано\n- [ ] ещё\n", 1},

    {"вложенный пункт продолжается вложенным",
     "- верх\n  - вложенный\n",
     1, 11, {key("Return"), type("сосед")},
     "- верх\n  - вложенный\n  - сосед\n", 2},
};

// Слияние и снятие списка.
const Case kBackspaceCases[] = {
    {"пункт сливается с предыдущим",
     "- раз\n- два\n",
     1, 0, {key("Backspace")},
     "- раздва\n", 0},

    {"первый пункт перестаёт быть пунктом",
     "- раз\n- два\n",
     0, 0, {key("Backspace")},
     "раз\n- два\n", 0},

    {"пустой пункт исчезает",
     "- раз\n- \n",
     1, 0, {key("Backspace")},
     "- раз\n", 0},

    // Сливаемся только со своим списком: тот же вид маркера и тот же уровень.
    // С чужим — снимаем маркер, а строение не рушим. Слияние буллета с задачей
    // давало "- [ ] задачабуллет", и человек получал это, всего лишь пытаясь
    // отменить только что поставленный маркер.
    {"задача не сливается с буллетом",
     "- буллет\n- [ ] задача\n",
     1, 0, {key("Backspace")},
     "- буллет\n\nзадача\n", 2},

    {"вложенный не сливается с родителем",
     "- верх\n  - вложенный\n",
     1, 0, {key("Backspace")},
     "- верх\n\nвложенный\n", 2},

    {"нумерованный не сливается с буллетом",
     "- буллет\n1. номер\n",
     1, 0, {key("Backspace")},
     "- буллет\n\nномер\n", 2},

    // Круг «поставил маркер — снял маркер» ничего не портит: это и есть та
    // отмена, ради которой Backspace жмут сразу после автозамены.
    {"маркер поставили и сняли",
     "- [ ] задача\n\nдо 19 июля:\n",
     2, 0, {type("*"), key("Space"), key("Backspace")},
     "- [ ] задача\n\nдо 19 июля:\n", 2},
};

// Уровни. Поддерево едет вместе с родителем — это правило легко сломать.
const Case kIndentCases[] = {
    {"отступ пункта",
     "- раз\n- два\n",
     1, 0, {key("Tab")},
     "- раз\n  - два\n", 1},

    {"первый пункт отступать некуда",
     "- раз\n- два\n",
     0, 0, {key("Tab")},
     "- раз\n- два\n", 0},

    {"поддерево едет вместе с пунктом",
     "- раз\n- два\n  - внук\n",
     1, 0, {key("Tab")},
     "- раз\n  - два\n    - внук\n", 1},

    {"выступ возвращает на уровень",
     "- раз\n  - два\n",
     1, 0, {key("Shift+Tab")},
     "- раз\n- два\n", 1},

    {"выступ тянет поддерево",
     "- раз\n  - два\n    - внук\n",
     1, 0, {key("Shift+Tab")},
     "- раз\n- два\n  - внук\n", 1},
};

// Превращения. Нумерация обязана пересчитаться сама.
const Case kKindCases[] = {
    // Пустых строк между прогонами канон не ставит: маркеры и так разные.
    {"буллет в нумерованный",
     "- раз\n- два\n- три\n",
     1, 0, {key("Ctrl+3")},
     "- раз\n1. два\n- три\n", 1},

    {"весь список в нумерованный",
     "- раз\n- два\n- три\n",
     0, 0, {key("Ctrl+A"), key("Ctrl+3")},
     "1. раз\n2. два\n3. три\n", -1},

    {"весь список в задачи",
     "- раз\n- два\n",
     0, 0, {key("Ctrl+A"), key("Ctrl+T")},
     "- [ ] раз\n- [ ] два\n", -1},

    {"задачи в буллеты — отметка исчезает",
     "- [x] раз\n- [ ] два\n",
     0, 0, {key("Ctrl+A"), key("Ctrl+8")},
     "- раз\n- два\n", -1},

    // Абзац между списками их и разделяет. Пустая строка после него в файле не
    // появляется: стык "задачки:" и "- [ ] дело" был плотным и таким остался.
    {"пункт в абзац разлепляет списки",
     "- раз\n- задачки:\n- [ ] дело\n",
     1, 0, {key("Ctrl+Shift+0")},
     "- раз\n\nзадачки:\n- [ ] дело\n", 2},
};

// Перестановка. Пункт едет со своим поддеревом, нумерация пересчитывается.
const Case kMoveCases[] = {
    {"пункт вниз",
     "- раз\n- два\n- три\n",
     0, 3, {key("Ctrl+Down")},
     "- два\n- раз\n- три\n", 1},

    {"пункт вверх",
     "- раз\n- два\n- три\n",
     2, 3, {key("Ctrl+Up")},
     "- раз\n- три\n- два\n", 1},

    {"первый пункт вверх не едет",
     "- раз\n- два\n",
     0, 3, {key("Ctrl+Up")},
     "- раз\n- два\n", 0},

    {"последний вниз не едет",
     "- раз\n- два\n",
     1, 3, {key("Ctrl+Down")},
     "- раз\n- два\n", 1},

    {"пункт едет с поддеревом",
     "- раз\n  - внук\n- два\n",
     0, 3, {key("Ctrl+Down")},
     "- два\n- раз\n  - внук\n", 1},

    // Просторный список — тот же список: пустая строка между пунктами прогон не
    // рвёт, и при обмене она остаётся на месте, а не уезжает с пунктом.
    {"пункт вниз в просторном списке",
     "- раз\n\n- два\n\n- три\n",
     0, 3, {key("Ctrl+Down")},
     "- два\n\n- раз\n\n- три\n", 2},

    {"нумерация пересчитывается",
     "1. раз\n2. два\n3. три\n",
     0, 4, {key("Ctrl+Down")},
     "1. два\n2. раз\n3. три\n", 1},
};

// Автозамена при наборе — с ней списки и заводят.
const Case kInputCases[] = {
    {"дефис и пробел заводят список",
     "текст\n",
     0, 5, {key("Return"), key("Return"), type("-"), key("Space"), type("пункт")},
     "текст\n\n- пункт\n", 2},

    {"звёздочка тоже",
     "текст\n",
     0, 5, {key("Return"), key("Return"), type("*"), key("Space"), type("пункт")},
     "текст\n\n- пункт\n", 2},

    {"номер заводит нумерованный",
     "текст\n",
     0, 5, {key("Return"), key("Return"), type("1."), key("Space"), type("пункт")},
     "текст\n\n1. пункт\n", 2},

    {"дефис со скобкой заводит задачу",
     "текст\n",
     0, 5, {key("Return"), key("Return"), type("-["), key("Space"), type("дело")},
     "текст\n\n- [ ] дело\n", 2},

    {"правило работает и на второй строке абзаца",
     "вступление\n",
     0, 10, {key("Return"), type("-"), key("Space"), type("пункт")},
     "вступление\n- пункт\n", 1},
};

// Пустые строки. Каждая из них — свой блок, и клавиши обязаны вести себя так,
// как человек их видит: на пустую строку можно встать, её можно убрать, на ней
// можно набрать текст.
//
// Убрать обязательную пустую строку — значит слить два абзаца в один: иначе
// инвариант вернул бы её на место сразу же, и клавиша выглядела бы сломанной.
// На экране при этом ничего не двигается — меняется только строение.
const Case kBlankLineCases[] = {
    {"Backspace на пустой строке сливает абзацы",
     "первый\n\nвторой\n",
     2, 0, {key("Backspace")},
     "первый\nвторой\n", 0},

    {"Delete в конце абзаца — то же самое",
     "первый\n\nвторой\n",
     0, 6, {key("Delete")},
     "первый\nвторой\n", 0},

    {"из двух пустых строк убирается одна",
     "первый\n\n\nвторой\n",
     3, 0, {key("Backspace")},
     "первый\n\nвторой\n", 2},

    {"пустая строка перед заголовком просто исчезает",
     "текст\n\n# заголовок\n",
     2, 0, {key("Backspace")},
     "текст\n# заголовок\n", 1},

    {"набор на пустой строке сливает соседей",
     "первый\n\nвторой\n",
     1, 0, {type("х")},
     "первый\nх\nвторой\n", 0},

    {"два Enter в конце абзаца дают пустую строку",
     "текст\n",
     0, 5, {key("Return"), key("Return"), type("ещё")},
     "текст\n\nещё\n", 2},
};

// Второй абзац внутри пункта. Читается из файла сам собой, а с клавиатуры
// заводится Tab'ом на абзаце сразу под списком: правило Tab одно на всех —
// «сделать блок глубже», просто у абзаца и у пункта это значит разное.
const Case kInsideItemCases[] = {
    {"Tab привязывает абзац к пункту",
     "- пункт\n\nабзац\n",
     2, 0, {key("Tab")},
     "- пункт\n\n  абзац\n", 2},

    {"Shift+Tab отвязывает обратно",
     "- пункт\n\n  абзац\n",
     2, 0, {key("Shift+Tab")},
     "- пункт\n\nабзац\n", 2},

    {"круг из двух нажатий возвращает исходное",
     "- пункт\n\nабзац\n",
     2, 0, {key("Tab"), key("Shift+Tab")},
     "- пункт\n\nабзац\n", 2},

    {"нумерация продолжается через второй абзац",
     "1. раз\n\n   продолжение\n\n2. два\n",
     4, 0, {type("!")},
     "1. раз\n\n   продолжение\n\n2. !два\n", 4},

    // Привязывать не к чему — и Tab остаётся обычным знаком табуляции, как был
    // в абзаце всегда. Строение при этом не меняется: уровня у блока не
    // появляется.
    {"привязывать не к чему — Tab остаётся табуляцией",
     "абзац\n",
     0, 0, {key("Tab")},
     "\tабзац\n", 0},

    {"Enter во втором абзаце переносит строку, как в абзаце",
     "- пункт\n\n  абзац\n",
     2, 5, {key("Return"), type("ещё")},
     "- пункт\n\n  абзац\n  ещё\n", 2},
};

// Сколько строк вставили, столько и убирается. Два Enter в конце пункта дают две
// пустые строки; один Backspace обязан убрать ровно одну.
//
// Раньше он убирал обе: между пунктом и абзацем пустая строка обязательна, и
// вместо удаления происходило слияние — пустой абзац уезжал внутрь пункта, а
// каретка прыгала вправо, в конец его текста.
// Тематическая черта "---" — блок без текста. Набор превращает её в абзац,
// Enter заводит обычный текст под ней, а Backspace на обязательной пустой
// строке над ней не съедает ни строку, ни черту (абзац вплотную над чертой —
// это уже setext-заголовок): каретка просто уходит выше. На этом Backspace
// черта и терялась, пока пробник не показал.
// Backspace — удаление НАЗАД: посреди прогона пустых строк исчезает строка
// выше, и каретка уходит вверх. Оставаться на месте, съедая нижнее, — это
// Delete, и ровно так оно по ошибке и работало.
const Case kBackspaceUpCases[] = {
    {"Backspace на средней из трёх пустых уходит вверх",
     "а\n\n\n\nб\n",
     2, 0, {key("Backspace")},
     "а\n\n\nб\n", 1},

    // Канон "___" ни с чем не слипается, поэтому пустая строка возле черты
    // удаляется как любая другая — отказов больше нет.
    {"Backspace на пустой над чертой удаляет её",
     "до\n\n___\n",
     1, 0, {key("Backspace")},
     "до\n___\n", 0},

    {"Backspace на черте удаляет пустую строку над ней",
     "до\n\n___\n",
     2, 0, {key("Backspace")},
     "до\n___\n", 1},

    // Гибнет то, что НАД кареткой: с пустой строки под чертой Backspace
    // убирает черту, а своя строка остаётся под кареткой.
    {"Backspace на пустой под чертой удаляет черту",
     "___\n\n- пункт\n",
     1, 0, {key("Backspace")},
     "\n- пункт\n", 0},

    {"Backspace на пустой между чертами удаляет верхнюю черту",
     "___\n\n___\n",
     1, 0, {key("Backspace")},
     "\n___\n", 0},

    {"Backspace на нижней черте через пустую подтягивает её",
     "___\n\n___\n",
     2, 0, {key("Backspace")},
     "___\n___\n", 1},

    // Черта прямо над кареткой удаляется — удаление назад, как со знаком.
    {"Backspace под чертой удаляет её",
     "___\nпосле\n",
     1, 0, {key("Backspace")},
     "после\n", 0},

    {"Backspace на нижней из двух черт вплотную удаляет верхнюю",
     "___\n___\n",
     1, 0, {key("Backspace")},
     "___\n", 0},

    // Черта под кареткой — строка без содержимого: слить её вверх и значит
    // удалить, ровно как пустую строку. Без этого лесенку черт нельзя было
    // снести Backspace-ом до конца.
    {"Backspace на черте под заголовком удаляет её",
     "# з\n___\n",
     1, 0, {key("Backspace")},
     "# з\n", 0},

    // Старт — с пустой строки под нижней чертой; с начала пункта Backspace
    // сперва снял бы маркер, это отдельное правило списков.
    {"лесенка из трёх черт сносится Backspace-ом целиком",
     "текст\n\n___\n\n___\n\n___\n\n- пункт\n",
     7, 0,
     {key("Backspace"), key("Backspace"), key("Backspace"), key("Backspace"),
      key("Backspace"), key("Backspace"), key("Backspace")},
     "текст\n- пункт\n", 0},

    {"Delete на пустой под чертой убирает строку, не черту",
     "___\n\n- пункт\n",
     1, 0, {key("Delete")},
     "___\n- пункт\n", 1},
};

const Case kDividerCases[] = {
    {"три дефиса и пробел делают черту в конце заметки",
     "до\n",
     0, 99, {key("Return"), type("---"), key("Space")},
     // Пустая строка между абзацем и чертой канону не нужна; хвостовой пустой
     // абзац виден здесь, но в файл не печатается.
     "до\n___\n\n", 2},

    {"три дефиса и Enter делают черту",
     "до\n",
     0, 99, {key("Return"), type("---"), key("Return")},
     "до\n___\n\n", 2},

    {"подчёркивания тоже делают черту",
     "до\n",
     0, 99, {key("Return"), type("___"), key("Space")},
     "до\n___\n\n", 2},

    {"черта между абзацами не плодит пустых строк",
     "до\n\nпосле\n",
     0, 99, {key("Return"), type("---"), key("Space")},
     "до\n___\n\nпосле\n", 2},

    // Enter в абзаце — перенос строки внутри блока, поэтому непринятые дефисы
    // остаются его второй строкой; экран печатает её с защитным слэшем.
    {"два дефиса и пробел чертой не становятся",
     "до\n",
     0, 99, {key("Return"), type("--"), key("Space")},
     "до\n\\-- \n", 0},

    // Сериализатор экранирует дефисы в тексте пункта — иначе "- ---" при
    // следующем чтении стал бы пунктом с чертой внутри.
    {"дефисы в пункте списка остаются текстом",
     "- раз\n- два\n",
     1, 99, {key("Return"), type("---"), key("Space")},
     "- раз\n- два\n- \\--- \n", 2},

    {"набор на черте делает её абзацем",
     "до\n\n___\n\nпосле\n",
     2, 0, {type("текст")},
     "до\n\nтекст\n\nпосле\n", 2},

    {"Enter на черте заводит текст под ней",
     "до\n\n___\n\nпосле\n",
     2, 0, {key("Return"), type("абзац")},
     "до\n\n___\nабзац\n\nпосле\n", 3},

    {"Backspace над чертой при двух пустых строках убирает одну",
     "до\n\n\n___\n",
     2, 0, {key("Backspace")},
     "до\n\n___\n", 1},
};

const Case kBlankLineAfterItemCases[] = {
    {"Backspace убирает одну строку из двух",
     "- [ ] задача\n## Заголовок\n",
     0, 99, {key("Return"), key("Return"), key("Up"), key("Backspace")},
     "- [ ] задача\n\n## Заголовок\n", 0},

    {"и то же в буллете",
     "- пункт\n## Заголовок\n",
     0, 99, {key("Return"), key("Return"), key("Up"), key("Backspace")},
     "- пункт\n\n## Заголовок\n", 0},

    // Из вложенного списка выходят теми же двумя нажатиями, а не тремя: шаг по
    // уровню за раз здесь только мешает. Подняться на уровень, оставшись в
    // списке, можно иначе — Enter и Shift+Tab.
    {"два Enter выводят и из вложенного списка",
     "- [ ] раз\n  - [ ] вложенный\n## Заголовок\n",
     1, 99, {key("Return"), key("Return"), type("абзац")},
     "- [ ] раз\n  - [ ] вложенный\n\nабзац\n## Заголовок\n", 3},

    {"Enter и Shift+Tab поднимают на уровень",
     "- [ ] раз\n  - [ ] вложенный\n",
     1, 99, {key("Return"), key("Shift+Tab"), type("наверху")},
     "- [ ] раз\n  - [ ] вложенный\n- [ ] наверху\n", 2},

    // Два Enter в конце пункта — это выход из списка с пустой строкой перед
    // новым абзацем, а не «пункт съел строку».
    {"два Enter дают ровно две пустые строки",
     "- [ ] задача\n## Заголовок\n",
     0, 99, {key("Return"), key("Return")},
     "- [ ] задача\n\n\n## Заголовок\n", 2},
};

// Заголовок. Решётки — признак блока, а не текст, и вставка пустой строки перед
// ним признака касаться не должна. Разрез в начале строки отдавал заголовок
// пустой верхней половине, а текст уезжал вниз обычным абзацем: "## Редактор"
// превращался в "##" и "Редактор".
const Case kHeadingCases[] = {
    {"Enter в начале заголовка отбивает его сверху",
     "# Планы\n## Редактор\n",
     1, 0, {key("Return")},
     "# Планы\n\n## Редактор\n", 2},

    {"и подзаголовок остаётся подзаголовком после набора",
     "# Планы\n## Редактор\n",
     1, 0, {key("Return"), type("!")},
     "# Планы\n\n## !Редактор\n", 2},

    // А разрез по тексту по-прежнему даёт абзац: заголовок однострочен.
    // А разрез по тексту по-прежнему даёт абзац: заголовок однострочен. Смещение
    // считается по тексту блока, без решёток — их в тексте нет.
    {"разрез заголовка посередине даёт абзац",
     "## Редактордальше\n",
     0, 8, {key("Return")},
     "## Редактор\nдальше\n", 1},

    {"Enter в конце заголовка заводит абзац",
     "## Редактор\n",
     0, 8, {key("Return"), type("текст")},
     "## Редактор\nтекст\n", 1},
};

// Отметка задач.
const Case kTaskCases[] = {
    {"переключить задачу",
     "- [ ] дело\n",
     0, 6, {key("Ctrl+Space")},
     "- [x] дело\n", 0},

    {"переключить обратно",
     "- [x] дело\n",
     0, 6, {key("Ctrl+Space")},
     "- [ ] дело\n", 0},

    {"на буллете молчит",
     "- пункт\n",
     0, 3, {key("Ctrl+Space")},
     "- пункт\n", 0},

    {"выделенные задачи переключаются разом",
     "- [ ] раз\n- [ ] два\n- [ ] три\n",
     0, 0, {key("Ctrl+A"), key("Ctrl+Space")},
     "- [x] раз\n- [x] два\n- [x] три\n", -1},

    {"хоть одна выполненная — снимаются все",
     "- [x] раз\n- [ ] два\n",
     0, 0, {key("Ctrl+A"), key("Ctrl+Space")},
     "- [ ] раз\n- [ ] два\n", -1},
};

// Отмена: каждая правка списка обязана откатываться целиком.
void checkUndo() {
    struct Undoable {
        const char* what;
        const char* source;
        int block;
        int offset;
        const char* keys;
    };
    const Undoable cases[] = {
        {"Enter в начале", "- раз\n- два\n", 1, 0, "Return"},
        {"Backspace у маркера", "- раз\n- два\n", 1, 0, "Backspace"},
        {"отступ", "- раз\n- два\n", 1, 0, "Tab"},
        {"превращение", "- раз\n- два\n", 1, 0, "Ctrl+3"},
        {"перестановка", "- раз\n- два\n", 0, 3, "Ctrl+Down"},
        {"переключение задачи", "- [ ] дело\n", 0, 6, "Ctrl+Space"},
    };
    for (const Undoable& c : cases) {
        const QString path =
            writeNote(std::string("отмена-") + c.what + ".md", QString::fromUtf8(c.source));

        zametti::NoteEditor editor;
        editor.resize(700, 500);
        editor.show();
        QTest::qWait(20);
        editor.setFocus();
        editor.openFile(path);
        QTest::qWait(20);

        QTextCursor cursor = editor.textCursor();
        const QTextBlock start = editor.document()->findBlockByNumber(c.block);
        cursor.setPosition(start.position() + qMin(c.offset, start.length() - 1));
        editor.setTextCursor(cursor);

        const QKeySequence sequence(QString::fromLatin1(c.keys), QKeySequence::PortableText);
        QTest::keyClick(&editor, sequence[0].key(), sequence[0].keyboardModifiers());
        QTest::qWait(10);
        QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
        QTest::qWait(10);

        ZT_EQ(std::string("отмена вернула документ: ") + c.what, std::string(c.source),
              textOf(editor).toStdString());
    }
}

// Ритм страницы вокруг списка. Он намеренно несимметричен: список идёт вплотную
// под вводной строкой — так его и набирают, строка, Enter, "- " — а после списка
// стоит заметный воздух, как пустая строка в самом файле.
//
// Проверка нужна ровно потому, что однажды это уже «выровняли»: одна отбивка на
// все стыки убирала прыжок пункта, ставшего абзацем, но вид от этого стал явно
// хуже — огромный зазор между вводной строкой и первым пунктом.
void checkListRhythm() {
    const zametti::Appearance saved = zametti::appearance();
    zametti::appearance().blockSpacing = 0.667;
    struct Restore {
        const zametti::Appearance& from;
        ~Restore() { zametti::appearance() = from; }
    } restore{saved};

    // Отбивку решает файл, а не род блоков. Список, написанный вплотную под
    // вводной строкой, так и стоит; написанный через пустую строку — через
    // отбивку. Раньше у границ списка были свои значения, и пустая строка из
    // файла не была видна вовсе.
    const QString tight = writeNote(
        "ритм-вплотную.md",
        QStringLiteral("вводная строка:\n- первый\n- второй\n\nабзац после\n"));

    zametti::NoteEditor editor;
    editor.resize(700, 400);
    editor.show();
    QTest::qWait(20);
    editor.setFocus();
    editor.openFile(tight);
    QTest::qWait(20);

    auto marginOf = [&editor](int number) {
        return editor.document()->findBlockByNumber(number).blockFormat().topMargin();
    };
    auto isGap = [&editor](int number) {
        return zametti::isVSpaceBlock(editor.document()->findBlockByNumber(number));
    };
    auto heightOf = [&editor](int number) {
        return editor.document()
            ->documentLayout()
            ->blockBoundingRect(editor.document()->findBlockByNumber(number))
            .height();
    };

    ZT_TRUE("список вплотную под вводной строкой стоит вплотную",
            marginOf(1) <= 0.01);
    ZT_TRUE("и пункты между собой тоже", marginOf(2) <= 0.01);
    // Отбивку держит сама пустая строка, а не поле блока: сколько их в файле,
    // столько и на экране.
    ZT_TRUE("а абзац после списка отделён пустой строкой", isGap(3));
    ZT_TRUE("и она высотой в строку", heightOf(3) > 1.0);
    ZT_TRUE("сам абзац при этом полей не имеет", marginOf(4) <= 0.01);

    // Тот же список, но написанный через пустую строку.
    const QString loose = writeNote(
        "ритм-через-строку.md",
        QStringLiteral("вводная строка:\n\n- первый\n- второй\n"));
    editor.openFile(loose);
    QTest::qWait(20);
    ZT_TRUE("список через пустую строку отделён от вводной", isGap(1));
    ZT_TRUE("а пункты между собой всё равно вплотную", marginOf(3) <= 0.01);
}

// Круг «абзац → пункт → абзац» обязан вернуть блок ровно на прежнее место.
//
// Больше того: он не должен трогаться с места и по дороге. Отбивку теперь держат
// пустые строки из файла, а не поля блоков, и от смены рода она не зависит —
// раньше блок, став пунктом, подпрыгивал вверх, и человек видел это как рывок.
void checkKindRoundTripKeepsPlace() {
    const zametti::Appearance saved = zametti::appearance();
    zametti::appearance().blockSpacing = 0.667;
    struct Restore {
        const zametti::Appearance& from;
        ~Restore() { zametti::appearance() = from; }
    } restore{saved};

    const QString path = writeNote(
        "круг-вида.md",
        QStringLiteral("- [ ] верхняя\n  - [ ] вложенная\n\nдо 19 июля:\n\n- [x] дело\n"));

    zametti::NoteEditor editor;
    editor.resize(700, 400);
    editor.show();
    QTest::qWait(20);
    editor.setFocus();
    editor.openFile(path);
    QTest::qWait(20);

    int target = -1;
    for (QTextBlock block = editor.document()->begin(); block.isValid();
         block = block.next())
        if (block.text().startsWith(QStringLiteral("до 19"))) {
            target = block.blockNumber();
            break;
        }
    ZT_TRUE("строка \"до 19\" должна найтись", target > 0);

    auto topOf = [&editor, target] {
        return editor.document()->documentLayout()->blockBoundingRect(
            editor.document()->findBlockByNumber(target)).top();
    };
    const qreal asParagraph = topOf();

    QTextCursor cursor = editor.textCursor();
    cursor.setPosition(editor.document()->findBlockByNumber(target).position());
    editor.setTextCursor(cursor);
    QTest::keyClick(&editor, Qt::Key_8, Qt::ControlModifier);
    QTest::qWait(10);
    ZT_EQ("став пунктом, блок с места не сдвинулся",
          std::to_string(int(asParagraph)), std::to_string(int(topOf())));

    QTest::keyClick(&editor, Qt::Key_0, Qt::ControlModifier | Qt::ShiftModifier);
    QTest::qWait(10);
    ZT_EQ("вернувшись в абзац, блок встал на прежнее место",
          std::to_string(int(asParagraph)), std::to_string(int(topOf())));
}

}  // namespace

// Свойства Backspace на ЛЮБОЙ лесенке из текста, пустых строк и черт — их
// может быть и сто подряд, и правила обязаны держаться на каждой:
//   1) черты и текст не пропадают;
//   2) пустых строк уходит не больше одной за нажатие;
//   3) каретка не уезжает вниз;
//   4) документ остаётся записываемым (круг разбором сходится).
// Перебор всех лесенок длины до 4, каретка в каждом блоке, плюс длинный забор.
void checkBackspaceProperties() {
    zametti::NoteEditor editor;
    editor.resize(700, 500);
    editor.show();
    QTest::qWait(20);
    editor.setFocus();

    const auto snapshot = [](const QTextDocument& doc) {
        int dividers = 0, blanks = 0;
        QString text;
        for (QTextBlock b = doc.begin(); b.isValid(); b = b.next()) {
            if (zametti::isRawBlock(b)) continue;
            const zametti::Kind k = zametti::kindOf(b);
            if (k == zametti::Kind::Divider) ++dividers;
            else if (k == zametti::Kind::VSpace) ++blanks;
            else text += b.text().remove(QChar::LineSeparator).remove(QLatin1Char('\n'));
        }
        return std::tuple<int, int, QString>(dividers, blanks, text);
    };

    int powers[5] = {1, 3, 9, 27, 81};
    for (int len = 1; len <= 4; ++len) {
        for (int mask = 0; mask < powers[len]; ++mask) {
            QString src;
            for (int i = 0, m = mask; i < len; ++i, m /= 3) {
                const int a = m % 3;
                if (a == 0) src += QStringLiteral("т%1\n").arg(i);
                else if (a == 1) src += QStringLiteral("\n");
                else src += QStringLiteral("___\n");
            }
            const QString path = writeNote("свойства.md", src);

            for (int block = 0;; ++block) {
                editor.openFile(path);
                const QTextBlock target = editor.document()->findBlockByNumber(block);
                if (!target.isValid()) break;
                QTextCursor cursor(editor.document());
                cursor.setPosition(target.position());
                editor.setTextCursor(cursor);

                const auto [div0, blank0, text0] = snapshot(*editor.document());
                const QTextBlock above = target.previous();
                const bool dividerAbove = above.isValid() && !zametti::isRawBlock(above) &&
                                          zametti::kindOf(above) == zametti::Kind::Divider;
                // Считаем ДО нажатия: после правки хэндл блока протухает.
                const bool onDivider = !zametti::isRawBlock(target) &&
                                       zametti::kindOf(target) == zametti::Kind::Divider;
                QTest::keyClick(&editor, Qt::Key_Backspace);
                const auto [div1, blank1, text1] = snapshot(*editor.document());
                const int landed = editor.textCursor().blockNumber();

                const std::string tag = QStringLiteral("[%1] блок %2: ")
                                            .arg(src)
                                            .arg(block)
                                            .replace(QStringLiteral("\n"), QStringLiteral("|"))
                                            .toStdString();
                ZT_TRUE(tag + "текст цел", text1 == text0);
                ZT_TRUE(tag + "за нажатие уходит не больше одной строки",
                        div1 <= div0 && blank1 <= blank0 &&
                            (div0 - div1) + (blank0 - blank1) <= 1);
                ZT_TRUE(tag + "черта удаляется, только над кареткой или под ней",
                        div1 == div0 || dividerAbove || onDivider);
                ZT_TRUE(tag + "каретка не уехала вниз", landed <= block);

                // Ровно путь записи: с нормализацией documentForFile — файл,
                // например, не выражает пустую строку в самом начале.
                const std::string once = zametti::serialize(
                    zametti::documentForFile(zametti::readDocument(*editor.document())));
                const std::string twice = zametti::serialize(zametti::parse(once));
                ZT_TRUE(tag + "документ записываем", once == twice);
                // Правки не сохраняем: файл на каждый случай пишется заново, а
                // сохранение при смене файла может увести в модальный диалог —
                // на нём перебор и висел бы.
                editor.document()->setModified(false);
            }
        }
    }

    // Длинный забор: сорок черт вперемешку с пустыми, их может быть хоть
    // сотня. Backspace с самого низа до самого верха: текст обязан пережить
    // все нажатия, каретка не смеет уехать вниз, а документ — перестать
    // записываться. Сами черты при этом законно стираются: каждая была прямо
    // над кареткой.
    QString fence = QStringLiteral("верх\n");
    for (int i = 0; i < 40; ++i)
        fence += (i % 3 == 0) ? QStringLiteral("\n___\n") : QStringLiteral("___\n");
    fence += QStringLiteral("\nниз\n");
    const QString path = writeNote("забор.md", fence);
    editor.openFile(path);
    QTextCursor cursor(editor.document());
    cursor.setPosition(editor.document()->lastBlock().position());   // начало "низ"
    editor.setTextCursor(cursor);
    const auto [div0, blank0, text0] = snapshot(*editor.document());
    int previous = editor.textCursor().blockNumber();
    bool monotone = true;
    // Жмём, пока каретка не доберётся до верха: дальше Backspace начал бы
    // честно есть буквы — это уже не про структуру.
    for (int press = 0; press < 200 && editor.textCursor().blockNumber() > 0; ++press) {
        QTest::keyClick(&editor, Qt::Key_Backspace);
        const int now = editor.textCursor().blockNumber();
        if (now > previous) monotone = false;
        previous = now;
    }
    QTest::qWait(10);
    const auto [div1, blank1, text1] = snapshot(*editor.document());
    ZT_TRUE("забор: каретка ни разу не уехала вниз", monotone);
    ZT_TRUE("забор: текст цел", text1 == text0);
    ZT_TRUE("забор: строк не прибавилось", div1 + blank1 <= div0 + blank0);
    ZT_TRUE("забор: каретка добралась до верха", editor.textCursor().blockNumber() <= 1);
    const std::string once = zametti::serialize(zametti::readDocument(*editor.document()));
    ZT_TRUE("забор: документ записываем", once == zametti::serialize(zametti::parse(once)));
    editor.document()->setModified(false);
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    g_dir = fs::temp_directory_path() / "zametti-list-test";
    fs::remove_all(g_dir);
    fs::create_directories(g_dir);

    for (const Case& c : kEnterCases) run(c);
    for (const Case& c : kBackspaceCases) run(c);
    for (const Case& c : kIndentCases) run(c);
    for (const Case& c : kKindCases) run(c);
    for (const Case& c : kMoveCases) run(c);
    for (const Case& c : kInputCases) run(c);
    for (const Case& c : kTaskCases) run(c);
    for (const Case& c : kBlankLineCases) run(c);
    for (const Case& c : kInsideItemCases) run(c);
    for (const Case& c : kHeadingCases) run(c);
    for (const Case& c : kBlankLineAfterItemCases) run(c);
    for (const Case& c : kDividerCases) run(c);
    for (const Case& c : kBackspaceUpCases) run(c);
    checkBackspaceProperties();
    checkUndo();
    checkListRhythm();
    checkKindRoundTripKeepsPlace();

    fs::remove_all(g_dir);
    return zt::report("списки");
}
