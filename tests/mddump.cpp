// Отладочный инструмент: markdown из файла или stdin → JSON-дамп IR и обратная
// сериализация. Нужен, чтобы смотреть глазами, что именно вышло из разбора.
//
//   mddump [файл]           дамп IR
//   mddump [файл] --md      сериализованный markdown, ЖИВОЙ канон (writePieces)
//   mddump [файл] --file    то, что ушло бы в файл: bodyOf(x).toMarkdown()
//   mddump [файл] --check   дифф bodyOf(x).toMarkdown() с оригиналом
//
// РАЗНИЦА МЕЖДУ --md И --file И ЕСТЬ ПРЕДМЕТ ОТЛАДКИ: живой документ вправе
// держать то, чего markdown не хранит, а файл — нет. `--check` спрашивает
// ФАЙЛОВЫЙ канон: раньше он звал живого писателя, хотя шапка обещала обратное,
// и потому не видел ровно тех расхождений, ради которых его заводили.

#include "pieces.h"
#include "zstorage.h"

#include "test_util.h"

#include <QGuiApplication>

#include <cstdio>
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
        // Через хранилище, а не std::ifstream: узкий поток берёт имя файла в
        // кодировке ANSI системы, и под Windows путь с кириллицей ему не
        // открыть. Правило одно на весь проект — файлы читает Qt.
        if (!zametti::ZStorage::readFileBytes(QString::fromStdString(path), src)) {
            std::fprintf(stderr, "не читается: %s\n", path.c_str());
            return 2;
        }
    }

    std::vector<zametti::Piece> doc = pieces(src);

    if (mode == "--md") {
        std::fputs(markdownOf(doc).c_str(), stdout);
        return 0;
    }
    if (mode == "--file") {
        std::fputs(bodyOf(src).toMarkdown().c_str(), stdout);
        return 0;
    }
    if (mode == "--check") {
        std::string out = bodyOf(src).toMarkdown();
        if (out == src) return 0;
        std::fputs(zt::diff(src, out).c_str(), stdout);
        return 1;
    }
    std::fputs(dumpOf(doc).c_str(), stdout);
    return 0;
}
