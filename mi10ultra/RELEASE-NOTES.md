# HackiOS on Xiaomi Mi 10 Ultra (cas) — 最新进展版

把 Apple **XNU 内核 + iOS 16.5 userland** 移植到**小米10至尊纪念版（cas / SM8250 / kona）**。
本 Release 是 `Xiaomi-mi10ultra` 分支的最新快照，在 2026-10-24「触摸 + GPU」版本基础上新增了
**中文、完整外设驱动、数据持久化、UFS 大容量数据卷（实验性）**。

## 已验证（真机）

| 项 | 结果 |
|---|---|
| iOS 16.5 启动到桌面 | ✅ SpringBoard 主屏、小组件、Dock 正常 |
| 触摸 | ✅ ST FTS521（2758 事件、0 错误） |
| GPU 硬件渲染 | ✅ Adreno 650 完整 bring-up（GMU/zap/SMMU/587MHz），CoreAnimation 走 Mesa 25.0.7 FD650 |
| **中文** | ✅ 语言包 + 完整 PingFang 字体（78MB 未裁剪，5600 条 zh 条目）进 system 镜像 |
| WiFi / 蓝牙 | ✅ QCA6390、H4 TLV 固件加载、station ready |
| 声音 / 震动 / 电池 / 充电 | ✅ ADSP、AW8697、BQ28Z610 + BQ25790 充电控制 |
| **数据持久化** | ✅ `var_store=1 ufs_write=1`：/private/var 关机打包到 darwin，重启恢复（照片不丢） |
| 全程不写分区 | ✅ 纯 `fastboot boot` 内存启动 |

## 实验性 / 未完成

| 项 | 状态 |
|---|---|
| **UFS 大容量数据卷（128G）** | ⚠️ 已打通 `darwin_file=1` 的 128G 文件承载 + `ufs_var` 页落盘，容量可显示 128G；**但同步页 I/O 导致多数 App 卡死**，默认回退到内存数据卷（3G）。代码都在，待优化缓存/异步写回 |
| **CS35L41 扬声器** | ⚠️ 固件已从本机 vendor 提取（`fw-audio/cs35l41/`，HALO v4.3 / WMDR v1 校验通过），`hk_speaker.c` 驱动已就位，但 DSP 算法头解析未调通，输出保持禁用 |
| 独立启动（flash_root） | ❌ `common/flash/` 工具在树里，未启用 |

## 目录（分支 `Xiaomi-mi10ultra`）

```
mi10ultra/
  README.md            本移植指南
  RELEASE-NOTES.md     本文件
  cas-device/cas.conf  cas 设备配置
  loader/              我们自己的 EFI loader（platform_cas + darwin_file + mkpayload）
  kernel/              cas 内核驱动（hk_*.c / umi_*.c / ramfs.c / loader_return.c ...），
                       覆盖到 hackios-xnu-kernel 的 xnu/files/ 即可编译
  userland/            var_store.c / init.c / mini.* / varstore plist（覆盖到 hackios-xnu-userland）
  patches/             TzDxeLA 补丁、RPMh cmd-db dump、GPU terse、lld 包装等修复脚本
  scripts/             WinUsb daemon 与构建链脚本
  docs/                全部移植报告
```

## 关键前提（缺一不可）

1. 手机 `TzDxeLA.efi` 必须打补丁（`0xd469`: 0→1），否则 UEFI 启动 2 秒崩回 fastboot
2. loader 必须传 `/chosen/hackios-board = "umi"`（触摸/按键等驱动门控）
3. USB 接 **USB 3.0 口**（USB 2.0 会导致 loader 枚举失败）
4. 构建 userland 必须 `DEVICE=umi`（否则 WiFi 固件不入镜像）

## 来源与致谢

- 移植方法与驱动：hackios / Project Silicium 生态，`JopangJovan/hackios-xnu-kernel-umi`、
  `hackios-xnu-userland-umi`（真机验证：小米10）
- cas 专属资料：全部从本机手机自身提取（DTB、XBL 驱动、UEFI 固件、A650/QCA6390/ADSP/AW8697/CS35L41 固件）

> 仅供研究自用。iOS 系统文件与厂商固件版权归各自所有者，请勿再分发。
