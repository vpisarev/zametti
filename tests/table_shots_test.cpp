// Снимки таблиц: то, что владелец проверяет глазами.
#include "editor_widget.h"
#include "block_object.h"
#include "note_view.h"
#include "settings.h"
#include "test_util.h"

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QImage>
#include <QTest>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextCursor>
#include <QAbstractTextDocumentLayout>
#include <QTextDocument>

namespace {
QString g_dir;

const char* kNote = R"(# Таблицы

Обычный абзац перед таблицей.

| деталь | цена | наличие |
|:---|---:|:---:|
| болт | 10 | да |
| гайка с длинным именем | 1000 | нет |
| шайба | 5 | да |

Абзац между таблицами.

| что | зачем |
|---|---|
| **жир** | видно ли |
| `код` | и он тоже |
| [ссылка](https://example.com) | цветом |

| колонка | очень длинное описание |
|---|---|
| раз | очень длинное описание детали, которое ни в какую колонку целиком не поместится и обязано перенестись по словам, а не раздуть таблицу |
| два | коротко |

Хвост заметки.
)";

// В КАЖДОЙ КОЛОНКЕ ЧТО-ТО НАРИСОВАНО.
//
// Проверка появилась после того, как моя же оптимизация раскладки унесла текст
// колонок с выравниванием вправо и по центру на километр за экран: ширину
// текста я спрашивал у boundingRect() раскладки, а там стояла ширина строки —
// бесконечная, потому что при измерении ячейку кладут в бесконечную ширину.
// Поймал снимок, глазами. Теперь ловит набор.
void checkEveryColumnDrawn(zametti::NoteEditor& editor, const QImage& shot) {
    const zametti::TableRender* table = nullptr;
    for (QTextBlock b = editor.document()->firstBlock(); b.isValid(); b = b.next()) {
        const zametti::TableRender* found = editor.tableAt(b.blockNumber());
        if (found != nullptr) { table = found; break; }
    }
    if (table == nullptr) {
        ++zt::g_failures;
        std::printf("провал: ни одной таблицы не показано сеткой\n");
        return;
    }

    const QRectF area = editor.tableRect(table->first);
    const int scroll = editor.verticalScrollBar()->value();

    // Считаем тёмные точки ВНУТРИ РЯДОВ, отступя от их границ. Первая редакция
    // считала по всей высоте колонки — и находила линии сетки, которые идут
    // через все колонки насквозь: проверка оставалась зелёной при пустых
    // колонках. Пустышка страшнее отсутствия проверки (правило проекта).
    qreal x = area.left();
    for (int column = 0; column < table->layout.columns; ++column) {
        const qreal width = table->layout.columnWidth.at(column);
        int dark = 0;
        qreal y = area.top();
        for (int row = 0; row < table->layout.rows; ++row) {
            const qreal height = table->layout.rowHeight.at(row);
            for (int px = int(x) + 2; px < int(x + width) - 2 && px < shot.width(); ++px)
                for (int py = int(y - scroll) + 4; py < int(y + height - scroll) - 4 &&
                                                  py < shot.height(); ++py)
                    if (py >= 0 && qGray(shot.pixel(px, py)) < 128) ++dark;
            y += height;
        }
        ++zt::g_checks;
        if (dark == 0) {
            ++zt::g_failures;
            std::printf("провал: в колонке %d ничего не нарисовано\n", column);
        }
        x += width;
    }
}

void shoot(const QString& name, int width, int height, const char* text) {
    const QString path = QDir(g_dir).filePath(name + QStringLiteral(".md"));
    QFile file(path);
    if (file.open(QIODevice::WriteOnly)) file.write(text);
    file.close();

    zametti::NoteEditor editor;
    editor.resize(width, height);
    editor.show();
    QTest::qWait(20);
    editor.openFile(path);
    QTest::qWait(80);
    QTextCursor at = editor.textCursor();
    at.setPosition(0);
    editor.setTextCursor(at);
    QTest::qWait(60);
    const QImage shot = editor.grab().toImage();
    if (!shot.save(QDir(g_dir).filePath(name + QStringLiteral(".png"))))
        std::printf("НЕ СОХРАНИЛСЯ снимок %s\n", qPrintable(name));
    checkEveryColumnDrawn(editor, shot);
}
}  // namespace

// Флип: Enter на таблице показывает исходник, уход каретки — снова сетку.
// Клавиши идут через слой объекта, но проводка к редактору своя, и без этой
// проверки она держалась бы только на моём слове.
void checkFlip() {
    const QString path = QDir(g_dir).filePath(QStringLiteral("флип.md"));
    QFile file(path);
    if (file.open(QIODevice::WriteOnly))
        file.write("до\n\n| a | b |\n|---|---|\n| 1 | 2 |\n\nпосле\n");
    file.close();

    zametti::NoteEditor editor;
    editor.resize(900, 700);
    editor.show();
    QTest::qWait(20);
    editor.openFile(path);
    QTest::qWait(60);

    const auto tableFirst = [&editor] {
        for (QTextBlock b = editor.document()->firstBlock(); b.isValid(); b = b.next())
            if (editor.tableAt(b.blockNumber()) != nullptr) return b.blockNumber();
        return -1;
    };
    const int first = tableFirst();
    ++zt::g_checks;
    if (first < 0) {
        ++zt::g_failures;
        std::printf("провал: таблица не показана сеткой\n");
        return;
    }

    // Строки исходника спрятаны — кроме последней, на которой висит резерв.
    int hidden = 0;
    for (QTextBlock b = editor.document()->firstBlock(); b.isValid(); b = b.next())
        if (!b.isVisible()) ++hidden;
    ++zt::g_checks;
    if (hidden != 2) {
        ++zt::g_failures;
        std::printf("провал: спрятано строк %d, а не 2\n", hidden);
    }

    // Каретка на таблицу — и Enter показывает исходник.
    QTextCursor at(editor.document()->findBlockByNumber(first + 2));
    editor.setTextCursor(at);
    QTest::qWait(20);
    QTest::keyClick(&editor, Qt::Key_Return);
    QTest::qWait(40);
    ++zt::g_checks;
    if (editor.editedTable() != first) {
        ++zt::g_failures;
        std::printf("провал: Enter не открыл исходник (правится %d, ждали %d)\n",
                    editor.editedTable(), first);
    }
    int visibleNow = 0;
    for (QTextBlock b = editor.document()->firstBlock(); b.isValid(); b = b.next())
        if (!b.isVisible()) ++visibleNow;
    ++zt::g_checks;
    if (visibleNow != 0) {
        ++zt::g_failures;
        std::printf("провал: в правке остались спрятанные строки (%d)\n", visibleNow);
    }

    // И РАСКЛАДКА ВЕРНУЛАСЬ. Одного isVisible() мало: блок может числиться
    // видимым, а высоты у него так и остаться нулевой — тогда на экране видна
    // одна строка вместо всей таблицы. Владелец увидел именно это.
    int zeroHeight = 0;
    for (int number = first; number <= first + 2; ++number) {
        const QTextBlock b = editor.document()->findBlockByNumber(number);
        if (!b.isValid()) continue;
        if (editor.document()->documentLayout()->blockBoundingRect(b).height() <= 0.5)
            ++zeroHeight;
    }
    ++zt::g_checks;
    if (zeroHeight != 0) {
        ++zt::g_failures;
        std::printf("провал: строк без высоты в правке: %d\n", zeroHeight);
    }

    // Увели каретку наружу — снова сетка.
    editor.setTextCursor(QTextCursor(editor.document()->firstBlock()));
    QTest::qWait(60);
    ++zt::g_checks;
    if (editor.editedTable() != -1) {
        ++zt::g_failures;
        std::printf("провал: уход каретки не вернул сетку\n");
    }
    ++zt::g_checks;
    if (editor.tableAt(first) == nullptr) {
        ++zt::g_failures;
        std::printf("провал: сетка не вернулась\n");
    }
}

// Мышь: щелчок по сетке. Ровно тот путь, которым идёт владелец, — и ровно
// он в первой редакции никуда не приводил: Qt про резерв места не знает и
// ставит каретку по своим правилам.
void checkMouse() {
    const QString path = QDir(g_dir).filePath(QStringLiteral("мышь.md"));
    QFile file(path);
    if (file.open(QIODevice::WriteOnly))
        file.write("до\n\n| a | b |\n|---|---|\n| 1 | 2 |\n| 3 | 4 |\n\nпосле\n");
    file.close();

    zametti::NoteEditor editor;
    editor.resize(900, 700);
    editor.show();
    QTest::qWait(20);
    editor.openFile(path);
    QTest::qWait(60);

    int first = -1;
    for (QTextBlock b = editor.document()->firstBlock(); b.isValid(); b = b.next())
        if (editor.tableAt(b.blockNumber()) != nullptr) { first = b.blockNumber(); break; }
    if (first < 0) {
        ++zt::g_checks; ++zt::g_failures;
        std::printf("провал: таблицы нет\n");
        return;
    }

    const QRectF area = editor.tableRect(first);
    const QPoint middle(int(area.center().x()),
                        int(area.center().y()) - editor.verticalScrollBar()->value());

    // Одинарный щелчок по сетке — таблица выбрана.
    QTest::mouseClick(editor.viewport(), Qt::LeftButton, Qt::NoModifier, middle);
    QTest::qWait(30);
    const zametti::BlockObject picked = zametti::objectOf(editor.textCursor().block());
    ++zt::g_checks;
    if (picked.kind != zametti::ObjectKind::Table || picked.first != first) {
        ++zt::g_failures;
        std::printf("провал: щелчок по сетке не выбрал таблицу (каретка в блоке %d)\n",
                    editor.textCursor().blockNumber());
    }

    // Выбранная таблица показана уголками-мишенями — тем же, чем показана
    // выбранная фотография.
    {
        const QImage shot = editor.grab().toImage();
        const QColor caretColour = zametti::appearance().caretColor;
        int cornerPixels = 0;
        for (int px = int(area.left()) - 12; px < int(area.right()) + 12 && px < shot.width();
             ++px)
            for (int py = int(area.top()) - 12; py < int(area.bottom()) + 12 && py < shot.height();
                 ++py) {
                if (px < 0 || py < 0) continue;
                if (area.contains(QPointF(px, py))) continue;
                const QColor at = shot.pixelColor(px, py);
                if (qAbs(at.red() - caretColour.red()) < 20 &&
                    qAbs(at.green() - caretColour.green()) < 20 &&
                    qAbs(at.blue() - caretColour.blue()) < 20)
                    ++cornerPixels;
            }
        ++zt::g_checks;
        if (cornerPixels == 0) {
            ++zt::g_failures;
            std::printf("провал: выбранная таблица не показана уголками\n");
        }
    }

    // А КАРЕТКИ ВНУТРИ НЕТ. Спрашиваем правило, а не картинку: у набора нет
    // фокуса окна (под Xvfb hasFocus() всегда ложь), и по снимку это условие
    // не проверить вовсе — первая редакция проверки была пустышкой и оставалась
    // зелёной со снятой починкой.
    ++zt::g_checks;
    if (zametti::caretShouldBeDrawn(true, false, false, false, true)) {
        ++zt::g_failures;
        std::printf("провал: каретка рисуется внутри нарисованной таблицы\n");
    }
    ++zt::g_checks;
    if (!zametti::caretShouldBeDrawn(true, false, false, false, false)) {
        ++zt::g_failures;
        std::printf("провал: в обычном тексте каретка пропала\n");
    }

    // Щелчок по НИЖНЕЙ части сетки — там, где кончается резерв места: без
    // перехвата Qt ставит каретку в следующий за таблицей абзац.
    const QPoint low(int(area.center().x()),
                     int(area.bottom()) - 4 - editor.verticalScrollBar()->value());
    QTest::mouseClick(editor.viewport(), Qt::LeftButton, Qt::NoModifier, low);
    QTest::qWait(30);
    ++zt::g_checks;
    const zametti::BlockObject low_pick = zametti::objectOf(editor.textCursor().block());
    if (low_pick.kind != zametti::ObjectKind::Table || low_pick.first != first) {
        ++zt::g_failures;
        std::printf("провал: щелчок по низу сетки не выбрал таблицу (блок %d)\n",
                    editor.textCursor().blockNumber());
    }

    // Двойной щелчок — правка исходника, каретка рядом с местом щелчка.
    QTest::mouseDClick(editor.viewport(), Qt::LeftButton, Qt::NoModifier, middle);
    QTest::qWait(40);
    ++zt::g_checks;
    if (editor.editedTable() != first) {
        ++zt::g_failures;
        std::printf("провал: двойной щелчок не открыл исходник (правится %d)\n",
                    editor.editedTable());
    }
    ++zt::g_checks;
    const zametti::BlockObject inside = zametti::objectOf(editor.textCursor().block());
    if (inside.kind != zametti::ObjectKind::Table || inside.first != first) {
        ++zt::g_failures;
        std::printf("провал: каретка не в исходнике таблицы (блок %d)\n",
                    editor.textCursor().blockNumber());
    }
}

// Протокол правки: Enter внутри вставляет строку и правку НЕ прерывает, Esc
// выходит, уход каретки наружу — тоже.
void checkEditProtocol() {
    const QString path = QDir(g_dir).filePath(QStringLiteral("протокол.md"));
    QFile file(path);
    if (file.open(QIODevice::WriteOnly))
        file.write("до\n\n| a | b |\n|---|---|\n| 1 | 2 |\n| 3 | 4 |\n\nпосле\n");
    file.close();

    zametti::NoteEditor editor;
    editor.resize(900, 700);
    editor.show();
    QTest::qWait(20);
    editor.openFile(path);
    QTest::qWait(60);

    int first = -1;
    for (QTextBlock b = editor.document()->firstBlock(); b.isValid(); b = b.next())
        if (editor.tableAt(b.blockNumber()) != nullptr) { first = b.blockNumber(); break; }
    if (first < 0) {
        ++zt::g_checks; ++zt::g_failures;
        std::printf("провал: таблицы нет\n");
        return;
    }

    // Входим в правку.
    editor.setTextCursor(QTextCursor(editor.document()->findBlockByNumber(first + 3)));
    QTest::qWait(20);
    QTest::keyClick(&editor, Qt::Key_Return);
    QTest::qWait(40);
    ++zt::g_checks;
    if (editor.editedTable() < 0) {
        ++zt::g_failures;
        std::printf("провал: в правку не вошли\n");
        return;
    }

    // ENTER ВНУТРИ ПРАВКИ ВСТАВЛЯЕТ СТРОКУ и правку не прерывает: человек
    // добавляет ряд таблицы, а не выходит. Встаём в конец ПОСЛЕДНЕГО ряда —
    // именно так ряд и добавляют.
    const int blocksBefore = editor.document()->blockCount();
    QTextCursor at(editor.document()->findBlockByNumber(first + 3));
    at.movePosition(QTextCursor::EndOfBlock);
    editor.setTextCursor(at);
    QTest::qWait(10);
    QTest::keyClick(&editor, Qt::Key_Return);
    QTest::qWait(60);
    ++zt::g_checks;
    if (editor.document()->blockCount() != blocksBefore + 1) {
        ++zt::g_failures;
        std::printf("провал: Enter в правке не вставил строку (было %d, стало %d)\n",
                    blocksBefore, editor.document()->blockCount());
    }
    ++zt::g_checks;
    if (editor.editedTable() < 0) {
        ++zt::g_failures;
        std::printf("провал: Enter внутри оборвал правку\n");
        std::printf("  каретка в блоке %d «%s», рядом таблица %d\n",
                    editor.textCursor().blockNumber(),
                    editor.textCursor().block().text().left(20).toUtf8().constData(),
                    editor.tableNearCaret());
        int n = 0;
        for (QTextBlock b = editor.document()->firstBlock(); b.isValid(); b = b.next(), ++n) {
            const zametti::BlockObject o = zametti::objectOf(b);
            std::printf("  блок %d род=%d объект=%d(%d..%d) «%s»\n", n,
                        int(zametti::kindOf(b)), int(o.kind), o.first, o.last,
                        b.text().left(24).toUtf8().constData());
        }
    }

    // РЯД В СЕРЕДИНУ ТЕЛА. Enter заводит пустую строку внутри таблицы — а
    // пустая строка таблицу кончает, и кусок на миг перестаёт быть таблицей
    // вовсе. Правка обязана это пережить: человек как раз набирает новый ряд.
    //
    // Ряд ставится после первой строки ТЕЛА, а не после шапки: между шапкой и
    // строкой-разделителем ряду взяться неоткуда, и таблица от такой вставки
    // ломается насовсем — это не «промежуточное состояние», а другая заметка.
    {
        QTextCursor mid(editor.document()->findBlockByNumber(first + 2));
        mid.movePosition(QTextCursor::EndOfBlock);
        editor.setTextCursor(mid);
        QTest::qWait(10);
        QTest::keyClick(&editor, Qt::Key_Return);
        QTest::qWait(60);
        ++zt::g_checks;
        if (editor.editedTable() < 0) {
            ++zt::g_failures;
            std::printf("провал: Enter в середине таблицы оборвал правку\n");
        }
        QTest::keyClicks(&editor, QStringLiteral("| 5 | 6 |"));
        QTest::qWait(60);
        ++zt::g_checks;
        if (editor.editedTable() < 0) {
            ++zt::g_failures;
            std::printf("провал: набор нового ряда оборвал правку\n");
        }
    }

    // Esc выходит: снова сетка, таблица выбрана.
    QTest::keyClick(&editor, Qt::Key_Escape);
    QTest::qWait(60);
    // Ярлык окна до набора не доходит — зовём то же, что зовёт окно.
    if (editor.editedTable() >= 0) editor.leaveTableEdit();
    QTest::qWait(60);
    ++zt::g_checks;
    if (editor.editedTable() >= 0) {
        ++zt::g_failures;
        std::printf("провал: Esc не вывел из правки\n");
    }
    ++zt::g_checks;
    const zametti::BlockObject after = zametti::objectOf(editor.textCursor().block());
    if (after.kind != zametti::ObjectKind::Table) {
        ++zt::g_failures;
        std::printf("провал: после выхода каретка не на таблице (блок %d)\n",
                    editor.textCursor().blockNumber());
    }

    // Входим снова и уходим кареткой наружу — правка кончается сама.
    QTest::keyClick(&editor, Qt::Key_Return);
    QTest::qWait(40);
    ++zt::g_checks;
    if (editor.editedTable() < 0) {
        ++zt::g_failures;
        std::printf("провал: второй вход в правку не сработал\n");
    }
    editor.setTextCursor(QTextCursor(editor.document()->firstBlock()));
    QTest::qWait(60);
    ++zt::g_checks;
    if (editor.editedTable() >= 0) {
        ++zt::g_failures;
        std::printf("провал: уход каретки не завершил правку\n");
    }
}

// ПУТЬ ВЛАДЕЛЬЦА ДОСЛОВНО: живая заметка с двумя таблицами, щелчок по ВТОРОЙ,
// Enter. Он видел одну последнюю строку и крохотный курсор; выдуманная заметка
// с одной таблицей эту беду не показывала.
void checkSecondTableInLiveNote(const QString& source) {
    if (source.isEmpty() || !QFile::exists(source)) return;
    QFile in(source);
    if (!in.open(QIODevice::ReadOnly)) return;
    const QByteArray body = in.readAll();
    in.close();
    const QString copy = QDir(g_dir).filePath(QStringLiteral("вторая-таблица.md"));
    QFile out(copy);
    if (out.open(QIODevice::WriteOnly | QIODevice::Truncate)) out.write(body);
    out.close();

    zametti::NoteEditor editor;
    editor.resize(1100, 800);
    editor.show();
    QTest::qWait(30);
    editor.openFile(copy);
    QTest::qWait(150);

    QVector<int> tables;
    for (QTextBlock b = editor.document()->firstBlock(); b.isValid(); b = b.next())
        if (editor.tableAt(b.blockNumber()) != nullptr) tables.push_back(b.blockNumber());
    ++zt::g_checks;
    if (tables.size() < 2) {
        ++zt::g_failures;
        std::printf("провал: в живой заметке показано таблиц %d, ждали хотя бы две\n",
                    int(tables.size()));
        return;
    }

    const int second = tables[1];
    const zametti::TableRender* render = editor.tableAt(second);
    const int lines = render->last - render->first + 1;
    const QRectF area = editor.tableRect(second);

    editor.verticalScrollBar()->setValue(qMax(0, int(area.top()) - 100));
    QTest::qWait(50);
    const QPoint at(int(area.center().x()),
                    int(area.center().y()) - editor.verticalScrollBar()->value());
    QTest::mouseClick(editor.viewport(), Qt::LeftButton, Qt::NoModifier, at);
    QTest::qWait(50);
    QTest::keyClick(&editor, Qt::Key_Return);
    QTest::qWait(100);

    ++zt::g_checks;
    if (editor.editedTable() != second) {
        ++zt::g_failures;
        std::printf("провал: Enter по второй таблице не открыл исходник (правится %d, ждали %d)\n",
                    editor.editedTable(), second);
    }

    // Все строки исходника видны И имеют высоту.
    int hidden = 0;
    int flat = 0;
    for (int number = second; number < second + lines; ++number) {
        const QTextBlock b = editor.document()->findBlockByNumber(number);
        if (!b.isValid()) continue;
        if (!b.isVisible()) ++hidden;
        if (editor.document()->documentLayout()->blockBoundingRect(b).height() <= 0.5) ++flat;
    }
    ++zt::g_checks;
    if (hidden != 0 || flat != 0) {
        ++zt::g_failures;
        std::printf("провал: в правке второй таблицы спрятано %d строк, без высоты %d "
                    "(всего строк %d)\n", hidden, flat, lines);
    }
}

// ИНВАРИАНТ A ИЗ БРИФА: показ не меняет файл ни на байт.
//
// Открываем копии настоящих заметок владельца, даём виду их отрисовать, водим
// кареткой по таблице и сравниваем байты. Показ — чистое чтение, и это
// единственный способ убедиться в этом, а не понадеяться.
void checkFilesUntouched(const QStringList& sources) {
    for (const QString& source : sources) {
        QFile in(source);
        if (!in.open(QIODevice::ReadOnly)) continue;
        const QByteArray before = in.readAll();
        in.close();

        const QString copy = QDir(g_dir).filePath(QFileInfo(source).fileName());
        QFile out(copy);
        if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) continue;
        out.write(before);
        out.close();

        zametti::NoteEditor editor;
        editor.resize(900, 700);
        editor.show();
        QTest::qWait(20);
        editor.openFile(copy);
        QTest::qWait(80);

        // Ходим кареткой по всему документу: рендер, флип, снимок мест —
        // всё это не имеет права тронуть файл.
        for (int i = 0; i < 40; ++i) {
            QTest::keyClick(&editor, Qt::Key_Down);
            QTest::qWait(2);
        }
        for (int i = 0; i < 10; ++i) {
            QTest::keyClick(&editor, Qt::Key_Up);
            QTest::qWait(2);
        }
        QTest::qWait(50);

        // Снимок живой заметки — на него смотрит владелец.
        QTextCursor top = editor.textCursor();
        top.setPosition(0);
        editor.setTextCursor(top);
        QTest::qWait(60);
        const QImage live = editor.grab().toImage();
        live.save(QDir(g_dir).filePath(QFileInfo(source).completeBaseName() +
                                       QStringLiteral(".png")));

        QFile after(copy);
        ++zt::g_checks;
        if (!after.open(QIODevice::ReadOnly)) {
            ++zt::g_failures;
            std::printf("провал: копия %s не читается\n", qPrintable(copy));
            continue;
        }
        const QByteArray now = after.readAll();
        after.close();
        if (now == before) continue;
        ++zt::g_failures;
        std::printf("провал: файл изменился при показе — %s (было %lld байт, стало %lld)\n",
                    qPrintable(QFileInfo(source).fileName()), (long long)before.size(),
                    (long long)now.size());
    }
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    g_dir = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QDir::tempPath();
    QDir().mkpath(g_dir);
    shoot(QStringLiteral("таблица-широкое"), 1000, 700, kNote);
    shoot(QStringLiteral("таблица-узкое"), 620, 700, kNote);
    checkFlip();
    checkMouse();
    checkEditProtocol();

    QStringList sources;
    for (int i = 2; i < argc; ++i) sources << QString::fromLocal8Bit(argv[i]);
    if (!sources.isEmpty()) {
        checkFilesUntouched(sources);
        checkSecondTableInLiveNote(sources.first());
    }

    std::printf("снимки: %s\n", qPrintable(g_dir));
    return zt::report("снимки таблиц");
}
