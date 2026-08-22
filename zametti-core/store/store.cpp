#include "store.h"

#include "times.h"

#include "journal.h"

#include "note_id.h"
#include "document.h"
#include "znote.h"
#include "document_pieces.h"
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

// Времена шапки живут одним модулем (store/times.h): запись — ISO-8601 с
// офсетом, чтение — оба вида. Здесь остались только имена покороче.
QString isoUtc(const QDateTime& t) { return isoWithOffset(t); }
QDateTime parseIso(const QString& value) { return parseNoteTime(value); }

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
                *error = QStringLiteral("directory not empty: %1").arg(dir);
            return false;
        }
    } else if (!QDir().mkpath(dir)) {
        if (error != nullptr) *error = QStringLiteral("cannot create: %1").arg(dir);
        return false;
    }
    // .zametti — состояние; .rescue — побитые файлы редактора (с точкой: на
    // сервер не синхронизируется); history — история заметок (синхронизируется).
    for (const char* sub : {".zametti", ".rescue", "history"}) {
        if (!QDir(dir).mkpath(QString::fromLatin1(sub))) {
            if (error != nullptr)
                *error = QStringLiteral("cannot create: %1/%2").arg(dir, sub);
            return false;
        }
    }
    // Проверка прав — делом: пробный файл, а не флаги.
    QFile probe(dir + QStringLiteral("/.zametti/.probe"));
    if (!probe.open(QIODevice::WriteOnly)) {
        if (error != nullptr) *error = QStringLiteral("no write permission: %1").arg(dir);
        return false;
    }
    probe.close();
    probe.remove();
    return true;
}

QString newNote(const QString& root, const QString& parentId, QString* error) {
    if (!QDir(root).exists()) {
        if (error != nullptr) *error = QStringLiteral("no such directory: %1").arg(root);
        return {};
    }
    if (!parentId.isEmpty()) {
        if (!isValidNoteId(toUtf8(parentId))) {
            if (error != nullptr)
                *error = QStringLiteral("parent does not look like an id: %1").arg(parentId);
            return {};
        }
        if (!QFileInfo::exists(root + QLatin1Char('/') + parentId +
                               QStringLiteral(".md"))) {
            if (error != nullptr)
                *error = QStringLiteral("parent is not in the store: %1").arg(parentId);
            return {};
        }
    }

    // Без пустой строки после "-->": она положена перед контентом, а контента
    // в свежей заметке нет — иначе verify честно находил бы дрейф (замерено).
    std::string content = "<!-- zametti\n";
    content += std::string(NoteHeader::kVersionKey) + ": " + NoteHeader::kFormatVersion + "\n";
    if (!parentId.isEmpty()) content += "parent: " + toUtf8(parentId) + "\n";
    content += "created: " + toUtf8(isoNow()) + "\n-->\n";

    std::string path;
    const std::string id = createNoteFile(toUtf8(root), content, &path);
    if (id.empty()) {
        if (error != nullptr) *error = QStringLiteral("could not write into %1").arg(root);
        return {};
    }
    return fromUtf8(path);
}

QString importNote(const QString& root, const QString& parentId, const QString& sourcePath,
                   QString* error) {
    const auto fail = [&](const QString& why) {
        if (error != nullptr) *error = why;
        return QString();
    };
    if (!QDir(root).exists()) return fail(QStringLiteral("no store at: %1").arg(root));
    const QFileInfo info(sourcePath);
    if (!info.isFile()) return fail(QStringLiteral("not a file: %1").arg(sourcePath));
    if (!parentId.isEmpty() &&
        !QFileInfo::exists(root + QLatin1Char('/') + parentId + QStringLiteral(".md")))
        return fail(QStringLiteral("folder is not in the store: %1").arg(parentId));

    std::string bytes;
    if (!readAll(sourcePath, bytes))
        return fail(QStringLiteral("cannot read: %1").arg(sourcePath));

    // Мусорные неразрывные пробелы вычищаются ПРИ ВВОЗЕ, а не при первом
    // открытии: иначе привезённая заметка какое-то время лежала бы на диске
    // грязной, и человек, заглянувший в неё чужим редактором, увидел бы сор.
    ZNote doc;
    doc.load(bytes);

    // Времена. СОЗДАНА заметка тогда, когда её написали: своя шапка знает это
    // лучше файловой системы (файл могли скопировать, и mtime стал бы датой
    // копирования), поэтому created берётся у источника.
    //
    // А вот ПРАВЛЕНА она сейчас, и это не формальность. Средняя колонка по
    // умолчанию отсортирована по дате правки, и заметка, привезённая с чужой
    // датой, уходит в самый низ списка — владелец так и сказал: «непонятно
    // куда она импортируется, я не нашёл в какую папку она попадает».
    // Привезённое ищут среди свежего, потому что привоз и есть событие
    // «сейчас»; хронология источника при этом не теряется — она в created.
    const QDateTime fsModified = info.lastModified();
    const QDateTime fsBirth = info.birthTime();
    QString created = doc.created();
    // Своего created у файла нет — годится и чужая дата правки: заметка точно
    // существовала уже тогда. Это ближе к правде, чем время появления файла на
    // диске, которое у копии равно времени копирования.
    if (created.isEmpty()) created = doc.modified();
    if (created.isEmpty())
        created = isoUtc(fsBirth.isValid() && fsBirth <= fsModified ? fsBirth : fsModified);
    const QString modified = isoNow();

    // id и role чужого файла не наследуются: id принадлежит этому хранилищу
    // (иначе две заметки с одним id), а role сделал бы из заметки папку.
    doc.setHeaderValue(QStringLiteral("id"), QString());
    doc.setHeaderValue(QStringLiteral("role"), QString());
    doc.setParentId(parentId);   // пусто снимает ключ — «в корне»
    doc.setHeaderValue(QStringLiteral("created"), created);
    doc.setHeaderValue(QStringLiteral("modified"), modified);
    doc.setHasHeader(true);
    doc.header().ensureVersion();   // ввезённая — написана нами
    // Пустая строка после "-->" положена перед содержимым; у пустого файла
    // содержимого нет, и она дала бы дрейф.
    doc.header().setBlankAfter(!doc.doc().isEmpty());

    const std::string content = doc.toMarkdown();
    // Последний рубеж, тот же, что и у сохранения: записанное обязано читаться
    // обратно в себя. Ядро это гарантирует, но файл пришёл снаружи.
    {
        ZNote back;
        back.load(content);
        if (back.toMarkdown() != content)
            return fail(QStringLiteral("canonical form mismatch on %1").arg(info.fileName()));
    }

    std::string path;
    if (createNoteFile(toUtf8(root), content, &path).empty())
        return fail(QStringLiteral("could not write into %1").arg(root));
    return fromUtf8(path);
}

bool importTree(const ImportOptions& options, Report& report) {
    const QString srcRoot = QDir(options.from).absolutePath();
    if (!QDir(srcRoot).exists()) {
        report.problem(QStringLiteral("no source: %1").arg(options.from));
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
                report.note(QStringLiteral("hidden entry skipped: %1")
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
            report.problem(QStringLiteral("cannot read manifest: %1")
                               .arg(options.appleManifest));
            return false;
        }
        QJsonParseError parseError{};
        const QJsonDocument doc = QJsonDocument::fromJson(
            QByteArray(bytes.data(), qsizetype(bytes.size())), &parseError);
        if (!doc.isArray()) {
            report.problem(QStringLiteral("manifest is not a JSON array: %1")
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
                QStringLiteral("manifest is ambiguous for '%1': %2 entries")
                    .arg(e.rel)
                    .arg(matches));
            matched = nullptr;
        }
        std::string bytes;
        if (!e.isDir && !readAll(e.abs, bytes)) {
            report.problem(QStringLiteral("cannot read: %1").arg(e.rel));
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
            report.note(QStringLiteral("front matter in '%1': times taken, "
                                       "header left as is")
                            .arg(e.rel));
        } else {
            const QDateTime birth = info.birthTime();
            modified = info.lastModified();
            created = birth.isValid() && birth <= modified ? birth : modified;
            e.timesFrom = QStringLiteral("fs");
            report.note(QStringLiteral("times from the file system: %1").arg(e.rel));
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
            report.problem(QStringLiteral("manifest entry without a file: '%1' / '%2'")
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
            report.problem(QStringLiteral("no codec found, copied as is: %1")
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
                report.problem(QStringLiteral("attachment not written: %1").arg(name));
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

    const auto adoptWikiAttachments = [&](std::vector<Piece>& ir, const QString& noteDirRel) {
        std::vector<Piece> out;
        out.reserve(ir.size());
        for (size_t at = 0; at < ir.size(); ++at) {
            const Piece& b = ir[at];
            const bool candidate = !b.raw && b.kind == Kind::Paragraph && b.runs.empty() &&
                                   b.text.contains(QLatin1String("[["));
            if (!candidate) {
                out.push_back(b);
                continue;
            }
            const QStringList lines = b.text.split(QLatin1Char('\n'));
            std::vector<Piece> pieces;
            QStringList pending;
            const auto flushPending = [&]() {
                if (pending.isEmpty()) return;
                Piece piece;
                piece.text = pending.join(QLatin1Char('\n'));
                piece.level = b.level;
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
                    report.problem(QStringLiteral("cannot read attachment: %1")
                                       .arg(QDir(srcRoot).relativeFilePath(targetAbs)));
                    pending.append(line);
                    continue;
                }
                flushPending();
                Piece image;
                image.text = alt;
                image.level = b.level;
                Run span;
                span.set(InlineImage, true);
                span.href = width.isEmpty() ? name : name + QStringLiteral("#w=") + width;
                span.start = 0;
                span.end = int32_t(image.text.size());
                image.runs.push_back(std::move(span));
                pieces.push_back(std::move(image));
            }
            flushPending();
            if (pieces.size() <= 1 && pending.isEmpty() &&
                (pieces.empty() || pieces[0].runs.empty())) {
                // Ничего не выкроилось — блок как был.
                out.push_back(b);
                continue;
            }
            // Куски разделяются пустой строкой: соседство абзаца с абзацем
            // (и картинкой) без неё слиплось бы при перечитывании.
            for (size_t i = 0; i < pieces.size(); ++i) {
                if (i > 0) {
                    Piece gap;
                    gap.kind = Kind::VSpace;
                    out.push_back(std::move(gap));
                }
                out.push_back(std::move(pieces[i]));
            }
        }
        ir = std::move(out);
    };


    for (SrcEntry& e : entries) {
        std::string body;
        std::vector<Piece> ir;
        NoteHeader meta;
        if (e.isDir) {
            // Заметка-каталог: заголовок — имя каталога, признак — в мете
            // (правило владельца: у любой директории, пустой или нет).
            Piece h;
            h.kind = Kind::Heading;
            h.headingLevel = 1;
            h.text = e.title;
            ir.push_back(std::move(h));
            meta.set("role", "folder");
        } else {
            std::string bytes;
            if (!readAll(e.abs, bytes)) continue;   // уже в отчёте
            // Граница файла: байты → текст, один раз.
            parsePieces(normaliseSpaces(QString::fromUtf8(bytes.data(), qsizetype(bytes.size()))),
                        ir, meta);
        }

        // Заголовок заметки: Apple держит его первой строкой, конвертер унёс
        // в имя файла, а имя файла становится непрозрачным id — без возврата
        // заголовка в тело заметка осталась бы безымянной. Заголовок получают
        // ВСЕ заметки; не трогаем только те, где первый содержательный блок —
        // заголовок ровно с тем же текстом (правило владельца).
        if (!e.isDir) {
            QString firstHeading;
            for (const Piece& b : ir) {
                if (!b.raw && b.kind == Kind::VSpace) continue;
                if (!b.raw && b.kind == Kind::Heading) firstHeading = b.text.trimmed();
                break;
            }
            const QString title = e.displayTitle.trimmed();
            if (!title.isEmpty() && firstHeading != title) {
                Piece heading;
                heading.kind = Kind::Heading;
                heading.headingLevel = 1;
                heading.text = title;
                std::vector<Piece> withTitle;
                withTitle.push_back(std::move(heading));
                if (!ir.empty()) {
                    Piece gap;
                    gap.kind = Kind::VSpace;
                    withTitle.push_back(std::move(gap));
                }
                withTitle.insert(withTitle.end(), std::make_move_iterator(ir.begin()),
                                 std::make_move_iterator(ir.end()));
                ir = std::move(withTitle);
            }
        }

        // Вложения и ссылки. Вики-вложения усыновляются в канон, прочие
        // wikilinks не переписываются — только счёт.
        const QString noteDirRel = e.parentRel;
        adoptWikiAttachments(ir, noteDirRel);
        for (Piece& b : ir) {
            if (b.raw) {
                if (b.text.contains(QLatin1String("[["))) ++wikilinks;
                continue;
            }
            if (b.text.contains(QLatin1String("[["))) ++wikilinks;
            for (Run& s : b.runs) {
                if (s.href.isEmpty()) continue;
                QString href = s.href;
                // Фрагмент (#w=300) — часть нашего канона, не путь.
                QString fragment;
                const qsizetype hash = href.lastIndexOf(QLatin1Char('#'));
                if (hash >= 0) {
                    fragment = href.mid(hash);
                    href = href.left(hash);
                }
                if (!isLocalRelative(href)) continue;

                if (s.image()) {
                    // Уже канонное плоское имя — вложение усыновлено выше.
                    const qsizetype dot = href.lastIndexOf(QLatin1Char('.'));
                    if (dot > 0 && !href.contains(QLatin1Char('/')) &&
                        isValidNoteId(toUtf8(href.left(dot))))
                        continue;
                    const QString targetRel = resolveInside(srcRoot, noteDirRel, href);
                    if (targetRel.isEmpty()) {
                        report.problem(
                            QStringLiteral("attachment not found: '%1' in %2")
                                .arg(href, e.rel));
                        continue;
                    }
                    const QString name =
                        internAttachment(srcRoot + QLatin1Char('/') + targetRel);
                    if (name.isEmpty()) {
                        report.problem(
                            QStringLiteral("cannot read attachment: %1").arg(targetRel));
                        continue;
                    }
                    s.href = name + fragment;
                } else if (href.endsWith(QStringLiteral(".md"))) {
                    const QString joined = noteDirRel.isEmpty()
                                               ? href
                                               : noteDirRel + QLatin1Char('/') + href;
                    const QString cleaned = QDir::cleanPath(joined);
                    const auto found = byRel.find(cleaned);
                    if (found == byRel.end()) {
                        report.note(QStringLiteral("link did not resolve: '%1' in %2")
                                        .arg(href, e.rel));
                        continue;
                    }
                    s.href = QString::fromStdString(entries[found->second].id + ".md");
                }
            }
        }

        // Метаданные: существующие (при реимпорте) уважаются, наши ключи поверх.
        // Пустая строка после "-->" положена перед контентом; пустой заметке
        // она дала бы дрейф (замерено на «Вещи из Китая.md» — пустом файле).
        meta.setPresent(true);
        meta.setBlankAfter(!ir.empty());
        if (!e.parentRel.isEmpty()) meta.set("parent", dirIds[e.parentRel]);
        meta.set("created", toUtf8(isoUtc(e.created)));
        meta.set("modified", toUtf8(isoUtc(e.modified)));
        meta.ensureVersion();

        body = toUtf8(writePieces(ir, meta));

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
                report.problem(QStringLiteral("note for %1 not written").arg(e.rel));
                continue;
            }
            e.id = got;
        }
        report.note(QStringLiteral("%1 → %2%3")
                        .arg(e.rel, fromUtf8(e.id),
                             e.isDir ? QStringLiteral(" (directory)") : QString()));
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
            report.note(QStringLiteral("attachments .%1: %2 files, %3 bytes")
                            .arg(ext)
                            .arg(stat.first)
                            .arg(stat.second));
    }
    if (wikilinks > 0)
        report.note(QStringLiteral("wikilink blocks not rewritten: %1").arg(wikilinks));

    // Отчёт соответствия — рядом с хранилищем, не внутри.
    if (!options.dryRun) {
        QFile out(root + QStringLiteral(".import-report.txt"));
        if (out.open(QIODevice::WriteOnly)) {
            for (const QString& line : report.lines)
                out.write((line + QLatin1Char('\n')).toUtf8());
        } else {
            report.problem(QStringLiteral("report not written: %1").arg(out.fileName()));
        }
    }
    return report.problems == 0;
}

bool verifyStore(const QString& root, Report& report) {
    QDir d(root);
    if (!d.exists()) {
        report.problem(QStringLiteral("no store at: %1").arg(root));
        return false;
    }

    std::map<std::string, std::shared_ptr<ZNote>> notes;
    std::set<QString> attachments;         // имена файлов-вложений
    // ДВА множества, а не одно. Доктрина этапа 10: вложение живо, пока на него
    // ссылается хоть одна ЗАМЕТКА — живая или корзинная; упомянутое только из
    // корзины не аномалия, а «уйдёт при очистке». Слепки истории на живость
    // вложения больше не влияют вовсе (см. ниже).
    std::set<QString> referencedLive;
    std::set<QString> referencedTrashed;

    for (const QFileInfo& info :
         d.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden)) {
        const QString name = info.fileName();
        if (info.isDir()) {
            // Служебные каталоги прозрачны: состояние, побитые файлы
            // редактора, история заметок.
            if (name == QStringLiteral(".zametti") || name == QStringLiteral(".rescue") ||
                name == QStringLiteral("history"))
                continue;
            report.problem(QStringLiteral("foreign directory: %1").arg(name));
            continue;
        }
        const qsizetype dot = name.lastIndexOf(QLatin1Char('.'));
        const QString stem = dot > 0 ? name.left(dot) : name;
        if (dot <= 0 || !isValidNoteId(toUtf8(stem))) {
            report.problem(QStringLiteral("foreign file: %1").arg(name));
            continue;
        }
        if (!name.endsWith(QStringLiteral(".md"))) {
            attachments.insert(name);
            continue;
        }

        std::string bytes;
        if (!readAll(info.filePath(), bytes)) {
            report.problem(QStringLiteral("cannot read: %1").arg(name));
            continue;
        }
        // Мусорные неразрывные пробелы вычищаются ПРИ ВВОЗЕ, а не при первом
    // открытии: иначе привезённая заметка какое-то время лежала бы на диске
    // грязной, и человек, заглянувший в неё чужим редактором, увидел бы сор.
        auto note = std::make_shared<ZNote>();
        note->load(bytes);
        if (!note->hasHeader())
            report.problem(QStringLiteral("no metadata block: %1").arg(name));
        if (!note->isCanonical(bytes)) report.problem(QStringLiteral("drift: %1").arg(name));
        notes[toUtf8(stem)] = std::move(note);
    }

    // Лежит ли заметка в корзине: идём по цепочке родителей до заметки с
    // role: trash. Циклы уже проверены ниже, но на всякий случай ограничиваем
    // глубину — verify не имеет права зациклиться на битом хранилище.
    const auto inTrash = [&notes](const std::string& id) {
        std::string at = id;
        for (int depth = 0; depth < 64; ++depth) {
            const auto found = notes.find(at);
            if (found == notes.end()) return false;
            if (found->second->role() == QLatin1String("trash")) return true;
            const std::string parent = toUtf8(found->second->parentId());
            if (parent.empty()) return false;
            at = parent;
        }
        return false;
    };

    // Цели картинок: канонное плоское имя "<id>.<ext>" и существование.
    for (const auto& [id, note] : notes) {
        for (const Attachment& image : note->doc().attachments()) {
            const QString href = image.id;
            if (!isLocalRelative(href)) continue;
            const qsizetype dot = href.lastIndexOf(QLatin1Char('.'));
            const bool canonical = dot > 0 && !href.contains(QLatin1Char('/')) &&
                                   isValidNoteId(toUtf8(href.left(dot)));
            if (!canonical) {
                report.note(QStringLiteral("image name off canonical form: '%1' in %2.md")
                                .arg(href, fromUtf8(id)));
                continue;
            }
            if (inTrash(id)) referencedTrashed.insert(href);
            else referencedLive.insert(href);
            if (!QFileInfo::exists(root + QLatin1Char('/') + href))
                report.problem(QStringLiteral("missing attachment '%1' from %2.md")
                                   .arg(href, fromUtf8(id)));
        }
    }

    // Заметка-папка (и сама корзина) — структура, а не текст: в её файле
    // положено быть шапке и ровно одному заголовку, и больше ничему. Редактор
    // такой файл не открывает вовсе, так что тело в нём может завестись только
    // снаружи — и увидеть его будет негде: список показывает содержимое папки,
    // а не её саму.
    for (const auto& [id, note] : notes) {
        const QString role = note->role();
        if (role != QLatin1String("folder") && role != QLatin1String("trash")) continue;
        const ZDocument& doc = note->doc();
        if (doc.isEmpty()) {
            report.problem(QStringLiteral("folder without a title: %1.md").arg(fromUtf8(id)));
            continue;
        }
        const BlockInfo first = doc.blockAt(0);
        if (doc.blockCount() == 1 && !first.raw && first.kind == Kind::Heading) continue;
        report.problem(QStringLiteral("folder %1.md has body beyond the title (%2 blocks)")
                           .arg(fromUtf8(id))
                           .arg(doc.blockCount()));
    }

    // parent: существование и циклы.
    for (const auto& [id, note] : notes) {
        const std::string parent = toUtf8(note->parentId());
        if (parent.empty()) continue;
        if (!isValidNoteId(parent) || notes.find(parent) == notes.end()) {
            report.problem(QStringLiteral("parent %1 does not exist (from %2)")
                               .arg(fromUtf8(parent), fromUtf8(id)));
            continue;
        }
        std::set<std::string> seen{id};
        std::string at = parent;
        while (!at.empty()) {
            if (!seen.insert(at).second) {
                report.problem(QStringLiteral("parent cycle through %1").arg(fromUtf8(id)));
                break;
            }
            const auto next = notes.find(at);
            if (next == notes.end()) break;
            at = toUtf8(next->second->parentId());
        }
    }

    // --- журналы ------------------------------------------------------------
    //
    // Журнал — не кэш: он не восстановим из файлов и однажды поедет в синк.
    // Поэтому проверяется он всерьёз: рамки записей и версия формата (их
    // разбирает сам читатель), а сверх того — что КАЖДЫЙ слепок собирается и
    // сходится со своим отпечатком. Собрать значит и распаковать по кодеку
    // записи, и пройти всю цепочку её поколения: у звена нет смысла в отрыве
    // от предшественника.
    //
    // ВЛОЖЕНИЯ ОТСЮДА БОЛЬШЕ НЕ БЕРУТСЯ. До этапа 10 картинка считалась живой,
    // пока её видел хоть один слепок истории; с этапа 10 доктрина другая
    // (решение владельца): удаление радикально, предохранителей три (корзина →
    // очистка корзины → мусорка ОС), а слепок со ссылкой на исчезнувшее
    // деградирует штатной рамкой «файл не найден». Иначе ни одна картинка не
    // ушла бы из хранилища никогда: её видит прошлое.
    //
    // Поэтому битая ссылка ИЗ СЛЕПКА — норма и в отчёт не идёт вовсе. Слепки
    // по-прежнему собираются и сверяются с отпечатками: это про целость
    // журнала, а не про картинки.
    const QDir historyDir(d.filePath(QStringLiteral("history")));
    int journals = 0;
    qint64 records = 0;
    if (historyDir.exists()) {
        journal::History history(root);
        for (const QString& name :
             historyDir.entryList({QStringLiteral("*.log")}, QDir::Files)) {
            const QString noteId = name.left(name.size() - 4);
            if (!isValidNoteId(toUtf8(noteId))) {
                report.problem(QStringLiteral("foreign file in history/: %1").arg(name));
                continue;
            }
            journal::ZJournal j;
            QString error;
            if (!history.read(noteId, &j, &error)) {
                report.problem(QStringLiteral("journal %1: %2").arg(name, error));
                continue;
            }
            ++journals;
            records += j.size();
            // Испорченная рамка — беда, а не примечание: роду, времени и
            // ревизии такой записи верить нельзя, и молчать об этом нельзя.
            if (j.damagedCount() > 0)
                report.problem(QStringLiteral("journal %1: %2 record(s) with a broken frame "
                                              "checksum")
                                   .arg(name)
                                   .arg(j.damagedCount()));
            if (j.tailTrimmed())
                report.note(QStringLiteral("journal %1: truncated tail "
                                           "(will be cut on the next append)")
                                .arg(name));

            for (int i = 0; i < j.size(); ++i) {
                if (!j.at(i).hasSnapshot()) continue;
                QByteArray body;
                if (!history.snapshotAt(noteId, i, &body, &error)) {
                    report.problem(QStringLiteral("journal %1, record %2: %3")
                                       .arg(name)
                                       .arg(i)
                                       .arg(error));
                    continue;
                }
                // Слепок собрался и сошёлся с отпечатком — этого и добивались.
                // Ссылки на картинки в нём не читаются: см. выше про доктрину.
            }

            // Журнал без заметки. Надгробие — норма: заметку удалили, и её
            // история осталась намеренно, по ней её и воскрешают. Нет
            // надгробия — заметку унесли мимо программы, и сказать об этом
            // надо, но бедой это не считаем: файл мог убрать сам человек.
            if (notes.find(toUtf8(noteId)) != notes.end()) continue;
            // Похоронена ли — спрашиваем у ГОЛОВЫ, а не у последней по файлу:
            // диагноз обязан считаться по тому же порядку, по которому
            // программа выбирает состояние заметки.
            const int head = j.headIndex();
            const bool buried = head >= 0 && j.at(head).kind() == journal::Kind::Tombstone;
            report.note(buried ? QStringLiteral("journal of deleted note %1 (with tombstone)")
                                     .arg(noteId)
                               : QStringLiteral("journal %1 without a note and without a tombstone: "
                                                "the file was taken outside the app")
                                     .arg(noteId));
        }
    }

    // ТРИ КАТЕГОРИИ ВЛОЖЕНИЙ, и разница между ними — не косметика:
    //   живое      — на него ссылается хоть одна незакорзиненная заметка;
    //   «в корзине» — только корзинные; уйдёт вместе с очисткой корзины, и это
    //                 не аномалия, а расписание;
    //   сирота     — не упомянуто НИ В ОДНОЙ заметке вовсе. Вот это отчёт.
    for (const QString& name : attachments) {
        if (referencedLive.find(name) != referencedLive.end()) continue;
        if (referencedTrashed.find(name) != referencedTrashed.end()) {
            report.note(QStringLiteral("attachment only in archive: %1 (will go with cleanup)")
                            .arg(name));
            continue;
        }
        report.note(QStringLiteral("orphan attachment: %1").arg(name));
    }

    report.note(QStringLiteral("notes: %1, attachments: %2, journals: %3 (records %4)")
                    .arg(notes.size())
                    .arg(attachments.size())
                    .arg(journals)
                    .arg(records));
    return report.problems == 0;
}

QStringList attachmentsLeavingWith(const QString& root, const QStringList& noteIds) {
    const QDir dir(root);
    const QSet<QString> doomed(noteIds.begin(), noteIds.end());

    // Все вложения хранилища: всё, что не .md и носит наш id.
    QStringList attachments;
    for (const QString& name :
         dir.entryList(QDir::Files | QDir::NoDotAndDotDot | QDir::Hidden)) {
        if (name.endsWith(QStringLiteral(".md"))) continue;
        const qsizetype dot = name.lastIndexOf(QLatin1Char('.'));
        if (dot <= 0 || !isValidNoteId(toUtf8(name.left(dot)))) continue;
        attachments.append(name);
    }
    if (attachments.isEmpty()) return {};

    // Кандидаты — те, что упомянуты в очищаемых заметках. Иначе очистка
    // корзины уносила бы заодно всех сирот, а решение про сирот владелец
    // отложил («дальше придумаем»).
    QSet<QString> candidates;
    for (const QString& id : noteIds) {
        std::string bytes;
        if (!readAll(dir.filePath(id + QStringLiteral(".md")), bytes)) continue;
        const QByteArray text = QByteArray::fromStdString(bytes);
        for (const QString& name : attachments)
            if (!candidates.contains(name) &&
                text.contains(name.left(name.lastIndexOf(QLatin1Char('.'))).toLatin1()))
                candidates.insert(name);
    }
    if (candidates.isEmpty()) return {};

    // И вычёркиваем всё, что упомянуто в ОСТАЮЩИХСЯ заметках. Внешний цикл по
    // заметкам, внутренний по картинкам: каждая заметка читается один раз.
    for (const QString& name : dir.entryList({QStringLiteral("*.md")}, QDir::Files)) {
        const QString id = name.left(name.size() - 3);
        if (doomed.contains(id)) continue;
        std::string bytes;
        if (!readAll(dir.filePath(name), bytes)) continue;
        const QByteArray text = QByteArray::fromStdString(bytes);
        for (auto it = candidates.begin(); it != candidates.end();) {
            const QString stem = it->left(it->lastIndexOf(QLatin1Char('.')));
            if (text.contains(stem.toLatin1())) it = candidates.erase(it);
            else ++it;
        }
        if (candidates.isEmpty()) break;
    }

    QStringList out(candidates.begin(), candidates.end());
    out.sort();
    return out;
}

bool deleteAttachmentFile(const QString& root, const QString& name, QString* error) {
    const QString file = QDir(root).filePath(name);
    if (!QFile::exists(file)) return true;   // уже нет — и хорошо
    if (!QFile::moveToTrash(file) && !QFile::remove(file)) {
        if (error) *error = QStringLiteral("cannot delete attachment file %1").arg(name);
        return false;
    }
    return true;
}

bool deleteNoteFile(const QString& root, const QString& noteId, QString* error) {
    const QString file = QDir(root).filePath(noteId + QStringLiteral(".md"));
    if (!QFile::exists(file)) {
        if (error) *error = QStringLiteral("note %1 is not in the store").arg(noteId);
        return false;
    }

    journal::History history(root);
    QString historyError;
    const bool marked =
        history.append(noteId, journal::NewRecord::tombstone(), &historyError);

    if (!QFile::moveToTrash(file) && !QFile::remove(file)) {
        if (error) *error = QStringLiteral("cannot delete note file %1").arg(noteId);
        return false;
    }
    if (!marked && error) *error = QStringLiteral("tombstone not written: %1").arg(historyError);
    return true;
}

}  // namespace zametti::store
