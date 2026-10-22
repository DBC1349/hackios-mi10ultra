#!/usr/bin/env python3
"""
Make a boot bundle for an installed device: the "XNUP" container the loader runs (kernel, boot volume,
command line -- the same one `xl run` sends) behind a header that the loader checks before it boots it.

The bundle goes into one of the two boot slots of darwin (xnuloader/flashboot.c): slot N starts at 4 KiB
block 2042368 + N * 24576 of the partition; the header is the first block, the container follows.

usage: mkbundle.py -o bundle.img --kernel K --ramdisk R [--cmdline "..."] [--generation N]
"""
import argparse, os, struct, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
BLOCK = 4096
AREA_FIRST, SLOT_BLOCKS = 2042368, 24576
PAYLOAD_LIMIT = 0x04000000 - 0x100000

def fnv(data, h=0xcbf29ce484222325):
    for b in data:
        h ^= b
        h = (h * 0x100000001b3) & 0xffffffffffffffff
    return h

def fnv_fast(data):
    """the same FNV-1a 64, a megabyte at a time"""
    h, mv = 0xcbf29ce484222325, memoryview(data)
    for i in range(0, len(mv), 1 << 20):
        h = fnv(bytes(mv[i:i + (1 << 20)]), h)
    return h

ap = argparse.ArgumentParser()
ap.add_argument('-o', '--output', required=True)
ap.add_argument('--kernel', required=True)
ap.add_argument('--ramdisk', required=True)
ap.add_argument('--cmdline', default='')
ap.add_argument('--generation', type=int, default=None, help='default: seconds since 1970 (newer wins)')
args = ap.parse_args()

with tempfile.TemporaryDirectory() as td:
    cont = os.path.join(td, 'payload.bin')
    subprocess.check_call([sys.executable, os.path.join(ROOT, 'xnuloader', 'mkpayload.py'), '-o', cont,
                           f'kernel={args.kernel}', f'ramdisk={args.ramdisk}', '--cmdline', args.cmdline],
                          stdout=subprocess.DEVNULL)
    data = open(cont, 'rb').read()
if len(data) > PAYLOAD_LIMIT or len(data) > (SLOT_BLOCKS - 1) * BLOCK:
    sys.exit(f'mkbundle: the container is {len(data)} bytes, more than a slot or the payload window takes')
gen = args.generation if args.generation is not None else int(__import__('time').time())
head = b'HKBOOT01' + struct.pack('<QQQ', gen, len(data), fnv_fast(data))
head += struct.pack('<Q', fnv(head))
out = head + b'\0' * (BLOCK - len(head)) + data
out += b'\0' * (-len(out) % BLOCK)
open(args.output, 'wb').write(out)
print(f'mkbundle: {args.output}: generation {gen}, container {len(data)} bytes, {len(out) // BLOCK} blocks')
