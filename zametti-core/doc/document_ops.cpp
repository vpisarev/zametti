// СТРОЕНИЕ И НАЧЕРТАНИЕ — глаголами заметки, поверх базиса правки.
//
// Здесь живёт политика: что значит «сделать пунктом», «поставить заголовок»,
// «нажать Tab». Само изменение документа делает базис (document_edit.cpp), а
// разметка блоков — местные функции editor_ops.cpp, наружу из ядра не
// выходящие.
//
// ЧТО ЗДЕСЬ ГЛАВНОЕ: правка МЕСТНАЯ. Прежде каждая из этих операций кончалась
// обходом всего документа (piecesOf) и полной пересборкой — на заметке в 239 КБ
// это 1.9 мс обхода и 151 мс сборки, и платилось это за каждое нажатие Ctrl+B.
// Теперь пересобирается только тронутый диапазон, и цена операции стала ценой
// диапазона, а не ценой заметки.

#include "document_impl.h"

#include "doc_model.h"
#include "document_builder.h"
#include "document_pieces.h"
#include "editor_ops.h"

#include <QTextBlock>
#include <QTextCharFormat>
#include <QTextCursor>

#include <vector>

namespace zametti {
namespace {

// Биты начертания, которыми меряет разметка. Перевод здесь один: наружу из
// заметки уходит Style, внутрь — биты, и голых чисел за пределами ядра нет.
int bitsOf(ZDocument::Style style) {
    switch (style) {
        case ZDocument::Style::Bold: return SpanBold;
        case ZDocument::Style::Italic: return SpanItalic;
        case ZDocument::Style::Strike: return SpanStrike;
        case ZDocument::Style::Code: return SpanCode;
    }
    return 0;
}

}  // namespace

#ifndef NDEBUG
void ZDocument::checkCanonical() const {
    std::vector<Piece> now;
    walkPieces(d_->text, [&now](const Piece& piece) {
        now.push_back(piece);
        return true;
    });
    checkMatchesBuild(now, d_->text);
}
#endif

bool ZDocument::runLocalEdit(QTextCursor& at, const std::function<bool(QTextCursor&)>& body) {
    if (at.document() != &d_->text) return false;

    // Диапазон СЧИТАЕМ ТАК ЖЕ, КАК ЕГО СЧИТАЮТ САМИ ОПЕРАЦИИ: они правят блоки
    // выделения вместе с поддеревьями пунктов, и пересобрать надо ровно их.
    const BlockRange wanted = selectedBlocks(d_->text, at);
    const int countBefore = d_->text.blockCount();

    QTextCursor edit(at);
    edit.beginEditBlock();
    if (!body(edit)) {
        edit.endEditBlock();
        return false;
    }

    // Операция могла завести блоки (разрез строки в отдельный блок, пустая
    // строка у заголовка) — на столько же съехало всё, что ниже.
    const int grew = qMax(0, d_->text.blockCount() - countBefore);
    const int here = d_->text.findBlock(edit.position()).blockNumber();
    const int there = d_->text.findBlock(edit.anchor()).blockNumber();
    const int first = qMin(qMin(wanted.first, here), there) - 1;
    const int last = qMax(qMax(wanted.last + grew, here), there) + 1;

    rebuildRange(first, last, &edit);
    settleSeam(first, last);
    edit.endEditBlock();

#ifndef NDEBUG
    checkCanonical();
#endif

    at = edit;
    return true;
}

// --- НАЧЕРТАНИЕ -------------------------------------------------------------

bool ZDocument::toggleStyle(QTextCursor& at, Style style) {
    return runLocalEdit(at, [this, style](QTextCursor& edit) {
        switch (style) {
            case Style::Bold: return toggleBold(d_->text, edit);
            case Style::Italic: return toggleItalic(d_->text, edit);
            case Style::Strike: return toggleStrike(d_->text, edit);
            case Style::Code: return toggleCode(d_->text, edit);
        }
        return false;
    });
}

QTextCharFormat ZDocument::styleForTyping(const QTextCursor& at,
                                          const QTextCharFormat& current, Style style) const {
    if (at.document() != &d_->text) return current;
    // В блоке кода и в дословном куске текст буквальный — начертанию там взяться
    // неоткуда, ровно как и при выделении. Спрашивают об этом заметку, а не
    // смотрят на блок снаружи: снаружи про блоки кода знать не должны.
    const QTextBlock block = at.block();
    if (isRawBlock(block) || kindOf(block) == Kind::Code) return current;
    return inlineStyleForTyping(block, current, bitsOf(style));
}

// --- РОД БЛОКА --------------------------------------------------------------

bool ZDocument::setHeadingLevel(QTextCursor& at, int level) {
    return runLocalEdit(at, [this, level](QTextCursor& edit) {
        return zametti::setHeadingLevel(d_->text, edit, level);
    });
}

bool ZDocument::makeBullet(QTextCursor& at) {
    return runLocalEdit(at, [this](QTextCursor& edit) {
        return zametti::makeBullet(d_->text, edit);
    });
}

bool ZDocument::makeOrdered(QTextCursor& at) {
    return runLocalEdit(at, [this](QTextCursor& edit) {
        return zametti::makeOrdered(d_->text, edit);
    });
}

bool ZDocument::makeTask(QTextCursor& at) {
    return runLocalEdit(at, [this](QTextCursor& edit) {
        return zametti::makeTask(d_->text, edit);
    });
}

bool ZDocument::makeParagraph(QTextCursor& at) {
    return runLocalEdit(at, [this](QTextCursor& edit) {
        return zametti::makeParagraph(d_->text, edit);
    });
}

bool ZDocument::toggleComment(QTextCursor& at) {
    return runLocalEdit(at, [this](QTextCursor& edit) {
        return toggleCommentAtCursor(d_->text, edit);
    });
}

bool ZDocument::toggleTask(QTextCursor& at) {
    return runLocalEdit(at, [this](QTextCursor& edit) {
        return toggleTaskAtCursor(d_->text, edit);
    });
}

// --- АВТОЗАМЕНЫ ПРИ НАБОРЕ --------------------------------------------------

bool ZDocument::applyInputRule(QTextCursor& at) {
    return runLocalEdit(at, [this](QTextCursor& edit) {
        return applyInputRuleAtCursor(d_->text, edit);
    });
}

bool ZDocument::applyCodeSpanRule(QTextCursor& at) {
    return runLocalEdit(at, [this](QTextCursor& edit) {
        return applyCodeSpanRuleAtCursor(d_->text, edit);
    });
}

bool ZDocument::applyDividerRule(QTextCursor& at) {
    return runLocalEdit(at, [this](QTextCursor& edit) {
        return applyDividerRuleAtCursor(d_->text, edit);
    });
}

// --- ФОТОГРАФИЯ -------------------------------------------------------------

bool ZDocument::setImageWidth(QTextCursor& at, int width) {
    return runLocalEdit(at, [this, width](QTextCursor& edit) {
        return setImageWidthAtCursor(d_->text, edit, width);
    });
}

bool ZDocument::setImageAlign(QTextCursor& at, ImageAlign align) {
    return runLocalEdit(at, [this, align](QTextCursor& edit) {
        return setImageAlignAtCursor(d_->text, edit, align);
    });
}

bool ZDocument::cutImageLine(QTextCursor& at) {
    return runLocalEdit(at, [this](QTextCursor& edit) {
        return cutImageLineAtCursor(d_->text, edit);
    });
}

bool ZDocument::deleteImageAbove(QTextCursor& at) {
    return runLocalEdit(at, [this](QTextCursor& edit) {
        return deleteImageLineBackward(d_->text, edit);
    });
}

bool ZDocument::deleteImageBelow(QTextCursor& at) {
    return runLocalEdit(at, [this](QTextCursor& edit) {
        return deleteImageLineForward(d_->text, edit);
    });
}

// --- БЛОК КОДА --------------------------------------------------------------

bool ZDocument::setCodeLanguage(QTextCursor& at, const QString& language) {
    return runLocalEdit(at, [this, language](QTextCursor& edit) {
        return zametti::setCodeLanguage(d_->text, edit, language);
    });
}

QStringList ZDocument::codeLanguagesNear(int blockNumber) const {
    return zametti::codeLanguagesNear(d_->text, blockNumber);
}

// --- СЛОЙ ОБЪЕКТОВ ----------------------------------------------------------

bool ZDocument::insertLineAfter(QTextCursor& at, int blockIndex) {
    return runLocalEdit(at, [this, blockIndex](QTextCursor& edit) {
        const QTextBlock block = d_->text.findBlockByNumber(blockIndex);
        if (!block.isValid()) return false;
        edit.setPosition(block.position() + block.length() - 1);
        edit.insertBlock(vspaceBlockFormat(false, false), QTextCharFormat());
        return true;
    });
}

bool ZDocument::removeBlocks(QTextCursor& at, int first, int last) {
    return runLocalEdit(at, [this, first, last](QTextCursor& edit) {
        const QTextBlock from = d_->text.findBlockByNumber(first);
        const QTextBlock to = d_->text.findBlockByNumber(last);
        if (!from.isValid() || !to.isValid()) return false;
        edit.setPosition(from.position());
        // Вместе с разделителем блока: иначе от объекта остаётся пустая строка,
        // которой в файле не было.
        const int end = qMin(to.position() + to.length(), d_->text.characterCount() - 1);
        edit.setPosition(end, QTextCursor::KeepAnchor);
        edit.removeSelectedText();
        return true;
    });
}

// --- ЗАМЕНА ПО ВСЕЙ ЗАМЕТКЕ -------------------------------------------------

int ZDocument::replaceAll(const QString& text, bool caseSensitive, const QString& with) {
    if (text.isEmpty()) return 0;
    QTextDocument::FindFlags flags;
    if (caseSensitive) flags |= QTextDocument::FindCaseSensitively;

    int replaced = 0;
    QTextCursor group(&d_->text);
    group.beginEditBlock();
    QTextCursor at(&d_->text);
    while (true) {
        at = d_->text.find(text, at, flags);
        if (at.isNull()) break;
        at.insertText(with);
        ++replaced;
    }
    if (replaced > 0) {
        // ШОВ ПО ВСЕЙ ЗАМЕТКЕ — здесь это законно: вхождения рассыпаны по ней
        // целиком, и дешевле шва на каждое одно на всё.
        settleSeam(0, d_->text.blockCount() - 1);
        rebuildRange(0, d_->text.blockCount() - 1);
    }
    group.endEditBlock();

#ifndef NDEBUG
    if (replaced > 0) checkCanonical();
#endif
    return replaced;
}

// --- СТОРОЖ СТРОЕНИЯ --------------------------------------------------------

QString ZDocument::structureProblem() const {
    QString problem;
    if (!listInvariantHolds(d_->text, &problem)) return QStringLiteral("списки: ") + problem;
    if (!literalInvariantHolds(d_->text, &problem))
        return QStringLiteral("продолжения: ") + problem;
    if (!gapInvariantHolds(d_->text, &problem))
        return QStringLiteral("пустые строки: ") + problem;
    return {};
}

// --- ФОРМУЛА ИЗ-ПОД КЛАВИАТУРЫ ----------------------------------------------

bool ZDocument::toggleInlineMath(QTextCursor& at) {
    return runLocalEdit(at, [this](QTextCursor& edit) {
        return zametti::toggleInlineMath(d_->text, edit);
    });
}

bool ZDocument::toggleDisplayMath(QTextCursor& at) {
    return runLocalEdit(at, [this](QTextCursor& edit) {
        return zametti::toggleDisplayMath(d_->text, edit);
    });
}

// --- ОТСТУП -----------------------------------------------------------------
//
// ЧТО ЗНАЧИТ Tab, РЕШАЕТ МЕСТО, и решает его заметка: снаружи спрашивать «а мы
// сейчас в коде?» некому — там про блоки кода знать не должны вовсе.

bool ZDocument::indent(QTextCursor& at) {
    return runLocalEdit(at, [this](QTextCursor& edit) {
        return indentCodeAtCursor(d_->text, edit) || indentListItems(d_->text, edit);
    });
}

bool ZDocument::outdent(QTextCursor& at) {
    return runLocalEdit(at, [this](QTextCursor& edit) {
        return outdentCodeAtCursor(d_->text, edit) || outdentListItems(d_->text, edit);
    });
}

}  // namespace zametti
