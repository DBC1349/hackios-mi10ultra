#!/usr/bin/env python3
"""
A phone's darwin: where it is and what it is, in one file that the loader, the kernel and tools/darwin-write.py
all take it from. Every phone gets its darwin cut from the end of its own userdata (tools/userdata-carve.py),
so two phones of the same model have it in different places (a 128 GB and a 256 GB POCO X5 Pro differ by
tens of millions of blocks) and with different GUIDs.

  darwinid.py from-gpt GPT_HEAD SERIAL   read darwin's entry out of a GPT (8 blocks, as the carve makes them)
                                         and write phones/SERIAL.darwin
  darwinid.py header FILE OUT            the C header the loader is built with (xnuloader/build.sh, DARWIN_ID=)
  darwinid.py bootargs FILE              the kernel's darwin_lba= darwin_blocks= (common/flash/install.sh)
  darwinid.py show FILE

phones/ is not in the repository: it describes one person's phone. Without a file, everything keeps the values
of the phone the port was brought up on.
"""
import json, os, struct, sys, uuid

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
PHONES = os.path.join(ROOT, 'phones')
BS = 4096
APFS = uuid.UUID('7c3457ef-0000-11aa-aa11-00306543ecac')
LEAST = 3145728                 # the var store's two slots end here
DEFAULT = {'first': 25374720, 'last': 30617594, 'partn': 34,
           'unique': 'fd6854ad-24aa-4fc3-94fe-290116858d68', 'type': str(APFS)}


def from_gpt(head_path, serial):
    head = open(head_path, 'rb').read()
    h = head[BS:2 * BS]
    if h[:8] != b'EFI PART':
        sys.exit('darwinid: no GPT header in %s' % head_path)
    elba, count, esize = struct.unpack_from('<Q', h, 72)[0], struct.unpack_from('<I', h, 80)[0], struct.unpack_from('<I', h, 84)[0]
    found = None
    for i in range(count):
        e = head[elba * BS + i * esize:elba * BS + (i + 1) * esize]
        if len(e) < 128:
            break
        if e[56:128].decode('utf-16le', 'replace').split('\0')[0] == 'darwin':
            found = {'first': struct.unpack_from('<Q', e, 32)[0], 'last': struct.unpack_from('<Q', e, 40)[0],
                     'partn': i + 1, 'unique': str(uuid.UUID(bytes_le=e[16:32])),
                     'type': str(uuid.UUID(bytes_le=e[:16]))}
    if found is None:
        sys.exit('darwinid: no entry named darwin in %s' % head_path)
    if found['type'] != str(APFS) or found['last'] - found['first'] + 1 < LEAST:
        sys.exit('darwinid: darwin is not an APFS container of at least %d blocks' % LEAST)
    os.makedirs(PHONES, exist_ok=True)
    out = os.path.join(PHONES, serial + '.darwin')
    json.dump(found, open(out, 'w'), indent=1)
    print('darwinid: %s: LBA %d..%d (%.2f GiB), sda%d, %s' % (out, found['first'], found['last'],
          (found['last'] - found['first'] + 1) * BS / 2**30, found['partn'], found['unique']))


def load(path):
    return json.load(open(path)) if path and os.path.exists(path) else dict(DEFAULT)


def header(path, out):
    d = load(path)
    u = uuid.UUID(d['unique'])
    f = u.fields
    node = u.bytes[8:]
    with open(out, 'w') as o:
        o.write('/* made by common/flash/darwinid.py from %s: where this phone\'s darwin is */\n'
                % (os.path.relpath(path, ROOT) if path and os.path.exists(path) else 'the defaults'))
        o.write('#define BLOCK_DARWIN_FIRST %dULL\n#define BLOCK_DARWIN_LAST %dULL\n' % (d['first'], d['last']))
        o.write('#define BLOCK_DARWIN_UNIQ { 0x%08x, 0x%04x, 0x%04x, { %s } }\n'
                % (f[0], f[1], f[2], ', '.join('0x%02x' % b for b in node)))


def main(a):
    if a[:1] == ['from-gpt'] and len(a) == 3:
        from_gpt(a[1], a[2])
    elif a[:1] == ['header'] and len(a) == 3:
        header(a[1], a[2])
    elif a[:1] == ['bootargs'] and len(a) == 2:
        d = load(a[1])
        print('darwin_lba=%d darwin_blocks=%d' % (d['first'], d['last'] - d['first'] + 1))
    elif a[:1] == ['show'] and len(a) == 2:
        print(json.dumps(load(a[1]), indent=1))
    else:
        print(__doc__)
        return 2
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
