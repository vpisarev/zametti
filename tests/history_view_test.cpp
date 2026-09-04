// Режим истории целиком — контроллер, вид разности и список записей поверх
// ZNoteTimeline, на живом редакторе в окне С СОСЕДОМ.
//
// Что здесь проверяется (сессия 7, вид один — markdown построчно):
//   * вход/выход/шаги: живой буфер редактора не трогается ни на байт;
//   * убранные строки видны своим текстом, «удалено: N» нет; «+»/«−» на поле
//     И ПРАВДА НАРИСОВАНЫ (по пикселям), а не только помечены;
//   * F4 ходит по изменениям и ставит их в золотое сечение — и без фокуса в
//     тексте; Ctrl+Z/Ctrl+Shift+Z шагают по слепкам; Esc выводит; печатающая
//     клавиша ничего не восстанавливает и говорит об этом вслух;
//   * смена базы и переход к другому слепку держат место; масштаб не стирает
//     слепок; база «со свежей» и «с предыдущей» — разные сравнения;
//   * восстановление кладёт в заметку слепок, а не документ разности;
//   * копирование из слепка — сырые строки markdown;
//   * на корпусе: документ разности согласован со сравнением построчно.
//
// Снимки приёмки — в каталог набора (широкое и узкое окно).

#include "diff.h"
#include "zstorage.h"
#include "editor_widget.h"
#include "history_controller.h"
#include "history_panel.h"
#include "history_view.h"
#include "journal.h"
#include "key_binding.h"
#include "settings.h"
#include "zapp.h"

#include "test_util.h"
#include "testdata.h"

#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QKeyEvent>
#include <QListWidget>
#include <QScrollBar>
#include <QTemporaryDir>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QTest>

#include <cmath>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextFrame>
#include <QVBoxLayout>

#include <set>
#include <string>
#include <vector>

using namespace zametti;

namespace {

template <typename T>
std::string num(T value) { return std::to_string(value); }

QString g_root;
QString g_corpus;

// Заметка с историей: две версии, обе в журнале, файл — вторая. Возвращает путь.
QString makeNoteWithHistory(const QString& id, const QByteArray& first,
                            const QByteArray& second) {
    QDir().mkpath(g_root + QStringLiteral("/.zametti"));
    QDir().mkpath(g_root + QStringLiteral("/history"));
    const QString path = g_root + QLatin1Char('/') + id + QStringLiteral(".md");
    QFile file(path);
    if (file.open(QIODevice::WriteOnly)) file.write(second);
    file.close();

    ZStorage history(g_root);
    QString error;
    const qint64 now = 1'700'000'000'000LL;
    history.appendToJournal(id, zametti::ZJournal::NewRecord::save(first, ZJournal::Stamp::at(now)), &error);
    history.appendToJournal(id, zametti::ZJournal::NewRecord::save(second, ZJournal::Stamp::at(now + 60'000)), &error);
    return path;
}

QByteArray note(const char* body, const char* stamp) {
    return QByteArray("<!-- zametti\ncreated: 2026-01-01T00:00:00Z\nmodified: ") + stamp +
           "\n-->\n\n" + body;
}

// Окно режима: редактор, вид истории, список записей и сосед, которому мог бы
// достаться фокус, — ровно то, что стоит в живом окне. Контроллер связывает
// их так же, как main().
struct Rig {
    QWidget window;
    QListWidget* neighbour;
    NoteEditor* editor;
    HistoryView* view;
    HistoryTimeline* list;   // список записей — внутри вида истории
    HistoryController controller;

    Rig()
        : neighbour(new QListWidget(&window)),
          editor(new NoteEditor(&window)),
          view(new HistoryView(&window)),
          list(&view->list()),
          controller(*editor, *view) {
        auto* layout = new QVBoxLayout(&window);
        layout->addWidget(neighbour);
        layout->addWidget(editor, 1);
        layout->addWidget(view, 1);
        neighbour->addItem(QStringLiteral("сосед, которому достался бы фокус"));
        editor->setStoreRoot(g_root);
        window.resize(900, 700);
    }
    bool open(const QString& path, int index = -1) {
        editor->openFile(path);
        return controller.enter(index);
    }
    DiffTextView& text() { return view->textView(); }
    std::shared_ptr<ZNoteTimeline> tl() { return controller.timeline(); }
    QStringList lines() {
        QStringList out;
        for (QTextBlock b = text().document()->begin(); b.isValid(); b = b.next())
            out.append(b.text());
        return out;
    }
    void show() {
        window.show();
        window.activateWindow();
        window.raise();
        (void)QTest::qWaitForWindowActive(&window, 1000);
        QApplication::processEvents();
    }
};

void checkBasics() {
    const QString path = makeNoteWithHistory(
        QStringLiteral("01dddddddddd01"),
        note("# Заголовок\n\nПервый абзац.\n\nВторой абзац.\n\n- пункт\n- ещё пункт\n", "a"),
        note("# Заголовок\n\nПервый абзац поправленный.\n\n- пункт\n- ещё пункт\n"
             "- третий пункт\n", "b"));

    Rig rig;
    rig.editor->openFile(path);
    const QString liveBefore = rig.editor->document()->toPlainText();
    ZT_TRUE("вошли в историю", rig.controller.enter());
    ZT_TRUE("режим идёт", rig.controller.active());
    ZT_TRUE("сравнение нашло изменения: " + num(rig.tl()->changedLines()),
            rig.tl()->changedLines() > 0);
    ZT_EQ("живой буфер не тронут", liveBefore.toStdString(),
          rig.editor->document()->toPlainText().toStdString());
    ZT_TRUE("редактор не переводился в «только чтение»", !rig.editor->isReadOnly());
    // «+m/−n» В БАННЕРЕ — своей надписью, числа те же, что у сравнения. Хвостом
    // строки слепка счёт резался многоточием первым (снимок владельца). +m на
    // фоне diff.added, −n — diff.removed (просьба владельца): надпись — rich
    // text, проверяем и числа, и цвета.
    {
        const QString plus = QStringLiteral("+%1").arg(rig.tl()->addedLines());
        const QString minus = QStringLiteral("−%1").arg(rig.tl()->removedLines());
        ZT_TRUE("есть и добавленные, и убранные: " + (plus + minus).toStdString(),
                rig.tl()->addedLines() > 0 && rig.tl()->removedLines() > 0);
        QLabel* counts = nullptr;
        for (QLabel* label : rig.view->findChildren<QLabel*>())
            if (label->text().contains(plus) && label->text().contains(minus)) counts = label;
        ZT_TRUE("баннер показывает " + (plus + "/" + minus).toStdString(), counts != nullptr);
        if (counts != nullptr) {
            const QString html = counts->text();
            const int plusAt = int(html.indexOf(plus));
            const int minusAt = int(html.indexOf(minus));
            ZT_TRUE("+m раньше −n", plusAt >= 0 && minusAt > plusAt);
            // Фон — цвет разности, РАЗБАВЛЕННЫЙ как у строк на поле (diffTint
            // поверх фона), а не сырой: сырой diff.added густо-зелёный, и
            // владелец увидел его на снимке. Достаём цвет из background-color
            // перед числом и сверяем: того же оттенка, но светлее сырого.
            const auto groundOf = [&](const QString& piece) {
                const int at = int(piece.lastIndexOf(QStringLiteral("background-color:")));
                return at < 0 ? QColor() : QColor(piece.mid(at + 17, 7));
            };
            const QColor plusGround = groundOf(html.left(plusAt));
            const QColor minusGround = groundOf(html.mid(plusAt, minusAt - plusAt));
            const QColor added = settings().style().diffAdded();
            const QColor removed = settings().style().diffRemoved();
            ZT_TRUE("+m на фоне: зелень разбавлена, а не сырая",
                    plusGround.isValid() && plusGround != added &&
                        plusGround.lightness() > added.lightness() &&
                        plusGround.green() >= plusGround.red());
            ZT_TRUE("−n на фоне: краснота разбавлена, а не сырая",
                    minusGround.isValid() && minusGround != removed &&
                        minusGround.lightness() > removed.lightness() &&
                        minusGround.red() >= minusGround.green());
        }
        // Секции через «·», как в полосе сведений: точка перед счётом видна.
        int dots = 0;
        for (QLabel* label : rig.view->findChildren<QLabel*>())
            if (label->text() == QStringLiteral("·") && label->isVisibleTo(rig.view)) ++dots;
        ZT_EQ("две точки-разделителя: перед «+m/−n» и перед счётом кусков", num(2), num(dots));
        // Строка даты — «Date: …», а не «Snapshot from …» (просьба владельца).
        bool dated = false;
        for (QLabel* label : rig.view->findChildren<QLabel*>())
            if (label->toolTip().startsWith(QStringLiteral("Date: "))) dated = true;
        ZT_TRUE("строка слепка начинается с «Date: »", dated);
        // Месяц — тремя буквами, и в строке, и в списке (одна функция на обоих).
        // Время второй записи фикстуры (makeNoteWithHistory): ноябрь 2023 в
        // любом часовом поясе.
        const QString moment = historyMoment(1'700'000'060'000LL);
        ZT_TRUE("месяц коротко: " + moment.toStdString(),
                moment.contains(QStringLiteral("Nov")) && !moment.contains(QStringLiteral("November")));
        bool listed = false;
        for (int row = 0; row < rig.list->findChild<QListWidget*>()->count(); ++row)
            if (rig.list->findChild<QListWidget*>()->item(row)->text().startsWith(moment)) listed = true;
        ZT_TRUE("и в списке записей та же строка", listed);
    }

    // УБРАННЫЕ СТРОКИ ВИДНЫ СВОИМ ТЕКСТОМ. «Второй абзац.» исчез — он в
    // документе разности красным, а сводки «удалено: N» нет.
    const QStringList lines = rig.lines();
    ZT_TRUE("убранная строка показана", lines.contains(QStringLiteral("Второй абзац.")));
    ZT_TRUE("сводки «удалено:» нет", lines.filter(QStringLiteral("removed:")).isEmpty());
    const int gone = int(lines.indexOf(QStringLiteral("Второй абзац.")));
    ZT_TRUE("и она помечена убранной",
            gone >= 0 && rig.tl()->markOfBlock(gone) == diff::Mark::Removed);
    // Изменённая строка — парой: старая, за ней новая.
    const int was = int(lines.indexOf(QStringLiteral("Первый абзац.")));
    const int now = int(lines.indexOf(QStringLiteral("Первый абзац поправленный.")));
    ZT_TRUE("изменённая строка — пара «− старая / + новая»", was >= 0 && now == was + 1);

    // ХОДЬБА ПО ИЗМЕНЕНИЯМ. Первый шаг обязан привести на изменённый блок,
    // а круг — вернуть на то же место.
    QTextCursor top(rig.text().document());
    top.setPosition(0);
    rig.text().setTextCursor(top);
    ZT_TRUE("шаг к изменению удался", rig.text().stepChange(true));
    const int first = rig.text().textCursor().blockNumber();
    ZT_TRUE("и он привёл на изменённое место",
            rig.tl()->markOfBlock(first) != diff::Mark::Same);
    int steps = 0;
    while (steps < 20) {
        rig.text().stepChange(true);
        ++steps;
        if (rig.text().textCursor().blockNumber() == first) break;
    }
    ZT_TRUE("ходьба идёт по кругу: шагов " + num(steps), steps < 20);

    rig.controller.leave();
    ZT_TRUE("вышли", !rig.controller.active());
    ZT_EQ("и живой буфер по-прежнему тот же", liveBefore.toStdString(),
          rig.editor->document()->toPlainText().toStdString());
}

// «+»/«−» НАРИСОВАНЫ на поле — по пикселям, а не по меткам: метка без глифа
// была бы обещанием, которого человек не видит.
void checkGutterIsPainted() {
    const QString path = makeNoteWithHistory(
        QStringLiteral("01dddddddddd02"),
        note("# Заголовок\n\nбыло вот так\n\nостаётся\n", "a"),
        note("# Заголовок\n\nстало иначе\n\nостаётся\n", "b"));
    Rig rig;
    rig.show();
    ZT_TRUE("вошли в историю", rig.open(path));
    rig.editor->hide();
    rig.neighbour->hide();
    QApplication::processEvents();
    DiffTextView& text = rig.text();
    const QImage shot = text.viewport()->grab().toImage();

    const QAbstractTextDocumentLayout* layout = text.document()->documentLayout();
    // Поле под глиф — прямо слева от прямоугольника блока (он начинается за
    // полем корневой рамки).
    const qreal gutter = ZDocument::diffGutterWidth(text.docStyle());
    const int scroll = text.verticalScrollBar()->value();
    // Чернила глифа сглажены и смешаны с фоном: считаем пиксель «своим», если
    // он ближе к цвету метки, чем к бумаге, — и заметно ближе.
    const QColor page = text.palette().color(QPalette::Base);
    const auto distance = [](const QColor& a, const QColor& b) {
        return qAbs(a.red() - b.red()) + qAbs(a.green() - b.green()) + qAbs(a.blue() - b.blue());
    };
    const auto inkOf = [&](int blockNumber, const QColor& want) {
        const QTextBlock block = text.document()->findBlockByNumber(blockNumber);
        if (!block.isValid()) return 0;
        const QRectF rect = layout->blockBoundingRect(block);
        int hits = 0;
        for (int y = int(rect.top()) - scroll; y < int(rect.bottom()) - scroll; ++y)
            for (int x = int(rect.left() - gutter); x < int(rect.left()); ++x) {
                if (x < 0 || y < 0 || x >= shot.width() || y >= shot.height()) continue;
                const QColor c = shot.pixelColor(x, y);
                if (distance(c, want) + 60 < distance(c, page)) ++hits;
            }
        return hits;
    };
    const QStringList lines = rig.lines();
    const int gone = int(lines.indexOf(QStringLiteral("было вот так")));
    const int came = int(lines.indexOf(QStringLiteral("стало иначе")));
    const int same = int(lines.indexOf(QStringLiteral("остаётся")));
    ZT_TRUE("у убранной строки на поле красный «−»: пикселей " + num(inkOf(gone, text.docStyle().diffRemoved())),
            gone >= 0 && inkOf(gone, text.docStyle().diffRemoved()) > 0);
    ZT_TRUE("у добавленной — зелёный «+»: пикселей " + num(inkOf(came, text.docStyle().diffAdded())),
            came >= 0 && inkOf(came, text.docStyle().diffAdded()) > 0);
    ZT_TRUE("у нетронутой поле пустое",
            same >= 0 && inkOf(same, text.docStyle().diffAdded()) == 0 &&
                inkOf(same, text.docStyle().diffRemoved()) == 0);

    // F4: ТЕКУЩИЙ КУСОК — ОРАНЖЕВОЙ ПОЛОСОЙ левее глифов (просьба владельца:
    // выделение забивало бы заливку строк, а выделять мышью нужно для
    // копирования). До F4 полосы нет, после — есть у обеих строк куска и нет у
    // нетронутой; выделения F4 не делает.
    const QColor orange = text.docStyle().diffChanged();
    const auto barOf = [&](const QImage& img, int blockNumber) {
        const QTextBlock block = text.document()->findBlockByNumber(blockNumber);
        if (!block.isValid()) return 0;
        const QRectF rect = layout->blockBoundingRect(block);
        int hits = 0;
        for (int y = int(rect.top()) - scroll; y < int(rect.bottom()) - scroll; ++y)
            for (int x = int(rect.left() - gutter); x < int(rect.left() - gutter + gutter / 3.0); ++x) {
                if (x < 0 || y < 0 || x >= img.width() || y >= img.height()) continue;
                const QColor c = img.pixelColor(x, y);
                if (distance(c, orange) + 60 < distance(c, page)) ++hits;
            }
        return hits;
    };
    ZT_TRUE("до F4 полосы нет", barOf(shot, gone) == 0 && barOf(shot, came) == 0);
    QTextCursor top(text.document());
    top.setPosition(0);
    text.setTextCursor(top);
    ZT_TRUE("F4 привёл к куску", text.stepChange(true));
    ZT_TRUE("выделения F4 не делает", !text.textCursor().hasSelection());
    const DiffTextView::Hunk hunk = text.currentHunk();
    ZT_TRUE("кусок — обе строки «− старая / + новая»: " + num(hunk.first) + ".." + num(hunk.last),
            hunk.first == gone && hunk.last == came);
    QApplication::processEvents();
    const QImage after = text.viewport()->grab().toImage();
    ZT_TRUE("полоса у убранной строки куска: пикселей " + num(barOf(after, gone)), barOf(after, gone) > 0);
    ZT_TRUE("и у добавленной", barOf(after, came) > 0);
    ZT_TRUE("а у нетронутой нет", barOf(after, same) == 0);
    ZT_TRUE("кусок «−/+» — оранжевый", hunk.kind == diff::Mark::Changed);
    rig.controller.leave();

    // Цвет полосы по составу куска: только добавили — зелёная, только убрали —
    // красная.
    const QString path2 = makeNoteWithHistory(
        QStringLiteral("01dddddddddd03"),
        note("# Заголовок\n\nодин\n\nдва\n\nтри\n\nчетыре\n", "a"),
        note("# Заголовок\n\nодин\n\nпришло\n\nдва\n\nчетыре\n", "b"));
    Rig rig2;
    rig2.show();
    ZT_TRUE("вошли в историю", rig2.open(path2));
    rig2.editor->hide();
    rig2.neighbour->hide();
    QApplication::processEvents();
    DiffTextView& text2 = rig2.text();
    QTextCursor top2(text2.document());
    top2.setPosition(0);
    text2.setTextCursor(top2);
    // СЧЁТ ОТЛИЧИЙ «m/n» (просьба владельца): без него по F4 не видно ни
    // сколько их всего, ни далеко ли до конца. Счёт обязан совпадать с
    // настоящей ходьбой — потому и спрашивается на каждом шаге, а не отдельно.
    ZT_EQ("отличий в этой разности — два", "2", num(text2.hunkTotal()));
    ZT_EQ("до F4 мы ни на одном", "0", num(text2.hunkIndex()));
    ZT_TRUE("F4 — к первому куску", text2.stepChange(true));
    const DiffTextView::Hunk added = text2.currentHunk();
    ZT_TRUE("первый кусок — только добавленное", added.kind == diff::Mark::Added);
    ZT_EQ("и счёт говорит «1»", "1", num(text2.hunkIndex()));
    ZT_TRUE("F4 — ко второму куску", text2.stepChange(true));
    const DiffTextView::Hunk removed = text2.currentHunk();
    ZT_TRUE("второй кусок — только убранное", removed.kind == diff::Mark::Removed);
    ZT_TRUE("и это другой кусок", removed.first != added.first);
    ZT_EQ("счёт говорит «2»", "2", num(text2.hunkIndex()));
    // По кругу — назад к первому: пиксели зелёные, красных нет.
    ZT_TRUE("F4 — по кругу к первому", text2.stepChange(true));
    ZT_TRUE("снова первый", text2.currentHunk().first == added.first);
    ZT_EQ("и счёт вернулся к «1»", "1", num(text2.hunkIndex()));
    // Shift+F4 (шаг назад) считает так же: круг в другую сторону.
    ZT_TRUE("Shift+F4 — назад по кругу", text2.stepChange(false));
    ZT_EQ("счёт — последний", num(text2.hunkTotal()), num(text2.hunkIndex()));

    // НАДПИСЬ В БАННЕРЕ — то, что человек видит. Спрашиваем сам виджет: сигнал
    // и связь с ним тоже часть починки, и без этого проверка мерила бы только
    // счётчик.
    {
        const QList<QLabel*> labels = rig2.view->findChildren<QLabel*>();
        QString shown;
        for (QLabel* label : labels)
            if (label->text().contains(QLatin1Char('/')) &&
                label->text().contains(QString::fromStdString(num(text2.hunkTotal()))))
                shown = label->text();
        ZT_EQ("баннер показывает счёт отличий",
              num(text2.hunkIndex()) + "/" + num(text2.hunkTotal()), shown.toStdString());
    }
    // Возвращаемся на первый кусок: следующая проверка меряет ЕГО полосу по
    // пикселям, и оставить каретку на другом значило бы мерить не то.
    ZT_TRUE("F4 — снова на первый", text2.stepChange(true));
    ZT_TRUE("и это он", text2.currentHunk().first == added.first);
    QApplication::processEvents();
    const QImage shot2 = text2.viewport()->grab().toImage();
    const auto barColourOf = [&](int blockNumber, const QColor& want) {
        const QTextBlock block = text2.document()->findBlockByNumber(blockNumber);
        if (!block.isValid()) return 0;
        const QRectF rect = text2.document()->documentLayout()->blockBoundingRect(block);
        const qreal g = ZDocument::diffGutterWidth(text2.docStyle());
        const int sc = text2.verticalScrollBar()->value();
        int hits = 0;
        for (int y = int(rect.top()) - sc; y < int(rect.bottom()) - sc; ++y)
            for (int x = int(rect.left() - g); x < int(rect.left() - g + g / 3.0); ++x) {
                if (x < 0 || y < 0 || x >= shot2.width() || y >= shot2.height()) continue;
                // Ближайший из четырёх цветов: бумага, зелёный, красный,
                // оранжевый — иначе зелёный сошёл бы за «ближе к оранжевому,
                // чем к бумаге».
                const QColor c = shot2.pixelColor(x, y);
                const QColor candidates[] = {page, text2.docStyle().diffAdded(),
                                             text2.docStyle().diffRemoved(),
                                             text2.docStyle().diffChanged()};
                QColor best = page;
                for (const QColor& k : candidates)
                    if (distance(c, k) < distance(c, best)) best = k;
                if (best == want && best != page) ++hits;
            }
        return hits;
    };
    ZT_TRUE("полоса у добавленного — зелёная", barColourOf(added.first, text2.docStyle().diffAdded()) > 0);
    ZT_TRUE("и не оранжевая", barColourOf(added.first, text2.docStyle().diffChanged()) == 0);

    rig2.controller.leave();
}

void checkKeysAreWired() {
    const QString path = makeNoteWithHistory(
        QStringLiteral("01dddddddddd05"),
        note("# Заголовок\n\nбыло вот так\n", "a"),
        note("# Заголовок\n\nстало иначе\n", "b"));

    // ОКНО С СОСЕДОМ и активное: ярлыки окна (QShortcut) срабатывают только в
    // активном окне; под голым Xvfb, где нет оконного менеджера, окно само
    // активным не становится, и проверка краснела бы там, где всё в порядке.
    Rig rig;
    rig.show();
    rig.controller.installShortcuts(&rig.window);   // ровно то, что делает окно
    ZT_TRUE("вошли в историю", rig.open(path));
    rig.view->setFocus();
    ZT_TRUE("фокус в тексте разности", rig.window.focusWidget() == &rig.text());

    // F4 — из конфига, поэтому нажимаем не «F4», а то, что там записано. И с
    // фокусом У СОСЕДА: владелец — «встаёшь на слепок в списке справа — F4 не
    // работает».
    // Ключ — СПИСОК через точку с запятой (F4 и Ctrl+] — на маке F4 без fn не
    // нажать), и работать обязано КАЖДОЕ сочетание из него, а не первое.
    const QList<QKeySequence> nexts = keySequencesOf(settings().editor().diffNextKey());
    ZT_TRUE("сочетаний для ходьбы по изменениям не меньше двух: " + num(nexts.size()),
            nexts.size() >= 2);
    for (const QKeySequence& next : nexts) {
        ZT_TRUE("сочетание разобрано: " + next.toString().toStdString(), next.count() > 0);
        QTextCursor top(rig.text().document());
        top.setPosition(0);
        rig.text().setTextCursor(top);
        rig.neighbour->setFocus();
        ZT_TRUE("фокус у соседа", rig.window.focusWidget() == rig.neighbour);
        QTest::keyClick(&rig.window, Qt::Key(next[0].key()), next[0].keyboardModifiers());
        const int at = rig.text().textCursor().blockNumber();
        ZT_TRUE(next.toString().toStdString() +
                    ": шаг по изменениям сработал клавишей и без фокуса в тексте",
                rig.tl()->markOfBlock(at) != diff::Mark::Same);
    }

    // Ctrl+Z / Ctrl+Shift+Z в тексте разности — шаги по слепкам; Esc — выход.
    rig.text().setFocus();
    ZT_EQ("показан последний слепок", num(1), num(rig.controller.index()));
    QTest::keyClick(&rig.text(), Qt::Key_Z, Qt::ControlModifier);
    ZT_EQ("Ctrl+Z — шаг в прошлое", num(0), num(rig.controller.index()));
    QTest::keyClick(&rig.text(), Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
    ZT_EQ("Ctrl+Shift+Z — шаг в будущее", num(1), num(rig.controller.index()));

    // Печатающая клавиша не восстанавливает и не правит — говорит вслух.
    int refusals = 0;
    QObject::connect(&rig.controller, &HistoryController::editRefused, [&refusals] { ++refusals; });
    const QString shown = rig.text().document()->toPlainText();
    QTest::keyClicks(&rig.text(), QStringLiteral("x"));
    QTest::qWait(10);
    ZT_EQ("печатающая клавиша слепок не меняет", shown.toStdString(),
          rig.text().document()->toPlainText().toStdString());
    ZT_EQ("и про отказ сказано вслух", num(1), num(refusals));

    // Смена базы часто и с прокруткой событий: документ подменяется из
    // обработчика — старый умирает, пока Qt ещё разбирается с событием
    // (падение владельца этапа 10). Стучим двенадцать раз.
    for (int i = 0; i < 12; ++i) {
        rig.controller.setBaseFresh(i % 2 == 0);
        QApplication::processEvents();
    }
    ZT_TRUE("двенадцать переключений подряд программу не уронили", true);

    QTest::keyClick(&rig.text(), Qt::Key_Escape);
    ZT_TRUE("Esc вывел из режима", !rig.controller.active());
    ZT_TRUE("фокус не улетел к соседу", rig.window.focusWidget() != rig.neighbour);
}

// F4 ставит изменение в ЗОЛОТОЕ СЕЧЕНИЕ окна, а не у нижней кромки
// (владелец: «скроллится до самой ранней позиции — цветная полоска в самом
// низу»; ensureCursorVisible прокручивает МИНИМАЛЬНО).
void checkStepLandsInGolden() {
    QString before = QStringLiteral("# Длинная\n\n");
    for (int i = 0; i < 200; ++i) before += QStringLiteral("строка номер %1\n\n").arg(i);
    QString after = before;
    after.replace(QStringLiteral("строка номер 150\n"),
                  QStringLiteral("строка номер 150 поправленная\n"));
    const QString path = makeNoteWithHistory(
        QStringLiteral("01dddddddddd06"),
        note(before.toUtf8().constData(), "a"), note(after.toUtf8().constData(), "b"));

    Rig rig;
    rig.show();
    ZT_TRUE("вошли в историю", rig.open(path));
    rig.editor->hide();   // как в окне: место отдано виду истории
    QApplication::processEvents();
    DiffTextView& text = rig.text();
    QTextCursor top(text.document());
    top.setPosition(0);
    text.setTextCursor(top);
    text.verticalScrollBar()->setValue(0);
    ZT_TRUE("шаг к изменению удался", text.stepChange(true));

    const QTextBlock block = text.textCursor().block();
    const qreal y = text.document()->documentLayout()->blockBoundingRect(block).top() -
                    text.verticalScrollBar()->value();
    const qreal height = text.viewport()->height();
    ZT_TRUE("изменение оказалось не у кромки, а около золотого сечения: " +
                num(int(y)) + " из " + num(int(height)),
            height > 0 && y > height * 0.2 && y < height * 0.6);
    rig.controller.leave();
}

// Переход к ДРУГОМУ слепку держит место примерно там же (просьба владельца).
void checkSnapshotSwitchKeepsPlace() {
    QString first = QStringLiteral("# Длинная\n\n");
    for (int i = 0; i < 150; ++i) first += QStringLiteral("строка номер %1\n\n").arg(i);
    QString second = first;
    second.replace(QStringLiteral("строка номер 80\n"),
                   QStringLiteral("строка номер 80 поправленная\n"));
    const QString path = makeNoteWithHistory(
        QStringLiteral("01dddddddddd08"),
        note(first.toUtf8().constData(), "a"), note(second.toUtf8().constData(), "b"));

    Rig rig;
    rig.show();
    ZT_TRUE("вошли в историю", rig.open(path));
    rig.editor->hide();
    QApplication::processEvents();
    DiffTextView& text = rig.text();
    QTextCursor top(text.document());
    top.setPosition(0);
    text.setTextCursor(top);
    ZT_TRUE("встали на изменение", text.stepChange(true));
    const int scrollBefore = text.verticalScrollBar()->value();
    ZT_TRUE("и это не начало: прокрутка " + num(scrollBefore), scrollBefore > 0);

    ZT_TRUE("перешли к другому слепку", rig.controller.select(0));
    const int scrollAfter = text.verticalScrollBar()->value();
    ZT_TRUE("вид остался примерно там же: было " + num(scrollBefore) + ", стало " +
                num(scrollAfter),
            scrollAfter > scrollBefore / 2);
    rig.controller.leave();
}

// СМЕНА БАЗЫ ДЕРЖИТ МЕСТО — ТО, ЧТО НА ЭКРАНЕ, а не каретку: читая, человек
// крутит колесо, каретка стоит в начале (владелец: «убегает на начало»).
void checkBaseSwitchKeepsPlace() {
    QString before = QStringLiteral("# Длинная\n\n");
    for (int i = 0; i < 150; ++i) before += QStringLiteral("строка номер %1\n\n").arg(i);
    QString after = before;
    after.replace(QStringLiteral("строка номер 70\n"),
                  QStringLiteral("строка номер 70 поправленная\n"));
    const QString path = makeNoteWithHistory(
        QStringLiteral("01dddddddddd13"),
        note(before.toUtf8().constData(), "a"), note(after.toUtf8().constData(), "b"));

    Rig rig;
    rig.show();
    ZT_TRUE("вошли в историю", rig.open(path));
    rig.editor->hide();
    QApplication::processEvents();
    DiffTextView& text = rig.text();
    QTextCursor top(text.document());
    top.setPosition(0);
    text.setTextCursor(top);
    text.verticalScrollBar()->setValue(text.verticalScrollBar()->maximum() / 2);
    QApplication::processEvents();
    const int scrollBefore = text.verticalScrollBar()->value();
    ZT_TRUE("прокрутили в середину: " + num(scrollBefore), scrollBefore > 100);
    ZT_TRUE("а каретка осталась в начале — это и есть случай владельца",
            text.textCursor().position() == 0);

    rig.controller.setBaseFresh(true);
    const int scrollFresh = text.verticalScrollBar()->value();
    ZT_TRUE("со свежей базой место то же: было " + num(scrollBefore) + ", стало " +
                num(scrollFresh),
            qAbs(scrollFresh - scrollBefore) <= 40);
    rig.controller.setBaseFresh(false);
    const int scrollBack = text.verticalScrollBar()->value();
    ZT_TRUE("и обратно на то же место: " + num(scrollBack), qAbs(scrollBack - scrollBefore) <= 40);
    rig.controller.leave();
}

// ВОССТАНОВЛЕНИЕ КЛАДЁТ В ЗАМЕТКУ СЛЕПОК, А НЕ ДОКУМЕНТ РАЗНОСТИ. В документе
// разности убранные строки видны — попади он в заметку, они бы «вернулись»
// (класс инцидента №15: показанное утекло в живую заметку).
void checkRestoreWritesSnapshot() {
    const QString path = makeNoteWithHistory(
        QStringLiteral("01dddddddddd12"),
        note("# Заголовок\n\nпервый\n\nвторой\n", "a"),
        note("# Заголовок\n\nпервый\n", "b"));
    Rig rig;
    ZT_TRUE("вошли в историю", rig.open(path));
    ZT_TRUE("человек видит убранную строку",
            rig.text().document()->toPlainText().contains(QStringLiteral("второй")));
    // А сам слепок её не содержит — и восстановление берёт именно его.
    const std::string snapshot = rig.tl()->snapshotBody();
    ZT_TRUE("в слепке убранной строки нет", snapshot.find("второй") == std::string::npos);
    ZT_TRUE("а его текст на месте", snapshot.find("первый") != std::string::npos);

    // Восстановление ПОСЛЕДНЕГО слепка = нынешняя версия: режим закрывается,
    // а сказано «этот слепок и есть нынешняя версия».
    bool alreadyCurrent = false;
    const qint64 same = rig.controller.restore(&alreadyCurrent);
    ZT_TRUE("последний слепок и есть нынешняя версия", same == 0 && alreadyCurrent);
    ZT_TRUE("режим закрылся", !rig.controller.active());
    const QString live = rig.editor->document()->toPlainText();
    ZT_TRUE("в заметке нет строки из документа разности", !live.contains(QStringLiteral("второй")));

    // Восстановление ПЕРВОГО — тело первой записи, одной правкой.
    ZT_TRUE("вошли на первый слепок", rig.controller.enter(0));
    const int undoBefore = rig.editor->undoSteps();
    const qint64 source = rig.controller.restore(&alreadyCurrent);
    ZT_TRUE("восстановлено из первой записи", source > 0 && !alreadyCurrent);
    ZT_TRUE("режим закрылся", !rig.controller.active());
    ZT_TRUE("в заметке вернулась строка первой записи",
            rig.editor->document()->toPlainText().contains(QStringLiteral("второй")));
    ZT_TRUE("цепочка отмены не пуста", rig.editor->undoSteps() > undoBefore);
    rig.editor->undo();
    ZT_TRUE("Ctrl+Z отменяет восстановление",
            !rig.editor->document()->toPlainText().contains(QStringLiteral("второй")));
}

// Смена масштаба в режиме истории не стирает слепок и не теряет метки.
void checkZoomKeepsSnapshot() {
    const QString path = makeNoteWithHistory(
        QStringLiteral("01dddddddddd07"),
        note("# Заголовок\n\nбыло вот так\n", "a"),
        note("# Заголовок\n\nстало иначе\n", "b"));
    Rig rig;
    rig.show();
    ZT_TRUE("вошли в историю", rig.open(path));
    ZT_TRUE("слепок показан",
            rig.text().document()->toPlainText().contains(QStringLiteral("стало иначе")));
    rig.text().applyZoom(1.4);
    ZT_TRUE("после смены масштаба слепок на месте, а не чистый лист",
            rig.text().document()->toPlainText().contains(QStringLiteral("стало иначе")));
    bool anyMark = false;
    for (int i = 0; i < rig.text().document()->blockCount(); ++i)
        anyMark = anyMark || rig.tl()->markOfBlock(i) != diff::Mark::Same;
    ZT_TRUE("и метки не потерялись", anyMark);
    ZT_TRUE("масштаб применился", rig.text().zoom() > 1.3);

    // МАСШТАБ РАЗНОСТИ ЖИВУЮ ЗАМЕТКУ НЕ ТРОГАЕТ (беда владельца 27.08.2026:
    // «Ctrl+− в истории — а при выходе текст заметки стал меньше»). Ступень у
    // разности теперь общая с плоскими видами (sourceZoom), но инвариант тот
    // же: вид разности и вид заметки держат каждый свой кегль, и проверяется
    // это в обе стороны.
    const qreal noteZoom = rig.editor->zoom();
    ZT_TRUE("заметка своего масштаба не меняла", qFuzzyCompare(noteZoom, rig.editor->zoom()));
    rig.text().applyZoom(1.8);
    ZT_TRUE("разность увеличилась", rig.text().zoom() > 1.7);
    ZT_TRUE("а заметка осталась как была", qFuzzyCompare(rig.editor->zoom(), noteZoom));
    rig.controller.leave();
    ZT_TRUE("и после выхода из режима — тоже",
            qFuzzyCompare(rig.editor->zoom(), noteZoom));
    rig.editor->applyZoom(1.3);
    ZT_TRUE("заметка увеличилась", rig.editor->zoom() > noteZoom);
    ZT_TRUE("а разность осталась со своим", qFuzzyCompare(rig.text().zoom(), 1.8));

    // КЕГЛЬ РАЗНОСТИ — МОНОШИРИННЫЙ ПЛОСКИХ ВИДОВ (решение владельца,
    // 02.09.2026): codeFamily × monospacePoint × масштаб, как у правки
    // исходника, а не baseFontPoint со ступенью. Тест обязан краснеть при
    // снятой починке: прежний путь давал baseFontPoint × 1.2.
    {
        const zametti::ZDocStyle& look = zametti::settings().style();
        const QFont shown = rig.text().document()->defaultFont();
        ZT_EQ("гарнитура разности — кода", look.codeFamily().toStdString(),
              (shown.families().isEmpty() ? shown.family() : shown.families().first())
                  .toStdString());
        ZT_TRUE("кегль разности = monospacePoint × масштаб",
                qFuzzyCompare(shown.pointSizeF(), look.monospacePoint() * 1.8));
    }
    rig.text().applyZoom(1.0);
}

// Переключатель базы и вправду меняет сравнение: со свежей версией у последней
// записи разницы нет вовсе, а с предыдущей — есть.
void checkBaseSwitch() {
    const QString path = makeNoteWithHistory(
        QStringLiteral("01dddddddddd04"),
        note("# Заголовок\n\nстарое\n", "a"),
        note("# Заголовок\n\nновое\n", "b"));
    Rig rig;
    ZT_TRUE("вошли в историю", rig.open(path));
    ZT_TRUE("с предыдущей записью разница есть: " + num(rig.tl()->changedLines()),
            rig.tl()->changedLines() > 0);
    ZT_TRUE("баннер показывает базу «с предыдущей»", !rig.tl()->baseIsFresh());
    rig.controller.setBaseFresh(true);
    ZT_EQ("со свежей версией у последнего слепка разницы нет", num(0),
          num(rig.tl()->changedLines()));
    rig.controller.setBaseFresh(false);
    ZT_TRUE("вернули базу — вернулась и разница", rig.tl()->changedLines() > 0);
    rig.controller.leave();
}

// Копирование из слепка — сырые строки markdown, байт в байт: главный смысл
// режима — утащить кусок прошлого и вставить в живую заметку.
void checkCopyIsRawText() {
    const QString path = makeNoteWithHistory(
        QStringLiteral("01dddddddddd14"),
        note("# Заголовок\n\n- пункт\n", "a"),
        note("# Заголовок\n\n- пункт\n- ещё пункт\n", "b"));
    Rig rig;
    ZT_TRUE("вошли в историю", rig.open(path));
    const QStringList lines = rig.lines();
    const int at = int(lines.indexOf(QStringLiteral("- ещё пункт")));
    ZT_TRUE("строка есть", at >= 0);
    if (at < 0) return;
    QTextCursor select(rig.text().document()->findBlockByNumber(at));
    select.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
    rig.text().setTextCursor(select);
    rig.text().copy();
    ZT_EQ("в буфере — строка как в файле, без экранирования", std::string("- ещё пункт"),
          QApplication::clipboard()->text().toStdString());
    rig.controller.leave();
}

// Открытие другой заметки выводит из режима (правило одно на все двери).
void checkOpenLeaves() {
    const QString first = makeNoteWithHistory(
        QStringLiteral("01dddddddddd15"), note("# Один\n\nраз\n", "a"), note("# Один\n\nдва\n", "b"));
    const QString second = makeNoteWithHistory(
        QStringLiteral("01dddddddddd16"), note("# Два\n\nраз\n", "a"), note("# Два\n\nдва\n", "b"));
    Rig rig;
    ZT_TRUE("вошли в историю первой", rig.open(first));
    rig.editor->openFile(second);
    ZT_TRUE("открытие другой заметки вывело из режима", !rig.controller.active());
    ZT_TRUE("вид истории ни на что не смотрит", !rig.view->isAttached());
}

// На корпусе: документ разности построчно согласован со сравнением — каждая
// строка сравнения на месте, убранные и добавленные сходятся по счёту.
// ШРИФТ ОБОЛОЧКИ — У ВСЕГО ВИДА ИСТОРИИ, а не только у списка записей.
// Снимок владельца (мак, 04.09.2026): в одной строке баннера три гарнитуры —
// надпись шрифтом приложения, кнопки системным шрифтом Aqua, список своим.
// На маке у QPushButton классовый шрифт платформы, и общий
// QApplication::setFont его не перекрывает, — перекрывает только шрифт,
// поставленный виджету. Спрашиваем каждого ребёнка, который что-то пишет:
// надписи, кнопки, список — все обязаны отвечать одной гарнитурой и одним
// кеглем. Текст разности в счёт не идёт: у него кегль моноширинного.
void checkShellFont() {
    const QString path = makeNoteWithHistory(
        QStringLiteral("01ffffffffff01"), note("# Шрифт\n\nстрока\n", "a"),
        note("# Шрифт\n\nстрока другая\n", "b"));
    Rig rig;
    ZT_TRUE("вошли", rig.open(path));
    rig.show();

    QFont shell(QStringLiteral("IBM Plex Sans SemiCondensed"));
    shell.setPointSizeF(17.5);   // кегль, которого нет ни у кого по умолчанию
    rig.view->setShellFont(shell);
    QApplication::processEvents();

    int seen = 0;
    const auto probe = [&](QWidget* widget, const std::string& what) {
        if (widget->parentWidget() == nullptr || widget->window() != &rig.window) return;
        // Дети текста разности (полосы прокрутки, вьюпорт) — не оболочка.
        for (QWidget* up = widget; up != nullptr; up = up->parentWidget())
            if (up == &rig.text()) return;
        ++seen;
        const QFont got = widget->font();
        ZT_EQ(what + ": гарнитура", shell.family().toStdString(), got.family().toStdString());
        ZT_TRUE(what + ": кегль " + num(int(got.pointSizeF() * 10)),
                std::fabs(got.pointSizeF() - shell.pointSizeF()) < 0.01);
    };
    for (QLabel* label : rig.view->findChildren<QLabel*>())
        probe(label, "надпись «" + label->text().left(24).toStdString() + "»");
    for (QPushButton* button : rig.view->findChildren<QPushButton*>())
        probe(button, "кнопка «" + button->text().toStdString() + "»");
    for (QListWidget* list : rig.view->findChildren<QListWidget*>()) probe(list, "список записей");
    // Две надписи и четыре кнопки баннера, заголовок, крестик и список
    // таймлайна — меньше девяти значит, что кого-то не спросили.
    ZT_TRUE("спрошены все, кто пишет: " + num(seen), seen >= 9);

    // Надпись слепка после смены шрифта режется заново, а не остаётся с
    // многоточием от прежних метрик: полный текст хранится в подсказке.
    QLabel* snapshot = nullptr;
    for (QLabel* label : rig.view->findChildren<QLabel*>())
        if (label->toolTip().startsWith(QStringLiteral("Date:"))) snapshot = label;
    ZT_TRUE("надпись слепка найдена", snapshot != nullptr);
    if (snapshot != nullptr)
        ZT_TRUE("и её шрифт — оболочки",
                snapshot->fontMetrics().height() == QFontMetrics(shell).height());

    // ПОДСВЕТКА «RESTORE THIS ONE» (печатающая клавиша в слепке) ставит кнопке
    // stylesheet с рамкой — и шрифт кнопки обязан пережить это: stylesheet на
    // виджете отключает наследование, и без явного шрифта кнопка на 1,2 с
    // прыгала бы в системный.
    QTest::keyClick(&rig.text(), Qt::Key_X);
    QApplication::processEvents();
    for (QPushButton* button : rig.view->findChildren<QPushButton*>()) {
        if (button->text() != QStringLiteral("Restore this one")) continue;
        ZT_TRUE("подсветка стоит", !button->styleSheet().isEmpty());
        ZT_EQ("шрифт кнопки под подсветкой — оболочки", shell.family().toStdString(),
              button->font().family().toStdString());
        ZT_TRUE("и кегль тот же", std::fabs(button->font().pointSizeF() - 17.5) < 0.01);
    }
    rig.controller.leave();
}

// ВОПРОС ПЕРЕД ВОССТАНОВЛЕНИЕМ — второе исключение владельца из правила «без
// диалогов»: кнопка «Restore this one» сперва спрашивает «Put the snapshot
// from … on top of the undo stack?». «Нет» — заметка не тронута и режим идёт;
// «Да» — слепок восстановлен, режим закрыт. Диалог немодальный (open), и
// ответить ему можно его же кнопкой.
void checkRestoreAsks() {
    const QString path = makeNoteWithHistory(
        QStringLiteral("01aaaaaaaaaa02"), note("# Вопрос\n\nстарое\n", "a"),
        note("# Вопрос\n\nновое\n", "b"));
    Rig rig;
    ZT_TRUE("вошли", rig.open(path, 0));   // показан старый слепок
    rig.show();
    const QString liveBefore = rig.editor->document()->toPlainText();

    // Вопрос немодальный (open, не exec): после щелчка по кнопке он ждёт
    // ответа в общем цикле событий, и ответить ему можно его же кнопкой.
    const auto answerWith = [&](QMessageBox::StandardButton answer, const std::string& what) {
        for (QPushButton* button : rig.view->findChildren<QPushButton*>())
            if (button->text() == QStringLiteral("Restore this one")) button->click();
        QTest::qWait(30);
        QMessageBox* box = nullptr;
        for (QMessageBox* candidate : rig.window.findChildren<QMessageBox*>())
            if (candidate->isVisible()) box = candidate;
        ZT_TRUE(what + ": вопрос был задан", box != nullptr);
        if (box == nullptr) return;
        const QString asked = box->text();
        ZT_TRUE(what + ": вопрос называет дату слепка — " + asked.toStdString(),
                asked.startsWith(QStringLiteral("Put the snapshot from ")) &&
                    asked.contains(QStringLiteral(" on top of the undo stack")) &&
                    asked.endsWith(QLatin1Char('?')));
        // Кнопки — свои (YesRole / NoRole), не стандартные: ищем по роли.
        QPushButton* yesButton = nullptr;
        QPushButton* noButton = nullptr;
        for (QAbstractButton* candidate : box->buttons()) {
            if (box->buttonRole(candidate) == QMessageBox::YesRole)
                yesButton = qobject_cast<QPushButton*>(candidate);
            if (box->buttonRole(candidate) == QMessageBox::NoRole)
                noButton = qobject_cast<QPushButton*>(candidate);
        }
        ZT_TRUE(what + ": есть «да» и «нет»", yesButton != nullptr && noButton != nullptr);
        if (yesButton == nullptr || noButton == nullptr) return;
        ZT_TRUE(what + ": по умолчанию — «нет»", box->defaultButton() == noButton);
        // ОБЛИК ОДНОЙ ДВЕРИ (ZApp::messageBox): рисует Qt, без значка, шрифт
        // оболочки — так же выглядит и «Delete permanently?».
        ZT_TRUE(what + ": окно рисует Qt, не система",
                box->testOption(QMessageBox::Option::DontUseNativeDialog));
        ZT_TRUE(what + ": без значка", box->icon() == QMessageBox::NoIcon);
        ZT_EQ(what + ": шрифт оболочки", ZApp::instance().uiStyle().appFont().family().toStdString(),
              box->font().family().toStdString());
        (answer == QMessageBox::Yes ? yesButton : noButton)->click();
        QTest::qWait(30);
    };
    answerWith(QMessageBox::No, "отказ");
    ZT_TRUE("после отказа режим идёт", rig.controller.active());
    ZT_EQ("и заметка не тронута", liveBefore.toStdString(),
          rig.editor->document()->toPlainText().toStdString());

    answerWith(QMessageBox::Yes, "согласие");
    ZT_TRUE("после согласия режим закрыт", !rig.controller.active());
    ZT_TRUE("и слепок восстановлен",
            rig.editor->document()->toPlainText().contains(QStringLiteral("старое")));
}

void checkCorpus() {
    if (g_corpus.isEmpty()) {
        std::fprintf(stderr, "ПРОПУЩЕНО: корпус не задан — согласованность только на придуманных "
                             "случаях\n");
        return;
    }
    QStringList files;
    QDirIterator walk(g_corpus, {QStringLiteral("*.md")}, QDir::Files,
                      QDirIterator::Subdirectories);
    while (walk.hasNext()) files << walk.next();
    files.sort();

    int done = 0;
    for (const QString& name : files) {
        if (done >= 12) break;   // дюжины хватает: случаи повторяются
        QFile file(name);
        if (!file.open(QIODevice::ReadOnly)) continue;
        const QByteArray text = file.readAll();
        if (text.size() < 200) continue;

        QList<QByteArray> lines = text.split('\n');
        QByteArray changed;
        for (int i = 0; i < lines.size(); ++i) {
            if (i % 5 == 4) continue;                       // выбросили строку
            changed += lines[i];
            if (i % 3 == 0 && !lines[i].trimmed().isEmpty()) changed += " (правка)";
            if (i + 1 < lines.size()) changed += "\n";
        }
        const QString id = QStringLiteral("01ccccccccc%1").arg(done, 3, 10, QLatin1Char('0'));
        const QString path = makeNoteWithHistory(id, text, changed);

        Rig rig;
        if (!rig.open(path)) continue;
        // Вход в историю — точка сохранения: файл корпуса неканоничен, и его
        // канонический вид ложится третьей записью. Нам нужна пара «правленый
        // против исходного» — вторая запись против первой.
        if (!rig.controller.select(1)) continue;
        const diff::Result& result = rig.tl()->result();
        int removedRows = 0, addedRows = 0;
        for (const diff::Row& row : result.rows) {
            if (row.mark == diff::Mark::Removed || row.mark == diff::Mark::Changed) ++removedRows;
            if (row.mark == diff::Mark::Added || row.mark == diff::Mark::Changed) ++addedRows;
        }
        int removedBlocks = 0, addedBlocks = 0, blocks = rig.text().document()->blockCount();
        for (int b = 0; b < blocks; ++b) {
            const diff::Mark mark = rig.tl()->markOfBlock(b);
            if (mark == diff::Mark::Removed) ++removedBlocks;
            if (mark == diff::Mark::Added) ++addedBlocks;
        }
        const std::string what = QFileInfo(name).fileName().toStdString();
        ZT_EQ(what + ": убранных строк столько же, сколько блоков «−»", num(removedRows), num(removedBlocks));
        ZT_EQ(what + ": добавленных — сколько блоков «+»", num(addedRows), num(addedBlocks));
        ZT_TRUE(what + ": разница ненулевая", result.changed > 0);
        rig.controller.leave();
        ++done;
    }
    ZT_TRUE("на корпусе проверено файлов: " + num(done), done > 0);
}

// --- прибор ------------------------------------------------------------------
//
// Сколько стоит показать слепок, сменить базу и перейти к другому слепку.
//   taskset -c 0 ./zametti-tests --gtest_filter=HistoryView.* --bench <файл.md>
// (в наборе — через ztRunSuite с argv).
void bench(const QString& file) {
    QFile source(file);
    if (!source.open(QIODevice::ReadOnly)) {
        std::printf("не прочитан: %s\n", file.toUtf8().constData());
        return;
    }
    const QByteArray text = source.readAll();
    QByteArray changed;
    int line = 0;
    for (const QByteArray& one : text.split('\n')) {
        ++line;
        if (line % 3 == 0) continue;
        changed += one;
        if (line % 5 == 0) changed += " (правка)";
        changed += "\n";
    }
    const QString path = makeNoteWithHistory(QStringLiteral("01bbbbbbbbbb01"), text, changed);

    Rig rig;
    rig.window.resize(900, 700);
    rig.show();
    QElapsedTimer open;
    open.start();
    rig.editor->openFile(path);
    std::printf("открыть заметку: %lld мс\n", (long long)open.elapsed());
    QApplication::processEvents();

    QElapsedTimer clock;
    clock.start();
    const bool ok = rig.controller.enter();
    std::printf("вход в историю: %lld мс\n", (long long)clock.elapsed());
    if (!ok) return;
    for (int i = 0; i < 4; ++i) {
        clock.restart();
        rig.controller.setBaseFresh(i % 2 == 0);
        std::printf("  база %s: %lld мс\n", i % 2 == 0 ? "свежая    " : "предыдущая",
                    (long long)clock.elapsed());
    }
    if (rig.tl()->count() >= 2) {
        clock.restart();
        rig.controller.select(0);
        std::printf("  переход к другому слепку: %lld мс\n", (long long)clock.elapsed());
        clock.restart();
        rig.controller.select(1);
        std::printf("  и обратно: %lld мс\n", (long long)clock.elapsed());
    }
    rig.controller.leave();
}

void writeShots(const QString& dir) {
    QDir().mkpath(dir);
    const QString path = makeNoteWithHistory(
        QStringLiteral("01dddddddddd09"),
        note("# Список дел\n\nБыло записано так.\n\nЭтот абзац потом исчезнет.\n\n"
             "- купить хлеб\n- позвонить маме\n\n```\nint main();\n```\n",
             "a"),
        note("# Список дел\n\nСтало записано **иначе** и с `кодом` в строке.\n\n"
             "- купить хлеб\n- позвонить маме\n- [x] забрать посылку\n\n"
             "Формула $E=mc^2$ и цена $5. Ссылка [сюда](https://example.org) и ![снимок](img/1.jpg)\n\n"
             "```\nint main();\nreturn 0;\n```\n\n- пункт с кодом\n  ```\n  int a;\n  ```\n\n"
             "<!-- заметка себе: проверить -->\n",
             "b"));
    // Широкое и узкое окно (правило UI-матрицы: у агента окно узкое, у
    // владельца широкое).
    for (const int width : {1100, 640}) {
        Rig rig;
        rig.window.resize(width, 780);
        rig.show();
        if (!rig.open(path)) return;
        rig.neighbour->hide();
        rig.editor->hide();
        QApplication::processEvents();
        rig.view->grab().save(QDir(dir).filePath(
            QStringLiteral("история-%1.png").arg(width >= 1000 ? QStringLiteral("широко")
                                                                 : QStringLiteral("узко"))));
        // Со счётом отличий: два шага F4 — и в баннере «2/6». Снимок нужен
        // именно такой: надпись «—/6» до первого шага видна на снимке выше,
        // а с номером — только после ходьбы.
        rig.text().stepChange(true);
        rig.text().stepChange(true);
        QApplication::processEvents();
        rig.view->grab().save(QDir(dir).filePath(
            QStringLiteral("история-счёт-%1.png").arg(width >= 1000 ? QStringLiteral("широко")
                                                                     : QStringLiteral("узко"))));
        rig.controller.setBaseFresh(true);
        QApplication::processEvents();
        rig.view->grab().save(QDir(dir).filePath(
            QStringLiteral("история-со-свежей-%1.png").arg(width >= 1000 ? QStringLiteral("широко")
                                                                            : QStringLiteral("узко"))));
        rig.controller.leave();
    }
}

}  // namespace

static int ztRunSuite(int argc, char** argv) {
    QTemporaryDir tmp;
    if (!tmp.isValid()) {
        std::printf("не завёлся временный каталог\n");
        return 2;
    }
    g_root = tmp.path();
    if (argc > 2 && std::string(argv[1]) == "--bench") {
        bench(QString::fromLocal8Bit(argv[2]));
        return 0;
    }
    if (argc > 1) g_corpus = QString::fromLocal8Bit(argv[1]);

    checkBasics();
    checkGutterIsPainted();
    checkKeysAreWired();
    checkStepLandsInGolden();
    checkSnapshotSwitchKeepsPlace();
    checkBaseSwitchKeepsPlace();
    checkRestoreWritesSnapshot();
    checkZoomKeepsSnapshot();
    checkBaseSwitch();
    checkCopyIsRawText();
    checkOpenLeaves();
    checkShellFont();
    checkRestoreAsks();
    checkCorpus();
    writeShots(zt::TestData::outDir(QStringLiteral("history-view")));

    return zt::report("history-view");
}

TEST(HistoryView, All) {
    std::vector<QByteArray> ztArgs{QByteArrayLiteral("history_view_test")};
    // Прибор: ZAMETTI_HISTORY_BENCH=<файл.md> — вместо проверок замер на этом
    // файле (taskset -c 0 … --gtest_filter=HistoryView.*).
    const QByteArray bench = qgetenv("ZAMETTI_HISTORY_BENCH");
    if (!bench.isEmpty()) {
        ztArgs.push_back(QByteArrayLiteral("--bench"));
        ztArgs.push_back(bench);
    } else {
        ztArgs.push_back((zt::TestData::corpus(QStringLiteral("corpus"))).toLocal8Bit());
    }
    std::vector<char*> ztArgv;
    for (QByteArray& a : ztArgs) ztArgv.push_back(a.data());
    EXPECT_EQ(0, ztRunSuite(int(ztArgv.size()), ztArgv.data()));
}
