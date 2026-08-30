// ZStorage::importTree — ввоз дерева чужих .md (zametti store import).
//
// Отдельный файл: это единственная часть хранилища, которая зовёт внешние
// утилиты (cwebp, heif-convert, exiftool) и знает про манифест Apple Notes и
// вики-вложения Obsidian. Правило безопасности: импорт НИКОГДА не пишет в
// источник — новое хранилище создаётся рядом, старое дерево остаётся эталоном.

#include "zstorage.h"
#include "zsystem.h"

#include "times.h"

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
#include <QTemporaryDir>
#include <QTimeZone>

#include <algorithm>
#include <iterator>
#include <map>
#include <random>
#include <set>
#include <string>
#include <vector>

namespace zametti {
namespace {

QString fromUtf8(const std::string& s) {
    return QString::fromUtf8(s.data(), qsizetype(s.size()));
}

std::string toUtf8(const QString& s) {
    const QByteArray b = s.toUtf8();
    return std::string(b.constData(), size_t(b.size()));
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
        const QDateTime value = store::parseNoteTime(line.mid(colon + 1).trimmed());
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
// Дверь наружу одна на весь проект — ZSystem, см. её шапку.
bool runTool(const QString& program, const QStringList& args) {
    return ZSystem::runTool(program, args);
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

bool ZStorage::importTree(const ImportOptions& options, Report& report) {
    const QString srcRoot = QDir(options.from).absolutePath();
    if (!QDir(srcRoot).exists()) {
        report.problem(QStringLiteral("no source: %1").arg(options.from));
        return false;
    }
    const QString root = QDir(root_).absolutePath();

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
        if (!readFileBytes(options.appleManifest, bytes)) {
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
            m.created = store::parseNoteTime(o.value(QStringLiteral("created")).toString());
            m.modified = store::parseNoteTime(o.value(QStringLiteral("modified")).toString());
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
        if (!e.isDir && !readFileBytes(e.abs, bytes)) {
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
        if (!init(&initError)) {
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
            if (!readFileBytes(e.abs, bytes)) continue;   // уже в отчёте
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
        meta.set("created", toUtf8(store::isoWithOffset(e.created)));
        meta.set("modified", toUtf8(store::isoWithOffset(e.modified)));
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

}  // namespace zametti
