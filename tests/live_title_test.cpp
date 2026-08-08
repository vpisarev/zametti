// Живой заголовок: печатают в заметке — строка средней колонки меняется сразу.
//
// Владелец нашёл, что у ТОЛЬКО ЧТО СОЗДАННОЙ заметки строка не обновляется,
// пока не переключишься на другую и обратно. Набор идёт по той же цепочке, что
// и окно: правка -> NoteTreeModel::updateTitle -> сигнал noteRowChanged ->
// NoteListModel::updateRow -> роль TitleRole в списке. Каждое звено проверяется
// отдельно: молчащее звено посередине иначе не найти, сигнал просто не дойдёт.

#include "doc_model.h"
#include "editor_widget.h"
#include "note_list.h"
#include "note_tree.h"
#include "store.h"

#include "test_util.h"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QSignalSpy>
#include <QTest>
#include <QTextBlock>
#include <QTextCursor>

#include <cstdio>
#include <string>

using zametti::NoteListModel;
using zametti::NoteTreeModel;

namespace {

std::string s(const QString& q) { return q.toStdString(); }

QString g_root;

void writeNote(const QString& id, const QString& body) {
    QFile f(g_root + QLatin1Char('/') + id + QStringLiteral(".md"));
    if (!f.open(QIODevice::WriteOnly)) return;
    f.write((QStringLiteral("<!-- zametti\ncreated: 2026-01-01T00:00:00Z\n"
                            "modified: 2026-01-02T00:00:00Z\n-->\n\n") +
             body)
                .toUtf8());
}

// Заголовок, который сейчас показывает средняя колонка для заметки с этим id.
QString shownTitle(const NoteListModel& list, const QString& id) {
    for (int row = 0; row < list.rowCount(); ++row) {
        const QModelIndex at = list.index(row, 0);
        if (list.data(at, NoteListModel::IdRole).toString() == id)
            return list.data(at, NoteListModel::TitleRole).toString();
    }
    return QStringLiteral("<строки нет>");
}

// Окно связывает дерево со списком ровно так. Повторяем связь, а не зовём
// updateRow руками: половина беды как раз в том, доходит ли сигнал.
void wire(NoteTreeModel& model, NoteListModel& list) {
    QObject::connect(&model, &NoteTreeModel::noteRowChanged, &list,
                     [&](const QString& id) { list.updateRow(model.rowOf(id)); });
}

// Заметка, прочитанная с диска при построении дерева. Здесь всё работало и до
// починки — проверка нужна как опора: если покраснеет она, дело не в новизне.
void checkExistingNote() {
    NoteTreeModel model(g_root);
    model.setFoldersOnly(true);
    NoteListModel list;
    wire(model, list);
    list.setRows(model.notesInSubtree(QModelIndex()));

    const QString id = QStringLiteral("00000000000001");
    ZT_EQ("до правки в списке прежний заголовок", std::string("Старая"),
          s(shownTitle(list, id)));

    model.updateTitle(g_root + QStringLiteral("/00000000000001.md"),
                      QStringLiteral("Стамбул"));
    ZT_EQ("после правки заголовок в списке новый", std::string("Стамбул"),
          s(shownTitle(list, id)));
}

// Заметка, созданная в этом же запуске штатным путём (Ctrl+N и кнопка тулбара
// зовут именно store::newNote). Ради неё набор и написан.
void checkFreshNote() {
    NoteTreeModel model(g_root);
    model.setFoldersOnly(true);
    NoteListModel list;
    wire(model, list);

    QString error;
    const QString made = zametti::store::newNote(g_root, QString(), &error);
    ZT_TRUE("новая заметка создана: " + s(error), !made.isEmpty());
    if (made.isEmpty()) return;

    // Окно после создания перестраивает дерево и заново наполняет список.
    model.refresh();
    list.setRows(model.notesInSubtree(QModelIndex()));

    const QString id = QFileInfo(made).completeBaseName();
    ZT_TRUE("новая заметка попала в список",
            shownTitle(list, id) != QStringLiteral("<строки нет>"));

    QSignalSpy spy(&model, &NoteTreeModel::noteRowChanged);
    model.updateTitle(made, QStringLiteral("Стамбул"));
    ZT_EQ("дерево сообщило о смене строки", std::string("1"),
          std::to_string(spy.count()));
    ZT_EQ("у новой заметки заголовок в списке тоже меняется сразу",
          std::string("Стамбул"), s(shownTitle(list, id)));

    // И дальше по букве: печатают не один раз, а каждым нажатием.
    model.updateTitle(made, QStringLiteral("Стамбул, Турция"));
    ZT_EQ("и на следующей правке", std::string("Стамбул, Турция"),
          s(shownTitle(list, id)));
}

// Заголовок первого содержательного блока — ровно так его берёт окно
// (main.cpp, обработчик textChanged). Копия здесь не от хорошей жизни: окна как
// объекта не существует, вся его проводка живёт лямбдами внутри main().
QString titleFromEditor(const zametti::NoteEditor& editor) {
    for (QTextBlock block = editor.document()->begin(); block.isValid();
         block = block.next()) {
        if (!zametti::isRawBlock(block) && zametti::kindOf(block) == zametti::Kind::VSpace)
            continue;
        QString text = block.text();
        const qsizetype eol = text.indexOf(QChar::LineSeparator);
        if (eol >= 0) text = text.left(eol);
        return text.trimmed().left(64);
    }
    return QString();
}

// Главная проверка: заметка создана штатным путём, открыта в настоящем
// редакторе, заголовок печатается клавишами. Именно здесь ломалось у владельца.
void checkFreshNoteThroughEditor() {
    NoteTreeModel model(g_root);
    model.setFoldersOnly(true);
    NoteListModel list;
    wire(model, list);

    QString error;
    const QString made = zametti::store::newNote(g_root, QString(), &error);
    ZT_TRUE("вторая новая заметка создана: " + s(error), !made.isEmpty());
    if (made.isEmpty()) return;
    model.refresh();
    list.setRows(model.notesInSubtree(QModelIndex()));

    zametti::NoteEditor editor;
    editor.setStoreRoot(g_root);
    editor.openFile(made);

    // Путь, которым заметку зовёт редактор, и путь, которым её знает дерево,
    // ОБЯЗАНЫ совпадать: по нему дерево ищет строку. Расхождение здесь и есть
    // тихий отказ — сигнал не дойдёт, и никто об этом не скажет.
    const QString id = QFileInfo(made).completeBaseName();
    ZT_EQ("редактор и дерево зовут заметку одинаково", s(model.rowOf(id).path),
          s(editor.filePath()));

    QObject::connect(&editor, &QTextEdit::textChanged, &editor, [&] {
        QString title = titleFromEditor(editor);
        if (title.isEmpty()) title = QStringLiteral("Без названия");
        model.updateTitle(editor.filePath(), title);
    });

    editor.setFocus();
    QTextCursor cursor = editor.textCursor();
    cursor.movePosition(QTextCursor::Start);
    editor.setTextCursor(cursor);
    QTest::keyClicks(&editor, QStringLiteral("Istanbul"));

    ZT_EQ("напечатанный заголовок сразу виден в средней колонке",
          std::string("Istanbul"), s(shownTitle(list, id)));
}

// СТОРОЖ ПОЧИНКИ. Заметку ищут по id из имени файла, а не по тексту пути —
// потому что пути с двух сторон приходят разными дорогами и совпадают не
// всегда. Здесь расхождение делается нарочно: дерево строится по ссылке на
// каталог, а заметка создаётся по настоящему пути. До починки такая пара
// молча переставала обновлять строку списка — ровно то, что видел владелец.
void checkTitleSurvivesPathMismatch() {
    const QString real = g_root + QStringLiteral("-mismatch");
    const QString link = g_root + QStringLiteral("-mismatch-ссылка");
    QDir(real).removeRecursively();
    QFile::remove(link);
    QDir().mkpath(real + QStringLiteral("/.zametti"));
    ZT_TRUE("ссылка на каталог хранилища создана", QFile::link(real, link));
    if (!QFileInfo::exists(link)) return;

    // Дерево знает хранилище ПО ССЫЛКЕ, редактор получит настоящий путь.
    NoteTreeModel model(link);
    model.setFoldersOnly(true);
    NoteListModel list;
    wire(model, list);

    QString error;
    const QString made = zametti::store::newNote(real, QString(), &error);
    ZT_TRUE("заметка создана по настоящему пути: " + s(error), !made.isEmpty());
    if (made.isEmpty()) return;
    model.refresh();
    list.setRows(model.notesInSubtree(QModelIndex()));

    const QString id = QFileInfo(made).completeBaseName();
    ZT_TRUE("заметка в списке есть",
            shownTitle(list, id) != QStringLiteral("<строки нет>"));
    // Главное: пути РАЗНЫЕ, а строка всё равно обновляется.
    ZT_TRUE("пути и правда разошлись — иначе проверка ничего не стережёт",
            model.rowOf(id).path != made);
    model.updateTitle(made, QStringLiteral("Стамбул"));
    ZT_EQ("заголовок подхватился, несмотря на разные пути", std::string("Стамбул"),
          s(shownTitle(list, id)));
}

// ЗАМЕР, а не проверка: сколько стоит одно нажатие клавиши. Вопрос владельца
// был прямой — «директории теперь сканируются на каждую букву?». Не сканируются:
// completeBaseName() режет строку и в файловую систему не ходит, а поиск идёт по
// дереву в памяти. Но это рассуждение, а число — здесь.
//
// Зовётся ключом --bench, в ctest не попадает: замер — не проверка.
void benchKeystroke(int notes) {
    const QString root = g_root + QStringLiteral("-bench");
    QDir(root).removeRecursively();
    QDir().mkpath(root + QStringLiteral("/.zametti"));
    QString last;
    for (int i = 0; i < notes; ++i) {
        const QString id = QStringLiteral("%1").arg(i, 14, 10, QLatin1Char('0'));
        last = root + QLatin1Char('/') + id + QStringLiteral(".md");
        QFile f(last);
        if (f.open(QIODevice::WriteOnly))
            f.write(QStringLiteral("<!-- zametti\ncreated: 2026-01-01T00:00:00Z\n"
                                   "modified: 2026-01-02T00:00:00Z\n-->\n\n# Заметка %1\n")
                        .arg(i)
                        .toUtf8());
    }

    NoteTreeModel model(root);
    model.setFoldersOnly(true);

    // Эталонный счётный цикл рядом: пока он стоит намертво, разброс в замере —
    // свойство измеряемого кода, а не машины.
    QElapsedTimer yard;
    yard.start();
    volatile double acc = 0;
    for (int i = 0; i < 20'000'000; ++i) acc += i * 1e-9;
    const qint64 yardUs = yard.nsecsElapsed() / 1000;

    // Худший случай: заметка, добавленная последней, — обход всего дерева.
    const int taps = 2000;
    QElapsedTimer t;
    t.start();
    for (int i = 0; i < taps; ++i)
        model.updateTitle(last, QStringLiteral("Заголовок %1").arg(i));
    const qint64 us = t.nsecsElapsed() / 1000;

    std::fprintf(stderr,
                 "заметок %d: %.2f мкс на нажатие (всего %lld мкс на %d нажатий, "
                 "эталон %lld мкс, acc %.0f)\n",
                 notes, double(us) / taps, (long long)us, taps, (long long)yardUs,
                 double(acc));
}

// СТОРОЖ ЛИШНЕЙ КОСОЙ ЧЕРТЫ. Владелец запустил программу как
// `--root sandbox/vpnotes/` — дополнение в оболочке добавляет черту само, — и
// в логе появилось `vpnotes//<id>.md`. Путь рабочий, открывается и пишется, но
// по строке не равен тому, которым ту же заметку зовёт дерево. Ровно этот
// лишний знак и ломал заголовок в средней колонке.
void checkTrailingSlashInRoot() {
    const QString real = g_root + QStringLiteral("-slash");
    QDir(real).removeRecursively();
    QDir().mkpath(real + QStringLiteral("/.zametti"));

    // Корень С ЧЕРТОЙ на конце — как его отдаёт оболочка.
    NoteTreeModel model(real + QLatin1Char('/'));
    model.setFoldersOnly(true);
    NoteListModel list;
    wire(model, list);

    QString error;
    const QString made = zametti::store::newNote(model.nodePath(QModelIndex()), QString(), &error);
    ZT_TRUE("заметка создана: " + s(error), !made.isEmpty());
    if (made.isEmpty()) return;
    ZT_TRUE("в пути новой заметки нет двойной черты: " + s(made),
            !made.contains(QStringLiteral("//")));

    model.refresh();
    list.setRows(model.notesInSubtree(QModelIndex()));
    const QString id = QFileInfo(made).completeBaseName();
    ZT_EQ("дерево и хранилище зовут заметку одинаково", s(made), s(model.rowOf(id).path));

    model.updateTitle(made, QStringLiteral("Стамбул"));
    ZT_EQ("и заголовок подхватывается", std::string("Стамбул"), s(shownTitle(list, id)));
}

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);

    g_root = QDir::tempPath() + QStringLiteral("/zametti-live-title-test");
    QDir(g_root).removeRecursively();
    QDir().mkpath(g_root + QStringLiteral("/.zametti"));
    writeNote(QStringLiteral("00000000000001"), QStringLiteral("# Старая\n"));

    checkExistingNote();
    checkFreshNote();
    checkFreshNoteThroughEditor();
    checkTitleSurvivesPathMismatch();
    checkTrailingSlashInRoot();

    if (argc > 1 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("--bench")) {
        for (int notes : {100, 300, 1000, 3000}) benchKeystroke(notes);
        return 0;
    }

    return zt::report("live-title");
}
