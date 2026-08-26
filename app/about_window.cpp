#include "about_window.h"

#include "build_facts.h"
#include "document_builder.h"
#include "note_view.h"
#include "resources.h"
#include "settings.h"

#include <QDialogButtonBox>
#include <QIcon>
#include <QPushButton>
#include <QTextBrowser>
#include <QStringList>
#include <QTabWidget>
#include <QTextDocument>
#include <QVBoxLayout>

#include <algorithm>
#include <vector>


namespace zametti {
namespace {

// Показ markdown нашим же путём: разбор ядром, сборка тем же сборщиком, что и
// заметки, тот же вид. Отличается только одно — править нельзя.
NoteView* markdownPage(const QString& markdown, QWidget* parent) {
    auto* view = new NoteView(parent);
    view->setReadOnly(true);
    // Ссылки НЕ ходят внутри окна (QTextBrowser в режиме чтения грузит адрес в
    // себя и остаётся пустым насовсем — владелец на это и наткнулся), а внешние
    // уходят в браузер. Здесь для этого нет ни строчки: правило живёт в общем
    // предке, NoteView, — и стояла тут вторая его копия, которая рано или
    // поздно разошлась бы с первой.
    // Документ принадлежит виду: своей жизни у справки нет, а Qt удалит его
    // вместе с родителем.
    auto* document = new QTextDocument(view);
    std::vector<Piece> blocks;
    NoteHeader ignored;
    parsePieces(markdown, blocks, ignored);
    buildDocument(blocks, *document);
    view->setDocument(document);
    view->applyContentWidth();
    return view;
}

}  // namespace

QString aboutMarkdown() {
    // ЧТО ЭТО, ЧЬЁ ЭТО, ИЗ ЧЕГО СОБРАНО — одной страницей. Первое, зачем сюда
    // приходят, — узнать версию и условия; второе — приложить сводку сборки к
    // письму об ошибке. Поэтому и то и другое здесь, и копируется как есть.
    QString out;
    out += QStringLiteral("# zametti\n\n");
    out += QStringLiteral("take notes, organize 'em, encrypt, sync via cloud\n\n");
    out += QStringLiteral(
        "Notes on disk are plain markdown: they can be edited with any tools and kept in git. "
        "The application does not own the format — it only reads and writes it.\n\n");
    // Год и имя — не сочинённые: год берётся из истории репозитория, имя — из
    // подписи её автора. Владельцу проверить эту строку глазами.
    out += QStringLiteral(
        "Copyright © 2026 Vadim Pisarevsky. Licensed under GPL-3.0; the full text, and the terms "
        "of everything bundled with it, are on the **Licenses** tab.\n\n");
    // Дверь к документации названа прямо: иначе человек будет искать её здесь,
    // где она была четырьмя вкладками, и не найдёт.
    out += QStringLiteral(
        "The manual lives in the note tree: the **Info** folder at the bottom of the left "
        "column — it is read there like any other note.\n\n");
    out += buildFactsMarkdown();
    return out;
}

QString buildFactsMarkdown() {
    QString out;
    out += QStringLiteral("# What it is built from\n\n");
    for (const BuildFact& fact : kBuildFacts) {
        out += QStringLiteral("- **%1:** %2\n")
                   .arg(QString::fromUtf8(fact.name), QString::fromUtf8(fact.value));
    }
    out += QStringLiteral("- **extra image readers:** %1\n")
               .arg(QString::fromUtf8(kImageReaders));

    out += QStringLiteral("\n## Bundled libraries\n\n");
    for (const VendoredFact& fact : kVendoredFacts) {
        out += QStringLiteral("- **%1 %2** — %3\n")
                   .arg(QString::fromUtf8(fact.name), QString::fromUtf8(fact.version),
                        QString::fromUtf8(fact.what));
    }

    // Иконки и шрифты — такая же часть сборки, как библиотеки: они вшиты в
    // бинарник, и человек вправе знать, чьи они. Числа берутся из тех же
    // списков, по которым идёт загрузка (resources.h), а не переписаны сюда:
    // разойтись им тогда негде.
    out += QStringLiteral("\n## Icons and fonts\n\n");
    out += QStringLiteral("- **Lucide** — %1 toolbar and tree icons, ISC\n")
               .arg(embeddedIcons().size());

    QStringList families;
    for (const EmbeddedFace& face : embeddedFaces()) {
        const QString family = QString::fromUtf8(face.family);
        if (!families.contains(family)) families << family;
    }
    out += QStringLiteral("- **%1** — %2 styles, OFL 1.1\n")
               .arg(families.join(QStringLiteral(" and ")))
               .arg(embeddedFaces().size());
    out += QStringLiteral(
        "\nThe fonts are bundled on purpose: the family name in settings is a request, "
        "not a promise. If the font is missing from the system, Qt silently substitutes "
        "whatever it finds, and the layout drifts on the first foreign machine.\n");
    return out;
}

QString licensesMarkdown() {
    // ОДИН ТЕКСТ — ОДИН РАЗ. Тексты сверяются между собой, а не пути: у libjxl
    // и jpegli файл лицензии совпадает байт в байт (оба — JPEG XL Project
    // Authors), и печатать полторы килобайты дважды значит удлинять и без того
    // длинную страницу без всякой пользы. Сравнение по содержимому, а не по
    // имени файла: если upstream однажды разойдётся, страница разойдётся с ним
    // сама, без нашего участия.
    //
    // Схлопывание — только для СОВПАВШИХ БАЙТ В БАЙТ. Похожие не схлопываем:
    // COPYING у libheif и libde265 отличаются двумя первыми строками — теми
    // самыми, что называют, к какой библиотеке относятся условия. Собрать их
    // «почти одинаковыми» значило бы показать условия не той библиотеки.
    struct Group {
        QString text;
        std::vector<const EmbeddedLicense*> owners;
    };
    std::vector<Group> groups;
    for (const EmbeddedLicense& item : embeddedLicenses()) {
        const QString text = embeddedText(item.path).trimmed();
        auto same = std::find_if(groups.begin(), groups.end(),
                                 [&text](const Group& g) { return g.text == text; });
        if (same == groups.end())
            groups.push_back({text, {&item}});
        else
            same->owners.push_back(&item);
    }

    QString out;
    for (const Group& group : groups) {
        QStringList names;
        for (const EmbeddedLicense* item : group.owners)
            names << QString::fromUtf8(item->name);
        // Лицензия у совпавших текстов одна по определению, поэтому берём её у
        // первого; разными могли бы быть только наши подписи, а не условия.
        out += QStringLiteral("# %1 — %2\n\n")
                   .arg(names.join(QStringLiteral(", ")),
                        QString::fromUtf8(group.owners.front()->license));
        if (group.owners.size() == 1) {
            out += QStringLiteral("%1\n\n").arg(QString::fromUtf8(group.owners.front()->what));
        } else {
            for (const EmbeddedLicense* item : group.owners)
                out += QStringLiteral("* **%1** — %2\n")
                           .arg(QString::fromUtf8(item->name), QString::fromUtf8(item->what));
            out += QStringLiteral("\n");
        }
        out += QStringLiteral("```\n%1\n```\n\n").arg(group.text);
    }
    return out;
}

AboutWindow::AboutWindow(QWidget* parent) : QDialog(parent) {
    setWindowTitle(QStringLiteral("zametti"));

    tabs_ = new QTabWidget(this);
    // ДВЕ ВКЛАДКИ, И БОЛЬШЕ НИ ОДНОЙ (решение владельца). Справки здесь нет:
    // документацию читают ЗАМЕТКАМИ, в папке Info дерева, — с тем же кеглем,
    // тем же масштабом по Ctrl+= и той же шириной колонки, что и свои записи.
    // Диалог 760×620 для длинного README был неудобен, и это была не мелочь:
    // окно «о программе» не читалка и читалкой стать не может.
    tabs_->addTab(markdownPage(aboutMarkdown(), tabs_), QStringLiteral("About"));

    // Лицензии — одной страницей, а не списком с выбором: человек, который
    // сюда пришёл, ищет либо одну конкретную (поиском по странице), либо
    // смотрит, чего вообще намешано. Оба случая — это листать.
    tabs_->addTab(markdownPage(licensesMarkdown(), tabs_), QStringLiteral("Licenses"));

    // «OK», а не «Close» с красным крестом (просьба владельца): окно ни о чём
    // не спрашивает и ничего не отменяет — это справка, и кнопка в ней значит
    // «посмотрел», а не «закрой, пока не поздно». Значок ей стиль подставляет
    // сам по роли, поэтому роль и меняем, а не подпись.
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok, this);
    // БЕЗ ЗНАЧКА, ОДНА НАДПИСЬ (просьба владельца): значок кнопке подставляет
    // стиль системы, и он тут ни о чём не говорит — «OK» и так однозначно.
    if (QPushButton* ok = buttons->button(QDialogButtonBox::Ok); ok != nullptr)
        ok->setIcon(QIcon());
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(tabs_, 1);
    layout->addWidget(buttons);
    resize(760, 620);
}

}  // namespace zametti
