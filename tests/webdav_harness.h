// Настоящий WebDAV на время набора (m17, сессия 3).
//
// Приёмка, а не условие запуска (решение владельца): весь движок проверяется
// на фиктивных адаптерах, а здесь — что мы правильно говорим по проводу.
// Docker на машине владельца нет, зато есть uv: сервер поднимается как
// `uvx wsgidav` и живёт ровно столько, сколько набор.
//
// Нет uvx, нет сети (первый запуск качает пакет), сервер не встал — набор
// ГРОМКО пропускается, как и при отсутствии корпусов. Молчаливый пропуск
// неотличим от работающей проверки.

#ifndef ZAMETTI_TESTS_WEBDAV_HARNESS_H
#define ZAMETTI_TESTS_WEBDAV_HARNESS_H

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QProcess>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUrl>

#include <memory>

namespace zt {

class WebDavStand {
public:
    static constexpr char kUser[] = "zametti";
    static constexpr char kPassword[] = "пароль-набора";

    // dir — куда сервер положит файлы (каталог набора).
    explicit WebDavStand(const QString& dir) : root_(dir) {
        QDir().mkpath(root_);
        uvx_ = QStandardPaths::findExecutable(QStringLiteral("uvx"));
        if (uvx_.isEmpty()) {
            why_ = QStringLiteral("uvx не найден — поднять wsgidav нечем");
            return;
        }
        port_ = freePort();
        if (port_ == 0) {
            why_ = QStringLiteral("свободный порт не нашёлся");
            return;
        }
        if (!writeConfig()) {
            why_ = QStringLiteral("не записался конфиг wsgidav");
            return;
        }
        start();
    }

    ~WebDavStand() {
        if (server_) {
            server_->terminate();
            if (!server_->waitForFinished(5000)) server_->kill();
        }
    }

    bool running() const { return running_; }
    // Почему не поднялся — для громкого пропуска.
    const QString& why() const { return why_; }
    QUrl url() const {
        return QUrl(QStringLiteral("http://127.0.0.1:%1/облако/").arg(port_));
    }
    const QString& root() const { return root_; }

protected:
    static int freePort() {
        QTcpServer probe;
        if (!probe.listen(QHostAddress::LocalHost, 0)) return 0;
        const int port = probe.serverPort();
        probe.close();
        return port;
    }

    bool writeConfig() {
        // Пароль в открытом виде: это стенд, и лежит он в каталоге сборки.
        config_ = root_ + QStringLiteral("/wsgidav.yaml");
        QFile file(config_);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
        const QString text =
            QStringLiteral(
                "host: 127.0.0.1\n"
                "port: %1\n"
                "provider_mapping:\n"
                "  \"/\": \"%2\"\n"
                "simple_dc:\n"
                "  user_mapping:\n"
                "    \"*\":\n"
                "      \"%3\":\n"
                "        password: \"%4\"\n"
                "http_authenticator:\n"
                "  domain_controller: null\n"
                "  accept_basic: true\n"
                "  accept_digest: false\n"
                "  default_to_digest: false\n"
                "verbose: 1\n")
                .arg(port_)
                .arg(root_ + QStringLiteral("/served"))
                // fromUtf8, а не QLatin1String: константы — байты UTF-8, и
                // латиницей их прочитать значит закодировать второй раз (yaml
                // тогда падает на «unacceptable character»).
                .arg(QString::fromUtf8(kUser))
                .arg(QString::fromUtf8(kPassword));
        file.write(text.toUtf8());
        file.close();
        QDir().mkpath(root_ + QStringLiteral("/served"));
        return true;
    }

    void start() {
        server_ = std::make_shared<QProcess>();
        server_->setProcessChannelMode(QProcess::MergedChannels);
        // cheroot — сам веб-сервер: без него wsgidav ставится, но не
        // поднимается («No module named cheroot»), и первая редакция обвязки
        // молча уходила в пропуск.
        server_->start(uvx_, QStringList()
                                 << QStringLiteral("--quiet")
                                 << QStringLiteral("--with") << QStringLiteral("cheroot")
                                 << QStringLiteral("wsgidav")
                                 << QStringLiteral("--config") << config_);
        if (!server_->waitForStarted(20000)) {
            why_ = QStringLiteral("uvx не запустился: %1").arg(server_->errorString());
            server_.reset();
            return;
        }
        // Ждём, пока порт начнёт отвечать. Первый запуск качает пакет —
        // отсюда щедрый срок.
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < 90000) {
            if (server_->state() != QProcess::Running) {
                why_ = QStringLiteral("wsgidav завершился сам: %1")
                           .arg(QString::fromUtf8(server_->readAll()).left(400));
                server_.reset();
                return;
            }
            QTcpSocket probe;
            probe.connectToHost(QHostAddress::LocalHost, quint16(port_));
            if (probe.waitForConnected(300)) {
                probe.close();
                running_ = true;
                return;
            }
            server_->waitForReadyRead(200);
        }
        why_ = QStringLiteral("wsgidav не открыл порт за 90 с");
        server_->terminate();
        server_.reset();
    }

    QString root_, uvx_, config_, why_;
    int port_ = 0;
    bool running_ = false;
    std::shared_ptr<QProcess> server_;
};

}  // namespace zt

// Пропустить набор, громко сказав почему: тот же уговор, что у корпусов.
#define ZT_SKIP_NO_WEBDAV(stand)                                                  \
    do {                                                                          \
        if (!(stand).running()) {                                                 \
            GTEST_SKIP() << "настоящий WebDAV не поднялся: "                      \
                         << (stand).why().toStdString();                          \
        }                                                                         \
    } while (false)

#endif  // ZAMETTI_TESTS_WEBDAV_HARNESS_H
