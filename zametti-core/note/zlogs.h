// ЛОГИ ПРОГРАММЫ: err.log и sync.log (решение владельца, 24.08.2026).
//
// Файлы лежат РЯДОМ С config.json и state.json — их легко найти и не
// потерять, в отличие от /var. Пишут и окно, и zametti-store: настройки одни
// (config.json, раздел "logs"), дефолт — выключено.
//
// Предел — РАЗМЕРОМ, не строками (решение владельца: размер посмотреть
// быстро и откусить быстро, строки надо считать). Переполнение проверяется
// ОДИН РАЗ — при первой записи в файл за сессию: если файл больше предела,
// остаются свежие 80% предела, рез по границе строки; дальше сессия только
// дописывает. Каждая строка начинается меткой времени UTC.
//
// СЕКРЕТОВ ЗДЕСЬ НЕ БЫВАЕТ — и держится это не дисциплиной вызывающих, а
// тем, что ни у движка синка, ни у CLI паролей в сообщениях нет вовсе.
//
// Класс настроек НЕ читает: его часть настроек приходит параметром
// (configure), как ImportLimits у ввоза. Потокобезопасен — движок синка
// пишет из рабочего потока.

#ifndef ZAMETTI_ZLOGS_H
#define ZAMETTI_ZLOGS_H

#include <QMutex>
#include <QString>
#include <QTextStream>

namespace zametti {

// Какой лог. Файлов два — сознательно: err.log несёт только беды (быстро
// глянуть, не листая ход синка), sync.log — подробный ход; ошибка синка
// пишется в оба.
enum class LogKind {
    Err,
    Sync,
};

class ZLogs {
public:
    struct Limits {
        bool errEnabled = false;
        bool syncEnabled = false;
        qint64 maxBytes = 10 * 1024 * 1024;
    };

    // ЕДИНСТВЕННЫЙ ЭКЗЕМПЛЯР ПРОГРАММЫ — та же дверь, что ZApp::instance():
    // логи нужны и окну, и CLI, и любому будущему месту, где случилась беда.
    // Наборы движка им не пользуются — они подают СВОЙ ZLogs через SyncOptions.
    static ZLogs& instance();

    // dir пуст — каталог конфига приложения (рядом с config.json).
    explicit ZLogs(const QString& dir = QString());

    // Перечитанные настройки. Смена предела заново взводит проверку
    // переполнения — она снова случится при первой же записи.
    void configure(const Limits& limits);

    void err(const QString& line) { write(LogKind::Err, line); }
    void sync(const QString& line) { write(LogKind::Sync, line); }
    void write(LogKind kind, const QString& line);

    QString errPath() const;
    QString syncPath() const;

protected:
    void writeFile(const QString& path, bool enabled, bool* checked, const QString& line);
    // Первая запись сессии: файл больше предела — оставить свежие 80%,
    // рез по границе строки. Хвост читается с конца, целиком файл не грузится.
    void trimIfOversized(const QString& path);

    QString dir_;
    Limits limits_;
    bool errChecked_ = false;
    bool syncChecked_ = false;
    QMutex gate_;
};

// ПОТОК-СТРОКА в духе qDebug(): собирается операторами <<, пишется
// разрушением — в ZLogs::instance(). Удобная дверь для одиночной строки:
//
//     getLogStream(LogKind::Sync) << "merged " << name << ": +" << n;
//
// Копировать и хранить его не надо (и нельзя): это одна строка лога.
class LogStream {
public:
    explicit LogStream(LogKind kind) : kind_(kind), stream_(&buffer_) {}
    ~LogStream() {
        if (!buffer_.isEmpty()) ZLogs::instance().write(kind_, buffer_);
    }
    LogStream(const LogStream&) = delete;
    LogStream& operator=(const LogStream&) = delete;

    template <typename T>
    LogStream& operator<<(const T& value) {
        stream_ << value;
        return *this;
    }

protected:
    LogKind kind_;
    QString buffer_;
    QTextStream stream_;
};

inline LogStream getLogStream(LogKind kind) { return LogStream(kind); }

}  // namespace zametti

#endif  // ZAMETTI_ZLOGS_H
