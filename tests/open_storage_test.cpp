// ОТКРЫТЬ ИЛИ ЗАВЕСТИ ХРАНИЛИЩЕ ИЗ ОКНА — то, на чём стоит кнопка `database`.
//
// Что стережётся:
//
//   * ZStorage::inspect — четыре исхода, по которым кнопка решает, открыть,
//     спросить или отказать. Решение принадлежит хранилищу, и проверяется оно
//     здесь, а не через диалог;
//   * ЗАМОК ОТПУСКАЕТСЯ СМЕРТЬЮ ОБЪЕКТА. Отдельного close() у ZStorage нет:
//     отпустили последний shared_ptr — деструктор снял замок. Забытая где-то
//     копия тихо удержала бы его, и следующее открытие того же каталога
//     упёрлось бы в «уже открыто другой копией zametti», указывающее на нас
//     самих;
//   * СИММЕТРИЯ ПЕРЕКЛЮЧЕНИЯ: A → B → A совпадает со свежим открытием A.
//     Правило проекта: производное состояние показа восстанавливает одна
//     функция, и её зовут ОБА пути. Классический баг — «свежее открытие
//     рисуется, переключился-вернулся — хвост пропал»;
//   * кнопка «открыть хранилище» есть в тулбаре и гаснет вместе с остальными,
//     когда хранилища нет, — кроме себя самой.

#include "note_panels.h"
#include "note_tree.h"
#include "toolbar.h"
#include "zstorage.h"

#include "test_util.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <memory>
#include <string>
#include <vector>

namespace {

using zametti::NotePanels;
using zametti::Toolbar;
using zametti::ZStorage;

std::string s(const QString& q) { return q.toStdString(); }

// Дерево словами: строка на узел, вложенность отступом. Сравнивать состояния
// дерева иначе нечем — а сравнивать надо именно то, что видно.
QString dump(const zametti::NoteTreeModel& model, const QModelIndex& parent, int depth) {
    QString out;
    for (int row = 0; row < model.rowCount(parent); ++row) {
        const QModelIndex at = model.index(row, 0, parent);
        out += QString(depth * 2, QLatin1Char(' ')) + model.titleOf(at) + QLatin1Char('\n');
        out += dump(model, at, depth + 1);
    }
    return out;
}

// Хранилище с узнаваемым деревом: имя папки — чтобы в дампе было видно, чьё оно.
std::shared_ptr<ZStorage> makeStore(const QString& root, const QString& mark, QString* error) {
    if (!ZStorage(root).init(error)) return nullptr;
    auto storage = std::make_shared<ZStorage>(root);
    storage->reload();
    storage->ensureRootNote(error);
    const QString folder = storage->createNote(QString(), true, error);
    storage->rename(folder, QStringLiteral("папка ") + mark, zametti::ZJournal::Rules{}, error);
    const QString note = storage->createNote(folder, false, error);
    storage->rename(note, QStringLiteral("заметка ") + mark, zametti::ZJournal::Rules{}, error);
    return storage;
}

}  // namespace

static int ztRunSuite(int, char**) {
    QTemporaryDir home;
    QString error;

    // --- ЧТО НАМ НАЗВАЛИ ----------------------------------------------------
    {
        using Kind = ZStorage::DirKind;
        const QString nowhere = home.path() + QStringLiteral("/нет-такого");
        ZT_TRUE("несуществующий каталог", ZStorage::inspect(nowhere) == Kind::Missing);
        ZT_TRUE("пустая строка — тоже не каталог", ZStorage::inspect(QString()) == Kind::Missing);

        const QString empty = home.path() + QStringLiteral("/пустой");
        QDir().mkpath(empty);
        ZT_TRUE("пустой каталог", ZStorage::inspect(empty) == Kind::Empty);

        // СКРЫТЫЙ ФАЙЛ — ТОЖЕ СОДЕРЖИМОЕ. Каталог с одним .DS_Store честнее
        // назвать чужим, чем засеять поверх чужого добра.
        const QString hidden = home.path() + QStringLiteral("/со-скрытым");
        QDir().mkpath(hidden);
        QFile dot(hidden + QStringLiteral("/.DS_Store"));
        ZT_TRUE("скрытый файл создан", dot.open(QIODevice::WriteOnly));
        dot.close();
        ZT_TRUE("каталог с одним скрытым файлом — чужой",
                ZStorage::inspect(hidden) == Kind::Foreign);

        const QString foreign = home.path() + QStringLiteral("/чужой");
        QDir().mkpath(foreign);
        QFile plain(foreign + QStringLiteral("/письмо.txt"));
        ZT_TRUE("чужой файл создан", plain.open(QIODevice::WriteOnly));
        plain.close();
        ZT_TRUE("непустой чужой каталог", ZStorage::inspect(foreign) == Kind::Foreign);

        const QString store = home.path() + QStringLiteral("/хранилище");
        ZT_TRUE("хранилище заведено: " + s(error), ZStorage(store).init(&error));
        ZT_TRUE("живое хранилище", ZStorage::inspect(store) == Kind::Store);
        // Хранилище проверяется РАНЬШЕ пустоты, и это не вопрос вкуса: у него
        // внутри есть .zametti, то есть пустым оно не бывает никогда.
        ZT_TRUE("хранилище не считается ни пустым, ни чужим",
                ZStorage::inspect(store) != Kind::Empty &&
                    ZStorage::inspect(store) != Kind::Foreign);
    }

    // --- ЗАМОК ОТПУСКАЕТСЯ СМЕРТЬЮ ОБЪЕКТА ---------------------------------
    {
        const QString root = home.path() + QStringLiteral("/замок");
        ZT_TRUE("хранилище заведено: " + s(error), ZStorage(root).init(&error));

        auto first = std::make_shared<ZStorage>(root);
        ZT_TRUE("замок взят", first->lock().locked);
        {
            ZStorage second(root);
            ZT_TRUE("второй не берёт занятый замок", !second.lock().locked);
            ZT_TRUE("и говорит, что занято, а не что его негде завести",
                    second.lock().busy);
        }
        // Ни close(), ни release() — просто отпускаем последнюю копию.
        first.reset();
        ZStorage third(root);
        ZT_TRUE("после смерти объекта замок свободен", third.lock().locked);
    }

    // --- СИММЕТРИЯ ПЕРЕКЛЮЧЕНИЯ --------------------------------------------
    {
        const QString rootA = home.path() + QStringLiteral("/A");
        const QString rootB = home.path() + QStringLiteral("/B");
        auto a = makeStore(rootA, QStringLiteral("A"), &error);
        auto b = makeStore(rootB, QStringLiteral("B"), &error);
        ZT_TRUE("оба хранилища заведены: " + s(error), a != nullptr && b != nullptr);

        // Свежее открытие A — эталон.
        const QString fresh = [&] {
            NotePanels panels(a);
            return dump(panels.model(), QModelIndex(), 0);
        }();
        ZT_TRUE("в свежем A видно его папку", fresh.contains(QStringLiteral("папка A")));

        NotePanels panels(a);
        const QString atA = dump(panels.model(), QModelIndex(), 0);
        ZT_EQ("свежее открытие повторяемо", s(fresh), s(atA));

        panels.setStorage(b);
        const QString atB = dump(panels.model(), QModelIndex(), 0);
        ZT_TRUE("после переключения видно B", atB.contains(QStringLiteral("папка B")));
        ZT_TRUE("и не видно A", !atB.contains(QStringLiteral("папка A")));

        panels.setStorage(a);
        ZT_EQ("переключился-вернулся == открыл заново", s(fresh),
              s(dump(panels.model(), QModelIndex(), 0)));

        // БЕЗ ХРАНИЛИЩА ДЕРЕВО ПУСТО, а не показывает текущий каталог: пустой
        // корень уходил в обход каталога, а QDir("") для Qt — это cwd, и окно
        // без хранилища показывало содержимое места запуска.
        panels.setStorage(nullptr);
        ZT_EQ("без хранилища дерево пусто", std::string(),
              s(dump(panels.model(), QModelIndex(), 0)));
        ZT_TRUE("и это не хранилище", !panels.isStore());
    }

    // --- КНОПКА ------------------------------------------------------------
    {
        Toolbar bar;
        ZT_TRUE("кнопка «открыть хранилище» построена",
                bar.buttonFor(Toolbar::Button::OpenStore) != nullptr);
        ZT_TRUE("она самая левая в списке",
                Toolbar::specs().front().id == Toolbar::Button::OpenStore);

        // Без хранилища гаснет всё, кроме неё, — иначе пустое окно не
        // подсказывает ничего.
        for (const Toolbar::Spec& spec : Toolbar::specs()) {
            if (spec.id == Toolbar::Button::OpenStore) continue;
            bar.setPromise(spec.id, QStringLiteral("no storage is open"));
        }
        int lit = 0;
        for (const Toolbar::Spec& spec : Toolbar::specs())
            if (bar.isEnabled(spec.id)) ++lit;
        ZT_EQ("горит ровно одна кнопка", std::string("1"), std::to_string(lit));
        ZT_TRUE("и это она", bar.isEnabled(Toolbar::Button::OpenStore));
    }

    return zt::report("открыть хранилище");
}

TEST(OpenStorage, All) {
    std::vector<QByteArray> ztArgs{QByteArrayLiteral("open_storage_test")};
    std::vector<char*> ztArgv;
    for (QByteArray& a : ztArgs) ztArgv.push_back(a.data());
    EXPECT_EQ(0, ztRunSuite(int(ztArgv.size()), ztArgv.data()));
}
