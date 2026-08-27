// ZStorage: заведение хранилища, рождение и ввоз заметки, проверка, вложения,
// удаление насовсем и воскрешение. Ввоз дерева — в import_tree.cpp, архив и
// миграции — в archive.cpp, бюро находок — в lost_found.cpp.

#include "zstorage.h"

#include "exif.h"
#include "deleted_image.h"
#include "times.h"
#include "journal.h"
#include "note_id.h"
#include "document.h"
#include "znote.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QSet>

#include <map>
#include <set>
#include <string>

namespace zametti {
namespace {

QString fromUtf8(const std::string& s) {
    return QString::fromUtf8(s.data(), qsizetype(s.size()));
}

std::string toUtf8(const QString& s) {
    const QByteArray b = s.toUtf8();
    return std::string(b.constData(), size_t(b.size()));
}

}  // namespace

// --- общие помощники файлов --------------------------------------------------

QString ZStorage::attachmentPath(const QString& name) const {
    return root_ + QLatin1Char('/') + name;
}

bool ZStorage::readFileBytes(const QString& path, std::string& out) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    const QByteArray bytes = f.readAll();
    out.assign(bytes.constData(), size_t(bytes.size()));
    return true;
}

bool ZStorage::writeFileBytes(const QString& path, const std::string& bytes, QString* error) {
    // ПОМЕТКА ДО ЗАПИСИ (write-ahead, m17): упали посреди сохранения —
    // заметка уже в dirty-set и будет выровнена синком одна.
    const QFileInfo target(path);
    if (target.suffix() == QStringLiteral("md")) {
        const QString stem = target.completeBaseName();
        if (isValidNoteId(toUtf8(stem))) markDirty(stem);
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error != nullptr)
            *error = QStringLiteral("cannot open for writing: %1").arg(file.errorString());
        return false;
    }
    file.write(bytes.data(), qint64(bytes.size()));
    if (file.commit()) return true;
    if (error != nullptr) *error = QStringLiteral("write failed: %1").arg(file.errorString());
    return false;
}

bool ZStorage::isLocalRelative(const QString& href) {
    if (href.isEmpty()) return false;
    if (href.startsWith(QLatin1Char('/')) || href.startsWith(QLatin1Char('#'))) return false;
    if (href.contains(QStringLiteral("://"))) return false;
    if (href.startsWith(QStringLiteral("mailto:")) || href.startsWith(QStringLiteral("tel:")))
        return false;
    return true;
}

// --- заведение ---------------------------------------------------------------

// Каркас хранилища В ПУСТОМ ИЛИ НЕСУЩЕСТВУЮЩЕМ каталоге — общий низ init() и
// бутстрапа (connectRemote): каталоги и метка, но НЕ идентичность. Кто её
// заводит, решает вызывающий: init чеканит свою, бутстрап наследует облачную —
// и второй ни в коем случае не должен чеканить (иначе облако станет «чужим»).
bool ZStorage::makeSkeleton(QString* error) {
    QDir d(root_);
    if (d.exists()) {
        const QStringList entries =
            d.entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden);
        if (!entries.isEmpty()) {
            if (error != nullptr)
                *error = QStringLiteral("directory not empty: %1").arg(root_);
            return false;
        }
    } else if (!QDir().mkpath(root_)) {
        if (error != nullptr) *error = QStringLiteral("cannot create: %1").arg(root_);
        return false;
    }
    // .zametti — состояние; .rescue — побитые файлы редактора (с точкой: на
    // сервер не синхронизируется); history — история заметок (синхронизируется).
    for (const char* sub : {".zametti", ".rescue", "history"}) {
        if (!QDir(root_).mkpath(QString::fromLatin1(sub))) {
            if (error != nullptr)
                *error = QStringLiteral("cannot create: %1/%2").arg(root_, sub);
            return false;
        }
    }
    // Проверка прав — делом: пробный файл, а не флаги.
    QFile probe(root_ + QStringLiteral("/.zametti/.probe"));
    if (!probe.open(QIODevice::WriteOnly)) {
        if (error != nullptr) *error = QStringLiteral("no write permission: %1").arg(root_);
        return false;
    }
    probe.close();
    probe.remove();
    // С этого мгновения объект — хранилище: метка на месте.
    store_ = isStoreRoot(root_);
    return true;
}

bool ZStorage::init(QString* error) {
    if (!makeSkeleton(error)) return false;

    // ИДЕНТИЧНОСТЬ — при рождении хранилища, а не при первом синке: id
    // чеканится один раз, и лучший момент для этого тот, когда каталог заведомо
    // наш и пуст.
    QString why;
    if (ensureIdentity(&why).isEmpty()) {
        if (error != nullptr) *error = why;
        return false;
    }
    return true;
}

QString ZStorage::newNoteFile(const QString& parentId, QString* error) {
    if (!QDir(root_).exists()) {
        if (error != nullptr) *error = QStringLiteral("no such directory: %1").arg(root_);
        return {};
    }
    if (!parentId.isEmpty()) {
        if (!isValidNoteId(toUtf8(parentId))) {
            if (error != nullptr)
                *error = QStringLiteral("parent does not look like an id: %1").arg(parentId);
            return {};
        }
        if (!QFileInfo::exists(pathOf(parentId))) {
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
    content += "created: " + toUtf8(store::isoNow()) + "\n-->\n";

    std::string path;
    const std::string id = createNoteFile(toUtf8(root_), content, &path);
    if (id.empty()) {
        if (error != nullptr) *error = QStringLiteral("could not write into %1").arg(root_);
        return {};
    }
    return fromUtf8(path);
}

QString ZStorage::importNote(const QString& parentId, const QString& sourcePath, QString* error) {
    const auto fail = [&](const QString& why) {
        if (error != nullptr) *error = why;
        return QString();
    };
    if (!store_) return fail(QStringLiteral("not a store"));
    const QFileInfo info(sourcePath);
    if (!info.isFile()) return fail(QStringLiteral("not a file: %1").arg(sourcePath));
    if (!parentId.isEmpty() && !QFileInfo::exists(pathOf(parentId)))
        return fail(QStringLiteral("folder is not in the store: %1").arg(parentId));

    std::string bytes;
    if (!readFileBytes(sourcePath, bytes))
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
        created = store::isoWithOffset(fsBirth.isValid() && fsBirth <= fsModified ? fsBirth
                                                                                   : fsModified);
    const QString modified = store::isoNow();

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
    if (createNoteFile(toUtf8(root_), content, &path).empty())
        return fail(QStringLiteral("could not write into %1").arg(root_));
    const QString id = idOfPath(fromUtf8(path));
    refreshNote(id);   // новая — структурная новость сама по себе
    return id;
}

// --- проверка ----------------------------------------------------------------

bool ZStorage::verify(Report& report) {
    QDir d(root_);
    if (!d.exists()) {
        report.problem(QStringLiteral("no store at: %1").arg(root_));
        return false;
    }

    // ИДЕНТИЧНОСТЬ. Файла может не быть — так выглядит хранилище, заведённое
    // прежней сборкой; это не беда, а работа для `zametti store root init`.
    // А вот версия новее нашей — беда: сборка, не знающая половины ключей,
    // перепишет файл без них и потеряет данные молча.
    {
        QString why;
        const Identity identity = this->identity(&why);
        if (!why.isEmpty()) report.problem(why);
        if (identity.isEmpty() && why.isEmpty())
            report.note(QStringLiteral("no %1 yet (run: zametti store root init)")
                            .arg(QLatin1String(Identity::kFile)));
        if (identity.tooNew())
            report.problem(QStringLiteral("%1: format version %2 is newer than mine (%3)")
                               .arg(QLatin1String(Identity::kFile))
                               .arg(identity.formatVersion())
                               .arg(Identity::kFormatVersion));
    }

    std::map<std::string, std::shared_ptr<ZNote>> notes;
    std::set<QString> attachments;         // имена файлов-вложений
    // ДВА множества, а не одно. Доктрина этапа 10: вложение живо, пока на него
    // ссылается хоть одна ЗАМЕТКА — живая или архивная; упомянутое только из
    // архива не аномалия, а «уйдёт при удалении». Слепки истории на живость
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
        // Идентичность хранилища — свой файл, а не заметка: имя у него
        // человеческое, и чужим он не считается.
        if (name == QLatin1String(Identity::kFile)) continue;
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
        if (!readFileBytes(info.filePath(), bytes)) {
            report.problem(QStringLiteral("cannot read: %1").arg(name));
            continue;
        }
        auto note = std::make_shared<ZNote>();
        note->load(bytes);
        if (!note->hasHeader())
            report.problem(QStringLiteral("no metadata block: %1").arg(name));
        if (!note->isCanonical(bytes)) report.problem(QStringLiteral("drift: %1").arg(name));
        notes[toUtf8(stem)] = std::move(note);
    }

    // Лежит ли заметка в корзине прежних версий: идём по цепочке родителей до
    // заметки с role: trash. Циклы уже проверены ниже, но на всякий случай
    // ограничиваем глубину — verify не имеет права зациклиться на битом
    // хранилище.
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
            if (!QFileInfo::exists(attachmentPath(href)))
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
    // (решение владельца): удаление радикально, предохранителей три (архив →
    // удаление из архива → мусорка ОС), а слепок со ссылкой на исчезнувшее
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
        for (const QString& name :
             historyDir.entryList({QStringLiteral("*.zm"), QStringLiteral("*.log")},
                                  QDir::Files)) {
            const QString noteId = name.endsWith(QStringLiteral(".zm"))
                                       ? name.left(name.size() - 3)
                                       : name.left(name.size() - 4);
            if (!isValidNoteId(toUtf8(noteId))) {
                report.problem(QStringLiteral("foreign file in history/: %1").arg(name));
                continue;
            }
            ZJournal j;
            QString error;
            if (!readJournal(noteId, &j, &error)) {
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
                if (!journalSnapshot(noteId, i, &body, &error)) {
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
            const bool buried = head >= 0 && j.at(head).kind() == ZJournal::Kind::Tombstone;
            report.note(buried ? QStringLiteral("journal of deleted note %1 (with tombstone)")
                                     .arg(noteId)
                               : QStringLiteral("journal %1 without a note and without a tombstone: "
                                                "the file was taken outside the app")
                                     .arg(noteId));
        }
    }

    // ЧЕТЫРЕ КАТЕГОРИИ ВЛОЖЕНИЙ, и разница между ними — не косметика:
    //   живое      — на него ссылается хоть одна неархивная заметка;
    //   «в архиве» — только архивные; это расписание, а не аномалия;
    //   ПОСМЕРТНОЕ — на него не ссылается никто, но в нём стоит метка
    //                «удалено»: это не сирота, а надгробие вложения, и оно
    //                обязано лежать в хранилище, чтобы удаление доехало до
    //                других устройств;
    //   сирота     — не упомянуто НИ В ОДНОЙ заметке и метки не несёт. Вот
    //                это отчёт.
    for (const QString& name : attachments) {
        if (referencedLive.find(name) != referencedLive.end()) continue;
        if (referencedTrashed.find(name) != referencedTrashed.end()) {
            report.note(QStringLiteral("attachment only in archive: %1 (will go with cleanup)")
                            .arg(name));
            continue;
        }
        // Метку читаем только у кандидатов в сироты — их единицы, и чтение
        // заголовка стоит копейки.
        QFile file(attachmentPath(name));
        if (file.open(QIODevice::ReadOnly)) {
            const QByteArray bytes = file.read(64 * 1024);
            file.close();
            const ImageMeta meta =
                readImageMeta(std::string_view(bytes.constData(), size_t(bytes.size())));
            if (xmpZamettiDeleted(meta.xmp)) {
                report.note(QStringLiteral("deleted attachment (a mini preview): %1").arg(name));
                continue;
            }
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

// --- вложения ----------------------------------------------------------------

QStringList ZStorage::attachmentNames() const {
    QStringList attachments;
    for (const QString& name :
         QDir(root_).entryList(QDir::Files | QDir::NoDotAndDotDot | QDir::Hidden)) {
        if (name.endsWith(QStringLiteral(".md"))) continue;
        const qsizetype dot = name.lastIndexOf(QLatin1Char('.'));
        if (dot <= 0 || !isValidNoteId(toUtf8(name.left(dot)))) continue;
        attachments.append(name);
    }
    return attachments;
}

bool ZStorage::readAttachmentBytes(const QString& name, QByteArray* out,
                                   QString* error) const {
    Q_ASSERT(out != nullptr);
    std::string bytes;
    if (!readFileBytes(attachmentPath(name), bytes)) {
        if (error != nullptr)
            *error = QStringLiteral("cannot read attachment %1").arg(name);
        return false;
    }
    *out = QByteArray::fromStdString(bytes);
    return true;
}

QStringList ZStorage::attachmentsLeavingWith(const QStringList& noteIds) const {
    const QDir dir(root_);
    const QSet<QString> doomed(noteIds.begin(), noteIds.end());

    // Все вложения хранилища — один способ их перечислить на всю программу.
    const QStringList attachments = attachmentNames();
    if (attachments.isEmpty()) return {};

    // Кандидаты — те, что упомянуты в уходящих заметках. Иначе удаление
    // уносило бы заодно всех сирот, а решение про сирот владелец отложил
    // («дальше придумаем»).
    QSet<QString> candidates;
    for (const QString& id : noteIds) {
        std::string bytes;
        if (!readFileBytes(pathOf(id), bytes)) continue;
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
        const QString id = idOfPath(name);
        if (doomed.contains(id)) continue;
        std::string bytes;
        if (!readFileBytes(dir.filePath(name), bytes)) continue;
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

bool ZStorage::deleteAttachmentFile(const QString& name, QString* error) {
    const QString file = attachmentPath(name);
    if (!QFile::exists(file)) return true;   // уже нет — и хорошо
    if (!QFile::moveToTrash(file) && !QFile::remove(file)) {
        if (error) *error = QStringLiteral("cannot delete attachment file %1").arg(name);
        return false;
    }
    return true;
}

bool ZStorage::retireAttachment(const QString& name, const ImportLimits& limits, QString* error) {
    const QString path = attachmentPath(name);
    QFile file(path);
    if (!file.exists()) return true;   // уже нет — и хорошо
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("cannot read attachment %1").arg(name);
        return false;
    }
    const QByteArray bytes = file.readAll();
    file.close();

    // ИДЕМПОТЕНТНОСТЬ: уже похороненное не трогаем ни байтом. Иначе ревизия
    // росла бы при каждом заходе, а картинка ужималась заново.
    const ImageMeta meta = readImageMeta(std::string_view(bytes.constData(), size_t(bytes.size())));
    if (xmpZamettiDeleted(meta.xmp)) return true;

    QString why;
    const QByteArray preview = makeDeletedImage(bytes, limits, &why);
    if (preview.isEmpty()) {
        // Не прочли картинку — старое поведение, и причина названа вслух.
        if (error) *error = QStringLiteral("attachment %1 not buried (%2), deleted instead")
                                .arg(name, why);
        return deleteAttachmentFile(name, nullptr);
    }

    // Превью всегда JXL: `<id>.webp` становится `<id>.jxl`, исходник уходит.
    const QString id = QFileInfo(path).completeBaseName();
    const QString target = attachmentPath(id + QStringLiteral(".jxl"));
    QSaveFile out(target);
    if (!out.open(QIODevice::WriteOnly)) {
        if (error) *error = QStringLiteral("cannot write the deleted preview of %1").arg(name);
        return false;
    }
    out.write(preview);
    if (!out.commit()) {
        if (error) *error = QStringLiteral("cannot write the deleted preview of %1").arg(name);
        return false;
    }
    if (target != path && !deleteAttachmentFile(name, &why)) {
        if (error) *error = why;
        return false;
    }
    return true;
}

// --- удаление насовсем и воскрешение -----------------------------------------

bool ZStorage::deleteNoteFile(const QString& id, QString* error) {
    const QString file = pathOf(id);
    if (!QFile::exists(file)) {
        if (error) *error = QStringLiteral("note %1 is not in the store").arg(id);
        return false;
    }

    // НАДГРОБИЕ — ПОСЛЕДНЯЯ ЗАПИСЬ ЖУРНАЛА, И ЖУРНАЛ ОСТАЁТСЯ. Он единственный
    // носитель самого факта: заметки нет, файла нет, и сказать другим
    // устройствам «её больше нет» может только надгробие. Снести журнал значило
    // бы не удалить заметку, а спрятать её локально — первый же синк привёз бы
    // её обратно с любой машины, где она ещё цела.
    //
    // Но полная история после ДВУХ осознанных решений человека (в архив, потом
    // удалить из архива) — мёртвый груз, поэтому надгробие гасит всё, кроме
    // последнего слепка. Его хватает, чтобы поднять заметку (zametti store
    // resurrect), и гашение адресное — значит и на других устройствах журнал
    // похудеет так же, а не разрастётся обратно объединением.
    QString historyError;
    ZJournal read;
    QVector<ZJournal::RecordRef> voids;
    if (readJournal(id, &read, &historyError)) {
        const int keep = read.lastSnapshotIndex();
        for (int i = 0; i < read.size(); ++i) {
            if (i == keep || read.isVoided(i) || read.isDamaged(i)) continue;
            voids.append(ZJournal::RecordRef(read.at(i).time(), read.at(i).digest()));
        }
    }
    const bool marked =
        appendToJournal(id, ZJournal::NewRecord::tombstone().voiding(voids), &historyError);

    if (!QFile::moveToTrash(file) && !QFile::remove(file)) {
        if (error) *error = QStringLiteral("cannot delete note file %1").arg(id);
        return false;
    }
    if (!marked && error) *error = QStringLiteral("tombstone not written: %1").arg(historyError);
    return true;
}

bool ZStorage::resurrect(const QString& id, QString* error) {
    const QString file = pathOf(id);
    if (QFile::exists(file)) {
        if (error) *error = QStringLiteral("note %1 is in the store — nothing to resurrect").arg(id);
        return false;
    }

    ZJournal journal;
    QString why;
    if (!readJournal(id, &journal, &why) || journal.isEmpty()) {
        if (error) *error = QStringLiteral("no history for %1: %2").arg(id, why);
        return false;
    }
    const int head = journal.headIndex();
    if (head < 0 || journal.at(head).kind() != ZJournal::Kind::Tombstone) {
        if (error) *error = QStringLiteral("note %1 was not deleted (no tombstone at the head)").arg(id);
        return false;
    }
    const int content = journal.lastSnapshotIndex();
    QByteArray body;
    if (content < 0 || !journalSnapshot(id, content, &body, &why)) {
        if (error) *error = QStringLiteral("history of %1 has no body: %2").arg(id, why);
        return false;
    }

    if (!writeFileBytes(file, std::string(body.constData(), size_t(body.size())), error))
        return false;

    // ЗАПИСЬ ПОВЕРХ НАДГРОБИЯ: заметка снова жива, и голова обязана это
    // сказать. Вид — «восстановление», источник — время того слепка, из
    // которого её подняли.
    if (!appendToJournal(id, ZJournal::NewRecord::restore(body, journal.at(content).time()), &why) &&
        error != nullptr)
        *error = QStringLiteral("resurrection not written to history: %1").arg(why);
    if (loaded_) refreshNote(id);
    return true;
}

}  // namespace zametti
