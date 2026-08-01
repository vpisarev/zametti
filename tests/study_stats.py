#!/usr/bin/env python3
"""Разбор замеров стенда: где кривая качества перестаёт окупаться.

Мысль владельца: единственная настройка, которую за человека никто не выберет,
— это S, максимальное линейное разрешение. Всё остальное выводится из замера.

    study_stats.py <csv> [quality|restore]

Считает по каждому значению перебираемого параметра: медиану, среднее
геометрическое, среднее со среднеквадратичным отклонением и доли выборки за
порогами. Плюс главное — ПРЕДЕЛЬНУЮ ЦЕНУ: сколько килобайт стоит один
следующий балл SSIMULACRA2. Точка, где эта цена взлетает, и есть та, за
которой платить перестаёт иметь смысл.
"""

import csv
import math
import sys
from collections import defaultdict


def geomean(values):
    # Оценки бывают отрицательными (испорченная картинка), а геометрическое
    # среднее от отрицательных не определено. Сдвигаем шкалу так, чтобы ноль
    # шкалы CID22 стал единицей: сравнивать всё равно можно только между собой.
    shifted = [max(v + 101.0, 1e-6) for v in values]
    return math.exp(sum(math.log(v) for v in shifted) / len(shifted)) - 101.0


def median(values):
    s = sorted(values)
    n = len(s)
    return s[n // 2] if n % 2 else (s[n // 2 - 1] + s[n // 2]) / 2


def stddev(values):
    if len(values) < 2:
        return 0.0
    m = sum(values) / len(values)
    return math.sqrt(sum((v - m) ** 2 for v in values) / (len(values) - 1))


def main(path, mode):
    rows = []
    with open(path, newline='', encoding='utf-8') as f:
        for r in csv.DictReader(f):
            try:
                rows.append({
                    'источник': r['источник'],
                    'файл': r['файл'],
                    'S': int(r['S']),
                    'качество': int(r['качество']),
                    'байт': int(r['байт']),
                    'ssimu2': float(r['ssimu2']),
                    'уменьшали': r.get('уменьшали', '1') == '1',
                })
            except (ValueError, KeyError):
                continue
    if not rows:
        print('в csv нет разобранных строк')
        return 1

    key = 'качество' if mode == 'quality' else 'S'
    groups = defaultdict(list)
    for r in rows:
        groups[r[key]].append(r)

    files = len({r['файл'] for r in rows})
    print(f'файлов в выборке: {files}, строк: {len(rows)}')
    if mode == 'restore':
        shrunk = len({r['файл'] for r in rows if r['уменьшали']})
        print(f'из них уменьшались хотя бы при одном S: {shrunk}')
    print()

    header = 'качество' if mode == 'quality' else '   S'
    print(f'{header}  {"объём":>9} {"медиана":>8} {"геом":>7} {"среднее":>8} {"σ":>6}'
          f' {"<90":>6} {"<80":>6} {"<70":>6} {"<50":>6}')
    print('-' * 78)

    summary = []
    for k in sorted(groups):
        g = groups[k]
        scores = [r['ssimu2'] for r in g]
        total = sum(r['байт'] for r in g)
        under = lambda t: 100.0 * sum(1 for s in scores if s < t) / len(scores)
        print(f'{k:>8}  {total / 1048576:8.1f}М {median(scores):8.2f} {geomean(scores):7.2f}'
              f' {sum(scores) / len(scores):8.2f} {stddev(scores):6.2f}'
              f' {under(90):5.0f}% {under(80):5.0f}% {under(70):5.0f}% {under(50):5.0f}%')
        summary.append((k, total, median(scores)))

    # ПРЕДЕЛЬНАЯ ЦЕНА. Между соседними ступенями: сколько мегабайт добавилось и
    # сколько баллов медианы это принесло. Пока цена ровная — платить стоит;
    # там, где она подскакивает в разы, дальше платить незачем.
    print()
    print('предельная цена одного балла медианы:')
    for i in range(1, len(summary)):
        k0, b0, m0 = summary[i - 1]
        k1, b1, m1 = summary[i]
        dm = m1 - m0
        db = (b1 - b0) / 1048576.0
        if abs(dm) < 1e-9:
            print(f'  {k0} → {k1}: качество не изменилось, а объём {db:+.1f} МБ')
            continue
        print(f'  {k0} → {k1}: {db:+7.1f} МБ за {dm:+5.2f} балла '
              f'= {db / dm:8.2f} МБ на балл')
    return 0


if __name__ == '__main__':
    if len(sys.argv) < 2:
        print(__doc__)
        raise SystemExit(2)
    raise SystemExit(main(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else 'quality'))
