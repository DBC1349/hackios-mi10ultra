#!/usr/bin/env python3
# patch_early_dump2.py — 修正版：名字用两个对齐的 32 位读拼出（Device 内存不允许非对齐访问）
import subprocess, sys, re
K = "/root/hackios/hackios-xnu-kernel-umi"
R = f"{K}/xnu/files/bsd/dev/umi_rpmh.c"
A = f"{K}/xnu/files/bsd/dev/hk_adreno.c"

subprocess.run(["git", "-C", K, "checkout", "--", "xnu/files/bsd/dev/umi_rpmh.c"], check=True)
src = open(R, encoding="utf-8").read()

incs = list(re.finditer(r"^#include .*$", src, re.M))
pos = incs[-1].end()
mirror = '''

/* Lines said here also go to the loader's 32 KB buffer (as hk_adreno.c does). */
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

anchor2 = "static int\nrh_lookup(const char *res, uint32_t *address)\n{"
assert anchor2 in src
body = '''/*
 * One dump of the command DB, once per boot. Names are read as two ALIGNED 32-bit words:
 * the DB lives in Device memory and an unaligned load there faults (this cost a boot).
 */
static void rh_dump_names(void);

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
\t\tif (count > 256 || slave < 3 || slave > 5 || (entries & 3u) ||
\t\t    !rh_range(entries, count * RH_DB_ENTRY) || !rh_range(aux, 0)) {
\t\t\tprintf("UMI RPMH: dump: malformed slave header %u (slave=%u entries=%#x count=%u)\\n",
\t\t\t    i, slave, entries, count);
\t\t\tcontinue;
\t\t}
\t\tprintf("UMI RPMH: dump: slave %u has %u entries\\n", slave, count);
\t\tfor (unsigned j = 0; j < count; ++j) {
\t\t\tuint32_t e = entries + RH_DB_ENTRY * j;
\t\t\tuint32_t w0, w1, addr;
\t\t\tchar name[9];
\t\t\tif (!rh_range(e, RH_DB_ENTRY)) break;
\t\t\tw0 = rh_read(rh_db, e);
\t\t\tw1 = rh_read(rh_db, e + 4);
\t\t\taddr = rh_read(rh_db, e + 16);
\t\t\tfor (unsigned k = 0; k < 4; ++k) {
\t\t\t\tname[k] = (char)((w0 >> (8u * k)) & 0xffu);
\t\t\t\tname[4 + k] = (char)((w1 >> (8u * k)) & 0xffu);
\t\t\t}
\t\t\tname[8] = 0;
\t\t\tprintf("UMI RPMH: rail \\"%s\\" addr=%#x\\n", name, addr);
\t\t}
\t}
}

/*
 * Called from the GPU driver's entry, the first thing that logs in a boot: the rails then sit
 * at the very start of the log, inside the loader's 32 KB window.
 */
void
umi_rpmh_dump_early(void)
{
\tif (rh_map() == 0) {
\t\trh_dump_names();
\t}
}

'''
src = src.replace(anchor2, body + anchor2, 1)
anchor_map = "static int\nrh_map(void)\n{"
assert anchor_map in src
src = src.replace(anchor_map, "static void rh_dump_names(void);\n\n" + anchor_map, 1)
open(R, "w", encoding="utf-8").write(src)
print("umi_rpmh.c patched (aligned reads)")

subprocess.run(["git", "-C", K, "checkout", "--", "xnu/files/bsd/dev/hk_adreno.c"], check=True)
a = open(A, encoding="utf-8").read()
m = re.search(r"^hk_adreno_init\(void\)\n\{\n", a, re.M)
assert m
ins = "{\n\t/* The RPMh rails first: they land at the top of the log, inside the loader's window. */\n\textern void umi_rpmh_dump_early(void);\n\tumi_rpmh_dump_early();\n"
a = a[:m.end()] + ins + a[m.end():]
open(A, "w", encoding="utf-8").write(a)
print("hk_adreno.c patched")