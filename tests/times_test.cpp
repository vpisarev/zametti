// Времена шапки: ISO-8601 с офсетом, чтение обоих видов, сравнение по моментам.
//
// Главная беда, ради которой набор и написан: строки с офсетами НЕЛЬЗЯ
// сравнивать побайтово. «2026-08-14T21:40:00+02:00» лексикографически больше
// «2026-08-14T19:40:00Z», а это один и тот же момент — сортировка «по дате
// создания» рассыпалась бы ровно на хранилище, пожившем в двух зонах.
//
// Отсюда три части: круг «время → строка → время», порядок по моментам вместо
// строк и живая фикстура «Китай → Москва» на дереве заметок.

#include "note_tree.h"
#include "times.h"

#include "parser.h"
#include "serializer.h"
#include "test_util.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QTimeZone>

#include <string>

using zametti::NoteTreeModel;
using zametti::SortKey;
using zametti::SortOrder;
using zametti::store::comparableTime;
using zametti::store::isoWithOffset;
using zametti::store::parseNoteTime;

namespace {

QString g_root;

std::string s(const QString& q) { return q.toStdString(); }

void note(const QString& id, const QString& meta, const QString& body) {
    QFile f(g_root + QLatin1Char('/') + id + QStringLiteral(".md"));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return;
    QString text = QStringLiteral("<!-- zametti\n") + meta + QStringLiteral("-->\n");
    if (!body.isEmpty()) text += QStringLiteral("\n") + body;
    f.write(text.toUtf8());
}

// --- 1. круг --------------------------------------------------------------

void checkRoundTrip() {
    const QTimeZone shanghai("Asia/Shanghai");
    const QTimeZone moscow("Europe/Moscow");
    const QTimeZone kolkata("Asia/Kolkata");   // +05:30 — офсет не целый час
    ZT_TRUE("зоны из системной базы доступны",
            shanghai.isValid() && moscow.isValid() && kolkata.isValid());

    const QDateTime instant = QDateTime::fromSecsSinceEpoch(1786736400, QTimeZone::utc());
    for (const QTimeZone& zone : {shanghai, moscow, kolkata, QTimeZone::utc()}) {
        const QDateTime local = instant.toTimeZone(zone);
        const QString text = isoWithOffset(local);
        ZT_TRUE("строка непуста в зоне " + std::string(zone.id().constData()),
                !text.isEmpty());
        const QDateTime back = parseNoteTime(text);
        ZT_TRUE("круг время→строка→время в " + std::string(zone.id().constData()) + ": " +
                    s(text),
                back.isValid() && back.toSecsSinceEpoch() == instant.toSecsSinceEpoch());
        // Момент один, а строки разные — в этом весь смысл нового вида: он
        // помнит не только «когда», но и «сколько было на часах у человека».
        ZT_TRUE("местный час записан в строку: " + s(text),
                text.startsWith(local.toString(QStringLiteral("yyyy-MM-ddTHH:mm:ss"))));
    }

    // Доли секунды в шапку не попадают: иначе один и тот же момент давал бы
    // разные строки от сохранения к сохранению.
    QDateTime withMsec = instant.toTimeZone(moscow);
    withMsec = withMsec.addMSecs(456);
    ZT_TRUE("миллисекунды отброшены",
            !isoWithOffset(withMsec).contains(QLatin1Char('.')));

    // Читаются все три вида. Метка без зоны — законная (чужой файл), и по
    // уговору она читается в зоне читающей машины.
    ZT_TRUE("старая метка с Z читается", parseNoteTime(QStringLiteral(
                                             "2026-08-14T19:40:00Z")).isValid());
    ZT_TRUE("новая метка с офсетом читается",
            parseNoteTime(QStringLiteral("2026-08-14T21:40:00+02:00")).isValid());
    const QDateTime bare = parseNoteTime(QStringLiteral("2026-08-14T21:40:00"));
    ZT_TRUE("метка без зоны читается", bare.isValid());
    ZT_TRUE("и считается местной", bare.timeSpec() == Qt::LocalTime);
    ZT_TRUE("мусор не читается", !parseNoteTime(QStringLiteral("позавчера")).isValid());
    ZT_TRUE("пустая строка не читается", !parseNoteTime(QString()).isValid());
    ZT_TRUE("у мусора нет сравнимой формы",
            comparableTime(QStringLiteral("позавчера")).isEmpty());
}

// --- 2. порядок по моментам, а не по строкам ------------------------------

void checkOrderIsByInstants() {
    // Один и тот же момент в двух зонах: строки разные, сравнимые формы равны.
    const QString east = QStringLiteral("2026-08-14T21:40:00+08:00");
    const QString west = QStringLiteral("2026-08-14T13:40:00Z");
    ZT_TRUE("строки различны", east != west);
    ZT_EQ("сравнимые формы совпадают", s(comparableTime(east)), s(comparableTime(west)));

    // РАНЬШЕ ПО МОМЕНТУ, НО БОЛЬШЕ ПО СТРОКЕ — та самая ловушка.
    const QString earlier = QStringLiteral("2026-08-14T21:40:00+08:00");   // 13:40 UTC
    const QString later = QStringLiteral("2026-08-14T19:40:00Z");
    ZT_TRUE("по строкам порядок ЛОЖНЫЙ", earlier > later);
    ZT_TRUE("по сравнимым формам порядок верный",
            comparableTime(earlier) < comparableTime(later));
}

// --- 3. живая фикстура: писали в Китае, читаем в Москве --------------------

void checkStoreSortsAcrossZones() {
    QDir(g_root).removeRecursively();
    QDir().mkpath(g_root + QStringLiteral("/.zametti"));

    note("00000000000c01", "created: 2026-01-01T00:00:00Z\nmodified: 2026-01-01T00:00:00Z\n",
         "# Поездка\n");
    // Три записи одного дня, сделанные в разных зонах и в РАЗНОМ виде: первая
    // старым UTC, вторая и третья новым — с китайским и московским офсетом.
    // По моментам порядок: утро (05:00Z) → день (11:00Z) → вечер (17:00Z).
    note("00000000000c02",
         "parent: 00000000000c01\ncreated: 2026-08-14T05:00:00Z\n"
         "modified: 2026-08-14T05:00:00Z\n",
         "# Утро (старая метка UTC)\n");
    note("00000000000c03",
         "parent: 00000000000c01\ncreated: 2026-08-14T19:00:00+08:00\n"
         "modified: 2026-08-14T19:00:00+08:00\n",
         "# День (китайский офсет)\n");
    note("00000000000c04",
         "parent: 00000000000c01\ncreated: 2026-08-14T20:00:00+03:00\n"
         "modified: 2026-08-14T20:00:00+03:00\n",
         "# Вечер (московский офсет)\n");

    NoteTreeModel model(g_root);
    const QString folder = QStringLiteral("00000000000c01");
    const auto order = [&model, &folder] {
        QStringList out;
        for (const zametti::NoteRow& row :
             model.notesInSubtree(model.indexForPath(model.pathOfId(folder))))
            out << row.title.left(5).trimmed();
        return out.join(QLatin1Char('|'));
    };

    model.setSortOrder(SortOrder{SortKey::Created, true});
    ZT_EQ("по созданию: утро → день → вечер, невзирая на зоны и виды меток",
          std::string("Утро|День|Вечер"), s(order()));
    model.setSortOrder(SortOrder{SortKey::Modified, false});
    ZT_EQ("по правке в обратную сторону — тот же порядок наоборот",
          std::string("Вечер|День|Утро"), s(order()));

    // ПОРЯДОК НЕ ЗАВИСИТ ОТ ЗОНЫ МАШИНЫ (инвариант D брифа). Строки в файлах
    // не трогаем — меняем зону читателя и строим дерево заново.
    const QByteArray hadTz = qgetenv("TZ");
    for (const char* zone : {"Asia/Shanghai", "Europe/Moscow", "America/Los_Angeles", "UTC"}) {
        qputenv("TZ", zone);
        NoteTreeModel elsewhere(g_root);
        elsewhere.setSortOrder(SortOrder{SortKey::Created, true});
        QStringList out;
        for (const zametti::NoteRow& row :
             elsewhere.notesInSubtree(elsewhere.indexForPath(elsewhere.pathOfId(folder))))
            out << row.title.left(5).trimmed();
        ZT_EQ(std::string("порядок в зоне ") + zone, std::string("Утро|День|Вечер"),
              s(out.join(QLatin1Char('|'))));
    }
    if (hadTz.isEmpty()) qunsetenv("TZ");
    else qputenv("TZ", hadTz);

    QDir(g_root).removeRecursively();
}

// --- 4. новая метка проходит через ядро -----------------------------------
//
// Времена лежат в шапке — HTML-комментарии, которые разбирает наше ядро, а
// вокруг них работает md4c. Круг «разобрать → записать» обязан быть побайтовым
// и для нового вида метки: офсет это «+», «:» и знак, которых в старых метках
// не было вовсе.
void checkCoreKeepsOffsets() {
    const std::string source =
        "<!-- zametti\n"
        "parent: 01n6cqevr3wprw\n"
        "created: 2026-08-14T21:40:00+08:00\n"
        "modified: 2026-08-14T20:00:00+03:00\n"
        "-->\n\n# Заметка\n\nТекст.\n";
    const zametti::Document doc = zametti::parse(source);
    ZT_EQ("круг разбор→запись побайтовый", source, zametti::serialize(doc));
    ZT_EQ("created прочитан как есть", "2026-08-14T21:40:00+08:00", doc.meta.get("created"));
    ZT_EQ("modified прочитан как есть", "2026-08-14T20:00:00+03:00", doc.meta.get("modified"));

    // И правка одной метки не трогает соседнюю: у ядра это обещание формата, а
    // офсет в значении для него — обычные знаки.
    zametti::Document edited = doc;
    edited.meta.set("modified", "2026-12-31T23:59:00+01:00");
    const std::string out = zametti::serialize(edited);
    ZT_TRUE("новая метка записалась",
            out.find("modified: 2026-12-31T23:59:00+01:00") != std::string::npos);
    ZT_TRUE("соседняя цела", out.find("created: 2026-08-14T21:40:00+08:00") != std::string::npos);
    ZT_EQ("и круг всё ещё побайтовый", out, zametti::serialize(zametti::parse(out)));
}

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    g_root = QDir::tempPath() + QStringLiteral("/zametti-times-test");

    checkRoundTrip();
    checkOrderIsByInstants();
    checkStoreSortsAcrossZones();
    checkCoreKeepsOffsets();

    return zt::report("времена");
}
