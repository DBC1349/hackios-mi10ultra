#!/usr/bin/env python3
# add_hackios_board.py — 给自家 loader 的 /chosen 加 hackios-board 属性（umi 内核驱动门控用）
import shutil, sys
L = "/root/hackios/xnuloader/loader.c"
src = open(L, encoding="utf-8").read()
if 'hackios-board' in src and 'dt_prop_str(&w, "hackios-board"' in src:
    print("already patched"); sys.exit(0)
anchor = '\tdt_prop_str(&w, "psci-method", plat->psci_hvc ? "hvc" : "smc");\n'
if anchor not in src:
    print("!! anchor not found"); sys.exit(1)
ins = anchor + '\t/*\n\t * Which board this is: the HackiOS kernel\'s board-specific drivers\n\t * (touch/keys/sound/...) gate themselves on this property.\n\t */\n\tdt_prop_str(&w, "hackios-board", plat->name);\n'
src = src.replace(anchor, ins, 1)
shutil.copyfile(L, L + ".pre-boardprop.bak")
open(L, "w", encoding="utf-8").write(src)
print("added hackios-board property after psci-method")
for i, l in enumerate(src.split("\n"), 1):
    if 'hackios-board' in l:
        print(f"  {i}: {l.strip()}")
