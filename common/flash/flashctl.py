#!/usr/bin/env python3
"""
Install, update and switch HackiOS on a phone as a dual boot next to Android -- from a computer, with the
phone in Android (adb with root) or in fastboot.

  flashctl.py status                     what is where: slots, darwin, the boot bundles, the UEFI   (writes nothing)
  flashctl.py backup                     the partitions the install touches or depends on -> backups/<serial>/<time>/
  flashctl.py write-bundle BUNDLE        a boot bundle into the older of darwin's two boot slots, read back
  flashctl.py write-uefi IMAGE           the UEFI with the loader into the spare slot's boot partition, read back
  flashctl.py switch hackios             make the spare slot active and reboot into HackiOS (from Android)
  flashctl.py switch android             make Android's slot active again (from fastboot; HackiOS's menu does it too)
  flashctl.py restore                    put the spare slot's boot partition back from the newest backup

How the dual boot is laid out (xnuloader/flashboot.c): the phone runs Android from one A/B slot; the other
slot's boot partition holds the UEFI with our loader. Android's partitions are never written. darwin holds the
iOS volume, the cryptexes, the boot area (two bundle slots) and the var store. Switching slots is what
`bootctl set-active-boot-slot` / `fastboot --set-active` do; the loader's menu switches back.

Safety:
  * every write is preceded by a check that it goes where it should (darwin by tools/darwin-write.py's own
    identity checks; the boot partition by name, slot and size) and followed by a read back;
  * write-uefi refuses without a backup of the partition it overwrites, and never touches Android's slot;
  * switch refuses while a Virtual A/B update is not merged, and when the spare slot has no UEFI of ours;
  * Android can always be brought back from fastboot (volume down + power): `fastboot --set-active=<slot>`.
Set FLASHCTL_YES=1 to answer the confirmations (scripts); every action goes into logs/flashctl.log.
"""
import hashlib, json, os, struct, subprocess, sys, time

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
DW = [sys.executable, os.path.join(ROOT, 'tools', 'darwin-write.py')]
LOG = os.path.join(ROOT, 'logs', 'flashctl.log')
BS = 4096
AREA_FIRST, SLOT_BLOCKS, SLOTS = 2042368, 24576, 2
TMP = '/data/local/tmp/hackios-flash.img'


class Refused(Exception):
    pass


def log(line):
    os.makedirs(os.path.dirname(LOG), exist_ok=True)
    with open(LOG, 'a') as f:
        f.write('%s %s\n' % (time.strftime('%Y-%m-%d %H:%M:%S'), line))
    print('flashctl: ' + line)


def sh(script):
    r = subprocess.run(['adb', 'shell', 'su -c sh'], input=script.encode(), capture_output=True, timeout=3600)
    if r.returncode != 0:
        raise Refused('the phone said %d: %s' % (r.returncode, (r.stdout + r.stderr).decode(errors='replace')[-300:]))
    return r.stdout.decode(errors='replace').strip()


def raw(cmd):
    r = subprocess.run(['adb', 'exec-out', 'su -c "%s"' % cmd], capture_output=True, timeout=1800)
    if r.returncode != 0:
        raise Refused('reading from the phone: exit %d' % r.returncode)
    return r.stdout


def confirm(what):
    if os.environ.get('FLASHCTL_YES') == '1':
        return
    if input('%s\ntype YES to go on: ' % what).strip() != 'YES':
        raise Refused('not confirmed')


def in_android():
    r = subprocess.run(['adb', 'get-state'], capture_output=True, text=True)
    return r.stdout.strip() == 'device'


def slots():
    """(android's slot letter, the spare slot letter)"""
    cur = sh('getprop ro.boot.slot_suffix').strip().lstrip('_')
    if cur not in ('a', 'b'):
        raise Refused('not an A/B phone (ro.boot.slot_suffix is %r)' % cur)
    return cur, 'b' if cur == 'a' else 'a'


def preflight():
    if not in_android():
        raise Refused('the phone is not in Android with adb (adb get-state)')
    if sh('id -u') != '0':
        raise Refused('no root shell (su)')
    unlocked = sh('getprop ro.boot.flash.locked') == '0' or sh('getprop ro.boot.verifiedbootstate') == 'orange'
    if not unlocked:
        raise Refused('the bootloader is locked')
    cur, spare = slots()
    pending = sh('ls /metadata/ota/snapshots 2>/dev/null | wc -l')
    return cur, spare, pending.strip() not in ('', '0')


def part_size(name):
    return int(sh('blockdev --getsize64 /dev/block/by-name/%s' % name))


def sha_of_part(name, size):
    return sh('head -c %d /dev/block/by-name/%s | sha256sum' % (size, name)).split()[0]


def bundle_headers():
    """the two boot slots' headers, read from darwin"""
    out = []
    for s in range(SLOTS):
        blk = raw('dd if=/dev/block/by-name/darwin bs=%d skip=%d count=1 2>/dev/null' % (BS, AREA_FIRST + s * SLOT_BLOCKS))
        if len(blk) < 40 or blk[:8] != b'HKBOOT01':
            out.append(None)
            continue
        gen, size, total = struct.unpack_from('<QQQ', blk, 8)
        out.append({'generation': gen, 'size': size})
    return out


def cmd_status():
    cur, spare, pending = preflight()
    print('android slot      : _%s' % cur)
    print('hackios slot      : _%s (boot_%s, %d MB)' % (spare, spare, part_size('boot_' + spare) >> 20))
    print('A/B update pending: %s' % ('YES -- finish it in Android before switching' if pending else 'no'))
    dw = subprocess.run(DW + ['check'], capture_output=True, text=True)
    print('darwin            : %s' % ('identity right' if dw.returncode == 0 else 'NOT as expected: ' + dw.stdout.strip()[-200:]))
    blk = raw('dd if=/dev/block/by-name/darwin bs=%d count=1 2>/dev/null' % BS)
    print('iOS volume        : %s' % ('present (APFS container)' if blk[32:36] == b'NXSB' else 'missing -- write it first (README)'))
    for s, h in enumerate(bundle_headers()):
        print('boot slot %d       : %s' % (s, 'generation %d, %d bytes' % (h['generation'], h['size']) if h else 'empty'))
    head = raw('dd if=/dev/block/by-name/boot_%s bs=4096 count=1 2>/dev/null' % spare)
    print('boot_%s           : %s' % (spare, 'an Android boot image' if head[:8] == b'ANDROID!' else 'unknown contents'))


def cmd_backup():
    cur, spare, _ = preflight()
    serial = subprocess.run(['adb', 'get-serialno'], capture_output=True, text=True).stdout.strip()
    d = os.path.join(ROOT, 'backups', serial, time.strftime('%Y%m%d-%H%M%S'))
    os.makedirs(d)
    names = [p + '_' + s for p in ('boot', 'vbmeta', 'dtbo', 'vendor_boot', 'init_boot') for s in ('a', 'b')]
    manifest = {}
    for n in names:
        if sh('[ -e /dev/block/by-name/%s ] && echo y || echo n' % n) != 'y':
            continue
        data = raw('dd if=/dev/block/by-name/%s bs=1048576 2>/dev/null' % n)
        open(os.path.join(d, n + '.img'), 'wb').write(data)
        manifest[n] = hashlib.sha256(data).hexdigest()
        log('backup %s: %d bytes, %s' % (n, len(data), manifest[n][:16]))
    # the GPT of the unit that holds boot_a/boot_b (the slot switch rewrites its attributes)
    dev = sh('basename $(readlink -f /dev/block/by-name/boot_a) | sed "s/[0-9]*$//"')
    for name, script in (('gpt-primary', 'dd if=/dev/block/%s bs=4096 count=6 2>/dev/null' % dev),
                         ('gpt-backup', 'n=$(( $(blockdev --getsz /dev/block/%s) / 8 )); dd if=/dev/block/%s bs=4096 skip=$((n - 5)) count=5 2>/dev/null' % (dev, dev))):
        data = raw(script)
        open(os.path.join(d, '%s-%s.img' % (name, dev)), 'wb').write(data)
        manifest['%s-%s' % (name, dev)] = hashlib.sha256(data).hexdigest()
    manifest['android_slot'] = cur
    json.dump(manifest, open(os.path.join(d, 'manifest.json'), 'w'), indent=1)
    log('backup in %s' % d)


def newest_backup():
    serial = subprocess.run(['adb', 'get-serialno'], capture_output=True, text=True).stdout.strip()
    base = os.path.join(ROOT, 'backups', serial)
    if not os.path.isdir(base) or not os.listdir(base):
        return None
    return os.path.join(base, sorted(os.listdir(base))[-1])


def cmd_write_bundle(path):
    preflight()
    data = open(path, 'rb').read()
    if data[:8] != b'HKBOOT01' or len(data) % BS or len(data) // BS > SLOT_BLOCKS:
        raise Refused('%s is not a bundle from mkbundle.py' % path)
    hs = bundle_headers()
    gen = struct.unpack_from('<Q', data, 8)[0]
    # the slot that does not hold the newest good bundle
    good = [(h['generation'], s) for s, h in enumerate(hs) if h]
    slot = 0 if not good else (1 - max(good)[1])
    if good and gen <= max(good)[0]:
        raise Refused('generation %d is not newer than the installed %d' % (gen, max(good)[0]))
    first = AREA_FIRST + slot * SLOT_BLOCKS
    confirm('write a %d MB bundle (generation %d) into darwin boot slot %d (blocks %d..%d)'
            % (len(data) >> 20, gen, slot, first, first + len(data) // BS - 1))
    env = dict(os.environ, DARWIN_WRITE='yes')
    body = path + '.body'
    open(body, 'wb').write(data[BS:])
    head = path + '.head'
    open(head, 'wb').write(data[:BS])
    for step in (DW + ['zero', str(first), '1'], DW + ['write', body, str(first + 1)], DW + ['write', head, str(first)]):
        r = subprocess.run(step, env=env)
        if r.returncode != 0:
            raise Refused('darwin-write refused or failed: %s (the other boot slot is untouched)' % ' '.join(step[2:]))
    os.remove(body); os.remove(head)
    log('bundle generation %d in boot slot %d' % (gen, slot))


def cmd_write_uefi(path):
    cur, spare, _ = preflight()
    img = open(path, 'rb').read()
    if img[:8] != b'ANDROID!':
        raise Refused('%s is not a boot image (mkxnuimg.py makes one)' % path)
    target = 'boot_' + spare
    if spare == cur:
        raise Refused('refusing to write Android\'s own slot')
    size = part_size(target)
    if len(img) > size:
        raise Refused('the image (%d) is bigger than %s (%d)' % (len(img), target, size))
    b = newest_backup()
    if not b or not os.path.exists(os.path.join(b, target + '.img')):
        raise Refused('no backup of %s: run `flashctl.py backup` first' % target)
    want = hashlib.sha256(img).hexdigest()
    confirm('write %s (%d KB, sha256 %s...) to %s -- Android on _%s is not touched' % (path, len(img) >> 10, want[:12], target, cur))
    subprocess.run(['adb', 'push', path, TMP], check=True, capture_output=True)
    sh('[ "$(sha256sum %s | cut -d" " -f1)" = "%s" ] && [ -b /dev/block/by-name/%s ] && '
       'dd if=%s of=/dev/block/by-name/%s bs=1048576 conv=fsync 2>/dev/null; rm -f %s' % (TMP, want, target, TMP, target, TMP))
    got = sha_of_part(target, len(img))
    if got != want:
        raise Refused('read back of %s differs (%s): restore it with `flashctl.py restore`' % (target, got[:12]))
    log('UEFI written to %s, read back right' % target)


def cmd_switch(to):
    if to == 'android':
        r = subprocess.run(['fastboot', 'getvar', 'current-slot'], capture_output=True, text=True, timeout=20)
        if 'current-slot' not in (r.stdout + r.stderr):
            raise Refused('put the phone in fastboot (volume down + power), or choose Android in the HackiOS menu')
        b = newest_backup()
        slot = json.load(open(os.path.join(b, 'manifest.json')))['android_slot'] if b else None
        if slot not in ('a', 'b'):
            raise Refused('no backup says which slot is Android\'s: run `fastboot --set-active=<slot>` yourself')
        subprocess.run(['fastboot', '--set-active=' + slot], check=True)
        subprocess.run(['fastboot', 'reboot'], check=True)
        log('switched to Android (_%s) from fastboot' % slot)
        return
    cur, spare, pending = preflight()
    if pending:
        raise Refused('a Virtual A/B update is still being merged: let Android finish it, reboot once, then switch')
    head = raw('dd if=/dev/block/by-name/boot_%s bs=4096 count=1 2>/dev/null' % spare)
    if head[:8] != b'ANDROID!':
        raise Refused('boot_%s holds no boot image: `flashctl.py write-uefi` first' % spare)
    if not any(bundle_headers()):
        raise Refused('no boot bundle in darwin: `flashctl.py write-bundle` first')
    confirm('make slot _%s active and reboot into HackiOS (the HackiOS menu or fastboot brings Android back)' % spare)
    sh('bootctl set-active-boot-slot %d' % (0 if spare == 'a' else 1))
    log('slot _%s active, rebooting' % spare)
    sh('reboot')


def cmd_restore():
    cur, spare, _ = preflight()
    b = newest_backup()
    src = b and os.path.join(b, 'boot_%s.img' % spare)
    if not src or not os.path.exists(src):
        raise Refused('no backup of boot_%s' % spare)
    cmd_write_uefi_raw(src, 'boot_' + spare)


def cmd_write_uefi_raw(path, target):
    img = open(path, 'rb').read(); want = hashlib.sha256(img).hexdigest()
    confirm('put %s back into %s' % (path, target))
    subprocess.run(['adb', 'push', path, TMP], check=True, capture_output=True)
    sh('[ "$(sha256sum %s | cut -d" " -f1)" = "%s" ] && dd if=%s of=/dev/block/by-name/%s bs=1048576 conv=fsync 2>/dev/null; rm -f %s'
       % (TMP, want, TMP, target, TMP))
    if sha_of_part(target, len(img)) != want:
        raise Refused('read back of %s differs' % target)
    log('%s restored from %s' % (target, path))


def main(a):
    try:
        if a[:1] == ['status']: cmd_status()
        elif a[:1] == ['backup']: cmd_backup()
        elif a[:1] == ['write-bundle'] and len(a) == 2: cmd_write_bundle(a[1])
        elif a[:1] == ['write-uefi'] and len(a) == 2: cmd_write_uefi(a[1])
        elif a[:1] == ['switch'] and len(a) == 2 and a[1] in ('hackios', 'android'): cmd_switch(a[1])
        elif a[:1] == ['restore']: cmd_restore()
        else:
            print(__doc__); return 2
    except Refused as e:
        log('refused: %s' % e)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
