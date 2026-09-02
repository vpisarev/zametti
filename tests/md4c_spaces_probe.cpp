// ПРОБНИК: обычные ведущие пробелы против md4c с NOINDENTEDCODEBLOCKS.
//
// Вопрос перед отказом от NBSP в файле: если писать отступы и выравнивание
// ОБЫЧНЫМИ пробелами, что именно прочитает назад md4c? Флаг
// MD_FLAG_NOINDENTEDCODEBLOCKS снимает главные грабли (4+ ведущих пробела =
// indented code block), но у него известен побочный эффект: пороги
// `indent < code_indent_offset` становятся всегда истинными, и маркеры
// списков, цитат, заголовков, заборов, setext и черты распознаются на ЛЮБОЙ
// глубине отступа. Пробник печатает разбор матрицы краёв — по нему пишется
// список знаков, которые писатель обязан экранировать после ведущих пробелов.
//
//   zametti-bench md-spaces

// Библиотека собрана под UTF-16 (PUBLIC-дефайн цели md4c); включение без
// макроса дало бы char-версию типов и ABI-рассинхрон с библиотекой.
#define MD4C_USE_UTF16
#include "../3rdparty/md4c/md4c.h"

#include <QString>

#include <cstdio>
#include <string>

namespace {

std::string g_log;
int g_depth = 0;

const char* blockName(MD_BLOCKTYPE type) {
    switch (type) {
        case MD_BLOCK_DOC: return "doc";
        case MD_BLOCK_QUOTE: return "quote";
        case MD_BLOCK_UL: return "ul";
        case MD_BLOCK_OL: return "ol";
        case MD_BLOCK_LI: return "li";
        case MD_BLOCK_HR: return "hr";
        case MD_BLOCK_H: return "h";
        case MD_BLOCK_CODE: return "code";
        case MD_BLOCK_HTML: return "html";
        case MD_BLOCK_P: return "p";
        case MD_BLOCK_TABLE: return "table";
        default: return "?";
    }
}

int enterBlock(MD_BLOCKTYPE type, void*, void*) {
    if (type != MD_BLOCK_DOC) {
        g_log += std::string(size_t(g_depth) * 2, ' ') + blockName(type) + "\n";
        ++g_depth;
    }
    return 0;
}
int leaveBlock(MD_BLOCKTYPE type, void*, void*) {
    if (type != MD_BLOCK_DOC) --g_depth;
    return 0;
}
int enterSpan(MD_SPANTYPE, void*, void*) { return 0; }
int leaveSpan(MD_SPANTYPE, void*, void*) { return 0; }

int onText(MD_TEXTTYPE type, const MD_CHAR* text, MD_SIZE size, void*) {
    const QString piece = QString::fromUtf16(reinterpret_cast<const char16_t*>(text), size);
    std::string shown;
    for (const QChar qc : piece) {
        if (qc == QLatin1Char('\n'))
            shown += "\\n";
        else if (qc == QLatin1Char(' '))
            shown += "·";   // пробел видимым знаком: их и меряем
        else
            shown += QString(qc).toUtf8().constData();
    }
    g_log += std::string(size_t(g_depth) * 2, ' ') + "text" +
             (type == MD_TEXT_BR ? "(br)" : type == MD_TEXT_SOFTBR ? "(soft)" : "") +
             " '" + shown + "'\n";
    return 0;
}

void probe(const char* what, const char* source, unsigned extraFlags) {
    g_log.clear();
    g_depth = 0;
    MD_PARSER parser = {};
    parser.abi_version = 0;
    parser.flags = MD_FLAG_TABLES | MD_FLAG_STRIKETHROUGH | MD_FLAG_TASKLISTS |
                   MD_FLAG_PERMISSIVEATXHEADERS | MD_FLAG_LATEXMATHSPANS | extraFlags;
    parser.enter_block = enterBlock;
    parser.leave_block = leaveBlock;
    parser.enter_span = enterSpan;
    parser.leave_span = leaveSpan;
    parser.text = onText;

    const QString text = QString::fromUtf8(source);
    md_parse(reinterpret_cast<const MD_CHAR*>(text.utf16()), MD_SIZE(text.size()), &parser,
             nullptr);
    std::string shownSource;
    for (const char* p = source; *p != 0; ++p)
        shownSource += *p == '\n' ? std::string("\\n") : *p == ' ' ? std::string("·")
                                                                   : std::string(1, *p);
    std::printf("--- %s: '%s'\n%s", what, shownSource.c_str(), g_log.c_str());
}

}  // namespace

int ztMdSpacesProbe(int argc, char** argv) {
    const unsigned noIndent = MD_FLAG_NOINDENTEDCODEBLOCKS;
    // Разовый ввод: zametti-bench md-spaces 'сырой markdown с \n'.
    if (argc > 1) {
        QString raw = QString::fromUtf8(argv[1]);
        raw.replace(QStringLiteral("\\n"), QStringLiteral("\n"));
        probe("ввод", raw.toUtf8().constData(), noIndent);
        return 0;
    }

    std::printf("=== БЕЗ ФЛАГА: 4 ведущих пробела — код ===\n");
    probe("4 пробела", "    abc\n", 0);

    std::printf("\n=== С ФЛАГОМ NOINDENTEDCODEBLOCKS ===\n");
    probe("1 пробел", " abc\n", noIndent);
    probe("3 пробела", "   abc\n", noIndent);
    probe("4 пробела", "    abc\n", noIndent);
    probe("8 пробелов", "        abc\n", noIndent);
    probe("продолжение абзаца с отступом", "abc\n    def\n", noIndent);
    probe("после пустой строки", "abc\n\n    def\n", noIndent);
    probe("серия в середине", "int a   = 5\n", noIndent);
    probe("хвостовые два пробела (hard break)", "abc  \ndef\n", noIndent);
    probe("таб в отступе", "\tabc\n", noIndent);

    std::printf("\n=== МАРКЕРЫ НА ГЛУБОКОМ ОТСТУПЕ (что экранировать) ===\n");
    probe("дефис-список", "    - foo\n", noIndent);
    probe("звёздочка", "    * foo\n", noIndent);
    probe("плюс", "    + foo\n", noIndent);
    probe("номер", "    1. foo\n", noIndent);
    probe("цитата", "    > foo\n", noIndent);
    probe("заголовок", "    # foo\n", noIndent);
    probe("забор кода", "    ```\n    int a;\n    ```\n", noIndent);
    probe("setext =", "abc\n    ===\n", noIndent);
    probe("setext -", "abc\n    ---\n", noIndent);
    probe("черта ***", "    ***\n", noIndent);
    probe("черта ---", "    ---\n", noIndent);

    std::printf("\n=== СПИСКИ И ЦИТАТЫ ===\n");
    probe("пункт: продолжение на content+4", "- пункт\n      хвост\n", noIndent);
    probe("вложенный список", "- раз\n  - два\n", noIndent);
    probe("пункт: было бы кодом внутри пункта", "- пункт\n\n        код?\n", noIndent);
    probe("цитата с пустой строкой", "> a\n>\n> b\n", noIndent);
    probe("цитата: хвостовой пробел после >", "> a\n> \n> b\n", noIndent);

    return 0;
}
