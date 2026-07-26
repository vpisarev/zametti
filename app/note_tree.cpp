#include "note_tree.h"

#include "settings.h"

#include <QCollator>
#include <QDir>
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
    : QAbstractItemModel(parent) {
    QCollator collator;
    collator.setNumericMode(true);
    collator.setCaseSensitivity(Qt::CaseInsensitive);

    root_ = buildDir(root, QFileInfo(root).fileName(), collator);
    if (root_ == nullptr) {
        root_ = std::make_unique<Node>();
        root_->title = QFileInfo(root).fileName();
        root_->path = QFileInfo(root).absoluteFilePath();
        root_->dir = true;
    }
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
    if (role == Qt::DisplayRole) return node->title;
    if (role == Qt::ToolTipRole && !node->isDir()) return node->path;
    if (role == Qt::DecorationRole && node->isDir())
        return folderPixmap(expanded_.contains(node->path));
    return {};
}

QString NoteTreeModel::filePath(const QModelIndex& index) const {
    if (!index.isValid()) return {};
    const Node* node = static_cast<const Node*>(index.internalPointer());
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
        if (probe.exists(QStringLiteral(".obsidian")) || probe.exists(QStringLiteral(".git")))
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
