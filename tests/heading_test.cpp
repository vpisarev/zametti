// Смена уровня заголовка: матрица «что было × во что превращаем × где стоит».
//
// Правило владельца: операция меняет ТОЛЬКО род блока и ничего больше. Пустых
// строк вокруг заголовка мы не заводим — их ставит человек. Единственное
// исключение делает не эта операция, а инвариант пустых строк: если без
// разделителя блоки слиплись бы в файле, пустая строка появится сама.
//
// Первый заход обособлял заголовок всегда, «чтобы красивее». Владелец на это
// и указал: пустые строки, взявшиеся там, где их не просили, раздражают
// сильнее, чем отсутствие отбивки.
//
// Проверяется КРУГ ЧЕРЕЗ ФАЙЛ, а не только вид документа: род блока живёт в
// свойствах, и ошибка в них видна лишь тогда, когда документ уедет на диск и
// прочтётся обратно. «Выглядит как заголовок» тут не значит ничего.

#include "document_builder.h"
#include "document_reader.h"
#include "editor_ops.h"
#include "parser.h"
#include "serializer.h"
#include "test_util.h"

#include <QGuiApplication>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>

#include <string>

namespace {

// markdown → документ → операция → markdown. Курсор ставится на строку с
// заданным текстом — ИМЕННО НА СТРОКУ, а не на блок: абзац со стихотворными
// переносами это один блок, и внутри него строк несколько.
std::string apply(const std::string& source, const std::string& onLine, int level,
                  bool selectAll = false) {
    const zametti::Document ir = zametti::parse(source);
    QTextDocument doc;
    zametti::buildDocument(ir, doc);

    QTextCursor caret(&doc);
    bool found = false;
    const QString want = QString::fromStdString(onLine);
    for (QTextBlock block = doc.begin(); block.isValid() && !found; block = block.next()) {
        const QString text = block.text();
        if (text == want) {
            caret.setPosition(block.position());
            found = true;
            break;
        }
        // Строка ВНУТРИ блока: ищем её между мягкими переносами.
        int at = 0;
        for (const QString& line : text.split(QChar::LineSeparator)) {
            if (line == want) {
                caret.setPosition(block.position() + at);
                caret.setPosition(block.position() + at + line.size(),
                                  QTextCursor::KeepAnchor);
                found = true;
                break;
            }
            at += line.size() + 1;
        }
    }
    if (!found) return "СТРОКА НЕ НАЙДЕНА: " + onLine;
    if (selectAll) {
        caret.setPosition(0);
        caret.movePosition(QTextCursor::End, QTextCursor::KeepAnchor);
    }

    zametti::setHeadingLevel(doc, caret, level);
    return zametti::serialize(zametti::readDocument(doc));
}

void check(const std::string& what, const std::string& expected, const std::string& actual) {
    ZT_EQ(what, expected, actual);
    // КРУГ: получившийся markdown обязан прочитаться в то же самое. Пустых
    // строк мы вокруг заголовка не ставим, и это ровно тот случай, где склейка
    // блоков в файле укусила бы молча — «- раз / ## два» читается верно, а
    // что-нибудь похожее могло бы и слипнуться.
    if (expected != actual) return;   // уже красное, второй раз не шумим
    const std::string again = zametti::serialize(zametti::parse(actual));
    ZT_EQ(what + ": и переживает круг через разбор", actual, again);
}

}  // namespace

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);

    // --- текст стоит обособленно: превращается как есть -------------------
    check("одинокий абзац становится заголовком",
          "# Заголовок\n\nхвост\n",
          apply("Заголовок\n\nхвост\n", "Заголовок", 1));

    check("заголовок первого уровня становится вторым",
          "## Заголовок\n\nхвост\n",
          apply("# Заголовок\n\nхвост\n", "Заголовок", 2));

    check("заголовок становится обычным текстом",
          "Заголовок\n\nхвост\n",
          apply("## Заголовок\n\nхвост\n", "Заголовок", 0));

    // --- текст посреди другого текста: обособляется ------------------------
    check("абзац посреди текста: пустые строки как были, так и остались",
          "первый\n\n## середина\n\nтретий\n",
          apply("первый\n\nсередина\n\nтретий\n", "середина", 2));

    // --- внутри списка: список рвётся -------------------------------------
    //
    // Пункт, ставший заголовком, перестаёт быть пунктом, и список продолжается
    // после него новым. Пустых строк не заводим: markdown читает это верно и
    // без них, а лишние строки в файле — не наше дело.
    check("пункт списка становится заголовком, список рвётся",
          "- раз\n## два\n- три\n",
          apply("- раз\n- два\n- три\n", "два", 2));

    // --- строка внутри абзаца ---------------------------------------------
    //
    // Абзац со стихотворными переносами — ОДИН блок. Заголовком обязана стать
    // выделенная строка, а не весь абзац: владелец наткнулся на это дважды —
    // сперва при наборе «# », теперь при смене уровня.
    check("строка посреди абзаца становится заголовком одна",
          "первая\n## вторая\nтретья\n",
          apply("первая\nвторая\nтретья\n", "вторая", 2));

    check("первая строка абзаца становится заголовком одна",
          "## первая\nвторая\nтретья\n",
          apply("первая\nвторая\nтретья\n", "первая", 2));

    check("последняя строка абзаца становится заголовком одна",
          "первая\nвторая\n## третья\n",
          apply("первая\nвторая\nтретья\n", "третья", 2));

    // --- чего трогать нельзя ----------------------------------------------
    check("строка кода заголовком не становится",
          "```\nкод\n```\n",
          apply("```\nкод\n```\n", "код", 1));

    check("пустая строка заголовком не становится",
          "раз\n\nдва\n",
          apply("раз\n\nдва\n", "", 1));

    // --- выделение из нескольких блоков -----------------------------------
    check("выделение целиком приводится к одному уровню",
          "### раз\n\n### два\n",
          apply("раз\n\nдва\n", "раз", 3, true));

    return zt::report("уровень заголовка");
}
