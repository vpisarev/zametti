// Просмотрщик: один .md на входе, окно с отрендеренным документом.
//
// Это не самоцель, а первый стенд для проверки ядра. Отсюда же работает режим
// --check: прогнать parse → serialize и показать расхождение с оригиналом.

#include "document_builder.h"
#include "parser.h"
#include "serializer.h"
#include "settings.h"

#include <QApplication>
#include <QFileInfo>
#include <QIcon>
#include <QKeySequence>
#include <QScrollBar>
#include <QShortcut>
#include <QTextBrowser>
#include <QTextCursor>
#include <QTextDocument>
#include <QTimer>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

bool readFile(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

std::vector<std::string> splitLines(const std::string& s) {
    std::vector<std::string> lines;
    size_t pos = 0;
    while (pos <= s.size()) {
        size_t e = s.find('\n', pos);
        if (e == std::string::npos) {
            if (pos < s.size()) lines.push_back(s.substr(pos));
            break;
        }
        lines.push_back(s.substr(pos, e - pos));
        pos = e + 1;
        if (pos == s.size()) break;
    }
    return lines;
}

// Построчный дифф. Не «первый различающийся байт»: расхождение надо читать
// глазами, а не вычислять.
int runCheck(const std::string& path) {
    std::string src;
    if (!readFile(path, src)) {
        std::fprintf(stderr, "не читается: %s\n", path.c_str());
        return 2;
    }

    std::string out = zametti::serialize(zametti::parse(src));
    if (out == src) return 0;

    std::vector<std::string> a = splitLines(src);
    std::vector<std::string> b = splitLines(out);
    size_t n = a.size() > b.size() ? a.size() : b.size();
    for (size_t i = 0; i < n; ++i) {
        const std::string* ea = i < a.size() ? &a[i] : nullptr;
        const std::string* eb = i < b.size() ? &b[i] : nullptr;
        if (ea != nullptr && eb != nullptr && *ea == *eb) continue;
        if (ea != nullptr) std::printf("%5zu - %s\n", i + 1, ea->c_str());
        if (eb != nullptr) std::printf("%5zu + %s\n", i + 1, eb->c_str());
    }
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    std::string path;
    bool check = false;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--check") check = true;
        else if (arg.rfind("--", 0) == 0) {
            std::fprintf(stderr, "неизвестный ключ: %s\n", arg.c_str());
            return 2;
        } else {
            path = arg;
        }
    }

    // --check не должен требовать дисплея: он работает в конвейерах и в CI.
    if (check) {
        if (path.empty()) {
            std::fprintf(stderr, "использование: zametti --check файл.md\n");
            return 2;
        }
        return runCheck(path);
    }

    QApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("zametti"));
    QGuiApplication::setWindowIcon(QIcon(QStringLiteral(":/zametti.png")));

    QString configError;
    if (!zametti::loadAppearance(&configError)) {
        // Молча подставить умолчания нельзя: опечатка в конфиге выглядела бы
        // как «настройка не работает».
        std::fprintf(stderr, "конфиг не разобран, взяты значения по умолчанию:\n  %s\n",
                     configError.toUtf8().constData());
    }

    const zametti::Session session = zametti::loadSession();

    // Без аргумента открываем то, что читали в прошлый раз.
    if (path.empty() && !session.lastFile.isEmpty()) path = session.lastFile.toStdString();
    if (path.empty()) {
        std::fprintf(stderr, "использование: zametti [--check] файл.md\n");
        return 2;
    }

    std::string src;
    if (!readFile(path, src)) {
        std::fprintf(stderr, "не читается: %s\n", path.c_str());
        return 2;
    }

    const QString absolutePath = QFileInfo(QString::fromStdString(path)).absoluteFilePath();
    const bool sameFileAsLastTime = session.lastFile == absolutePath;
    const zametti::Document doc = zametti::parse(src);

    QTextBrowser view;
    view.setOpenExternalLinks(true);
    zametti::applyPalette(view);
    view.setWindowTitle(QFileInfo(absolutePath).fileName() + QStringLiteral(" — zametti"));

    // Кегль задан явно в каждом формате, поэтому штатный зум QTextEdit до него
    // не дотягивается: при смене масштаба документ собирается заново. Место в
    // тексте держим по доле прокрутки — в пикселях оно после пересборки другое.
    qreal zoom = std::clamp(sameFileAsLastTime ? session.zoom : qreal(1.0),
                            zametti::appearance().zoomMin, zametti::appearance().zoomMax);

    auto scrollRatio = [&view]() -> double {
        const QScrollBar* bar = view.verticalScrollBar();
        return bar->maximum() > 0 ? double(bar->value()) / bar->maximum() : 0.0;
    };
    auto rebuild = [&view, &doc](qreal z, double ratio) {
        zametti::buildDocument(doc, *view.document(), z);
        view.moveCursor(QTextCursor::Start);
        QScrollBar* bar = view.verticalScrollBar();
        bar->setValue(int(ratio * bar->maximum()));
    };

    auto applyZoom = [&](qreal factor) {
        const qreal next = std::clamp(zoom * factor, zametti::appearance().zoomMin,
                                      zametti::appearance().zoomMax);
        if (next == zoom) return;
        const double ratio = scrollRatio();
        zoom = next;
        rebuild(zoom, ratio);
    };

    const auto shortcut = [&view](const QKeySequence& keys, auto&& slot) {
        QObject::connect(new QShortcut(keys, &view), &QShortcut::activated, &view, slot);
    };
    // Ctrl+= рядом с Ctrl++: увеличивают одной и той же клавишей, с шифтом и без.
    shortcut(QKeySequence(QStringLiteral("Ctrl+=")),
             [&] { applyZoom(zametti::appearance().zoomStep); });
    shortcut(QKeySequence(QStringLiteral("Ctrl++")),
             [&] { applyZoom(zametti::appearance().zoomStep); });
    shortcut(QKeySequence(QStringLiteral("Ctrl+-")),
             [&] { applyZoom(1.0 / zametti::appearance().zoomStep); });
    shortcut(QKeySequence(QStringLiteral("Ctrl+0")), [&] {
        if (zoom == qreal(1.0)) return;
        const double ratio = scrollRatio();
        zoom = 1.0;
        rebuild(zoom, ratio);
    });

    if (!session.windowGeometry.isEmpty()) view.restoreGeometry(session.windowGeometry);
    else view.resize(900, 700);
    view.show();

    rebuild(zoom, 0.0);

    // Прокрутку можно ставить только когда документ уже разложен по размерам
    // окна, а это происходит после show(), в следующем проходе цикла событий.
    const double startRatio = sameFileAsLastTime ? session.scrollRatio : 0.0;
    if (startRatio > 0.0) {
        QTimer::singleShot(0, &view, [&view, startRatio] {
            QScrollBar* bar = view.verticalScrollBar();
            bar->setValue(int(startRatio * bar->maximum()));
        });
    }

    QObject::connect(&app, &QCoreApplication::aboutToQuit, &view, [&] {
        zametti::Session out;
        out.lastFile = absolutePath;
        out.scrollRatio = scrollRatio();
        out.zoom = zoom;
        out.windowGeometry = view.saveGeometry();
        zametti::saveSession(out);
    });

    return app.exec();
}
