#!/usr/bin/env python3
# patch_gpu_terse.py — 给 hk_adreno.c 加 gpu_terse=1（抑制每寄存器刷屏，保留结论行）
import subprocess, sys
K = "/root/hackios/hackios-xnu-kernel-umi"
P = f"{K}/xnu/files/bsd/dev/hk_adreno.c"
subprocess.run(["git", "-C", K, "checkout", "--", "xnu/files/bsd/dev/hk_adreno.c"], check=True)
src = open(P, encoding="utf-8").read()

# 1) 在 quiet 声明后加 terse 助手
anchor = "static bool quiet;                      /* repeating accesses that have been announced and seen to work: say nothing */\n"
assert anchor in src, "quiet anchor missing"
helper = anchor + '''/*
 * gpu_terse=1: the same bring-up with the per-register lines left out. The loader can hand
 * back only about 32 KB of a run's log, and the register-by-register account of steps 1..21
 * is most of it -- what a board-specific failure in a later driver (Wi-Fi's RPMh rails, say)
 * has to say then falls outside. The announce()/say() conclusions are kept either way.
 */
static bool
terse_mode(void)
{
\tstatic int v = -1;
\tif (v < 0) {
\t\tv = 0;
\t\tPE_parse_boot_argn("gpu_terse", &v, sizeof(v));
\t}
\treturn v != 0;
}
'''
src = src.replace(anchor, helper, 1)

# 2) get(): quiet 时也静默
old = "\tneed(b);\n\tif (quiet) {\n\t\treturn rd(b->base + offset);\n\t}\n"
assert old in src, "get() pattern missing"
src = src.replace(old, "\tneed(b);\n\tif (quiet || terse_mode()) {\n\t\treturn rd(b->base + offset);\n\t}\n", 1)

# 3) put(): 同样
old2 = "\tneed(b);\n\tif (!quiet) {\n\t\tprintf(\"adreno: %u: about to write %s %s at %#x: %#x\\n\", step, b->name, name, b->phys + offset, value);\n"
assert old2 in src, "put() pattern missing"
src = src.replace(old2, "\tneed(b);\n\tif (!quiet && !terse_mode()) {\n\t\tprintf(\"adreno: %u: about to write %s %s at %#x: %#x\\n\", step, b->name, name, b->phys + offset, value);\n", 1)

open(P, "w", encoding="utf-8").write(src)
print("patched hk_adreno.c with gpu_terse")
for i, l in enumerate(src.split("\n"), 1):
    if "terse_mode" in l:
        print(f"  {i}: {l.strip()}")