#!/usr/bin/env python3
# extract_payload.py — 从 XNUP payload 中提取条目（把"能用版内核"从已验证 payload 里取出来）
import struct, sys, os
src = sys.argv[1]; outdir = sys.argv[2]
data = open(src, 'rb').read()
assert data[:4] == b'XNUP', data[:4]
size = struct.unpack('<I', data[4:8])[0]
off = 8
os.makedirs(outdir, exist_ok=True)
n = 0
while off + 24 <= len(data):
    name = data[off:off+16].split(b'\0')[0].decode('ascii', 'replace')
    if not name:
        break
    esz, _ = struct.unpack('<II', data[off+16:off+24])
    body = data[off+24: off+24+esz]
    path = os.path.join(outdir, name)
    open(path, 'wb').write(body)
    print(f"  {name}: {esz} bytes -> {path}")
    n += 1
    off += 24 + ((esz + 15) // 16) * 16
print(f"extracted {n} entries (payload size field {size})")