// Просмотрщик: один .md на входе, окно с отрендеренным документом.
//
// Это не самоцель, а первый стенд для проверки ядра. Отсюда же работает режим
// --check: прогнать parse → serialize и показать расхождение с оригиналом.

#include "document_builder.h"
#include "parser.h"
#include "serializer.h"

#include <QApplication>
#include <QFileInfo>
#include <QTextBrowser>
#include <QTextDocument>

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

    if (path.empty()) {
        std::fprintf(stderr, "использование: zametti [--check] файл.md\n");
        return 2;
    }

    // --check не должен требовать дисплея: он работает в конвейерах и в CI.
    if (check) return runCheck(path);

    std::string src;
    if (!readFile(path, src)) {
        std::fprintf(stderr, "не читается: %s\n", path.c_str());
        return 2;
    }

    QApplication app(argc, argv);

    QTextBrowser view;
    view.setOpenExternalLinks(true);
    zametti::buildDocument(zametti::parse(src), *view.document());
    view.setWindowTitle(QFileInfo(QString::fromStdString(path)).fileName() +
                        QStringLiteral(" — zametti"));
    view.resize(900, 700);
    view.show();

    return app.exec();
}
