#include "image_importer.h"

#include <QFileInfo>
#include <QThread>

namespace zametti {
namespace {

// Кто сейчас везёт. Не «флаг, что везём»: флаг — это ВТОРОЙ ответ на вопрос,
// который уже умеет отвечать сам импортёр, и рано или поздно он с ним
// разойдётся (забыли снять на раннем возврате, сняли дважды, сняли не тот).
// Спрашиваем источник правды напрямую.
ImageImporter* g_active = nullptr;

}  // namespace

bool editingAllowed() { return g_active == nullptr || !g_active->busy(); }

// Работник живёт в чужом потоке и ничего не знает ни про окно, ни про
// документ. Всё общение — сигналами: очередь ему наполняют через
// QMetaObject::invokeMethod, и Qt сама перекладывает вызов в его поток.
class ImageImporter::Worker : public QObject {
    Q_OBJECT

public:
    Worker(std::atomic<bool>& cancel, std::atomic<bool>& busy, const ImportLimits& limits)
        : cancel_(cancel), busy_(busy), limits_(limits) {}

public slots:
    // Задание целиком: файлы или готовые пиксели. Пиксели приходят копией —
    // QImage разделяет данные по счётчику, и передавать её между потоками
    // законно ровно потому, что писать в неё никто не будет.
    void run(const QStringList& paths, const QImage& pixels, const QString& storeDir) {
        const int total = paths.isEmpty() ? 1 : int(paths.size());
        emit started(total);

        const ImportLimits limits = limits_;
        int done = 0;
        bool cancelled = false;
        for (int i = 0; i < total; ++i) {
            if (cancel_.load()) {
                cancelled = true;
                break;
            }
            const QString source = paths.isEmpty() ? QString() : paths.at(i);
            const QString name =
                source.isEmpty() ? QStringLiteral("from clipboard") : QFileInfo(source).fileName();
            emit progress(done, total, name);

            ImportedImage out;
            out.index = i;
            out.source = source;
            // Здесь и только здесь тяжёлое: разжатие, проба, энкод, арбитр, а
            // следом запись файла на свежее имя. Поток один, поэтому запись
            // последовательна по построению.
            out.stored = source.isEmpty() ? storeImagePixels(pixels, storeDir, limits)
                                          : storeImageFile(source, storeDir, limits);
            ++done;
            emit imported(out);
        }
        emit progress(done, total, QString());
        busy_.store(false);
        emit finished(done, total, cancelled);
    }

signals:
    void started(int total);
    void progress(int done, int total, const QString& name);
    void imported(const zametti::ImportedImage& image);
    void finished(int done, int total, bool cancelled);

private:
    std::atomic<bool>& cancel_;
    std::atomic<bool>& busy_;
    // Границы даны ОДИН раз, при заведении работника, параметром: лезть в
    // настройки из чужого потока нельзя, да и незачем — они не его.
    ImportLimits limits_;
};

ImageImporter::ImageImporter(const ImportLimits& limits, QObject* parent) : QObject(parent) {
    qRegisterMetaType<zametti::ImportedImage>("zametti::ImportedImage");
    thread_ = new QThread(this);
    worker_ = new Worker(cancel_, busy_, limits);
    worker_->moveToThread(thread_);
    // Работник умирает вместе с потоком, а не с нами: удалить объект, живущий
    // в чужом потоке, из своего — это гонка.
    connect(thread_, &QThread::finished, worker_, &QObject::deleteLater);

    connect(worker_, &Worker::started, this, &ImageImporter::started);
    connect(worker_, &Worker::progress, this, &ImageImporter::progress);
    connect(worker_, &Worker::imported, this, &ImageImporter::imported);
    connect(worker_, &Worker::finished, this, &ImageImporter::finished);
    thread_->start();
    // Импортёр в программе один; если заведут второй, замок будет спрашивать
    // последнего — и это честнее молчаливого «первый навсегда».
    g_active = this;
}

ImageImporter::~ImageImporter() {
    if (g_active == this) g_active = nullptr;
    cancel_.store(true);
    thread_->quit();
    // Ждём: незавершённый энкодер в чужом потоке при выходе из программы — это
    // падение на ровном месте. Ожидание конечно, потому что отмена проверяется
    // между картинками.
    thread_->wait();
}

void ImageImporter::importFiles(const QStringList& paths, const QString& storeDir) {
    if (paths.isEmpty()) return;
    cancel_.store(false);
    busy_.store(true);
    QMetaObject::invokeMethod(worker_, "run", Qt::QueuedConnection,
                              Q_ARG(QStringList, paths), Q_ARG(QImage, QImage()),
                              Q_ARG(QString, storeDir));
}

void ImageImporter::importPixels(const QImage& image, const QString& storeDir) {
    if (image.isNull()) return;
    cancel_.store(false);
    busy_.store(true);
    QMetaObject::invokeMethod(worker_, "run", Qt::QueuedConnection,
                              Q_ARG(QStringList, QStringList()), Q_ARG(QImage, image),
                              Q_ARG(QString, storeDir));
}

void ImageImporter::cancel() { cancel_.store(true); }

bool ImageImporter::busy() const { return busy_.load(); }

}  // namespace zametti

#include "image_importer.moc"
