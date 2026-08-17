#include "export_note.h"

#include "znote.h"

#include "document.h"
#include "serializer.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QUrl>

#include <algorithm>
#include <string>
#include <vector>

namespace zametti {
namespace {

// Знаки, которые хоть где-то запрещены в имени файла. В ext4 незаконен один
// '/', но экспортированный файл для того и делают, чтобы он уехал дальше —
// в архив, в почту, на флешку с FAT32, к человеку с Windows. Поэтому список
// общий: то, что не примет NTFS/FAT/exFAT, и двоеточие, на котором спотыкается
// macOS в старых оболочках.
bool forbidden(QChar ch) {
    static const QString bad = QStringLiteral("/\\:*?\"<>|");
    return ch.unicode() < 0x20 || bad.contains(ch);
}

// Имена, занятые в Windows под устройства. Расширение не спасает: "CON.md" там
// тоже не создать. Мы не на Windows, но файл туда попадёт.
bool reservedOnWindows(const QString& base) {
    static const QStringList names = {
        QStringLiteral("CON"),  QStringLiteral("PRN"),  QStringLiteral("AUX"),
        QStringLiteral("NUL"),  QStringLiteral("COM1"), QStringLiteral("COM2"),
        QStringLiteral("COM3"), QStringLiteral("COM4"), QStringLiteral("COM5"),
        QStringLiteral("COM6"), QStringLiteral("COM7"), QStringLiteral("COM8"),
        QStringLiteral("COM9"), QStringLiteral("LPT1"), QStringLiteral("LPT2"),
        QStringLiteral("LPT3"), QStringLiteral("LPT4"), QStringLiteral("LPT5"),
        QStringLiteral("LPT6"), QStringLiteral("LPT7"), QStringLiteral("LPT8"),
        QStringLiteral("LPT9")};
    return names.contains(base.toUpper());
}

std::string readAll(const QString& path, bool* ok) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (ok != nullptr) *ok = false;
        return {};
    }
    const QByteArray bytes = file.readAll();
    if (ok != nullptr) *ok = true;
    return std::string(bytes.constData(), size_t(bytes.size()));
}

bool writeAll(const QString& path, const std::string& bytes, QString* error) {
    // QSaveFile: недописанного файла на месте старого не остаётся. fsync не
    // зовём — уговор владельца, он общий на всю программу.
    QSaveFile file(path);
    file.setDirectWriteFallback(true);
    if (!file.open(QIODevice::WriteOnly)) {
        *error = QStringLiteral("не открыть на запись: %1").arg(path);
        return false;
    }
    const qint64 written = file.write(bytes.data(), qint64(bytes.size()));
    if (written != qint64(bytes.size()) || !file.commit()) {
        *error = QStringLiteral("не записать: %1").arg(path);
        return false;
    }
    return true;
}

bool sameBytes(const QString& a, const QString& b) {
    QFile fa(a);
    QFile fb(b);
    if (!fa.open(QIODevice::ReadOnly) || !fb.open(QIODevice::ReadOnly)) return false;
    if (fa.size() != fb.size()) return false;
    // Кусками: вложение бывает и в сотню мегабайт, а сравнение ради имени файла
    // не повод поднять их обе в память целиком.
    constexpr qint64 kChunk = 1 << 20;
    while (!fa.atEnd()) {
        if (fa.read(kChunk) != fb.read(kChunk)) return false;
    }
    return fb.atEnd();
}

// Ссылка на файл рядом, а не на сеть. Всё, у чего есть схема (http:, data:,
// mailto:), — чужое: такие оставляем как есть.
bool localReference(const QString& href) {
    if (href.isEmpty()) return false;
    const QUrl url(href);
    if (url.scheme().size() > 1) return false;   // "c:" на Windows — не схема
    return true;
}

// "имя-1.jxl", "имя-2.jxl", ... Свободное имя рядом с занятым.
QString freeName(const QDir& dir, const QString& wanted) {
    const QFileInfo info(wanted);
    const QString base = info.completeBaseName();
    const QString suffix = info.suffix().isEmpty() ? QString() : QLatin1Char('.') + info.suffix();
    for (int n = 1; n < 10000; ++n) {
        const QString tried = QStringLiteral("%1-%2%3").arg(base).arg(n).arg(suffix);
        if (!QFileInfo::exists(dir.filePath(tried))) return tried;
    }
    return {};
}

}  // namespace

QString exportTargetPath(const QString& dir, const QString& base, bool paper) {
    const QString name = base + (paper ? QStringLiteral(".pdf") : QStringLiteral(".md"));
    return dir.isEmpty() ? name : QDir(dir).filePath(name);
}

QString fileNameFromTitle(const QString& title) {
    QString out;
    out.reserve(title.size());
    for (const QChar ch : title) out.append(forbidden(ch) ? QLatin1Char('-') : ch);

    // Точки и пробелы по краям Windows молча срезает при создании файла — а
    // значит имя, которое мы предложили, и имя, которое получилось, разошлись
    // бы. Срезаем сами, чтобы человек видел то, что будет.
    while (!out.isEmpty() && (out.endsWith(QLatin1Char('.')) || out.endsWith(QLatin1Char(' '))))
        out.chop(1);
    while (!out.isEmpty() && out.startsWith(QLatin1Char(' '))) out.remove(0, 1);

    // Предел — в БАЙТАХ, а не в знаках: в ext4 и большинстве прочих потолок
    // компонента пути 255 байт, а кириллическая буква весит два. Режем с
    // запасом под расширение и под "-1" при столкновении.
    constexpr int kMaxBytes = 200;
    while (out.toUtf8().size() > kMaxBytes) out.chop(1);
    while (!out.isEmpty() && (out.endsWith(QLatin1Char('.')) || out.endsWith(QLatin1Char(' '))))
        out.chop(1);

    if (out.isEmpty()) return QStringLiteral("Без названия");
    if (reservedOnWindows(out)) out.prepend(QLatin1Char('_'));
    return out;
}

// Срезать шапку с НАЧАЛА текста, не трогая всего остального. Ищем ровно то, что
// разобрало ядро: закрывающий «-->» первого комментария и перевод строки за ним,
// плюс пустую строку, если она там была (meta.blankAfter()).
std::string withoutMeta(const std::string& source, const ZNote& note) {
    if (!note.hasHeader()) return source;
    const size_t open = source.find("<!-- zametti");
    if (open != 0) return source;   // шапка не в начале — не наша, не трогаем
    const size_t close = source.find("-->", open);
    if (close == std::string::npos) return source;
    size_t at = close + 3;
    if (at < source.size() && source[at] == '\n') ++at;
    if (note.header().blankAfter() && at < source.size() && source[at] == '\n') ++at;
    return source.substr(at);
}

ExportReport exportMarkdown(const QString& notePath, const QString& targetPath, bool keepMeta) {
    ExportReport report;

    bool read = false;
    const std::string source = readAll(notePath, &read);
    if (!read) {
        report.error = QStringLiteral("не прочитать заметку: %1").arg(notePath);
        return report;
    }

    const QDir storeDir = QFileInfo(notePath).absoluteDir();
    const QDir outDir = QFileInfo(targetPath).absoluteDir();
    if (!outDir.exists()) {
        report.error = QStringLiteral("каталога нет: %1").arg(outDir.path());
        return report;
    }
    // Вывоз В САМО ХРАНИЛИЩЕ запрещён. Заметка легла бы туда под человеческим
    // именем — а хранилище держится на том, что имя файла это id, и такой файл
    // стал бы для программы заметкой-самозванцем.
    if (outDir.absolutePath() == storeDir.absolutePath()) {
        report.error = QStringLiteral(
            "вывоз в само хранилище невозможен: там имя файла — это идентификатор");
        return report;
    }

    ZNote note;
    note.load(source);

    // Все вложения заметки, по одному разу на имя: одна картинка бывает
    // вставлена дважды, а копировать её дважды незачем.
    struct Carried {
        QString href;      // имя файла, как оно стоит в заметке (без атрибутов)
        QString outName;   // под каким именем ложится рядом (может отличаться)
        bool renamed = false;
    };
    std::vector<Carried> found;
    auto indexOf = [&found](const QString& href) -> int {
        for (size_t i = 0; i < found.size(); ++i)
            if (found[i].href == href) return int(i);
        return -1;
    };

    for (const Attachment& image : note.doc().attachments()) {
        if (!localReference(image.id) || indexOf(image.id) >= 0) continue;
        found.push_back({image.id, QFileInfo(image.id).fileName(), false});
    }

    // Решение по каждому вложению принимается ДО того, как что-то записано:
    // половина вывоза хуже, чем отказ.
    for (Carried& item : found) {
        const QString from = QDir::isAbsolutePath(item.href) ? item.href
                                                             : storeDir.filePath(item.href);
        if (!QFileInfo::exists(from)) {
            ++report.imagesMissing;
            report.notes << QStringLiteral("нет вложения: %1").arg(item.href);
            item.outName.clear();   // копировать нечего, ссылку не трогаем
            continue;
        }
        const QString to = outDir.filePath(item.outName);
        if (!QFileInfo::exists(to)) continue;   // место свободно, имя остаётся
        if (sameBytes(from, to)) {
            ++report.imagesReused;
            continue;   // это та же картинка, уже вывезенная
        }
        const QString fresh = freeName(outDir, item.outName);
        if (fresh.isEmpty()) {
            report.error = QStringLiteral("не подобрать свободное имя для %1").arg(item.outName);
            return report;
        }
        item.outName = fresh;
        item.renamed = true;
    }

    const bool renaming = std::any_of(found.begin(), found.end(),
                                      [](const Carried& a) { return a.renamed; });

    std::string outText = source;
    if (renaming) {
        // Круг проверяется на ЭТОЙ заметке и до всякой записи. Если разбор и
        // сборка не дают исходные байты, пересобирать нельзя: наружу уехало бы
        // не то, что лежит в хранилище, и человек об этом не узнал бы.
        if (!note.isCanonical(source)) {
            report.error = QStringLiteral(
                "рядом уже лежит другой файл с именем вложения, а переписать ссылку нельзя: "
                "разбор этой заметки не сходится с её байтами. Вывезите в пустой каталог.");
            return report;
        }
        note.doc().rewriteAttachments([&](const QString& href) {
            const int at = indexOf(href);
            if (at < 0 || !found[size_t(at)].renamed) return QString();
            return found[size_t(at)].outName;
        });
        outText = note.toMarkdown();
    }
    if (!keepMeta) outText = withoutMeta(outText, note);

    if (!writeAll(targetPath, outText, &report.error)) return report;

    for (const Carried& item : found) {
        if (item.outName.isEmpty()) continue;
        const QString to = outDir.filePath(item.outName);
        if (QFileInfo::exists(to)) continue;   // тот самый побайтово совпавший
        const QString from = QDir::isAbsolutePath(item.href) ? item.href
                                                             : storeDir.filePath(item.href);
        if (!QFile::copy(from, to)) {
            report.error = QStringLiteral("не скопировать вложение: %1").arg(item.href);
            return report;
        }
        ++report.imagesCopied;
        if (item.renamed)
            report.notes << QStringLiteral("вложение %1 легло как %2: рядом уже был другой файл "
                                           "с таким именем")
                                .arg(item.href, item.outName);
    }
    return report;
}

}  // namespace zametti
