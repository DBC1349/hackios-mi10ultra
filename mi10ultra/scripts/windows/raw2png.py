#!/usr/bin/env python3
# raw2png.py — 把 loader 帧缓冲 raw（BGRA, 1080x2340, stride=width*4）转 PNG（纯标准库）
import struct, sys, zlib

src = sys.argv[1] if len(sys.argv) > 1 else 'win_screen.raw'
dst = sys.argv[2] if len(sys.argv) > 2 else 'win_screen.png'
W, H = 1080, 2340
data = open(src, 'rb').read()
stride = W * 4
need = stride * H
if len(data) < need:
    print(f"raw too small: {len(data)} < {need}"); sys.exit(1)

def chunk(tag, payload):
    c = struct.pack('>I', len(payload)) + tag + payload
    return c + struct.pack('>I', zlib.crc32(tag + payload) & 0xffffffff)

raw_rows = bytearray()
for y in range(H):
    raw_rows.append(0)  # filter none
    row = data[y*stride:(y+1)*stride]
    # BGRA -> RGB
    for x in range(0, stride, 4):
        b, g, r, a = row[x], row[x+1], row[x+2], row[x+3]
        raw_rows += bytes((r, g, b))

png = b'\x89PNG\r\n\x1a\n'
png += chunk(b'IHDR', struct.pack('>IIBBBBB', W, H, 8, 2, 0, 0, 0))
png += chunk(b'IDAT', zlib.compress(bytes(raw_rows), 6))
png += chunk(b'IEND', b'')
open(dst, 'wb').write(png)
print(f"wrote {dst} {len(png)} bytes")
