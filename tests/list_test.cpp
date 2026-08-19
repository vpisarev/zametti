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
#include "pieces.h"
#include "document_saver.h"
#include "editor_widget.h"
#include "marker.h"
#include "settings.h"
#include "settings_hook.h"

#include "test_util.h"
#include "testdata.h"

#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QKeySequence>
#include <QScrollBar>
#include <QKeyEvent>
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

// Набор произвольного текста. QTest::keyClicks переводит знаки в коды клавиш
// по таблице ASCII и на «№» падает с утверждением — шлём события ввода
// напрямую, как это делает раскладка.
void typeText(zametti::NoteEditor& editor, const QString& text) {
    for (const QChar ch : text) {
        QKeyEvent press(QEvent::KeyPress, Qt::Key_unknown, Qt::NoModifier, QString(ch));
        QApplication::sendEvent(&editor, &press);
        QKeyEvent release(QEvent::KeyRelease, Qt::Key_unknown, Qt::NoModifier, QString(ch));
        QApplication::sendEvent(&editor, &release);
    }
}

QString textOf(const zametti::NoteEditor& editor) {
    return QString::fromStdString(
        markdownOf(blocksOf(*editor.document())));
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

    // Сливаемся в один ТЕКСТ только со своим списком: тот же вид маркера и
    // тот же уровень. С чужим — снимаем маркер, и разжалованная строка мягким
    // переносом пристаёт к пункту выше: строки на экране как стояли, так и
    // стоят, документ от Backspace не растёт. Склейки текстов в одну строку
    // ("- [ ] задачабуллет") при этом нет — перенос сохраняется.
    {"задача снимает маркер и пристаёт строкой к буллету",
     "- буллет\n- [ ] задача\n",
     1, 0, {key("Backspace")},
     "- буллет\n  задача\n", 0},

    {"вложенный снимает маркер и пристаёт строкой к родителю",
     "- верх\n  - вложенный\n",
     1, 0, {key("Backspace")},
     "- верх\n  вложенный\n", 0},

    {"нумерованный снимает маркер и пристаёт строкой к буллету",
     "- буллет\n1. номер\n",
     1, 0, {key("Backspace")},
     "- буллет\n  номер\n", 0},

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
     1, 0, {key("Ctrl+7")},
     "- раз\n1. два\n- три\n", 1},

    {"весь список в нумерованный",
     "- раз\n- два\n- три\n",
     0, 0, {key("Ctrl+A"), key("Ctrl+7")},
     "1. раз\n2. два\n3. три\n", -1},

    {"весь список в задачи",
     "- раз\n- два\n",
     0, 0, {key("Ctrl+A"), key("Ctrl+9")},
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

    // Привязывать не к чему — и Tab не делает НИЧЕГО. До этапа 11 он вставлял
    // сюда знак табуляции, и это было ошибкой: "\tабзац" в markdown — блок
    // кода с отступом, то есть абзац, в начале которого нажали Tab, при
    // следующем открытии заметки становился кодом. Нашла матрица краёв блока
    // кода; сам этот набор при старом ответе жаловался на самопроверку
    // («разобранное обратно отличается от документа») и всё равно считался
    // зелёным, потому что спрашивал только текст.
    {"привязывать не к чему — Tab не делает ничего",
     "абзац\n",
     0, 0, {key("Tab")},
     "абзац\n", 0},

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
// Набор на пустой строке между абзацами: соседи сливаются в один блок (строки
// как стояли, так и стоят), а каретка обязана остаться у набранного — она
// уезжала за вставленный при слиянии перенос, и продолжение набора ложилось в
// начало нижней строки.
const Case kTypeOnBlankCases[] = {
    {"набор на пустой между абзацами не разъезжается",
     "до\n\nпосле\n",
     1, 0, {type("a"), type("b"), type("c")},
     "до\nabc\nпосле\n", 0},

    {"дефисы на пустой между абзацами остаются вместе",
     "до\n\nпосле\n",
     1, 0, {type("-"), type("-"), type("-")},
     "до\n\\---\nпосле\n", 0},
};

const Case kBackspaceUpCases[] = {
    // Полный прогон владельца: справа от "\---" под чертой жмём Backspace до
    // упора. Каждое нажатие убирает ровно одну строку НАД кареткой (после
    // дефисов), каретка не прыгает: дефис, дефис, дефис, пустая, черта,
    // пустая-дубль, своя пустая в конец пункта.
    {"снос дефисов и черты справа от текста — монотонный",
     "- пункт\n  - вложенный\n\n___\n\n\\---\n\nдальше:\n",
     5, 99,
     {key("Backspace"), key("Backspace"), key("Backspace"), key("Backspace"),
      key("Backspace"), key("Backspace"), key("Backspace")},
     "- пункт\n  - вложенный\n\nдальше:\n", 1},

    // Каретка на пустом абзаце (стёрли текст), над ней пустая строка перед
    // чертой: гибнет пустая НАД кареткой, а не собственная строка каретки —
    // прыжок через строку на черту был ровно этим.
    {"Backspace на пустом абзаце под пустой строкой берёт верхнюю",
     "___\n\nтекст\n",
     2, 99,
     {key("Backspace"), key("Backspace"), key("Backspace"), key("Backspace"),
      key("Backspace"), key("Backspace")},
     // Пять букв и пустая над кареткой; своя пустая строка остаётся под ней.
     "___\n\n", 1},

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

    // Плоская модель: Backspace удаляет СЛЕВА, а слева от каретки на черте —
    // перевод строки заголовка, который удалить нельзя (черта не живёт в
    // строке текста). Отказ, каретка шагает в конец заголовка. Сама черта под
    // кареткой — дело Delete; лесенки сносятся удалениями слева, что держит
    // плоский фаззер.
    {"Backspace на черте под заголовком шагает, не удаляя",
     "# з\n___\n",
     1, 0, {key("Backspace")},
     "# з\n___\n", 0},

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

    // Enter в начале пункта заводит пустой пункт НАД текущим, и текст целиком
    // уезжает вниз. Значит отметка обязана уехать вместе с текстом, а новый —
    // пустой — пункт выполненным быть не может: отмечать в нём нечего.
    // Раньше выходило наоборот: пустой сверху оказывался выполненным, а с
    // текста отметка снималась.
    {"Enter в начале выполненной задачи не переносит отметку наверх",
     "- [x] сделано\n",
     0, 0, {key("Return")},
     "- [ ]\n- [x] сделано\n", 0},

    {"то же посреди списка",
     "- [x] раз\n- [x] два\n",
     1, 0, {key("Return")},
     "- [x] раз\n- [ ]\n- [x] два\n", 1},

    {"у невыполненной задачи всё как было",
     "- [ ] дело\n",
     0, 0, {key("Return")},
     "- [ ]\n- [ ] дело\n", 0},

    // Разрез по тексту — случай обратный: новым становится НИЖНИЙ пункт, и
    // отметка остаётся у верхнего, где текст и был.
    // Смещение считается по тексту блока: маркер в текст не входит, и "сде"
    // это ровно три знака.
    {"Enter посреди выполненной задачи заводит невыполненный ниже",
     "- [x] сделано\n",
     0, 3, {key("Return")},
     "- [x] сде\n- [ ] лано\n", 1},
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
    const zametti::ZSettings saved = zametti::settings();
    zametti::mutableSettingsForTests().style().setBlockSpacing(0.667);
    struct Restore {
        const zametti::ZSettings& from;
        ~Restore() { zametti::mutableSettingsForTests() = from; }
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
    const zametti::ZSettings saved = zametti::settings();
    zametti::mutableSettingsForTests().style().setBlockSpacing(0.667);
    struct Restore {
        const zametti::ZSettings& from;
        ~Restore() { zametti::mutableSettingsForTests() = from; }
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

// Ввод черты и отмена — один жест: пробел (или Enter), создавший черту, не
// попадает в шаг истории, и Ctrl+Z возвращает голые дефисы с кареткой сразу
// за ними — без хвостового пробела и без лишней строки.
void checkDividerUndo() {
    // На существующей пустой строке (под кареткой уже есть содержимое): после
    // создания черты правило уводит каретку на блок ниже, но отмена всё равно
    // обязана вернуть её к месту правки — сразу за дефисы, а не ниже.
    {
        zametti::NoteEditor editor;
        editor.resize(700, 500);
        editor.show();
        QTest::qWait(20);
        editor.setFocus();
        const QString path =
            writeNote("черта-на-пустой.md", QStringLiteral("# з\n\n___\n"));
        editor.openFile(path);
        QTest::qWait(20);

        QTextCursor cursor(editor.document());
        cursor.setPosition(editor.document()->findBlockByNumber(1).position());
        editor.setTextCursor(cursor);
        for (int i = 0; i < 3; ++i) QTest::keyClick(&editor, Qt::Key_Minus);
        QTest::keyClick(&editor, Qt::Key_Space);
        QTest::qWait(10);
        ZT_EQ("черта на пустой строке появилась", "# з\n___\n___\n",
              textOf(editor).toStdString());

        QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
        QTest::qWait(10);
        const QTextCursor after = editor.textCursor();
        ZT_EQ("отмена вернула дефисы на пустой строке", "# з\n\\---\n___\n",
              textOf(editor).toStdString());
        // КАРЕТКА ПОСЛЕ ОТМЕНЫ — НА МЕСТЕ ПРАВКИ. Скобку правки открывает курсор
        // человека, и Qt возвращает каретку именно туда.
        ZT_TRUE("каретка на восстановленной строке, а не на блоке ниже",
                after.blockNumber() == 1);
    }

    for (const bool viaEnter : {false, true}) {
        zametti::NoteEditor editor;
        editor.resize(700, 500);
        editor.show();
        QTest::qWait(20);
        editor.setFocus();
        const QString path = writeNote(viaEnter ? "черта-enter.md" : "черта-пробел.md",
                                       QStringLiteral("до\n"));
        editor.openFile(path);
        QTest::qWait(20);

        QTextCursor cursor = editor.textCursor();
        cursor.movePosition(QTextCursor::End);
        editor.setTextCursor(cursor);
        QTest::keyClick(&editor, Qt::Key_Return);
        editor.insertPlainText(QStringLiteral("---"));
        QTest::keyClick(&editor, viaEnter ? Qt::Key_Return : Qt::Key_Space);
        QTest::qWait(10);
        ZT_EQ(std::string("черта появилась (") + (viaEnter ? "Enter" : "пробел") + ")",
              "до\n___\n\n", textOf(editor).toStdString());

        QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
        QTest::qWait(10);
        const QTextCursor after = editor.textCursor();
        ZT_EQ(std::string("отмена вернула дефисы без хвоста (") +
                  (viaEnter ? "Enter" : "пробел") + ")",
              "до\n\\---\n", textOf(editor).toStdString());
        // Каретка — на строке с дефисами (правило Qt: на месте отменённой
        // правки), а не в дорисованном хвосте, которого больше нет.
        ZT_TRUE(std::string("каретка на строке с дефисами (") +
                    (viaEnter ? "Enter" : "пробел") + ")",
                after.block().text().endsWith(QStringLiteral("---")));
    }
}

// Строка из одних пробелов глазом неотличима от пустой, а вела себя как
// текст — на этом ловилась «склейка при двух пустых». Правило: хвостовые
// пробелы умирают при уходе каретки со строки; опустевшая строка становится
// настоящей пустой. Пока каретка на строке — свобода. Кода не касается.
void checkWhitespaceLineTidy() {
    zametti::NoteEditor editor;
    editor.resize(700, 500);
    editor.show();
    QTest::qWait(20);
    editor.setFocus();

    // Пробелы на пустой строке + уход: строка снова пустая.
    {
        const QString path = writeNote("пробельная.md", QStringLiteral("до\n\nпосле\n"));
        editor.openFile(path);
        QTest::qWait(20);
        QTextCursor c(editor.document());
        c.setPosition(editor.document()->findBlockByNumber(1).position());
        editor.setTextCursor(c);
        QTest::keyClick(&editor, Qt::Key_Space);
        QTest::keyClick(&editor, Qt::Key_Space);
        QTest::keyClick(&editor, Qt::Key_Down);
        QTest::qWait(10);
        ZT_TRUE("строка из пробелов затвердела в пустую",
                zametti::isVSpaceBlock(editor.document()->findBlockByNumber(1)));
        ZT_EQ("файл не заметил пробелов", "до\n\nпосле\n", textOf(editor).toStdString());

        // И Backspace от «после» теперь ведёт себя как с настоящей пустой.
        c.setPosition(editor.document()->findBlockByNumber(2).position());
        editor.setTextCursor(c);
        QTest::keyClick(&editor, Qt::Key_Backspace);
        QTest::qWait(10);
        ZT_EQ("Backspace после затвердевания честный", "до\nпосле\n",
              textOf(editor).toStdString());
    }

    // Хвостовые пробелы за словом умирают при уходе.
    {
        const QString path = writeNote("хвост.md", QStringLiteral("слово\n\nниз\n"));
        editor.openFile(path);
        QTest::qWait(20);
        QTextCursor c = editor.textCursor();
        c.setPosition(editor.document()->findBlockByNumber(0).position() + 5);
        editor.setTextCursor(c);
        QTest::keyClick(&editor, Qt::Key_Space);
        QTest::keyClick(&editor, Qt::Key_Space);
        QTest::keyClick(&editor, Qt::Key_Down);
        QTest::qWait(10);
        ZT_EQ("хвостовые пробелы умерли при уходе", "слово",
              editor.document()->findBlockByNumber(0).text().toStdString());
    }

    // В коде хвостовые пробелы — содержимое: не трогаем.
    {
        const QString path =
            writeNote("код-хвост.md", QStringLiteral("```\nx = 1\n```\n\nниз\n"));
        editor.openFile(path);
        QTest::qWait(20);
        // Заборы — не блоки: строка кода лежит нулевым блоком.
        QTextCursor c(editor.document());
        const QTextBlock codeLine = editor.document()->findBlockByNumber(0);
        c.setPosition(codeLine.position() + codeLine.length() - 1);
        editor.setTextCursor(c);
        QTest::keyClick(&editor, Qt::Key_Space);
        QTest::keyClick(&editor, Qt::Key_Space);
        QTest::keyClick(&editor, Qt::Key_Down);
        QTest::qWait(10);
        ZT_EQ("в коде хвост цел", "x = 1  ",
              editor.document()->findBlockByNumber(0).text().toStdString());
    }
}

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

    // Алфавит: абзац, пустая, черта, пункт, вложенный пункт, заголовок.
    // Каждый провал Backspace до сих пор был атомом, которого тут не хватало.
    const char* atoms[] = {"т%1\n", "\n", "___\n", "- п%1\n", "  - в%1\n", "## з%1\n"};
    const int kAtoms = 6;
    int powers[4] = {1, kAtoms, kAtoms * kAtoms, kAtoms * kAtoms * kAtoms};
    for (int len = 1; len <= 3; ++len) {
        for (int mask = 0; mask < powers[len]; ++mask) {
            QString src;
            for (int i = 0, m = mask; i < len; ++i, m /= kAtoms) {
                QString atom = QString::fromUtf8(atoms[m % kAtoms]);
                if (atom.contains(QStringLiteral("%1"))) atom = atom.arg(i);
                src += atom;
            }
            const QString path = writeNote("свойства.md", src);

            // Режим 1: сначала стереть текст блока до ПУСТОГО АБЗАЦА — это
            // состояние не существует в файлах и рождается только в редакторе;
            // ровно в нём пряталась ветка, стрелявшая вне своей зоны.
            for (int mode = 0; mode < 2; ++mode)
            for (int block = 0;; ++block) {
                editor.openFile(path);
                const QTextBlock target = editor.document()->findBlockByNumber(block);
                if (!target.isValid()) break;
                QTextCursor cursor(editor.document());
                if (mode == 1) {
                    const int len = target.length() - 1;
                    if (len == 0) continue;   // пустые и черты стирать нечего
                    cursor.setPosition(target.position() + len);
                    editor.setTextCursor(cursor);
                    for (int k = 0; k < len; ++k)
                        QTest::keyClick(&editor, Qt::Key_Backspace);
                    // Дальше проверяется одно СТРУКТУРНОЕ нажатие с пустого
                    // абзаца; сам блок для отчёта берём заново — старый протух.
                } else {
                    cursor.setPosition(target.position());
                    editor.setTextCursor(cursor);
                }
                const QTextBlock at =
                    editor.document()->findBlockByNumber(editor.textCursor().blockNumber());

                const auto [div0, blank0, text0] = snapshot(*editor.document());
                const QTextBlock above = at.previous();
                const bool dividerAbove = above.isValid() && !zametti::isRawBlock(above) &&
                                          zametti::kindOf(above) == zametti::Kind::Divider;
                // Считаем ДО нажатия: после правки хэндл блока протухает.
                const bool onDivider = !zametti::isRawBlock(at) &&
                                       zametti::kindOf(at) == zametti::Kind::Divider;
                QTest::keyClick(&editor, Qt::Key_Backspace);
                const auto [div1, blank1, text1] = snapshot(*editor.document());
                const int landed = editor.textCursor().blockNumber();

                const std::string tag = QStringLiteral("[%1] режим %2 блок %3: ")
                                            .arg(src)
                                            .arg(mode)
                                            .arg(block)
                                            .replace(QStringLiteral("\n"), QStringLiteral("|"))
                                            .toStdString();
                ZT_TRUE(tag + "текст цел", text1 == text0);
                ZT_TRUE(tag + "за нажатие уходит не больше одной строки",
                        div1 <= div0 && blank1 <= blank0 &&
                            (div0 - div1) + (blank0 - blank1) <= 1);
                ZT_TRUE(tag + "черта удаляется, только над кареткой или под ней",
                        div1 == div0 || dividerAbove || onDivider);
                // Стирание текста блок не двигает: и в режиме 1 каретка перед
                // структурным нажатием стоит в блоке под тем же номером.
                ZT_TRUE(tag + "каретка не уехала вниз", landed <= block);

                // Ровно путь записи: с нормализацией documentForFile — файл,
                // например, не выражает пустую строку в самом начале.
                const std::string once = markdownOf(
                    zametti::documentForFile(blocksOf(*editor.document())));
                const std::string twice = noteOf(once).toMarkdown();
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
    const std::string once = markdownOf(blocksOf(*editor.document()));
    ZT_TRUE("забор: документ записываем", once == noteOf(once).toMarkdown());
    editor.document()->setModified(false);
}

// Нумерованный маркер меняет вид по вложенности, три вида по кругу:
// 1. 2. 3. → a. b. c. → 1) 2) 3) → снова цифры. Буквы биективны: z, aa..zz, aaa.
// Комментарии в редакторе: Ctrl+/ делает блок комментарием и обратно,
// Backspace в начале комментария — жест снятия комментарности (буфер цел,
// слияние — только следующим нажатием), как у первого пункта списка.
void checkCommentOps() {
    zametti::NoteEditor editor;
    editor.resize(700, 500);
    editor.show();
    QTest::qWait(20);
    editor.setFocus();
    const QString path = writeNote(
        "комментарии.md", QStringLiteral("текст\n\n<!-- ком -->\n\nхвост\n"));
    editor.openFile(path);
    QTest::qWait(20);

    const auto blockAt = [&](int n) { return editor.document()->findBlockByNumber(n); };
    ZT_TRUE("комментарий прочитан родом",
            zametti::kindOf(blockAt(2)) == zametti::Kind::Html &&
                blockAt(2).text() == QStringLiteral("ком"));

    // Жест: Backspace в начале комментария снимает комментарность, текст цел,
    // блоки не сливаются.
    QTextCursor cursor(blockAt(2));
    editor.setTextCursor(cursor);
    const int blocks = editor.document()->blockCount();
    QTest::keyClick(&editor, Qt::Key_Backspace);
    QTest::qWait(10);
    ZT_TRUE("жест снял комментарность, буфер цел",
            zametti::kindOf(blockAt(2)) == zametti::Kind::Paragraph &&
                blockAt(2).text() == QStringLiteral("ком") &&
                editor.document()->blockCount() == blocks);

    // Ctrl+/ возвращает комментарность на место.
    QTest::keyClick(&editor, Qt::Key_Slash, Qt::ControlModifier);
    QTest::qWait(10);
    ZT_TRUE("Ctrl+/ сделал блок комментарием",
            zametti::kindOf(blockAt(2)) == zametti::Kind::Html);
    {
        const std::vector<zametti::Piece> ir = blocksOf(*editor.document());
        const std::string out = markdownOf(ir);
        ZT_TRUE("в файл уходит <!-- ком -->",
                out.find("<!-- ком -->") != std::string::npos);
    }

    // Ctrl+/ на обычном абзаце — комментарий, ещё раз — обратно.
    editor.setTextCursor(QTextCursor(blockAt(4)));
    QTest::keyClick(&editor, Qt::Key_Slash, Qt::ControlModifier);
    QTest::qWait(10);
    ZT_TRUE("хвост стал комментарием",
            zametti::kindOf(blockAt(4)) == zametti::Kind::Html);
    QTest::keyClick(&editor, Qt::Key_Slash, Qt::ControlModifier);
    QTest::qWait(10);
    ZT_TRUE("и обратно абзацем",
            zametti::kindOf(blockAt(4)) == zametti::Kind::Paragraph &&
                blockAt(4).text() == QStringLiteral("хвост"));

    // Ctrl+/ построчный: выделение второй строки пункта комментирует только
    // её — пункт остаётся пунктом, хвост — продолжением без маркера.
    {
        const QString p2 = writeNote(
            "построчный.md", QStringLiteral("- пункт\n  вторая строка\n  третья\n"));
        editor.document()->setModified(false);
        editor.openFile(p2);
        QTest::qWait(20);
        const QTextBlock item = editor.document()->findBlockByNumber(0);
        ZT_TRUE("пункт прочитан одной тройкой строк",
                zametti::kindOf(item) == zametti::Kind::ListItem &&
                    item.text().count(QChar::LineSeparator) == 2);

        const QString t = item.text();
        const int lineFrom = int(t.indexOf(QStringLiteral("вторая")));
        QTextCursor sel(editor.document());
        sel.setPosition(item.position() + lineFrom);
        sel.setPosition(item.position() + lineFrom +
                            int(QStringLiteral("вторая строка").size()),
                        QTextCursor::KeepAnchor);
        editor.setTextCursor(sel);
        QTest::keyClick(&editor, Qt::Key_Slash, Qt::ControlModifier);
        QTest::qWait(10);

        const auto blockAt = [&](int n) {
            return editor.document()->findBlockByNumber(n);
        };
        ZT_TRUE("пункт остался пунктом с одной строкой",
                zametti::kindOf(blockAt(0)) == zametti::Kind::ListItem &&
                    blockAt(0).text() == QStringLiteral("пункт"));
        ZT_TRUE("закомментирована только выделенная строка",
                zametti::kindOf(blockAt(1)) == zametti::Kind::Html &&
                    blockAt(1).text() == QStringLiteral("вторая строка") &&
                    zametti::levelOf(blockAt(1)) == 0);
        ZT_TRUE("хвост — продолжение без маркера",
                zametti::kindOf(blockAt(2)) == zametti::Kind::Paragraph &&
                    blockAt(2).text().trimmed() == QStringLiteral("третья"));
        {
            const std::vector<zametti::Piece> ir = blocksOf(*editor.document());
            const std::string out = markdownOf(ir);
            ZT_TRUE("в файле комментарий с отступом пункта",
                    out.find("  <!-- вторая строка -->") != std::string::npos);
        }
        QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
        QTest::qWait(10);
        ZT_TRUE("Ctrl-Z собрал пункт обратно",
                zametti::kindOf(blockAt(0)) == zametti::Kind::ListItem &&
                    blockAt(0).text().count(QChar::LineSeparator) == 2);
        editor.document()->setModified(false);
        editor.openFile(path);
        QTest::qWait(20);
    }

    // Ctrl-Z раскатывает всю лесенку обратно.
    QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
    QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
    QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
    QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
    QTest::qWait(10);
    ZT_TRUE("история вернула исходник",
            zametti::kindOf(blockAt(2)) == zametti::Kind::Html &&
                blockAt(2).text() == QStringLiteral("ком") &&
                zametti::kindOf(blockAt(4)) == zametti::Kind::Paragraph);
}

// «## » на первой строке многострочного абзаца делает заголовком только её:
// хвост остаётся текстом (сценарий владельца: «июль» + «просто текст»).
void checkInputRuleSplitsLine() {
    zametti::NoteEditor editor;
    editor.resize(700, 500);
    editor.show();
    QTest::qWait(20);
    editor.setFocus();
    const QString path = writeNote(
        "июль.md", QStringLiteral("# 2026\n\nиюль\nпросто текст\n"));
    editor.openFile(path);
    QTest::qWait(20);

    const auto blockAt = [&](int n) { return editor.document()->findBlockByNumber(n); };
    ZT_TRUE("июль и просто текст — один блок из двух строк",
            blockAt(2).text().count(QChar::LineSeparator) == 1);

    QTextCursor cursor(blockAt(2));
    editor.setTextCursor(cursor);
    QTest::keyClicks(&editor, QStringLiteral("## "));
    QTest::qWait(10);

    ZT_TRUE("июль стал подзаголовком",
            zametti::kindOf(blockAt(2)) == zametti::Kind::Heading &&
                blockAt(2).text() == QStringLiteral("июль"));
    bool tailIntact = false;
    for (QTextBlock b = editor.document()->begin(); b.isValid(); b = b.next())
        if (zametti::kindOf(b) == zametti::Kind::Paragraph &&
            b.text() == QStringLiteral("просто текст"))
            tailIntact = true;
    ZT_TRUE("просто текст остался текстом", tailIntact);

    // И середина блока: правило режет с обеих сторон.
    editor.document()->setModified(false);
    const QString path2 = writeNote(
        "середина.md", QStringLiteral("раз\nдва\nтри\n"));
    editor.openFile(path2);
    QTest::qWait(20);
    QTextCursor mid(editor.document());
    const QString all = editor.document()->firstBlock().text();
    mid.setPosition(editor.document()->firstBlock().position() +
                    int(all.indexOf(QStringLiteral("два"))));
    editor.setTextCursor(mid);
    QTest::keyClicks(&editor, QStringLiteral("# "));
    QTest::qWait(10);
    int headings = 0;
    bool one = false;
    bool three = false;
    for (QTextBlock b = editor.document()->begin(); b.isValid(); b = b.next()) {
        if (zametti::kindOf(b) == zametti::Kind::Heading) {
            ++headings;
            ZT_TRUE("заголовком стала только «два»",
                    b.text() == QStringLiteral("два"));
        }
        if (b.text().startsWith(QStringLiteral("раз"))) one = true;
        if (b.text().contains(QStringLiteral("три"))) three = true;
    }
    ZT_TRUE("заголовок ровно один", headings == 1);
    ZT_TRUE("соседние строки уцелели текстом", one && three);
}

// Знак номера работает при наборе наравне с решёткой: на русской раскладке «#»
// набирается только переключением на латиницу (просьба владельца). В файл при
// этом уходит решётка — знак номера живёт только на клавиатуре.
void checkNumberSignHeadings() {
    zametti::NoteEditor editor;
    editor.resize(700, 500);
    editor.show();
    QTest::qWait(20);
    editor.setFocus();

    const QString path = writeNote("номер.md", QStringLiteral("текст\n"));
    editor.openFile(path);
    QTest::qWait(20);

    const auto typeInFresh = [&](const QString& prefix, const QString& title) {
        QTextCursor end(editor.document());
        end.movePosition(QTextCursor::End);
        editor.setTextCursor(end);
        QTest::keyClick(&editor, Qt::Key_Return);
        QTest::keyClick(&editor, Qt::Key_Return);
        typeText(editor, prefix + QStringLiteral(" ") + title);
        QTest::qWait(10);
        return editor.textCursor().block();
    };

    for (int level = 1; level <= 6; ++level) {
        const QString prefix(level, QChar(0x2116));
        const QTextBlock block = typeInFresh(prefix, QStringLiteral("уровень%1").arg(level));
        ZT_TRUE("№ завёл заголовок", zametti::kindOf(block) == zametti::Kind::Heading);
        ZT_TRUE("уровень по числу знаков", block.blockFormat().headingLevel() == level);
        ZT_TRUE("знак номера съеден вместе с пробелом",
                block.text() == QStringLiteral("уровень%1").arg(level));
    }

    // В файл уходит решётка, а не знак номера: канон его не знает.
    editor.save(false);
    QTest::qWait(20);
    const QString written = textOf(editor);
    ZT_TRUE("в файле решётки", written.contains(QStringLiteral("###### уровень6")));
    ZT_TRUE("знака номера в файле нет", !written.contains(QChar(0x2116)));

    // Семь знаков — не заголовок, как и семь решёток.
    {
        const QTextBlock block = typeInFresh(QString(7, QChar(0x2116)), QStringLiteral("нет"));
        ZT_TRUE("семь знаков заголовком не делают",
                zametti::kindOf(block) != zametti::Kind::Heading);
    }
    // Смешанный ряд ничего не значит: правило требует однородности.
    {
        const QTextBlock block =
            typeInFresh(QStringLiteral("#") + QChar(0x2116), QStringLiteral("нет"));
        ZT_TRUE("смесь решётки и номера — не заголовок",
                zametti::kindOf(block) != zametti::Kind::Heading);
    }
}

// Автозамены по сочетанию: знаков нет на клавиатуре, а в тексте они нужны.
// Список задаётся конфигом; по умолчанию в нём одно длинное тире.
void checkSpecialKeys() {
    // Список читается редактором при создании — правим оформление ДО него.
    const auto saved = zametti::settings().editor().specialKeys();
    zametti::mutableSettingsForTests().editor().setSpecialKeys({
        {QStringLiteral("Alt+-"), QStringLiteral("—")},
        {QStringLiteral("Ctrl+Alt+G"), QStringLiteral("→")},
        {QStringLiteral("Ctrl+Alt+T"), QStringLiteral("тчк")},   // замена может быть строкой
    });

    zametti::NoteEditor editor;
    editor.resize(700, 500);
    editor.show();
    QTest::qWait(20);
    editor.setFocus();

    const QString path = writeNote("тире.md", QStringLiteral("раз\n"));
    editor.openFile(path);
    QTest::qWait(20);

    QTextCursor end(editor.document());
    end.movePosition(QTextCursor::End);
    editor.setTextCursor(end);
    typeText(editor, QStringLiteral(" "));
    QTest::keyClick(&editor, Qt::Key_Minus, Qt::AltModifier);
    typeText(editor, QStringLiteral(" два "));
    QTest::keyClick(&editor, Qt::Key_G, Qt::ControlModifier | Qt::AltModifier);
    typeText(editor, QStringLiteral(" три "));
    QTest::keyClick(&editor, Qt::Key_T, Qt::ControlModifier | Qt::AltModifier);
    QTest::qWait(10);

    ZT_TRUE("замены встали в текст",
            editor.document()->firstBlock().text() ==
                QStringLiteral("раз — два → три тчк"));

    // Это обычный набор, а не операция: в файл всё уходит как есть.
    editor.save(false);
    QTest::qWait(20);
    ZT_TRUE("в файле то же самое",
            textOf(editor).contains(QStringLiteral("раз — два → три тчк")));

    // Сочетание, которого в списке нет, ничего не вставляет. Клавиша нужна
    // такая, у которой нет собственного знака: Ctrl+Alt+J, например, Qt
    // сопровождает переводом строки, и проверка ловила бы его, а не замену.
    QTest::keyClick(&editor, Qt::Key_F7, Qt::ControlModifier | Qt::AltModifier);
    QTest::qWait(10);
    ZT_TRUE("чужое сочетание молчит",
            editor.document()->firstBlock().text() ==
                QStringLiteral("раз — два → три тчк"));

    zametti::mutableSettingsForTests().editor().setSpecialKeys(saved);
}

// Ctrl+Shift+E: крайние пустые строки выделения не входят в блок кода —
// клавиатурное выделение легко цепляет соседний VSpace, и код съедал
// отбивку у черты сверху (сценарий владельца).
void checkCodeToggleTrimsBlankEdges() {
    zametti::NoteEditor editor;
    editor.resize(700, 500);
    editor.show();
    QTest::qWait(20);
    editor.setFocus();
    const QString path = writeNote(
        "код-под-чертой.md",
        QStringLiteral("___\n\nimport sys\nprint(sys.argv)\n\nхвост\n"));
    editor.openFile(path);
    QTest::qWait(20);

    QTextBlock program;
    for (QTextBlock b = editor.document()->begin(); b.isValid(); b = b.next())
        if (b.text().contains(QStringLiteral("import sys"))) { program = b; break; }
    ZT_TRUE("программа найдена", program.isValid());

    // Якорь на пустой строке НАД программой (как выходит Shift-стрелками),
    // конец — за последней строкой (на начале строки ниже).
    QTextCursor sel(editor.document());
    sel.setPosition(program.position() - 1);
    sel.setPosition(program.position() + program.length(), QTextCursor::KeepAnchor);
    editor.setTextCursor(sel);
    QTest::keyClick(&editor, Qt::Key_E, Qt::ControlModifier | Qt::ShiftModifier);
    QTest::qWait(10);

    const std::string out =
        markdownOf(blocksOf(*editor.document()));
    ZT_TRUE("пустая строка над кодом уцелела",
            out.find("___\n\n```") != std::string::npos);
    ZT_TRUE("в коде ровно программа",
            out.find("```\nimport sys\nprint(sys.argv)\n```") != std::string::npos);
    ZT_TRUE("хвост не тронут", out.find("\n\nхвост\n") != std::string::npos);
}

void checkOrderedMarkerFaces() {
    const zametti::MarkerStyle ordered{zametti::Marker::Ordered, false};
    const auto face = [&](int ordinal, int level) {
        return zametti::markerText(ordered, ordinal, level).toStdString();
    };
    ZT_EQ("уровень 0 — арабские с точкой", std::string("1."), face(1, 0));
    ZT_EQ("уровень 1 — буквы", std::string("a."), face(1, 1));
    ZT_EQ("уровень 1, номер 26 — z", std::string("z."), face(26, 1));
    ZT_EQ("уровень 1, номер 27 — aa", std::string("aa."), face(27, 1));
    ZT_EQ("уровень 1, номер 702 — zz", std::string("zz."), face(702, 1));
    ZT_EQ("уровень 1, номер 703 — aaa", std::string("aaa."), face(703, 1));
    ZT_EQ("уровень 2 — цифры со скобкой", std::string("2)"), face(2, 2));
    ZT_EQ("уровень 3 — круг замкнулся", std::string("3."), face(3, 3));
    ZT_EQ("уровень 4 — снова буквы", std::string("b."), face(2, 4));
}

static int ztRunSuite(int argc, char** argv) {
    (void)argc;
    (void)argv;
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
    for (const Case& c : kTypeOnBlankCases) run(c);
    for (const Case& c : kBackspaceUpCases) run(c);
    checkWhitespaceLineTidy();
    checkDividerUndo();
    checkBackspaceProperties();
    checkUndo();
    checkListRhythm();
    checkKindRoundTripKeepsPlace();
    checkOrderedMarkerFaces();
    checkCommentOps();
    checkInputRuleSplitsLine();
    checkNumberSignHeadings();
    checkSpecialKeys();
    checkCodeToggleTrimsBlankEdges();

    fs::remove_all(g_dir);
    return zt::report("списки");
}

// Набор целиком одним TEST: тело не тронуто, argv ему собран здесь.
// Дробить на отдельные проверки — отдельная работа, по одному набору.
TEST(List, All) {
    std::vector<QByteArray> ztArgs{QByteArrayLiteral("list_test")};
    std::vector<char*> ztArgv;
    for (QByteArray& a : ztArgs) ztArgv.push_back(a.data());
    EXPECT_EQ(0, ztRunSuite(int(ztArgv.size()), ztArgv.data()));
}
