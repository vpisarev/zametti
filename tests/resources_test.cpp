// Вшитое в бинарник: шрифты и иконки.
//
// Набор отвечает на один вопрос — «а точно ли оно там?». Проверять это глазами
// нельзя: на машине разработчика IBM Plex стоит в системе, и программа со
// сломанным qrc выглядит ровно так же, как исправная. Сломается она у другого
// человека. Поэтому все проверки спрашивают не «нашёлся ли шрифт», а «принял ли
// Qt наш файл из ресурсов и то ли в нём, что мы обещали».

#include "resources.h"
#include "settings.h"

#include "test_util.h"

#include <vector>
#include "testdata.h"

#include <QDir>
#include <QFile>
#include <QFont>
#include <QFontDatabase>
#include <QFontInfo>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QXmlStreamReader>

#include <string>

namespace {

std::string s(const QString& q) { return q.toStdString(); }

// Каждое влинкованное начертание грузится и отдаёт обещанное семейство.
// Проверка краснеет от любой из трёх бед: файла нет в qrc, имя в qrc и в
// таблице разошлись, в файл положено не то начертание.
void checkFacesLoad() {
    for (const zametti::EmbeddedFace& face : zametti::embeddedFaces()) {
        const QString path = QString::fromLatin1(face.path);
        const int id = QFontDatabase::addApplicationFont(path);
        ZT_TRUE("шрифт " + s(path) + " принят Qt", id >= 0);
        if (id < 0) continue;
        const QStringList families = QFontDatabase::applicationFontFamilies(id);
        ZT_EQ("семейство из " + s(path), std::string(face.family),
              s(families.value(0)));
    }
}

// То же самое, но через ту дверь, которой пользуется main: пустой список
// значит «всё влинковано».
void checkLoaderIsQuiet() {
    const QStringList failed = zametti::loadEmbeddedFonts();
    ZT_EQ("loadEmbeddedFonts ни на что не жалуется", std::string(),
          s(failed.join(QStringLiteral(", "))));
}

// Умолчания настроек называют семейства по имени. Если имя разойдётся с тем,
// что мы вшили, Qt подставит замену молча — и вёрстка поедет у всех, кроме нас.
void checkDefaultsAreEmbedded() {
    QSet<QString> embedded;
    for (const zametti::EmbeddedFace& face : zametti::embeddedFaces())
        embedded.insert(QString::fromLatin1(face.family));

    const zametti::Appearance def;
    ZT_TRUE("шрифт текста из умолчаний влинкован: " + s(def.fontFamily),
            embedded.contains(def.fontFamily));
    ZT_TRUE("шрифт панелей из умолчаний влинкован: " + s(def.sidebarFontFamily),
            embedded.contains(def.sidebarFontFamily));
}

// Строка из таблицы name шрифта: nameID 1 — семейство, 2 — начертание. Берём
// запись платформы 3 (Windows, UTF-16BE) — она есть в любом ttf нашего века.
//
// Зачем свой разбор вместо QFontDatabase::styles(). У владельца IBM Plex стоит
// в системе, и styles() честно перечислит все начертания даже с наглухо пустым
// qrc: набор был бы зелёным ровно там, где должен краснеть. Спрашивать надо
// байты нашего файла, а не общий список Qt.
QString nameRecord(const QByteArray& ttf, quint16 wanted) {
    const auto u16 = [&](int at) -> quint32 {
        if (at + 1 >= ttf.size()) return 0;
        return quint32(quint8(ttf[at])) << 8 | quint8(ttf[at + 1]);
    };
    const auto u32 = [&](int at) -> quint32 { return u16(at) << 16 | u16(at + 2); };

    const quint32 tables = u16(4);
    int nameOff = 0;
    for (quint32 i = 0; i < tables; ++i) {
        const int rec = 12 + int(i) * 16;
        if (ttf.mid(rec, 4) == "name") nameOff = int(u32(rec + 8));
    }
    if (nameOff == 0) return QString();

    const quint32 count = u16(nameOff + 2);
    const int storage = nameOff + int(u16(nameOff + 4));
    for (quint32 i = 0; i < count; ++i) {
        const int rec = nameOff + 6 + int(i) * 12;
        if (u16(rec) != 3 || u16(rec + 6) != wanted) continue;
        const int len = int(u16(rec + 8));
        const int off = storage + int(u16(rec + 10));
        QString out;
        for (int k = 0; k + 1 < len; k += 2) out.append(QChar(char16_t(u16(off + k))));
        return out;
    }
    return QString();
}

// В каждом файле лежит то начертание, которое обещано его строкой в таблице.
// Жирное и курсив разметка просит на каждой второй заметке; файл с именем
// «-Bold», в котором на самом деле Regular, не сломает ни сборку, ни запуск.
void checkFilesHoldWhatTheyPromise() {
    for (const zametti::EmbeddedFace& face : zametti::embeddedFaces()) {
        const QString path = QString::fromLatin1(face.path);
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) continue;  // о пропаже кричит checkFacesLoad
        const QByteArray ttf = f.readAll();
        ZT_EQ("семейство в самом файле " + s(path), std::string(face.family),
              s(nameRecord(ttf, 1)));
        ZT_EQ("начертание в самом файле " + s(path), std::string(face.style),
              s(nameRecord(ttf, 2)));
    }
}

// Иконки. Проверяем разбираемость, а не отрисовку: модуль Qt6::Svg в сборке
// может отсутствовать (у владельца стоит только его runtime), а вопрос
// «доехал ли файл до ресурсов» от отрисовки не зависит.
void checkIconsArePresent() {
    for (const char* name : zametti::embeddedIcons()) {
        const QString path = zametti::iconPath(name);
        QFile f(path);
        ZT_TRUE("иконка " + s(path) + " есть в ресурсах",
                f.open(QIODevice::ReadOnly));
        if (!f.isOpen()) continue;
        const QByteArray data = f.readAll();
        ZT_TRUE("иконка " + s(path) + " не пуста", !data.isEmpty());

        QXmlStreamReader xml(data);
        QString root;
        while (!xml.atEnd() && root.isEmpty())
            if (xml.readNext() == QXmlStreamReader::StartElement) root = xml.name().toString();
        ZT_EQ("корень " + s(path), std::string("svg"), s(root));
        // Перекрашивать мы собираемся снаружи, композицией. Иконка с зашитой
        // заливкой это молча переживёт и останется чёрной на тёмной теме.
        ZT_TRUE("иконка " + s(path) + " красится снаружи",
                data.contains("stroke=\"currentColor\"") && !data.contains("fill=\"#"));
    }
}

// Список в коде и список в qrc обязаны совпадать. Без этой проверки лишний файл
// в qrc не всплывёт никогда, а недостающий — только на глаз.
void checkListsAgree() {
    QStringList inQrc = QDir(QStringLiteral(":/icons")).entryList(QDir::Files, QDir::Name);
    QStringList inCode;
    for (const char* name : zametti::embeddedIcons())
        inCode.append(QString::fromLatin1(name) + QStringLiteral(".svg"));
    inCode.sort();
    ZT_EQ("иконки в qrc и в коде — один список", s(inCode.join(QStringLiteral("\n"))),
          s(inQrc.join(QStringLiteral("\n"))));

    QStringList fontsInQrc = QDir(QStringLiteral(":/fonts")).entryList(QDir::Files, QDir::Name);
    QStringList fontsInCode;
    for (const zametti::EmbeddedFace& face : zametti::embeddedFaces())
        fontsInCode.append(QString::fromLatin1(face.path).section(QLatin1Char('/'), -1));
    fontsInCode.sort();
    ZT_EQ("шрифты в qrc и в коде — один список",
          s(fontsInCode.join(QStringLiteral("\n"))), s(fontsInQrc.join(QStringLiteral("\n"))));
}

// Иконка приложения пережила переезд art -> resources. Проверка на один байт,
// но именно она краснеет, если путь в qrc забыли поправить.
void checkAppIconSurvived() {
    QFile f(QStringLiteral(":/zametti.png"));
    ZT_TRUE("иконка приложения на месте", f.open(QIODevice::ReadOnly));
    if (f.isOpen()) ZT_TRUE("иконка приложения не пуста", f.size() > 1000);
}

// Снимок приёмки: обе гарнитуры в четырёх начертаниях. Числом это не
// проверяется — на него смотрят. Ценность появляется, когда его снимают на
// машине без IBM Plex в системе: там видно, чем именно программа рисует.
void writeSample(const QString& dir) {
    QImage shot(760, 300, QImage::Format_RGB32);
    shot.fill(Qt::white);
    QPainter p(&shot);
    p.setRenderHint(QPainter::TextAntialiasing);

    int y = 40;
    for (const zametti::EmbeddedFace& face : zametti::embeddedFaces()) {
        QFont f(QString::fromLatin1(face.family), 15);
        const QString style = QString::fromLatin1(face.style);
        f.setBold(style.contains(QStringLiteral("Bold")));
        f.setItalic(style.contains(QStringLiteral("Italic")));
        p.setFont(f);
        p.drawText(20, y, QStringLiteral("%1 %2 — Съешь ещё этих Ямщик gjq 0O1lI")
                              .arg(QString::fromLatin1(face.family), style));
        // Чем нарисовано на самом деле: если файл не доехал, здесь будет чужое
        // имя, и снимок это скажет прямо, а не намёком через вёрстку.
        p.setFont(QFont(QStringLiteral("DejaVu Sans"), 8));
        p.drawText(20, y + 14, QStringLiteral("нарисовано: ") + QFontInfo(f).family());
        y += 32;
    }
    p.end();
    shot.save(QDir(dir).filePath(QStringLiteral("fonts-sample.png")));
}

}  // namespace

static int ztRunSuite(int argc, char** argv) {

    checkFacesLoad();
    checkLoaderIsQuiet();
    checkDefaultsAreEmbedded();
    checkFilesHoldWhatTheyPromise();
    checkIconsArePresent();
    checkListsAgree();
    checkAppIconSurvived();

    // Каталог для снимка приёмки — необязательный аргумент, как у image_shots.
    if (argc > 1) writeSample(QString::fromLocal8Bit(argv[1]));

    return zt::report("resources");
}

// Набор целиком одним TEST: тело не тронуто, argv ему собран здесь.
// Дробить на отдельные проверки — отдельная работа, по одному набору.
TEST(Resources, All) {
    std::vector<QByteArray> ztArgs{QByteArrayLiteral("resources_test")};
    ztArgs.push_back((zt::TestData::outDir(QStringLiteral("resources"))).toLocal8Bit());
    std::vector<char*> ztArgv;
    for (QByteArray& a : ztArgs) ztArgv.push_back(a.data());
    EXPECT_EQ(0, ztRunSuite(int(ztArgv.size()), ztArgv.data()));
}
