#!/usr/bin/env python3
# patch_rpmh_dump_v2.py — 正确的 cmd-db dump（带 rh_range 边界校验，只打印一次）
import subprocess, sys, os
K = "/root/hackios/hackios-xnu-kernel-umi"
P = f"{K}/xnu/files/bsd/dev/umi_rpmh.c"
# 1) 先把文件还原到仓库原始状态
subprocess.run(["git", "-C", K, "checkout", "--", "xnu/files/bsd/dev/umi_rpmh.c"], check=True)
src = open(P, encoding="utf-8").read()
assert "rh_dump_names" not in src, "restore failed"

anchor = "static int\nrh_lookup(const char *res, uint32_t *address)\n{"
assert anchor in src
dump = '''/*
 * Diagnostic, once per boot: print every resource name the command DB carries.
 * cas (Mi 10 Ultra) has a different rail set from UMI's: "smpf2 lookup matches=0; refusing".
 * Bounds are checked exactly as rh_lookup() does, so a malformed DB cannot fault here.
 */
static void
rh_dump_names(void)
{
\tstatic bool done;
\tif (done) return;
\tdone = true;
\tfor (unsigned i = 0; i < 8; ++i) {
\t\tuint32_t h = 8u + 16u * i;
\t\tunsigned slave = rh_db16(h);
\t\tif (!slave) break;
\t\tuint32_t entries = RH_DB_HEADER + rh_db16(h + 2);
\t\tuint32_t aux = RH_DB_HEADER + rh_db16(h + 4);
\t\tunsigned count = rh_db16(h + 6);
\t\tif (slave < 3 || slave > 5 || (entries & 3u) ||
\t\t    !rh_range(entries, count * RH_DB_ENTRY) || !rh_range(aux, 0)) {
\t\t\tprintf("UMI RPMH: dump: malformed slave header %u (slave=%u entries=%#x count=%u)\\n",
\t\t\t    i, slave, entries, count);
\t\t\tcontinue;
\t\t}
\t\tprintf("UMI RPMH: dump: slave %u has %u entries\\n", slave, count);
\t\tfor (unsigned j = 0; j < count; ++j) {
\t\t\tuint32_t e = entries + RH_DB_ENTRY * j;
\t\t\tchar name[9];
\t\t\tfor (unsigned k = 0; k < 8; ++k)
\t\t\t\tname[k] = (char)(rh_read(rh_db, e + k) & 0xff);
\t\t\tname[8] = 0;
\t\t\tprintf("UMI RPMH: rail \\"%s\\" addr=%#x\\n", name, rh_read(rh_db, e + 16));
\t\t}
\t}
}

'''
src = src.replace(anchor, dump + anchor, 1)

old_ret = '''\tif (magic != RH_DB_MAGIC || db_version != 1 || (flags & 1u)) {
\t\tprintf("UMI RPMH: unsupported/standalone cmd-db\\n");
\t\treturn EINVAL;
\t}
\treturn 0;
}'''
assert old_ret in src
src = src.replace(old_ret, old_ret.replace("\treturn 0;\n}", "\trh_dump_names();\n\treturn 0;\n}"), 1)
open(P, "w", encoding="utf-8").write(src)
print("patched v2 (bounded, once)")
for i, l in enumerate(src.split("\n"), 1):
    if "rh_dump_names" in l:
        print(f"  {i}: {l.strip()}")