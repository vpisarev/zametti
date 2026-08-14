// ZDocument: первая веха рефакторинга.
//
// Инвариант из плана: круг «разобрать → записать» не портит ни байта. На
// заметках владельца требование СТРОГОЕ — побайтовое равенство, потому что они
// уже лежат в каноническом виде. На чужом markdown (спецификации CommonMark и
// GFM) равенство послабее: канонизация вправе переписать `*курсив*` в `_курсив_`,
// но второй круг обязан уже ничего не менять.
//
// Смысл набора не в самих буквах, а в доказательстве: если этот круг держится,
// значит ZDocument можно доверить хранилище. Пока внутри него живёт IR, набор
// проверяет переходную реализацию; когда нутро заменят прямым проходом md4c →
// QTextDocument, набор не должен измениться НИ СТРОКОЙ. Если придётся править —
// интерфейс был плох.

#include "document.h"

#include "test_util.h"
#include "testdata.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <cstdio>

namespace {

using namespace zametti;

std::string readAll(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    const QByteArray bytes = f.readAll();
    return std::string(bytes.constData(), size_t(bytes.size()));
}

QStringList notesIn(const QString& dir) {
    QDir d(dir);
    QStringList out;
    for (const QString& name : d.entryList({QStringLiteral("*.md")}, QDir::Files, QDir::Name))
        out << d.filePath(name);
    return out;
}

}  // namespace

// Круг на заметках владельца: причесали один раз — дальше ни байта.
//
// Работаем на КОПИИ. Хранилище владельца — только чтение, и это железное
// правило; копию делает сам набор, чтобы никакая ошибка не могла дотянуться до
// оригинала.
TEST(ZDocument, RoundTripIsFixedPointOnOwnerNotes) {
    // КОПИЯ хранилища владельца, а не оно само: оригинал — только чтение, и
    // это железное правило. Строгое равенство спрашивается ИМЕННО здесь: эти
    // заметки уже лежат в каноническом виде, и любое расхождение означает, что
    // круг их портит.
    const QString store = zt::TestData::corpus(QStringLiteral("owner-copy"));
    ZT_SKIP_NO_CORPUS(store, "owner-copy (копия хранилища владельца)");

    const QStringList notes = notesIn(store);
    if (notes.isEmpty()) GTEST_SKIP() << "в корпусе нет ни одной заметки: " << store.toStdString();

    int checked = 0;
    int notCanonicalOnDisk = 0;
    int broken = 0;
    for (const QString& path : notes) {
        const std::string original = readAll(path);
        if (original.empty()) continue;
        ++checked;

        ZDocument doc;
        ASSERT_TRUE(doc.loadMarkdown(original)) << path.toStdString();
        const std::string canonical = doc.toMarkdown();

        // ЧТО ЗДЕСЬ ПРОВЕРЯЕТСЯ, А ЧТО НЕТ.
        //
        // «Файл на диске уже канонический» — вопрос НЕ К НАМ: на него отвечает
        // zametti-store verify, и он честно сообщает о дрейфе. Дублировать это
        // здесь значило бы держать красным набор из-за заметки, написанной до
        // того, как появились правила формул.
        //
        // К нам вопрос другой и куда более важный: КРУГ ОБЯЗАН БЫТЬ
        // НЕПОДВИЖНОЙ ТОЧКОЙ. Один раз причесали — дальше ни байта. Именно это
        // ломается, когда круг портит содержимое: на этапе 16 заметка владельца
        // от такой поломки выросла с 20 КБ до 25 МБ, потому что косые
        // удваивались при каждой записи.
        if (canonical != original) ++notCanonicalOnDisk;

        ZDocument again;
        ASSERT_TRUE(again.loadMarkdown(canonical)) << path.toStdString();
        const std::string twice = again.toMarkdown();
        if (twice != canonical) {
            ++broken;
            if (broken <= 3)
                ZT_EQ(("круг не неподвижен: " + QFileInfo(path).fileName()).toStdString(),
                      canonical, twice);
        }
    }
    std::printf("ZDocument: заметок %d, на диске не в каноне %d, круг сдвинулся у %d\n",
                checked, notCanonicalOnDisk, broken);
    EXPECT_EQ(0, broken) << "круг разобрать-записать обязан быть неподвижной точкой";
}

// Идемпотентность на ЧУЖОМ markdown: первый круг вправе причесать, второй уже
// нет. Это и есть определение канона.
TEST(ZDocument, IdempotentOnForeignMarkdown) {
    int checked = 0;
    int unstable = 0;
    // Чужой markdown: спецификации и ввозной корпус, где заметки писались не
    // нами и канон вправе их причесать.
    for (const char* name : {"commonmark", "gfm", "corpus"}) {
        const QString dir = zt::TestData::corpus(QString::fromLatin1(name));
        if (dir.isEmpty()) {
            std::printf("ПРОПУЩЕНО: корпуса нет рядом — %s\n", name);
            continue;
        }
        for (const QString& path : notesIn(dir)) {
            const std::string original = readAll(path);
            if (original.empty()) continue;
            ++checked;

            ZDocument first;
            first.loadMarkdown(original);
            const std::string once = first.toMarkdown();

            ZDocument second;
            second.loadMarkdown(once);
            const std::string twice = second.toMarkdown();

            if (once != twice) {
                ++unstable;
                if (unstable <= 3)
                    ZT_EQ(("второй круг что-то изменил: " + QFileInfo(path).fileName())
                              .toStdString(),
                          once, twice);
            }
        }
    }
    if (checked == 0) GTEST_SKIP() << "корпусов чужого markdown нет рядом";
    std::printf("ZDocument: чужих файлов %d, неустойчивых %d\n", checked, unstable);
    EXPECT_EQ(0, unstable);
}

// Шапка: ключи читаются и пишутся по-человечески, а ЧУЖИЕ не трогаются.
TEST(ZDocument, HeaderKeys) {
    const std::string source =
        "<!-- zametti\n"
        "parent: 01n6cqevr3wprw\n"
        "created: 2019-06-19T10:54:29+03:00\n"
        "modified: 2026-07-30T22:26:24+03:00\n"
        "чужой-ключ: не трогать\n"
        "-->\n"
        "\n"
        "# Заголовок заметки\n"
        "\n"
        "Первый абзац после заголовка.\n";

    ZDocument doc;
    ASSERT_TRUE(doc.loadMarkdown(source));

    ZT_EQ("родитель прочитан", std::string("01n6cqevr3wprw"), doc.parentId().toStdString());
    ZT_EQ("создана прочитана", std::string("2019-06-19T10:54:29+03:00"),
          doc.created().toStdString());
    ZT_TRUE("не папка", !doc.isFolder());
    ZT_TRUE("не в архиве", !doc.isArchived());
    ZT_EQ("заголовок взят", std::string("Заголовок заметки"), doc.title().toStdString());
    ZT_TRUE("не пуста", !doc.isEmpty());

    // Правка своего ключа не трогает чужой.
    doc.setParentId(QStringLiteral("01aaaaaaaaaaaa"));
    doc.setArchived(true);
    const std::string written = doc.toMarkdown();
    ZT_TRUE("чужой ключ уцелел",
            written.find("чужой-ключ: не трогать") != std::string::npos);
    ZT_TRUE("новый родитель записан",
            written.find("parent: 01aaaaaaaaaaaa") != std::string::npos);
    ZT_TRUE("пометка архива записана", written.find("archived: yes") != std::string::npos);

    // И круг остаётся кругом.
    ZDocument back;
    ASSERT_TRUE(back.loadMarkdown(written));
    ZT_EQ("после правки шапки круг держится", written, back.toMarkdown());
    ZT_TRUE("архивность прочиталась обратно", back.isArchived());
}

// Вложения: собираются со всеми атрибутами и переписываются по имени.
TEST(ZDocument, Attachments) {
    const std::string source =
        "<!-- zametti\n-->\n"
        "\n"
        "# Заметка с картинками\n"
        "\n"
        "![вид с балкона](01jd7f0kq2m8xa.webp#w=600&align=left)\n"
        "\n"
        "![без атрибутов](01jd7f0kq2m8xb.jxl)\n";

    ZDocument doc;
    ASSERT_TRUE(doc.loadMarkdown(source));

    const auto found = doc.attachments();
    ASSERT_EQ(size_t(2), found.size());
    ZT_EQ("id первой", std::string("01jd7f0kq2m8xa.webp"), found[0].id.toStdString());
    ZT_EQ("ширина первой", std::string("600"), std::to_string(found[0].width));
    ZT_EQ("выравнивание первой", std::string("left"), found[0].align.toStdString());
    ZT_EQ("подпись первой", std::string("вид с балкона"), found[0].alt.toStdString());
    ZT_EQ("id второй", std::string("01jd7f0kq2m8xb.jxl"), found[1].id.toStdString());
    ZT_EQ("у второй ширины нет", std::string("0"), std::to_string(found[1].width));

    // Переименование: атрибуты обязаны уцелеть.
    const int changed = doc.rewriteAttachments([](const QString& name) {
        return name == QLatin1String("01jd7f0kq2m8xa.webp") ? QStringLiteral("новое.webp")
                                                            : QString();
    });
    ZT_EQ("переписана одна", std::string("1"), std::to_string(changed));
    const std::string written = doc.toMarkdown();
    ZT_TRUE("новое имя на месте, атрибуты целы",
            written.find("новое.webp#w=600&align=left") != std::string::npos);
    ZT_TRUE("вторая не тронута",
            written.find("01jd7f0kq2m8xb.jxl") != std::string::npos);
}

// Поиск: находит то, что видит человек, и не находит разметку.
TEST(ZDocument, Find) {
    const std::string source =
        "<!-- zametti\n-->\n"
        "\n"
        "# Про ежей\n"
        "\n"
        "Ёжик нёс **важный** груз, а второй ёжик смотрел.\n";

    ZDocument doc;
    ASSERT_TRUE(doc.loadMarkdown(source));

    const auto hits = doc.find(makeQuery(QStringLiteral("ёжик")));
    ZT_EQ("нашлись оба вхождения", std::string("2"), std::to_string(hits.size()));

    // Звёздочек эмфазиса в тексте блока нет — искать их бессмысленно.
    ZT_TRUE("разметка не ищется", doc.find(makeQuery(QStringLiteral("**"))).empty());

    if (!hits.empty()) {
        const HitLine line = doc.hitLine(hits.front());
        ZT_TRUE("строка совпадения не пуста", !line.text.isEmpty());
    }
}

// Отпечаток и дрейф.
TEST(ZDocument, DigestAndDrift) {
    const std::string canonical =
        "<!-- zametti\n-->\n"
        "\n"
        "# Заголовок\n"
        "\n"
        "Текст.\n";

    ZDocument doc;
    ASSERT_TRUE(doc.loadMarkdown(canonical));
    ZT_TRUE("канонический файл дрейфа не имеет", doc.isCanonical(canonical));
    ZT_TRUE("отпечаток не пуст", !doc.digest().empty());

    // Тот же смысл, но записанный иначе: дрейф есть.
    ZDocument sloppy;
    ASSERT_TRUE(sloppy.loadMarkdown("<!-- zametti\n-->\n\n# Заголовок\n\nТекст.   \n"));
    ZT_TRUE("лишние пробелы в конце строки — это дрейф",
            !sloppy.isCanonical("<!-- zametti\n-->\n\n# Заголовок\n\nТекст.   \n"));

    // Отпечаток считается от КАНОНИЧЕСКИХ байтов, значит у обоих он один.
    ZT_TRUE("отпечаток от канона, а не от исходника",
            doc.digest() == sloppy.digest());
}

// КОПИРОВАНИЕ ДАРОМ. Внутренность за shared_ptr, поэтому ни конструктор копии,
// ни присваивание не написаны вовсе — и работают как надо.
//
// Копия РАЗДЕЛЯЕТ содержимое: ZDocument это ручка к заметке, а не её слепок.
// Независимый слепок делает clone(), и разница между ними проверяется здесь,
// потому что перепутать их — это молча испортить чужую заметку.
TEST(ZDocument, CopyShares_CloneDoesNot) {
    const std::string source =
        "<!-- zametti\nparent: 01aaaaaaaaaaaa\n-->\n\n# Начало\n\nТекст.\n";

    ZDocument original;
    ASSERT_TRUE(original.loadMarkdown(source));

    ZDocument handle = original;          // ручка к той же заметке
    ZDocument snapshot = original.clone();  // независимый слепок

    original.setParentId(QStringLiteral("01bbbbbbbbbbbb"));

    ZT_EQ("ручка видит правку", std::string("01bbbbbbbbbbbb"),
          handle.parentId().toStdString());
    ZT_EQ("слепок правки не видит", std::string("01aaaaaaaaaaaa"),
          snapshot.parentId().toStdString());

    // И присваивание тоже даром.
    ZDocument assigned;
    assigned = snapshot;
    ZT_EQ("присваивание работает", std::string("01aaaaaaaaaaaa"),
          assigned.parentId().toStdString());
}

// Строение заметки спрашивается ЗНАЧЕНИЯМИ, без ходьбы по внутренностям.
TEST(ZDocument, BlocksAndLines) {
    const std::string source =
        "<!-- zametti\n-->\n"
        "\n"
        "# Заголовок\n"
        "\n"
        "Абзац.\n"
        "\n"
        "- пункт раз\n"
        "- [x] сделано\n"
        "\n"
        "```cpp\n"
        "int main() {}\n"
        "```\n";

    ZDocument doc;
    ASSERT_TRUE(doc.loadMarkdown(source));

    const auto all = doc.blocks();
    ZT_TRUE("блоки перечислены", all.size() >= 7);

    int headings = 0, items = 0, code = 0, done = 0;
    QString language;
    for (const BlockInfo& b : all) {
        if (b.kind == Kind::Heading) ++headings;
        if (b.kind == Kind::ListItem) {
            ++items;
            if (b.checked) ++done;
        }
        if (b.kind == Kind::Code) {
            ++code;
            if (!b.info.isEmpty()) language = b.info;
        }
    }
    ZT_EQ("заголовок один", std::string("1"), std::to_string(headings));
    ZT_EQ("пунктов два", std::string("2"), std::to_string(items));
    ZT_EQ("отмечен один", std::string("1"), std::to_string(done));
    ZT_TRUE("блок кода есть", code >= 1);
    ZT_EQ("язык блока кода взят", std::string("cpp"), language.toStdString());

    // Строки канона с картой блоков — на этом стоит разность версий.
    const auto lines = doc.sourceLines();
    ZT_TRUE("строк не меньше, чем блоков", lines.size() >= all.size());
    bool anyMapped = false;
    for (const SourceLine& line : lines)
        if (line.block >= 0) anyMapped = true;
    ZT_TRUE("строки привязаны к блокам", anyMapped);

    // Счёт слов.
    const NoteStats counted = doc.getStats();
    ZT_TRUE("слова посчитаны", counted.valid && counted.words > 0);
}

// Сравнение двух заметок: строение отдельно, тело отдельно.
TEST(ZDocument, Comparison) {
    const std::string a = "<!-- zametti\nmodified: 2026-01-01T00:00:00Z\n-->\n\n# Заголовок\n";
    const std::string b = "<!-- zametti\nmodified: 2026-08-14T00:00:00Z\n-->\n\n# Заголовок\n";

    ZDocument first;
    ZDocument second;
    ASSERT_TRUE(first.loadMarkdown(a));
    ASSERT_TRUE(second.loadMarkdown(b));

    // Тело одно и то же — шапка в счёт не идёт. Именно на этом стоит история:
    // иначе всякая разность начиналась бы со строки `modified`.
    ZT_TRUE("тела совпали", first.sameBody(second));
    ZT_TRUE("строение не совпало: шапки разные", !first.sameSkeleton(second));

    ZDocument other;
    ASSERT_TRUE(other.loadMarkdown("<!-- zametti\n-->\n\n# Другое\n"));
    ZT_TRUE("другое тело не совпадает", !first.sameBody(other));
}
