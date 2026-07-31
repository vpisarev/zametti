#include "note_tree.h"

#include "ir.h"
#include "note_id.h"
#include "parser.h"
#include "settings.h"

#include <QCollator>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QHash>
#include <QPainter>
#include <QPixmap>

#include <algorithm>

namespace zametti {

struct NoteTreeModel::Node {
    QString title;   // подпись: имя заметки без ".md" или имя каталога
    QString path;    // полный путь, и у файла, и у каталога
    QString id;      // id заметки в хранилище; вне хранилища пусто
    QString badge;   // пометка починки («сирота», «цикл») — подпись и подсказка
    QString snippet; // начало текста для средней колонки
    QString modified;   // ISO из меты — для сортировки свежие сверху
    QString effectiveModified;   // максимум по поддереву: живые каталоги вперёд
    bool trash = false; // корзина: в самом низу корня
    bool folder = false;   // role: folder — директория и без детей
    bool dir = false;
    bool storeRoot = false;   // «All notes»: корень хранилища отдельной строкой
    Node* parent = nullptr;
    std::vector<std::unique_ptr<Node>> children;
    // Кого из детей видно наружу. В режиме «только папки» заметки остаются в
    // children (средняя колонка берёт их оттуда), но в модель не попадают —
    // иначе пришлось бы держать два дерева и синхронизировать их.
    std::vector<Node*> shown;

    bool isDir() const { return dir; }
    int rowInParent() const {
        if (parent == nullptr) return 0;
        for (size_t i = 0; i < parent->shown.size(); ++i)
            if (parent->shown[i] == this) return int(i);
        return 0;
    }
};

namespace {

// Каталоги идут первыми, дальше по названию с учётом языка: "Ядро" не должно
// оказываться после "Zoo" только потому, что кириллица дальше в кодировке.
void sortChildren(std::vector<std::unique_ptr<NoteTreeModel::Node>>& children,
                  const QCollator& collator) {
    std::sort(children.begin(), children.end(),
              [&collator](const auto& a, const auto& b) {
                  if (a->isDir() != b->isDir()) return a->isDir();
                  return collator.compare(a->title, b->title) < 0;
              });
}

// Собирает поддерево каталога. Возвращает nullptr, если заметок внутри нет:
// показывать пустые ветки незачем.
std::unique_ptr<NoteTreeModel::Node> buildDir(const QString& dirPath, const QString& title,
                                              const QCollator& collator) {
    auto node = std::make_unique<NoteTreeModel::Node>();
    node->title = title;
    node->path = QFileInfo(dirPath).absoluteFilePath();
    node->dir = true;

    QDir dir(dirPath);
    const QFileInfoList entries =
        dir.entryInfoList(QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot);

    for (const QFileInfo& entry : entries) {
        // Служебные каталоги хранилища (.obsidian, .git) заметками не являются.
        if (entry.fileName().startsWith(QLatin1Char('.'))) continue;

        if (entry.isDir()) {
            auto child = buildDir(entry.absoluteFilePath(), entry.fileName(), collator);
            if (child != nullptr) {
                child->parent = node.get();
                node->children.push_back(std::move(child));
            }
        } else if (entry.suffix().compare(QLatin1String("md"), Qt::CaseInsensitive) == 0) {
            auto child = std::make_unique<NoteTreeModel::Node>();
            child->title = entry.completeBaseName();
            child->path = entry.absoluteFilePath();
            child->parent = node.get();
            node->children.push_back(std::move(child));
        }
    }

    if (node->children.empty()) return nullptr;
    sortChildren(node->children, collator);
    return node;
}

// --- плоское хранилище ------------------------------------------------------
//
// Дерево из метаданных: скан всех "<id>.md", у каждого — parent, modified,
// заголовок и начало текста. Читается и разбирается ядром весь файл: сниппет
// средней колонки — это текст блоков IR, а не первые байты файла, и получить
// его из головы нельзя. Замер этапа 4: полный проход с разбором — 7.2 мс на
// 271 заметке против 1.3 мс у чтения голов по 4096 байт; разница ниже порога
// заметности, а колонка получает то, что показано человеку.
//
// Починка структуры живёт в памяти и только в ней: parent в никуда — заметка
// в корне с пометкой «сирота»; цикл родителей рвётся, виновник в корень с
// пометкой «цикл». Загрузчик НИКОГДА не пишет в файлы.
struct StoreNote {
    QString id;
    QString parent;
    QString title;
    QString snippet;
    QString modified;
    bool trash = false;
    bool folder = false;
};

// Сколько знаков сниппета держим. Две-три строки списка при любой разумной
// ширине панели; резать точно по строкам нельзя — ширина известна только
// делегату, и она меняется вместе с разделителем.
constexpr int kSnippetChars = 200;

// Текст блока так, как его видит человек: у дословных кусков — сам кусок, он
// и показан.
QString blockPlainText(const Document& doc, const Block& block) {
    const std::string_view text = doc.text(block);
    return QString::fromUtf8(text.data(), qsizetype(text.size()));
}

// Первая строка: заголовок в списке однострочный, а текст блока может нести
// мягкие переносы.
QString firstLine(const QString& text) {
    const qsizetype eol = text.indexOf(QLatin1Char('\n'));
    return (eol < 0 ? text : text.left(eol)).trimmed();
}

// Заголовок — первый содержательный блок; сниппет — то, что идёт за ним.
// Пустые строки и HTML-комментарии в сниппет не берутся: первые ничего не
// говорят, вторые — разметка, а не текст (шапка метаданных блоком и не
// является — она в doc.meta).
void describeNote(const Document& doc, StoreNote& out) {
    bool haveTitle = false;
    QString snippet;
    for (const Block& block : doc.blocks) {
        if (!block.raw && (block.kind == Kind::VSpace || block.kind == Kind::Html)) continue;
        if (block.raw && doc.isClosedHtmlComment(block)) continue;
        const QString text = blockPlainText(doc, block).simplified();
        if (text.isEmpty()) continue;
        if (!haveTitle) {
            out.title = firstLine(text).left(64);
            haveTitle = true;
            continue;
        }
        if (!snippet.isEmpty()) snippet += QLatin1Char(' ');
        snippet += text;
        if (snippet.size() >= kSnippetChars) break;
    }
    if (snippet.size() > kSnippetChars)
        snippet = snippet.left(kSnippetChars - 1) + QChar(0x2026);
    out.snippet = snippet;
    if (out.title.isEmpty()) out.title = QStringLiteral("Без названия");
}

bool readStoreNote(const QString& path, StoreNote& out) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    const QByteArray bytes = f.readAll();
    const Document doc = parse(std::string_view(bytes.constData(), size_t(bytes.size())));

    out.parent = QString::fromStdString(doc.meta.get("parent"));
    out.modified = QString::fromStdString(doc.meta.get("modified"));
    const std::string role = doc.meta.get("role");
    out.trash = role == "trash";
    out.folder = role == "folder";
    describeNote(doc, out);
    return true;
}

// Подпись корневой строки левой панели: имя хранилища из конфига, а нет его —
// «All notes» (бриф этапа 4). Имя каталога сюда не подставляется намеренно:
// оно техническое (у владельца это "vpnotes"), а строка означает не каталог, а
// «все заметки хранилища». Выбор этой строки и означает ровно это.
QString storeRootTitle(const QString&) {
    const QString configured = appearance().storeTitle;
    return configured.isEmpty() ? QStringLiteral("All notes") : configured;
}

std::unique_ptr<NoteTreeModel::Node> buildStore(const QString& rootPath) {
    // Два корня: невидимый (им отвечает QModelIndex()) и видимый — строка
    // «All notes», которая в левой панели всегда первая и всегда на месте.
    // Держать её узлом, а не рисовать отдельно, дешевле всего: перенос в
    // корень, раскрытие и выделение работают тем же кодом, что и у папок.
    auto hidden = std::make_unique<NoteTreeModel::Node>();
    hidden->path = QFileInfo(rootPath).absoluteFilePath();
    hidden->dir = true;

    auto rootOwned = std::make_unique<NoteTreeModel::Node>();
    rootOwned->title = storeRootTitle(rootPath);
    rootOwned->path = QFileInfo(rootPath).absoluteFilePath();
    rootOwned->dir = true;
    rootOwned->storeRoot = true;
    rootOwned->parent = hidden.get();
    NoteTreeModel::Node* root = rootOwned.get();
    hidden->children.push_back(std::move(rootOwned));

    // Скан: только "<id>.md".
    std::vector<std::unique_ptr<NoteTreeModel::Node>> nodes;
    QHash<QString, NoteTreeModel::Node*> byId;
    QHash<QString, QString> parentOf;
    for (const QFileInfo& info :
         QDir(rootPath).entryInfoList({QStringLiteral("*.md")}, QDir::Files)) {
        const QString stem = info.completeBaseName();
        if (!isValidNoteId(stem.toStdString())) continue;
        StoreNote meta;
        if (!readStoreNote(info.absoluteFilePath(), meta)) continue;
        auto node = std::make_unique<NoteTreeModel::Node>();
        node->id = stem;
        node->title = meta.title;
        node->snippet = meta.snippet;
        node->path = info.absoluteFilePath();
        node->modified =
            meta.modified.isEmpty()
                ? info.lastModified().toUTC().toString(Qt::ISODate)
                : meta.modified;
        node->trash = meta.trash;
        node->folder = meta.folder;
        byId.insert(stem, node.get());
        parentOf.insert(stem, meta.parent);
        nodes.push_back(std::move(node));
    }

    // Циклы: подъём по родителям с пометкой пройденного. Виновник — первый,
    // чей подъём вернулся в него самого.
    for (auto& node : nodes) {
        QSet<QString> seen{node->id};
        QString at = parentOf.value(node->id);
        while (!at.isEmpty() && byId.contains(at)) {
            if (seen.contains(at)) {
                if (at == node->id) {
                    node->badge = QStringLiteral("цикл");
                    parentOf[node->id] = QString();
                }
                break;
            }
            seen.insert(at);
            at = parentOf.value(at);
        }
    }

    // Подвес: parent в никуда — сирота в корне с пометкой.
    for (auto& node : nodes) {
        const QString parent = parentOf.value(node->id);
        NoteTreeModel::Node* home = root;
        if (!parent.isEmpty()) {
            if (byId.contains(parent)) home = byId.value(parent);
            else node->badge = QStringLiteral("сирота");
        }
        node->parent = home;
        home->children.push_back(std::move(node));
    }

    // Директория — по явному role: folder, по детям (на неё ссылаются как на
    // родителя) или корзина. Директория не открывается — это чисто структура.
    // Эффективный modified — максимум по поддереву: каталог, где правили
    // позже всех, всплывает вперёд.
    struct Finish {
        static QString run(NoteTreeModel::Node* node) {
            node->dir = node->dir || node->folder || node->trash ||
                        !node->children.empty();
            node->effectiveModified = node->modified;
            for (auto& child : node->children) {
                const QString sub = run(child.get());
                if (sub > node->effectiveModified) node->effectiveModified = sub;
            }
            return node->effectiveModified;
        }
    };
    for (auto& child : root->children) Finish::run(child.get());
    root->effectiveModified.clear();
    for (auto& child : root->children)
        if (child->effectiveModified > root->effectiveModified)
            root->effectiveModified = child->effectiveModified;
    return hidden;
}

// Братья: корзина всегда внизу; дальше по режиму.
void sortStore(NoteTreeModel::Node* node, NoteTreeModel::SortMode mode,
               const QCollator& collator) {
    std::sort(node->children.begin(), node->children.end(),
              [mode, &collator](const auto& a, const auto& b) {
                  if (a->trash != b->trash) return b->trash;
                  if (mode == NoteTreeModel::SortMode::ByName) {
                      if (a->dir != b->dir) return a->dir;
                      return collator.compare(a->title, b->title) < 0;
                  }
                  return a->effectiveModified > b->effectiveModified;
              });
    for (auto& child : node->children) sortStore(child.get(), mode, collator);
}

// Кого показывать наружу. Пересчитывается после каждой сортировки и после
// смены режима: порядок shown обязан совпадать с порядком children, иначе
// строки и узлы разъедутся.
void rebuildShown(NoteTreeModel::Node* node, bool foldersOnly) {
    node->shown.clear();
    for (auto& child : node->children) {
        if (!foldersOnly || child->isDir()) node->shown.push_back(child.get());
        rebuildShown(child.get(), foldersOnly);
    }
}

// Значок папки рисуется знаком из шрифта: готовых чёрно-белых иконок в Qt нет,
// а эмодзи-шрифты дают цветные. Результат кэшируется — иначе он перерисовывался
// бы на каждую отрисовку строки.
// Запасная пара на случай, если настроенной гарнитуры в системе нет: эти знаки
// есть в Noto Sans Symbols2, который ставится вместе с дистрибутивом.
constexpr char16_t kFallbackFamily[] = u"Noto Sans Symbols2";
constexpr char32_t kFallbackClosed = U'\U0001F5C0';
constexpr char32_t kFallbackOpen = U'\U0001F5C1';

QPixmap folderPixmap(bool open) {
    const Appearance& a = appearance();
    const qreal dpr = qGuiApp != nullptr ? qGuiApp->devicePixelRatio() : 1.0;
    const QString key = QStringLiteral("%1|%2|%3").arg(int(open)).arg(a.sidebarFontPoint).arg(dpr);

    static QHash<QString, QPixmap> cache;
    const auto found = cache.constFind(key);
    if (found != cache.constEnd()) return *found;

    QString family = a.sidebarFolderFamily;
    QString glyph = open ? a.sidebarFolderOpen : a.sidebarFolderClosed;
    if (!QFontDatabase::families().contains(family)) {
        family = QString::fromUtf16(kFallbackFamily);
        glyph = QString::fromUcs4(open ? &kFallbackOpen : &kFallbackClosed, 1);
    }

    QFont font(family);
    font.setPointSizeF(a.sidebarFontPoint * a.sidebarFolderScale);
    const QFontMetrics metrics(font);
    const int side = metrics.height();

    QPixmap pixmap(QSize(side, side) * dpr);
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setFont(font);
    painter.setPen(a.sidebarFolderColor);
    painter.drawText(QRect(0, 0, side, side), Qt::AlignCenter, glyph);
    painter.end();

    cache.insert(key, pixmap);
    return pixmap;
}

const NoteTreeModel::Node* nodeOf(const QModelIndex& index, const NoteTreeModel::Node* root) {
    return index.isValid() ? static_cast<const NoteTreeModel::Node*>(index.internalPointer())
                           : root;
}

}  // namespace

NoteTreeModel::NoteTreeModel(const QString& root, QObject* parent)
    : QAbstractItemModel(parent), rootPath_(root), store_(isStoreRoot(root)) {
    build();
}

bool NoteTreeModel::isStoreRoot(const QString& dir) {
    return QFileInfo(dir + QStringLiteral("/.zametti")).isDir();
}

void NoteTreeModel::build() {
    QCollator collator;
    collator.setNumericMode(true);
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    if (store_) {
        root_ = buildStore(rootPath_);
        sortStore(root_.get(), sortMode_, collator);
        rebuildShown(root_.get(), foldersOnly_);
        return;
    }
    root_ = buildDir(rootPath_, QFileInfo(rootPath_).fileName(), collator);
    if (root_ == nullptr) {
        root_ = std::make_unique<Node>();
        root_->title = QFileInfo(rootPath_).fileName();
        root_->path = QFileInfo(rootPath_).absoluteFilePath();
        root_->dir = true;
    }
    rebuildShown(root_.get(), false);
}

void NoteTreeModel::setFoldersOnly(bool on) {
    if (on == foldersOnly_) return;
    foldersOnly_ = on;
    beginResetModel();
    rebuildShown(root_.get(), foldersOnly_);
    endResetModel();
}

void NoteTreeModel::setSortMode(SortMode mode) {
    if (mode == sortMode_) return;
    sortMode_ = mode;
    refresh();
}

void NoteTreeModel::refresh() {
    beginResetModel();
    build();
    endResetModel();
}

QString NoteTreeModel::idOf(const QModelIndex& index) const {
    if (!index.isValid()) return {};
    return static_cast<const Node*>(index.internalPointer())->id;
}

void NoteTreeModel::updateTitle(const QString& filePath, const QString& title) {
    if (title.isEmpty()) return;
    // По всему дереву, а не по видимой части: заголовок правится у заметки, а
    // заметок в левой панели теперь нет — строку ждёт средняя колонка.
    struct Find {
        static Node* run(Node* node, const QString& path) {
            for (auto& child : node->children) {
                if (child->path == path) return child.get();
                Node* found = run(child.get(), path);
                if (found != nullptr) return found;
            }
            return nullptr;
        }
    };
    Node* node = Find::run(root_.get(), filePath);
    if (node == nullptr || node->title == title) return;
    node->title = title;
    const QModelIndex index = indexForNode(node);
    if (index.isValid()) emit dataChanged(index, index, {Qt::DisplayRole});
    emit noteRowChanged(node->id);
}

NoteTreeModel::~NoteTreeModel() = default;

QModelIndex NoteTreeModel::index(int row, int column, const QModelIndex& parent) const {
    if (!hasIndex(row, column, parent)) return {};
    const Node* parentNode = nodeOf(parent, root_.get());
    return createIndex(row, column, parentNode->shown[size_t(row)]);
}

QModelIndex NoteTreeModel::parent(const QModelIndex& child) const {
    if (!child.isValid()) return {};
    const Node* node = static_cast<const Node*>(child.internalPointer());
    Node* parentNode = node->parent;
    if (parentNode == nullptr || parentNode == root_.get()) return {};
    return createIndex(parentNode->rowInParent(), 0, parentNode);
}

int NoteTreeModel::rowCount(const QModelIndex& parent) const {
    if (parent.column() > 0) return 0;
    return int(nodeOf(parent, root_.get())->shown.size());
}

int NoteTreeModel::columnCount(const QModelIndex&) const { return 1; }

QVariant NoteTreeModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid()) return {};
    const Node* node = static_cast<const Node*>(index.internalPointer());
    if (role == Qt::DisplayRole)
        return node->badge.isEmpty()
                   ? node->title
                   : node->title + QStringLiteral(" [") + node->badge + QLatin1Char(']');
    if (role == Qt::ToolTipRole && !node->isDir()) {
        if (node->badge == QStringLiteral("сирота"))
            return QStringLiteral("родитель не найден — показана в корне: ") + node->path;
        if (node->badge == QStringLiteral("цикл"))
            return QStringLiteral("цикл родителей разорван: ") + node->path;
        return node->path;
    }
    if (role == Qt::DecorationRole && node->isDir())
        return folderPixmap(expanded_.contains(node->path));
    return {};
}

QString NoteTreeModel::filePath(const QModelIndex& index) const {
    if (!index.isValid()) return {};
    const Node* node = static_cast<const Node*>(index.internalPointer());
    // Директория не открывается — это чисто структура (решение владельца).
    return node->isDir() ? QString() : node->path;
}

QString NoteTreeModel::nodePath(const QModelIndex& index) const {
    if (!index.isValid()) return root_->path;
    return static_cast<const Node*>(index.internalPointer())->path;
}

bool NoteTreeModel::isDirectory(const QModelIndex& index) const {
    if (!index.isValid()) return true;
    return static_cast<const Node*>(index.internalPointer())->isDir();
}

QModelIndex NoteTreeModel::indexForPath(const QString& path) const {
    // Обходим дерево целиком: заметок сотни, искать быстрее, чем держать
    // отдельный указатель на каждую.
    struct Search {
        static QModelIndex run(const NoteTreeModel* model, const QModelIndex& parent,
                               const QString& path) {
            const int rows = model->rowCount(parent);
            for (int i = 0; i < rows; ++i) {
                const QModelIndex child = model->index(i, 0, parent);
                if (model->nodePath(child) == path) return child;
                const QModelIndex found = run(model, child, path);
                if (found.isValid()) return found;
            }
            return {};
        }
    };
    return Search::run(this, QModelIndex(), path);
}

void NoteTreeModel::setExpanded(const QModelIndex& index, bool expanded) {
    const QString path = nodePath(index);
    if (path.isEmpty()) return;
    if (expanded) expanded_.insert(path);
    else expanded_.remove(path);
    if (index.isValid()) emit dataChanged(index, index, {Qt::DecorationRole});
}

bool NoteTreeModel::isEmpty() const { return topNode()->children.empty(); }

// Узел, под которым лежит содержимое: в хранилище это видимая строка
// «All notes», вне его — сам корень.
const NoteTreeModel::Node* NoteTreeModel::topNode() const {
    if (store_ && !root_->children.empty()) return root_->children.front().get();
    return root_.get();
}

std::vector<NoteRow> NoteTreeModel::notesInSubtree(const QModelIndex& index) const {
    std::vector<NoteRow> out;
    if (!store_) return out;
    const Node* node = index.isValid() ? static_cast<const Node*>(index.internalPointer())
                                       : topNode();
    // Корзина в общий список не попадает: выброшенное не должно всплывать
    // рядом с живым (так же ведёт себя Apple Notes). Внутри самой корзины —
    // наоборот, показываем всё её содержимое.
    const bool insideTrash = [&] {
        for (const Node* up = node; up != nullptr; up = up->parent)
            if (up->trash) return true;
        return false;
    }();
    struct Walk {
        static void run(const Node* node, bool insideTrash, std::vector<NoteRow>& out) {
            for (const auto& child : node->children) {
                if (child->trash && !insideTrash) continue;
                // Папка — структура, а не заметка: её тело открывается через
                // «Открыть как заметку», в списке ей делать нечего.
                if (!child->isDir())
                    out.push_back(NoteRow{child->id, child->path, child->title,
                                          child->snippet, child->modified});
                run(child.get(), insideTrash, out);
            }
        }
    };
    Walk::run(node, insideTrash, out);
    return out;
}

QModelIndex NoteTreeModel::folderIndexForNote(const QString& noteId) const {
    if (!store_ || noteId.isEmpty()) return {};
    struct Find {
        static const Node* run(const Node* node, const QString& id) {
            for (const auto& child : node->children) {
                if (child->id == id) return node;
                const Node* found = run(child.get(), id);
                if (found != nullptr) return found;
            }
            return nullptr;
        }
    };
    const Node* folder = Find::run(topNode(), noteId);
    if (folder == nullptr) return {};
    return indexForNode(folder);
}

QModelIndex NoteTreeModel::indexForNode(const Node* node) const {
    if (node == nullptr || node == root_.get()) return {};
    return createIndex(node->rowInParent(), 0, const_cast<Node*>(node));
}

const NoteTreeModel::Node* NoteTreeModel::nodeById(const QString& id) const {
    if (id.isEmpty()) return nullptr;
    struct Find {
        static const Node* run(const Node* node, const QString& id) {
            for (const auto& child : node->children) {
                if (child->id == id) return child.get();
                const Node* found = run(child.get(), id);
                if (found != nullptr) return found;
            }
            return nullptr;
        }
    };
    return Find::run(root_.get(), id);
}

bool NoteTreeModel::hasNote(const QString& id) const { return nodeById(id) != nullptr; }

bool NoteTreeModel::isFolderId(const QString& id) const {
    const Node* node = nodeById(id);
    return node != nullptr && node->isDir();
}

bool NoteTreeModel::inTrashId(const QString& id) const {
    for (const Node* node = nodeById(id); node != nullptr; node = node->parent)
        if (node->trash) return true;
    return false;
}

QString NoteTreeModel::parentIdOf(const QString& id) const {
    const Node* node = nodeById(id);
    if (node == nullptr || node->parent == nullptr) return {};
    return node->parent->id;
}

QString NoteTreeModel::titleOfId(const QString& id) const {
    const Node* node = nodeById(id);
    return node == nullptr ? QString() : node->title;
}

QStringList NoteTreeModel::ancestorTitles(const QString& id) const {
    QStringList out;
    const Node* node = nodeById(id);
    if (node == nullptr) return out;
    for (const Node* up = node->parent; up != nullptr && !up->storeRoot; up = up->parent)
        out.prepend(up->title);
    return out;
}

int NoteTreeModel::childCountOf(const QString& id) const {
    const Node* node = nodeById(id);
    return node == nullptr ? 0 : int(node->children.size());
}

QString NoteTreeModel::pathOfId(const QString& id) const {
    const Node* node = nodeById(id);
    return node == nullptr ? QString() : node->path;
}

QString NoteTreeModel::childFolderByTitle(const QString& parentId,
                                          const QString& title) const {
    const Node* parent = parentId.isEmpty() ? topNode() : nodeById(parentId);
    if (parent == nullptr) return {};
    for (const auto& child : parent->children) {
        if (!child->isDir() || child->trash) continue;
        if (child->title == title) return child->id;
    }
    return {};
}

QString NoteTreeModel::firstNoteId() const {
    struct Walk {
        static QString run(const Node* node) {
            for (const auto& child : node->children) {
                if (!child->isDir()) return child->id;
                const QString inside = run(child.get());
                if (!inside.isEmpty()) return inside;
            }
            return {};
        }
    };
    return Walk::run(topNode());
}

QString NoteTreeModel::neighbourOf(const QString& id) const {
    const Node* node = nodeById(id);
    if (node == nullptr || node->parent == nullptr) return {};
    const auto& siblings = node->parent->children;
    size_t at = siblings.size();
    for (size_t i = 0; i < siblings.size(); ++i)
        if (siblings[i].get() == node) at = i;
    if (at == siblings.size()) return {};
    for (size_t i = at + 1; i < siblings.size(); ++i)
        if (!siblings[i]->isDir()) return siblings[i]->id;
    for (size_t i = at; i-- > 0;)
        if (!siblings[i]->isDir()) return siblings[i]->id;
    return {};
}

void NoteTreeModel::refreshNote(const QString& path) {
    if (!store_) return;
    // Ищем по всему дереву, а не через indexForPath: заметку в режиме «только
    // папки» модель наружу не показывает, а обновить её строку надо.
    struct Find {
        static Node* run(Node* node, const QString& path) {
            for (auto& child : node->children) {
                if (child->path == path) return child.get();
                Node* found = run(child.get(), path);
                if (found != nullptr) return found;
            }
            return nullptr;
        }
    };
    Node* node = Find::run(root_.get(), path);
    if (node == nullptr) return;
    StoreNote fresh;
    if (!readStoreNote(path, fresh)) return;
    if (node->title == fresh.title && node->snippet == fresh.snippet &&
        node->modified == fresh.modified)
        return;
    node->title = fresh.title;
    node->snippet = fresh.snippet;
    if (!fresh.modified.isEmpty()) node->modified = fresh.modified;
    const QModelIndex index = indexForNode(node);
    if (index.isValid()) emit dataChanged(index, index, {Qt::DisplayRole});
    emit noteRowChanged(node->id);
}

NoteRow NoteTreeModel::rowOf(const QString& id) const {
    const Node* node = nodeById(id);
    if (node == nullptr) return {};
    return NoteRow{node->id, node->path, node->title, node->snippet, node->modified};
}

QString NoteTreeModel::folderIdFor(const QModelIndex& index) const {
    const Node* node =
        index.isValid() ? static_cast<const Node*>(index.internalPointer()) : nullptr;
    while (node != nullptr && node != root_.get()) {
        if (node->isDir()) return node->id;
        node = node->parent;
    }
    return {};
}

QString NoteTreeModel::titleOf(const QModelIndex& index) const {
    if (!index.isValid()) return {};
    return static_cast<const Node*>(index.internalPointer())->title;
}

QString NoteTreeModel::trashId() const {
    for (const auto& child : topNode()->children)
        if (child->trash) return child->id;
    return {};
}

bool NoteTreeModel::inTrash(const QModelIndex& index) const {
    for (const Node* node = index.isValid()
                                ? static_cast<const Node*>(index.internalPointer())
                                : nullptr;
         node != nullptr; node = node->parent)
        if (node->trash) return true;
    return false;
}

bool NoteTreeModel::isDescendantOf(const QString& candidateId, const QString& id) const {
    if (id.isEmpty() || candidateId.isEmpty()) return false;
    struct Find {
        static const Node* run(const Node* node, const QString& id) {
            if (node->id == id) return node;
            for (const auto& child : node->children) {
                const Node* found = run(child.get(), id);
                if (found != nullptr) return found;
            }
            return nullptr;
        }
    };
    const Node* top = Find::run(root_.get(), id);
    if (top == nullptr) return false;
    return Find::run(top, candidateId) != nullptr;
}

Qt::ItemFlags NoteTreeModel::flags(const QModelIndex& index) const {
    Qt::ItemFlags out = QAbstractItemModel::flags(index);
    if (!store_) return out;
    if (index.isValid()) out |= Qt::ItemIsEditable | Qt::ItemIsDragEnabled;
    out |= Qt::ItemIsDropEnabled;   // и корень: перенос «в корень» легален
    return out;
}

bool NoteTreeModel::setData(const QModelIndex& index, const QVariant& value, int role) {
    if (!store_ || !index.isValid() || role != Qt::EditRole) return false;
    const QString title = value.toString().trimmed();
    if (title.isEmpty()) return false;
    emit renameRequested(static_cast<const Node*>(index.internalPointer())->path, title);
    return true;
}

QStringList NoteTreeModel::mimeTypes() const {
    return {QStringLiteral("application/x-zametti-note-id")};
}

QMimeData* NoteTreeModel::mimeData(const QModelIndexList& indexes) const {
    if (indexes.isEmpty()) return nullptr;
    auto* data = new QMimeData;
    data->setData(QStringLiteral("application/x-zametti-note-id"),
                  idOf(indexes.first()).toUtf8());
    return data;
}

bool NoteTreeModel::canDropMimeData(const QMimeData* data, Qt::DropAction, int, int,
                                    const QModelIndex& parent) const {
    if (!store_ || data == nullptr) return false;
    const QString id =
        QString::fromUtf8(data->data(QStringLiteral("application/x-zametti-note-id")));
    if (id.isEmpty()) return false;
    // Сбрасывать можно только в папку или в корень: заметка папкой не
    // становится никогда (правило владельца).
    if (parent.isValid() &&
        !static_cast<const Node*>(parent.internalPointer())->isDir())
        return false;
    const QString target = idOf(parent);
    if (target == id) return false;
    // В собственное поддерево нельзя: папка стала бы своим же предком.
    return !isDescendantOf(target, id);
}

bool NoteTreeModel::dropMimeData(const QMimeData* data, Qt::DropAction action, int row,
                                 int column, const QModelIndex& parent) {
    if (!canDropMimeData(data, action, row, column, parent)) return false;
    const QString id =
        QString::fromUtf8(data->data(QStringLiteral("application/x-zametti-note-id")));
    emit moveRequested(id, idOf(parent));
    return true;
}

Qt::DropActions NoteTreeModel::supportedDropActions() const { return Qt::MoveAction; }

QString NoteTreeModel::rootFor(const QString& filePath, const QString& configuredRoot) {
    if (!configuredRoot.isEmpty()) {
        QFileInfo info(QDir::home().filePath(configuredRoot));
        if (info.isDir()) return info.absoluteFilePath();
    }

    QDir dir = QFileInfo(filePath).absoluteDir();
    const QString home = QDir::homePath();

    // Идём вверх, пока путь остаётся внутри домашнего каталога, и запоминаем
    // самый верхний каталог с меткой хранилища. Так заметка, открытая из
    // глубины, всё равно показывает дерево целиком.
    QString best = dir.absolutePath();
    QDir probe = dir;
    while (probe.absolutePath().startsWith(home) && probe.absolutePath() != home) {
        if (probe.exists(QStringLiteral(".zametti")) ||
            probe.exists(QStringLiteral(".obsidian")) || probe.exists(QStringLiteral(".git")))
            best = probe.absolutePath();
        if (!probe.cdUp()) break;
    }
    return best;
}

NoteTreeView::NoteTreeView(QWidget* parent) : QTreeView(parent) {
    // Раз треугольников нет, папка должна раскрываться по обычному щелчку:
    // иначе цели для нажатия не остаётся вовсе.
    connect(this, &QTreeView::clicked, this, [this](const QModelIndex& index) {
        if (model() != nullptr && model()->hasChildren(index))
            setExpanded(index, !isExpanded(index));
    });
}

void NoteTreeView::drawBranches(QPainter*, const QRect&, const QModelIndex&) const {}

QSize NoteTreeDelegate::sizeHint(const QStyleOptionViewItem& option,
                                 const QModelIndex& index) const {
    QSize size = QStyledItemDelegate::sizeHint(option, index);
    const qreal height =
        QFontMetricsF(option.font).height() * appearance().sidebarLineHeightFactor;
    size.setHeight(int(height + 0.5));
    return size;
}

}  // namespace zametti
