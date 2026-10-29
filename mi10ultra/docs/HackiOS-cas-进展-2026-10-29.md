# HackiOS cas 进展 — 2026-10-29

> 承接 `HackiOS-cas-工程交接.md` 与 10-24 的「触摸 + GPU」验证。本文记录 10-24 → 10-29 的增量。

## 一、已验证新增

### 中文
- `ios165/select-system.py` 的 `KEEP_LANG` 加入 zh-Hans/zh-Hant/zh_CN/zh_TW/zh_HK/ja/ko。
- PingFang.ttc 不再被裁剪（`ios165/fetch.sh` 已注释 font_trim），完整 78MB 字体进镜像。
- 产物：`system-zh-full.apfs`（2,561,617,920 字节）。

### 完整外设
- WiFi QCA6390、蓝牙 H4、声音 ADSP、震动 AW8697、电池 BQ28Z610、充电 BQ25790 全部真机验证。
- 构建必须 `DEVICE=umi`（WiFi 固件门禁）、`BLUETOOTH=1`（蓝牙 job）。

### 数据持久化（var_store）
- `var_store=1 ufs_write=1`：会话结束把 /private/var 打包写入 darwin，重启恢复。
- 格式 v1（文件内容打包），双槽乒乓，槽 4 GiB ×2（8 GiB 保留区）。

## 二、UFS 大容量数据卷（128G，实验性）

### 方案
- darwin 改为 **/data 里的 128G 文件**（`darwin_file=1`，loader 按 FIEMAP extent 服务块读写），
  不再依赖 20G 分区（f2fs 不能缩，无法安全扩分区）。
- 内核新增 `hk_ufsvar.c`（块位图分配器，元数据仓 + 头 FNV 校验 + 几何自校验）。
- ramfs 页落盘：`ramfs.c` 加 `rn_blocks[]`/`rn_dirty[]`，一页 16K = 4 块（run 分配），
  `rm_max_pages` 改义为 256M 缓存。

### 结果
- 容量显示可达 128G（数据区 31,510,016 块 / 120G）。
- **问题**：同步页 I/O + 小缓存导致多数 App 读写时卡死。默认回退 `ufs_var=0`（内存数据卷 3G）。

### 文件
- `kernel/bsd/dev/hk_ufsvar.c`（新增）、`kernel/bsd/miscfs/ramfs/ramfs.c`、
  `kernel/bsd/dev/hk_ufs.c`、`kernel/osfmk/arm64/loader_return.c`、`loader/darwinfile.c`。

## 三、CS35L41 扬声器（未完成）

- 固件从本机 vendor（官改包 `super.zst` → vendor EROFS → `/firmware`）提取到 `fw-audio/cs35l41/`：
  `cs35l41-dsp1-spk-prot-revb2.wmfw` + `.bin` + `-RCV-.bin`（HALO v4.3 / WMDR v1，校验通过）。
- `hk_speaker.c`：固件 6 个 HALO 内存区已能加载并读回；卡在 DSP 算法头解析
  （`0x02800000` 读回全零，窗口寄存器 `0x02bc1000` 语义未定）。
- 注意增量编译陷阱：改 `hk_speaker.c` 后必须 `rm` 对应 `.o`，否则不重编。

## 四、其他

- 内核日志完整内容在手机 `logdump` 分区（`oem stop` 干净返回才写），`oem log` 只抓 loader 自己的行。
- 构建 cas-ufs2.img 必须用**顶层树** `/root/hackios/xnuloader/`（含 `platform_cas`），
  用 umi 仓库树会缺 `platform_cas` 导致真机卡死。
