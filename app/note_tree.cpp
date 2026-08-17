#include "note_tree.h"

#include <QMouseEvent>
#include <QSignalBlocker>

#include <functional>
#include <optional>

#include "icons.h"
#include "document.h"
#include "note_id.h"
#include "settings.h"
#include "archive.h"
#include "lost_found.h"
#include "times.h"

#include <QCollator>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QHash>
#include <QPainter>
#include <QPen>
#include <QPixmap>

#include <algorithm>
#include <cstdio>

namespace zametti {

struct NoteTreeModel::Node {
    QString title;   // подпись: имя заметки без ".md" или имя каталога
    QString path;    // полный путь, и у файла, и у каталога
    QString id;      // id заметки в хранилище; вне хранилища пусто
    QString badge;   // пометка починки («сирота», «цикл») — подпись и подсказка
    QString snippet; // начало текста для средней колонки
    QString modified;   // ISO из меты — для сортировки свежие сверху
    QString effectiveModified;   // максимум по поддереву: живые каталоги вперёд
    // Дата создания из шапки. У папки берётся её собственная, а не максимум по
    // поддереву: папка «2026/08» заводится один раз и с тех пор стоит на месте
    // — этим она и хороша для дневника. Пусто — в шапке ключа нет; тогда её
    // место в хронологии занимает modified (лучше, чем «в начале времён»).
    QString created;
    // Метка сортировки из шапки папки: `sort: created-desc`. Пусто — своей
    // метки нет, порядок наследуется.
    std::optional<SortOrder> sortMark;
    // Помечена архивной (`archived: yes` в шапке, или старый `role: trash`).
    // Пометка НЕ переносит заметку никуда: parent у неё прежний, а показывает
    // её виртуальный «Архив» — узел ниже.
    bool archived = false;
    // Тот самый виртуальный узел: файла за ним нет вовсе, он собирается из
    // помеченных заметок при каждой сборке дерева. Стоит в самом низу корня.
    bool archiveBox = false;
    bool folder = false;   // role: folder — директория и без детей
    bool lostFound = false;   // role: lost — бюро находок, спецпапка
    bool dir = false;
    bool storeRoot = false;   // «All notes»: корень хранилища отдельной строкой
    Node* parent = nullptr;
    std::vector<std::shared_ptr<Node>> children;
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
void sortChildren(std::vector<std::shared_ptr<NoteTreeModel::Node>>& children,
                  const QCollator& collator) {
    std::sort(children.begin(), children.end(),
              [&collator](const auto& a, const auto& b) {
                  if (a->isDir() != b->isDir()) return a->isDir();
                  return collator.compare(a->title, b->title) < 0;
              });
}

// Собирает поддерево каталога. Возвращает nullptr, если заметок внутри нет:
// показывать пустые ветки незачем.
std::shared_ptr<NoteTreeModel::Node> buildDir(const QString& dirPath, const QString& title,
                                              const QCollator& collator) {
    auto node = std::make_shared<NoteTreeModel::Node>();
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
            auto child = std::make_shared<NoteTreeModel::Node>();
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
// Подпись корневой строки левой панели: имя хранилища из конфига, а нет его —
// ИМЯ КАТАЛОГА хранилища (решение владельца). Прежде здесь стояло «All notes»
// с обоснованием «имя каталога техническое»; на деле оно как раз и отвечает на
// вопрос «а какое хранилище открыто», а у человека их бывает несколько.
QString storeRootTitle(const QString& root) {
    const QString configured = settings().store().storeTitle();
    if (!configured.isEmpty()) return configured;
    // Голое имя каталога, без пути. Завершающая черта в корне отрезается —
    // иначе dirName() вернул бы пустую строку (тот же лишний слэш из оболочки,
    // который однажды уже стоил нам бага с заголовком).
    const QString name = QDir(QDir::cleanPath(root)).dirName();
    return name.isEmpty() ? QStringLiteral("All notes") : name;
}

std::shared_ptr<NoteTreeModel::Node> buildStore(const QString& rootPath, const ZStorage& storage) {
    // Два корня: невидимый (им отвечает QModelIndex()) и видимый — строка
    // «All notes», которая в левой панели всегда первая и всегда на месте.
    // Держать её узлом, а не рисовать отдельно, дешевле всего: перенос в
    // корень, раскрытие и выделение работают тем же кодом, что и у папок.
    auto hidden = std::make_shared<NoteTreeModel::Node>();
    hidden->path = QFileInfo(rootPath).absoluteFilePath();
    hidden->dir = true;

    auto rootOwned = std::make_shared<NoteTreeModel::Node>();
    rootOwned->title = storeRootTitle(rootPath);
    rootOwned->path = QFileInfo(rootPath).absoluteFilePath();
    rootOwned->dir = true;
    rootOwned->storeRoot = true;
    rootOwned->parent = hidden.get();
    NoteTreeModel::Node* root = rootOwned.get();
    hidden->children.push_back(std::move(rootOwned));

    // Узлы — из КАТАЛОГА хранилища; диск читает только оно.
    std::vector<std::shared_ptr<NoteTreeModel::Node>> nodes;
    QHash<QString, NoteTreeModel::Node*> byId;
    QHash<QString, QString> parentOf;
    for (const QString& id : storage.ids()) {
        const ZStorage::NoteInfo* meta = storage.info(id);
        if (meta == nullptr) continue;
        auto node = std::make_shared<NoteTreeModel::Node>();
        node->id = id;
        node->title = meta->title();
        node->snippet = meta->snippet();
        node->path = meta->path();
        node->modified = meta->modified();
        node->created = meta->created();
        node->sortMark = meta->sortMark();
        node->archived = meta->archived();
        node->folder = meta->folder();
        node->lostFound = meta->lostFound();
        byId.insert(id, node.get());
        parentOf.insert(id, meta->parent());
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

    // АРХИВ — ВИРТУАЛЬНАЯ ПАПКА. Файла за ней нет: она собирается из
    // помеченных заметок при каждой сборке дерева. Прежняя корзина была
    // настоящей заметкой, куда переносили детей; архив не переносит ничего —
    // `parent` у заметки остаётся прежним, и возврат домой поэтому ничего не
    // помнит и не ищет.
    //
    // Но в дереве он есть ВСЕГДА, даже пустой (см. вызов boxFor ниже): место,
    // куда человек убирает, не должно исчезать вместе с последней убранной
    // заметкой. Бюро находок так себя и ведёт — оно настоящая папка-файл и
    // остаётся на месте пустым; архив обязан не отличаться.
    const auto archivedAbove = [&](const QString& id) {
        // Помечен ли КТО-ТО ВЫШЕ по цепочке родителей: помеченная заметка
        // внутри помеченной папки попадает в ящик не сама по себе, а вместе с
        // папкой — иначе поддерево разъехалось бы на два места.
        QSet<QString> seen{id};
        QString at = parentOf.value(id);
        while (!at.isEmpty() && byId.contains(at) && !seen.contains(at)) {
            if (byId.value(at)->archived) return true;
            seen.insert(at);
            at = parentOf.value(at);
        }
        return false;
    };
    NoteTreeModel::Node* archiveBox = nullptr;
    const auto boxFor = [&]() -> NoteTreeModel::Node* {
        if (archiveBox != nullptr) return archiveBox;
        auto box = std::make_shared<NoteTreeModel::Node>();
        box->title = QStringLiteral("Архив");
        // Путь синтетический: узла-файла за ящиком нет, но путь нужен —
        // им адресуются раскрытые ветки и выбранная папка (indexForPath).
        // Точка в начале имени держит его подальше от настоящих заметок:
        // «<id>.md» так выглядеть не может.
        box->path = QFileInfo(rootPath).absoluteFilePath() + QStringLiteral("/.archive");
        box->dir = true;
        box->archiveBox = true;
        box->parent = root;
        archiveBox = box.get();
        root->children.push_back(std::move(box));
        return archiveBox;
    };

    // Подвес: parent в никуда — сирота в корне с пометкой.
    for (auto& node : nodes) {
        const QString parent = parentOf.value(node->id);
        NoteTreeModel::Node* home = root;
        if (node->archived && !archivedAbove(node->id)) {
            home = boxFor();
        } else if (!parent.isEmpty()) {
            if (byId.contains(parent)) home = byId.value(parent);
            else node->badge = QStringLiteral("сирота");
        }
        node->parent = home;
        home->children.push_back(std::move(node));
    }

    // АРХИВ В ДЕРЕВЕ ЕСТЬ ВСЕГДА, даже пустой. Прежняя Корзина была настоящей
    // папкой и стояла на месте независимо от содержимого; Архив собирается из
    // помеченных заметок — и, опустев, исчезал вместе с последней. Владелец
    // увидел это как «исчез Архив»: заметок в нём не осталось, и место, куда
    // он привык убирать, пропало из дерева. Место должно быть на месте.
    boxFor();

    // Директория — по явному role: folder, по детям (на неё ссылаются как на
    // родителя) или корзина. Директория не открывается — это чисто структура.
    // Эффективный modified — максимум по поддереву: каталог, где правили
    // позже всех, всплывает вперёд.
    struct Finish {
        static QString run(NoteTreeModel::Node* node) {
            node->dir = node->dir || node->folder || node->archiveBox ||
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

// Братья: корзина всегда внизу; дальше по порядку.
//
// Корзина не участвует в сортировке ВООБЩЕ, ни в каком направлении: она не
// «самая старая папка», а ящик под столом, и всплывать наверх от переворота
// направления ей незачем.
//
// Даты сравниваются строками: записи ISO одной длины и всегда в UTC — так их
// пишет ядро. Равные даты разводятся именем, иначе порядок в дневнике, где
// десяток заметок заведён в одну секунду, менялся бы от запуска к запуску.
// ПОРЯДОК — СВОЙСТВО ПАПКИ, а не всего дерева. Каждая папка упорядочивает своих
// детей по своей метке; нет метки — по той, что пришла сверху, и так до корня,
// где действует переключатель тулбара.
//
// Прежде здесь был один порядок на всё дерево — «свойство точки обзора», — и
// это была моя ошибка в устройстве: выбор папки перекладывал ВСЮ левую панель,
// потому что вместе с выбором менялся действующий порядок. Владелец увидел это
// так: «тыкаю на OpenCV, а мышка вдруг оказывается на Путешествия→Сочи —
// причём это не курсор прыгнул, а дерево перестроилось». Теперь выбор папки не
// трогает дерево вовсе: порядок в нём меняется только от переключателя или от
// правки метки.
//
// inherited — порядок, действующий у РОДИТЕЛЯ этого узла.
void sortStore(NoteTreeModel::Node* node, SortOrder inherited, const QCollator& collator) {
    const SortOrder order = node->sortMark.value_or(inherited);
    // СЛУЖЕБНЫЕ ПАПКИ ВНИЗУ, и в своём порядке: сперва всё живое, под ним бюро
    // находок, а в самом низу Архив (просьба владельца — бюро это
    // вспомогательный архив, и стоять оно должно рядом с ним). Ни та, ни другая
    // в сортировке не участвуют вовсе: это не «самые старые папки», а другие
    // места, и всплывать от переворота направления им незачем.
    const auto rank = [](const NoteTreeModel::Node* n) {
        if (n->archiveBox) return 2;
        if (n->lostFound) return 1;
        return 0;
    };
    std::sort(node->children.begin(), node->children.end(),
              [order, &collator, &rank](const auto& a, const auto& b) {
                  if (rank(a.get()) != rank(b.get())) return rank(a.get()) < rank(b.get());
                  if (order.key == SortKey::Name) {
                      // Папки первыми — только по имени: в хронологии они стоят
                      // наравне с заметками, иначе дневниковая лента
                      // разваливалась бы на «сначала все папки, потом всё
                      // остальное».
                      if (a->dir != b->dir) return a->dir;
                      const int cmp = collator.compare(a->title, b->title);
                      return order.ascending ? cmp < 0 : cmp > 0;
                  }
                  const QString& left =
                      order.key == SortKey::Created ? a->created : a->effectiveModified;
                  const QString& right =
                      order.key == SortKey::Created ? b->created : b->effectiveModified;
                  if (left != right) return order.ascending ? left < right : left > right;
                  return collator.compare(a->title, b->title) < 0;
              });
    for (auto& child : node->children) sortStore(child.get(), order, collator);
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

// Значок строки дерева — иконка Lucide, та же семья, что и на тулбаре.
//
// Раньше здесь рисовался ЗНАК ИЗ ШРИФТА: готовых чёрно-белых иконок в Qt нет, а
// эмодзи-шрифты дают цветные, и приходилось просить «Noto Emoji» с приписанным
// U+FE0E, да ещё держать запасную пару из Noto Sans Symbols2 на случай, если
// гарнитуры в системе не окажется. Всё это была плата за отсутствие своих
// иконок. Иконки появились — плата отменяется: рисунок больше не зависит ни от
// установленных шрифтов, ни от того, как эмодзи выглядит в этом году.
//
// Размер привязан к кеглю панели, а не задан числом: строка растёт вместе с
// шрифтом, и значок обязан расти с ней.
QPixmap rowPixmap(const char* icon) {
    const ZSettings& a = settings();
    const qreal dpr = qGuiApp != nullptr ? qGuiApp->devicePixelRatio() : 1.0;

    QFont font;
    font.setPointSizeF(a.ui().sidebarFontPoint() * a.ui().sidebarFolderScale());
    const int side = QFontMetrics(font).height();
    return toolbarIcon(QString::fromLatin1(icon), side, a.ui().sidebarFolderColor(), dpr);
}

const NoteTreeModel::Node* nodeOf(const QModelIndex& index, const NoteTreeModel::Node* root) {
    return index.isValid() ? static_cast<const NoteTreeModel::Node*>(index.internalPointer())
                           : root;
}

}  // namespace

NoteTreeModel::NoteTreeModel(std::shared_ptr<ZStorage> storage, QObject* parent)
    // Корень чистый — его привело к чистому виду хранилище, у двери (см.
    // ZStorage): «vpnotes//<id>.md» не равен по строке «vpnotes/<id>.md», и на
    // этом переставал обновляться заголовок в средней колонке.
    : QAbstractItemModel(parent),
      rootPath_(storage->root()),
      store_(storage->isStore()),
      storage_(std::move(storage)) {
    build();
    // ДЕРЕВО — ПРОЕКЦИЯ КАТАЛОГА, и о переменах каталог говорит сам: строится
    // заново на структурной новости, обновляет одну строку на новости о заметке.
    // Кто менял хранилище — окно, редактор, сторож каталога, — дерево не
    // спрашивает; ему всё равно.
    connect(storage_.get(), &ZStorage::catalogChanged, this, &NoteTreeModel::rebuild);
    connect(storage_.get(), &ZStorage::noteChanged, this, &NoteTreeModel::refreshRow);
}

namespace {
std::shared_ptr<ZStorage> loadedStorage(const QString& root) {
    auto storage = std::make_shared<ZStorage>(root);
    storage->reload();
    return storage;
}
}  // namespace

NoteTreeModel::NoteTreeModel(const QString& root, QObject* parent)
    : NoteTreeModel(loadedStorage(root), parent) {}

void NoteTreeModel::build() {
    QCollator collator;
    collator.setNumericMode(true);
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    if (store_) {
        // По каталогу, какой он сейчас: перечитывает его хранилище (reload), а
        // не дерево, и говорит об этом сигналом.
        root_ = buildStore(rootPath_, *storage_);
        sortStore(root_.get(), rootSort_, collator);
        rebuildShown(root_.get(), foldersOnly_);
        return;
    }
    root_ = buildDir(rootPath_, QFileInfo(rootPath_).fileName(), collator);
    if (root_ == nullptr) {
        root_ = std::make_shared<Node>();
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

void NoteTreeModel::setRootSort(SortOrder order) {
    if (order == rootSort_) return;
    rootSort_ = order;
    // ПЕРЕСОРТИРОВКА, А НЕ ПЕРЕСБОРКА. Прежде здесь стоял refresh(), то есть
    // полный скан хранилища с разбором каждого файла — и это была не мелочь:
    // на 2000 заметках он стоит 24 мс (замер), а порядок теперь меняется на
    // КАЖДОМ переходе между папками с разными метками, а не по нажатию кнопки
    // раз в день. Узлы уже в памяти, и переупорядочить их стоит 0.9 мс.
    if (!store_) return;   // вне хранилища дерево идёт по именам файлов
    QCollator collator;
    collator.setNumericMode(true);
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    beginResetModel();
    sortStore(root_.get(), rootSort_, collator);
    rebuildShown(root_.get(), foldersOnly_);
    endResetModel();
}

std::optional<SortOrder> NoteTreeModel::explicitSortOf(const QString& id) const {
    const Node* node = nodeById(id);
    return node != nullptr ? node->sortMark : std::nullopt;
}

SortOrder NoteTreeModel::effectiveSortFor(const QString& id, SortOrder fallback,
                                          bool* fromMark) const {
    if (fromMark != nullptr) *fromMark = false;
    // Корень — не файл, метки на нём быть не может: там действует переключатель.
    for (const Node* node = nodeById(id); node != nullptr; node = node->parent) {
        if (!node->sortMark.has_value()) continue;
        if (fromMark != nullptr) *fromMark = true;
        return *node->sortMark;
    }
    return fallback;
}

void NoteTreeModel::refresh() {
    // Перечитать хранилище; дерево перестроится по его сигналу. Вне хранилища
    // сигнала не будет — строимся сами.
    if (store_) {
        storage_->reload();
        return;
    }
    rebuild();
}

void NoteTreeModel::rebuild() {
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
    Node* node = findByFile(filePath);
    // Не нашли — значит путь, которым заметку зовёт редактор, и путь, которым
    // её знает дерево, разошлись. Молчать тут нельзя: строка средней колонки
    // просто перестаёт обновляться, и выглядит это как «живой заголовок иногда
    // не работает». Владелец именно так этот отказ и описал, а найти его
    // изнутри было нечем — отказ был беззвучным.
    if (node == nullptr) {
        std::fprintf(stderr,
                     "живой заголовок: заметки нет в дереве по пути [%s] — "
                     "строка списка не обновится\n",
                     filePath.toUtf8().constData());
        return;
    }
    if (node->title == title) return;
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
    if (role == SecondaryRole) return !secondaryPath_.isEmpty() && node->path == secondaryPath_;
    if (role == Qt::ToolTipRole && !node->isDir()) {
        if (node->badge == QStringLiteral("сирота"))
            return QStringLiteral("родитель не найден — показана в корне: ") + node->path;
        if (node->badge == QStringLiteral("цикл"))
            return QStringLiteral("цикл родителей разорван: ") + node->path;
        return node->path;
    }
    if (role == Qt::DecorationRole && node->isDir()) {
        // У Архива свой значок: это не папка, а другое место, и путать их
        // нельзя — перетаскивание туда означает архивацию.
        if (node->archiveBox) return rowPixmap("archive");
        // Бюро находок — тоже не обычная папка: в неё не кладут, из неё
        // забирают.
        if (node->lostFound) return rowPixmap("folder-search");
        // Папка, которую НЕ РАСКРЫТЬ, рисуется открытой: закрытый значок обещает
        // содержимое, которого в дереве нет, и человек тыкает в неё снова и
        // снова, ничего не добившись. Открытая честно говорит «дальше пусто».
        //
        // Считать надо по shown, а не по children. В хранилище дерево показывает
        // ОДНИ ПАПКИ (setFoldersOnly), и у папки с заметками, но без подпапок,
        // children не пуст, а раскрывать нечего: строк под ней ноль. Такая папка
        // рисовалась закрытой и не поддавалась щелчку. Владелец заметил это на
        // маке, но беда никакая не маковая: в его хранилище под такое описание
        // подходят 16 папок из 19.
        const bool open = expanded_.contains(node->path) || node->shown.empty();
        return rowPixmap(open ? "folder-open" : "folder");
    }
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

void expandAncestors(QTreeView& tree, const QModelIndex& row) {
    if (!row.isValid()) return;
    // Именно row.parent(), а не row: см. договор в заголовке.
    for (QModelIndex up = row.parent(); up.isValid(); up = up.parent()) tree.expand(up);
}

void NoteTreeModel::setExpanded(const QModelIndex& index, bool expanded) {
    const QString path = nodePath(index);
    if (path.isEmpty()) return;
    if (expanded) expanded_.insert(path);
    else expanded_.remove(path);
    if (index.isValid()) emit dataChanged(index, index, {Qt::DecorationRole});
}

void NoteTreeModel::setSecondaryPath(const QString& path) {
    if (path == secondaryPath_) return;
    const QModelIndex was = indexForPath(secondaryPath_);
    secondaryPath_ = path;
    const QModelIndex now = indexForPath(secondaryPath_);
    if (was.isValid()) emit dataChanged(was, was, {SecondaryRole});
    if (now.isValid()) emit dataChanged(now, now, {SecondaryRole});
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
    // Архив в общий список не попадает: убранное не должно всплывать рядом с
    // живым (так же ведёт себя Apple Notes). Внутри самого Архива — наоборот,
    // показываем всё его содержимое.
    const bool insideTrash = [&] {
        for (const Node* up = node; up != nullptr; up = up->parent)
            if (up->archiveBox) return true;
        return false;
    }();
    struct Walk {
        static void run(const Node* node, bool insideTrash, std::vector<NoteRow>& out) {
            for (const auto& child : node->children) {
                if (child->archiveBox && !insideTrash) continue;
                // Папка — структура, а не заметка: в списке ей делать нечего,
                // и открыть её тело редактором нельзя вовсе.
                if (!child->isDir())
                    out.push_back(NoteRow{child->id, child->path, child->title,
                                          child->snippet, child->modified, child->created});
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

// Узел заметки по ФАЙЛУ, которым её зовёт редактор.
//
// Искали по строке пути — и это была ошибка. Владелец видел её так: у только
// что созданной заметки заголовок в средней колонке не менялся, пока не
// переключишься на другую и обратно. Заголовок окна при этом обновлялся: ему
// путь не нужен, а поиску строки — нужен. Строки путей у редактора и у дерева
// приходят разными дорогами (одна собрана хранилищем при создании, другая —
// сканом каталога) и совпадают не всегда: хватает лишней косой черты, «..» или
// символической ссылки в корне хранилища.
//
// В плоском хранилище у заметки есть настоящее имя — её id, и он же лежит в
// имени файла. Сравнивать надо ЕГО: это чистое сравнение строк без обращений к
// файловой системе, и оно не зависит от того, как записан путь. Путь остаётся
// запасным ходом — для дерева каталогов, где id нет вовсе.
NoteTreeModel::Node* NoteTreeModel::findByFile(const QString& filePath) {
    struct Find {
        static Node* byId(Node* node, const QString& id) {
            for (auto& child : node->children) {
                if (!child->id.isEmpty() && child->id == id) return child.get();
                if (Node* found = byId(child.get(), id)) return found;
            }
            return nullptr;
        }
        static Node* byPath(Node* node, const QString& path) {
            for (auto& child : node->children) {
                if (child->path == path) return child.get();
                if (Node* found = byPath(child.get(), path)) return found;
            }
            return nullptr;
        }
    };
    if (store_) {
        const QString id = QFileInfo(filePath).completeBaseName();
        if (Node* found = Find::byId(root_.get(), id)) {
            // Нашли по id, а по пути не нашли бы: значит строки путей с двух
            // сторон РАЗНЫЕ. Сама по себе беда невелика (id её лечит), но знать,
            // чем именно они отличаются, стоит: это скажет, кто из двух путей
            // записан не так. Жалуемся один раз за запуск, чтобы не залить
            // stderr на каждой букве.
            static bool told = false;
            if (!told && found->path != filePath) {
                told = true;
                std::fprintf(stderr,
                             "пути заметки расходятся:\n  редактор: [%s]\n  дерево:   [%s]\n",
                             filePath.toUtf8().constData(), found->path.toUtf8().constData());
            }
            return found;
        }
    }
    return Find::byPath(root_.get(), filePath);
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

bool NoteTreeModel::inArchiveId(const QString& id) const {
    for (const Node* node = nodeById(id); node != nullptr; node = node->parent)
        if (node->archived || node->archiveBox) return true;
    return false;
}

bool NoteTreeModel::isArchivedId(const QString& id) const {
    const Node* node = nodeById(id);
    return node != nullptr && node->archived;
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

QStringList NoteTreeModel::descendantIdsOf(const QString& id) const {
    QStringList out;
    const Node* node = nodeById(id);
    if (node == nullptr) return out;
    // Обход в глубину с добавлением ПОСЛЕ детей: родитель всегда оказывается
    // в списке позже своих детей, и удаление подряд не наткнётся на папку,
    // внутри которой ещё что-то лежит.
    const std::function<void(const Node*)> walk = [&](const Node* at) {
        for (const auto& child : at->children) {
            walk(child.get());
            out << child->id;
        }
    };
    walk(node);
    return out;
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
        if (!child->isDir() || child->archiveBox) continue;
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
    Node* node = findByFile(path);
    if (node == nullptr) {
        std::fprintf(stderr, "обновление строки: заметки нет в дереве по пути [%s]\n",
                     path.toUtf8().constData());
        return;
    }
    // Строку обновит сигнал хранилища (noteChanged → refreshRow); сменилось
    // место — придёт catalogChanged, и дерево построится заново.
    if (!storage_->refreshNote(node->id)) {
        // Строка списка осталась бы показывать прежний заголовок и прежнюю
        // дату — то есть врать о файле, которого мы не прочли.
        std::fprintf(stderr, "строка списка не обновлена: заметка не читается [%s]\n",
                     path.toUtf8().constData());
    }
}

void NoteTreeModel::refreshRow(const QString& id) {
    if (!store_) return;
    Node* node = const_cast<Node*>(nodeById(id));
    if (node == nullptr) return;
    const ZStorage::NoteInfo* fresh = storage_->info(id);
    if (fresh == nullptr) return;
    if (node->title == fresh->title() && node->snippet == fresh->snippet() &&
        node->modified == fresh->modified())
        return;
    node->title = fresh->title();
    node->snippet = fresh->snippet();
    if (!fresh->modified().isEmpty()) node->modified = fresh->modified();
    const QModelIndex index = indexForNode(node);
    if (index.isValid()) emit dataChanged(index, index, {Qt::DisplayRole});
    emit noteRowChanged(node->id);
}

NoteRow NoteTreeModel::rowOf(const QString& id) const {
    const Node* node = nodeById(id);
    if (node == nullptr) return {};
    return NoteRow{node->id,      node->path,     node->title,
                   node->snippet, node->modified, node->created};
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

QStringList NoteTreeModel::archivedIds() const {
    QStringList out;
    struct Walk {
        static void run(const Node* node, QStringList& out) {
            for (const auto& child : node->children) {
                if (!child->id.isEmpty()) out << child->id;
                run(child.get(), out);
            }
        }
    };
    for (const auto& child : topNode()->children)
        if (child->archiveBox) Walk::run(child.get(), out);
    return out;
}

bool NoteTreeModel::isArchiveBox(const QModelIndex& index) const {
    if (!index.isValid()) return false;
    return static_cast<const Node*>(index.internalPointer())->archiveBox;
}

bool NoteTreeModel::inArchive(const QModelIndex& index) const {
    for (const Node* node = index.isValid()
                                ? static_cast<const Node*>(index.internalPointer())
                                : nullptr;
         node != nullptr; node = node->parent)
        if (node->archived || node->archiveBox) return true;
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
        if (model() == nullptr || !model()->hasChildren(index)) return;
        // От состояния НА НАЖАТИИ, а не на отпускании (почему — в заголовке).
        // Если нажали одну строку, а отпустили на другой, щелчка не было и
        // переключать нечего.
        if (QModelIndex(pressedRow_) != index) return;
        // ПЕРВЫЙ ЩЕЛЧОК ПАПКУ НЕ ЗАКРЫВАЕТ. Он её выбирает, а закрытую заодно
        // раскрывает: человек идёт смотреть, что внутри. Закрыть можно вторым
        // щелчком — по уже выбранной (см. pressedWasCurrent_).
        if (!pressedWasCurrent_) {
            if (!pressedExpanded_) setExpanded(index, true);
            return;
        }
        setExpanded(index, !pressedExpanded_);
    });
}

void NoteTreeView::setModel(QAbstractItemModel* model) {
    if (this->model() != nullptr) disconnect(this->model(), nullptr, this, nullptr);
    QTreeView::setModel(model);
    if (model == nullptr) return;
    connect(model, &QAbstractItemModel::modelAboutToBeReset, this, [this] {
        keptExpanded_ = expandedDirs();
        keptCurrent_ = currentPath();
    });
    connect(model, &QAbstractItemModel::modelReset, this, [this] {
        restoreExpanded(keptExpanded_);
        setCurrentPath(keptCurrent_, /*quiet=*/true);
        keptExpanded_.clear();
        keptCurrent_.clear();
        emit rebuilt();
    });
}

void NoteTreeView::collectExpanded(const QModelIndex& parent, QStringList& out) const {
    const auto* tree = qobject_cast<const NoteTreeModel*>(model());
    if (tree == nullptr) return;
    const int rows = tree->rowCount(parent);
    for (int i = 0; i < rows; ++i) {
        const QModelIndex child = tree->index(i, 0, parent);
        if (!tree->isDirectory(child)) continue;
        if (isExpanded(child)) out.append(tree->nodePath(child));
        collectExpanded(child, out);
    }
}

QStringList NoteTreeView::expandedDirs() const {
    // Обходом дерева: у QTreeView нет готового списка, а хранить путь каждой
    // ветки отдельно незачем — их десятки.
    QStringList out;
    collectExpanded(QModelIndex(), out);
    return out;
}

void NoteTreeView::restoreExpanded(const QStringList& dirs) {
    const auto* tree = qobject_cast<const NoteTreeModel*>(model());
    if (tree == nullptr) return;
    for (const QString& dir : dirs) {
        const QModelIndex index = tree->indexForPath(dir);
        if (index.isValid() && tree->isDirectory(index)) expand(index);
    }
}

QString NoteTreeView::currentPath() const {
    const auto* tree = qobject_cast<const NoteTreeModel*>(model());
    if (tree == nullptr) return {};
    return tree->nodePath(currentIndex());
}

bool NoteTreeView::setCurrentPath(const QString& path, bool quiet) {
    const auto* tree = qobject_cast<const NoteTreeModel*>(model());
    if (tree == nullptr || path.isEmpty()) return false;
    const QModelIndex index = tree->indexForPath(path);
    if (!index.isValid()) return false;
    std::optional<QSignalBlocker> blocked;
    if (quiet) blocked.emplace(selectionModel());
    expandAncestors(*this, index);
    setCurrentIndex(index);
    return true;
}

void NoteTreeView::mousePressEvent(QMouseEvent* event) {
    const QModelIndex at = indexAt(event->pos());
    pressedRow_ = at;
    pressedExpanded_ = at.isValid() && isExpanded(at);
    // Спрашиваем ДО базового обработчика: он и переставит курсор.
    pressedWasCurrent_ = at.isValid() && at == currentIndex();
    QTreeView::mousePressEvent(event);
}

void NoteTreeView::drawBranches(QPainter*, const QRect&, const QModelIndex&) const {}

void NoteTreeDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option,
                             const QModelIndex& index) const {
    QStyledItemDelegate::paint(painter, option, index);
    if (!index.data(NoteTreeModel::SecondaryRole).toBool()) return;
    // Пунктирная незакрашенная рамка вокруг строки: «открытая заметка лежит
    // здесь». Цветом текста, чтобы читалась и на выделении, и в тёмной теме.
    painter->save();
    QPen pen(option.palette.color(QPalette::Text));
    pen.setStyle(Qt::DashLine);
    pen.setWidth(1);
    painter->setPen(pen);
    painter->setBrush(Qt::NoBrush);
    painter->setRenderHint(QPainter::Antialiasing, false);
    painter->drawRect(option.rect.adjusted(1, 1, -2, -2));
    painter->restore();
}

QSize NoteTreeDelegate::sizeHint(const QStyleOptionViewItem& option,
                                 const QModelIndex& index) const {
    QSize size = QStyledItemDelegate::sizeHint(option, index);
    const qreal height =
        QFontMetricsF(option.font).height() * settings().ui().sidebarLineHeightFactor();
    size.setHeight(int(height + 0.5));
    return size;
}

}  // namespace zametti
