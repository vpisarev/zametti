#include "store.h"

#include "note_id.h"
#include "parser.h"
#include "serializer.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QTimeZone>

#include <algorithm>
#include <iterator>
#include <map>
#include <random>
#include <set>
#include <string>
#include <vector>

namespace zametti::store {
namespace {

QString fromUtf8(const std::string& s) {
    return QString::fromUtf8(s.data(), qsizetype(s.size()));
}

std::string toUtf8(const QString& s) {
    const QByteArray b = s.toUtf8();
    return std::string(b.constData(), size_t(b.size()));
}

bool readAll(const QString& path, std::string& out) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    const QByteArray bytes = f.readAll();
    out.assign(bytes.constData(), size_t(bytes.size()));
    return true;
}

QString isoUtc(const QDateTime& t) {
    return t.toUTC().toString(QStringLiteral("yyyy-MM-ddTHH:mm:ss'Z'"));
}

// ISO-8601 UTC из брифа: YYYY-MM-DDTHH:MM:SSZ. Прочее не признаём — лучше
// честные fs-времена с пометкой, чем криво разобранная дата.
QDateTime parseIso(const QString& value) {
    QDateTime t = QDateTime::fromString(value, QStringLiteral("yyyy-MM-ddTHH:mm:ss'Z'"));
    if (t.isValid()) t.setTimeZone(QTimeZone::utc());
    return t;
}

std::uint64_t randomPart() {
    std::random_device rd;
    return (std::uint64_t(rd()) << 32) | rd();
}

// --- import ------------------------------------------------------------

struct SrcEntry {
    QString rel;        // путь от корня источника, '/'-разделители; для файла — с ".md"
    QString abs;
    bool isDir = false;
    QString title;      // для каталога — его имя
    QString parentRel;  // rel родительского каталога; пусто — корень
    QDateTime created;
    QDateTime modified;
    QString timesFrom;  // "manifest" | "front matter" | "fs"
    QString displayTitle;   // настоящий заголовок: из манифеста, иначе имя файла
    std::string id;
};

struct ManifestEntry {
    QString folder;
    QString title;
    QDateTime created;
    QDateTime modified;
    bool used = false;
};

// Времена из YAML-шапки источника, если она есть. Сама шапка остаётся в
// содержимом как есть (в корпусе таких файлов нет — замерено; крючок
// минимальный, факт попадает в отчёт).
bool frontMatterTimes(const std::string& bytes, QDateTime& created, QDateTime& modified) {
    if (bytes.compare(0, 4, "---\n") != 0) return false;
    const size_t end = bytes.find("\n---\n", 4);
    if (end == std::string::npos) return false;
    const QString head = fromUtf8(bytes.substr(4, end - 4));
    bool any = false;
    for (const QString& line : head.split(QLatin1Char('\n'))) {
        const qsizetype colon = line.indexOf(QLatin1Char(':'));
        if (colon <= 0) continue;
        const QString key = line.left(colon).trimmed().toLower();
        const QDateTime value = parseIso(line.mid(colon + 1).trimmed());
        if (!value.isValid()) continue;
        if (key == QStringLiteral("created")) { created = value; any = true; }
        if (key == QStringLiteral("modified") || key == QStringLiteral("updated")) {
            modified = value;
            any = true;
        }
    }
    return any;
}

// Запуск утилиты; пустой вывод не интересен, важен только код возврата.
bool runTool(const QString& program, const QStringList& args) {
    QProcess process;
    process.start(program, args);
    if (!process.waitForStarted(5000)) return false;
    if (!process.waitForFinished(120000)) {
        process.kill();
        return false;
    }
    return process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
}

// Пережатие вложения по правилам владельца: png → webp без потерь, heic →
// webp q90 (heif-convert + cwebp), jpeg и webp — байт в байт. EXIF/ICC/XMP
// переезжают целиком: у png/jpeg-источников — cwebp -metadata all, у heic —
// exiftool копирует из исходника прямо в webp. Без кодеков — копия как есть
// и беда в отчёте (байты не теряются никогда).
struct Converted {
    QByteArray bytes;
    QString ext;        // конечное расширение
    bool degraded = false;   // кодека не нашлось, скопировано как есть
};

Converted convertAttachment(const QString& srcAbs, const QString& tempDir) {
    Converted out;
    const QString ext = QFileInfo(srcAbs).suffix().toLower();
    out.ext = ext.isEmpty() ? QStringLiteral("bin") : ext;

    const auto readBack = [&out](const QString& path) {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) return false;
        out.bytes = f.readAll();
        return true;
    };
    const auto copyAsIs = [&]() {
        QFile f(srcAbs);
        if (f.open(QIODevice::ReadOnly)) out.bytes = f.readAll();
        return out;
    };

    if (ext == QStringLiteral("png")) {
        const QString dst = tempDir + QStringLiteral("/out.webp");
        if (runTool(QStringLiteral("cwebp"),
                    {QStringLiteral("-lossless"), QStringLiteral("-metadata"),
                     QStringLiteral("all"), srcAbs, QStringLiteral("-o"), dst}) &&
            readBack(dst)) {
            out.ext = QStringLiteral("webp");
            return out;
        }
        out.degraded = true;
        return copyAsIs();
    }

    if (ext == QStringLiteral("heic") || ext == QStringLiteral("heif")) {
        const QString mid = tempDir + QStringLiteral("/mid.png");
        const QString dst = tempDir + QStringLiteral("/out.webp");
        bool ok = runTool(QStringLiteral("heif-convert"), {srcAbs, mid});
        QString midPath = mid;
        if (ok && !QFileInfo::exists(mid)) {
            // Много-картиночный heic: heif-convert пишет mid-1.png и далее.
            const QStringList parts = QDir(tempDir).entryList(
                {QStringLiteral("mid*.png")}, QDir::Files, QDir::Name);
            if (parts.isEmpty()) ok = false;
            else midPath = tempDir + QLatin1Char('/') + parts.first();
        }
        ok = ok && runTool(QStringLiteral("cwebp"),
                           {QStringLiteral("-q"), QStringLiteral("90"), midPath,
                            QStringLiteral("-o"), dst});
        if (ok) {
            // Метаданные heic живут в контейнере — копируются из исходника.
            // Неудача exiftool не роняет пережатие: картинка дороже тегов.
            runTool(QStringLiteral("exiftool"),
                    {QStringLiteral("-overwrite_original"),
                     QStringLiteral("-TagsFromFile"), srcAbs,
                     QStringLiteral("-all:all"), dst});
            if (readBack(dst)) {
                out.ext = QStringLiteral("webp");
                return out;
            }
        }
        out.degraded = true;
        return copyAsIs();
    }

    return copyAsIs();
}

// Ссылка локальная? Схемы, абсолютные пути и якоря — нет.
bool isLocalRelative(const QString& href) {
    if (href.isEmpty()) return false;
    if (href.startsWith(QLatin1Char('/')) || href.startsWith(QLatin1Char('#'))) return false;
    if (href.contains(QStringLiteral("://"))) return false;
    if (href.startsWith(QStringLiteral("mailto:")) || href.startsWith(QStringLiteral("tel:")))
        return false;
    return true;
}

// Путь цели относительно корня источника; пусто — вышли за корень.
QString resolveInside(const QString& srcRoot, const QString& noteDirRel, const QString& href) {
    const QString joined =
        noteDirRel.isEmpty() ? href : noteDirRel + QLatin1Char('/') + href;
    const QString cleaned = QDir::cleanPath(joined);
    if (cleaned.startsWith(QStringLiteral("../")) || cleaned == QStringLiteral("..")) return {};
    if (!QFileInfo::exists(srcRoot + QLatin1Char('/') + cleaned)) return {};
    return cleaned;
}

// Заголовок так, как его чистит конвертер под имя файла (замерено на
// корпусе): '#' и ':' удаляются, '/' становится '-', неразрывный пробел —
// обычным, края обрезаются.
QString sanitizedTitle(QString title) {
    title.remove(QLatin1Char('#'));
    title.remove(QLatin1Char(':'));
    title.remove(QLatin1Char('"'));
    title.replace(QLatin1Char('/'), QLatin1Char('-'));
    title.replace(QChar(0x00A0), QLatin1Char(' '));
    return title.trimmed();
}

}  // namespace

bool initStore(const QString& dir, QString* error) {
    QDir d(dir);
    if (d.exists()) {
        const QStringList entries =
            d.entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden);
        if (!entries.isEmpty()) {
            if (error != nullptr)
                *error = QStringLiteral("каталог не пуст: %1").arg(dir);
            return false;
        }
    } else if (!QDir().mkpath(dir)) {
        if (error != nullptr) *error = QStringLiteral("не создаётся: %1").arg(dir);
        return false;
    }
    // .zametti — состояние; .rescue — побитые файлы редактора (с точкой: на
    // сервер не синхронизируется); history — история заметок (синхронизируется).
    for (const char* sub : {".zametti", ".rescue", "history"}) {
        if (!QDir(dir).mkpath(QString::fromLatin1(sub))) {
            if (error != nullptr)
                *error = QStringLiteral("не создаётся: %1/%2").arg(dir, sub);
            return false;
        }
    }
    // Проверка прав — делом: пробный файл, а не флаги.
    QFile probe(dir + QStringLiteral("/.zametti/.probe"));
    if (!probe.open(QIODevice::WriteOnly)) {
        if (error != nullptr) *error = QStringLiteral("нет прав на запись: %1").arg(dir);
        return false;
    }
    probe.close();
    probe.remove();
    return true;
}

QString newNote(const QString& root, const QString& parentId, QString* error) {
    if (!QDir(root).exists()) {
        if (error != nullptr) *error = QStringLiteral("нет каталога: %1").arg(root);
        return {};
    }
    if (!parentId.isEmpty()) {
        if (!isValidNoteId(toUtf8(parentId))) {
            if (error != nullptr)
                *error = QStringLiteral("родитель не похож на id: %1").arg(parentId);
            return {};
        }
        if (!QFileInfo::exists(root + QLatin1Char('/') + parentId +
                               QStringLiteral(".md"))) {
            if (error != nullptr)
                *error = QStringLiteral("родителя нет в хранилище: %1").arg(parentId);
            return {};
        }
    }

    // Без пустой строки после "-->": она положена перед контентом, а контента
    // в свежей заметке нет — иначе verify честно находил бы дрейф (замерено).
    std::string content = "<!-- zametti\n";
    if (!parentId.isEmpty()) content += "parent: " + toUtf8(parentId) + "\n";
    content += "created: " + toUtf8(isoUtc(QDateTime::currentDateTimeUtc())) + "\n-->\n";

    std::string path;
    const std::string id = createNoteFile(toUtf8(root), content, &path);
    if (id.empty()) {
        if (error != nullptr) *error = QStringLiteral("не записалось в %1").arg(root);
        return {};
    }
    return fromUtf8(path);
}

bool importTree(const ImportOptions& options, Report& report) {
    const QString srcRoot = QDir(options.from).absolutePath();
    if (!QDir(srcRoot).exists()) {
        report.problem(QStringLiteral("нет источника: %1").arg(options.from));
        return false;
    }
    const QString root = QDir(options.root).absolutePath();

    // --- скан источника --------------------------------------------------
    std::vector<SrcEntry> entries;
    // Скрытое (".obsidian" и прочий служебный мусор) не импортируется — но и
    // не молчком: пропуск виден в отчёте.
    {
        QDirIterator hidden(srcRoot,
                            QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot | QDir::Hidden,
                            QDirIterator::Subdirectories);
        while (hidden.hasNext()) {
            hidden.next();
            if (hidden.fileName().startsWith(QLatin1Char('.')))
                report.note(QStringLiteral("скрытое пропущено: %1")
                                .arg(QDir(srcRoot).relativeFilePath(hidden.filePath())));
        }
    }
    {
        QDirIterator it(srcRoot, QDir::Dirs | QDir::NoDotAndDotDot,
                        QDirIterator::Subdirectories);
        while (it.hasNext()) {
            it.next();
            SrcEntry e;
            e.abs = it.filePath();
            e.rel = QDir(srcRoot).relativeFilePath(e.abs);
            e.isDir = true;
            e.title = it.fileName();
            const QString parent = QFileInfo(e.rel).path();
            e.parentRel = parent == QStringLiteral(".") ? QString() : parent;
            entries.push_back(std::move(e));
        }
    }
    {
        QDirIterator it(srcRoot, {QStringLiteral("*.md")}, QDir::Files,
                        QDirIterator::Subdirectories);
        while (it.hasNext()) {
            it.next();
            SrcEntry e;
            e.abs = it.filePath();
            e.rel = QDir(srcRoot).relativeFilePath(e.abs);
            e.isDir = false;
            e.title = QFileInfo(e.abs).completeBaseName();
            const QString parent = QFileInfo(e.rel).path();
            e.parentRel = parent == QStringLiteral(".") ? QString() : parent;
            entries.push_back(std::move(e));
        }
    }
    // Устойчивый порядок: сначала по глубине неважно, важно детерминированно.
    std::sort(entries.begin(), entries.end(),
              [](const SrcEntry& a, const SrcEntry& b) { return a.rel < b.rel; });

    // --- манифест Apple Notes --------------------------------------------
    std::vector<ManifestEntry> manifest;
    if (!options.appleManifest.isEmpty()) {
        std::string bytes;
        if (!readAll(options.appleManifest, bytes)) {
            report.problem(QStringLiteral("манифест не читается: %1")
                               .arg(options.appleManifest));
            return false;
        }
        QJsonParseError parseError{};
        const QJsonDocument doc = QJsonDocument::fromJson(
            QByteArray(bytes.data(), qsizetype(bytes.size())), &parseError);
        if (!doc.isArray()) {
            report.problem(QStringLiteral("манифест не JSON-массив: %1")
                               .arg(parseError.errorString()));
            return false;
        }
        for (const QJsonValue& value : doc.array()) {
            const QJsonObject o = value.toObject();
            ManifestEntry m;
            m.folder = o.value(QStringLiteral("folder")).toString();
            m.title = o.value(QStringLiteral("title")).toString();
            m.created = parseIso(o.value(QStringLiteral("created")).toString());
            m.modified = parseIso(o.value(QStringLiteral("modified")).toString());
            manifest.push_back(std::move(m));
        }
    }

    // --- времена и id ------------------------------------------------------
    std::set<std::string> takenIds;
    std::map<QString, size_t> byRel;   // rel .md-файла → индекс entry
    for (size_t i = 0; i < entries.size(); ++i) {
        SrcEntry& e = entries[i];
        const QFileInfo info(e.abs);

        // Приоритет: манифест → front matter → fs-времена с пометкой.
        ManifestEntry* matched = nullptr;
        int matches = 0;
        if (!e.isDir) {
            for (ManifestEntry& m : manifest)
                if (m.folder == e.parentRel && m.title == e.title) {
                    ++matches;
                    matched = &m;
                }
            // Конвертер чистил заголовки под имена файлов — второй заход по
            // очищенному ключу, только если точный не нашёлся.
            if (matches == 0)
                for (ManifestEntry& m : manifest)
                    if (m.folder == e.parentRel && sanitizedTitle(m.title) == e.title) {
                        ++matches;
                        matched = &m;
                    }
        }
        if (matches > 1) {
            report.problem(
                QStringLiteral("манифест неоднозначен для «%1»: %2 записи")
                    .arg(e.rel)
                    .arg(matches));
            matched = nullptr;
        }
        std::string bytes;
        if (!e.isDir && !readAll(e.abs, bytes)) {
            report.problem(QStringLiteral("не читается: %1").arg(e.rel));
            continue;
        }

        QDateTime created;
        QDateTime modified;
        e.displayTitle = e.title;
        if (matched != nullptr && matched->created.isValid()) {
            matched->used = true;
            created = matched->created;
            modified = matched->modified.isValid() ? matched->modified : matched->created;
            e.timesFrom = QStringLiteral("manifest");
            // Заголовок из манифеста — настоящий: конвертер чистил его под
            // имя файла ('#', ':', кавычки), манифест хранит как было.
            e.displayTitle = matched->title;
        } else if (!e.isDir && frontMatterTimes(bytes, created, modified)) {
            if (!modified.isValid()) modified = created;
            if (!created.isValid()) created = modified;
            e.timesFrom = QStringLiteral("front matter");
            report.note(QStringLiteral("front matter в «%1»: времена взяты, "
                                       "шапка оставлена как есть")
                            .arg(e.rel));
        } else {
            const QDateTime birth = info.birthTime();
            modified = info.lastModified();
            created = birth.isValid() && birth <= modified ? birth : modified;
            e.timesFrom = QStringLiteral("fs");
            report.note(QStringLiteral("времена из файловой системы: %1").arg(e.rel));
        }
        e.created = created;
        e.modified = modified;

        // created кодируется в id; случайная часть уникальна в пределах партии.
        for (;;) {
            e.id = makeNoteId(std::uint64_t(created.toSecsSinceEpoch()), randomPart());
            if (takenIds.insert(e.id).second) break;
        }
        if (!e.isDir) byRel[e.rel] = i;
    }

    // Манифест, не нашедший файла, — в отчёт: молчаливых пропусков нет.
    for (const ManifestEntry& m : manifest)
        if (!m.used)
            report.problem(QStringLiteral("манифест без файла: «%1» / «%2»")
                               .arg(m.folder, m.title));

    // rel каталога → id заметки-каталога.
    std::map<QString, std::string> dirIds;
    for (const SrcEntry& e : entries)
        if (e.isDir) dirIds[e.rel] = e.id;

    // --- запись ------------------------------------------------------------
    if (!options.dryRun) {
        QString initError;
        if (!initStore(root, &initError)) {
            report.problem(initError);
            return false;
        }
    }

    QTemporaryDir temp;
    std::map<QByteArray, QString> attachmentByHash;   // содержимое исходника → имя
    std::map<QString, QString> attachmentByAbs;       // абсолютный путь → имя
    std::map<QString, qint64> attachmentSizes;        // имя → байты
    int wikilinks = 0;

    // Вложение → "<id>.<ext>" в том же плоском каталоге. Возвращает пустое имя
    // при ошибке чтения; про деградацию (нет кодека) отчитывается само.
    const auto internAttachment = [&](const QString& targetAbs) -> QString {
        const auto cached = attachmentByAbs.find(targetAbs);
        if (cached != attachmentByAbs.end()) return cached->second;

        QFile src(targetAbs);
        if (!src.open(QIODevice::ReadOnly)) return {};
        const QByteArray sourceBytes = src.readAll();
        src.close();
        const QByteArray key =
            QCryptographicHash::hash(sourceBytes, QCryptographicHash::Sha256);
        const auto known = attachmentByHash.find(key);
        if (known != attachmentByHash.end()) {
            attachmentByAbs[targetAbs] = known->second;
            return known->second;
        }

        const Converted converted = convertAttachment(targetAbs, temp.path());
        if (converted.bytes.isEmpty()) return {};
        if (converted.degraded)
            report.problem(QStringLiteral("кодека не нашлось, скопировано как есть: %1")
                               .arg(QDir(srcRoot).relativeFilePath(targetAbs)));

        std::string id;
        do {
            id = makeNoteId(
                std::uint64_t(QFileInfo(targetAbs).lastModified().toSecsSinceEpoch()),
                randomPart());
        } while (!takenIds.insert(id).second);
        const QString name = fromUtf8(id) + QLatin1Char('.') + converted.ext;

        if (!options.dryRun) {
            QFile out(root + QLatin1Char('/') + name);
            if (!out.open(QIODevice::WriteOnly) ||
                out.write(converted.bytes) != converted.bytes.size()) {
                report.problem(QStringLiteral("вложение не записалось: %1").arg(name));
                return {};
            }
        }
        attachmentByHash[key] = name;
        attachmentByAbs[targetAbs] = name;
        attachmentSizes[name] = converted.bytes.size();
        report.note(QStringLiteral("%1 → %2")
                        .arg(QDir(srcRoot).relativeFilePath(targetAbs), name));
        return name;
    };

    // Вики-вложение строкой абзаца: "![[путь|W]]", "[[путь]]" и родня.
    // Работает построчно, как Ctrl+/ в редакторе: строка-вложение
    // выкраивается из блока в свой блок-картинку (подпись — родное имя,
    // ширина — в "#w="), соседние строки остаются абзацем, уровень
    // наследуется. Прочие wikilinks не трогаются, только считаются.
    const auto wikiTarget = [&](const QString& line, const QString& noteDirRel,
                                QString* targetAbs, QString* width,
                                QString* alt) -> bool {
        QString text = line.trimmed();
        if (text.startsWith(QLatin1Char('!'))) text = text.mid(1);
        if (!text.startsWith(QStringLiteral("[[")) || !text.endsWith(QStringLiteral("]]")))
            return false;
        QString inner = text.mid(2, text.size() - 4);
        const qsizetype bar = inner.lastIndexOf(QLatin1Char('|'));
        if (bar >= 0) {
            bool ok = false;
            const int w = inner.mid(bar + 1).trimmed().toInt(&ok);
            if (ok && w > 0) *width = QString::number(w);
            inner = inner.left(bar);
        }
        inner = inner.trimmed();
        if (inner.isEmpty() || inner.endsWith(QStringLiteral(".md"))) return false;
        const QString rel = resolveInside(srcRoot, noteDirRel, inner);
        if (rel.isEmpty()) return false;   // не файл — обычный wikilink
        *targetAbs = srcRoot + QLatin1Char('/') + rel;
        *alt = QFileInfo(inner).completeBaseName();
        return true;
    };

    const auto adoptWikiAttachments = [&](std::vector<Block>& blocks,
                                          const QString& noteDirRel) {
        std::vector<Block> out;
        out.reserve(blocks.size());
        for (Block& b : blocks) {
            const bool candidate = b.rawSource.empty() && b.kind == Kind::Paragraph &&
                                   b.inlines.empty() &&
                                   b.text.find("[[") != std::string::npos;
            if (!candidate) {
                out.push_back(std::move(b));
                continue;
            }
            const QStringList lines = fromUtf8(b.text).split(QLatin1Char('\n'));
            std::vector<Block> pieces;
            QStringList pending;
            const auto flushPending = [&]() {
                if (pending.isEmpty()) return;
                Block piece;
                piece.kind = Kind::Paragraph;
                piece.level = b.level;
                piece.text = toUtf8(pending.join(QLatin1Char('\n')));
                pieces.push_back(std::move(piece));
                pending.clear();
            };
            for (const QString& line : lines) {
                QString targetAbs;
                QString width;
                QString alt;
                if (!wikiTarget(line, noteDirRel, &targetAbs, &width, &alt)) {
                    pending.append(line);
                    continue;
                }
                const QString name = internAttachment(targetAbs);
                if (name.isEmpty()) {
                    report.problem(QStringLiteral("вложение не читается: %1")
                                       .arg(QDir(srcRoot).relativeFilePath(targetAbs)));
                    pending.append(line);
                    continue;
                }
                flushPending();
                Block image;
                image.kind = Kind::Paragraph;
                image.level = b.level;
                image.text = toUtf8(alt);
                Span span;
                span.image = true;
                span.href = toUtf8(
                    width.isEmpty() ? name : name + QStringLiteral("#w=") + width);
                span.offset = 0;
                span.length = int(image.text.size());
                image.inlines.push_back(std::move(span));
                pieces.push_back(std::move(image));
            }
            flushPending();
            if (pieces.size() <= 1 && pending.isEmpty() &&
                (pieces.empty() || pieces[0].inlines.empty())) {
                // Ничего не выкроилось — блок как был.
                out.push_back(std::move(b));
                continue;
            }
            // Куски разделяются пустой строкой: соседство абзаца с абзацем
            // (и картинкой) без неё слиплось бы при перечитывании.
            for (size_t i = 0; i < pieces.size(); ++i) {
                if (i > 0) {
                    Block gap;
                    gap.kind = Kind::VSpace;
                    out.push_back(std::move(gap));
                }
                out.push_back(std::move(pieces[i]));
            }
        }
        blocks = std::move(out);
    };


    for (SrcEntry& e : entries) {
        std::string body;
        Document ir;
        if (e.isDir) {
            // Заметка-каталог: заголовок — имя каталога.
            Block h;
            h.kind = Kind::Heading;
            h.headingLevel = 1;
            h.text = toUtf8(e.title);
            ir.blocks.push_back(std::move(h));
        } else {
            std::string bytes;
            if (!readAll(e.abs, bytes)) continue;   // уже в отчёте
            ir = parse(bytes);
        }

        // Заголовок заметки: Apple держит его первой строкой, конвертер унёс
        // в имя файла, а имя файла становится непрозрачным id — без возврата
        // заголовка в тело заметка осталась бы безымянной. Заголовок получают
        // ВСЕ заметки; не трогаем только те, где первый содержательный блок —
        // заголовок ровно с тем же текстом (правило владельца).
        if (!e.isDir) {
            QString firstHeading;
            for (const Block& b : ir.blocks) {
                if (b.rawSource.empty() && b.kind == Kind::VSpace) continue;
                if (b.rawSource.empty() && b.kind == Kind::Heading)
                    firstHeading = fromUtf8(b.text).trimmed();
                break;
            }
            const QString title = e.displayTitle.trimmed();
            if (!title.isEmpty() && firstHeading != title) {
                Block heading;
                heading.kind = Kind::Heading;
                heading.headingLevel = 1;
                heading.text = toUtf8(title);
                std::vector<Block> withTitle;
                withTitle.push_back(std::move(heading));
                if (!ir.blocks.empty()) {
                    Block gap;
                    gap.kind = Kind::VSpace;
                    withTitle.push_back(std::move(gap));
                }
                withTitle.insert(withTitle.end(),
                                 std::make_move_iterator(ir.blocks.begin()),
                                 std::make_move_iterator(ir.blocks.end()));
                ir.blocks = std::move(withTitle);
            }
        }

        // Вложения и ссылки. Вики-вложения усыновляются в канон, прочие
        // wikilinks не переписываются — только счёт.
        const QString noteDirRel = e.parentRel;
        adoptWikiAttachments(ir.blocks, noteDirRel);
        for (Block& b : ir.blocks) {
            if (!b.rawSource.empty()) {
                if (b.rawSource.find("[[") != std::string::npos) ++wikilinks;
                continue;
            }
            if (b.text.find("[[") != std::string::npos) ++wikilinks;
            for (Span& s : b.inlines) {
                if (s.href.empty()) continue;
                QString href = fromUtf8(s.href);
                // Фрагмент (#w=300) — часть нашего канона, не путь.
                QString fragment;
                const qsizetype hash = href.lastIndexOf(QLatin1Char('#'));
                if (hash >= 0) {
                    fragment = href.mid(hash);
                    href = href.left(hash);
                }
                if (!isLocalRelative(href)) continue;

                if (s.image) {
                    // Уже канонное плоское имя — вложение усыновлено выше.
                    const qsizetype dot = href.lastIndexOf(QLatin1Char('.'));
                    if (dot > 0 && !href.contains(QLatin1Char('/')) &&
                        isValidNoteId(toUtf8(href.left(dot))))
                        continue;
                    const QString targetRel = resolveInside(srcRoot, noteDirRel, href);
                    if (targetRel.isEmpty()) {
                        report.problem(
                            QStringLiteral("вложение не найдено: «%1» в %2")
                                .arg(href, e.rel));
                        continue;
                    }
                    const QString name =
                        internAttachment(srcRoot + QLatin1Char('/') + targetRel);
                    if (name.isEmpty()) {
                        report.problem(
                            QStringLiteral("вложение не читается: %1").arg(targetRel));
                        continue;
                    }
                    s.href = toUtf8(name + fragment);
                } else if (href.endsWith(QStringLiteral(".md"))) {
                    const QString joined = noteDirRel.isEmpty()
                                               ? href
                                               : noteDirRel + QLatin1Char('/') + href;
                    const QString cleaned = QDir::cleanPath(joined);
                    const auto found = byRel.find(cleaned);
                    if (found == byRel.end()) {
                        report.note(QStringLiteral("ссылка не разрешилась: «%1» в %2")
                                        .arg(href, e.rel));
                        continue;
                    }
                    s.href = entries[found->second].id + ".md";
                }
            }
        }

        // Метаданные: существующие (при реимпорте) уважаются, наши ключи поверх.
        // Пустая строка после "-->" положена перед контентом; пустой заметке
        // она дала бы дрейф (замерено на «Вещи из Китая.md» — пустом файле).
        ir.meta.present = true;
        ir.meta.blankAfter = !ir.blocks.empty();
        if (!e.parentRel.isEmpty()) ir.meta.set("parent", dirIds[e.parentRel]);
        ir.meta.set("created", toUtf8(isoUtc(e.created)));
        ir.meta.set("modified", toUtf8(isoUtc(e.modified)));

        body = serialize(ir);

        if (!options.dryRun) {
            // O_EXCL с целевым id; коллизия на диске невозможна (id уникальны
            // в партии, хранилище свежее), но честность дешева.
            const std::string want = e.id;
            const QDateTime created = e.created;
            int extra = 0;
            const auto generator = [&want, &created, &extra]() {
                if (extra++ == 0) return want;
                return makeNoteId(std::uint64_t(created.toSecsSinceEpoch()), randomPart());
            };
            std::string path;
            const std::string got = createNoteFile(toUtf8(root), body, &path, generator);
            if (got.empty()) {
                report.problem(QStringLiteral("не записалась заметка для %1").arg(e.rel));
                continue;
            }
            e.id = got;
        }
        report.note(QStringLiteral("%1 → %2%3")
                        .arg(e.rel, fromUtf8(e.id),
                             e.isDir ? QStringLiteral(" (каталог)") : QString()));
    }

    // Сводка вложений: по расширениям, число и байты — по ней будет
    // настраиваться пережатие.
    if (!attachmentSizes.empty()) {
        std::map<QString, std::pair<int, qint64>> byExt;
        for (const auto& [name, size] : attachmentSizes) {
            const QString ext = QFileInfo(name).suffix();
            byExt[ext].first++;
            byExt[ext].second += size;
        }
        for (const auto& [ext, stat] : byExt)
            report.note(QStringLiteral("вложения .%1: %2 шт, %3 байт")
                            .arg(ext)
                            .arg(stat.first)
                            .arg(stat.second));
    }
    if (wikilinks > 0)
        report.note(QStringLiteral("wikilink-блоков не переписано: %1").arg(wikilinks));

    // Отчёт соответствия — рядом с хранилищем, не внутри.
    if (!options.dryRun) {
        QFile out(root + QStringLiteral(".import-report.txt"));
        if (out.open(QIODevice::WriteOnly)) {
            for (const QString& line : report.lines)
                out.write((line + QLatin1Char('\n')).toUtf8());
        } else {
            report.problem(QStringLiteral("отчёт не записался: %1").arg(out.fileName()));
        }
    }
    return report.problems == 0;
}

bool verifyStore(const QString& root, Report& report) {
    QDir d(root);
    if (!d.exists()) {
        report.problem(QStringLiteral("нет хранилища: %1").arg(root));
        return false;
    }

    std::map<std::string, Document> notes;
    std::set<QString> attachments;         // имена файлов-вложений
    std::set<QString> referenced;

    for (const QFileInfo& info :
         d.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden)) {
        const QString name = info.fileName();
        if (info.isDir()) {
            // Служебные каталоги прозрачны: состояние, побитые файлы
            // редактора, история заметок.
            if (name == QStringLiteral(".zametti") || name == QStringLiteral(".rescue") ||
                name == QStringLiteral("history"))
                continue;
            report.problem(QStringLiteral("чужой каталог: %1").arg(name));
            continue;
        }
        const qsizetype dot = name.lastIndexOf(QLatin1Char('.'));
        const QString stem = dot > 0 ? name.left(dot) : name;
        if (dot <= 0 || !isValidNoteId(toUtf8(stem))) {
            report.problem(QStringLiteral("чужой файл: %1").arg(name));
            continue;
        }
        if (!name.endsWith(QStringLiteral(".md"))) {
            attachments.insert(name);
            continue;
        }

        std::string bytes;
        if (!readAll(info.filePath(), bytes)) {
            report.problem(QStringLiteral("не читается: %1").arg(name));
            continue;
        }
        Document doc = parse(bytes);
        if (!doc.meta.present)
            report.problem(QStringLiteral("нет блока метаданных: %1").arg(name));
        if (serialize(doc) != bytes)
            report.problem(QStringLiteral("дрейф: %1").arg(name));
        notes[toUtf8(stem)] = std::move(doc);
    }

    // Цели картинок: канонное плоское имя "<id>.<ext>" и существование.
    for (const auto& [id, doc] : notes) {
        for (const Block& b : doc.blocks)
            for (const Span& span : b.inlines) {
                if (!span.image) continue;
                QString href = fromUtf8(span.href);
                const qsizetype hash = href.lastIndexOf(QLatin1Char('#'));
                if (hash >= 0) href = href.left(hash);
                if (!isLocalRelative(href)) continue;
                const qsizetype dot = href.lastIndexOf(QLatin1Char('.'));
                const bool canonical =
                    dot > 0 && !href.contains(QLatin1Char('/')) &&
                    isValidNoteId(toUtf8(href.left(dot)));
                if (!canonical) {
                    report.note(QStringLiteral("картинка мимо канона имён: «%1» в %2.md")
                                    .arg(href, fromUtf8(id)));
                    continue;
                }
                referenced.insert(href);
                if (!QFileInfo::exists(root + QLatin1Char('/') + href))
                    report.problem(QStringLiteral("нет вложения «%1» из %2.md")
                                       .arg(href, fromUtf8(id)));
            }
    }

    // parent: существование и циклы.
    for (const auto& [id, doc] : notes) {
        const std::string parent = doc.meta.get("parent");
        if (parent.empty()) continue;
        if (!isValidNoteId(parent) || notes.find(parent) == notes.end()) {
            report.problem(QStringLiteral("parent %1 не существует (из %2)")
                               .arg(fromUtf8(parent), fromUtf8(id)));
            continue;
        }
        std::set<std::string> seen{id};
        std::string at = parent;
        while (!at.empty()) {
            if (!seen.insert(at).second) {
                report.problem(QStringLiteral("цикл родителей через %1").arg(fromUtf8(id)));
                break;
            }
            const auto next = notes.find(at);
            if (next == notes.end()) break;
            at = next->second.meta.get("parent");
        }
    }

    for (const QString& name : attachments)
        if (referenced.find(name) == referenced.end())
            report.note(QStringLiteral("осиротевшее вложение: %1").arg(name));

    report.note(QStringLiteral("заметок: %1, вложений: %2")
                    .arg(notes.size())
                    .arg(attachments.size()));
    return report.problems == 0;
}

}  // namespace zametti::store
