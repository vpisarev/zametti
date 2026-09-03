// ЧТО СТОИТ QTextEdit::setDocument НА УЖЕ СВЁРСТАННОМ ДОКУМЕНТЕ — и что из
// этого наше. Заметка из кэша готова целиком, а подмена документа в виде
// стоила 25 мс на 1.9 МБ (стенд switch). Здесь — чистый Qt без нашего слоя:
// документ строится сборщиком, кладётся в голый QTextEdit, верстается до
// конца, и дальше меряются по отдельности: setDocument того же документа,
// setPageSize того же размера, setDefaultTextOption того же, markContentsDirty
// на весь документ + кадр.
//
//   zametti-bench setdoc-probe <заметка.md>

#include "document_builder.h"
#include "document_pieces.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QTest>
#include <QTextDocument>
#include <QStackedWidget>
#include <QTextEdit>
#include <QAbstractTextDocumentLayout>

#include <cstdio>
#include <ctime>

namespace {
double ms(const std::function<void()>& body) {
    QElapsedTimer t;
    t.start();
    body();
    return t.nsecsElapsed() / 1e6;
}
}  // namespace

int ztSetDocProbe(int argc, char** argv) {
    if (argc < 2) {
        std::printf("zametti-bench setdoc-probe <заметка.md>\n");
        return 2;
    }
    QFile f(QString::fromLocal8Bit(argv[1]));
    if (!f.open(QIODevice::ReadOnly)) return 1;
    std::vector<zametti::Piece> pieces;
    zametti::NoteHeader header;
    zametti::parsePieces(QString::fromUtf8(f.readAll()), pieces, header);
    QTextDocument doc;
    zametti::buildDocument(pieces, doc);
    QTextEdit view;
    view.resize(1000, 800);
    view.show();
    QTest::qWait(30);
    std::printf("блоков %d, знаков %d; миллисекунды\n", doc.blockCount(), doc.characterCount());
    std::printf("  setDocument первый раз          %8.2f\n", ms([&] { view.setDocument(&doc); }));
    // Доверстать до конца: спросить высоту всего документа.
    std::printf("  вёрстка целиком (documentSize)  %8.2f\n",
                ms([&] { (void)doc.documentLayout()->documentSize(); }));
    QCoreApplication::processEvents();
    for (int i = 0; i < 3; ++i)
        std::printf("  setDocument того же документа   %8.2f\n", ms([&] { view.setDocument(&doc); }));
    std::printf("  после него documentSize         %8.2f\n",
                ms([&] { (void)doc.documentLayout()->documentSize(); }));
    for (int i = 0; i < 2; ++i)
        std::printf("  setPageSize того же             %8.2f\n",
                    ms([&] { doc.setPageSize(doc.pageSize()); }));
    std::printf("  после него documentSize         %8.2f\n",
                ms([&] { (void)doc.documentLayout()->documentSize(); }));
    for (int i = 0; i < 2; ++i)
        std::printf("  setDefaultTextOption того же    %8.2f\n",
                    ms([&] { doc.setDefaultTextOption(doc.defaultTextOption()); }));
    std::printf("  после него documentSize         %8.2f\n",
                ms([&] { (void)doc.documentLayout()->documentSize(); }));
    std::printf("  markContentsDirty(всё)          %8.2f\n",
                ms([&] { doc.markContentsDirty(0, doc.characterCount()); }));
    std::printf("  после него documentSize         %8.2f\n",
                ms([&] { (void)doc.documentLayout()->documentSize(); }));
    std::printf("  кадр (repaint)                  %8.2f\n", ms([&] { view.viewport()->repaint(); }));
    // Второй вид: документ в другом QTextEdit и обратно — так живёт кэш? Нет:
    // у нас один вид, документы сменяются в нём. Меряем ровно это: A → B → A.
    QTextDocument other;
    other.setPlainText(QStringLiteral("другая"));
    std::printf("  setDocument(другой) → (этот)    %8.2f\n", ms([&] {
        view.setDocument(&other);
        view.setDocument(&doc);
    }));
    std::printf("  после него documentSize         %8.2f\n",
                ms([&] { (void)doc.documentLayout()->documentSize(); }));
    // ДОВЁРСТЫВАЕТ ЛИ Qt В ФОНЕ после подмены: даём циклу событий секунду и
    // спрашиваем размер снова. Ноль — вёрстка уже доделана таймером ленивой
    // вёрстки (и её цена легла на цикл событий, где живёт всё остальное).
    view.setDocument(&other);
    view.setDocument(&doc);
    QTest::qWait(1000);
    std::printf("  подмена, секунда цикла событий, documentSize %8.2f\n",
                ms([&] { (void)doc.documentLayout()->documentSize(); }));
    // И сколько процессорного времени ушло за эту секунду ожидания.
    view.setDocument(&other);
    view.setDocument(&doc);
    const clock_t cpu0 = std::clock();
    QTest::qWait(1000);
    std::printf("  процессор за секунду после подмены %8.2f ms\n",
                double(std::clock() - cpu0) * 1000.0 / CLOCKS_PER_SEC);

    // АЛЬТЕРНАТИВА: у каждого документа свой вид, переключается виджет, а не
    // документ — вёрстка остаётся при документе и виде.
    QStackedWidget stack;
    QTextEdit* viewA = new QTextEdit;
    QTextEdit* viewB = new QTextEdit;
    QTextDocument docA;
    zametti::buildDocument(pieces, docA);
    viewA->setDocument(&docA);
    viewB->setDocument(&other);
    stack.addWidget(viewA);
    stack.addWidget(viewB);
    stack.resize(1000, 800);
    stack.show();
    QTest::qWait(1500);   // дать A доверстаться
    std::printf("  A доверстан: documentSize        %8.2f\n",
                ms([&] { (void)docA.documentLayout()->documentSize(); }));
    stack.setCurrentWidget(viewB);
    QTest::qWait(100);
    const clock_t cpu1 = std::clock();
    std::printf("  стек: показать A снова           %8.2f\n", ms([&] {
        stack.setCurrentWidget(viewA);
        viewA->viewport()->repaint();
    }));
    QTest::qWait(1000);
    std::printf("  процессор за секунду после стека %8.2f ms\n",
                double(std::clock() - cpu1) * 1000.0 / CLOCKS_PER_SEC);
    std::printf("  A после стека: documentSize      %8.2f\n",
                ms([&] { (void)docA.documentLayout()->documentSize(); }));
    // Документы объявлены ПОЗЖЕ видов и умрут раньше — вид не должен на них
    // смотреть в момент своей смерти (падение на выходе в первой редакции).
    viewA->setDocument(nullptr);
    viewB->setDocument(nullptr);
    view.setDocument(nullptr);
    return 0;
}
