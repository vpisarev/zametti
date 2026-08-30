#include "recompress.h"

#include "zsystem.h"

#include "import.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

namespace zametti {

namespace {

// Расширения, которые вообще могут быть вложением. Заметки и служебное сюда не
// попадают: у них другие расширения, и путать их нельзя ни при каких условиях.
bool isAttachment(const QString& suffix) {
    static const QStringList kKnown = {
        QStringLiteral("jxl"),  QStringLiteral("jpg"),  QStringLiteral("jpeg"),
        QStringLiteral("png"),  QStringLiteral("webp"), QStringLiteral("gif"),
        QStringLiteral("tif"),  QStringLiteral("tiff"), QStringLiteral("avif"),
        QStringLiteral("heic"), QStringLiteral("heif"), QStringLiteral("bmp"),
    };
    return kKnown.contains(suffix.toLower());
}

QString human(qint64 bytes) {
    if (bytes >= 1024 * 1024)
        return QStringLiteral("%1 MB").arg(double(bytes) / 1048576.0, 0, 'f', 1);
    return QStringLiteral("%1 KB").arg(double(bytes) / 1024.0, 0, 'f', 0);
}

}  // namespace

bool recompressStore(const RecompressOptions& options, RecompressReport& report) {
    if (options.root.isEmpty()) {
        report.problems << QStringLiteral("no store named to process");
        return false;
    }
    // Главная защита от опечатки: без явного --id команда не делает НИЧЕГО.
    if (options.id.isEmpty()) {
        report.problems << QStringLiteral(
            "nothing named to recompress. One image — «--id <id>», all of them — «--id all». "
            "This key deliberately has no default: recompression is irreversible.");
        return false;
    }

    QDir dir(options.root);
    if (!dir.exists()) {
        report.problems << QStringLiteral("no directory %1").arg(options.root);
        return false;
    }
    // Область разрушения — само хранилище, и уже отсюда видно, что за его
    // пределы пережатие не выйдет ни на один файл. Путь доводится до
    // абсолютного здесь: `--root ./notes` — законный способ назвать СВОЙ
    // каталог, в отличие от адреса облака.
    const ZSystem area(ZSystem::Area::Storage, dir.absolutePath());

    const bool all = options.id == QStringLiteral("all");
    QStringList names;
    for (const QString& name : dir.entryList(QDir::Files | QDir::NoDotAndDotDot, QDir::Name)) {
        const QFileInfo fi(name);
        if (!isAttachment(fi.suffix())) continue;
        if (!all && fi.completeBaseName() != options.id) continue;
        names << name;
    }
    if (names.isEmpty()) {
        report.problems << (all ? QStringLiteral("nothing to work on: no images found")
                                : QStringLiteral("no image with id %1 in the store")
                                      .arg(options.id));
        return false;
    }

    bool ok = true;
    for (const QString& name : names) {
        const QString path = dir.filePath(name);
        const QFileInfo fi(path);
        const qint64 before = fi.size();
        ++report.examined;
        report.bytesBefore += before;

        const ImportResult r = importImage(path, options.limits);
        if (!r.ok()) {
            ++report.failed;
            report.bytesAfter += before;
            report.lines << QStringLiteral("%1: untouched — %2").arg(name, r.message);
            ok = false;
            continue;
        }

        // Расширение могло смениться: webp, ставший JXL, обязан и называться
        // иначе, иначе программа будет читать его по расширению неправильно.
        const QString newName = fi.completeBaseName() + QLatin1Char('.') + r.extension;
        const QString newPath = dir.filePath(newName);
        const bool sameFile = newName == name;

        // ИДЕМПОТЕНТНОСТЬ. Если байты те же — не трогаем файл вовсе: иначе
        // повторный прогон менял бы mtime, а с ним и кэш, на ровном месте.
        if (sameFile && r.bytes.size() == before) {
            QFile f(path);
            if (f.open(QIODevice::ReadOnly) && f.readAll() == r.bytes) {
                ++report.untouched;
                report.bytesAfter += before;
                report.lines << QStringLiteral("%1: already as it should be (%2)")
                                    .arg(name, routeName(r.route));
                continue;
            }
        }

        // Не стало меньше — оставляем как было. Пережимать ради того, чтобы
        // потолстеть, бессмысленно, а качество при этом теряется.
        if (r.bytes.size() >= before && r.route != Route::AsIs) {
            ++report.untouched;
            report.bytesAfter += before;
            report.lines << QStringLiteral("%1: kept — recompression did not shrink it (%2 → %3)")
                                .arg(name, human(before), human(r.bytes.size()));
            continue;
        }

        report.bytesAfter += r.bytes.size();
        ++report.rewritten;
        QString what = QStringLiteral("%1: %2, %3 → %4")
                           .arg(name, QString::fromUtf8(routeName(r.route)), human(before),
                                human(r.bytes.size()));
        if (r.size.width > 0) what += QStringLiteral(", %1x%2").arg(r.size.width).arg(r.size.height);
        if (!r.message.isEmpty()) what += QStringLiteral(" — %1").arg(r.message);
        if (!sameFile) what += QStringLiteral(" (now %1)").arg(newName);
        report.lines << what;

        if (options.dryRun) continue;

        // Пишем через QSaveFile: оборванная запись не должна оставить половину
        // картинки вместо целой.
        QSaveFile out(newPath);
        if (!out.open(QIODevice::WriteOnly) || out.write(r.bytes) != r.bytes.size() ||
            !out.commit()) {
            report.problems << QStringLiteral("failed to write %1").arg(newName);
            ok = false;
            continue;
        }
        // Старый файл удаляем ТОЛЬКО после того, как новый записан целиком.
        // Мимо корзины: пережатие и без того объявлено необратимым, а копия
        // старого веса в корзине обманывала бы обещанием «место освободилось».
        QString whyDelete;
        if (!sameFile && !area.removeForever(path, &whyDelete))
            report.problems << QStringLiteral("failed to remove the old %1: %2")
                                   .arg(name, whyDelete);
    }

    report.lines << QString();
    report.lines << QStringLiteral(
        "Recompression is irreversible — except for transcoded JPEGs, which rebuild "
        "byte for byte. Until the result has been checked by eye, keep the reference "
        "import tree.");
    return ok;
}

}  // namespace zametti
