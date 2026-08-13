// Круг через ЖИВОЙ ДОКУМЕНТ: файл → IR → QTextDocument → IR → файл.
//
// Этого набора не было, и это дыра в матрице, а не в коде: круг ядра
// (parse → serialize) проверялся давно и подробно, а вот путь через документ
// Qt — тот самый, которым идёт КАЖДАЯ запись заметки, — не проверялся ни разу.
// Владелец нашёл дыру за меня: «каждая попытка архивировать добавляет огромное
// количество символов \». Заметка с формулами теряла в этом круге признак
// математики, `\gamma` уезжал как `\\gamma`, и каждая запись удваивала косые.
//
// Проверка идёт ТРИ круга. Одного мало: беда была не в первом проходе, а в
// накоплении — файл портился на каждой записи понемногу.

#include "document_builder.h"
#include "document_reader.h"
#include "parser.h"
#include "serializer.h"

#include "test_util.h"

#include <QApplication>
#include <QTextDocument>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using zametti::buildDocument;
using zametti::parse;
using zametti::readDocument;
using zametti::serialize;

namespace {

std::string readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    std::string text = buffer.str();
    // Файлы корпуса бывают с CRLF; заметка в хранилище — всегда с LF, и круг
    // спрашивается о ней.
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n') continue;
        out += text[i];
    }
    return out;
}

// Один круг через документ.
std::string throughDocument(const std::string& text) {
    QTextDocument doc;
    buildDocument(parse(text), doc, 1.0);
    return serialize(readDocument(doc));
}

// Канон: то, что уже прошло круг ЯДРА. С ним и сравниваем — расхождения самого
// ядра (списки, курсив) здесь ни при чём, они разобраны своим набором.
void checkFile(const std::string& path, const std::string& name) {
    const std::string source = readFile(path);
    if (source.empty()) {
        std::printf("нет файла %s — пропущено\n", path.c_str());
        return;
    }
    const std::string canon = serialize(parse(source));

    std::string text = canon;
    for (int round = 1; round <= 3; ++round) {
        const std::string back = throughDocument(text);
        // Место расхождения — в сообщение: без него «файлы разные» ничего не
        // говорит на заметке в 20 килобайт.
        std::string where;
        if (back != text) {
            size_t i = 0;
            while (i < back.size() && i < text.size() && back[i] == text[i]) ++i;
            where = ", разошлось на " + std::to_string(i) + ": ждали [" +
                    text.substr(i, 48) + "] вышло [" + back.substr(i, 48) + "]";
        }
        ZT_TRUE(name + ", круг " + std::to_string(round) + where, back == text);
        text = back;
    }
}

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);

    // Заметка с формулами — та, на которой беду и нашли.
    checkFile(argc > 1 ? argv[1] : "../.testdata/Typesetting Math in Texts.md",
              "заметка с формулами");

    // И весь корпус: беда была общая, а не про формулы, и матрица должна
    // спрашивать про все заметки, а не про удобную.
    const std::string corpus = argc > 2 ? argv[2] : "../.testdata/corpus";
    std::error_code ec;
    std::vector<std::string> files;
    if (std::filesystem::is_directory(corpus, ec)) {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(
                 corpus, std::filesystem::directory_options::skip_permission_denied, ec))
            if (entry.is_regular_file(ec) && entry.path().extension() == ".md")
                files.push_back(entry.path().string());
    }
    std::sort(files.begin(), files.end());
    for (const std::string& file : files)
        checkFile(file, std::filesystem::path(file).filename().string());
    if (files.empty()) std::printf("корпуса нет — проверена только заметка с формулами\n");

    return zt::report("круг через документ");
}
