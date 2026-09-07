#include "resources.h"

#include <QDir>
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
    // ВТОРОЙ РЕСУРС — ДОКУМЕНТАЦИЯ, и её надо подключить ОТДЕЛЬНО. Порождённый
    // CMake info_docs.qrc даёт свой символ инициализации, и в статической
    // библиотеке компоновщик вправе выбросить его так же, как и первый:
    // ресурсы молча оказывались пустыми, а папка Info — без единой строки.
    Q_INIT_RESOURCE(info_docs);
}

namespace {

// The order is "text first, then panels", as in the settings: Mono — the
// note text and code blocks, Sans — the proportional alternative a person may
// choose (and the footing of the PDF export on machines without fonts), Sans
// SemiCondensed — the panels; Source Serif 4 — the book; Source Code Pro —
// code blocks in a book. The families are not invented: they are what Qt
// reads from the name table of the files (nameID 1, or nameID 16 for the
// instances of a variable font).
//
// SEMIBOLD IS EMBEDDED FOR EVERY NOTE FONT (07.09.2026): inline code is set
// in the text's family at weight 600. Plex Mono brings it as two more static
// faces of one version; Serif and Code Pro are VARIABLE fonts — one upright
// and one italic file each, and Qt (6.7+) registers every named instance of
// the weight axis as a style. Measured before embedding (`zametti-bench
// fonts`): weights 400/500/600/700 resolve exactly, italic comes from the
// italic file, the ink grows with the weight; Source Code Pro's default
// instance is ExtraLight, and weight 400 still resolves to Regular. The
// variable Plex Mono was NOT taken: its family is "IBM Plex Mono Var", and a
// QFont substitution from "IBM Plex Mono" does not reach it (measured: the
// substituted request falls back to another font while QFontInfo reports
// the asked-for name).
constexpr zametti::EmbeddedFace kFaces[] = {
    {":/fonts/IBMPlexMono-Regular.ttf", "IBM Plex Mono", "Regular", false},
    {":/fonts/IBMPlexMono-Italic.ttf", "IBM Plex Mono", "Italic", false},
    {":/fonts/IBMPlexMono-SemiBold.ttf", "IBM Plex Mono", "SemiBold", false},
    {":/fonts/IBMPlexMono-SemiBoldItalic.ttf", "IBM Plex Mono", "SemiBold Italic", false},
    {":/fonts/IBMPlexMono-Bold.ttf", "IBM Plex Mono", "Bold", false},
    {":/fonts/IBMPlexMono-BoldItalic.ttf", "IBM Plex Mono", "Bold Italic", false},
    {":/fonts/IBMPlexSans-Regular.ttf", "IBM Plex Sans", "Regular", false},
    {":/fonts/IBMPlexSans-Italic.ttf", "IBM Plex Sans", "Italic", false},
    {":/fonts/IBMPlexSans-SemiBold.ttf", "IBM Plex Sans", "SemiBold", false},
    {":/fonts/IBMPlexSans-SemiBoldItalic.ttf", "IBM Plex Sans", "SemiBold Italic", false},
    {":/fonts/IBMPlexSans-Bold.ttf", "IBM Plex Sans", "Bold", false},
    {":/fonts/IBMPlexSans-BoldItalic.ttf", "IBM Plex Sans", "Bold Italic", false},
    {":/fonts/IBMPlexSans_SemiCondensed-Regular.ttf", "IBM Plex Sans SemiCondensed", "Regular", false},
    {":/fonts/IBMPlexSans_SemiCondensed-Italic.ttf", "IBM Plex Sans SemiCondensed", "Italic", false},
    {":/fonts/IBMPlexSans_SemiCondensed-Bold.ttf", "IBM Plex Sans SemiCondensed", "Bold", false},
    {":/fonts/IBMPlexSans_SemiCondensed-BoldItalic.ttf", "IBM Plex Sans SemiCondensed",
     "Bold Italic", false},
    {":/fonts/SourceSerif4-Variable.ttf", "Source Serif 4", "Regular", true},
    {":/fonts/SourceSerif4-VariableItalic.ttf", "Source Serif 4", "Italic", true},
    {":/fonts/SourceCodePro-Variable.ttf", "Source Code Pro", "ExtraLight", true},
    {":/fonts/SourceCodePro-VariableItalic.ttf", "Source Code Pro", "ExtraLight Italic", true},
};

constexpr const char* kIcons[] = {
    "archive",             "arrow-down",           "arrow-down-a-z",
    "arrow-up",            "arrow-up-a-z",
    "badge-info",          "calendar-arrow-down",  "calendar-arrow-up",
    "check",               "circle-question-mark",
    "clock-arrow-down",    "clock-arrow-up",       "cloud-sync",
    "columns-3",           "copy",                 "database",
    "database-search",     "ellipsis",             "eye",
    "eye-closed",
    "fast-forward",        "file-plus-corner",
    "folder",
    "folder-input",        "folder-open",          "folder-plus",
    "image-down",          "folder-search",        "list-clock",
    // Books (brief 18): the lock, the reading mode, bookmarks, comments.
    "book-open-text",      "bookmark",             "lock",
    "lock-open",           "message-square-more",  "scroll-text",
    "regex",               "rewind",
    "rotate-ccw-clock",
    "search",              "settings",             "square-arrow-out-up-right",
    "square-m",
};

}  // namespace

namespace {

// Лицензии всего вшитого. Порядок — как в окне: сначала сама программа, потом
// то, что видно глазом (шрифты, иконки), потом библиотеки в порядке появления
// в сборке.
//
// Здесь ОДНА СТРОКА НА СОСТАВНУЮ ЧАСТЬ, даже если текст лицензии у двух из
// них совпадает байт в байт (так у libjxl и jpegli — оба JPEG XL Project
// Authors). Показ схлопывает такие пары сам, сверяя ТЕКСТ, а не путь:
// сцепить их здесь значило бы, что при расхождении upstream мы этого не
// заметим и покажем чужую лицензию под своим именем.
constexpr zametti::EmbeddedLicense kLicenses[] = {
    {":/licenses/zametti.txt", "zametti", "the program itself", "GPL-3.0"},
    {":/licenses/ibm-plex.txt", "IBM Plex", "UI and text fonts", "OFL 1.1"},
    {":/licenses/source-serif-4.txt", "Source Serif 4", "the book font", "OFL 1.1"},
    {":/licenses/source-code-pro.txt", "Source Code Pro", "code in a book", "OFL 1.1"},
    {":/licenses/lucide.txt", "Lucide", "toolbar and tree icons", "ISC"},
    {":/licenses/md4c.txt", "md4c", "markdown parsing", "MIT"},
    {":/licenses/blake3.txt", "BLAKE3", "hashes of notes and attachments", "CC0 / Apache-2.0"},
    {":/licenses/dtl.txt", "DTL", "version diffs in history", "BSD-3"},
    {":/licenses/zstd.txt", "zstd", "snapshots in the edit journal", "BSD / GPL-2.0"},
    {":/licenses/zlib.txt", "zlib", "Deflate inside TIFF", "zlib"},
    {":/licenses/libtiff.txt", "libtiff", "TIFF reading on import", "libtiff"},
    {":/licenses/highway.txt", "highway", "SIMD for libjxl and jpegli", "Apache-2.0 / BSD-3"},
    {":/licenses/libjxl.txt", "libjxl", "images: codec and JPEG transcode", "BSD-3"},
    {":/licenses/jpegli.txt", "jpegli", "16-bit JPEG decode", "BSD-3"},
    {":/licenses/libwebp.txt", "libwebp", "WebP reading on import", "BSD-3"},
    {":/licenses/libde265.txt", "libde265", "HEVC decoding inside HEIC", "LGPL-3 (samples: MIT)"},
    {":/licenses/libgav1.txt", "libgav1", "AV1 decoding inside AVIF", "Apache-2.0"},
    {":/licenses/libheif.txt", "libheif", "HEIF and AVIF container", "LGPL-3 (samples: MIT)"},
    {":/licenses/libsodium.txt", "libsodium", "cloud encryption and key derivation", "ISC"},
    {":/licenses/microtex.txt", "microtex", "LaTeX formula rendering", "MIT"},
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

QStringList embeddedDocs() {
    zamettiInitResources();
    // Порядок — по имени файла: он же порядок строк в папке Info. Нужен
    // другой — файлы называют с числом впереди, как принято у таких каталогов;
    // правкой кода порядок документации не задаётся.
    QStringList out;
    const QDir dir(QStringLiteral(":/docs/info"));
    for (const QString& name : dir.entryList({QStringLiteral("*.md")}, QDir::Files, QDir::Name))
        out << dir.filePath(name);
    if (out.isEmpty())
        std::fprintf(stderr, "no embedded docs: :/docs/info is empty\n");
    return out;
}

QString embeddedText(const char* path) {
    zamettiInitResources();
    QFile file(QString::fromLatin1(path));
    if (!file.open(QIODevice::ReadOnly)) {
        // Молча пустая вкладка «Лицензии» — это нарушение чужих условий, о
        // котором никто не узнает. Жалуемся.
        std::fprintf(stderr, "no embedded file: %s\n", path);
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
