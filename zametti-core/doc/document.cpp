#include "document_impl.h"

#include "archive.h"
#include "doc_model.h"
#include "document_builder.h"
#include "document_pieces.h"
#include "note_header.h"
#include "serializer.h"
#include "sort_order.h"
#include "text_stats.h"
#include "times.h"

#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>

namespace zametti {

// ЖИВАЯ МОДЕЛЬ — QTextDocument, и только он. Шапка живёт рядом: в документе её
// нет и быть не должно, редактор её не видит.

namespace {

// СТРОЕНИЕ ЗАМЕТКИ ОДНОЙ СТРОКОЙ: род, уровень, маркер, отметка, язык и текст
// каждого блока. Разметка внутри строки НЕ входит нарочно — голую ссылку
// человек набирает текстом, а файл читает её ссылкой, и самопроверка записи
// обязана считать это одним и тем же.
//
// Строкой, а не отпечатком: на этом стоит последний рубеж против потери
// данных, и мириться там с вероятностью совпадения хешей нельзя.
QString skeletonOf(const QTextDocument& text) {
    QString out;
    walkPieces(text, [&](const Piece& piece) {
        if (piece.raw) {
            out += QLatin1String("raw\x1f");
            out += piece.text;
            out += QChar(0x1e);
            return true;
        }
        out += QString::number(int(piece.kind));
        out += QChar(0x1f);
        out += QString::number(piece.level);
        out += QChar(0x1f);
        out += QString::number(int(piece.marker));
        out += QChar(0x1f);
        out += piece.checked ? u'1' : u'0';
        out += QChar(0x1f);
        out += piece.info;
        out += QChar(0x1f);
        out += piece.text;
        out += QChar(0x1e);
        return true;
    });
    return out;
}

}  // namespace

ZDocument::ZDocument() : d_(std::make_shared<Data>()) {}

ZDocument ZDocument::clone() const {
    ZDocument out;
    // Содержимое переносим байтами: копировать QTextDocument иначе значило бы
    // тащить с собой стек отмены и состояние вёрстки, а слепку они не нужны.
    out.loadMarkdown(toMarkdown());
    return out;
}

// --- круг с диском ---------------------------------------------------------

bool ZDocument::loadMarkdown(std::string_view bytes, NoteHeader* lifted, std::vector<Piece>* built) {
    // НОРМАЛИЗАЦИЯ ПРОБЕЛОВ — ЧАСТЬ ВВОЗА, а не отдельный шаг: так делают все
    // нынешние места вызова, и без неё ZDocument читал бы не то же, что читает
    // программа.
    //
    // Промежуточного представления на этом пути больше нет: md4c собирает
    // логические блоки, сборщик кладёт их в живой документ. Черновик разбора
    // умирает вместе с вызовом.
    // ГРАНИЦА ФАЙЛА: байты становятся текстом ровно здесь, один раз. Дальше —
    // разбор, черновик, блоки, документ — всё в UTF-16, без единой конверсии.
    std::vector<Piece> blocks;
    NoteHeader header;
    parsePieces(normaliseSpaces(QString::fromUtf8(bytes.data(), qsizetype(bytes.size()))),
                blocks, header);
    if (lifted != nullptr) *lifted = std::move(header);
    buildDocument(blocks, d_->text);
    if (built != nullptr) *built = std::move(blocks);
    return true;
}


int ZDocument::revision() const { return d_->text.revision(); }

const ZDocStyle& ZDocument::style() const { return styleOf(d_->text); }

void ZDocument::setStyle(std::shared_ptr<const ZDocStyle> style) {
    attachStyle(d_->text, std::move(style));
}

std::shared_ptr<const ZDocStyle> ZDocument::stylePtr() const { return attachedStyle(d_->text); }


// --- о чём заметка ---------------------------------------------------------

namespace {

// СОДЕРЖАТЕЛЬНЫЙ ЛИ БЛОК. Правило было написано ТРИЖДЫ — в поиске по хранилищу,
// в дереве и в стабе архива; здесь оно одно.
//
// Пустая строка и html-комментарий не говорят ничего: первая пуста, второй —
// разметка, а не текст. Законченный дословный комментарий — тот же случай:
// заметка, начинающаяся с «<!-- набросок -->», называется своим заголовком, а
// не комментарием.
bool isMeaningful(const Piece& piece) {
    if (piece.raw) return !piece.isClosedHtmlComment();
    return piece.kind != Kind::VSpace && piece.kind != Kind::Html;
}

QString firstLineOf(const QString& text) {
    const qsizetype end = text.indexOf(QLatin1Char('\n'));
    return (end < 0 ? text : text.left(end)).simplified();
}

}  // namespace

// ЗАГОЛОВОК — ПЕРВАЯ СТРОКА ПЕРВОГО СОДЕРЖАТЕЛЬНОГО БЛОКА, и обход обрывается
// на нём. Цена вопроса названа в document_pieces.h: список заметок спрашивает
// заголовок у каждой, и платить за него размером самой большой нельзя.
QString ZDocument::title() const {
    QString out;
    walkPieces(d_->text, [&](const Piece& piece) {
        if (!isMeaningful(piece)) return true;
        const QString text = piece.text.simplified();
        if (text.isEmpty()) return true;
        out = firstLineOf(text).left(64);
        return false;
    });
    return out;
}

void ZDocument::setTitle(const QString& title) {
    // Собираем блоки заново: заголовок либо заменяет первый содержательный
    // блок, либо встаёт перед ним. Заметка при этом пересобирается целиком —
    // переименование случается по одному разу на действие человека, и платить
    // за него заплаткой незачем.
    std::vector<Piece> blocks;
    bool placed = false;
    walkPieces(d_->text, [&](const Piece& piece) {
        if (!placed && isMeaningful(piece)) {
            placed = true;
            Piece heading;
            heading.kind = Kind::Heading;
            heading.headingLevel =
                !piece.raw && piece.kind == Kind::Heading && piece.headingLevel > 0
                    ? piece.headingLevel
                    : 1;
            heading.text = title;
            // Заголовком был — заменяем его; не был — встаёт перед ним, и
            // между ними обязана стоять пустая строка (инвариант файла).
            const bool replace = !piece.raw && piece.kind == Kind::Heading;
            blocks.push_back(std::move(heading));
            if (!replace) {
                Piece gap;
                gap.kind = Kind::VSpace;
                blocks.push_back(std::move(gap));
                blocks.push_back(piece);
            }
            return true;
        }
        blocks.push_back(piece);
        return true;
    });
    if (!placed) {
        Piece heading;
        heading.kind = Kind::Heading;
        heading.headingLevel = 1;
        heading.text = title;
        blocks.insert(blocks.begin(), std::move(heading));
    }
    buildDocument(blocks, d_->text);
}

QString ZDocument::snippet(int limit) const {
    QString out;
    bool haveTitle = false;
    walkPieces(d_->text, [&](const Piece& piece) {
        if (!isMeaningful(piece)) return true;
        const QString text = piece.text.simplified();
        if (text.isEmpty()) return true;
        if (!haveTitle) {
            haveTitle = true;
            return true;
        }
        if (!out.isEmpty()) out += QLatin1Char(' ');
        out += text;
        return out.size() < limit;
    });
    if (out.size() > limit) out = out.left(limit - 1) + QChar(0x2026);
    return out;
}

// СТАБ АРХИВА: та же шапка с пометкой `archived` плюс одна строка — заголовок.
// Собирается заметкой-однодневкой и записывается общим писателем: второго
// способа получить байты заметки не бывает.
ZDocument ZDocument::headingOnly() const {
    // Заголовок ищем так же, как его видит средняя колонка: первый
    // содержательный блок. Не нашли — стаб остаётся без тела, и это законно:
    // заметка без единой строки текста и была пустой.
    Piece heading;
    walkPieces(d_->text, [&](const Piece& piece) {
        if (piece.raw) {
            // Дословный кусок заголовком не считаем; законченный комментарий
            // пропускаем — он и в дереве заголовком не выглядит.
            return piece.isClosedHtmlComment();
        }
        if (piece.kind == Kind::VSpace || piece.kind == Kind::Html) return true;
        // Первая строка: заголовок стаба однострочный, а блок может нести
        // мягкие переносы.
        QString line = piece.text.left(piece.text.indexOf(QLatin1Char('\n')));
        while (!line.isEmpty() && (line.back() == u' ' || line.back() == u'\r')) line.chop(1);
        if (line.isEmpty()) return true;
        heading.kind = Kind::Heading;
        heading.headingLevel =
            piece.kind == Kind::Heading && piece.headingLevel > 0 ? piece.headingLevel : 1;
        heading.text = std::move(line);
        return false;   // заголовок найден, дальше не идём
    });

    std::vector<Piece> body;
    if (!heading.text.isEmpty()) body.push_back(std::move(heading));
    ZDocument stub;
    attachStyle(stub.d_->text, attachedStyle(d_->text));
    buildDocument(body, stub.d_->text);
    return stub;
}

// ПУСТА ЛИ ЗАМЕТКА ПО СУЩЕСТВУ: ни одного блока, кроме пустых строк.
//
// Обходом логических блоков, а не блоков документа, и это не вкусовщина. У
// пустого QTextDocument один блок есть ВСЕГДА, свойств у него нет, и прямой
// проход считал такую заметку непустой — а от этого ответа зависит пустая
// строка после шапки, то есть побайтовый круг привезённого файла. Поймал набор
// ввоза («пустой файл импортируется»), и поймал только после того, как ответ
// стал спрашиваться у заметки, а не у блоков.
bool ZDocument::isEmpty() const {
    bool empty = true;
    walkPieces(d_->text, [&](const Piece& piece) {
        if (piece.raw || piece.kind != Kind::VSpace) {
            empty = false;
            return false;
        }
        return true;
    });
    return empty;
}

NoteStats ZDocument::getStats() const { return documentStats(d_->text); }

// --- блоки -----------------------------------------------------------------

int ZDocument::blockCount() const { return d_->text.blockCount(); }

BlockInfo ZDocument::blockAt(int index) const {
    BlockInfo out;
    const QTextBlock block = d_->text.findBlockByNumber(index);
    if (!block.isValid()) return out;
    out.index = index;
    out.kind = kindOf(block);
    const MarkerStyle style = markerOf(block);
    out.marker = style.marker;
    out.level = levelOf(block);
    out.headingLevel = block.blockFormat().headingLevel();
    out.checked = style.checked;
    out.raw = isRawBlock(block);
    out.continuation = isContinuationBlock(block);
    out.info = block.blockFormat().stringProperty(InfoProperty);
    out.text = block.text();
    return out;
}

std::vector<BlockInfo> ZDocument::blocks() const {
    std::vector<BlockInfo> out;
    out.reserve(size_t(d_->text.blockCount()));
    int index = 0;
    for (QTextBlock b = d_->text.begin(); b.isValid(); b = b.next(), ++index)
        out.push_back(blockAt(index));
    return out;
}


// --- вложения --------------------------------------------------------------

std::vector<Attachment> ZDocument::attachments() const {
    std::vector<Attachment> out;
    for (QTextBlock b = d_->text.begin(); b.isValid(); b = b.next()) {
        for (auto it = b.begin(); it != b.end(); ++it) {
            const QTextFragment fragment = it.fragment();
            if (!fragment.isValid()) continue;
            const QTextCharFormat format = fragment.charFormat();
            if ((format.intProperty(SpanStyleProperty) & SpanImage) == 0) continue;
            const QString href = format.anchorHref();
            if (href.isEmpty()) continue;

            Attachment a;
            // Атрибуты живут во фрагменте адреса: «id.webp#w=600&align=left».
            const qsizetype hash = href.indexOf(QLatin1Char('#'));
            a.id = hash < 0 ? href : href.left(hash);
            if (hash >= 0) {
                for (const QString& part : href.mid(hash + 1).split(QLatin1Char('&'))) {
                    if (part.startsWith(QLatin1String("w=")))
                        a.width = part.mid(2).toInt();
                    else if (part.startsWith(QLatin1String("align=")))
                        a.align = part.mid(6);
                }
            }
            // У объекта текст фрагмента — это U+FFFC; подпись живёт рядом,
            // в свойстве (см. ObjectAltProperty в doc_model.h).
            a.alt = format.hasProperty(ObjectAltProperty)
                        ? format.property(ObjectAltProperty).toString()
                        : fragment.text();
            out.push_back(std::move(a));
        }
    }
    return out;
}

int ZDocument::rewriteAttachments(const std::function<QString(const QString&)>& rename) {
    int changed = 0;
    QTextCursor cursor(&d_->text);
    cursor.beginEditBlock();
    for (QTextBlock b = d_->text.begin(); b.isValid(); b = b.next()) {
        for (auto it = b.begin(); it != b.end(); ++it) {
            const QTextFragment fragment = it.fragment();
            if (!fragment.isValid()) continue;
            QTextCharFormat format = fragment.charFormat();
            if ((format.intProperty(SpanStyleProperty) & SpanImage) == 0) continue;
            const QString href = format.anchorHref();
            if (href.isEmpty()) continue;

            const qsizetype hash = href.indexOf(QLatin1Char('#'));
            const QString name = hash < 0 ? href : href.left(hash);
            const QString fresh = rename(name);
            if (fresh.isEmpty() || fresh == name) continue;

            format.setAnchorHref(hash < 0 ? fresh : fresh + href.mid(hash));
            cursor.setPosition(fragment.position());
            cursor.setPosition(fragment.position() + fragment.length(),
                               QTextCursor::KeepAnchor);
            cursor.setCharFormat(format);
            ++changed;
        }
    }
    cursor.endEditBlock();
    return changed;
}

// --- поиск -----------------------------------------------------------------

// ПОИСК ИДЁТ ПО ЖИВОМУ ДОКУМЕНТУ, блок за блоком. Никаких копий содержимого:
// текст блока и так лежит готовым, доставать его вторично незачем.
std::vector<Hit> ZDocument::find(const Query& query) const {
    std::vector<Hit> hits;
    if (query.isEmpty()) return hits;
    int ordinal = 0;
    int index = 0;
    for (QTextBlock b = d_->text.begin(); b.isValid(); b = b.next(), ++index) {
        const QString text = b.text();
        if (text.isEmpty()) continue;
        qsizetype at = text.indexOf(query.needle, 0, query.sensitivity());
        while (at >= 0) {
            hits.push_back(Hit{index, int(at), int(query.needle.size()), ordinal++});
            // Со следующего знака, а не через длину запроса: перекрывающиеся
            // вхождения («аа» в «ааа») — тоже вхождения, и счётчик «3/17»
            // обязан считать их так же, как их потом обойдёт F3.
            at = text.indexOf(query.needle, at + 1, query.sensitivity());
        }
    }
    return hits;
}

HitLine ZDocument::hitLine(const Hit& hit, int radius) const {
    HitLine out;
    const QTextBlock block = d_->text.findBlockByNumber(hit.block);
    if (!block.isValid()) return out;
    const QString text = block.text();
    if (hit.offset < 0 || hit.offset > text.size()) return out;

    // Строка, в которой стоит совпадение: у блока их может быть несколько —
    // мягкие переносы внутри абзаца стоят разделителем строк, — а в списке
    // результатов нужна одна.
    constexpr QChar kBreak = QChar::LineSeparator;
    qsizetype from = text.lastIndexOf(kBreak, hit.offset > 0 ? hit.offset - 1 : 0);
    from = from < 0 ? 0 : from + 1;
    qsizetype to = text.indexOf(kBreak, hit.offset);
    if (to < 0) to = text.size();

    // Окно вокруг совпадения: длинную строку кода целиком в список не
    // вместить, а совпадение обязано быть видно.
    const qsizetype start = qMax(from, qsizetype(hit.offset) - radius);
    const qsizetype end = qMin(to, qsizetype(hit.offset + hit.length) + radius);
    QString line = text.mid(start, end - start);
    int offset = int(hit.offset - start);
    if (start > from) {
        line.prepend(QChar(0x2026));
        ++offset;
    }
    if (end < to) line.append(QChar(0x2026));

    out.text = line;
    out.offset = offset;
    out.length = hit.length;
    return out;
}

// --- показ -----------------------------------------------------------------

QTextDocument* ZDocument::getDocument() {
    // Вёрстку включаем здесь: пока заметку не показывают, считать строки и
    // глифы незачем, а с этой минуты — нужно.
    d_->text.setLayoutEnabled(true);
    return &d_->text;
}

// --- сравнение -------------------------------------------------------------

bool ZDocument::sameSkeleton(const ZDocument& other) const {
    return skeletonOf(d_->text) == skeletonOf(other.d_->text);
}

bool ZDocument::sameBody(const ZDocument& other) const {
    return toMarkdown() == other.toMarkdown();
}

}  // namespace zametti
