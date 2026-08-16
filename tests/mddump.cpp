// Отладочный инструмент: markdown из файла или stdin → JSON-дамп IR и обратная
// сериализация. Нужен, чтобы смотреть глазами, что именно вышло из разбора.
//
//   mddump [файл]           дамп IR
//   mddump [файл] --md      сериализованный markdown
//   mddump [файл] --check   дифф noteOf(x).toMarkdown() с оригиналом

#include "pieces.h"

#include "test_util.h"

#include <QGuiApplication>

#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

int main(int argc, char** argv) {
    // ПЛАТФОРМА — ДО СОЗДАНИЯ ПРИЛОЖЕНИЯ, и приложение обязательно: приведение
    // к канону идёт через живой QTextDocument, а сборщик спрашивает метрики
    // шрифта — без QGuiApplication это падение, а падений у нас не бывает
    // (`--check` валился в core dump на всякой машине без дисплея).
    //
    // Заданную снаружи платформу не перебиваем — под условием, как и в
    // zametti-store: спрашиваем «пуста ли», потому что пустая платформа для Qt
    // не платформа, он уходит в автоопределение и падает.
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);

    std::string path;
    std::string mode;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a.rfind("--", 0) == 0) mode = a;
        else path = a;
    }

    std::string src;
    if (path.empty()) {
        std::ostringstream ss;
        ss << std::cin.rdbuf();
        src = ss.str();
    } else {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            std::fprintf(stderr, "не читается: %s\n", path.c_str());
            return 2;
        }
        std::ostringstream ss;
        ss << in.rdbuf();
        src = ss.str();
    }

    std::vector<zametti::Piece> doc = pieces(src);

    if (mode == "--md") {
        std::fputs(markdownOf(doc).c_str(), stdout);
        return 0;
    }
    if (mode == "--check") {
        std::string out = markdownOf(doc);
        if (out == src) return 0;
        std::fputs(zt::diff(src, out).c_str(), stdout);
        return 1;
    }
    std::fputs(dumpOf(doc).c_str(), stdout);
    return 0;
}
