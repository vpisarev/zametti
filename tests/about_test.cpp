// Окно «О программе»: что в нём есть на самом деле.
//
// Проверяется не расположение, а содержимое, и главное — ЛИЦЕНЗИИ. Вшитый в
// бинарник чужой шрифт без текста лицензии рядом — это нарушение чужих
// условий, которое видно только тому, кто откроет вкладку. Пустая вкладка
// молчит, поэтому спрашивает набор: у каждой строки списка есть текст, он
// непуст и похож на лицензию.

#include "doc_model.h"
#include "about_window.h"
#include "resources.h"
#include "test_util.h"

#include <vector>
#include "testdata.h"

#include <QApplication>
#include <QTabWidget>
#include <QTest>
#include <QTextBlock>
#include <QTextBrowser>
#include <QTextFragment>
#include <QUrl>
#include <QTextDocument>
#include <QTextEdit>

#include <string>

namespace {

void check(bool ok, const std::string& what) {
    ++zt::g_checks;
    if (ok) return;
    ++zt::g_failures;
    std::printf("провал: %s\n", what.c_str());
}

// Весь видимый текст вкладки: страницы собраны нашим сборщиком, и разметки в
// тексте документа нет — значит искать в нём можно прямо словами.
QString pageText(QTabWidget* tabs, int index) {
    auto* view = qobject_cast<QTextEdit*>(tabs->widget(index));
    return view == nullptr ? QString() : view->document()->toPlainText();
}

int tabWith(QTabWidget* tabs, const QString& title) {
    for (int i = 0; i < tabs->count(); ++i)
        if (tabs->tabText(i) == title) return i;
    return -1;
}

// Каждая лицензия из списка вшита и непуста. Список — тот же, по которому
// строится окно: расходиться им негде.
void checkLicensesEmbedded() {
    check(!zametti::embeddedLicenses().empty(), "список лицензий не пуст");
    for (const zametti::EmbeddedLicense& item : zametti::embeddedLicenses()) {
        const QString text = zametti::embeddedText(item.path);
        const std::string name = item.name;
        check(!text.isEmpty(), "лицензия вшита: " + name);
        // Двести знаков — порог от «файл есть, но в нём заглушка». Самая
        // короткая настоящая лицензия здесь (ISC) — больше семисот.
        check(text.size() > 200, "лицензия не заглушка: " + name);
        const bool looksLikeLicense =
            text.contains(QStringLiteral("License"), Qt::CaseInsensitive) ||
            text.contains(QStringLiteral("Copyright"), Qt::CaseInsensitive) ||
            text.contains(QStringLiteral("PERMISSION"), Qt::CaseInsensitive);
        check(looksLikeLicense, "текст похож на лицензию: " + name);
    }
}

void checkDocsEmbedded() {
    check(!zametti::embeddedDocs().empty(), "список справки не пуст");
    for (const zametti::EmbeddedDoc& doc : zametti::embeddedDocs()) {
        const QString text = zametti::embeddedText(doc.path);
        check(text.size() > 500, std::string("справка вшита и непуста: ") + doc.title);
    }
}

// Числа сборки берутся из порождённого CMake заголовка. Если он когда-нибудь
// разойдётся с настройкой, здесь будут пустые значения вместо версий.
void checkBuildFacts() {
    const QString facts = zametti::buildFactsMarkdown();
    check(facts.contains(QStringLiteral("md4c")), "версия md4c в сводке сборки");
    check(facts.contains(QStringLiteral("libjxl")), "версия libjxl в сводке сборки");
    check(facts.contains(QStringLiteral("Qt")), "версия Qt в сводке сборки");
    // Незаполненная подстановка CMake выглядит как "@ИМЯ@" — так молча
    // выглядел бы и потерянный configure_file.
    check(!facts.contains(QLatin1Char('@')), "подстановки CMake раскрыты все");
    // Пустая версия — это "**md4c ** — разбор markdown": библиотека названа,
    // а версии нет. Ловим по двойному пробелу перед тире.
    check(!facts.contains(QStringLiteral("  —")), "пустых версий в сводке нет");
    // Иконки и шрифты — тоже вшитое, и о них сказано числами из тех же
    // списков, по которым идёт загрузка.
    check(facts.contains(QStringLiteral("Lucide")), "иконки названы в сводке");
    check(facts.contains(QStringLiteral("IBM Plex")), "шрифты названы в сводке");
    check(facts.contains(QStringLiteral("%1").arg(zametti::embeddedIcons().size())),
          "число иконок — из списка загрузки, а не переписано");
    check(facts.contains(QStringLiteral("%1").arg(zametti::embeddedFaces().size())),
          "число начертаний — из списка загрузки");

    // Два списка вшитого живут в разных местах: лицензии — в resources.cpp,
    // версии — в build_facts.h.in. Разойтись им ничего не мешает, и они
    // разошлись: dtl приехал на этапе 10 с лицензией, но без версии, и
    // заметил это владелец, а не набор. Спрашиваем связь: всё, у чего есть
    // лицензия, названо и в сводке сборки. Имена сверяем без учёта регистра —
    // в списке лицензий пишется «DTL» и «BLAKE3».
    for (const zametti::EmbeddedLicense& item : zametti::embeddedLicenses()) {
        const QString name = QString::fromUtf8(item.name);
        // Сама программа — не вшитая библиотека, её версии в этом списке нет.
        if (name == QStringLiteral("zametti")) continue;
        check(facts.contains(name, Qt::CaseInsensitive),
              "вшитое названо и в сводке сборки: " + std::string(item.name));
    }
}

void checkWindow() {
    zametti::AboutWindow about;
    QTabWidget* tabs = about.tabs();
    check(tabs != nullptr && tabs->count() >= 4, "вкладок в окне не меньше четырёх");
    if (tabs == nullptr) return;

    const int licenses = tabWith(tabs, QStringLiteral("Лицензии"));
    check(licenses >= 0, "вкладка лицензий есть");
    if (licenses >= 0) {
        const QString text = pageText(tabs, licenses);
        for (const zametti::EmbeddedLicense& item : zametti::embeddedLicenses())
            check(text.contains(QString::fromUtf8(item.name)),
                  std::string("на вкладке лицензий названо: ") + item.name);
        check(text.size() > 5000, "тексты лицензий на вкладке, а не одни заголовки");
    }

    const int readme = tabWith(tabs, QStringLiteral("О программе"));
    check(readme >= 0, "вкладка README есть");
    if (readme >= 0) {
        const QString text = pageText(tabs, readme);
        check(text.contains(QStringLiteral("zametti")), "README показан");
        // Разметка markdown в тексте документа не остаётся: заголовок собран
        // жирным, а не решёткой. Если решётки видны — README показывают не
        // нашим сборщиком, а как есть.
        //
        // Искать «##» где попало нельзя: в самом README есть строка про
        // синтаксис заголовков, и внутри строчного кода решётки стоят законно.
        // Спрашиваем именно про НАЧАЛО строки — там разметке взяться неоткуда.
        bool rawMarkup = false;
        const QStringList lines = text.split(QLatin1Char('\n'));
        for (const QString& line : lines)
            if (line.startsWith(QStringLiteral("# ")) || line.startsWith(QStringLiteral("## "))) {
                rawMarkup = true;
                std::printf("  сырая строка: %s\n", line.toUtf8().constData());
                break;
            }
        check(!rawMarkup, "справка собрана, а не показана сырьём");
    }

    // Ссылки НЕ ходят внутри окна. QTextBrowser в режиме чтения по щелчку
    // грузит адрес в себя, не может — и остаётся пустым насовсем; окно
    // чинилось только выходом из программы. Проверяем и признак, и сам щелчок.
    for (int i = 0; i < tabs->count(); ++i) {
        auto* view = qobject_cast<QTextBrowser*>(tabs->widget(i));
        check(view != nullptr && !view->openLinks(),
              std::string("ссылки не ходят внутри вкладки ") +
                  tabs->tabText(i).toUtf8().constData());
        if (view == nullptr) continue;
    }

    const int storage = tabWith(tabs, QStringLiteral("Формат хранилища"));
    check(storage >= 0, "вкладка формата хранилища есть");

    // И настоящим щелчком по настоящей ссылке — потому что признак можно
    // выставить, а обработчик написать неверно. Сигнал руками тут не годится:
    // переход у QTextBrowser заведён на СВОЙ разбор нажатия, и «испущенный»
    // anchorClicked ничего не ломает — такая проверка была бы пустышкой.
    if (readme >= 0) {
        auto* view = qobject_cast<QTextBrowser*>(tabs->widget(readme));
        tabs->setCurrentIndex(readme);
        about.resize(900, 700);
        about.show();
        QApplication::processEvents();

        QTextCursor anchor;
        for (QTextBlock block = view->document()->begin();
             block.isValid() && anchor.isNull(); block = block.next()) {
            for (QTextBlock::iterator it = block.begin(); !it.atEnd(); ++it) {
                const QTextFragment fragment = it.fragment();
                if (!fragment.isValid() || !fragment.charFormat().isAnchor()) continue;
                anchor = QTextCursor(view->document());
                anchor.setPosition(fragment.position() + fragment.length() / 2);
                break;
            }
        }
        check(!anchor.isNull(), "в README есть хотя бы одна ссылка");
        if (!anchor.isNull()) {
            view->setTextCursor(anchor);
            view->ensureCursorVisible();
            QApplication::processEvents();
            const QString before = view->document()->toPlainText();
            const QRect box = view->cursorRect(anchor);
            QTest::mouseClick(view->viewport(), Qt::LeftButton, Qt::NoModifier, box.center());
            QApplication::processEvents();
            check(view->document()->toPlainText() == before,
                  "после щелчка по ссылке страница цела");
            check(!view->document()->toPlainText().isEmpty(), "страница не опустела");
        }
    }

    const int build = tabWith(tabs, QStringLiteral("Сборка"));
    check(build >= 0, "вкладка сборки есть");
    if (build >= 0) check(pageText(tabs, build).contains(QStringLiteral("md4c")),
                          "версии вшитого видны в окне");
}

}  // namespace

static int ztRunSuite(int argc, char** argv) {

    // Снимок окна для глаз: приёмка расположения — дело картинки, а не текста.
    // В ctest не попадает, зовётся руками: about_test --shot <файл> [вкладка].
    if (argc > 2 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("--shot")) {
        zametti::loadEmbeddedFonts();
        zametti::AboutWindow about;
        about.resize(900, 700);
        about.show();
        if (argc > 3) about.tabs()->setCurrentIndex(QString::fromLocal8Bit(argv[3]).toInt());
        QApplication::processEvents();
        const bool saved = about.grab().save(QString::fromLocal8Bit(argv[2]));
        std::printf("снимок %s: %s\n", saved ? "записан" : "НЕ записан", argv[2]);
        return saved ? 0 : 1;
    }

    checkLicensesEmbedded();
    checkDocsEmbedded();
    checkBuildFacts();
    checkWindow();

    return zt::report("about");
}

// Набор целиком одним TEST: тело не тронуто, argv ему собран здесь.
// Дробить на отдельные проверки — отдельная работа, по одному набору.
TEST(About, All) {
    std::vector<QByteArray> ztArgs{QByteArrayLiteral("about_test")};
    std::vector<char*> ztArgv;
    for (QByteArray& a : ztArgs) ztArgv.push_back(a.data());
    EXPECT_EQ(0, ztRunSuite(int(ztArgv.size()), ztArgv.data()));
}
