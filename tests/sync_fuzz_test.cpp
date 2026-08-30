// ДВУХДЕВАЙСНАЯ СХОДИМОСТЬ — fuzz.
//
// Два хранилища с одним storeId над одним облаком-каталогом; случайные
// создания, правки и удаления с обеих сторон, синки вперемешку. Инварианты:
// после ≤3 замыкающих раундов хранилища ПОБАЙТОВО идентичны (файлы и
// журналы), и ни одна запись ни одной стороны не потеряна — присутствует в
// итоговом журнале своим адресом либо погашена его записью.
//
// Зерно печатается; упавший прогон повторяется дословно.

#include "folder_remote.h"
#include "import_limits.h"
#include "journal.h"
#include "keyfile.h"
#include "zstorage.h"

#include "mini_store.h"
#include "test_util.h"

#include <QDir>
#include <QFile>
#include <random>
#include <string>
#include <vector>

using namespace zametti;

namespace {

const Keyfile::KdfParams kTinyKdf{1, 1 << 20};

QByteArray noteBody(unsigned n) {
    QByteArray body("<!-- zametti\nversion: 1\n-->\n\n# Заметка\n\n");
    body += QByteArray("вариант ") + QByteArray::number(n) + "\n";
    for (unsigned i = 0; i < n % 7; ++i) body += "строка про осень и кофе №" + QByteArray::number(i) + "\n";
    return body;
}

QString idAt(int n) {
    // Валидные 14-значные id: цифры и буквы без i, l, o, u.
    return QStringLiteral("01n6cqev%1").arg(n + 100000, 6, 10, QLatin1Char('0'));
}

struct Device {
    zt::MiniStore store;
    std::shared_ptr<ZStorage> s;
    std::shared_ptr<FolderCloud> cloud;

    QByteArray fileOf(const QString& id) const {
        QFile f(store.root() + QStringLiteral("/") + id + QStringLiteral(".md"));
        if (!f.open(QIODevice::ReadOnly)) return QByteArray();
        return f.readAll();
    }
};

// Все адреса записей всех журналов каталога.
QVector<QPair<QString, ZJournal::RecordRef>> allRecords(const QString& root) {
    QVector<QPair<QString, ZJournal::RecordRef>> out;
    const QDir history(root + QStringLiteral("/history"));
    for (const QString& name : history.entryList({QStringLiteral("*.zm")}, QDir::Files)) {
        QFile f(history.filePath(name));
        if (!f.open(QIODevice::ReadOnly)) continue;
        ZJournal j;
        QString why;
        if (!j.parse(f.readAll(), ZJournal::Want::Frames, 0, &why)) continue;
        for (int i = 0; i < j.size(); ++i)
            out.append({name, ZJournal::RecordRef(j.at(i).time(), j.at(i).digest())});
    }
    return out;
}

bool accountedIn(const QString& root, const QString& journalName, const ZJournal::RecordRef& ref) {
    QFile f(root + QStringLiteral("/history/") + journalName);
    if (!f.open(QIODevice::ReadOnly)) return false;
    ZJournal j;
    QString why;
    if (!j.parse(f.readAll(), ZJournal::Want::Frames, 0, &why)) return false;
    for (int i = 0; i < j.size(); ++i) {
        if (j.at(i).isAddressedBy(ref)) return true;
        if (j.at(i).voidsRecord(ZJournal::Record(ZJournal::Kind::Save, ref.time(), 0, ref.digest())))
            return true;
    }
    return false;
}

void checkTwoDeviceConvergence(unsigned seed) {
    std::mt19937 rng(seed);
    fprintf(stderr, "sync_fuzz: зерно %u\n", seed);

    zt::MiniStore cloudHome;
    const QString cloud = cloudHome.root() + QStringLiteral("/облако");
    Device a, b;
    a.s = std::make_shared<ZStorage>(a.store.root());
    QString err;
    const ZStorage::Identity identity = a.s->ensureIdentity(&err);
    ZT_TRUE("идентичность отчеканилась", !identity.isEmpty());
    ZT_TRUE("копия идентичности легла",
            QFile::copy(a.store.root() + QStringLiteral("/zametti.json"),
                        b.store.root() + QStringLiteral("/zametti.json")));
    b.s = std::make_shared<ZStorage>(b.store.root());
    Keyfile keyfile;
    ZT_TRUE("ключ отчеканился",
            Keyfile::create(identity.storeId(), QStringLiteral("пароль"), kTinyKdf, &keyfile, &err));
    a.cloud = std::make_shared<FolderCloud>(cloud);
    b.cloud = std::make_shared<FolderCloud>(cloud);
    ZT_TRUE("облако A подключено", a.s->setCloud(a.cloud, keyfile, &err));
    ZT_TRUE("облако B подключено", b.s->setCloud(b.cloud, keyfile, &err));

    unsigned serial = 0;
    const auto randomOp = [&](Device& d) {
        const unsigned dice = rng() % 100;
        const QString id = idAt(int(rng() % 8));
        const QString path = d.store.root() + QStringLiteral("/") + id + QStringLiteral(".md");
        if (dice < 70 || !QFile::exists(path)) {
            // Создание или правка «vim-ом» — самый частый ход.
            QFile f(path);
            if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) f.write(noteBody(++serial));
        } else {
            d.s->reload();
            QString why;
            d.s->remove(id, ImportLimits(), &why);
        }
    };
    const auto syncOf = [&](Device& d, const char* what) {
        ZStorage::SyncReport report;
        QString why;
        const bool ok = d.s->sync({}, &report, &why);
        ZT_TRUE((std::string(what) + ": " + why.toStdString()).c_str(), ok);
        return report;
    };

    const int failuresBefore = zt::g_failures;
    for (int round = 0; round < 12 && zt::g_failures == failuresBefore; ++round) {
        const int opsA = int(rng() % 4);
        const int opsB = int(rng() % 4);
        for (int i = 0; i < opsA; ++i) randomOp(a);
        for (int i = 0; i < opsB; ++i) randomOp(b);
        // Синки вперемешку, порядок случайный, иногда кто-то молчит весь раунд.
        for (int i = 0; i < 2; ++i) {
            switch (rng() % 3) {
                case 0: syncOf(a, "промежуточный A"); break;
                case 1: syncOf(b, "промежуточный B"); break;
                default: break;
            }
        }

        // Записи, которые существуют ПЕРЕД замыкающими раундами.
        const auto recordsA = allRecords(a.store.root());
        const auto recordsB = allRecords(b.store.root());

        // Замыкание: ≤3 раундов до полной сходимости.
        syncOf(a, "замыкающий A");
        syncOf(b, "замыкающий B");
        syncOf(a, "замыкающий A2");
        syncOf(b, "замыкающий B2");

        // Побайтовая идентичность файлов и журналов.
        const auto listing = [](const QString& root, const char* glob) {
            QStringList names = QDir(root).entryList({QLatin1String(glob)}, QDir::Files);
            names.sort();
            return names;
        };
        ZT_EQ("состав заметок совпал",
              listing(a.store.root(), "*.md").join(QLatin1Char(',')).toStdString(),
              listing(b.store.root(), "*.md").join(QLatin1Char(',')).toStdString());
        for (const QString& name : listing(a.store.root(), "*.md")) {
            const QString id = name.left(name.size() - 3);
            ZT_TRUE("заметка побайтово совпала", a.fileOf(id) == b.fileOf(id));
        }
        ZT_EQ("состав журналов совпал",
              listing(a.store.root() + QStringLiteral("/history"), "*.zm")
                  .join(QLatin1Char(','))
                  .toStdString(),
              listing(b.store.root() + QStringLiteral("/history"), "*.zm")
                  .join(QLatin1Char(','))
                  .toStdString());
        for (const QString& name :
             listing(a.store.root() + QStringLiteral("/history"), "*.zm")) {
            QFile fa(a.store.root() + QStringLiteral("/history/") + name);
            QFile fb(b.store.root() + QStringLiteral("/history/") + name);
            ZT_TRUE("журнал A открылся", fa.open(QIODevice::ReadOnly));
            ZT_TRUE("журнал B открылся", fb.open(QIODevice::ReadOnly));
            ZT_TRUE("журнал побайтово совпал", fa.readAll() == fb.readAll());
        }

        // НИ ОДНА запись не потеряна: адрес присутствует или погашен.
        for (const auto& rec : recordsA)
            ZT_TRUE("запись стороны A не потеряна",
                    accountedIn(a.store.root(), rec.first, rec.second));
        for (const auto& rec : recordsB)
            ZT_TRUE("запись стороны B не потеряна",
                    accountedIn(a.store.root(), rec.first, rec.second));

        if (zt::g_failures != failuresBefore)
            fprintf(stderr, "sync_fuzz: расхождение в раунде %d, зерно %u\n", round, seed);
    }
}

}  // namespace

static int ztRunSuite(int argc, char** argv) {
    unsigned seed = 20260824u;
    if (argc > 1) seed = unsigned(std::stoul(argv[1]));
    // Наборы идут одним процессом и argv у них общий — другое зерно подаётся
    // переменной среды: ZAMETTI_SYNC_FUZZ_SEED=7 ./zametti-tests …
    const QByteArray env = qgetenv("ZAMETTI_SYNC_FUZZ_SEED");
    if (!env.isEmpty()) seed = env.toUInt();
    checkTwoDeviceConvergence(seed);
    return zt::report("sync_fuzz");
}

TEST(SyncFuzz, All) {
    std::vector<QByteArray> ztArgs{QByteArrayLiteral("sync_fuzz_test")};
    std::vector<char*> ztArgv;
    for (QByteArray& a : ztArgs) ztArgv.push_back(a.data());
    EXPECT_EQ(0, ztRunSuite(int(ztArgv.size()), ztArgv.data()));
}
