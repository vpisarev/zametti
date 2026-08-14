// Где лежат корпуса и куда набору писать своё.
//
// Раньше и то и другое приходило argv: каждому бинарнику свой путь, а в
// CMakeLists — цикл по ZAMETTI_CORPUS с добавлением пяти проверок за раз.
// В одном процессе argv на всех один, и место корпусов надо знать иначе.
//
// ПРАВИЛО ПРО ПРОПУСКИ. Корпуса в репозиторий не кладутся — их у нас гигабайты
// (одни картинки 1.6 ГБ), и на чужой машине их не будет. Набор, которому нечего
// проверять, обязан СКАЗАТЬ об этом вслух и пройти пустым, а не притвориться
// зелёным. Я на этом уже обжигался: молчаливый пропуск неотличим от работающей
// проверки, и правило «не заглушать наборы» держится в первую очередь на том,
// что пропуск видно.

#pragma once

#include <gtest/gtest.h>

#include <QDir>
#include <QFileInfo>
#include <QString>

namespace zt {

class TestData {
public:
    // Корень корпусов. Берётся из ZAMETTI_TESTDATA, иначе — .testdata рядом с
    // исходниками (путь к ним прописан сборкой).
    static QString root() {
        const QByteArray env = qgetenv("ZAMETTI_TESTDATA");
        if (!env.isEmpty()) return QString::fromLocal8Bit(env);
        return QStringLiteral(ZAMETTI_SOURCE_DIR) + QStringLiteral("/.testdata");
    }

    // Каталог корпуса по имени: corpus, commonmark, gfm, images/originals…
    // Пусто — корпуса нет.
    static QString corpus(const QString& name) {
        const QString path = root() + QLatin1Char('/') + name;
        return QFileInfo::exists(path) ? path : QString();
    }

    // Файл корпуса. Пусто — файла нет.
    static QString file(const QString& relative) {
        const QString path = root() + QLatin1Char('/') + relative;
        return QFileInfo::exists(path) ? path : QString();
    }

    // Свой каталог набору под то, что он пишет. СВОЙ у каждого: в одном
    // процессе восемьдесят наборов, и общий каталог они бы затоптали.
    // Чистится при выдаче — набор всегда начинает с пустого места.
    static QString outDir(const QString& suite) {
        const QString path =
            QStringLiteral(ZAMETTI_BINARY_DIR) + QStringLiteral("/наборы/") + suite;
        QDir dir(path);
        if (dir.exists()) dir.removeRecursively();
        QDir().mkpath(path);
        return path;
    }
};

}  // namespace zt

// Пропустить набор, громко сказав почему. Обёртка нужна ровно затем, чтобы
// пропуск нельзя было сделать молча одним GTEST_SKIP() без объяснения.
#define ZT_SKIP_NO_CORPUS(path, what)                                              \
    do {                                                                           \
        if ((path).isEmpty()) {                                                    \
            GTEST_SKIP() << "корпуса нет рядом: " << (what)                        \
                         << " (см. ZAMETTI_TESTDATA)";                             \
        }                                                                          \
    } while (false)
