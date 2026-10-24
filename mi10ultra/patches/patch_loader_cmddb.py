#!/usr/bin/env python3
# patch_loader_cmddb.py — 给我们的 loader 加 "oem cmddb"：把 RPMh cmd-db 原样上传
import shutil, sys
L = "/root/hackios/xnuloader/loader.c"
src = open(L, encoding="utf-8").read()
if 'starts_with(cmd, "oem cmddb")' in src:
    print("already patched"); sys.exit(0)

anchor = '''		} else if (starts_with(cmd, "oem memmap")) {'''
assert anchor in src, "oem memmap anchor not found"
add = '''		} else if (starts_with(cmd, "oem cmddb")) {
			/*
			 * The RPMh command DB, raw (the firmware publishes it at 0x80860000, 128 KiB;
			 * the locator in the loader's device tree says so). Read here rather than on the
			 * kernel side: the loader can hand back 128 KiB while a kernel's log reaches the
			 * host only through a 32 KiB window. Parsed offline, the resource names it holds
			 * are what a board-specific rail vote has to use.
			 */
			static uint8_t cmddb_copy[0x20000];
			const volatile uint8_t *cmddb = (const volatile uint8_t *)(uintptr_t)0x80860000UL;
			for (unsigned i = 0; i < sizeof(cmddb_copy); ++i) {
				cmddb_copy[i] = cmddb[i];
			}
			staged_data = (const char *)cmddb_copy;
			staged_len = sizeof(cmddb_copy);
			fb_reply("OKAY", "");
'''
src = src.replace(anchor, add + anchor, 1)
shutil.copyfile(L, L + ".pre-cmddb.bak")
open(L, "w", encoding="utf-8").write(src)
print("loader.c patched: oem cmddb")
for i, l in enumerate(src.split("\n"), 1):
    if "oem cmddb" in l:
        print(f"  {i}: {l.strip()}")