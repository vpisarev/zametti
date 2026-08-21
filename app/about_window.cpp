#include "about_window.h"

#include "build_facts.h"
#include "document_builder.h"
#include "note_view.h"
#include "resources.h"
#include "settings.h"

#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QIcon>
#include <QPushButton>
#include <QTextBrowser>
#include <QUrl>
#include <QStringList>
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
    // Ссылки НЕ ходят внутри окна. QTextBrowser в режиме чтения по щелчку
    // пытается загрузить адрес в себя, не может — и остаётся пустым насовсем:
    // документ уже подменён, вернуть его нечем, и окно чинится только выходом
    // из программы. Владелец на это и наткнулся. Внешние адреса уходят в
    // браузер, всё остальное не делает ничего.
    view->setOpenLinks(false);
    QObject::connect(view, &QTextBrowser::anchorClicked, view, [](const QUrl& url) {
        if (url.scheme() == QStringLiteral("http") || url.scheme() == QStringLiteral("https") ||
            url.scheme() == QStringLiteral("mailto"))
            QDesktopServices::openUrl(url);
    });
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

QString buildFactsMarkdown() {
    QString out;
    out += QStringLiteral("# What it is built from\n\n");
    for (const BuildFact& fact : kBuildFacts) {
        out += QStringLiteral("- **%1:** %2\n")
                   .arg(QString::fromUtf8(fact.name), QString::fromUtf8(fact.value));
    }
    out += QStringLiteral("- **image readers:** %1\n")
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

    // Данные не заперты — это уговор владельца, и место ему здесь, рядом с
    // версиями: человек, читающий «о программе», как раз и спрашивает, что
    // будет с его заметками, если программа исчезнет.
    out += QStringLiteral(
        "\n## Your data is not locked in\n\n"
        "A note is plain markdown in UTF-8, an attachment is a plain file next to it. "
        "The store can be read and edited without this program: with any editor, "
        "any script, anything at all. The full format description is in the "
        "“Store format” tab.\n");
    return out;
}

AboutWindow::AboutWindow(QWidget* parent) : QDialog(parent) {
    setWindowTitle(QStringLiteral("zametti"));

    tabs_ = new QTabWidget(this);
    // Справка: markdown из ресурсов, показанный нашим же ядром.
    for (const EmbeddedDoc& doc : embeddedDocs())
        tabs_->addTab(markdownPage(embeddedText(doc.path), tabs_),
                      QString::fromUtf8(doc.title));

    tabs_->addTab(markdownPage(buildFactsMarkdown(), tabs_), QStringLiteral("Build"));

    // Лицензии — одной страницей, а не списком с выбором: их одиннадцать, и
    // человек, который сюда пришёл, ищет либо одну конкретную (поиском по
    // странице), либо смотрит, чего вообще намешано. Оба случая — это листать.
    QString licenses;
    for (const EmbeddedLicense& item : embeddedLicenses()) {
        licenses += QStringLiteral("# %1 — %2\n\n%3\n\n```\n%4\n```\n\n")
                        .arg(QString::fromUtf8(item.name), QString::fromUtf8(item.license),
                             QString::fromUtf8(item.what), embeddedText(item.path).trimmed());
    }
    tabs_->addTab(markdownPage(licenses, tabs_), QStringLiteral("Licenses"));

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
