#include "note_tree.h"

#include "note_id.h"
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
    QString modified;   // ISO из меты — для сортировки свежие сверху
    bool trash = false; // корзина: в самом низу корня
    bool dir = false;
    Node* parent = nullptr;
    std::vector<std::unique_ptr<Node>> children;

    bool isDir() const { return dir; }
    int rowInParent() const {
        if (parent == nullptr) return 0;
        for (size_t i = 0; i < parent->children.size(); ++i)
            if (parent->children[i].get() == this) return int(i);
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
// Дерево из метаданных: скан всех "<id>.md", у каждого — parent, modified и
// заголовок. Заголовок — первая содержательная строка после шапки метаданных:
// строка "#..." без решёток, иначе просто строка, усечённая; совсем пусто —
// «Без названия». Файлы канонические (заголовок — первый блок), полный разбор
// ядром здесь не нужен.
//
// Починка структуры живёт в памяти и только в ней: parent в никуда — заметка
// в корне с пометкой «сирота»; цикл родителей рвётся, виновник в корень с
// пометкой «цикл». Загрузчик НИКОГДА не пишет в файлы.
struct StoreNote {
    QString id;
    QString parent;
    QString title;
    QString modified;
    bool trash = false;
};

bool readStoreNote(const QString& path, StoreNote& out) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    // Головы файла достаточно: шапка и заголовок живут в первых строках.
    const QString head = QString::fromUtf8(f.read(4096));
    const QStringList lines = head.split(QLatin1Char('\n'));
    int i = 0;
    if (i < lines.size() && lines[i] == QStringLiteral("<!-- zametti")) {
        for (++i; i < lines.size() && lines[i] != QStringLiteral("-->"); ++i) {
            const qsizetype colon = lines[i].indexOf(QLatin1Char(':'));
            if (colon <= 0) continue;
            const QString key = lines[i].left(colon).trimmed();
            const QString value = lines[i].mid(colon + 1).trimmed();
            if (key == QStringLiteral("parent")) out.parent = value;
            else if (key == QStringLiteral("modified")) out.modified = value;
            else if (key == QStringLiteral("role") && value == QStringLiteral("trash"))
                out.trash = true;
        }
        if (i < lines.size()) ++i;   // сама "-->"
    }
    for (; i < lines.size(); ++i) {
        const QString line = lines[i].trimmed();
        if (line.isEmpty()) continue;
        if (line.startsWith(QLatin1Char('#'))) {
            qsizetype at = 0;
            while (at < line.size() && line[at] == QLatin1Char('#')) ++at;
            out.title = line.mid(at).trimmed();
        } else {
            out.title = line.left(64);
        }
        break;
    }
    if (out.title.isEmpty()) out.title = QStringLiteral("Без названия");
    return true;
}

std::unique_ptr<NoteTreeModel::Node> buildStore(const QString& rootPath) {
    auto root = std::make_unique<NoteTreeModel::Node>();
    root->title = QFileInfo(rootPath).fileName();
    root->path = QFileInfo(rootPath).absoluteFilePath();
    root->dir = true;

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
        node->path = info.absoluteFilePath();
        node->modified =
            meta.modified.isEmpty()
                ? info.lastModified().toUTC().toString(Qt::ISODate)
                : meta.modified;
        node->trash = meta.trash;
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
        NoteTreeModel::Node* home = root.get();
        if (!parent.isEmpty()) {
            if (byId.contains(parent)) home = byId.value(parent);
            else node->badge = QStringLiteral("сирота");
        }
        node->parent = home;
        home->children.push_back(std::move(node));
    }

    // Свежие сверху; корзина — в самом низу корня.
    struct Sorter {
        static void run(NoteTreeModel::Node* node) {
            std::sort(node->children.begin(), node->children.end(),
                      [](const auto& a, const auto& b) {
                          if (a->trash != b->trash) return b->trash;
                          return a->modified > b->modified;
                      });
            for (auto& child : node->children) run(child.get());
        }
    };
    Sorter::run(root.get());
    return root;
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
    if (store_) {
        root_ = buildStore(rootPath_);
        return;
    }
    QCollator collator;
    collator.setNumericMode(true);
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    root_ = buildDir(rootPath_, QFileInfo(rootPath_).fileName(), collator);
    if (root_ == nullptr) {
        root_ = std::make_unique<Node>();
        root_->title = QFileInfo(rootPath_).fileName();
        root_->path = QFileInfo(rootPath_).absoluteFilePath();
        root_->dir = true;
    }
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
    const QModelIndex index = indexForPath(filePath);
    if (!index.isValid() || title.isEmpty()) return;
    Node* node = static_cast<Node*>(index.internalPointer());
    if (node->title == title) return;
    node->title = title;
    emit dataChanged(index, index, {Qt::DisplayRole});
}

NoteTreeModel::~NoteTreeModel() = default;

QModelIndex NoteTreeModel::index(int row, int column, const QModelIndex& parent) const {
    if (!hasIndex(row, column, parent)) return {};
    const Node* parentNode = nodeOf(parent, root_.get());
    return createIndex(row, column, parentNode->children[size_t(row)].get());
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
    return int(nodeOf(parent, root_.get())->children.size());
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
    // В хранилище «каталог» — любая заметка с детьми: значок папки и есть
    // указатель раскрываемости.
    if (role == Qt::DecorationRole && (node->isDir() || !node->children.empty()))
        return folderPixmap(expanded_.contains(node->path));
    return {};
}

QString NoteTreeModel::filePath(const QModelIndex& index) const {
    if (!index.isValid()) return {};
    const Node* node = static_cast<const Node*>(index.internalPointer());
    // В хранилище заметка с детьми — всё равно заметка: открывается.
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

bool NoteTreeModel::isEmpty() const { return root_->children.empty(); }

QString NoteTreeModel::trashId() const {
    for (const auto& child : root_->children)
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
    const QString target = idOf(parent);
    if (target == id) return false;
    // В собственное поддерево нельзя: заметка стала бы своим же предком.
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
