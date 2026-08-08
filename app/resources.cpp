#include "resources.h"

#include <QFontDatabase>
#include <QString>

// Q_INIT_RESOURCE разворачивается в объявление функции прямо в месте вызова, а
// объявление в области блока принадлежит ОБЪЕМЛЮЩЕМУ пространству имён. Из
// zametti:: или из безымянного пространства оно искало бы
// zametti::qInitResources_resources (либо символ с внутренним связыванием) и не
// нашло бы. Отсюда обёртка в глобальном пространстве — это необходимость, а не
// вкус. Звать можно сколько угодно: Qt сама отсекает повторную регистрацию.
static void zamettiInitResources() {
    Q_INIT_RESOURCE(resources);
}

namespace {

// Порядок — «сначала текст, потом панели», как в настройках. Семейства взяты
// не из головы: это то, что Qt читает из таблицы name файлов (nameID 1).
constexpr zametti::EmbeddedFace kFaces[] = {
    {":/fonts/IBMPlexMono-Regular.ttf", "IBM Plex Mono", "Regular"},
    {":/fonts/IBMPlexMono-Italic.ttf", "IBM Plex Mono", "Italic"},
    {":/fonts/IBMPlexMono-Bold.ttf", "IBM Plex Mono", "Bold"},
    {":/fonts/IBMPlexMono-BoldItalic.ttf", "IBM Plex Mono", "Bold Italic"},
    {":/fonts/IBMPlexSans_SemiCondensed-Regular.ttf", "IBM Plex Sans SemiCondensed", "Regular"},
    {":/fonts/IBMPlexSans_SemiCondensed-Italic.ttf", "IBM Plex Sans SemiCondensed", "Italic"},
    {":/fonts/IBMPlexSans_SemiCondensed-Bold.ttf", "IBM Plex Sans SemiCondensed", "Bold"},
    {":/fonts/IBMPlexSans_SemiCondensed-BoldItalic.ttf", "IBM Plex Sans SemiCondensed",
     "Bold Italic"},
};

constexpr const char* kIcons[] = {
    "arrow-down-a-z",   "circle-question-mark", "clock-arrow-down",
    "cloud-sync",       "columns-3",            "database-search",
    "fast-forward",     "file-plus-corner",     "folder",
    "folder-input",     "folder-open",          "folder-plus",
    "image-down",       "rewind",               "rotate-ccw-clock",
    "search",           "settings",             "square-arrow-out-up-right",
    "trash-2",
};

}  // namespace

namespace zametti {

std::span<const EmbeddedFace> embeddedFaces() {
    return std::span<const EmbeddedFace>(kFaces, std::size(kFaces));
}

std::span<const char* const> embeddedIcons() {
    return std::span<const char* const>(kIcons, std::size(kIcons));
}

QStringList loadEmbeddedFonts() {
    zamettiInitResources();
    QStringList failed;
    for (const EmbeddedFace& face : kFaces) {
        const QString path = QString::fromLatin1(face.path);
        // Повторную регистрацию Qt переживает сама: тот же файл получает новый
        // id, семейство не двоится. Считать «уже грузили» своим флагом нечестно
        // — он врал бы после QFontDatabase::removeAllApplicationFonts().
        const int id = QFontDatabase::addApplicationFont(path);
        if (id < 0) {
            failed.append(path);
            continue;
        }
        // Файл принят — это ещё не значит, что в нём то, что мы думаем. Имя
        // семейства решает всё: по нему настройки и спрашивают шрифт.
        if (!QFontDatabase::applicationFontFamilies(id).contains(
                QString::fromLatin1(face.family)))
            failed.append(path);
    }
    return failed;
}

QString iconPath(const char* name) {
    zamettiInitResources();
    return QStringLiteral(":/icons/") + QString::fromLatin1(name) +
           QStringLiteral(".svg");
}

}  // namespace zametti
