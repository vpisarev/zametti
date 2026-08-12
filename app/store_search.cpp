#include "store_search.h"

#include "note_id.h"
#include "parser.h"
#include "search.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>

namespace zametti {

namespace {

// Потолок списка результатов. Не про скорость — про смысл: две тысячи строк
// глазами всё равно не читают, а память и отрисовка не бесконечны. Упёрлись —
// говорим об этом вслух, молча обрезанный список выглядел бы как полный.
constexpr int kMaxResults = 2000;

// Заголовок заметки — первый содержательный блок; то же правило, что у
// дерева. Дублировать разбор незачем: документ уже разобран.
QString titleOf(const Document& doc) {
    for (const Block& block : doc.blocks) {
        if (!block.raw && (block.kind == Kind::VSpace || block.kind == Kind::Html)) continue;
        const QString text = blockText(doc, block).simplified();
        if (text.isEmpty()) continue;
        const qsizetype eol = text.indexOf(QLatin1Char('\n'));
        return (eol < 0 ? text : text.left(eol)).left(64);
    }
    return QStringLiteral("Без названия");
}

}  // namespace

// Живёт в своём потоке. Отмена — общий счётчик поколений: главный поток
// увеличивает его на каждый новый запрос, работник сверяется с ним между
// файлами и бросает устаревший проход.
class StoreSearch::Worker : public QObject {
    Q_OBJECT

public:
    explicit Worker(std::shared_ptr<std::atomic<quint64>> latest)
        : latest_(std::move(latest)) {}

public slots:
    void run(const QString& root, const QString& text, quint64 generation) {
        if (latest_->load() != generation) return;

        QElapsedTimer timer;
        timer.start();
        const Query query = makeQuery(text);
        QVector<SearchResult> results;
        bool truncated = false;
        int scanned = 0;

        const QFileInfoList files =
            QDir(root).entryInfoList({QStringLiteral("*.md")}, QDir::Files, QDir::Name);
        for (const QFileInfo& info : files) {
            // Отмена между файлами: бросать заметку разобранной наполовину
            // незачем, а один файл — это доли миллисекунды.
            if (latest_->load() != generation) return;
            if (!isValidNoteId(info.completeBaseName().toStdString())) continue;

            QFile file(info.absoluteFilePath());
            if (!file.open(QIODevice::ReadOnly)) continue;
            const QByteArray bytes = file.readAll();
            file.close();
            ++scanned;

            const Document doc =
                parse(std::string_view(bytes.constData(), size_t(bytes.size())));
            // Заметки-папки (и сама корзина) — структура хранилища, а не текст:
            // в среднем списке их нет, и в результатах поиска им делать нечего.
            // Иначе щелчок по находке открыл бы в редакторе файл, который тело
            // иметь не должен.
            const std::string role = doc.meta.get("role");
            if (role == "folder" || role == "trash") continue;
            const std::vector<Hit> hits = findInDocument(doc, query);
            if (hits.empty()) continue;

            const QString title = titleOf(doc);
            for (const Hit& hit : hits) {
                if (results.size() >= kMaxResults) {
                    truncated = true;
                    break;
                }
                const HitLine line = hitLine(doc, hit);
                // Поля слепка остаются нулевыми: это находка в живой заметке.
                results.append(SearchResult{info.completeBaseName(),
                                            info.absoluteFilePath(), title, line.text,
                                            line.offset, line.length, hit.ordinal, 0, {}});
            }
            if (truncated) break;
        }

        if (latest_->load() != generation) return;
        emit done(text, results, truncated, timer.elapsed(), scanned);
    }

signals:
    void done(const QString& text, const QVector<zametti::SearchResult>& results,
              bool truncated, qint64 elapsedMs, int scanned);

private:
    std::shared_ptr<std::atomic<quint64>> latest_;
};

StoreSearch::StoreSearch(QObject* parent)
    : QObject(parent), latest_(std::make_shared<std::atomic<quint64>>(0)) {
    qRegisterMetaType<QVector<zametti::SearchResult>>("QVector<zametti::SearchResult>");
    worker_ = new Worker(latest_);
    worker_->moveToThread(&thread_);
    connect(&thread_, &QThread::finished, worker_, &QObject::deleteLater);
    connect(worker_, &Worker::done, this,
            [this](const QString& text, const QVector<SearchResult>& results, bool truncated,
                   qint64 elapsedMs, int scanned) {
                scanned_ = scanned;
                emit found(text, results, truncated, elapsedMs);
            });
    thread_.start();
}

StoreSearch::~StoreSearch() {
    cancel();
    thread_.quit();
    thread_.wait();
}

void StoreSearch::search(const QString& root, const QString& text) {
    const quint64 generation = ++generation_;
    latest_->store(generation);
    QMetaObject::invokeMethod(worker_, "run", Qt::QueuedConnection, Q_ARG(QString, root),
                              Q_ARG(QString, text), Q_ARG(quint64, generation));
}

void StoreSearch::cancel() { latest_->store(++generation_); }

}  // namespace zametti

#include "store_search.moc"
