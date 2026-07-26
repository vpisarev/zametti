#!/usr/bin/env python3
"""Сузить падающий файл до минимального куска, на котором ещё ломается инвариант.

    tests/reduce.py <mddump> <файл.md>

Режет построчно: пока падает, продолжает выкидывать. Нужен ровно для того, чтобы
не гадать по стостраничной заметке, какая именно строка ломает разбор.
"""
import subprocess
import sys
import tempfile
import os


def fails(dump, text):
    """Инвариант: serialize(parse(x)) — неподвижная точка, а IR не плывёт."""
    with tempfile.NamedTemporaryFile("w", suffix=".md", delete=False) as f:
        f.write(text)
        path = f.name
    try:
        once = subprocess.run([dump, path, "--md"], capture_output=True).stdout
        with open(path, "wb") as f:
            f.write(once)
        ir1 = subprocess.run([dump, path], capture_output=True).stdout
        twice = subprocess.run([dump, path, "--md"], capture_output=True).stdout
        with open(path, "wb") as f:
            f.write(twice)
        ir2 = subprocess.run([dump, path], capture_output=True).stdout
        return once != twice or ir1 != ir2
    finally:
        os.unlink(path)


def main():
    dump, src = sys.argv[1], sys.argv[2]
    lines = open(src, encoding="utf-8", errors="surrogateescape").read().splitlines(keepends=True)
    if not fails(dump, "".join(lines)):
        print("не падает вовсе", file=sys.stderr)
        return 1

    chunk = max(len(lines) // 2, 1)
    while chunk >= 1:
        i = 0
        while i < len(lines):
            trial = lines[:i] + lines[i + chunk:]
            if trial and fails(dump, "".join(trial)):
                lines = trial
            else:
                i += chunk
        if chunk == 1:
            break
        chunk = max(chunk // 2, 1)
    sys.stdout.write("".join(lines))
    return 0


if __name__ == "__main__":
    sys.exit(main())
