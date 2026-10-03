// The phone shell, step 0 of the Android port (docs/zametti-brief-android0.md).
//
// The desktop's start (app/main.cpp) without its command line: the same
// codecs, fonts, formula engine, ZApp and settings, then a MobileWindow
// instead of the three-column window. The store is the private directory of
// the app and nothing else (brief §7): no external storage, no permissions —
// the sandbox and the device encryption are the fence.
//
// STATE IS SAVED TWICE: on aboutToQuit, as on the desktop, and when the
// application goes to the background — Android kills background processes
// without a quit, and the reading place would be lost with it.

#include "build_facts.h"
#include "formula.h"
#include "heif_handler.h"
#include "mobile_window.h"
#include "perf_log.h"
#include "resources.h"
#include "settings.h"
#include "zapp.h"
#include "zstorage.h"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QTimer>
#include <QStandardPaths>
#include <QStyleFactory>

namespace {

// The store on the phone: <AppDataLocation>/vpnotes, seeded by
// packaging/android/device.sh (tar | adb exec-in run-as). Not created here:
// an empty store would hide a failed seeding behind an empty list.
QString expectedStoreRoot() {
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
           QStringLiteral("/vpnotes");
}

}  // namespace

int main(int argc, char* argv[]) {
    QElapsedTimer clock;
    clock.start();

    QCoreApplication::setApplicationName(QStringLiteral("zametti"));
    QCoreApplication::setApplicationVersion(QString::fromUtf8(zametti::kVersion));
    QApplication app(argc, argv);
    // Fusion: one look on every device, no Android style plugin involved
    // (the brief's choice for step 0).
    QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    zametti::perfLog("QApplication", clock.elapsed());

    zametti::HeifHandler::registerCodecs();
    zametti::loadEmbeddedFonts();
    zametti::perfLog("codecs + fonts", clock.elapsed());
    {
        QString error;
        if (!zametti::Formulas::init(&error))
            qCWarning(lcZametti) << "formulas:" << error;
    }
    zametti::perfLog("formulas", clock.elapsed());

    zametti::ZApp zapp;
    {
        QString error;
        QStringList unknown;
        if (!zapp.reloadSettings(&error, &unknown))
            qCWarning(lcZametti) << "config not accepted:" << error;
        for (const QString& key : unknown) qCWarning(lcZametti) << "unknown setting:" << key;
    }
    QApplication::setFont(zapp.uiStyle().appFont());

    const QString root = expectedStoreRoot();
    std::shared_ptr<zametti::ZStorage> storage;
    if (zametti::ZStorage::isStoreRoot(root)) {
        storage = zapp.openStorage(root);
        zametti::perfLog("store opened", clock.elapsed());
    } else {
        qCWarning(lcZametti) << "no store at" << root;
        storage = zapp.openStorage(QString());
    }

    zametti::MobileWindow window(storage, root);
    window.show();
    zametti::perfLog("window shown", clock.elapsed());

    // THE PROBE DOOR for the measurements (brief §9): a file next to the
    // store naming a note id — `adb shell run-as … sh -c 'echo <id> > files/probe-open'`
    // — opens that note as soon as the list is on screen, so that "open →
    // paint" of a NAMED note (the Karamazovs, the formulas) is measured on
    // the real path, without hunting for its row by coordinates. Read once,
    // never written by the program.
    if (storage != nullptr) {
        QFile probe(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
                    QStringLiteral("/probe-open"));
        if (probe.open(QIODevice::ReadOnly)) {
            const QString id = QString::fromUtf8(probe.readAll()).trimmed();
            const QString file = storage->pathOf(id);
            qCInfo(lcZametti) << "probe-open" << id << (file.isEmpty() ? "(unknown id)" : "");
            // After the window has settled: opened at once, the pages would
            // be laid out for the pre-safe-area size and the first spread
            // would come out wider than the screen (seen: 1233 pages instead
            // of 1332, the right edge clipped). The delay is not measured —
            // the [perf] clock starts inside open().
            if (!file.isEmpty())
                QTimer::singleShot(300, &window, [&window, file] { window.openNote(file); });
        }
    }

    const auto saveAll = [&] {
        window.rememberPlaces();
        zapp.saveState();
    };
    QObject::connect(&app, &QGuiApplication::applicationStateChanged, &window,
                     [&](Qt::ApplicationState state) {
                         if (state == Qt::ApplicationSuspended || state == Qt::ApplicationHidden)
                             saveAll();
                     });
    QObject::connect(&app, &QCoreApplication::aboutToQuit, &window, saveAll);

    return app.exec();
}
