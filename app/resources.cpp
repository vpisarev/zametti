#include "resources.h"

#include <QFile>
#include <QFontDatabase>
#include <QString>

#include <cstdio>

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
    "arrow-down-a-z",      "arrow-up-a-z",         "calendar-arrow-down",
    "calendar-arrow-up",   "check",                "circle-question-mark",
    "clock-arrow-down",    "clock-arrow-up",       "cloud-sync",
    "columns-3",           "copy",                 "database-search",
    "fast-forward",        "file-plus-corner",     "folder",
    "folder-input",        "folder-open",          "folder-plus",
    "image-down",          "rewind",               "rotate-ccw-clock",
    "search",              "settings",             "square-arrow-out-up-right",
    "trash-2",
};

}  // namespace

namespace {

// Лицензии всего вшитого. Порядок — как в окне: сначала сама программа, потом
// то, что видно глазом (шрифты, иконки), потом библиотеки в порядке появления
// в сборке.
constexpr zametti::EmbeddedLicense kLicenses[] = {
    {":/licenses/zametti.txt", "zametti", "сама программа", "GPL-3.0"},
    {":/licenses/ibm-plex.txt", "IBM Plex", "шрифты интерфейса и текста", "OFL 1.1"},
    {":/licenses/lucide.txt", "Lucide", "иконки тулбара и дерева", "ISC"},
    {":/licenses/md4c.txt", "md4c", "разбор markdown", "MIT"},
    {":/licenses/blake3.txt", "BLAKE3", "отпечатки заметок и вложений", "CC0 / Apache-2.0"},
    {":/licenses/dtl.txt", "DTL", "разность версий в истории", "BSD-3"},
    {":/licenses/zstd.txt", "zstd", "слепки в журнале правок", "BSD / GPL-2.0"},
    {":/licenses/zlib.txt", "zlib", "Deflate внутри TIFF", "zlib"},
    {":/licenses/libtiff.txt", "libtiff", "чтение TIFF при импорте", "libtiff"},
    {":/licenses/highway.txt", "highway", "SIMD для libjxl и jpegli", "Apache-2.0 / BSD-3"},
    {":/licenses/libjxl.txt", "libjxl", "картинки: кодек и транскод JPEG", "BSD-3"},
    {":/licenses/jpegli.txt", "jpegli", "разжатие JPEG в 16 бит", "BSD-3"},
};

constexpr zametti::EmbeddedDoc kDocs[] = {
    {":/docs/README.md", "О программе"},
    {":/docs/zametti-storage.md", "Формат хранилища"},
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

std::span<const EmbeddedLicense> embeddedLicenses() {
    return std::span<const EmbeddedLicense>(kLicenses, std::size(kLicenses));
}

std::span<const EmbeddedDoc> embeddedDocs() {
    return std::span<const EmbeddedDoc>(kDocs, std::size(kDocs));
}

QString embeddedText(const char* path) {
    zamettiInitResources();
    QFile file(QString::fromLatin1(path));
    if (!file.open(QIODevice::ReadOnly)) {
        // Молча пустая вкладка «Лицензии» — это нарушение чужих условий, о
        // котором никто не узнает. Жалуемся.
        std::fprintf(stderr, "нет вшитого файла: %s\n", path);
        return {};
    }
    return QString::fromUtf8(file.readAll());
}

QString iconPath(const char* name) {
    zamettiInitResources();
    return QStringLiteral(":/icons/") + QString::fromLatin1(name) +
           QStringLiteral(".svg");
}

}  // namespace zametti
