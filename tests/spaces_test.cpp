// Пробелы: неразрывные там, где они значат, и обычные там, где не значат.
//
// Правило выведено ЗАМЕРОМ по корпусу владельца (274 заметки): ведущих
// неразрывных 179, одиночных в середине строк 346, серий из двух и более — ни
// одной. Значит одиночный в середине это мусор из чужой выгрузки, а серия —
// выравнивание, которое ставим мы сами.
//
// Три места, и все три обязаны сходиться:
//   загрузка   — одиночные в середине становятся обычными;
//   в код      — все неразрывные становятся обычными (в коде значим сам пробел);
//   из кода    — ведущие и серии становятся неразрывными (markdown их съест).

#include "document_builder.h"
#include "document_reader.h"
#include "document_saver.h"
#include "editor_ops.h"
#include "parser.h"
#include "serializer.h"
#include "test_util.h"

#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>

#include <string>

namespace {

const std::string kNbsp = "\xC2\xA0";

QString g_dir;

// Загрузка через тот же путь, что и у программы: файл причёсывается на месте.
std::string afterLoad(const std::string& source) {
    const QString path = QDir(g_dir).filePath(QStringLiteral("проба.md"));
    QFile file(path);
    if (file.open(QIODevice::WriteOnly)) file.write(source.data(), qint64(source.size()));
    file.close();

    std::string text = source;
    zametti::Digest digest = zametti::hashOf(text);
    zametti::canonicaliseNoteFile(path, text, digest);
    return text;
}

// Абзац → код и обратно, через настоящую операцию.
std::string toggle(const std::string& source) {
    QTextDocument doc;
    zametti::buildDocument(zametti::parse(source), doc);
    QTextCursor caret(&doc);
    caret.setPosition(0);
    caret.movePosition(QTextCursor::End, QTextCursor::KeepAnchor);

    const zametti::MoveResult moved = zametti::toggleCodeBlock(doc, caret);
    if (!moved.done) return "ОПЕРАЦИЯ НЕ СРАБОТАЛА";
    return zametti::serialize(moved.doc);
}

// Читаемый вид: неразрывный пробел показываем как «~», иначе провал в отчёте
// неотличим от совпадения.
std::string visible(const std::string& text) {
    std::string out;
    for (size_t i = 0; i < text.size();) {
        if (text.compare(i, kNbsp.size(), kNbsp) == 0) {
            out += '~';
            i += kNbsp.size();
            continue;
        }
        out.push_back(text[i]);
        ++i;
    }
    return out;
}

void checkLoad() {
    const std::string head = "<!-- zametti\ncreated: 2020-01-01T00:00:00Z\n-->\n\n";

    // Одиночный в середине — мусор: становится обычным.
    ZT_EQ("одиночный неразрывный между слов заменён",
          visible(head + "два слова\n"),
          visible(afterLoad(head + "два" + kNbsp + "слова\n")));

    // Ведущие — наш отступ, остаются.
    ZT_EQ("ведущие неразрывные целы",
          visible(head + kNbsp + kNbsp + "отступ\n"),
          visible(afterLoad(head + kNbsp + kNbsp + "отступ\n")));

    // Серия в середине — выравнивание, остаётся.
    ZT_EQ("серия неразрывных в середине цела",
          visible(head + "int a" + kNbsp + kNbsp + kNbsp + "= 5\n"),
          visible(afterLoad(head + "int a" + kNbsp + kNbsp + kNbsp + "= 5\n")));
}

void checkToCode() {
    // В код: неразрывные становятся обычными, в том числе ведущие.
    const std::string source = "int a" + kNbsp + kNbsp + "= 5\n" + kNbsp + "отступ\n";
    const std::string got = toggle(source);
    ZT_TRUE("в блоке кода неразрывных не осталось",
            got.find(kNbsp) == std::string::npos);
    ZT_TRUE("а сами пробелы на месте: " + visible(got),
            got.find("int a  = 5") != std::string::npos);
}

void checkFromCode() {
    // Из кода: ведущие и серии становятся неразрывными, одиночные — нет.
    const std::string source = "```\nint a    = 5;\nint bcdef = 6;\n  отступ\n```\n";
    const std::string got = toggle(source);
    ZT_TRUE("столбик удержан неразрывными: " + visible(got),
            got.find("int a" + kNbsp + kNbsp + kNbsp + kNbsp + "= 5;") != std::string::npos);
    ZT_TRUE("ведущий отступ удержан: " + visible(got),
            got.find(kNbsp + kNbsp + "отступ") != std::string::npos);
    ZT_TRUE("одиночный пробел между словами остался обычным: " + visible(got),
            got.find("int bcdef = 6;") != std::string::npos);
}

// Круг: то, что вышло из кода, обязано пережить загрузку. Иначе нормализация
// съела бы выравнивание, которое мы сами и поставили, — а это ровно та беда,
// на которой два правила расходятся молча.
void checkRoundTrip() {
    const std::string head = "<!-- zametti\ncreated: 2020-01-01T00:00:00Z\n-->\n\n";
    const std::string fromCode = toggle("```\nint a    = 5;\n```\n");
    const std::string loaded = afterLoad(head + fromCode);
    ZT_TRUE("выравнивание пережило загрузку: " + visible(loaded),
            loaded.find("int a" + kNbsp + kNbsp + kNbsp + kNbsp + "= 5;") != std::string::npos);
}

}  // namespace

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    QTemporaryDir tmp;
    if (!tmp.isValid()) {
        std::printf("не завёлся временный каталог\n");
        return 2;
    }
    g_dir = tmp.path();

    checkLoad();
    checkToCode();
    checkFromCode();
    checkRoundTrip();

    return zt::report("пробелы");
}
