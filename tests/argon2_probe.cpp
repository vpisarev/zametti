// Калибровка Argon2id на конкретной машине (m17, сессия 3).
//
// Пароль разворачивается в мастер-ключ ровно четыре раза за жизнь устройства
// (настройка, ротация, смена пароля, утраченный keyring), поэтому цена в
// полсекунды-секунду — правильная: дороже для перебора, незаметно для
// человека. Бриф велит выбрать параметры НА МАШИНЕ ВЛАДЕЛЬЦА и увезти их в
// keyfile рядом с солью — будущие устройства читают их оттуда, а не из
// констант.
//
// Пробник меряет сетку (память × проходы), печатает ВСЮ серию — по правилу
// «смотреть все прогоны, а не минимум» — и советует точку под окно
// 0.5–1 с: из подходящих берётся наибольшая память (стойкость Argon2 растёт
// прежде всего памятью), при равной памяти — меньше проходов.
//
//   zametti-bench argon2 [--target 0.75]
//
// Гонять с taskset, как все замеры проекта.

#include <sodium.h>

#include <QElapsedTimer>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

int ztArgon2Probe(int argc, char** argv);

namespace {

struct Point {
    size_t memMiB;
    unsigned long long ops;
    std::vector<double> seconds;   // вся серия, не только медиана
    double median() const {
        std::vector<double> s = seconds;
        std::sort(s.begin(), s.end());
        return s[s.size() / 2];
    }
};

double runOnce(size_t memMiB, unsigned long long ops) {
    static const char password[] = "калибровочный пароль средней длины";
    unsigned char salt[crypto_pwhash_SALTBYTES];
    std::memset(salt, 0x5a, sizeof(salt));
    unsigned char key[32];
    QElapsedTimer timer;
    timer.start();
    const int rc = crypto_pwhash(key, sizeof(key), password,
                                 sizeof(password) - 1, salt, ops,
                                 memMiB << 20, crypto_pwhash_ALG_ARGON2ID13);
    if (rc != 0) return -1.0;   // не хватило памяти — тоже результат
    return timer.nsecsElapsed() / 1e9;
}

}  // namespace

int ztArgon2Probe(int argc, char** argv) {
    double target = 0.75;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--target") == 0 && i + 1 < argc)
            target = std::atof(argv[++i]);
    }
    if (sodium_init() < 0) {
        std::printf("sodium_init не отработал\n");
        return 1;
    }
    const double lo = target * 2 / 3, hi = target * 4 / 3;
    std::printf("Argon2id: цель %.2f с (окно %.2f–%.2f с), повторов 3\n\n",
                target, lo, hi);
    std::printf("%8s %6s   %s\n", "память", "ops", "серия, с");

    const size_t mems[] = {64, 128, 256, 512, 1024};
    const unsigned long long opses[] = {1, 2, 3, 4, 5, 6};
    std::vector<Point> points;
    for (size_t mem : mems) {
        for (unsigned long long ops : opses) {
            Point p{mem, ops, {}};
            for (int rep = 0; rep < 3; ++rep) {
                const double s = runOnce(mem, ops);
                if (s < 0) break;
                p.seconds.push_back(s);
                // Точка заведомо за окном — серию не тянем, время дорого.
                if (rep == 0 && s > 3.0) break;
            }
            if (p.seconds.empty()) {
                std::printf("%6zu М %6llu   память не выделилась\n", mem, ops);
                continue;
            }
            std::string row;
            for (double s : p.seconds) {
                char buf[32];
                std::snprintf(buf, sizeof(buf), "%.3f ", s);
                row += buf;
            }
            if (p.seconds.size() < 3) row += "(>3 с, дальше не мерим)";
            std::printf("%6zu М %6llu   %s\n", mem, ops, row.c_str());
            points.push_back(p);
        }
    }

    // Совет: из точек в окне — наибольшая память, затем меньше проходов.
    const Point* best = nullptr;
    for (const Point& p : points) {
        if (p.seconds.size() < 3) continue;
        const double m = p.median();
        if (m < lo || m > hi) continue;
        if (!best || p.memMiB > best->memMiB ||
            (p.memMiB == best->memMiB && p.ops < best->ops))
            best = &p;
    }
    std::printf("\n");
    if (best) {
        std::printf("совет: memlimit %zu МиБ, opslimit %llu (медиана %.3f с)\n",
                    best->memMiB, best->ops, best->median());
        std::printf("параметры — порог владельца: в Keyfile::defaults() они "
                    "попадают только с его слова.\n");
    } else {
        std::printf("в окно не попала ни одна точка — расширь сетку или "
                    "поменяй --target.\n");
    }
    return 0;
}
