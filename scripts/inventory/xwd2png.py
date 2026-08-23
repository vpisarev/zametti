#!/usr/bin/env python3
# XWD → PNG без чужих библиотек. Поля заголовка XWD (32 бита, big-endian) идут
# в порядке: 0 header_size, 3 depth, 4 width, 5 height, 11 bits_per_pixel,
# 12 bytes_per_line, 19 colormap_entries(ncolors). Ошибиться индексом легко —
# прошлая сессия читала четыре байта на пиксель вместо трёх и приняла кашу за
# артефакт.
import struct, sys, zlib
src, dst = sys.argv[1], sys.argv[2]
d = open(src, 'rb').read()
h = struct.unpack('>25I', d[:100])
size, depth, w, hgt, bpp, bpl, ncolors = h[0], h[3], h[4], h[5], h[11], h[12], h[19]
off = size + ncolors * 12
rows = []
for y in range(hgt):
    line = d[off + y * bpl: off + y * bpl + w * (bpp // 8)]
    if bpp == 32:      # BGRX
        rows.append(b'\x00' + b''.join(bytes((line[x*4+2], line[x*4+1], line[x*4])) for x in range(w)))
    elif bpp == 24:    # BGR
        rows.append(b'\x00' + b''.join(bytes((line[x*3+2], line[x*3+1], line[x*3])) for x in range(w)))
    else:
        raise SystemExit(f'не умею {bpp} бит на пиксель')
raw = b''.join(rows)
def chunk(tag, payload):
    return struct.pack('>I', len(payload)) + tag + payload + struct.pack('>I', zlib.crc32(tag + payload) & 0xffffffff)
png = (b'\x89PNG\r\n\x1a\n'
       + chunk(b'IHDR', struct.pack('>IIBBBBB', w, hgt, 8, 2, 0, 0, 0))
       + chunk(b'IDAT', zlib.compress(raw, 6))
       + chunk(b'IEND', b''))
open(dst, 'wb').write(png)
print(f'{w}x{hgt}, {bpp} бит/пиксель, строка {bpl} байт → {dst}')
