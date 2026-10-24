#!/usr/bin/env python3
# patch_tz.py — 对 cas 提取出的 TzDxeLA 打 umi 同款补丁（offset 0xd469 = 1），并检查其它 .patched.efi
import hashlib
from pathlib import Path
BC = Path('/root/hackios/Mu-Silicium/Binaries/cas')
p = BC/'QcomPkg/Drivers/TzDxe/TzDxeLA.patched.efi'
b = bytearray(p.read_bytes())
EXPECT = 'a87cb38bfa7708ae9c801abf63e9c824fd873d9dfabeab2d2456aac265c7a272'
h = hashlib.sha256(b).hexdigest()
print(f"patch 前: sha256={h}  byte@0xd469={b[0xd469]}")
if h == EXPECT:
    b[0xd469] = 1
    p.write_bytes(b)
    print(f"已打补丁 -> byte@0xd469={b[0xd469]}  new sha256={hashlib.sha256(b).hexdigest()}")
elif b[0xd469] == 1:
    print("已经是 1，无需处理")
else:
    print("!! 哈希不匹配且标志为 0：cas 的 TzDxeLA 修订与本补丁预期不同，需人工确认")

print("\n=== Binaries/cas 下所有 .patched.efi（检查是否都被我们写成了未打补丁的原始内容） ===")
for q in sorted(BC.rglob('*.patched.efi')):
    bb = q.read_bytes()
    print(f"  {q.relative_to(BC)}  {len(bb)}B  sha256={hashlib.sha256(bb).hexdigest()[:16]}")
