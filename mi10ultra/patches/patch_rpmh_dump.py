#!/usr/bin/env python3
# patch_rpmh_dump.py — 给 umi_rpmh.c 加一个 cmd-db 资源名 dump（找出 cas 实际有哪些电源轨）
import shutil, sys, re
P = "/root/hackios/hackios-xnu-kernel-umi/xnu/files/bsd/dev/umi_rpmh.c"
src = open(P, encoding="utf-8").read()
if "rh_dump_names" in src:
    print("already patched"); sys.exit(0)

# 1) 在 rh_lookup 之前插入 dump 函数
anchor = "static int\nrh_lookup(const char *res, uint32_t *address)\n{"
assert anchor in src, "rh_lookup anchor not found"
dump = '''/*
 * Diagnostic: print every resource name the command DB carries, per slave header.
 * Matters on a board whose rail set differs from the one the driver was written for
 * (cas: "smpf2 lookup matches=0; refusing").
 */
static void
rh_dump_names(void)
{
\tfor (unsigned i = 0; i < 8; ++i) {
\t\tuint32_t h = 8u + 16u * i;
\t\tunsigned slave = rh_db16(h);
\t\tif (!slave) break;
\t\tuint32_t entries = RH_DB_HEADER + rh_db16(h + 2);
\t\tunsigned count = rh_db16(h + 6);
\t\tfor (unsigned j = 0; j < count; ++j) {
\t\t\tuint32_t e = entries + RH_DB_ENTRY * j;
\t\t\tchar name[9];
\t\t\tfor (unsigned k = 0; k < 8; ++k)
\t\t\t\tname[k] = (char)(rh_read(rh_db, e + k) & 0xff);
\t\t\tname[8] = 0;
\t\t\tprintf("UMI RPMH: slave %u entry %u/%u name=\\"%s\\" addr=%#x\\n",
\t\t\t    slave, j + 1, count, name, rh_read(rh_db, e + 16));
\t\t}
\t}
}

'''
src = src.replace(anchor, dump + anchor, 1)

# 2) 在 rh_map 成功返回前调用 dump（只 dump 一次）
old_ret = '''\tif (magic != RH_DB_MAGIC || db_version != 1 || (flags & 1u)) {
\t\tprintf("UMI RPMH: unsupported/standalone cmd-db\\n");
\t\treturn EINVAL;
\t}
\treturn 0;
}'''
assert old_ret in src, "rh_map tail not found"
new_ret = '''\tif (magic != RH_DB_MAGIC || db_version != 1 || (flags & 1u)) {
\t\tprintf("UMI RPMH: unsupported/standalone cmd-db\\n");
\t\treturn EINVAL;
\t}
\trh_dump_names();
\treturn 0;
}'''
src = src.replace(old_ret, new_ret, 1)

shutil.copyfile(P, P + ".pre-dump.bak")
open(P, "w", encoding="utf-8").write(src)
print("patched: rh_dump_names() called from rh_map()")
for i, l in enumerate(src.split("\n"), 1):
    if "rh_dump_names" in l:
        print(f"  {i}: {l.strip()}")