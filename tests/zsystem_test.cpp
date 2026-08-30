// ZSystem: барьер против того, что случилось 30.08.2026.
//
// Набор проверяет НЕ то, что дверь есть, а то, что она ЗАКРЫТА. Каждый случай
// повторяет одну из половин инцидента или прямое слово владельца, и каждый
// смотрит не только на возвращённое false, но и на ФАЙЛЫ: отказ, после которого
// что-то всё же исчезло, — не отказ.

#include "zsystem.h"

#include "test_util.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include <cstdio>
#include <string>

using zametti::ZSystem;

namespace {

std::string s(const QString& value) { return value.toStdString(); }

// Файл с содержимым; возвращает путь.
QString put(const QString& dir, const QString& name) {
    QDir().mkpath(dir);
    const QString path = dir + QLatin1Char('/') + name;
    QFile f(path);
    f.open(QIODevice::WriteOnly);
    f.write("живой");
    f.close();
    return path;
}

int countFiles(const QString& dir) {
    return int(QDir(dir).entryList(QDir::Files | QDir::NoDotAndDotDot).size());
}

// --- ПОЛОВИНА ПЕРВАЯ ИНЦИДЕНТА: относительный путь --------------------------

void checkRelativeAreaIsPowerless() {
    // Набранное в поле «../..» становилось каталогом-облаком ОТНОСИТЕЛЬНО
    // рабочего каталога процесса. Область, названная относительным путём, не
    // состоится вовсе, и разрушить ею нельзя ничего.
    QTemporaryDir home;
    const QString victim = put(home.path(), "не-трогать.md");

    for (const char* relative : {"..", "../..", ".", "заметки", "~/заметки"}) {
        const ZSystem area(ZSystem::Area::Cloud, QString::fromUtf8(relative));
        ZT_TRUE(std::string("область не состоялась: ") + relative, area.root().isEmpty());
        QString why;
        ZT_TRUE(std::string("и удалить ею нельзя: ") + relative,
                !area.removeForever(victim, &why));
        ZT_TRUE(std::string("и сказано почему: ") + relative, !why.isEmpty());
    }
    ZT_TRUE("жертва цела", QFile::exists(victim));
}

void checkAreaIsNotHomeOrRoot() {
    // Областью не объявляют корень, дом и рабочий каталог: «внутри» такой
    // области лежит слишком многое, и она перестаёт быть границей.
    for (const QString& wide : {QDir::rootPath(), QDir::homePath(), QDir::currentPath()}) {
        const ZSystem area(ZSystem::Area::Storage, wide);
        ZT_TRUE("широкая область не состоялась: " + s(wide), area.root().isEmpty());
    }
}

// --- ГРАНИЦА ОБЛАСТИ --------------------------------------------------------

void checkOutsideTheAreaIsRefused() {
    QTemporaryDir home;
    const QString inside = home.path() + QStringLiteral("/область");
    const QString outside = home.path() + QStringLiteral("/чужое");
    const QString mine = put(inside, "моё.md");
    const QString theirs = put(outside, "чужое.md");

    const ZSystem area(ZSystem::Area::Storage, inside);
    ZT_TRUE("область состоялась", !area.root().isEmpty());
    ZT_TRUE("своё удаляется", area.removeForever(mine));
    ZT_TRUE("и его правда нет", !QFile::exists(mine));

    QString why;
    ZT_TRUE("соседнее не удаляется", !area.removeForever(theirs, &why));
    ZT_TRUE("и отказ называет область", why.contains(QStringLiteral("outside")));
    ZT_TRUE("чужое цело", QFile::exists(theirs));

    // «..» внутри пути не выводит за границу молча: он разворачивается прежде
    // сравнения, и сравнение видит настоящую цель.
    const QString sneaky = inside + QStringLiteral("/../чужое/чужое.md");
    ZT_TRUE("обход через .. не проходит", !area.removeForever(sneaky));
    ZT_TRUE("чужое всё ещё цело", QFile::exists(theirs));
}

void checkAreaRootItselfSurvives() {
    // Сам корень области — не «внутри» неё: этой дверью его не снести.
    QTemporaryDir home;
    const QString inside = home.path() + QStringLiteral("/область");
    put(inside, "живой.md");
    const ZSystem area(ZSystem::Area::Storage, inside);
    ZT_TRUE("корень области не удаляется", !area.removeForever(inside));
    ZT_TRUE("и он на месте", QFileInfo(inside).isDir());
}

void checkDirectoriesNeverGoThroughRemove() {
    // Каталог у файловой двери не удаляется НИКОГДА: у каталогов своя дверь, и
    // она с доказательством.
    QTemporaryDir home;
    const QString inside = home.path() + QStringLiteral("/область");
    const QString sub = inside + QStringLiteral("/подкаталог");
    const QString kept = put(sub, "внутри.md");

    const ZSystem area(ZSystem::Area::Storage, inside);
    QString why;
    ZT_TRUE("каталог не удаляется файловой дверью", !area.removeForever(sub, &why));
    ZT_TRUE("и сказано, куда идти", why.contains(QStringLiteral("removeStorageTree")));
    ZT_TRUE("подкаталог цел", QFileInfo(sub).isDir());
    ZT_TRUE("и его содержимое тоже", QFile::exists(kept));
}

void checkRenameStaysInside() {
    QTemporaryDir home;
    const QString inside = home.path() + QStringLiteral("/область");
    const QString outside = home.path() + QStringLiteral("/чужое");
    QDir().mkpath(outside);
    const QString from = put(inside, "было.md");

    const ZSystem area(ZSystem::Area::Storage, inside);
    ZT_TRUE("внутри области переименование идёт",
            area.rename(from, inside + QStringLiteral("/стало.md")));
    ZT_TRUE("новое имя на месте", QFile::exists(inside + QStringLiteral("/стало.md")));

    const QString again = put(inside, "ещё.md");
    QString why;
    ZT_TRUE("наружу — нет", !area.rename(again, outside + QStringLiteral("/уехало.md"), &why));
    ZT_TRUE("и файл никуда не делся", QFile::exists(again));
    ZT_TRUE("наружу ничего не легло", countFiles(outside) == 0);
}

// --- ПОЛОВИНА ВТОРАЯ ИНЦИДЕНТА: «безымянное — значит наше» ------------------

void checkOnlyStorageDirectoriesAreRemoved() {
    // Слово владельца: каталог, который не является хранилищем zametti,
    // программа не удаляет вообще. Ровно это и промахнулось 30.08: каталог без
    // манифеста сочли безымянным облаком и снесли.
    QTemporaryDir home;
    const QString foreign = home.path() + QStringLiteral("/чужой-каталог");
    const QString file = put(foreign, "чужое.txt");
    QDir().mkpath(foreign + QStringLiteral("/вложенный"));

    QString why;
    ZT_TRUE("не хранилище — не сносится", !ZSystem::removeStorageTree(foreign, &why));
    ZT_TRUE("и сказано почему", why.contains(QStringLiteral("not a zametti storage")));
    ZT_TRUE("каталог цел", QFileInfo(foreign).isDir());
    ZT_TRUE("и файл в нём цел", QFile::exists(file));

    // Пустой каталог — тоже не хранилище: пустота доводом не считается.
    const QString empty = home.path() + QStringLiteral("/пусто");
    QDir().mkpath(empty);
    ZT_TRUE("пустой чужой каталог тоже не сносится", !ZSystem::removeStorageTree(empty));
    ZT_TRUE("и он на месте", QFileInfo(empty).isDir());
}

void checkStorageTreeNeedsAbsolutePath() {
    QString why;
    ZT_TRUE("относительный путь отвергнут", !ZSystem::removeStorageTree(QStringLiteral("..")));
    ZT_TRUE("и с объяснением",
            !ZSystem::removeStorageTree(QStringLiteral("../.."), &why) && !why.isEmpty());
    ZT_TRUE("объяснение называет рабочий каталог",
            why.contains(QStringLiteral("working directory")));
}

// --- ЖИЛАЯ ЗОНА И ВРЕМЕННАЯ ЗОНА -------------------------------------------

void checkHomeAreaIsOffLimits() {
    // Внутри /home и /Users каталоги не сносятся ни при каких доводах — даже
    // будучи настоящим хранилищем zametti.
    const QStringList homes = ZSystem::homeAreas();
    ZT_TRUE("жилая зона найдена", !homes.isEmpty());
    for (const QString& area : homes)
        ZT_TRUE("и названа абсолютно: " + s(area), QDir::isAbsolutePath(area));

    // ПРАВИЛО СПРАШИВАЕТСЯ ПО ПУТИ, А НЕ ПО ФАЙЛУ. Так оно проверяется и там,
    // где записать в домашний каталог нельзя вовсе (закрытая среда), — а
    // проверка правила не должна зависеть от прав на его нарушение.
    bool tempInsideHome = false;
    for (const QString& temp : ZSystem::tempAreas())
        for (const QString& area : homes)
            if (temp.startsWith(area + QLatin1Char('/'))) tempInsideHome = true;

    for (const QString& area : homes) {
        const QString victim = area + QStringLiteral("/кто-то/его-заметки");
        QString why;
        ZT_TRUE("каталог в жилой зоне сносить нельзя: " + s(victim),
                !ZSystem::mayRemoveTree(victim, &why));
        ZT_TRUE("и объяснение называет жилую зону: " + s(why),
                why.contains(QStringLiteral("where people keep their things")));
    }

    // И то же делом — если эта машина вообще даёт писать в дом.
    QTemporaryDir inHome(QDir::homePath() + QStringLiteral("/.zametti-набор-жилая-зона-"));
    if (!inHome.isValid()) {
        // Громкий пропуск, а не молчаливый: молчаливый неотличим от проверки.
        std::printf("  ПРОПУСК: в домашний каталог писать нельзя (%s) — правило "
                    "жилой зоны проверено по путям выше, но не делом\n",
                    inHome.errorString().toUtf8().constData());
        return;
    }
    const QString store = inHome.path() + QStringLiteral("/хранилище");
    QDir().mkpath(store + QStringLiteral("/.zametti"));
    const QString note = put(store, "01n6cqevh7bbfr.md");
    ZT_TRUE("это настоящее хранилище по метке", ZSystem::looksLikeStorage(store));
    if (tempInsideHome && store.startsWith(QDir::tempPath() + QLatin1Char('/'))) return;

    QString why;
    ZT_TRUE("хранилище в жилой зоне не сносится", !ZSystem::removeStorageTree(store, &why));
    ZT_TRUE("и объяснение называет жилую зону: " + s(why),
            why.contains(QStringLiteral("where people keep their things")));
    ZT_TRUE("хранилище цело", QFileInfo(store).isDir());
    ZT_TRUE("и заметка в нём цела", QFile::exists(note));
    // Уберёт за собой сам QTemporaryDir: он снёс ровно то, что создал, и это
    // его дело, а не наше — своей дверью мы бы этот каталог снести и не смогли,
    // в чём и состоит проверка.
}

void checkTempAreaIsKnown() {
    // Временная зона обязана быть известна — иначе наборы не приберут за собой.
    const QStringList temps = ZSystem::tempAreas();
    ZT_TRUE("временная зона найдена", !temps.isEmpty());
    bool covers = false;
    for (const QString& temp : temps)
        if (QDir::tempPath().startsWith(temp)) covers = true;
    ZT_TRUE("и она накрывает ответ Qt", covers);
    for (const QString& temp : temps)
        ZT_TRUE("и каждая названа абсолютно: " + s(temp), QDir::isAbsolutePath(temp));
}

void checkScratchTreeGoesOnlyInsideTemp() {
    QTemporaryDir sandbox;   // он и лежит во временной зоне
    const QString mine = sandbox.path() + QStringLiteral("/моё");
    const QString file = put(mine + QStringLiteral("/глубже"), "мусор.txt");
    ZT_TRUE("своё во временной зоне сносится: ", ZSystem::removeScratchTree(mine));
    ZT_TRUE("и его правда нет", !QFileInfo(mine).isDir());
    ZT_TRUE("и файла тоже", !QFile::exists(file));

    // А то, что вне временной зоны, — не песочница.
    QString why;
    ZT_TRUE("дом песочницей не считается",
            !ZSystem::removeScratchTree(QDir::homePath(), &why));
    ZT_TRUE("и корень тоже", !ZSystem::removeScratchTree(QDir::rootPath()));
    ZT_TRUE("сама временная зона — не песочница",
            !ZSystem::removeScratchTree(QDir::tempPath()));
    ZT_TRUE("и она на месте", QFileInfo(QDir::tempPath()).isDir());
}

void checkNothingSwallowsItsOwnHouse() {
    // Каталог, который СОДЕРЖИТ корень, дом или рабочий каталог процесса, не
    // сносится ни одной дверью.
    for (const QString& sacred : {QDir::rootPath(), QDir::homePath(), QDir::currentPath()}) {
        ZT_TRUE("не сносится: " + s(sacred), !ZSystem::removeStorageTree(sacred));
        ZT_TRUE("не сносится и как песочница: " + s(sacred),
                !ZSystem::removeScratchTree(sacred));
        ZT_TRUE("и он на месте: " + s(sacred), QFileInfo(sacred).isDir());
    }
}

}  // namespace

TEST(ZSystem, All) {
    checkRelativeAreaIsPowerless();
    checkAreaIsNotHomeOrRoot();
    checkOutsideTheAreaIsRefused();
    checkAreaRootItselfSurvives();
    checkDirectoriesNeverGoThroughRemove();
    checkRenameStaysInside();
    checkOnlyStorageDirectoriesAreRemoved();
    checkStorageTreeNeedsAbsolutePath();
    checkHomeAreaIsOffLimits();
    checkTempAreaIsKnown();
    checkScratchTreeGoesOnlyInsideTemp();
    checkNothingSwallowsItsOwnHouse();
    EXPECT_EQ(0, zt::freshFailures());
}
