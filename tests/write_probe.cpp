// ЦЕНА ОДНОЙ ПРОБЫ РАЗМЕТКИ: writePieces({блок}) строит документ-однодневку,
// parsePieces читает его назад. Стенд переключения (switch) показал, что
// сериализация заметки стоит в 20 раз дороже её разбора и не зависит от типа
// сборки; стеки под gdb указывают на шрифты и сборку документа. Здесь это
// раскладывается на слагаемые: шрифт (QFont + метрики + QRawFont), сборка
// пустого документа, проба целиком, разбор.
//
//   zametti-bench write-probe [повторов]
//   zametti-bench write-probe <заметка.md>   — слагаемые fileBytes на настоящей заметке

#include "document_builder.h"
#include "document_pieces.h"
#include "settings.h"
#include "znote.h"

#include <QFile>

#include <QElapsedTimer>
#include <QFontMetricsF>
#include <QRawFont>
#include <QTextDocument>

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <vector>

namespace {

double perCall(int n, const std::function<void()>& body) {
    QElapsedTimer t;
    t.start();
    for (int i = 0; i < n; ++i) body();
    return double(t.nsecsElapsed()) / 1000.0 / n;
}

}  // namespace

int ztWriteProbe(int argc, char** argv) {
    if (argc >= 2 && QString::fromLocal8Bit(argv[1]).endsWith(QStringLiteral(".md"))) {
        QFile f(QString::fromLocal8Bit(argv[1]));
        if (!f.open(QIODevice::ReadOnly)) return 1;
        const QByteArray bytes = f.readAll();
        const std::string text(bytes.constData(), size_t(bytes.size()));
        zametti::ZNote note;
        note.load(text);
        std::vector<zametti::Piece> pieces;
        zametti::NoteHeader header;
        zametti::parsePieces(QString::fromUtf8(bytes), pieces, header);
        int withRuns = 0;
        int raws = 0;
        for (const zametti::Piece& p : pieces) {
            if (p.raw) ++raws;
            else if (!p.runs.empty()) ++withRuns;
        }
        std::printf("%s: %zu байт, блоков %zu (с разметкой %d, дословных %d); микросекунды\n",
                    argv[1], text.size(), pieces.size(), withRuns, raws);
        const int n = 5;
        std::printf("  parsePieces(файл)              %10.0f\n",
                    perCall(n, [&] { std::vector<zametti::Piece> b; zametti::NoteHeader h;
                                     zametti::parsePieces(QString::fromUtf8(bytes), b, h); }));
        std::printf("  note.load(файл)                %10.0f\n",
                    perCall(n, [&] { zametti::ZNote x; x.load(text); }));
        std::printf("  documentForFile(куски)         %10.0f\n",
                    perCall(n, [&] { (void)zametti::documentForFile(pieces); }));
        std::printf("  writePieces(куски, шапка)      %10.0f\n",
                    perCall(n, [&] { (void)zametti::writePieces(pieces, header); }));
        std::printf("  note.fileBytes()               %10.0f\n",
                    perCall(n, [&] { (void)note.fileBytes(); }));
        std::printf("  note.toMarkdown()              %10.0f\n",
                    perCall(n, [&] { (void)note.toMarkdown(); }));
        return 0;
    }
    const int n = argc >= 2 ? std::atoi(argv[1]) : 500;
    const zametti::ZDocStyle& style = zametti::settings().style();
    zametti::Piece para;
    para.kind = zametti::Kind::Paragraph;
    para.text = QStringLiteral("Обычный абзац без разметки, каких в заметке большинство, "
                               "строк на пять, чтобы было похоже на правду и не слишком коротко.");
    std::vector<zametti::Piece> one{para};
    std::vector<zametti::Piece> back;
    zametti::NoteHeader ignored;
    const QString written = zametti::writePieces(one);

    std::printf("повторов %d, микросекунды на вызов\n", n);
    std::printf("  layoutBaseFont                 %8.1f\n",
                perCall(n, [&] { (void)zametti::layoutBaseFont(style); }));
    std::printf("  + QFontMetricsF::height        %8.1f\n",
                perCall(n, [&] { (void)QFontMetricsF(zametti::layoutBaseFont(style)).height(); }));
    std::printf("  layoutCharUnit                 %8.1f\n",
                perCall(n, [&] { (void)zametti::layoutCharUnit(style); }));
    std::printf("  QRawFont::fromFont             %8.1f\n",
                perCall(n, [&] { (void)QRawFont::fromFont(zametti::layoutBaseFont(style)); }));
    std::printf("  QTextDocument пустой           %8.1f\n",
                perCall(n, [&] { QTextDocument d; (void)d; }));
    std::printf("  buildDocument({абзац})         %8.1f\n",
                perCall(n, [&] { QTextDocument d; zametti::buildDocument(one, d); }));
    std::printf("  writePieces({абзац})           %8.1f\n",
                perCall(n, [&] { (void)zametti::writePieces(one); }));
    std::printf("  parsePieces(абзац)             %8.1f\n",
                perCall(n, [&] { back.clear(); zametti::parsePieces(written, back, ignored); }));
    return 0;
}
