#!/usr/bin/env python3
# add_umi_alias.py — 在自家 loader.c 里加一个名为 "umi" 的平台别名（值同 cas），排在 cas 之后
# 目的：让 umi 内核的 umi_touch/umi_keys 门控（要求 /chosen/hackios-board == "umi"）放行
import shutil, sys
L = "/root/hackios/xnuloader/loader.c"
src = open(L, encoding="utf-8").read()

if "platform_umi_alias" in src or 'platform_umi = {' in src:
    print("umi alias already present"); sys.exit(0)

anchor = "static const struct platform *const platforms[] = {"
assert anchor in src, "platforms[] anchor missing"

alias = '''static const struct platform platform_umi = {
\t/*
\t * Board-name alias: identical SoC values to cas, but named "umi" so that the
\t * umi-built XNU drivers (umi_touch/umi_keys, gated on hackios-board == "umi")
\t * engage. cas and umi share the same touch controller (ST FTS @0x49),
\t * reset GPIO38, IRQ GPIO39 and SDA36/SCL37.
\t */
\t.name = "umi", .io_base = 0x00400000, .io_size = 0x3FC00000,
\t.gic_dist = 0x17A00000, .gic_redist = 0x17A60000, .uart = 0, .log_other_cpu = 1,
\t.cores = 8, .core_shift = 8,
\t.small_cores = 0x0f,
\t.fb_base = 0x9C000000,
};

'''
src = src.replace(anchor, alias + anchor, 1)

# 注意：匹配循环没有 break，最后匹配者生效 -> umi 必须排在 cas 之后
reg_old = "\t&platform_cas,\n};"
assert reg_old in src, "platform_cas registration not found"
reg_new = "\t&platform_cas,\n\t&platform_umi,\n};"
src = src.replace(reg_old, reg_new, 1)

shutil.copyfile(L, L + ".pre-umialias.bak")
open(L, "w", encoding="utf-8").write(src)
print("added platform_umi alias; platforms[] order: redwood, cas, umi (last wins)")
for i, l in enumerate(src.split("\n"), 1):
    if "platform_umi" in l or "&platform_cas," in l or "&platform_umi," in l:
        print(f"  {i}: {l.strip()}")
