#!/usr/bin/env python3
"""Разложить примеры из spec.txt CommonMark/GFM по отдельным .md файлам.

    tests/extract_spec.py spec.txt .testdata/commonmark

Датасет реальных заметок для проверки инвариантов слабоват: он писался людьми
и конвертерами, и пограничные случаи в нём встречаются случайно. Спецификация
же состоит из них целиком — это ровно тот вход, на котором ломается
восстановление границ блоков.

Пример в spec.txt выглядит так:

    ```````````````````````````````` example
    markdown-вход
    .
    ожидаемый html
    ````````````````````````````````

Нужен только вход. Табуляции в spec.txt записаны стрелками "→".
"""
import os
import re
import sys

OPEN = re.compile(r"^`{32,} example")
CLOSE = re.compile(r"^`{32,}\s*$")


def main():
    if len(sys.argv) < 3:
        print(__doc__, file=sys.stderr)
        return 2
    spec, outdir = sys.argv[1], sys.argv[2]
    os.makedirs(outdir, exist_ok=True)

    lines = open(spec, encoding="utf-8").read().split("\n")
    n = 0
    i = 0
    while i < len(lines):
        if not OPEN.match(lines[i]):
            i += 1
            continue
        i += 1
        body = []
        while i < len(lines) and lines[i] != "." and not CLOSE.match(lines[i]):
            body.append(lines[i])
            i += 1
        while i < len(lines) and not CLOSE.match(lines[i]):
            i += 1
        i += 1

        n += 1
        text = "\n".join(body)
        if text:
            text += "\n"
        text = text.replace("→", "\t")
        with open(os.path.join(outdir, f"{n:04d}.md"), "w", encoding="utf-8") as f:
            f.write(text)

    print(f"{n} примеров → {outdir}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
