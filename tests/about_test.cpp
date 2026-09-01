// Окно «О программе»: что в нём есть на самом деле.
//
// Проверяется не расположение, а содержимое, и главное — ЛИЦЕНЗИИ. Вшитый в
// бинарник чужой шрифт без текста лицензии рядом — это нарушение чужих
// условий, которое видно только тому, кто откроет вкладку. Пустая вкладка
// молчит, поэтому спрашивает набор: у каждой строки списка есть текст, он
// непуст и похож на лицензию.

#include "doc_model.h"
#include "about_window.h"
#include "build_facts.h"
#include "resources.h"
#include "test_util.h"

#include <vector>
#include "testdata.h"

#include <QApplication>
#include <QDialogButtonBox>
#include <QPushButton>
#include <QRegularExpression>
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

// Документация вшита и читается. Списка здесь нет: что вшито, решает CMake
// обходом docs/info/, а мы спрашиваем у самих ресурсов — иначе набор проверял
// бы свой список, а не то, что попало в программу.
void checkDocsEmbedded() {
    check(!zametti::embeddedDocs().isEmpty(), "вшитая документация есть");
    for (const QString& path : zametti::embeddedDocs()) {
        const QString text = zametti::embeddedText(path.toUtf8().constData());
        check(text.size() > 500,
              std::string("документ вшит и непуст: ") + path.toUtf8().constData());
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
    // выглядел бы и потерянный configure_file. Ищем ИМЕННО ЕЁ, а не всякую
    // собаку: первая редакция запрещала символ '@' вовсе и покраснела от
    // честной строки версии `1.0.0 (openmath @086f4eb, 2024-08-05)` —
    // проверка ловила не то, что называла.
    check(!facts.contains(QRegularExpression(QStringLiteral("@[A-Z0-9_]+@"))),
          "подстановки CMake раскрыты все");
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

// Связь в ОБРАТНУЮ сторону: всё, что названо вшитой библиотекой, обязано иметь
// текст лицензии. Этой проверки не было, и список лицензий тихо отстал на
// шесть библиотек — libwebp, microtex, libsodium, libheif, libde265, libgav1,
// причём две последние под LGPL-3, где показ текста не вежливость, а условие.
// Заметил владелец, а не набор.
void checkEveryBundledLibraryHasLicense() {
    for (const zametti::VendoredFact& fact : zametti::kVendoredFacts) {
        const QString name = QString::fromUtf8(fact.name);
        bool found = false;
        for (const zametti::EmbeddedLicense& item : zametti::embeddedLicenses())
            if (name.compare(QString::fromUtf8(item.name), Qt::CaseInsensitive) == 0) found = true;
        check(found, "у вшитой библиотеки есть текст лицензии: " + std::string(fact.name));
    }
}

// Один текст — один раз. Байт в байт совпавшие лицензии (libjxl и jpegli) идут
// общим заголовком, а похожие, но разные (COPYING у libheif и libde265
// отличаются строкой, называющей библиотеку) — порознь.
void checkLicensesPage() {
    const QString page = zametti::licensesMarkdown();
    for (const zametti::EmbeddedLicense& item : zametti::embeddedLicenses())
        check(page.contains(QString::fromUtf8(item.name)),
              "составная часть названа на странице лицензий: " + std::string(item.name));

    // Ни один текст не выведен ОТДЕЛЬНЫМ БЛОКОМ дважды. Спрашиваем именно про
    // блок в тройных кавычках, а не про вхождение текста куда угодно, и вот
    // почему: LICENSE у highway — двойная лицензия, и внутрь него целиком
    // вложены и текст Apache-2.0 (он же весь файл у libgav1), и текст CC0 (он
    // же весь файл у BLAKE3). Первая редакция этой проверки искала вхождение
    // и покраснела на обоих — честно найдя наложение, которое разобрать нельзя:
    // разнять чужой файл лицензии на части мы не вправе.
    for (const zametti::EmbeddedLicense& item : zametti::embeddedLicenses()) {
        const QString fenced = QStringLiteral("```\n%1\n```")
                                   .arg(zametti::embeddedText(item.path).trimmed());
        const qsizetype at = page.indexOf(fenced);
        check(at >= 0, std::string("текст лицензии на странице: ") + item.name);
        if (at >= 0)
            check(page.indexOf(fenced, at + 1) < 0,
                  std::string("текст лицензии выведен один раз: ") + item.name);
    }

    // Схлопывание действительно случилось: у libjxl и jpegli файл совпадает
    // байт в байт, и заголовок обязан назвать обоих сразу. Если тексты
    // разойдутся у upstream, эта проверка покраснеет — и это правильно:
    // значит, схлопывать больше нечего и строку надо снимать осознанно.
    const bool sameText = zametti::embeddedText(":/licenses/libjxl.txt").trimmed() ==
                          zametti::embeddedText(":/licenses/jpegli.txt").trimmed();
    check(sameText, "у libjxl и jpegli лицензия совпадает байт в байт");
    if (sameText)
        check(page.contains(QStringLiteral("# libjxl, jpegli — ")),
              "совпавшие лицензии показаны общим заголовком");
}

void checkWindow() {
    zametti::AboutWindow about;
    QTabWidget* tabs = about.tabs();
    // ДВЕ ВКЛАДКИ, И НЕ БОЛЬШЕ (решение владельца): «About» — чьё это и из чего
    // собрано, «Licenses» — на каких условиях. Документация уехала в дерево
    // заметок, папкой Info, и вкладки README здесь больше нет.
    check(tabs != nullptr && tabs->count() == 2, "вкладок в окне ровно две");
    if (tabs == nullptr) return;

    const int licenses = tabWith(tabs, QStringLiteral("Licenses"));
    check(licenses >= 0, "вкладка лицензий есть");
    if (licenses >= 0) {
        const QString text = pageText(tabs, licenses);
        for (const zametti::EmbeddedLicense& item : zametti::embeddedLicenses())
            check(text.contains(QString::fromUtf8(item.name)),
                  std::string("на вкладке лицензий названо: ") + item.name);
        check(text.size() > 5000, "тексты лицензий на вкладке, а не одни заголовки");
    }

    const int first = tabWith(tabs, QStringLiteral("About"));
    check(first >= 0, "первая вкладка есть");
    if (first >= 0) {
        const QString text = pageText(tabs, first);
        // Имя программы — как его пишет владелец в самом тексте («Zametti
        // 0.9.0»); набор сверяет НАЛИЧИЕ имени, а не его написание, иначе
        // всякая правка заглавной буквы красит окно справки в красный.
        check(text.contains(QStringLiteral("zametti"), Qt::CaseInsensitive),
              "имя программы названо");
        // Обе половины первой вкладки на месте: чьё это и из чего собрано.
        check(text.contains(QStringLiteral("Copyright")), "copyright на первой вкладке");
        check(text.contains(QStringLiteral("GPL-3.0")), "лицензия названа");
        check(text.contains(QStringLiteral("Qt")), "версии сборки на той же вкладке");
        check(text.contains(QStringLiteral("md4c")), "и вшитые библиотеки тоже");
        // Дверь к документации названа словами: человек, искавший README
        // здесь, обязан узнать, куда он переехал.
        check(text.contains(QStringLiteral("Info")), "сказано, где теперь документация");
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

    // Вкладок README и «Store format» больше нет — документация читается
    // заметками (набор InfoFolder стережёт, что она там есть и читается).
    check(tabWith(tabs, QStringLiteral("Store format")) < 0, "вкладки формата хранилища нет");
    check(tabWith(tabs, QStringLiteral("Build")) < 0, "отдельной вкладки сборки нет");

    // Щелчок по настоящей ссылке проверяется там, где ссылки теперь и живут, —
    // в читалке документации (набор InfoFolder). Здесь остаётся признак: обе
    // вкладки не ходят по ссылкам сами.
}


// КНОПКА ОКНА СПРАВКИ — «OK», А НЕ «CLOSE» (просьба владельца): окно ни о чём
// не спрашивает, и красный крест на нём выглядит тревожнее, чем повод. Роль
// кнопки — то, по чему стиль выбирает и подпись, и значок; её и спрашиваем.
void checkOkButton() {
    zametti::AboutWindow window;
    auto* buttons = window.findChild<QDialogButtonBox*>();
    check(buttons != nullptr, "кнопки окна справки нашлись");
    if (buttons == nullptr) return;
    check(buttons->standardButtons() == QDialogButtonBox::Ok,
          "кнопка одна и это OK");
    QPushButton* ok = buttons->button(QDialogButtonBox::Ok);
    check(ok != nullptr && !ok->text().isEmpty(), "у неё есть подпись");
    // И БЕЗ ЗНАЧКА (просьба владельца): значок ставит стиль системы, и на
    // разных стилях он разный — здесь его нет вовсе.
    check(ok != nullptr && ok->icon().isNull(), "и нет значка — только надпись");
    check(buttons->button(QDialogButtonBox::Close) == nullptr, "кнопки Close больше нет");
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
    checkEveryBundledLibraryHasLicense();
    checkLicensesPage();
    checkWindow();
    checkOkButton();

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

// Снимок окна справки — артефакт приёмки (владелец смотрит глазами на кнопку и
// на вкладки). Не проверка: набор им ничего не решает.
TEST(AboutShot, All) {
    zametti::AboutWindow window;
    window.resize(900, 640);
    window.show();
    QTest::qWait(200);
    const QString path = zt::TestData::outDir(QStringLiteral("about")) +
                         QStringLiteral("/окно-справки.png");
    if (window.grab().save(path)) std::printf("снимок справки: %s\n", qPrintable(path));
}
