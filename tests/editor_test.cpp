// Виджет редактора на живом окне: что записывается в историю, а что нет.
//
// Единственный тест, которому нужен QtWidgets. Бриф просил обойтись без них, и
// слой модели без них и обходится — за этим следит view-without-widgets. Но обе
// ошибки, ради которых этот тест написан, живут именно в виджете и никаким
// тестом над QTextDocument не ловятся: перекладка полей под ширину окна
// записывалась в историю как правка, а первая правка в только что открытой
// заметке подмешивалась к её исходному состоянию и не отменялась.

#include "doc_model.h"
#include "document_reader.h"
#include "editor_widget.h"
#include "journal.h"
#include "marker.h"
#include "serializer.h"
#include "settings.h"
#include "test_util.h"

#include <vector>
#include "testdata.h"

#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QTest>
#include <QTextBlock>
#include <QAbstractTextDocumentLayout>
#include <QFontMetricsF>
#include <QHash>
#include <QImage>
#include <QPainter>
#include <QLineEdit>
#include <QListWidget>
#include <QScrollBar>
#include <QVBoxLayout>
#include <QTextCursor>
#include <QTextFragment>
#include <QTextFrame>
#include <QTextLayout>
#include <QTextLine>
#include <QTextDocument>

#include <cmath>
#include <string>
#include <tuple>

namespace {

QString g_dir;

void check(bool ok, const std::string& what) {
    ++zt::g_checks;
    if (ok) return;
    ++zt::g_failures;
    std::printf("провал: %s\n", what.c_str());
}

void checkEqual(const QString& expected, const QString& actual, const std::string& what) {
    ++zt::g_checks;
    if (expected == actual) return;
    ++zt::g_failures;
    std::printf("провал: %s\n  ждали:  %s\n  вышло:  %s\n", what.c_str(),
                expected.toUtf8().constData(), actual.toUtf8().constData());
}

QString readFile(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return QStringLiteral("<нет файла>");
    return QString::fromUtf8(file.readAll());
}

QString writeNote(const char* name, const QString& text) {
    const QString path = g_dir + QLatin1Char('/') + QLatin1String(name);
    QFile file(path);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) file.write(text.toUtf8());
    return path;
}

QString firstLine(const zametti::NoteEditor& editor) {
    return editor.document()->firstBlock().text();
}

void typeAtEnd(zametti::NoteEditor& editor, const QString& text) {
    QTextCursor cursor = editor.textCursor();
    cursor.movePosition(QTextCursor::EndOfBlock);
    editor.setTextCursor(cursor);
    editor.insertPlainText(text);
    QTest::qWait(10);
}

// Просто открыть заметку — не значит её изменить. Ошибка здесь означала бы, что
// чтение чужого файла его переписывает.
void checkOpenDoesNotTouchFile() {
    const QString source = QStringLiteral("#   заголовок с лишними пробелами\n\n*   буллет\n");
    const QString path = writeNote("некано.md", source);

    zametti::NoteEditor editor;
    editor.resize(700, 500);
    editor.show();
    QTest::qWait(20);
    editor.openFile(path);
    QTest::qWait(20);
    editor.save(false);

    checkEqual(source, readFile(path), "открытие заметки не должно её менять");

    // ЗАМЕТКА С ФОРМУЛАМИ — тот же вопрос, но на разметке, которую круг через
    // документ портил молча. Владелец нашёл это, архивируя `Typesetting Math`:
    // каждое открытие переписывало файл, `\gamma` уезжал как `\\gamma`, и
    // косые удваивались с каждым разом.
    const QString math =
        QStringLiteral("# Формулы\n\nСтрочная $\\gamma$ и $\\int_0^1 x^2 \\, dx$ в тексте.\n"
                       "\nВыключная:\n\n$$\\sum_{k=0}^\\infty \\frac{x^k}{k!}$$\n"
                       "\nИ ещё $\\sqrt{1+\\sqrt{2}}$.\n");
    const QString mathPath = writeNote("формулы.md", math);
    zametti::NoteEditor second;
    second.resize(700, 500);
    second.show();
    QTest::qWait(20);
    for (int round = 1; round <= 3; ++round) {
        second.openFile(mathPath);
        QTest::qWait(30);
        second.save(false);
        QTest::qWait(20);
        checkEqual(math, readFile(mathPath),
                   "заметка с формулами не меняется от открытия (круг " +
                       std::to_string(round) + ")");
    }

    // ОТСТУПЫ ВНУТРИ ФОРМУЛЫ ОСТАЮТСЯ ОБЫЧНЫМИ ПРОБЕЛАМИ. Сохранятель делает
    // ведущие пробелы неразрывными — отступ значим, им рисуют схемы. Формула,
    // записанная в несколько строк, попала под это правило заодно: её строки
    // уехали в файл с U+00A0, а движок рисует такие пробелы настоящими —
    // матрица разъезжается дырами. Владелец нашёл это на своей заметке.
    const QString matrix = QStringLiteral(
        "текст\n\n$$D = \\left[\n    \\begin{matrix} a & b \\end{matrix}\n"
        "    \\right].$$\n\nдальше\n");
    const QString matrixPath = writeNote("матрица.md", matrix);
    second.openFile(matrixPath);
    QTest::qWait(30);
    QTextCursor typing = second.textCursor();
    typing.movePosition(QTextCursor::End);
    second.setTextCursor(typing);
    QTest::keyClicks(&second, QStringLiteral("x"));
    QTest::qWait(20);
    second.save(false, true);
    QTest::qWait(20);
    const QString saved = readFile(matrixPath);
    check(!saved.contains(QChar(0x00A0)), "в формуле не появилось неразрывных пробелов");
    check(saved.contains(QStringLiteral("    \\begin{matrix}")),
          "отступ внутри формулы сохранён как есть");

    // И ПОМЕТКА АРХИВА ПЕРЕЖИВАЕТ ОТКРЫТИЕ. Стаб архивной заметки — обычный
    // файл, и канонизация при открытии не вправе потерять ни одного ключа
    // шапки: потеряет `archived` — заметка выпадет из архива, а «Архив» в
    // дереве собирается из помеченных и исчезнет вместе с ней.
    const QString stub = QStringLiteral(
        "<!-- zametti\nid: 01arch\ncreated: 2026-01-01T00:00:00+03:00\n"
        "modified: 2026-01-02T00:00:00+03:00\narchived: yes\n-->\n\n# Архивная\n");
    const QString stubPath = writeNote("стаб.md", stub);
    second.openFile(stubPath);
    QTest::qWait(30);
    second.save(false);
    QTest::qWait(20);
    check(readFile(stubPath).contains(QStringLiteral("archived: yes")),
          "пометка архива переживает открытие и запись");
}

// Слова и строки: когда они верны, когда честно неизвестны и когда снова верны.
//
// Проверяется именно ПОВЕДЕНИЕ, а не арифметика (её проверяет text_stats_test):
// после открытия число слов есть, после нажатия клавиши оно объявлено
// устаревшим, после записи — снова есть и уже другое.
void checkStatsFreshness() {
    const QString path = writeNote("счёт-слов.md", QStringLiteral("раз два три\n"));

    zametti::NoteEditor editor;
    editor.resize(700, 500);
    editor.show();
    QTest::qWait(20);

    int notified = 0;
    QObject::connect(&editor, &zametti::NoteEditor::statsChanged, &editor, [&] { ++notified; });

    editor.openFile(path);
    QTest::qWait(20);
    check(editor.statsFresh(), "после открытия число слов известно");
    check(editor.stats().words == 3, "три слова в открытой заметке");
    check(editor.stats().lines == 1, "одна строка");
    const int afterOpen = notified;

    // Каретка в конце: набранное в начале слиплось бы с первым словом, и
    // «шесть слов» означало бы пять. На этом и попался первый заход.
    QTextCursor caret = editor.textCursor();
    caret.movePosition(QTextCursor::End);
    editor.setTextCursor(caret);

    QTest::keyClicks(&editor, QStringLiteral(" chetyre"));
    QTest::qWait(20);
    check(!editor.statsFresh(), "после набора число слов объявлено устаревшим");
    check(notified > afterOpen, "об устаревании окно узнаёт сигналом, а не опросом");
    check(editor.stats().words == 3, "устаревшее число не подменяется догадкой");

    // Ещё десяток нажатий: сигнал об устаревании обязан быть ОДИН на серию,
    // иначе окно перерисовывалось бы на каждую букву.
    const int afterFirstKey = notified;
    QTest::keyClicks(&editor, QStringLiteral(" pyat shest"));
    QTest::qWait(20);
    check(notified == afterFirstKey, "на каждое нажатие сигнала нет");

    editor.save(false);
    QTest::qWait(20);
    check(editor.statsFresh(), "после записи число слов снова известно");
    checkEqual(QStringLiteral("6"), QString::number(editor.stats().words),
               "шесть слов после набора");
}

// Выделение над кодом обязано отличаться от выделения над бумагой: иначе
// Ctrl+E на выделенном тексте не меняет на экране ровным счётом ничего.
//
// Меряем цвет ВНУТРИ выделенного текста, а не «где-нибудь в строке»: две мои
// прежние попытки ловили то край сглаженной буквы, то фон СНАРУЖИ выделения —
// и проходили даже с непрозрачной заливкой, то есть не проверяли ничего.
void checkSelectionShowsCode() {
    // И блок кода, и СТРОЧНЫЙ код: Ctrl+E на выделении делает именно строчный,
    // и починка, лечившая только блоки, у владельца ничего не изменила.
    const QString path =
        writeNote("выделение.md",
                  QStringLiteral("обычный текст\n\nстрока с `кодом` внутри\n\n"
                                 "```\nкод кода\n```\n"));
    zametti::NoteEditor editor;
    editor.resize(700, 400);
    editor.show();
    // Ответ не выбрасываем: не показанное окно делает бессмысленным всё, что
    // ниже, — снимок брать неоткуда. В Debug это ещё и -Werror.
    ZT_TRUE("окно редактора показалось", QTest::qWaitForWindowExposed(&editor));
    editor.openFile(path);
    QTest::qWait(50);

    QTextCursor all = editor.textCursor();
    all.select(QTextCursor::Document);
    editor.setTextCursor(all);
    QTest::qWait(50);

    QImage frame(editor.viewport()->size(), QImage::Format_RGB32);
    frame.fill(Qt::white);
    QPainter painter(&frame);
    editor.viewport()->render(&painter);
    painter.end();

    // Цвет заливки ВНУТРИ ТЕКСТА строки: у блока кода есть отступ, и левая
    // часть его прямоугольника — это фон вне выделения. Две мои прежние
    // попытки мерили именно её и проходили со снятой починкой.
    const auto fillOn = [&](const QTextBlock& block) {
        const QRectF box = editor.document()->documentLayout()->blockBoundingRect(block);
        const QTextLayout* layout = block.layout();
        if (layout == nullptr || layout->lineCount() == 0) return QColor();
        const QTextLine line = layout->lineAt(0);
        const qreal x0 = line.cursorToX(0);
        const qreal x1 = line.cursorToX(block.length() - 1);
        const int y = int(box.top() + line.y() + line.height() / 2) -
                      editor.verticalScrollBar()->value();
        if (y < 0 || y >= frame.height()) return QColor();

        QHash<QRgb, int> seen;
        for (int x = int(box.left() + qMin(x0, x1)) + 1;
             x < int(box.left() + qMax(x0, x1)) && x < frame.width(); ++x)
            if (x >= 0) ++seen[frame.pixel(x, y)];
        QRgb best = 0;
        int most = 0;
        for (auto it = seen.begin(); it != seen.end(); ++it)
            if (it.value() > most) {
                most = it.value();
                best = it.key();
            }
        return most > 0 ? QColor(best) : QColor();
    };

    QTextBlock paper;
    QTextBlock code;
    for (QTextBlock b = editor.document()->begin(); b.isValid(); b = b.next()) {
        if (zametti::kindOf(b) == zametti::Kind::Code) code = b;
        else if (!b.text().isEmpty() && !paper.isValid()) paper = b;
    }
    check(paper.isValid() && code.isValid(), "в заметке есть и текст, и код");
    if (!paper.isValid() || !code.isValid()) return;

    const QColor onPaper = fillOn(paper);
    const QColor onCode = fillOn(code);
    check(onPaper.isValid() && onCode.isValid(), "заливка выделения видна на обеих строках");
    if (!onPaper.isValid() || !onCode.isValid()) return;

    // Опоры «заливка равна цвету выделения» здесь нет и быть не может:
    // внутри текста преобладает не чистая заливка, а её смесь со сглаженными
    // буквами (замер даёт #308cc6 при цвете выделения #bfdbfe). Поэтому
    // сравниваем ДВЕ строки между собой — и требуем, чтобы разница шла в
    // нужную сторону.
    // А над кодом она обязана быть ДРУГОЙ и темнее: подложка кода —
    // полупрозрачный чёрный поверх синевы.
    check(onCode != onPaper, "над кодом выделение выглядит иначе");
    check(onCode.red() < onPaper.red() && onCode.blue() < onPaper.blue(),
          "и именно темнее: подложка кода подкрасила выделение");

    // Строчный код: тот же вопрос к абзацу, где код — лишь кусок строки.
    QTextBlock inlineCode;
    for (QTextBlock b = editor.document()->begin(); b.isValid(); b = b.next())
        if (b.text().contains(QStringLiteral("кодом"))) inlineCode = b;
    check(inlineCode.isValid(), "абзац со строчным кодом найден");
    if (!inlineCode.isValid()) return;

    // Цвет НА САМОМ куске кода и рядом с ним, в одной и той же строке.
    const QRectF box =
        editor.document()->documentLayout()->blockBoundingRect(inlineCode);
    const QTextLine line = inlineCode.layout()->lineAt(0);
    const int y = int(box.top() + line.y() + line.height() / 2) -
                  editor.verticalScrollBar()->value();
    int codeAt = -1;
    int plainAt = -1;
    for (QTextBlock::iterator it = inlineCode.begin(); !it.atEnd(); ++it) {
        const QTextFragment fragment = it.fragment();
        if (!fragment.isValid() || fragment.length() < 2) continue;
        const int middle = fragment.position() - inlineCode.position() + fragment.length() / 2;
        const int x = int(box.left() + line.cursorToX(middle));
        if (fragment.charFormat().background().style() != Qt::NoBrush) codeAt = x;
        else if (plainAt < 0) plainAt = x;
    }
    check(codeAt > 0 && plainAt > 0, "в строке есть и код, и обычный текст");
    if (codeAt <= 0 || plainAt <= 0 || y < 0 || y >= frame.height()) return;
    // Мерим у НИЗА строки, а не по её середине: посередине стоят буквы, и
    // одна точка попадает в глиф, а не в заливку. Первый заход мерил именно
    // так и молчал при снятой починке — то есть не проверял ничего.
    const int low = int(box.top() + line.y() + line.height()) - 2 -
                    editor.verticalScrollBar()->value();
    if (low < 0 || low >= frame.height()) return;
    const QColor onInline = frame.pixelColor(codeAt, low);
    const QColor beside = frame.pixelColor(plainAt, low);
    check(onInline != beside, "строчный код под выделением выглядит иначе, чем текст рядом");
    check(onInline.red() < beside.red(), "и именно темнее: это подложка кода");
}

// Метаданные заметки редактор не видит — их нет в QTextDocument, — но терять
// при сохранении не имеет права: в parent живёт место заметки в дереве.
void checkMetaSurvivesEditing() {
    const QString source = QStringLiteral(
        "<!-- zametti\n"
        "parent: 01n6x9k2m4qp\n"
        "неизвестный: ключ\n"
        "-->\n"
        "\n"
        "# Заголовок\n");
    const QString path = writeNote("с-метаданными.md", source);

    zametti::NoteEditor editor;
    editor.resize(700, 500);
    editor.show();
    QTest::qWait(20);
    editor.setFocus();
    editor.openFile(path);
    QTest::qWait(20);

    check(editor.document()->firstBlock().text() == QStringLiteral("Заголовок"),
          "метаданных в документе нет, первый блок — заголовок");

    QTextCursor cursor = editor.textCursor();
    cursor.movePosition(QTextCursor::End);
    editor.setTextCursor(cursor);
    editor.insertPlainText(QStringLiteral(" дописан"));
    QTest::qWait(10);
    editor.save(false);

    // modified редактор дописывает при настоящем сохранении — его метка
    // времени плавает, сверяем всё вокруг неё.
    const QString saved = readFile(path);
    QString stripped = saved;
    stripped.replace(QRegularExpression(QStringLiteral("modified: [0-9T:Z+-]+\\n")),
                     QString());
    checkEqual(QStringLiteral("<!-- zametti\n"
                              "parent: 01n6x9k2m4qp\n"
                              "неизвестный: ключ\n"
                              "-->\n"
                              "\n"
                              "# Заголовок дописан\n"),
               stripped, "правка текста не теряет и не двигает метаданные");
    check(saved.contains(QStringLiteral("modified: ")),
          "сохранение проставило modified");

    // Правка одной меты (перенос, корзина, восстановление) modified не
    // трогает: заметка не должна всплывать наверх списка от переноса.
    const QString stamped = readFile(path);
    QTest::qWait(1100);   // чтобы возможный новый штамп отличался секундой
    editor.editMeta([](zametti::NoteMeta& meta) {
        meta.set("parent", "01n6x9k2m4qp");
    });
    QTest::qWait(10);
    const QString afterMeta = readFile(path);
    check(afterMeta.contains(QStringLiteral("parent: 01n6x9k2m4qp")),
          "мета-правка записана");
    QRegularExpression stamp(QStringLiteral("modified: ([0-9T:Z-]+)"));
    check(stamp.match(stamped).captured(1) == stamp.match(afterMeta).captured(1),
          "modified не изменился от мета-правки");
}

// Правило этапа: документ — содержимое, а не облик. Undo возвращает текст и не
// трогает масштаб.
void checkUndoKeepsAppearance() {
    const QString path = writeNote("правка.md", QStringLiteral("первая строка\n\n- буллет\n"));

    zametti::NoteEditor editor;
    editor.resize(700, 500);
    editor.show();
    QTest::qWait(20);
    editor.openFile(path);

    typeAtEnd(editor, QStringLiteral(" мама мыла раму"));
    checkEqual(QStringLiteral("первая строка мама мыла раму"), firstLine(editor),
               "набранное должно оказаться в документе");

    editor.applyZoom(2.0);
    QTest::qWait(10);
    checkEqual(QStringLiteral("первая строка мама мыла раму"), firstLine(editor),
               "масштаб не должен менять текст");

    editor.undo();
    QTest::qWait(10);
    checkEqual(QStringLiteral("первая строка"), firstLine(editor),
               "undo возвращает текст");
    check(editor.zoom() == qreal(2.0), "undo не должен откатывать масштаб");

    editor.redo();
    QTest::qWait(10);
    checkEqual(QStringLiteral("первая строка мама мыла раму"), firstLine(editor),
               "redo возвращает отменённое");

    editor.save(false);
    checkEqual(QStringLiteral("первая строка мама мыла раму\n\n- буллет\n"), readFile(path),
               "сохранённое содержимое");
}

// Смена облика шага истории не заводит: после зума и перекладки окна одного
// undo обязано хватить, чтобы вернуться к исходному тексту.
void checkAppearanceMakesNoHistoryStep() {
    const QString path = writeNote("облик.md", QStringLiteral("текст\n"));

    zametti::NoteEditor editor;
    editor.resize(700, 500);
    editor.show();
    QTest::qWait(20);
    editor.openFile(path);

    typeAtEnd(editor, QStringLiteral(" правка"));
    // Пауза дольше окна слипания: иначе смена облика подмешалась бы к серии
    // набора и завела бы не новый шаг, а правку текущего — то есть проверка
    // прошла бы и со сломанным кодом.
    QTest::qWait(zametti::appearance().undoCoalesceMs * 2);

    // Всё, что ниже, — облик: масштаб и ширина окна. Ширина особенно коварна:
    // она двигает поля документа, а документ шлёт contentsChanged и на это.
    // Ширины взяты по обе стороны от предела колонки (layout.maxContentWidth),
    // иначе поля не сдвинутся вовсе и проверять будет нечего.
    editor.applyZoom(1.5);
    QTest::qWait(10);
    editor.resize(600, 500);
    QTest::qWait(30);
    editor.resize(1600, 500);
    QTest::qWait(30);
    editor.resize(700, 500);
    QTest::qWait(30);
    editor.applyZoom(1.0);
    QTest::qWait(10);

    editor.undo();
    QTest::qWait(10);
    checkEqual(QStringLiteral("текст"), firstLine(editor),
               "одного undo хватает: облик шагов не заводит");
}

// Первая правка в только что открытой заметке обязана отменяться. Если серия
// набора тянется из прошлой заметки, эта правка сольётся с её исходным
// состоянием, и отменять станет нечего.
void checkFirstEditAfterOpenIsUndoable() {
    const QString first = writeNote("одна.md", QStringLiteral("одна\n"));
    const QString second = writeNote("другая.md", QStringLiteral("другая\n"));

    zametti::NoteEditor editor;
    editor.resize(700, 500);
    editor.show();
    QTest::qWait(20);

    editor.openFile(first);
    typeAtEnd(editor, QStringLiteral(" правка"));

    // Сразу, без паузы: как раз тот случай, когда серия набора могла бы
    // перетечь в новую заметку.
    editor.openFile(second);
    typeAtEnd(editor, QStringLiteral(" ещё"));
    checkEqual(QStringLiteral("другая ещё"), firstLine(editor), "правка во второй заметке");

    editor.undo();
    QTest::qWait(10);
    checkEqual(QStringLiteral("другая"), firstLine(editor),
               "первая правка после открытия обязана отменяться");

    editor.save(false);
    checkEqual(QStringLiteral("одна правка\n"), readFile(first),
               "первая заметка сохранена при переходе ко второй");
}

// Клавиши доходят до операций, и каждая операция — ровно один шаг отмены.
void checkKeysAreOperations() {
    const QString path = writeNote("клавиши.md", QStringLiteral("- пункт\n"));

    zametti::NoteEditor editor;
    editor.resize(700, 500);
    editor.show();
    QTest::qWait(20);
    editor.openFile(path);

    QTextCursor cursor = editor.textCursor();
    cursor.movePosition(QTextCursor::End);
    editor.setTextCursor(cursor);

    QTest::keyClick(&editor, Qt::Key_Return);
    QTest::qWait(10);
    check(editor.document()->blockCount() == 2, "Enter завёл новый пункт");
    check(zametti::isListBlock(editor.document()->findBlockByNumber(1)),
          "новый блок — пункт списка");

    // Набираем в новом пункте и убеждаемся, что это отдельный шаг.
    editor.insertPlainText(QStringLiteral("второй"));
    QTest::qWait(10);

    editor.undo();
    QTest::qWait(10);
    checkEqual(QStringLiteral("- пункт\n-\n"),
               QString::fromStdString(zametti::serialize(
                   zametti::readDocument(*editor.document()))),
               "первый undo снимает набор, но не Enter");

    editor.undo();
    QTest::qWait(10);
    checkEqual(QStringLiteral("- пункт\n"),
               QString::fromStdString(zametti::serialize(
                   zametti::readDocument(*editor.document()))),
               "второй undo снимает Enter");

    // Backspace в начале пункта снимает список — тоже одним шагом.
    editor.redo();
    QTest::qWait(10);
    cursor = editor.textCursor();
    cursor.setPosition(editor.document()->findBlockByNumber(1).position());
    editor.setTextCursor(cursor);
    QTest::keyClick(&editor, Qt::Key_Backspace);
    QTest::qWait(10);
    check(!zametti::isListBlock(editor.document()->findBlockByNumber(1)),
          "Backspace в начале пункта снял список");

    editor.undo();
    QTest::qWait(10);
    check(zametti::isListBlock(editor.document()->findBlockByNumber(1)),
          "undo вернул пункт списком");
}

// Tab, Shift+Tab и переключатель задачи доходят до операций через клавиатуру.
void checkListKeys() {
    const QString path =
        writeNote("списки.md", QStringLiteral("- раз\n- два\n- [ ] дело\n"));

    zametti::NoteEditor editor;
    editor.resize(700, 500);
    editor.show();
    QTest::qWait(20);
    editor.openFile(path);

    auto putCursorIn = [&editor](int block) {
        QTextCursor cursor = editor.textCursor();
        cursor.setPosition(editor.document()->findBlockByNumber(block).position());
        editor.setTextCursor(cursor);
    };
    auto text = [&editor] {
        return QString::fromStdString(
            zametti::serialize(zametti::readDocument(*editor.document())));
    };

    putCursorIn(1);
    QTest::keyClick(&editor, Qt::Key_Tab);
    QTest::qWait(10);
    checkEqual(QStringLiteral("- раз\n  - два\n- [ ] дело\n"), text(),
               "Tab увёл пункт на уровень внутрь");

    QTest::keyClick(&editor, Qt::Key_Backtab);
    QTest::qWait(10);
    checkEqual(QStringLiteral("- раз\n- два\n- [ ] дело\n"), text(),
               "Shift+Tab вернул его обратно");

    // Переключатель задачи — сочетание из конфига.
    const QKeySequence toggle(zametti::appearance().toggleTaskKey,
                              QKeySequence::PortableText);
    check(toggle.count() == 1, "хоткей переключателя разобран");
    putCursorIn(2);
    QTest::keyClick(&editor, Qt::Key(toggle[0].key()), toggle[0].keyboardModifiers());
    QTest::qWait(10);
    checkEqual(QStringLiteral("- раз\n- два\n- [x] дело\n"), text(),
               "задача отмечена");

    QTest::keyClick(&editor, Qt::Key(toggle[0].key()), toggle[0].keyboardModifiers());
    QTest::qWait(10);
    checkEqual(QStringLiteral("- раз\n- два\n- [ ] дело\n"), text(),
               "и снята");

    // Каждое нажатие — свой шаг истории.
    editor.undo();
    QTest::qWait(10);
    checkEqual(QStringLiteral("- раз\n- два\n- [x] дело\n"), text(),
               "undo снимает ровно последнее переключение");
}

// Перемещение пунктов с клавиатуры, вместе с поддеревом и с курсором.
void checkMoveKeys() {
    const QString path = writeNote(
        "перестановка.md", QStringLiteral("- раз\n  - вложенный\n- два\n- три\n"));

    zametti::NoteEditor editor;
    editor.resize(700, 500);
    editor.show();
    QTest::qWait(20);
    editor.openFile(path);

    auto text = [&editor] {
        return QString::fromStdString(
            zametti::serialize(zametti::readDocument(*editor.document())));
    };
    auto press = [&editor](const QString& keys) {
        const QKeySequence sequence(keys, QKeySequence::PortableText);
        QTest::keyClick(&editor, Qt::Key(sequence[0].key()),
                        sequence[0].keyboardModifiers());
        QTest::qWait(10);
    };

    // Курсор в первом пункте, у которого есть вложенный.
    QTextCursor cursor = editor.textCursor();
    cursor.setPosition(editor.document()->findBlockByNumber(0).position() + 2);
    editor.setTextCursor(cursor);

    press(zametti::appearance().moveDownKey);
    checkEqual(QStringLiteral("- два\n- раз\n  - вложенный\n- три\n"), text(),
               "пункт уехал вниз вместе с вложенным");
    checkEqual(QStringLiteral("раз"), editor.textCursor().block().text(),
               "курсор остался в перемещённом пункте");
    check(editor.textCursor().positionInBlock() == 2, "и на том же месте в нём");

    press(zametti::appearance().moveUpKey);
    checkEqual(QStringLiteral("- раз\n  - вложенный\n- два\n- три\n"), text(),
               "и вернулся обратно");

    // Каждое перемещение — свой шаг истории.
    editor.undo();
    QTest::qWait(10);
    checkEqual(QStringLiteral("- два\n- раз\n  - вложенный\n- три\n"), text(),
               "undo отменяет ровно последнее перемещение");
}

// Начертание: на выделение — правка документа, без выделения — формат для
// следующей буквы.
void checkInlineStyle() {
    const QString path = writeNote("начертание.md", QStringLiteral("обычный текст\n"));

    zametti::NoteEditor editor;
    editor.resize(700, 500);
    editor.show();
    QTest::qWait(20);
    editor.openFile(path);

    auto text = [&editor] {
        return QString::fromStdString(
            zametti::serialize(zametti::readDocument(*editor.document())));
    };

    // На выделение.
    QTextCursor cursor = editor.textCursor();
    cursor.setPosition(editor.document()->firstBlock().position());
    cursor.setPosition(editor.document()->firstBlock().position() + 7,
                       QTextCursor::KeepAnchor);
    editor.setTextCursor(cursor);
    QTest::keyClick(&editor, Qt::Key_B, Qt::ControlModifier);
    QTest::qWait(10);
    checkEqual(QStringLiteral("**обычный** текст\n"), text(), "Ctrl+B на выделении");

    editor.undo();
    QTest::qWait(10);
    checkEqual(QStringLiteral("обычный текст\n"), text(), "и отменяется одним шагом");

    // Без выделения: следующая набранная буква идёт жирной.
    cursor = editor.textCursor();
    cursor.movePosition(QTextCursor::End);
    editor.setTextCursor(cursor);
    QTest::keyClick(&editor, Qt::Key_B, Qt::ControlModifier);
    QTest::qWait(10);
    editor.insertPlainText(QStringLiteral(" жирное"));
    QTest::qWait(10);
    checkEqual(QStringLiteral("обычный текст** жирное**\n"), text(),
               "набор после Ctrl+B идёт жирным");
}

// Автозамена при наборе и её отдельный шаг отмены.
void checkInputRules() {
    const QString path = writeNote("автозамена.md", QStringLiteral("текст\n"));

    zametti::NoteEditor editor;
    editor.resize(700, 500);
    editor.show();
    QTest::qWait(20);
    editor.openFile(path);

    auto text = [&editor] {
        return QString::fromStdString(
            zametti::serialize(zametti::readDocument(*editor.document())));
    };

    QTextCursor cursor = editor.textCursor();
    cursor.setPosition(editor.document()->firstBlock().position());
    editor.setTextCursor(cursor);

    // Звёздочка — то, что просили: способ ввода, в файл уходит дефис.
    QTest::keyClick(&editor, Qt::Key_Asterisk);
    QTest::keyClick(&editor, Qt::Key_Space);
    QTest::qWait(10);
    checkEqual(QStringLiteral("- текст\n"), text(), "звёздочка с пробелом дала буллет");

    // Первый Ctrl+Z возвращает набранное, а не отменяет всё сразу.
    editor.undo();
    QTest::qWait(10);
    checkEqual(QStringLiteral("\\* текст\n"), text(),
               "первый undo возвращает набранные знаки");

    // Шаг отмены — СЛОВО: звёздочка и пробел после неё лежат в разных шагах.
    editor.undo();
    QTest::qWait(10);
    checkEqual(QStringLiteral("\\*текст\n"), text(), "второй undo снимает пробел");
    editor.undo();
    QTest::qWait(10);
    checkEqual(QStringLiteral("текст\n"), text(), "третий undo снимает саму звёздочку");
}

// Выделение обязано пережить операцию: она могла тронуть десяток пунктов, и
// терять его после этого — значит заставлять выделять заново.
void checkSelectionSurvivesOperation() {
    const QString path = writeNote(
        "выделение.md", QStringLiteral("- [ ] раз\n- [ ] два\n- [ ] три\n"));

    zametti::NoteEditor editor;
    editor.resize(700, 500);
    editor.show();
    QTest::qWait(20);
    editor.openFile(path);

    auto text = [&editor] {
        return QString::fromStdString(
            zametti::serialize(zametti::readDocument(*editor.document())));
    };

    QTextCursor cursor = editor.textCursor();
    cursor.setPosition(editor.document()->findBlockByNumber(0).position());
    const QTextBlock last = editor.document()->findBlockByNumber(2);
    cursor.setPosition(last.position() + last.length() - 1, QTextCursor::KeepAnchor);
    editor.setTextCursor(cursor);
    const int anchor = editor.textCursor().anchor();
    const int position = editor.textCursor().position();

    const QKeySequence toggle(zametti::appearance().toggleTaskKey,
                              QKeySequence::PortableText);
    QTest::keyClick(&editor, Qt::Key(toggle[0].key()), toggle[0].keyboardModifiers());
    QTest::qWait(10);

    checkEqual(QStringLiteral("- [x] раз\n- [x] два\n- [x] три\n"), text(),
               "переключились все три задачи");
    check(editor.textCursor().hasSelection(), "выделение обязано остаться");
    check(editor.textCursor().anchor() == anchor && editor.textCursor().position() == position,
          "и остаться на прежних границах");

    // Второе нажатие подряд должно снять отметки со всех — то есть выделение
    // действительно живо, а не просто «что-то выделено».
    QTest::keyClick(&editor, Qt::Key(toggle[0].key()), toggle[0].keyboardModifiers());
    QTest::qWait(10);
    checkEqual(QStringLiteral("- [ ] раз\n- [ ] два\n- [ ] три\n"), text(),
               "второе нажатие снимает отметки со всех");
}

// Отмена с клавиатуры, а не вызовом метода. Разница не умозрительная: QTextEdit
// объявляет Ctrl+Z своим и глотает его, так что ярлык окна не срабатывает ни
// разу — а тест, зовущий undo() напрямую, этого не замечает. Именно так ошибка
// и прожила незамеченной.
void checkUndoFromKeyboard() {
    const QString path = writeNote("отмена-клавишей.md", QStringLiteral("текст\n"));

    zametti::NoteEditor editor;
    editor.resize(700, 500);
    editor.show();
    QTest::qWait(20);
    editor.setFocus();
    editor.openFile(path);

    auto text = [&editor] {
        return QString::fromStdString(
            zametti::serialize(zametti::readDocument(*editor.document())));
    };

    QTextCursor cursor = editor.textCursor();
    cursor.movePosition(QTextCursor::End);
    editor.setTextCursor(cursor);
    editor.insertPlainText(QStringLiteral(" правка"));
    QTest::qWait(10);

    QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
    QTest::qWait(10);
    checkEqual(QStringLiteral("текст\n"), text(), "Ctrl+Z с клавиатуры отменяет");

    QTest::keyClick(&editor, Qt::Key_Y, Qt::ControlModifier);
    QTest::qWait(10);
    checkEqual(QStringLiteral("текст правка\n"), text(), "Ctrl+Y возвращает");

    QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
    QTest::qWait(10);
    QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
    QTest::qWait(10);
    // Ctrl+Shift+Z здесь — повтор, а не отмена: так его понимает система.
    checkEqual(QStringLiteral("текст правка\n"), text(), "Ctrl+Shift+Z возвращает");
}

// Снимок истории откладывается до конца серии набора: читать документ целиком
// на каждую букву — это O(N) на нажатие. Отложенный снимок обязан быть на
// месте всюду, где история читается или пополняется, и здесь проверены все
// такие места разом — каждое из них без записи снимка теряло бы набранное.
// Набираем латиницей: QTest::keyClicks умеет только ASCII. Каждый случай — со
// своим файлом: открытие заметки сохраняет прежнюю, и случаи протекали бы
// друг в друга.
void checkDeferredSnapshot() {
    zametti::NoteEditor editor;
    editor.resize(700, 500);
    editor.show();
    QTest::qWait(20);
    editor.setFocus();

    auto text = [&editor] {
        return QString::fromStdString(
            zametti::serialize(zametti::readDocument(*editor.document())));
    };
    auto open = [&](const char* name) {
        editor.openFile(writeNote(name, QStringLiteral("основа\n")));
        QTest::qWait(10);
        QTextCursor cursor = editor.textCursor();
        cursor.movePosition(QTextCursor::End);
        editor.setTextCursor(cursor);
    };

    // Отмена сразу после набора, без паузы: снимок ещё отложен, и отменять
    // было бы нечего.
    open("снимок-отмена.md");
    QTest::keyClicks(&editor, QStringLiteral(" tail"));
    editor.undo();
    QTest::qWait(10);
    // Пробел перед словом — свой шаг, поэтому отмен две.
    checkEqual(QStringLiteral("основа \n"), text(), "отмена сразу после набора сняла слово");
    editor.undo();
    QTest::qWait(10);
    checkEqual(QStringLiteral("основа\n"), text(), "вторая отмена сняла и пробел");

    // Набор, потом операция: набранное — свой шаг, операция — свой.
    open("снимок-операция.md");
    QTest::keyClicks(&editor, QStringLiteral(" more"));
    const QString typed = text();
    QTest::keyClick(&editor, Qt::Key_Return);
    QTest::qWait(10);
    editor.undo();
    QTest::qWait(10);
    checkEqual(typed, text(), "отмена операции возвращает к набранному");
    editor.undo();
    QTest::qWait(10);
    checkEqual(QStringLiteral("основа \n"), text(), "вторая отмена сняла слово");
    editor.undo();
    QTest::qWait(10);
    checkEqual(QStringLiteral("основа\n"), text(), "третья вернула к исходному");

    // Смена облика посреди серии: она собирает документ ИЗ ИСТОРИИ, и со
    // стухшим снимком набранное просто пропало бы с экрана.
    open("снимок-масштаб.md");
    QTest::keyClicks(&editor, QStringLiteral(" zoom"));
    editor.applyZoom(1.5);
    QTest::qWait(10);
    checkEqual(QStringLiteral("основа zoom\n"), text(), "набранное переживает смену масштаба");
    editor.applyZoom(1.0);
    QTest::qWait(10);

    // Сохранение посреди серии: оно умеет пересобрать документ из файла.
    const QString saved = writeNote("снимок-запись.md", QStringLiteral("основа\n"));
    editor.openFile(saved);
    QTest::qWait(10);
    {
        QTextCursor cursor = editor.textCursor();
        cursor.movePosition(QTextCursor::End);
        editor.setTextCursor(cursor);
    }
    QTest::keyClicks(&editor, QStringLiteral(" save"));
    editor.save(false);
    QTest::qWait(10);
    checkEqual(QStringLiteral("основа save\n"), readFile(saved), "набранное дошло до файла");
    editor.undo();
    QTest::qWait(10);
    checkEqual(QStringLiteral("основа \n"), text(), "после записи отмена снимает слово");
    editor.undo();
    QTest::qWait(10);
    checkEqual(QStringLiteral("основа\n"), text(), "а следующая — пробел перед ним");

    // Серия кончается тишиной: два прогона набора — два шага.
    open("снимок-пауза.md");
    QTest::keyClicks(&editor, QStringLiteral(" one"));
    QTest::qWait(zametti::appearance().undoCoalesceMs + 150);
    QTest::keyClicks(&editor, QStringLiteral(" two"));
    QTest::qWait(zametti::appearance().undoCoalesceMs + 150);
    editor.undo();
    QTest::qWait(10);
    checkEqual(QStringLiteral("основа one \n"), text(), "пауза разделяет серии набора");
}

// Широкое окно: колонка уже окна, и applyContentWidth раздвигает поля рамки,
// центрируя её. Поля рамки держит ВИД, а не сборщик, и заплатка их не трогает
// — но сверка заплатки с полной сборкой их сравнивала и роняла отладочную
// сборку на первом же Enter в списке. У всех прежних тестов окно было узкое,
// центрирование не включалось, и разница не всплывала.
void checkWideWindowOperations() {
    const QString path = writeNote("широкое-окно.md",
                                   QStringLiteral("15-21 декабря:\n- [x] пункт\n- [ ] другой\n"));
    zametti::NoteEditor editor;
    // Заведомо шире maxContentWidth: поля обязаны раздвинуться.
    editor.resize(1400, 800);
    editor.show();
    QTest::qWait(20);
    editor.setFocus();
    editor.openFile(path);
    QTest::qWait(20);

    const qreal side = editor.document()->rootFrame()->frameFormat().leftMargin();
    check(side > 60.0, "в широком окне колонка центрируется: поле рамки раздвинуто");

    // Enter в конце пункта — та самая операция, на которой падало.
    QTextCursor at(editor.document()->findBlockByNumber(1));
    editor.setTextCursor(at);
    QTest::keyClick(&editor, Qt::Key_End);
    QTest::qWait(10);
    QTest::keyClick(&editor, Qt::Key_Return);
    QTest::qWait(10);
    check(editor.document()->blockCount() == 4, "Enter завёл новый пункт");
    checkEqual(QStringLiteral("15-21 декабря:"), firstLine(editor),
               "текст первой строки цел");
    // И поля не сбились от заплатки: колонка осталась центрированной.
    check(std::fabs(editor.document()->rootFrame()->frameFormat().leftMargin() - side) < 0.5,
          "поля рамки после операции на месте");
}

// Кэш заметок сессии. Возвращаясь в недавнюю заметку, человек застаёт её
// такой, какой оставил: цела история правок, каретка и прокрутка. Кладётся
// туда только ЧИСТОЕ и только то, чья сериализация байт в байт равна файлу.
void checkNoteCache() {
    const QString first = writeNote("кэш-первая.md",
                                    QStringLiteral("# первая\n\nстрока раз\nстрока два\n"));
    const QString second = writeNote("кэш-вторая.md", QStringLiteral("# вторая\n\nтекст\n"));

    zametti::NoteEditor editor;
    editor.resize(700, 500);
    editor.show();
    QTest::qWait(20);
    editor.setFocus();

    auto text = [&editor] {
        return QString::fromStdString(
            zametti::serialize(zametti::readDocument(*editor.document())));
    };

    // Правим первую, сохраняем, уходим и возвращаемся.
    editor.openFile(first);
    QTest::qWait(20);
    QTextCursor at(editor.document()->findBlockByNumber(2));
    at.movePosition(QTextCursor::EndOfBlock);
    editor.setTextCursor(at);
    QTest::keyClicks(&editor, QStringLiteral(" tail"));
    editor.save(false);
    QTest::qWait(20);
    const QString saved = text();
    const int caret = editor.textCursor().position();

    editor.openFile(second);
    QTest::qWait(20);
    check(editor.cachedNoteCount() == 1, "чистая заметка отложена в кэш");
    check(editor.cachedNoteBytes() > 0, "вес отложенного посчитан");

    editor.openFile(first);
    QTest::qWait(20);
    checkEqual(saved, text(), "вернулись к тому же содержимому");
    checkEqual(QString::number(caret), QString::number(editor.textCursor().position()),
               "каретка вернулась на место");
    check(editor.cachedNoteCount() == 1, "отложенная взята из кэша, а вторая легла туда");

    // Главное: история цела — Ctrl+Z отменяет правку ПРОШЛОГО захода. Отмен
    // две: набрано было « tail», а пробел перед словом — свой шаг.
    editor.undo();
    QTest::qWait(10);
    checkEqual(QStringLiteral("# первая\n\nстрока раз\nстрока два \n"), text(),
               "Ctrl+Z отменяет слово, набранное в прошлый заход");
    editor.undo();
    QTest::qWait(10);
    checkEqual(QStringLiteral("# первая\n\nстрока раз\nстрока два\n"), text(),
               "а вторая отмена — пробел перед ним");

    // Внешняя правка: отпечаток не сойдётся, кэш выбрасывается, заметка
    // собирается с диска.
    editor.openFile(second);
    QTest::qWait(20);
    {
        QFile file(first);
        check(file.open(QIODevice::WriteOnly), "внешняя правка записана");
        file.write("# первая\n\nсовсем другое\n");
        file.close();
    }
    editor.openFile(first);
    QTest::qWait(20);
    checkEqual(QStringLiteral("# первая\n\nсовсем другое\n"), text(),
               "внешняя правка победила отложенное");
    editor.undo();
    QTest::qWait(10);
    checkEqual(QStringLiteral("# первая\n\nсовсем другое\n"), text(),
               "и истории от прошлого захода не осталось");

    // Смена облика: документы собраны с запечённым обликом, кэш протухает весь.
    editor.openFile(second);
    QTest::qWait(20);
    check(editor.cachedNoteCount() > 0, "перед сменой облика в кэше что-то есть");
    editor.applyZoom(1.5);
    QTest::qWait(10);
    check(editor.cachedNoteCount() == 0, "смена масштаба чистит кэш целиком");
    editor.applyZoom(1.0);
    QTest::qWait(10);

    // Несохранённое не откладывается: потерять правки страшнее, чем пересобрать.
    editor.openFile(first);
    QTest::qWait(20);
    QTextCursor dirty = editor.textCursor();
    dirty.movePosition(QTextCursor::End);
    editor.setTextCursor(dirty);
    QTest::keyClicks(&editor, QStringLiteral(" x"));
    QTest::qWait(10);
    const int before = editor.cachedNoteCount();
    editor.openFile(second);   // сохранит и отложит уже чистую
    QTest::qWait(20);
    check(editor.cachedNoteCount() >= before, "после сохранения заметка откладывается");

    // Заметка тяжелее всего бюджета в кэш не идёт.
    const int savedBudget = zametti::appearance().documentCacheSizeMb;
    zametti::appearance().documentCacheSizeMb = 1;
    editor.clearNoteCache();
    QString big = QStringLiteral("# большая\n\n");
    for (int i = 0; i < 40000; ++i) big += QStringLiteral("строка с текстом %1\n").arg(i);
    const QString heavy = writeNote("кэш-тяжёлая.md", big);
    editor.openFile(heavy);
    QTest::qWait(60);
    editor.openFile(second);
    QTest::qWait(20);
    check(editor.cachedNoteCount() == 0, "заметка тяжелее бюджета в кэш не идёт");
    zametti::appearance().documentCacheSizeMb = savedBudget;
}

// Канонизация при открытии. Хранилище наше, и сор в нём — лишние пробелы в
// конце строк, недостающий перевод строки в конце файла — причёсывается прямо
// на диске. Ни одно значение в шапке при этом не меняется: заметку всего лишь
// открыли, и всплывать наверх списка недавних ей не с чего. Чужой .md без
// шапки не трогаем вовсе.
void checkCanonicaliseOnOpen() {
    const QString stamp = QStringLiteral("2020-01-02T03:04:05Z");
    const QString header = QStringLiteral("<!-- zametti\nid: 01test\ncreated: %1\n"
                                          "modified: %1\n-->\n\n").arg(stamp);
    const QString messy = header + QStringLiteral("# заголовок   \n\nстрока с хвостом   ");
    const QString path = writeNote("сор.md", messy);

    zametti::NoteEditor editor;
    editor.resize(700, 500);
    editor.show();
    QTest::qWait(20);
    editor.openFile(path);
    QTest::qWait(20);

    const QString onDisk = readFile(path);
    check(!onDisk.contains(QStringLiteral("   \n")), "хвостовые пробелы с диска ушли");
    check(onDisk.endsWith(QLatin1Char('\n')), "перевод строки в конце файла появился");
    check(onDisk.count(stamp) == 2,
          "штампы created и modified не тронуты: заметку только открыли");
    check(!editor.document()->isModified(),
          "после канонизации документ не считается изменённым");

    // Повторное открытие уже канонического файла его не трогает.
    const QFileInfo info(path);
    const QDateTime was = info.lastModified();
    QTest::qWait(1100);
    editor.openFile(writeNote("другая-канон.md", QStringLiteral("другая\n")));
    QTest::qWait(20);
    editor.openFile(path);
    QTest::qWait(20);
    check(QFileInfo(path).lastModified() == was,
          "канонический файл при открытии не переписывается");

    // Чужой .md без шапки не наш: его не трогаем.
    const QString alien = writeNote("чужой.md", QStringLiteral("# чужой   \n\nхвост   "));
    editor.openFile(alien);
    QTest::qWait(20);
    checkEqual(QStringLiteral("# чужой   \n\nхвост   "), readFile(alien),
               "файл без шапки остался как был");
}

// Автосохранение можно откладывать надолго, но четыре пути к записи обязаны
// работать всегда: переключение на другую заметку, уход фокуса, Ctrl+S и
// закрытие. Задержку в тесте ставим заведомо больше прогона — тогда всё, что
// дошло до диска, дошло не по таймеру.
void checkSaveWithoutAutosave() {
    const int savedDelay = zametti::appearance().autosaveDelayMs;
    zametti::appearance().autosaveDelayMs = 600000;

    const QString first = writeNote("без-таймера-раз.md", QStringLiteral("раз\n"));
    const QString second = writeNote("без-таймера-два.md", QStringLiteral("два\n"));

    zametti::NoteEditor editor;
    editor.resize(700, 500);
    editor.show();
    QTest::qWait(20);
    editor.setFocus();

    const auto typeTail = [&editor](const QString& what) {
        QTextCursor at = editor.textCursor();
        at.movePosition(QTextCursor::End);
        editor.setTextCursor(at);
        QTest::keyClicks(&editor, what);
        QTest::qWait(10);
    };

    // Переключение на другую заметку.
    editor.openFile(first);
    QTest::qWait(20);
    typeTail(QStringLiteral(" one"));
    editor.openFile(second);
    QTest::qWait(20);
    checkEqual(QStringLiteral("раз one\n"), readFile(first),
               "переключение на другую заметку записывает прежнюю");

    // Ctrl+S.
    typeTail(QStringLiteral(" two"));
    QTest::keyClick(&editor, Qt::Key_S, Qt::ControlModifier);
    QTest::qWait(20);
    // Сочетание живёт в окне, а не в редакторе, — здесь зовём напрямую.
    editor.save(true);
    QTest::qWait(20);
    checkEqual(QStringLiteral("два two\n"), readFile(second), "Ctrl+S записывает");

    // Уход фокуса и закрытие идут тем же вызовом save(false) из окна; проверяем
    // сам вызов — что он пишет, не дожидаясь таймера.
    typeTail(QStringLiteral(" three"));
    editor.save(false);
    QTest::qWait(20);
    checkEqual(QStringLiteral("два two three\n"), readFile(second),
               "явное сохранение не ждёт таймера");

    // И обратное: пока таймер не сработал, файл не трогается.
    typeTail(QStringLiteral(" four"));
    QTest::qWait(200);
    checkEqual(QStringLiteral("два two three\n"), readFile(second),
               "до таймера набранное на диск не уходит");
    editor.save(false);

    zametti::appearance().autosaveDelayMs = savedDelay;
}

// Текст после переноса строки обязан набираться тем же кеглем. Разделитель
// строк шрифту неизвестен, и без оговорки он попадал под правило увеличения
// эмодзи — а набранное сразу после него наследовало крупный формат.
void checkSizeAfterSoftBreak() {
    const QString path = writeNote("кегль.md", QString());

    zametti::NoteEditor editor;
    editor.resize(700, 500);
    editor.show();
    QTest::qWait(20);
    editor.setFocus();
    editor.openFile(path);

    editor.insertPlainText(QStringLiteral("первая"));
    QTest::qWait(10);
    QTest::keyClick(&editor, Qt::Key_Return);
    QTest::qWait(10);
    editor.insertPlainText(QStringLiteral("вторая"));
    QTest::qWait(10);

    qreal smallest = 0;
    qreal largest = 0;
    for (QTextBlock block = editor.document()->begin(); block.isValid();
         block = block.next()) {
        for (QTextBlock::iterator it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            if (!fragment.isValid() || fragment.text().isEmpty()) continue;
            const qreal size = fragment.charFormat().fontPointSize();
            if (smallest == 0 || size < smallest) smallest = size;
            if (size > largest) largest = size;
        }
    }
    check(smallest > 0 && smallest == largest,
          "кегль после переноса строки не должен меняться");
}

// Встроенный код с клавиатуры и авто-отступ в блоке кода.
void checkCodeTyping() {
    const QString path = writeNote("код-набором.md", QString());

    zametti::NoteEditor editor;
    editor.resize(700, 500);
    editor.show();
    QTest::qWait(20);
    editor.setFocus();
    editor.openFile(path);

    auto text = [&editor] {
        return QString::fromStdString(
            zametti::serialize(zametti::readDocument(*editor.document())));
    };

    // Кавычки делают код, и набор после них идёт обычным текстом.
    editor.insertPlainText(QStringLiteral("вот "));
    QTest::keyClick(&editor, Qt::Key_QuoteLeft);
    editor.insertPlainText(QStringLiteral("код"));
    QTest::keyClick(&editor, Qt::Key_QuoteLeft);
    editor.insertPlainText(QStringLiteral(" конец"));
    QTest::qWait(10);
    checkEqual(QStringLiteral("вот `код` конец\n"), text(),
               "кавычки делают код, а дальше идёт обычный текст");

    // Авто-отступ: новая строка блока кода наследует отступ предыдущей.
    const QString code = writeNote("отступ-кода.md", QString());
    editor.openFile(code);
    QTest::qWait(20);
    for (int i = 0; i < 3; ++i) QTest::keyClick(&editor, Qt::Key_QuoteLeft);
    QTest::keyClick(&editor, Qt::Key_Return);
    editor.insertPlainText(QStringLiteral("    if x:"));
    QTest::keyClick(&editor, Qt::Key_Return);
    editor.insertPlainText(QStringLiteral("return 1"));
    QTest::qWait(10);
    checkEqual(QStringLiteral("```\n    if x:\n    return 1\n```\n"), text(),
               "новая строка кода наследует отступ предыдущей");

    // Enter вместе с отступом — один шаг отмены.
    QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
    QTest::qWait(10);
    QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
    QTest::qWait(10);
    checkEqual(QStringLiteral("```\n    if x:\n```\n"), text(),
               "Enter с отступом отменяется одним шагом");
}

// Ctrl+E без выделения — не правка документа, а формат следующей буквы. Случай,
// ради которого проверка написана: курсор стоит вплотную к правой кромке
// встроенного кода, и дописать оттуда обычный текст нечем — Qt берёт формат у
// знака слева. Проверяется не только разметка в файле, но и вид: признак кода
// снимался и раньше, а семейство с подложкой оставались, и выглядело это как
// «Ctrl+E не работает».
void checkCodeAtEdge() {
    const QString path = writeNote("кромка-кода.md", QStringLiteral("- пункт `код`\n"));

    zametti::NoteEditor editor;
    editor.resize(700, 500);
    editor.show();
    QTest::qWait(20);
    editor.setFocus();
    editor.openFile(path);
    QTest::qWait(20);

    auto text = [&editor] {
        return QString::fromStdString(
            zametti::serialize(zametti::readDocument(*editor.document())));
    };
    auto toEnd = [&editor] {
        QTextCursor cursor = editor.textCursor();
        cursor.movePosition(QTextCursor::End);
        editor.setTextCursor(cursor);
    };
    const QString mono = zametti::appearance().codeFamily;

    toEnd();
    check((editor.currentCharFormat().intProperty(zametti::SpanStyleProperty) &
           zametti::SpanCode) != 0,
          "у правой кромки формат набора — код, иначе случай не тот");

    QTest::keyClick(&editor, Qt::Key_E, Qt::ControlModifier);
    editor.insertPlainText(QStringLiteral("хвост"));
    QTest::qWait(10);
    checkEqual(QStringLiteral("- пункт `код`хвост\n"), text(),
               "Ctrl+E у кромки выводит набор из кода");

    QTextCursor tail(editor.document());
    tail.setPosition(editor.textCursor().position() - 1);
    tail.setPosition(editor.textCursor().position(), QTextCursor::KeepAnchor);
    const QTextCharFormat after = tail.charFormat();
    check(after.fontFamilies().toStringList().value(0) != mono,
          "набранное после Ctrl+E не должно остаться моноширинным");
    check(after.background().style() == Qt::NoBrush,
          "подложка кода после Ctrl+E остаться не должна");

    // Обратный ход, и в заголовке: код набирается своим кеглем, а после
    // повторного Ctrl+E кегль возвращается заголовочный, а не кодовый.
    const QString heading = writeNote("код-в-заголовке.md", QStringLiteral("## Тема\n"));
    editor.openFile(heading);
    QTest::qWait(20);
    toEnd();
    const qreal headingSize = editor.currentCharFormat().fontPointSize();
    QTest::keyClick(&editor, Qt::Key_E, Qt::ControlModifier);
    editor.insertPlainText(QStringLiteral(" код"));
    QTest::qWait(10);
    checkEqual(QStringLiteral("## Тема` код`\n"), text(),
               "Ctrl+E на чистом месте начинает встроенный код");
    QTest::keyClick(&editor, Qt::Key_E, Qt::ControlModifier);
    check(qAbs(editor.currentCharFormat().fontPointSize() - headingSize) < 0.01,
          "после выхода из кода кегль возвращается заголовочный");
}

// Набор на пустой строке и отмена: каретка обязана вернуться на эту строку.
// Записанное смещение каретки живёт в координатах отменяемого документа, и
// без отображения через префикс/суффикс каретка прыгала на пару строк вниз —
// ровно на длину набранного.
void checkUndoReturnsToBlankLine() {
    const QString path = writeNote("отмена-на-пустой.md", QStringLiteral("а\n\n\n\nб\n"));

    zametti::NoteEditor editor;
    editor.resize(700, 500);
    editor.show();
    QTest::qWait(20);
    editor.setFocus();
    editor.openFile(path);
    QTest::qWait(20);

    QTextCursor cursor(editor.document());
    cursor.setPosition(editor.document()->findBlockByNumber(2).position());
    editor.setTextCursor(cursor);
    QTest::keyClicks(&editor, QStringLiteral("xyz"));
    QTest::qWait(10);
    QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
    QTest::qWait(10);

    checkEqual(QStringLiteral("а\n\n\n\nб\n"),
               QString::fromStdString(
                   zametti::serialize(zametti::readDocument(*editor.document()))),
               "отмена вернула документ");
    checkEqual(QStringLiteral("2"), QString::number(editor.textCursor().blockNumber()),
               "каретка вернулась на свою пустую строку");
}

// Щелчок по чекбоксу — самый ходовой способ отметить задачу. Проверяется
// настоящим щелчком по вьюпорту, а не вызовом операции: попадание считается по
// геометрии рамки, и ошибиться в ней проще всего именно там.
void checkCheckboxClick() {
    const QString path = writeNote("щелчок.md",
                                   QStringLiteral("- [ ] первая\n- [ ] вторая\n"));

    zametti::NoteEditor editor;
    editor.resize(700, 500);
    editor.show();
    QTest::qWait(30);
    editor.setFocus();
    editor.openFile(path);
    QTest::qWait(20);

    auto text = [&editor] {
        return QString::fromStdString(
            zametti::serialize(zametti::readDocument(*editor.document())));
    };

    auto clickAt = [&editor](const QPointF& point) {
        QTest::mouseClick(
            editor.viewport(), Qt::LeftButton, Qt::NoModifier,
            QPoint(int(point.x()) - editor.horizontalScrollBar()->value(),
                   int(point.y()) - editor.verticalScrollBar()->value()));
        QTest::qWait(10);
    };

    const QRectF box =
        zametti::checkboxRect(editor.document()->firstBlock(), editor.baseFont());
    check(!box.isNull(), "у задачи должна быть рамка чекбокса");

    clickAt(box.center());
    checkEqual(QStringLiteral("- [x] первая\n- [ ] вторая\n"), text(),
               "щелчок по рамке отмечает задачу");

    clickAt(box.center());
    checkEqual(QStringLiteral("- [ ] первая\n- [ ] вторая\n"), text(),
               "второй щелчок снимает отметку");

    // Мимо рамки — обычный щелчок по тексту, отметка не меняется.
    clickAt(box.center() + QPointF(200, 0));
    checkEqual(QStringLiteral("- [ ] первая\n- [ ] вторая\n"), text(),
               "щелчок по тексту отметку не трогает");

    // И щелчок — обычная правка: отменяется.
    clickAt(box.center());
    QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
    QTest::qWait(10);
    checkEqual(QStringLiteral("- [ ] первая\n- [ ] вторая\n"), text(),
               "щелчок отменяется как обычная правка");
}

// Прокрутка при правке. Документ пересобирается целиком, и место в нём надо
// возвращать — но по экранной высоте курсора, а не по доле от высоты заметки:
// высота меняется от правки к правке, и доля каждый раз попадает не туда.
// Замер до починки: заметка уползала вверх на ~25 px за каждый добавленный пункт.
void checkScrollHolds() {
    QString source;
    for (int i = 0; i < 40; ++i)
        source += QStringLiteral("Абзац номер %1, чтобы заметка была длинной.\n\n").arg(i);
    for (int i = 0; i < 20; ++i) source += QStringLiteral("- пункт %1\n").arg(i);
    const QString path = writeNote("прокрутка.md", source);

    zametti::NoteEditor editor;
    editor.resize(700, 500);
    editor.show();
    QTest::qWait(30);
    editor.setFocus();
    editor.openFile(path);
    QTest::qWait(20);

    // Ищем пункт по тексту, а не по номеру: пустые строки между абзацами — тоже
    // блоки, и номер зависел бы от того, сколько их в заметке.
    int target = -1;
    for (QTextBlock block = editor.document()->begin(); block.isValid();
         block = block.next())
        if (block.text() == QStringLiteral("пункт 5")) {
            target = block.blockNumber();
            break;
        }
    check(target > 0, "пункт 5 должен найтись");

    QTextCursor cursor = editor.textCursor();
    cursor.setPosition(editor.document()->findBlockByNumber(target).position());
    cursor.movePosition(QTextCursor::EndOfBlock);
    editor.setTextCursor(cursor);
    editor.ensureCursorVisible();
    QTest::qWait(20);
    // Отводим курсор от нижнего края: у края прокрутка растёт законно.
    editor.verticalScrollBar()->setValue(editor.verticalScrollBar()->value() + 150);
    QTest::qWait(20);

    const int before = editor.verticalScrollBar()->value();
    check(before > 0, "заметка должна быть прокручена");

    for (int i = 0; i < 5; ++i) {
        QTest::keyClick(&editor, Qt::Key_Return);
        editor.insertPlainText(QStringLiteral("новый"));
        QTest::qWait(10);
    }
    checkEqual(QString::number(before),
               QString::number(editor.verticalScrollBar()->value()),
               "прокрутка держится, пока курсор виден");
}

// Правка, меняющая высоту собственного блока: выход из списка. Пункт становится
// абзацем и получает другие отступы. Держаться при этом за курсор нельзя — весь
// текст выше уехал бы (замер до починки: 23 px), поэтому вид держится за блок
// НАД правкой.
void checkScrollHoldsWhenBlockChangesHeight() {
    QString source;
    for (int i = 0; i < 30; ++i)
        source += QStringLiteral("Абзац номер %1 для высоты.\n\n").arg(i);
    source += QStringLiteral("Ну и номера:\n\n1. раз\n2. два\n3. три\n\nхвост\n");
    const QString path = writeNote("выход-из-списка.md", source);

    zametti::NoteEditor editor;
    editor.resize(700, 500);
    editor.show();
    QTest::qWait(30);
    editor.setFocus();
    editor.openFile(path);
    QTest::qWait(20);

    int last = -1;
    for (QTextBlock block = editor.document()->begin(); block.isValid();
         block = block.next())
        if (zametti::isListBlock(block)) last = block.blockNumber();
    check(last > 0, "список должен найтись");

    QTextCursor cursor = editor.textCursor();
    cursor.setPosition(editor.document()->findBlockByNumber(last).position());
    cursor.movePosition(QTextCursor::EndOfBlock);
    editor.setTextCursor(cursor);
    editor.ensureCursorVisible();
    editor.verticalScrollBar()->setValue(editor.verticalScrollBar()->value() + 120);
    QTest::qWait(20);

    // Следим за абзацем выше списка: он меняться не должен вовсе.
    const int watched = last - 4;
    auto watchedY = [&editor, watched] {
        const QTextBlock block = editor.document()->findBlockByNumber(watched);
        return int(editor.document()->documentLayout()->blockBoundingRect(block).top()) -
               editor.verticalScrollBar()->value();
    };
    const int before = watchedY();

    // Новый пункт, пустой пункт, выход из списка.
    QTest::keyClick(&editor, Qt::Key_Return);
    editor.insertPlainText(QStringLiteral("четыре"));
    QTest::qWait(10);
    checkEqual(QString::number(before), QString::number(watchedY()),
               "текст выше стоит при добавлении пункта");

    QTest::keyClick(&editor, Qt::Key_Return);
    QTest::qWait(10);
    QTest::keyClick(&editor, Qt::Key_Return);
    QTest::qWait(10);
    checkEqual(QString::number(before), QString::number(watchedY()),
               "и при выходе из списка тоже");
}

// Ссылка не должна расти от набора за её правым краем. Qt берёт оформление знака
// перед курсором, а у ссылки оно с адресом — пробел и запятая после ссылки
// уезжали внутрь неё, и в файл шло "[текст ,](адрес)".
void checkLinkDoesNotGrow() {
    const QString path =
        writeNote("ссылка.md", QStringLiteral("вот [ссылка](https://example.com)\n"));

    zametti::NoteEditor editor;
    editor.resize(700, 500);
    editor.show();
    QTest::qWait(20);
    editor.setFocus();
    editor.openFile(path);
    QTest::qWait(20);

    auto text = [&editor] {
        return QString::fromStdString(
            zametti::serialize(zametti::readDocument(*editor.document())));
    };

    QTextCursor cursor = editor.textCursor();
    cursor.movePosition(QTextCursor::End);
    editor.setTextCursor(cursor);
    QTest::keyClick(&editor, Qt::Key_Space);
    editor.insertPlainText(QStringLiteral("хвост"));
    QTest::qWait(10);
    checkEqual(QStringLiteral("вот [ссылка](https://example.com) хвост\n"), text(),
               "набранное за ссылкой в неё не уезжает");

    // Запятая — тот же случай, и на ней это заметили.
    QTest::keyClick(&editor, Qt::Key_Comma);
    QTest::qWait(10);
    check(!text().contains(QStringLiteral(",](")), "запятая тоже остаётся снаружи");

    // А набор ВНУТРИ ссылки её по-прежнему продолжает: там это и нужно.
    const QString inside =
        writeNote("внутри-ссылки.md", QStringLiteral("[ссылка](https://example.com)\n"));
    editor.openFile(inside);
    QTest::qWait(20);
    QTextCursor middle = editor.textCursor();
    middle.setPosition(editor.document()->firstBlock().position() + 3);
    editor.setTextCursor(middle);
    editor.insertPlainText(QStringLiteral("XX"));
    QTest::qWait(10);
    checkEqual(QStringLiteral("[ссыXXлка](https://example.com)\n"), text(),
               "внутри ссылки набор её продолжает");
}

// Щелчок по рамке при выделении: переключает всё выделенное разом и выделение
// сохраняет — ровно как Ctrl+Space. А двойной щелчок не должен выделять строку:
// человек метил в чекбокс, а не в слово под ним.
void checkCheckboxClickWithSelection() {
    const QString path = writeNote(
        "щелчок-выделение.md", QStringLiteral("- [ ] раз\n- [ ] два\n- [ ] три\n"));

    zametti::NoteEditor editor;
    editor.resize(700, 500);
    editor.show();
    QTest::qWait(30);
    editor.setFocus();
    editor.openFile(path);
    QTest::qWait(20);

    auto text = [&editor] {
        return QString::fromStdString(
            zametti::serialize(zametti::readDocument(*editor.document())));
    };
    auto clickBox = [&editor](int number, bool twice) {
        const QRectF box = zametti::checkboxRect(
            editor.document()->findBlockByNumber(number), editor.baseFont());
        const QPoint at(int(box.center().x()) - editor.horizontalScrollBar()->value(),
                        int(box.center().y()) - editor.verticalScrollBar()->value());
        QTest::mouseClick(editor.viewport(), Qt::LeftButton, Qt::NoModifier, at);
        if (twice)
            QTest::mouseDClick(editor.viewport(), Qt::LeftButton, Qt::NoModifier, at);
        QTest::qWait(10);
    };

    QTextCursor all = editor.textCursor();
    all.movePosition(QTextCursor::Start);
    all.movePosition(QTextCursor::End, QTextCursor::KeepAnchor);
    editor.setTextCursor(all);
    const int selected = all.selectionEnd() - all.selectionStart();

    clickBox(1, false);
    checkEqual(QStringLiteral("- [x] раз\n- [x] два\n- [x] три\n"), text(),
               "щелчок при выделении отмечает все задачи разом");
    checkEqual(QString::number(selected),
               QString::number(editor.textCursor().selectionEnd() -
                               editor.textCursor().selectionStart()),
               "выделение при этом остаётся");

    clickBox(1, false);
    checkEqual(QStringLiteral("- [ ] раз\n- [ ] два\n- [ ] три\n"), text(),
               "второй щелчок снимает отметки со всех");

    // Двойной щелчок по рамке: одно переключение и никакого выделения.
    const QString single = writeNote("двойной-щелчок.md", QStringLiteral("- [ ] дело\n"));
    editor.openFile(single);
    QTest::qWait(20);
    clickBox(0, true);
    checkEqual(QStringLiteral("- [x] дело\n"), text(),
               "двойной щелчок по рамке переключает задачу один раз");
    check(!editor.textCursor().hasSelection(),
          "двойной щелчок по рамке строку не выделяет");
}

// Отмена ставит курсор туда, где была отменяемая правка. Раньше он вставал
// туда, где стоял в возвращаемом состоянии, — а у только что открытого файла в
// первом шаге записан ноль, и первая же отмена швыряла курсор в начало заметки.
void checkUndoKeepsCursor() {
    const QString path = writeNote(
        "курсор-отмены.md",
        QStringLiteral("вступление\n\n- раз\n  - вложенный\n  - ещё вложенный\n- два\n"));

    zametti::NoteEditor editor;
    editor.resize(700, 500);
    editor.show();
    QTest::qWait(30);
    editor.setFocus();
    editor.openFile(path);
    QTest::qWait(20);

    QTextCursor cursor = editor.textCursor();
    cursor.setPosition(editor.document()->findBlockByNumber(2).position());
    const QTextBlock last = editor.document()->findBlockByNumber(3);
    cursor.setPosition(last.position() + last.length() - 1, QTextCursor::KeepAnchor);
    editor.setTextCursor(cursor);
    const int before = editor.textCursor().position();
    check(before > 0, "курсор должен стоять не в начале");

    QTest::keyClick(&editor, Qt::Key_3, Qt::ControlModifier);
    QTest::qWait(10);
    QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
    QTest::qWait(10);

    checkEqual(QStringLiteral("вступление\n\n- раз\n  - вложенный\n  - ещё вложенный\n- два\n"),
               QString::fromStdString(
                   zametti::serialize(zametti::readDocument(*editor.document()))),
               "отмена вернула прежний вид списка");
    checkEqual(QString::number(before), QString::number(editor.textCursor().position()),
               "курсор после отмены остался у правки");
}

// Вид не должен уезжать ни от одной операции. Правка меняет строку, а не окно:
// прокрутка обязана остаться на месте, а курсор — сдвинуться не больше чем на
// строку.
//
// Обмен пунктов местами этого не соблюдал: правки над IR ставят курсор уже
// после пересборки, и показ курсора внутри пересборки уводил вид к началу
// документа, а потом обратно вниз — переставленный пункт оказывался у самой
// нижней кромки окна.
void checkViewHoldsForEveryOperation() {
    struct Probe {
        const char* name;
        Qt::Key key;
        Qt::KeyboardModifiers mods;
    };
    const Probe probes[] = {
        {"Enter", Qt::Key_Return, Qt::NoModifier},
        {"Tab", Qt::Key_Tab, Qt::NoModifier},
        {"Ctrl+Space", Qt::Key_Space, Qt::ControlModifier},
        {"в нумерованный", Qt::Key_3, Qt::ControlModifier},
        {"в абзац", Qt::Key_0, Qt::ControlModifier | Qt::ShiftModifier},
        {"жирный", Qt::Key_B, Qt::ControlModifier},
        {"блок кода", Qt::Key_E, Qt::ControlModifier | Qt::ShiftModifier},
        {"Ctrl+Down", Qt::Key_Down, Qt::ControlModifier},
        {"Ctrl+Up", Qt::Key_Up, Qt::ControlModifier},
    };

    QString source;
    for (int i = 0; i < 30; ++i)
        source += QStringLiteral("Абзац %1 для высоты окна.\n\n").arg(i);
    for (int i = 0; i < 12; ++i) source += QStringLiteral("- [ ] пункт %1\n").arg(i);

    for (const Probe& probe : probes) {
        const QString path = writeNote(
            (std::string("вид-") + probe.name + ".md").c_str(), source);

        zametti::NoteEditor editor;
        editor.resize(700, 500);
        editor.show();
        QTest::qWait(20);
        editor.setFocus();
        editor.openFile(path);
        QTest::qWait(20);

        int first = -1;
        for (QTextBlock block = editor.document()->begin(); block.isValid();
             block = block.next())
            if (zametti::isListBlock(block)) {
                first = block.blockNumber();
                break;
            }
        check(first > 0, "список должен найтись");

        QTextCursor cursor = editor.textCursor();
        cursor.setPosition(
            editor.document()->findBlockByNumber(first + 4).position() + 3);
        editor.setTextCursor(cursor);
        editor.ensureCursorVisible();
        editor.verticalScrollBar()->setValue(editor.verticalScrollBar()->value() + 100);
        QTest::qWait(10);

        const int scroll = editor.verticalScrollBar()->value();
        const int y = editor.cursorRect().top();
        QTest::keyClick(&editor, probe.key, probe.mods);
        QTest::qWait(10);

        checkEqual(QString::number(scroll),
                   QString::number(editor.verticalScrollBar()->value()),
                   std::string("вид не уехал: ") + probe.name);
        // Курсор вправе сдвинуться на свою строку — но не на пол-окна.
        check(qAbs(editor.cursorRect().top() - y) <= 40,
              std::string("курсор сдвинулся не больше чем на строку: ") + probe.name);
    }
}

// Шаг вниз между блоками с разными полями. Маркер списка отодвигает текст
// пункта вправо, а движение по вертикали держит экранный X — из начала пункта
// курсор попадал на пару знаков внутрь следующего абзаца, и выделение
// прихватывало лишнее.
void checkColumnAcrossMargins() {
    const QString path = writeNote("колонка.md",
                                   QStringLiteral("- пункт списка\n\nОбычный абзац.\n"));

    zametti::NoteEditor editor;
    editor.resize(700, 400);
    editor.show();
    QTest::qWait(20);
    editor.setFocus();
    editor.openFile(path);
    QTest::qWait(20);

    check(editor.document()->firstBlock().blockFormat().leftMargin() >
              editor.document()->findBlockByNumber(1).blockFormat().leftMargin(),
          "у пункта поле шире, чем у абзаца");

    QTextCursor cursor = editor.textCursor();
    cursor.setPosition(editor.document()->firstBlock().position());
    editor.setTextCursor(cursor);
    QTest::keyClick(&editor, Qt::Key_Down);
    QTest::qWait(10);

    checkEqual(QStringLiteral("1"), QString::number(editor.textCursor().blockNumber()),
               "шаг вниз привёл в следующий блок");
    checkEqual(QStringLiteral("0"),
               QString::number(editor.textCursor().positionInBlock()),
               "и в ту же колонку, а не внутрь текста");

    // То же с выделением: лишних знаков захватываться не должно.
    cursor.setPosition(editor.document()->firstBlock().position());
    editor.setTextCursor(cursor);
    QTest::keyClick(&editor, Qt::Key_Down, Qt::ShiftModifier);
    QTest::qWait(10);
    checkEqual(QStringLiteral("пункт списка"),
               editor.textCursor().selectedText().replace(QChar::ParagraphSeparator,
                                                          QString()),
               "выделено ровно содержимое пункта");
}

// Выделение нескольких строк рисуется сплошным блоком. Высота строки назначена,
// а Qt красит выделение по естественной — между полосами оставался
// незакрашенный ряд, и на укороченной строке он читался сколом на углу.
void checkSelectionHasNoGaps() {
    QString source;
    for (int i = 0; i < 4; ++i)
        source += QStringLiteral("- пункт %1, достаточно длинный, чтобы занять ширину\n")
                      .arg(i);
    const QString path = writeNote("сплошное-выделение.md", source);

    zametti::NoteEditor editor;
    editor.resize(760, 260);
    editor.show();
    QTest::qWait(20);
    editor.setFocus();
    editor.openFile(path);
    QTest::qWait(20);

    QTextCursor cursor = editor.textCursor();
    cursor.setPosition(editor.document()->firstBlock().position());
    const QTextBlock last = editor.document()->findBlockByNumber(3);
    cursor.setPosition(last.position() + last.length() - 1, QTextCursor::KeepAnchor);
    editor.setTextCursor(cursor);
    QTest::qWait(20);

    QImage shot(editor.size(), QImage::Format_ARGB32);
    editor.render(&shot);

    const QColor highlight = editor.palette().color(QPalette::Highlight);
    auto isHighlight = [&highlight](QRgb pixel) {
        return qAbs(qRed(pixel) - highlight.red()) < 40 &&
               qAbs(qGreen(pixel) - highlight.green()) < 40 &&
               qAbs(qBlue(pixel) - highlight.blue()) < 40;
    };

    int firstRow = -1;
    int lastRow = -1;
    for (int y = 0; y < shot.height(); ++y) {
        bool any = false;
        for (int x = 0; x < shot.width() && !any; ++x) any = isHighlight(shot.pixel(x, y));
        if (!any) continue;
        if (firstRow < 0) firstRow = y;
        lastRow = y;
    }
    check(firstRow >= 0 && lastRow - firstRow > 40,
          "выделение должно занимать несколько строк");

    int empty = 0;
    for (int y = firstRow; y <= lastRow; ++y) {
        bool any = false;
        for (int x = 0; x < shot.width() && !any; ++x) any = isHighlight(shot.pixel(x, y));
        if (!any) ++empty;
    }
    checkEqual(QStringLiteral("0"), QString::number(empty),
               "внутри выделения не должно быть незакрашенных рядов");
}

// Пустая заметка. Каретка в ней должна быть видна и стоять там же, где встал бы
// текст: у левого поля и в высоту строки. До правки блок оставался вовсе без
// формата, каретка выходила кеглем по умолчанию в самом углу окна, и человек её
// попросту не находил.
void checkEmptyNoteCaret() {
    const QString path = writeNote("пустая.md", QString());

    // Окно нарочно широкое: при такой ширине колонка центрируется, и поля
    // документа меняются уже после сборки. Пустой документ от смены полей не
    // переразмечался — каретка оставалась у прежнего поля и кеглем по
    // умолчанию, то есть далеко от текста и вдвое ниже. На узком окне ошибки не
    // видно вовсе: там колонку не двигают.
    zametti::NoteEditor editor;
    editor.resize(1600, 400);
    editor.show();
    QTest::qWait(20);
    editor.setFocus();
    editor.openFile(path);
    QTest::qWait(20);

    const QRect caret = editor.cursorRect();

    // И тот же случай на пути запуска: заметку открывают ещё до show(), в узком
    // окне, а колонку двигает уже первое изменение размера — пересборки при
    // этом нет вовсе, и подсказка о разметке нужна именно там, где меняются
    // поля.
    {
        zametti::NoteEditor started;
        started.openFile(path);
        started.resize(1600, 400);
        started.show();
        QTest::qWait(20);
        started.setFocus();
        QTest::qWait(20);
        check(started.cursorRect().x() > 100,
              "при запуске каретка пустой заметки стоит в колонке текста");
        check(started.cursorRect().height() > 15,
              "и высотой в строку, а не кеглем по умолчанию");
    }
    // Левое поле страницы — там же, где начинается текст обычного абзаца.
    const QString filled = writeNote("не-пустая.md", QStringLiteral("текст\n"));
    editor.openFile(filled);
    QTest::qWait(20);
    const QRect withText = editor.cursorRect();
    editor.openFile(path);
    QTest::qWait(20);

    checkEqual(QString::number(withText.x()), QString::number(editor.cursorRect().x()),
               "каретка пустой заметки стоит у того же поля, что и текст");
    check(caret.height() > 1 && qAbs(caret.height() - withText.height()) <= 1,
          "и той же высоты, что строка текста");
}

// Каретка нарисована нами, а не Qt: цвет своей Qt не отдаёт ни одним способом.
// Раз рисуем сами — проверяем и то, что вокруг: след на прежнем месте, выделение
// и потерю фокуса. Ровно этого от кастомной каретки и опасаются.
void checkCaretPainting() {
    const zametti::Appearance saved = zametti::appearance();
    struct Restore {
        const zametti::Appearance& from;
        ~Restore() { zametti::appearance() = from; }
    } restore{saved};
    zametti::appearance().caretWidth = 4.0;
    zametti::appearance().caretColor = QColor(220, 30, 30);

    const QString path = writeNote("каретка-цвет.md", QStringLiteral("первая строка\n"));
    QWidget host;
    zametti::NoteEditor editor(&host);
    QLineEdit other(&host);
    editor.setGeometry(0, 0, 600, 200);
    other.setGeometry(0, 210, 600, 30);
    host.resize(600, 260);
    host.show();
    QTest::qWait(20);
    editor.setFocus();
    editor.openFile(path);
    QTest::qWait(20);

    // Каретка горит сразу после движения курсора, поэтому снимок берём тут же:
    // иначе кадр попал бы в погасшую фазу.
    // Ищем ровно цвет каретки, а не «что-нибудь красноватое»: подпалённые края
    // букв на выделении дают оранжевый, и на нём проверка ложно срабатывала.
    auto isCaret = [](QColor c) {
        return qAbs(c.red() - 220) < 12 && qAbs(c.green() - 30) < 12 && qAbs(c.blue() - 30) < 12;
    };
    auto ink = [&editor, isCaret](QPoint at) {
        const QImage shot = editor.viewport()->grab().toImage();
        int found = 0;
        for (int y = at.y() + 2; y <= at.y() + 10; ++y)
            for (int x = at.x(); x < at.x() + 6; ++x)
                if (shot.rect().contains(x, y) && isCaret(shot.pixelColor(x, y))) ++found;
        return found;
    };

    QTextCursor cursor = editor.textCursor();
    cursor.movePosition(QTextCursor::EndOfBlock);
    editor.setTextCursor(cursor);
    QTest::qWait(10);
    const QPoint away = editor.cursorRect().topLeft();
    check(ink(away) > 0, "каретка нарисована заданным цветом");

    cursor.setPosition(0);
    editor.setTextCursor(cursor);
    QTest::qWait(10);
    check(ink(away) == 0, "на прежнем месте следа не остаётся");
    check(ink(editor.cursorRect().topLeft()) > 0, "и она на новом месте");

    cursor.setPosition(5, QTextCursor::KeepAnchor);
    editor.setTextCursor(cursor);
    QTest::qWait(10);
    check(ink(editor.cursorRect().topLeft()) == 0, "при выделении каретка не рисуется");

    cursor.clearSelection();
    editor.setTextCursor(cursor);
    QTest::qWait(10);
    other.setFocus();
    QTest::qWait(30);
    check(ink(editor.cursorRect().topLeft()) == 0, "без фокуса каретки нет");
    editor.setFocus();
    QTest::qWait(20);
    check(ink(editor.cursorRect().topLeft()) > 0, "с возвратом фокуса — снова есть");
}

// Ширина каретки — настройка, и меряется она по нарисованному: своей каретки у
// Qt больше нет, её ширина всегда ноль.
void checkCaretWidth() {
    const zametti::Appearance saved = zametti::appearance();
    struct Restore {
        const zametti::Appearance& from;
        ~Restore() { zametti::appearance() = from; }
    } restore{saved};
    zametti::appearance().caretColor = QColor(220, 30, 30);

    const QString path = writeNote("каретка.md", QStringLiteral("текст\n"));

    // Сколько подряд закрашенных пикселей от левого края каретки.
    // Снимок берём у вьюпорта: cursorRect отдаёт именно его координаты, а у
    // виджета есть рамка, и по ней всё съезжает на пиксель.
    auto painted = [](zametti::NoteEditor& editor) {
        const QImage shot = editor.viewport()->grab().toImage();
        const QRect at = editor.cursorRect();
        int run = 0;
        for (int x = at.left(); x < at.left() + 20; ++x) {
            const QPoint p(x, at.top() + 5);
            if (!shot.rect().contains(p) || qAbs(shot.pixelColor(p).red() - 220) >= 12 ||
                qAbs(shot.pixelColor(p).green() - 30) >= 12)
                break;
            ++run;
        }
        return run;
    };

    for (const auto& [width, zoom, expected] :
         {std::tuple<qreal, qreal, int>{2.0, 1.0, 2}, {5.0, 1.0, 5}, {2.0, 2.0, 4}}) {
        zametti::appearance().caretWidth = width;
        zametti::NoteEditor editor;
        editor.resize(700, 300);
        editor.show();
        QTest::qWait(20);
        editor.setFocus();
        editor.setZoom(zoom);
        editor.openFile(path);
        QTest::qWait(20);
        QTextCursor cursor = editor.textCursor();
        cursor.movePosition(QTextCursor::EndOfBlock);
        editor.setTextCursor(cursor);
        QTest::qWait(10);
        checkEqual(QString::number(expected), QString::number(painted(editor)),
                   "ширина каретки: настройка " + std::to_string(int(width)) + ", масштаб " +
                       std::to_string(int(zoom)));
    }

    // Ноль в настройке каретку не прячет: меньше пикселя не бывает.
    zametti::appearance().caretWidth = 0.0;
    zametti::NoteEditor thin;
    thin.resize(700, 300);
    thin.show();
    QTest::qWait(20);
    thin.setFocus();
    thin.openFile(path);
    QTest::qWait(20);
    QTextCursor cursor = thin.textCursor();
    cursor.movePosition(QTextCursor::EndOfBlock);
    thin.setTextCursor(cursor);
    QTest::qWait(10);
    check(painted(thin) >= 1, "нулевая настройка не прячет каретку");
}

// Отмена правки, сделанной далеко за краем окна. Как футбольный арбитр: пока
// действие в пределах видимости — стоит на месте и картинку не дёргает; ушло за
// край — бежит в центр событий, а не к ближайшей кромке.
//
// Обычный ensureCursorVisible прокручивает ровно на минимум, и отменённая правка
// оказывалась впритык к нижнему или верхнему краю экрана.
void checkUndoShowsEditPlace() {
    // Заметка нарочно длинная, а правка в первой её трети: прокрутившись до
    // конца, мы уходим от места правки дальше, чем на окно, и после отмены оно
    // остаётся за краем. На короткой заметке отмена возвращает место обратно в
    // окно сама, и проверять было бы нечего.
    QString source;
    for (int i = 0; i < 600; ++i)
        source += QStringLiteral("Абзац номер %1, чтобы заметка была длинной.\n\n").arg(i);
    const QString path = writeNote("отмена-вид.md", source);

    auto edit = [&path](bool scrollAway, int* percent, int* moved) {
        zametti::NoteEditor editor;
        editor.resize(900, 600);
        editor.show();
        QTest::qWait(20);
        editor.setFocus();
        editor.openFile(path);
        QTest::qWait(30);

        // Правка посередине заметки: только там и есть куда центрировать.
        QTextCursor cursor = editor.textCursor();
        cursor.setPosition(editor.document()->findBlockByNumber(200).position());
        cursor.movePosition(QTextCursor::NextBlock, QTextCursor::KeepAnchor, 3);
        editor.setTextCursor(cursor);
        QTest::keyClick(&editor, Qt::Key_Delete);
        QTest::qWait(30);

        // До самого низа: место правки посреди заметки заведомо уходит за край.
        //
        // Ждём, пока раскладка досчитает высоту: сразу после открытия максимум
        // прокрутки ещё мал (замер: 4402 против настоящих 28830), и «в конец»
        // уводит недалеко — проверка мерила бы не то, что думает. Признак
        // готовности — максимум перестал расти.
        // Ctrl+End: так уходят в конец руками. Заодно досчитывается раскладка —
        // сразу после открытия высота документа ещё не известна, и прокрутка «в
        // конец» уводила бы недалеко (замер: максимум 4402 против настоящих
        // 28830). Курсор при этом уезжает вместе с видом, но отмене он и не
        // нужен: место правки она берёт из истории.
        if (scrollAway) QTest::keyClick(&editor, Qt::Key_End, Qt::ControlModifier);
        QTest::qWait(30);
        const int before = editor.verticalScrollBar()->value();


        QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
        QTest::qWait(50);
        *percent = editor.cursorRect().center().y() * 100 / editor.viewport()->height();
        *moved = editor.verticalScrollBar()->value() - before;
    };

    int percent = 0;
    int moved = 0;
    edit(true, &percent, &moved);
    check(percent > 35 && percent < 65,
          "отменённая правка из-за края окна показывается по центру, а не у кромки (вышло " +
              std::to_string(percent) + "%)");

    edit(false, &percent, &moved);
    checkEqual(QStringLiteral("0"), QString::number(moved),
               "а правку, которая и так на виду, вид не дёргает");
}

// Курсор не должен упираться в кромку окна. Qt прокручивает ровно до касания
// края, и поле страницы при этом уезжает за кромку: строка, которую набираешь,
// оказывается вплотную к рамке окна.
void checkCaretKeepsOffEdge() {
    const zametti::Appearance saved = zametti::appearance();
    struct Restore {
        const zametti::Appearance& from;
        ~Restore() { zametti::appearance() = from; }
    } restore{saved};
    zametti::appearance().verticalMargin = 1.5;

    QString source;
    for (int i = 0; i < 200; ++i)
        source += QStringLiteral("Абзац номер %1, чтобы заметка была длинной.\n\n").arg(i);
    const QString path = writeNote("зазор.md", source);

    zametti::NoteEditor editor;
    editor.resize(900, 500);
    editor.show();
    QTest::qWait(20);
    editor.setFocus();
    editor.openFile(path);
    QTest::qWait(30);

    const int gap = qRound(1.5 * QFontMetricsF(editor.baseFont()).height());
    auto near = [](int a, int b) { return qAbs(a - b) <= 2; };

    // Вниз до конца документа: на самом конце зазор держится полем страницы.
    for (int i = 0; i < 400; ++i) QTest::keyClick(&editor, Qt::Key_Down);
    QTest::qWait(30);
    const int below = editor.viewport()->height() - editor.cursorRect().bottom();
    check(near(below, gap), "внизу под кареткой остаётся зазор (вышло " +
                                std::to_string(below) + " при " + std::to_string(gap) + ")");

    for (int i = 0; i < 400; ++i) QTest::keyClick(&editor, Qt::Key_Up);
    QTest::qWait(30);
    const int above = editor.cursorRect().top();
    check(near(above, gap), "и наверху над ней тоже (вышло " + std::to_string(above) +
                                " при " + std::to_string(gap) + ")");

    // И посреди заметки, где упереться не во что.
    QTextCursor cursor = editor.textCursor();
    cursor.setPosition(editor.document()->findBlockByNumber(100).position());
    editor.setTextCursor(cursor);
    QTest::qWait(20);
    for (int i = 0; i < 60; ++i) QTest::keyClick(&editor, Qt::Key_Down);
    QTest::qWait(30);
    check(near(editor.viewport()->height() - editor.cursorRect().bottom(), gap),
          "зазор снизу держится и посреди заметки");
    for (int i = 0; i < 60; ++i) QTest::keyClick(&editor, Qt::Key_Up);
    QTest::qWait(30);
    check(near(editor.cursorRect().top(), gap), "и сверху посреди заметки");
}

// Пустые строки — содержимое заметки, а не мусор: ими отбивают куски текста.
// Набрали, сохранили, открыли заново — они на месте, и ровно в том же числе.
// В файл они уходят настоящими пустыми строками, без единого хитрого знака.
void checkBlankLinesSurviveSaving() {
    const QString path = writeNote("пустые-строки.md",
                                   QStringLiteral("- [ ] дело\n\nдо 19 июля:\n"));

    zametti::NoteEditor editor;
    editor.resize(700, 500);
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
    check(target > 0, "строка \"до 19\" должна найтись");

    QTextCursor cursor = editor.textCursor();
    cursor.setPosition(editor.document()->findBlockByNumber(target).position());
    editor.setTextCursor(cursor);
    for (int i = 0; i < 10; ++i) {
        QTest::keyClick(&editor, Qt::Key_Return);
        QTest::qWait(5);
    }
    // Считаем пустые СТРОКИ, а не блоки: десять Enter дают десять пустых строк
    // внутри одного абзаца, а не десять абзацев. Разрезает абзац только Enter на
    // пустой строке.
    auto blankLines = [&editor] {
        int count = 0;
        for (QTextBlock block = editor.document()->begin(); block.isValid();
             block = block.next()) {
            const QStringList lines =
                block.text().split(QChar::LineSeparator);
            for (const QString& line : lines)
                if (line.trimmed().isEmpty()) ++count;
        }
        return count;
    };
    // Одиннадцать, а не десять: одна пустая строка была в файле с самого начала,
    // и теперь она видна — это отдельный блок, а не невидимая отбивка.
    const int before = blankLines();
    checkEqual(QStringLiteral("11"), QString::number(before),
               "десять Enter дают десять пустых строк сверх бывшей в файле");

    editor.save(false);
    QTest::qWait(20);
    editor.openFile(writeNote("другая.md", QStringLiteral("другая\n")));
    QTest::qWait(20);
    editor.openFile(path);
    QTest::qWait(20);

    checkEqual(QString::number(before), QString::number(blankLines()),
               "пустые строки пережили запись и перечитывание");

    // А хвост пустых строк в конце заметки сохраняться не должен: он набирается
    // случайно и ничего не отбивает.
    QTextCursor tail = editor.textCursor();
    tail.movePosition(QTextCursor::End);
    editor.setTextCursor(tail);
    const int blocks = blankLines();
    for (int i = 0; i < 6; ++i) {
        QTest::keyClick(&editor, Qt::Key_Return);
        QTest::qWait(5);
    }
    editor.save(false);
    QTest::qWait(20);
    editor.openFile(writeNote("третья.md", QStringLiteral("третья\n")));
    QTest::qWait(20);
    editor.openFile(path);
    QTest::qWait(20);
    checkEqual(QString::number(blocks), QString::number(blankLines()),
               "хвост пустых строк в конце не сохраняется");
}

// Разделитель — прогон пустых строк, поставленный руками. Каждая пустая строка
// файла — свой блок, поэтому высота разделителя предсказуема сама собой: поле
// сверху, n высот строки, поле снизу. Курсор идёт по его строкам ровным шагом.
void checkSeparatorGeometry() {
    const zametti::Appearance saved = zametti::appearance();
    zametti::appearance().separatorSpacingBefore = 0.5;
    zametti::appearance().separatorSpacingAfter = 0.5;
    struct Restore {
        const zametti::Appearance& from;
        ~Restore() { zametti::appearance() = from; }
    } restore{saved};

    const QString path =
        writeNote("разделитель.md", QStringLiteral("- пункт\n\n\n\nабзац\n"));

    zametti::NoteEditor editor;
    editor.resize(700, 500);
    editor.show();
    QTest::qWait(20);
    editor.setFocus();
    editor.openFile(path);
    QTest::qWait(20);

    // Три пустые строки — три блока, а не один блок в три строки.
    checkEqual(QStringLiteral("5"), QString::number(editor.document()->blockCount()),
               "каждая пустая строка — свой блок");
    for (int i = 1; i <= 3; ++i)
        check(zametti::isVSpaceBlock(editor.document()->findBlockByNumber(i)),
              "блок " + std::to_string(i) + " — пустая строка");

    // Поля обрамляют прогон целиком: сверху перед первой пустой строкой, снизу
    // после последней. Внутри прогона полей нет — иначе высота трёх строк
    // перестала бы быть тремя высотами строки.
    auto marginOf = [&editor](int number) {
        return editor.document()->findBlockByNumber(number).blockFormat().topMargin();
    };
    const qreal line = editor.document()->findBlockByNumber(1).blockFormat().lineHeight();
    const qreal above = marginOf(1);
    const qreal below = marginOf(4);
    check(above > 0.0 && qAbs(above - below) < 0.01,
          "поля над и под разделителем равны между собой");
    check(qAbs(above / line - 0.5) < 0.1, "и составляют половину высоты строки");
    check(marginOf(2) <= 0.01 && marginOf(3) <= 0.01, "внутри разделителя полей нет");

    // Шаги курсора по пустым строкам одинаковы и равны высоте строки.
    QTextCursor cursor = editor.textCursor();
    cursor.setPosition(editor.document()->findBlockByNumber(1).position());
    editor.setTextCursor(cursor);
    QTest::qWait(10);

    auto lineTop = [&editor] {
        const QTextBlock block = editor.textCursor().block();
        const QRectF rect =
            editor.document()->documentLayout()->blockBoundingRect(block);
        const QTextLine at =
            block.layout()->lineForTextPosition(editor.textCursor().positionInBlock());
        return rect.top() + (at.isValid() ? at.y() : 0.0);
    };

    const qreal first = lineTop();
    QTest::keyClick(&editor, Qt::Key_Down);
    QTest::qWait(10);
    const qreal second = lineTop();
    QTest::keyClick(&editor, Qt::Key_Down);
    QTest::qWait(10);
    const qreal third = lineTop();

    check(qAbs((second - first) - (third - second)) < 0.01,
          "шаги по строкам разделителя одинаковы");
    check(qAbs((second - first) - line) < 0.01,
          "и равны высоте строки");
}

}  // namespace

// Точки записи истории: сохранение, внешняя правка, граница серии отмены.
//
// Проверяется не «журнал не пуст», а что в нём лежат ИМЕННО те байты, которые
// оказались на диске, и в том порядке, в каком случились события. Иначе
// история была бы правдоподобной, но не настоящей.
void checkHistoryPoints() {
    // Своё хранилище: истории нужен каталог history/ рядом с заметкой.
    const QString root = g_dir + QStringLiteral("/хранилище-истории");
    QDir().mkpath(root + QStringLiteral("/history"));
    const QString path = root + QStringLiteral("/01n6cqevh7bbfr.md");
    const QString noteId = QStringLiteral("01n6cqevh7bbfr");
    {
        QFile file(path);
        check(file.open(QIODevice::WriteOnly | QIODevice::Truncate), "заметка создана");
        file.write(QStringLiteral("# заметка\n\nстрока раз\n").toUtf8());
    }

    zametti::journal::History history(root);
    zametti::NoteEditor editor;
    editor.setStoreRoot(root);
    editor.resize(700, 500);
    editor.show();
    QTest::qWait(20);
    editor.setFocus();

    auto records = [&history, &noteId] {
        zametti::journal::Journal journal;
        QString error;
        history.read(noteId, &journal, &error);
        return journal.entries;
    };
    auto snapshot = [&history, &noteId](int index) {
        QByteArray got;
        QString error;
        if (!history.snapshotAt(noteId, index, &got, &error)) return QString();
        return QString::fromUtf8(got);
    };
    auto fileText = [&path] {
        QFile file(path);
        return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString();
    };

    editor.openFile(path);
    QTest::qWait(20);
    // Открытие кладёт в пустую историю опорную запись — то, с чем заметку
    // открыли. Без неё прошлое заметки, прожитой до появления журнала, было бы
    // недостижимо; см. checkHistoryBaseline.
    check(records().size() == 1, "открытие завело опорную запись");
    checkEqual(fileText(), snapshot(0), "и в ней заметка, как была на диске");

    // 1. Сохранение — запись save с теми же байтами, что легли в файл.
    QTextCursor at(editor.document()->findBlockByNumber(2));
    at.movePosition(QTextCursor::EndOfBlock);
    editor.setTextCursor(at);
    QTest::keyClicks(&editor, QStringLiteral(" tail"));
    editor.save(false);
    QTest::qWait(20);
    auto after = records();
    check(after.size() == 2, "сохранение записало шаг поверх опорного");
    check(after.size() == 2 && after[1].kind == zametti::journal::Kind::Save,
          "и это шаг save");
    checkEqual(fileText(), snapshot(1), "слепок — ровно то, что легло в файл");

    // Повторное сохранение без правок ничего не пишет: на диске уже это.
    editor.save(false);
    QTest::qWait(20);
    check(records().size() == 2, "сохранение без правок шага не добавляет");

    // 2. Внешняя правка — запись external с чужими байтами.
    const QString outside = QStringLiteral("# заметка\n\nстрока раз\n\nчужая правка\n");
    {
        QFile file(path);
        check(file.open(QIODevice::WriteOnly | QIODevice::Truncate), "чужая правка записана");
        file.write(outside.toUtf8());
    }
    // Ждём сторожа файлов: точку записи проверяем настоящим путём, а не
    // вызовом внутреннего метода, иначе проверялась бы не проводка, а функция.
    for (int i = 0; i < 100 && records().size() < 3; ++i) QTest::qWait(20);
    after = records();
    check(after.size() == 3, "внешняя правка записала шаг");
    check(after.size() == 3 && after[2].kind == zametti::journal::Kind::External,
          "и это шаг external");
    checkEqual(outside, snapshot(2), "слепок — чужие байты, как они есть на диске");

    // 3. Граница серии отмены: первое Ctrl+Z после правок сначала сохраняет,
    // иначе только что набранное не попало бы в историю вовсе.
    QTextCursor end(editor.document()->lastBlock());
    end.movePosition(QTextCursor::EndOfBlock);
    editor.setTextCursor(end);
    QTest::keyClicks(&editor, QStringLiteral(" before undo"));
    QTest::qWait(20);
    const int before = int(records().size());
    editor.undo();
    QTest::qWait(20);
    after = records();
    // Записи прибавилось, хотя правка мелкая: заменять было бы нечего — прошлая
    // запись здесь ВНЕШНЯЯ, а чужие вешки не стираются никогда.
    check(after.size() == before + 1, "первое Ctrl+Z после правок записало шаг");
    check(snapshot(int(after.size()) - 1).contains(QStringLiteral("before undo")),
          "первое Ctrl+Z после правок записало набранное");
    check(!after.isEmpty() && after.last().kind == zametti::journal::Kind::Save,
          "и это обычное сохранение, а не особая запись");
    check(snapshot(int(after.size()) - 1).contains(QStringLiteral("before undo")),
          "в истории осталось то, что отменили");

    // Отмена без правок второй записи не делает: сохранять нечего.
    const int settled = int(records().size());
    editor.undo();
    QTest::qWait(20);
    check(int(records().size()) == settled, "отмена без правок шага не пишет");
}

// Режим истории: вход, ходьба по слепкам, только чтение, восстановление.
//
// Главное, что здесь проверяется, — что живая заметка переживает поход в
// прошлое целиком: не только текст, но и цепочка отмены с кареткой. Пересборки
// из текста при возврате нет, и терять при нём нечего.
void checkHistoryMode() {
    const QString root = g_dir + QStringLiteral("/хранилище-режима");
    QDir().mkpath(root + QStringLiteral("/history"));
    const QString path = root + QStringLiteral("/01n6cqevsd7v5e.md");
    const QString noteId = QStringLiteral("01n6cqevsd7v5e");
    {
        QFile file(path);
        check(file.open(QIODevice::WriteOnly | QIODevice::Truncate), "заметка создана");
        file.write(QStringLiteral("# заметка\n\nодин\n").toUtf8());
    }

    zametti::journal::History history(root);
    zametti::NoteEditor editor;
    editor.setStoreRoot(root);
    editor.resize(700, 500);
    editor.show();
    QTest::qWait(20);
    editor.setFocus();

    auto text = [&editor] {
        return QString::fromStdString(
            zametti::serialize(zametti::readDocument(*editor.document())));
    };

    editor.openFile(path);
    QTest::qWait(20);

    // Три сохранения — три слепка в истории.
    //
    // Куски КРУПНЫЕ нарочно. Мелкая правка теперь не заводит новую запись, а
    // заменяет прошлую (historyMergeChars, решение владельца): двумя короткими
    // словами тут получилось бы две записи вместо трёх, и проверка режима
    // истории проверяла бы не то.
    for (const char* piece : {" two two two two two two two two two two two two two two two"
                              " two two two two two two two two two two two two two two two",
                              " three three three three three three three three three three"
                              " three three three three three three three three three three"}) {
        QTextCursor at(editor.document()->lastBlock());
        at.movePosition(QTextCursor::EndOfBlock);
        editor.setTextCursor(at);
        QTest::keyClicks(&editor, QString::fromLatin1(piece));
        // Пауза длиннее склейки набора: иначе обе серии стали бы одним шагом
        // отмены, и проверка «цепочка пережила поход» проверяла бы не то.
        QTest::qWait(zametti::appearance().undoCoalesceMs + 50);
        editor.save(false);
        QTest::qWait(20);
    }
    const QString live = text();

    // Вход в режим: показан последний слепок, править нельзя.
    check(!editor.inHistory(), "до входа режима нет");
    check(editor.enterHistory(), "вход в режим истории");
    check(editor.inHistory(), "режим идёт");
    check(editor.isReadOnly(), "в слепке править нельзя");
    // Записей три: опорная (с чем открыли) и два сохранения.
    check(editor.timeline().entries.size() == 3, "таймлайн знает про все три записи");
    check(editor.historyIndex() == 2, "показан последний слепок");
    checkEqual(live, text(), "последний слепок совпадает с живой версией");

    // Шаг назад — более старый слепок.
    check(editor.historyStepBack(), "шаг в прошлое");
    check(editor.historyIndex() == 1, "показан предыдущий слепок");
    check(text().contains(QStringLiteral("two")) && !text().contains(QStringLiteral("three")),
          "в нём нет того, что дописали позже");
    check(editor.historyStepBack(), "ещё шаг — к опорной записи");
    check(editor.historyIndex() == 0, "показана опорная запись");
    check(!text().contains(QStringLiteral("two")),
          "опорная запись — заметка, какой её открыли");
    check(!editor.historyStepBack(), "дальше опорной записи ходу нет");
    check(editor.historyIndex() == 0, "и мы остались на ней же");

    // Печатающая клавиша не восстанавливает и не правит.
    int refusals = 0;
    QObject::connect(&editor, &zametti::NoteEditor::historyEditRefused,
                     [&refusals] { ++refusals; });
    const QString beforeTyping = text();
    QTest::keyClicks(&editor, QStringLiteral("x"));
    QTest::qWait(10);
    checkEqual(beforeTyping, text(), "печатающая клавиша слепок не меняет");
    check(refusals == 1, "и про отказ сказано вслух");

    // Копировать из прошлого можно — ради этого режим и заведён.
    editor.selectAll();
    editor.copy();
    check(!QApplication::clipboard()->text().isEmpty(), "из слепка копируется");

    // Шагами вперёд — до последнего слепка и дальше, в живую версию.
    check(editor.historyStepForward(), "шаг в будущее");
    check(editor.historyIndex() == 1, "предыдущий слепок");
    check(editor.historyStepForward(), "ещё шаг");
    check(editor.historyIndex() == 2, "снова последний слепок");
    check(editor.historyStepForward(), "шаг дальше последнего");
    check(!editor.inHistory(), "и он вывел в живую версию");
    check(!editor.isReadOnly(), "живую версию снова можно править");
    checkEqual(live, text(), "живая версия вернулась целой");

    // Цепочка отмены пережила поход: Ctrl+Z отменяет правку, сделанную ДО него.
    // Отменяется СЛОВО (шаг отмены теперь пословный), поэтому смотрим не на
    // «пропало ли three целиком», а на то, что его стало на одно меньше.
    const auto countOf = [](const QString& where, const QString& what) {
        int seen = 0;
        for (qsizetype at = where.indexOf(what); at >= 0; at = where.indexOf(what, at + 1)) ++seen;
        return seen;
    };
    const int threesBefore = countOf(text(), QStringLiteral("three"));
    editor.undo();
    QTest::qWait(10);
    check(text().contains(QStringLiteral("two")) &&
              countOf(text(), QStringLiteral("three")) == threesBefore - 1,
          "отмена после возврата отменяет правку, а не поход в историю");
    editor.redo();
    QTest::qWait(10);

    // Восстановление: новая запись, отдельным шагом отмены.
    const int recordsBefore = [&] {
        zametti::journal::Journal journal;
        QString error;
        history.read(noteId, &journal, &error);
        return int(journal.entries.size());
    }();
    check(editor.enterHistory(0), "вход на первый слепок");
    const QString old = text();
    const qint64 source = editor.restoreShownSnapshot();
    QTest::qWait(20);
    check(source != 0, "восстановление состоялось");
    check(!editor.inHistory(), "и режим закрылся");
    checkEqual(old, text(), "в живой заметке теперь содержимое слепка");

    zametti::journal::Journal journal;
    QString error;
    history.read(noteId, &journal, &error);
    check(int(journal.entries.size()) == recordsBefore + 1,
          "восстановление дописало ровно одну запись");
    check(!journal.entries.isEmpty() &&
              journal.entries.last().kind == zametti::journal::Kind::Restore,
          "и это запись restore");
    check(!journal.entries.isEmpty() && journal.entries.last().source == source,
          "в записи назван источник");

    // Инвариант C: журнал не укоротился.
    check(int(journal.entries.size()) > recordsBefore, "журнал только вырос");

    // Слепок, совпадающий с нынешней версией, не восстанавливается: журнал не
    // растёт, цепочка отмены не засоряется пустым шагом, и человеку говорят
    // правду, а не «восстановлено».
    {
        const int wasRecords = [&] {
            zametti::journal::Journal journal;
            QString e;
            history.read(noteId, &journal, &e);
            return int(journal.entries.size());
        }();
        const int wasUndo = editor.undoSteps();
        check(editor.enterHistory(), "вход в историю на последний слепок");
        bool alreadyCurrent = false;
        const qint64 same = editor.restoreShownSnapshot(&alreadyCurrent);
        QTest::qWait(20);
        check(same == 0 && alreadyCurrent, "восстановление того же самого — не восстановление");
        check(!editor.inHistory(), "и режим всё равно закрылся");
        const int nowRecords = [&] {
            zametti::journal::Journal journal;
            QString e;
            history.read(noteId, &journal, &e);
            return int(journal.entries.size());
        }();
        check(nowRecords == wasRecords, "журнал не вырос");
        check(editor.undoSteps() == wasUndo, "и пустого шага отмены не добавилось");
    }

    // Ctrl+Z сразу после восстановления отменяет восстановление.
    editor.undo();
    QTest::qWait(20);
    checkEqual(live, text(), "отмена вернула то, что было до восстановления");
}

// Заметка старше своего журнала — и её прошлое обязано быть достижимо.
//
// Хранилище жило годами, история заведена только сейчас. Если не положить в
// пустой журнал опорную запись при открытии, первой записью станет первое
// сохранение — и всё, чем заметка была до него, не попадёт в историю никогда.
// Владелец наткнулся на это живьём: опустошил заметку (Ctrl+A, Delete),
// автосохранение записало пустоту, и она оказалась самой первой записью.
//
// Инвариант, который здесь стережётся, владелец назвал так: НАЧАЛЬНОЕ
// СОСТОЯНИЕ ДОКУМЕНТА НЕ ДОЛЖНО БЫТЬ НЕДОСТИЖИМО.
void checkHistoryBaseline() {
    const QString root = g_dir + QStringLiteral("/хранилище-опоры");
    QDir().mkpath(root + QStringLiteral("/history"));
    const QString path = root + QStringLiteral("/01n6n787fntjy8.md");
    const QString noteId = QStringLiteral("01n6n787fntjy8");
    const QString original =
        QStringLiteral("<!-- zametti\ncreated: 2023-06-02T23:00:02Z\n-->\n\n"
                       "# Python acceleration\n\nvery old text\n");
    {
        QFile file(path);
        check(file.open(QIODevice::WriteOnly | QIODevice::Truncate), "старая заметка на месте");
        file.write(original.toUtf8());
    }

    zametti::journal::History history(root);
    zametti::NoteEditor editor;
    editor.setStoreRoot(root);
    editor.resize(700, 500);
    editor.show();
    QTest::qWait(20);
    editor.setFocus();

    auto records = [&history, &noteId] {
        zametti::journal::Journal journal;
        QString error;
        history.read(noteId, &journal, &error);
        return journal.entries;
    };
    auto snapshot = [&history, &noteId](int index) {
        QByteArray got;
        QString error;
        if (!history.snapshotAt(noteId, index, &got, &error)) return QString();
        return QString::fromUtf8(got);
    };

    check(records().isEmpty(), "у старой заметки истории ещё нет");
    editor.openFile(path);
    QTest::qWait(20);

    auto after = records();
    check(after.size() == 1, "открытие завело опорную запись");
    checkEqual(original, snapshot(0), "и в ней заметка, как была на диске");
    // Время опорной записи — файла, а не «сейчас»: содержимое ровно такой
    // давности, и таймлайн не должен утверждать, будто оно свежее.
    check(!after.isEmpty() &&
              qAbs(after[0].time - QFileInfo(path).lastModified().toMSecsSinceEpoch()) < 2000,
          "время опорной записи взято у файла");

    // Повторное открытие второй опорной не плодит.
    editor.openFile(path);
    QTest::qWait(20);
    check(records().size() == 1, "повторное открытие опорную не удваивает");

    // Теперь то, на чём владелец обжёгся: выделить всё и стереть.
    QTest::keyClick(&editor, Qt::Key_A, Qt::ControlModifier);
    QTest::keyClick(&editor, Qt::Key_Delete);
    QTest::qWait(20);
    editor.save(false);
    QTest::qWait(20);
    after = records();
    check(after.size() == 2, "опустошение записано вторым шагом");

    // Главное: начальное состояние достижимо. Первый шаг назад из истории
    // приводит к тому, с чего заметка начиналась, а не в пустоту.
    check(editor.enterHistory(), "вход в историю");
    check(editor.historyStepBack(), "шаг в прошлое");
    check(editor.historyIndex() == 0, "и он привёл к самой первой записи");
    checkEqual(original, snapshot(editor.historyIndex()),
               "начальное состояние заметки достижимо");
    editor.leaveHistory();
}

// Уход в другую заметку обязан выводить из режима истории. Иначе редактор
// показывает слепок ПРЕЖНЕЙ заметки, имея путь новой, и первая же правка
// записала бы чужое прошлое в чужой файл.
void checkHistoryLeavesOnOpen() {
    const QString root = g_dir + QStringLiteral("/хранилище-ухода");
    QDir().mkpath(root + QStringLiteral("/history"));
    const QString first = root + QStringLiteral("/01n6cqevh7bbf1.md");
    const QString second = root + QStringLiteral("/01n6cqevh7bbf2.md");
    for (const QString& path : {first, second}) {
        QFile file(path);
        check(file.open(QIODevice::WriteOnly | QIODevice::Truncate), "заметка создана");
        file.write(QStringLiteral("<!-- zametti\ncreated: 2023-06-02T23:00:02Z\n-->\n\n"
                                  "# %1\n\nтекст\n")
                       .arg(QFileInfo(path).completeBaseName())
                       .toUtf8());
    }

    zametti::NoteEditor editor;
    editor.setStoreRoot(root);
    editor.resize(700, 500);
    editor.show();
    QTest::qWait(20);

    editor.openFile(first);
    QTest::qWait(20);
    check(editor.enterHistory(), "вошли в историю первой заметки");
    check(editor.inHistory(), "режим идёт");

    editor.openFile(second);
    QTest::qWait(20);
    check(!editor.inHistory(), "открытие другой заметки вывело из режима");
    check(!editor.isReadOnly(), "и править её можно");
    check(editor.document()->toPlainText().contains(QStringLiteral("bbf2")),
          "показана именно вторая заметка");

    // И первая при этом цела: чужой слепок в неё не уехал.
    QFile file(first);
    check(file.open(QIODevice::ReadOnly), "первая заметка на месте");
    check(QString::fromUtf8(file.readAll()).contains(QStringLiteral("bbf1")),
          "первая заметка не перезаписана слепком");
}

// Открыли заметку — печатать можно сразу.
//
// Беда владельца, дважды: сперва у новых заметок, потом у обычных. Заметка
// открывается, место каретки восстанавливается, а каретки не видно и нажатия
// уходят в никуда — фокус ввода остался в панели, из которой заметку выбрали, а
// каретку Qt рисует ТОЛЬКО в виджете с фокусом. Лечится не в местах выбора
// (их много, и каждое новое забудут), а в самом openFile.
//
// Проверка ставит рядом с редактором список — как средняя колонка в окне, — и
// спрашивает окно, кому оно отдало ввод. Спрашивать hasFocus() нельзя: под
// offscreen окно не становится активным, и он всегда ложь.
void checkOpenTakesCaretAndFocus() {
    const QString first = writeNote("фокус-первая.md",
                                    QStringLiteral("# первая\n\nстрока раз\nстрока два\n"));
    const QString second = writeNote("фокус-вторая.md", QStringLiteral("# вторая\n\nтекст\n"));

    QWidget window;
    auto* layout = new QVBoxLayout(&window);
    auto* panel = new QListWidget(&window);
    panel->addItem(QStringLiteral("строка"));
    auto* editor = new zametti::NoteEditor(&window);
    layout->addWidget(panel);
    layout->addWidget(editor);
    window.resize(700, 500);
    window.show();
    QTest::qWait(20);

    panel->setFocus();
    QTest::qWait(10);
    check(window.focusWidget() == panel, "ввод отдан панели — так бывает при выборе мышью");

    editor->openFile(first);
    QTest::qWait(20);
    check(window.focusWidget() == editor, "открытая заметка забрала ввод себе");

    // Выделение — не просто место каретки: у него два конца, и вернуться
    // обязаны оба (просьба владельца).
    QTextCursor pick(editor->document()->findBlockByNumber(2));
    pick.movePosition(QTextCursor::StartOfBlock);
    pick.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor, 6);
    editor->setTextCursor(pick);
    const int anchor = pick.anchor();
    const int position = pick.position();
    check(anchor != position, "выделение и правда есть");

    // Дорога первая: заметка вернулась из кэша, документ не пересобирался.
    editor->openFile(second);
    QTest::qWait(20);
    panel->setFocus();
    editor->openFile(first);
    QTest::qWait(20);
    check(editor->textCursor().anchor() == anchor && editor->textCursor().position() == position,
          "из кэша вернулось выделение целиком, а не одна каретка");
    check(window.focusWidget() == editor, "и ввод снова в тексте");

    // Дорога вторая: кэша нет, заметка собирается с диска заново. Место
    // каретки живёт тогда в отдельной карте, и выделение обязано быть и там —
    // иначе беда возвращалась бы, стоит заметке вытесниться из кэша.
    editor->openFile(second);
    QTest::qWait(20);
    editor->clearNoteCache();
    panel->setFocus();
    editor->openFile(first);
    QTest::qWait(20);
    check(editor->textCursor().anchor() == anchor && editor->textCursor().position() == position,
          "и с диска выделение вернулось таким же");
    check(window.focusWidget() == editor, "и ввод снова в тексте");

    // Единственное исключение: ходьба стрелками по панели. Там ↑/↓ обязаны
    // листать список, и утащить фокус значило бы отобрать саму ходьбу.
    panel->setFocus();
    editor->openFile(second, false);
    QTest::qWait(20);
    check(window.focusWidget() == panel, "ходьба стрелками ввод не отбирает");
}

// Найденное красится СВОИМ цветом, а не цветом выделения.
//
// Проверка идёт ПО СНИМКУ и считает точки нужного цвета: цвет подсветки видно
// только на экране, и «поставили в extraSelections» ещё не значит «человек это
// увидел». Ровно так и вышло бы, спроси я редактор: Qt рисует выделение ПОВЕРХ
// подсветки, и у текущей находки на экране цвет выделения, а не поиска.
//
// Отсюда и устройство проверки: считаются подсветки НЕ ТЕКУЩИХ находок. Их
// цвет — тот, что назначен в облике, и никем не перекрыт.
void checkSearchPaintsWithItsOwnColour() {
    const QString path = writeNote("подсветка.md",
                                   QStringLiteral("# заметка\n\nсосна и сосна\n"));
    const QColor keepSearch = zametti::appearance().searchHighlight;
    const QColor keepSelection = zametti::appearance().selectionBackground;
    // Цвета нарочно разные и ни на что не похожие: совпади они — проверка
    // прошла бы и на прежнем коде, бравшем цвет выделения. Оба непрозрачные и
    // далёкие от фона страницы, чтобы точки считались без догадок.
    zametti::appearance().searchHighlight = QColor(0x11, 0x99, 0x33);
    zametti::appearance().selectionBackground = QColor(0xcc, 0x22, 0x88);

    zametti::NoteEditor editor;
    editor.resize(700, 500);
    editor.show();
    QTest::qWait(20);
    editor.openFile(path);
    QTest::qWait(20);

    const int found = editor.findMatches(QStringLiteral("сосна"), false);
    check(found == 2, "нашлись оба вхождения");
    // Никуда не шагаем: пока по находкам не пошли, текущей нет вовсе и обе
    // подсветки видны как есть. Это же и есть обычный вид при наборе запроса.
    QTest::qWait(20);

    // Подсветка НЕ ТЕКУЩЕЙ находки полупрозрачна (так она уступает текущей), и
    // на экране лежит не сам цвет, а его смесь с фоном страницы в неизвестной
    // нам доле. Поэтому ищем точки, которые лежат НА ОТРЕЗКЕ «фон → искомый
    // цвет»: доля выясняется по каналу с наибольшим размахом и проверяется по
    // остальным. Цвета выше подобраны так, что отрезки до них расходятся
    // широко, и спутать их нельзя.
    const QColor page = zametti::appearance().pageBackground;
    auto count = [&editor, &page](const QColor& want) {
        const QImage shot = editor.viewport()->grab().toImage();
        const int span[3] = {want.red() - page.red(), want.green() - page.green(),
                             want.blue() - page.blue()};
        int widest = 0;
        for (int i = 1; i < 3; ++i)
            if (qAbs(span[i]) > qAbs(span[widest])) widest = i;
        if (span[widest] == 0) return -1;   // цвет неотличим от фона: считать нечего

        int painted = 0;
        for (int y = 0; y < shot.height(); ++y)
            for (int x = 0; x < shot.width(); ++x) {
                const QColor c = shot.pixelColor(x, y);
                const int got[3] = {c.red() - page.red(), c.green() - page.green(),
                                    c.blue() - page.blue()};
                const double part = double(got[widest]) / double(span[widest]);
                // Доля меньше четверти — это уже почти чистый фон, и по нему
                // отрезки всех цветов сходятся в одну точку.
                if (part < 0.25 || part > 1.05) continue;
                bool fits = true;
                for (int i = 0; i < 3; ++i)
                    if (qAbs(double(got[i]) - part * span[i]) > 4.0) fits = false;
                if (fits) ++painted;
            }
        return painted;
    };

    const int mine = count(zametti::appearance().searchHighlight);
    const int theirs = count(zametti::appearance().selectionBackground);
    check(mine > 200, "находки закрашены цветом поиска (" + std::to_string(mine) + " точек)");
    check(theirs == 0, "и ни одной точки цветом выделения (" + std::to_string(theirs) + ")");

    zametti::appearance().searchHighlight = keepSearch;
    zametti::appearance().selectionBackground = keepSelection;
}

static int ztRunSuite(int argc, char** argv) {
    if (argc < 2) {
        std::printf("использование: editor_test <каталог для временных файлов>\n");
        return 2;
    }

    // Окно слипания набора укорачиваем: иначе пауза в тесте была бы почти
    // секундой на каждую проверку.
    zametti::appearance().undoCoalesceMs = 40;

    g_dir = QString::fromLocal8Bit(argv[1]) + QStringLiteral("/editor-data");
    QDir(g_dir).removeRecursively();
    if (!QDir().mkpath(g_dir)) {
        std::printf("не создать каталог %s\n", g_dir.toUtf8().constData());
        return 2;
    }

    checkOpenDoesNotTouchFile();
    checkStatsFreshness();
    checkSelectionShowsCode();
    checkMetaSurvivesEditing();
    checkEmptyNoteCaret();
    checkCaretPainting();
    checkCaretWidth();
    checkUndoShowsEditPlace();
    checkCaretKeepsOffEdge();
    checkUndoKeepsAppearance();
    checkAppearanceMakesNoHistoryStep();
    checkFirstEditAfterOpenIsUndoable();
    checkKeysAreOperations();
    checkListKeys();
    checkMoveKeys();
    checkInlineStyle();
    checkInputRules();
    checkSelectionSurvivesOperation();
    checkUndoFromKeyboard();
    checkDeferredSnapshot();
    checkWideWindowOperations();
    checkNoteCache();
    checkOpenTakesCaretAndFocus();
    checkSearchPaintsWithItsOwnColour();
    checkHistoryPoints();
    checkHistoryMode();
    checkHistoryBaseline();
    checkHistoryLeavesOnOpen();
    checkCanonicaliseOnOpen();
    checkSaveWithoutAutosave();
    checkSizeAfterSoftBreak();
    checkCodeTyping();
    checkCodeAtEdge();
    checkUndoReturnsToBlankLine();
    checkCheckboxClick();
    checkScrollHolds();
    checkScrollHoldsWhenBlockChangesHeight();
    checkLinkDoesNotGrow();
    checkCheckboxClickWithSelection();
    checkUndoKeepsCursor();
    checkViewHoldsForEveryOperation();
    checkColumnAcrossMargins();
    checkSelectionHasNoGaps();
    checkBlankLinesSurviveSaving();
    checkSeparatorGeometry();

    std::printf("проверок %d, провалов %d\n", zt::g_checks, zt::g_failures);
    return zt::g_failures == 0 ? 0 : 1;
}

// Набор целиком одним TEST: тело не тронуто, argv ему собран здесь.
// Дробить на отдельные проверки — отдельная работа, по одному набору.
TEST(Editor, All) {
    std::vector<QByteArray> ztArgs{QByteArrayLiteral("editor_test")};
    ztArgs.push_back((zt::TestData::outDir(QStringLiteral("editor"))).toLocal8Bit());
    std::vector<char*> ztArgv;
    for (QByteArray& a : ztArgs) ztArgv.push_back(a.data());
    EXPECT_EQ(0, ztRunSuite(int(ztArgv.size()), ztArgv.data()));
}
