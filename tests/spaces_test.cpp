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
#include "pieces.h"
#include "document_saver.h"
#include "store.h"
#include "editor_ops.h"
#include "test_util.h"

#include <vector>
#include "testdata.h"

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
    zametti::ZNote::canonicaliseFile(path, text, digest);
    return text;
}

// Абзац → код и обратно, через настоящую операцию.
std::string toggle(const std::string& source) {
    zametti::ZNote note = noteOf(source);
    QTextCursor caret = note.doc().caretAtBlock(0);
    caret.movePosition(QTextCursor::End, QTextCursor::KeepAnchor);

    if (!note.doc().toggleCodeBlock(caret)) return "ОПЕРАЦИЯ НЕ СРАБОТАЛА";
    return note.toMarkdown();
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

// ВВОЗ чужого .md. Прежде нормализация жила только в открытии заметки, и
// привезённый файл лежал на диске грязным до первого открытия. Владелец
// спросил про это прямо — и оказался прав.
void checkImport() {
    const QString from = QDir(g_dir).filePath(QStringLiteral("чужая.md"));
    const std::string source =
        "# Чужая\n\nдва" + kNbsp + "слова и" + kNbsp + kNbsp + "столбик\n";
    QFile file(from);
    if (file.open(QIODevice::WriteOnly)) file.write(source.data(), qint64(source.size()));
    file.close();

    const QString store = QDir(g_dir).filePath(QStringLiteral("хранилище"));
    QDir().mkpath(store + QStringLiteral("/.zametti"));
    QString error;
    const QString made = zametti::store::importNote(store, QString(), from, &error);
    ZT_TRUE("заметка ввезена: " + error.toStdString(), !made.isEmpty());
    if (made.isEmpty()) return;

    QFile got(made);
    ZT_TRUE("файл ввезённой заметки читается", got.open(QIODevice::ReadOnly));
    const std::string text = got.readAll().toStdString();
    ZT_TRUE("одиночный неразрывный вычищен при ВВОЗЕ: " + visible(text),
            text.find("два слова") != std::string::npos);
    ZT_TRUE("а серия — цела: " + visible(text),
            text.find("и" + kNbsp + kNbsp + "столбик") != std::string::npos);
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

// ВЕДУЩИЕ ОБЫЧНЫЕ ПРОБЕЛЫ СТРОК АБЗАЦА СОХРАНЯЮТСЯ неразрывными (решение
// владельца, сессия 9: стихотворение с отступами, псевдографика). Структурный
// отступ — колонка содержимого пункта — не в счёт; разметка по дороге (`>`,
// `#`) — отступа нет; в коде пробел значим сам и остаётся обычным.
void checkLeadingSpacesKept() {
    const std::string head = "<!-- zametti\ncreated: 2020-01-01T00:00:00Z\n-->\n\n";
    const std::string n3 = kNbsp + kNbsp + kNbsp;
    ZT_EQ("ведущие пробелы абзаца — неразрывными, на каждой строке",
          visible(head + n3 + "стих\n" + n3 + "второй\n"),
          visible(afterLoad(head + "   стих\n   второй\n")));
    ZT_EQ("внутри пункта — сверх колонки содержимого",
          visible(head + "- пункт\n  " + kNbsp + kNbsp + "стих\n"),
          visible(afterLoad(head + "- пункт\n    стих\n")));
    ZT_EQ("обычное продолжение пункта отступа не получает",
          visible(head + "- пункт\n  продолжение\n"),
          visible(afterLoad(head + "- пункт\n  продолжение\n")));
    ZT_EQ("у задачи чекбокс не в счёт: колонка содержимого 2",
          visible(head + "- [ ] дело\n  " + kNbsp + "хвост\n"),
          visible(afterLoad(head + "- [ ] дело\n   хвост\n")));
    ZT_EQ("пробелы за маркером (до четырёх) — маркера, не автора: канон один пробел",
          visible(head + "1. номер\n"),
          visible(afterLoad(head + "1.   номер\n")));
    ZT_EQ("цитата: за `>` отступа нет",
          visible(head + "> цитата\n> с отступом\n"),
          visible(afterLoad(head + "> цитата\n>   с отступом\n")));
    ZT_EQ("в коде пробелы обычные",
          visible(head + "```\n    код\n```\n"),
          visible(afterLoad(head + "```\n    код\n```\n")));
}

// В блоке кода неразрывных быть не должно ни одного — даже ведущих. Владелец
// нашёл это на «type exp_t = ...»: отступы внутри забора так и остались
// заполнены неразрывными.
void checkCodeFenceOnLoad() {
    const std::string head = "<!-- zametti\ncreated: 2020-01-01T00:00:00Z\n-->\n\n";
    const std::string source = head + "```\n" + kNbsp + kNbsp + "type exp_t =\n" + kNbsp +
                               "| Nil\n```\n\n" + kNbsp + kNbsp + "а тут отступ живёт\n";
    const std::string got = afterLoad(source);

    const size_t fence = got.find("```");
    const size_t close = got.find("```", fence + 3);
    ZT_TRUE("заборы на месте", fence != std::string::npos && close != std::string::npos);
    if (fence == std::string::npos || close == std::string::npos) return;

    ZT_TRUE("внутри забора неразрывных не осталось: " + visible(got.substr(fence, close - fence)),
            got.substr(fence, close - fence).find(kNbsp) == std::string::npos);
    ZT_TRUE("а отступ ВНЕ забора цел: " + visible(got),
            got.find(kNbsp + kNbsp + "а тут отступ живёт") != std::string::npos);
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

static int ztRunSuite(int argc, char** argv) {
    (void)argc;
    (void)argv;
    QTemporaryDir tmp;
    if (!tmp.isValid()) {
        std::printf("не завёлся временный каталог\n");
        return 2;
    }
    g_dir = tmp.path();

    checkImport();
    checkLoad();
    checkLeadingSpacesKept();
    checkCodeFenceOnLoad();
    checkToCode();
    checkFromCode();
    checkRoundTrip();

    return zt::report("пробелы");
}

// Набор целиком одним TEST: тело не тронуто, argv ему собран здесь.
// Дробить на отдельные проверки — отдельная работа, по одному набору.
TEST(Spaces, All) {
    std::vector<QByteArray> ztArgs{QByteArrayLiteral("spaces_test")};
    std::vector<char*> ztArgv;
    for (QByteArray& a : ztArgs) ztArgv.push_back(a.data());
    EXPECT_EQ(0, ztRunSuite(int(ztArgv.size()), ztArgv.data()));
}
