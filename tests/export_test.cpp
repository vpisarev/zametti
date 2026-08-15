// Вывоз заметки наружу: имя из заголовка и картинки рядом.
//
// Матрица здесь не про код, а про СОСТОЯНИЯ КАТАЛОГА, куда вывозят. Их четыре,
// и они дают разные ответы:
//
//   пусто                     — картинка ложится под своим именем;
//   лежит ТА ЖЕ картинка      — не копируем ничего, имя остаётся;
//   лежит ЧУЖОЙ файл с тем же именем — заводим соседнее имя и правим ссылку;
//   вывоз в само хранилище    — отказ.
//
// Третий случай — единственный, ради которого заметка пересобирается из IR, и
// единственный, где чужой файл мог бы погибнуть. Он же и самый редкий: имя
// вложения — свежий id, столкнуться ему почти не с чем. Поэтому проверяется он
// тут, а не «когда-нибудь на живом».
//
// Имя файла проверяется на знаках, которые запрещены НЕ У НАС: экспорт для того
// и делают, чтобы файл уехал на чужую машину.

#include "export_note.h"
#include "test_util.h"

#include <vector>
#include "testdata.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <string>

namespace {

const char* kNote =
    "<!-- zametti\ncreated: 2020-01-01T00:00:00Z\n-->\n\n"
    "# Заметка с картинкой\n\n"
    "Текст.\n\n"
    "![снимок](01n6r08s8wy52h.jxl)\n\n"
    "И ещё раз та же: ![снимок](01n6r08s8wy52h.jxl)\n\n"
    "А это чужое: ![сеть](https://example.org/a.png)\n";

void put(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    if (file.open(QIODevice::WriteOnly)) file.write(bytes);
}

QByteArray get(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return file.readAll();
}

// Хранилище с одной заметкой и одним вложением; возвращает путь к заметке.
QString makeStore(const QDir& root, const QByteArray& picture) {
    QDir().mkpath(root.filePath(QStringLiteral("хранилище")));
    const QDir store(root.filePath(QStringLiteral("хранилище")));
    put(store.filePath(QStringLiteral("01n6r08s8wy52h.jxl")), picture);
    const QString note = store.filePath(QStringLiteral("01aaaaaaaaaaaa.md"));
    put(note, QByteArray(kNote));
    return note;
}

void checkNames() {
    ZT_EQ("запрещённые знаки заменяются", std::string("Отчёт-2026-план"),
          zametti::fileNameFromTitle(QStringLiteral("Отчёт/2026:план")).toStdString());
    ZT_EQ("точка на конце срезана (Windows её съест молча)", std::string("Итог"),
          zametti::fileNameFromTitle(QStringLiteral("Итог...")).toStdString());
    ZT_EQ("пустой заголовок получает имя", std::string("Без названия"),
          zametti::fileNameFromTitle(QStringLiteral("   ")).toStdString());
    ZT_EQ("имя устройства Windows обезврежено", std::string("_CON"),
          zametti::fileNameFromTitle(QStringLiteral("CON")).toStdString());
    // Предел в БАЙТАХ: кириллическая буква весит два, и 200 знаков дали бы 400
    // байт — больше, чем принимает компонент пути.
    const QString longTitle(300, QChar(0x0416));   // «Ж»
    ZT_TRUE("длинное имя урезано по байтам",
            zametti::fileNameFromTitle(longTitle).toUtf8().size() <= 200);
}

// Пустой каталог: всё ложится как есть, ссылка не трогается.
void checkFreshDir(const QDir& root) {
    const QString note = makeStore(QDir(root.filePath(QStringLiteral("a"))),
                                  QByteArray("КАРТИНКА-1"));
    const QDir out(root.filePath(QStringLiteral("a/вывоз")));
    QDir().mkpath(out.path());

    const QString target = out.filePath(QStringLiteral("Заметка с картинкой.md"));
    const zametti::ExportReport report = zametti::exportMarkdown(note, target);
    ZT_TRUE("вывоз удался: " + report.error.toStdString(), report.ok());
    ZT_TRUE("одна картинка скопирована, а не " + std::to_string(report.imagesCopied),
            report.imagesCopied == 1);
    ZT_TRUE("пропаж нет, а насчитано " + std::to_string(report.imagesMissing),
            report.imagesMissing == 0);
    // ПО УМОЛЧАНИЮ ШАПКА СРЕЗАНА (галочка выключена, этап 15): наружу уезжает
    // чистый markdown — его отдают тому, кто про zametti не знает вовсе.
    const std::string exported = get(target).toStdString();
    ZT_TRUE("шапки в вывезенном нет", exported.find("<!-- zametti") == std::string::npos);
    ZT_TRUE("а тело на месте", exported.find("# Заметка с картинкой") != std::string::npos);
    ZT_EQ("вывезенное — это хвост исходника после шапки",
          std::string(kNote).substr(std::string(kNote).find("# Заметка")), exported);

    // С ГАЛОЧКОЙ — как есть, байт в байт: такой файл кладут в другое хранилище
    // zametti, и шапка в нём и есть весь смысл.
    const QString asIs = out.filePath(QStringLiteral("как-есть.md"));
    const zametti::ExportReport kept = zametti::exportMarkdown(note, asIs, true);
    ZT_TRUE("вывоз «как есть» удался: " + kept.error.toStdString(), kept.ok());
    ZT_EQ("markdown байт в байт как в хранилище", std::string(kNote),
          get(asIs).toStdString());
    ZT_EQ("картинка легла рядом под своим именем", std::string("КАРТИНКА-1"),
          get(out.filePath(QStringLiteral("01n6r08s8wy52h.jxl"))).toStdString());
}

// ШИРИНА У КАРТИНКИ. В хранилище владельца ссылки выглядят как
// «01x.jxl#w=600»: имя файла и атрибуты показа в одном адресе. Вывоз обязан
// брать оттуда ИМЯ ФАЙЛА, а не весь адрес целиком.
//
// Проверка заведена задним числом: пока вывоз разбирал адрес сам, он искал на
// диске файл «01n6r08s8wy52h.jxl#w=600», не находил его и МОЛЧА вывозил
// заметку без картинки, засчитав пропажу. Набора на этот случай не было вовсе —
// во всех фикстурах картинки стояли без ширины.
void checkPictureWithWidth(const QDir& root) {
    const QDir store(root.filePath(QStringLiteral("w/хранилище")));
    QDir().mkpath(store.path());
    put(store.filePath(QStringLiteral("01n6r08s8wy52h.jxl")), QByteArray("КАРТИНКА-1"));
    const QString note = store.filePath(QStringLiteral("01aaaaaaaaaaaa.md"));
    put(note, QByteArray("<!-- zametti\ncreated: 2020-01-01T00:00:00Z\n-->\n\n"
                         "# С шириной\n\n![снимок](01n6r08s8wy52h.jxl#w=600)\n"));

    const QDir out(root.filePath(QStringLiteral("w/вывоз")));
    QDir().mkpath(out.path());
    const zametti::ExportReport report =
        zametti::exportMarkdown(note, out.filePath(QStringLiteral("Заметка.md")));

    ZT_TRUE("вывоз удался: " + report.error.toStdString(), report.ok());
    ZT_TRUE("пропаж нет, а насчитано " + std::to_string(report.imagesMissing),
            report.imagesMissing == 0);
    ZT_TRUE("картинка скопирована, а насчитано " + std::to_string(report.imagesCopied),
            report.imagesCopied == 1);
    ZT_EQ("и легла под своим именем, без атрибутов", std::string("КАРТИНКА-1"),
          get(out.filePath(QStringLiteral("01n6r08s8wy52h.jxl"))).toStdString());
    // Ширина остаётся в тексте: она часть заметки, а не имя файла.
    const std::string text = get(out.filePath(QStringLiteral("Заметка.md"))).toStdString();
    ZT_TRUE("ширина в ссылке уцелела: " + text,
            text.find("(01n6r08s8wy52h.jxl#w=600)") != std::string::npos);
}

// Та же картинка уже лежит: не копируем и не переименовываем.
void checkSamePicture(const QDir& root) {
    const QString note = makeStore(QDir(root.filePath(QStringLiteral("b"))),
                                  QByteArray("КАРТИНКА-1"));
    const QDir out(root.filePath(QStringLiteral("b/вывоз")));
    QDir().mkpath(out.path());
    put(out.filePath(QStringLiteral("01n6r08s8wy52h.jxl")), QByteArray("КАРТИНКА-1"));

    const zametti::ExportReport report =
        zametti::exportMarkdown(note, out.filePath(QStringLiteral("Заметка.md")));
    ZT_TRUE("вывоз удался: " + report.error.toStdString(), report.ok());
    ZT_TRUE("копировать было нечего, а скопировано " + std::to_string(report.imagesCopied),
            report.imagesCopied == 0);
    ZT_TRUE("прошлая копия признана своей, а насчитано " + std::to_string(report.imagesReused),
            report.imagesReused == 1);
    ZT_EQ("ссылка не тронута", std::string(kNote).substr(std::string(kNote).find("# Заметка")),
          get(out.filePath(QStringLiteral("Заметка.md"))).toStdString());
}

// Чужой файл с тем же именем: он обязан уцелеть, а ссылка — переехать.
void checkStrangerInTheWay(const QDir& root) {
    const QString note = makeStore(QDir(root.filePath(QStringLiteral("c"))),
                                  QByteArray("КАРТИНКА-1"));
    const QDir out(root.filePath(QStringLiteral("c/вывоз")));
    QDir().mkpath(out.path());
    put(out.filePath(QStringLiteral("01n6r08s8wy52h.jxl")), QByteArray("ЧУЖОЕ"));

    const zametti::ExportReport report =
        zametti::exportMarkdown(note, out.filePath(QStringLiteral("Заметка.md")));
    ZT_TRUE("вывоз удался: " + report.error.toStdString(), report.ok());
    ZT_EQ("чужой файл цел", std::string("ЧУЖОЕ"),
          get(out.filePath(QStringLiteral("01n6r08s8wy52h.jxl"))).toStdString());
    ZT_EQ("наша легла под соседним именем", std::string("КАРТИНКА-1"),
          get(out.filePath(QStringLiteral("01n6r08s8wy52h-1.jxl"))).toStdString());

    const std::string text = get(out.filePath(QStringLiteral("Заметка.md"))).toStdString();
    ZT_TRUE("ссылка переписана: " + text,
            text.find("(01n6r08s8wy52h-1.jxl)") != std::string::npos);
    ZT_TRUE("старого имени в ссылках не осталось: " + text,
            text.find("(01n6r08s8wy52h.jxl)") == std::string::npos);
    // ОБЕ вставки одной картинки обязаны переехать: их две, и вторая живёт
    // внутри строки, а не отдельным блоком.
    size_t moved = 0;
    for (size_t at = text.find("01n6r08s8wy52h-1.jxl"); at != std::string::npos;
         at = text.find("01n6r08s8wy52h-1.jxl", at + 1))
        ++moved;
    ZT_TRUE("переписаны обе вставки, а не " + std::to_string(moved), moved == 2);
    ZT_TRUE("чужая сетевая ссылка не тронута: " + text,
            text.find("https://example.org/a.png") != std::string::npos);
    ZT_TRUE("человеку сказано про переименование", !report.notes.isEmpty());
}

// Вложения нет на диске: вывоз идёт, но молчать об этом нельзя.
void checkMissing(const QDir& root) {
    const QString note = makeStore(QDir(root.filePath(QStringLiteral("d"))),
                                  QByteArray("КАРТИНКА-1"));
    QFile::remove(QDir(root.filePath(QStringLiteral("d/хранилище")))
                      .filePath(QStringLiteral("01n6r08s8wy52h.jxl")));
    const QDir out(root.filePath(QStringLiteral("d/вывоз")));
    QDir().mkpath(out.path());

    const zametti::ExportReport report =
        zametti::exportMarkdown(note, out.filePath(QStringLiteral("Заметка.md")));
    ZT_TRUE("вывоз всё равно удался: " + report.error.toStdString(), report.ok());
    ZT_TRUE("пропажа сосчитана, а насчитано " + std::to_string(report.imagesMissing),
            report.imagesMissing == 1);
    ZT_TRUE("и названа человеку", !report.notes.isEmpty());
    ZT_TRUE("сам markdown на месте",
            !get(out.filePath(QStringLiteral("Заметка.md"))).isEmpty());
}

// Вывоз в само хранилище: отказ. Заметка легла бы туда под человеческим именем
// и стала бы для программы заметкой-самозванцем.
void checkIntoStore(const QDir& root) {
    const QString note = makeStore(QDir(root.filePath(QStringLiteral("e"))),
                                  QByteArray("КАРТИНКА-1"));
    const QDir store = QFileInfo(note).absoluteDir();
    const zametti::ExportReport report =
        zametti::exportMarkdown(note, store.filePath(QStringLiteral("Заметка.md")));
    ZT_TRUE("вывоз в хранилище отвергнут", !report.ok());
    ZT_TRUE("файла не появилось",
            !QFile::exists(store.filePath(QStringLiteral("Заметка.md"))));
}

}  // namespace

// Полный путь в диалог вывоза, а не одно имя. Именно относительное имя и
// сбрасывало каталог на «Документы» — и при открытии, и при смене формата.
void checkExportTargetPath() {
    ZT_EQ("к имени приписан каталог", std::string("/tmp/куда/Заметка.md"),
          zametti::exportTargetPath(QStringLiteral("/tmp/куда"), QStringLiteral("Заметка"),
                                    false).toStdString());
    ZT_EQ("для бумаги расширение другое", std::string("/tmp/куда/Заметка.pdf"),
          zametti::exportTargetPath(QStringLiteral("/tmp/куда"), QStringLiteral("Заметка"),
                                    true).toStdString());
    ZT_TRUE("путь абсолютный",
            QFileInfo(zametti::exportTargetPath(QStringLiteral("/tmp/куда"),
                                                QStringLiteral("Заметка"), false))
                .isAbsolute());
    // Каталога нет — остаётся одно имя, и диалог решит сам: без этого путь
    // получался бы вида "/Заметка.md", то есть в корне.
    ZT_EQ("без каталога — одно имя", std::string("Заметка.md"),
          zametti::exportTargetPath(QString(), QStringLiteral("Заметка"), false).toStdString());
}

static int ztRunSuite(int argc, char** argv) {
    (void)argc;
    (void)argv;
    QTemporaryDir tmp;
    if (!tmp.isValid()) {
        std::printf("не завёлся временный каталог\n");
        return 2;
    }
    const QDir root(tmp.path());

    checkNames();
    checkExportTargetPath();
    checkFreshDir(root);
    checkPictureWithWidth(root);
    checkSamePicture(root);
    checkStrangerInTheWay(root);
    checkMissing(root);
    checkIntoStore(root);

    return zt::report("вывоз заметки");
}

// Набор целиком одним TEST: тело не тронуто, argv ему собран здесь.
// Дробить на отдельные проверки — отдельная работа, по одному набору.
TEST(Export, All) {
    std::vector<QByteArray> ztArgs{QByteArrayLiteral("export_test")};
    std::vector<char*> ztArgv;
    for (QByteArray& a : ztArgs) ztArgv.push_back(a.data());
    EXPECT_EQ(0, ztRunSuite(int(ztArgv.size()), ztArgv.data()));
}
