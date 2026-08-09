#include "about_window.h"

#include "build_facts.h"
#include "document_builder.h"
#include "note_view.h"
#include "parser.h"
#include "resources.h"
#include "settings.h"

#include <QDialogButtonBox>
#include <QTabWidget>
#include <QTextDocument>
#include <QVBoxLayout>


namespace zametti {
namespace {

// Показ markdown нашим же путём: разбор ядром, сборка тем же сборщиком, что и
// заметки, тот же вид. Отличается только одно — править нельзя.
NoteView* markdownPage(const QString& markdown, QWidget* parent) {
    auto* view = new NoteView(parent);
    view->setReadOnly(true);
    // Документ принадлежит виду: своей жизни у справки нет, а Qt удалит его
    // вместе с родителем.
    auto* document = new QTextDocument(view);
    buildDocument(parse(markdown.toStdString()), *document, view->zoom());
    view->setDocument(document);
    view->applyContentWidth();
    return view;
}

}  // namespace

QString buildFactsMarkdown() {
    QString out;
    out += QStringLiteral("# Из чего собрано\n\n");
    for (const BuildFact& fact : kBuildFacts) {
        out += QStringLiteral("- **%1:** %2\n")
                   .arg(QString::fromUtf8(fact.name), QString::fromUtf8(fact.value));
    }
    out += QStringLiteral("- **читатели картинок:** %1\n")
               .arg(QString::fromUtf8(kImageReaders));

    out += QStringLiteral("\n## Вшитые библиотеки\n\n");
    for (const VendoredFact& fact : kVendoredFacts) {
        out += QStringLiteral("- **%1 %2** — %3\n")
                   .arg(QString::fromUtf8(fact.name), QString::fromUtf8(fact.version),
                        QString::fromUtf8(fact.what));
    }

    // Данные не заперты — это уговор владельца, и место ему здесь, рядом с
    // версиями: человек, читающий «о программе», как раз и спрашивает, что
    // будет с его заметками, если программа исчезнет.
    out += QStringLiteral(
        "\n## Данные не заперты\n\n"
        "Заметка — обычный markdown в UTF-8, вложение — обычный файл рядом. "
        "Хранилище читается и правится без этой программы: любым редактором, "
        "любым скриптом, чем угодно. Полное описание формата — во вкладке "
        "«Формат хранилища».\n");
    return out;
}

AboutWindow::AboutWindow(QWidget* parent) : QDialog(parent) {
    setWindowTitle(QStringLiteral("zametti"));

    tabs_ = new QTabWidget(this);
    // Справка: markdown из ресурсов, показанный нашим же ядром.
    for (const EmbeddedDoc& doc : embeddedDocs())
        tabs_->addTab(markdownPage(embeddedText(doc.path), tabs_),
                      QString::fromUtf8(doc.title));

    tabs_->addTab(markdownPage(buildFactsMarkdown(), tabs_), QStringLiteral("Сборка"));

    // Лицензии — одной страницей, а не списком с выбором: их одиннадцать, и
    // человек, который сюда пришёл, ищет либо одну конкретную (поиском по
    // странице), либо смотрит, чего вообще намешано. Оба случая — это листать.
    QString licenses;
    for (const EmbeddedLicense& item : embeddedLicenses()) {
        licenses += QStringLiteral("# %1 — %2\n\n%3\n\n```\n%4\n```\n\n")
                        .arg(QString::fromUtf8(item.name), QString::fromUtf8(item.license),
                             QString::fromUtf8(item.what), embeddedText(item.path).trimmed());
    }
    tabs_->addTab(markdownPage(licenses, tabs_), QStringLiteral("Лицензии"));

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(tabs_, 1);
    layout->addWidget(buttons);
    resize(760, 620);
}

}  // namespace zametti
