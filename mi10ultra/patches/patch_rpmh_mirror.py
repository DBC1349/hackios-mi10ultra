#!/usr/bin/env python3
# patch_rpmh_mirror.py — 让 umi_rpmh.c 的输出镜像到 loader 的 32KB 日志缓冲
import subprocess, sys, re
K = "/root/hackios/hackios-xnu-kernel-umi"
P = f"{K}/xnu/files/bsd/dev/umi_rpmh.c"
subprocess.run(["git", "-C", K, "checkout", "--", "xnu/files/bsd/dev/umi_rpmh.c"], check=True)
src = open(P, encoding="utf-8").read()

# 1) 在最后一个 #include 之后插入镜像版 printf
incs = list(re.finditer(r"^#include .*$", src, re.M))
assert incs, "no includes"
pos = incs[-1].end()
mirror = '''

/*
 * The loader's own log buffer is 32 KB, and only lines the kernel mirrors char by char land in
 * it (hk_adreno.c does the same). A driver later in the boot -- this RPMh path included -- is
 * otherwise outside that window, which is exactly where a board-specific failure shows up.
 */
extern void loader_log_putc(char c);

static int say(const char *format, ...) __printflike(1, 2);

static int
say(const char *format, ...)
{
\tchar line[400];
\tva_list list;

\tva_start(list, format);
\tvsnprintf(line, sizeof(line), format, list);
\tva_end(list);
\tfor (const char *c = line; *c != 0; c++) {
\t\tloader_log_putc(*c);
\t}
\treturn printf("%s", line);
}
#define printf say
'''
src = src[:pos] + mirror + src[pos:]

# 2) 加回受边界保护的 dump（+ 前置声明）
anchor = "static int\nrh_map(void)\n{"
assert anchor in src, "rh_map missing"
dump = '''/*
 * Diagnostic, once per boot: every resource name the command DB carries. cas (Mi 10 Ultra)
 * has a rail set that differs from UMI's ("smpf2 lookup matches=0; refusing"). Bounds are
 * checked exactly as rh_lookup() does.
 */
static void rh_dump_names(void);

'''
src = src.replace(anchor, dump + anchor, 1)

# 3) dump 函数体插到 rh_lookup 之前
anchor2 = "static int\nrh_lookup(const char *res, uint32_t *address)\n{"
assert anchor2 in src
body = '''static void
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
src = src.replace(anchor2, body + anchor2, 1)

# 4) 在 rh_map 成功返回前调用 dump
old_ret = '''\tif (magic != RH_DB_MAGIC || db_version != 1 || (flags & 1u)) {
\t\tprintf("UMI RPMH: unsupported/standalone cmd-db\\n");
\t\treturn EINVAL;
\t}
\treturn 0;
}'''
assert old_ret in src
src = src.replace(old_ret, old_ret.replace("\treturn 0;\n}", "\trh_dump_names();\n\treturn 0;\n}"), 1)

open(P, "w", encoding="utf-8").write(src)
print("patched umi_rpmh.c: mirror + bounded dump")
for i, l in enumerate(src.split("\n"), 1):
    if "loader_log_putc" in l or "rh_dump_names" in l:
        print(f"  {i}: {l.strip()}")