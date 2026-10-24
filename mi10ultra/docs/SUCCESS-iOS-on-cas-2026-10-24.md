# 🎉 cas（小米10至尊纪念版）成功运行 iOS 16.5 —— 2026-10-24 15:13

## 结论：用户目标达成

**真机 `fastboot boot` → Project Silicium UEFI → XNULOADER → XNU → iOS 16.5 SpringBoard 主屏**，全部在 cas 上跑通。
证据：`evidence_ios_on_cas/ios5.png`（iOS 主屏截图）、`kernel_log_ios_on_cas.log`（402KB 内核日志）。

截图内容：状态栏 3:01 + 电池、天气/日历小组件、FaceTime/日历(24)/照片/邮件/备忘录/提醒/时钟/新闻/TV/播客/App Store/地图/健康/钱包/设置、底部 Dock（电话/Safari/信息/音乐）。

内核日志关键行：
```
== XNULOADER run (usb) ==
payload entry 'kernel' size e75ca8 / 'ramdisk' size 400000 / 'cmdline' size 5c
resident image at pa fc000000 size 948c0000: it is the root (md0), the payload ramdisk is md1
cmdline: debug=0x8 rd=md0 loader_return_after=600 ... bootcpu=7 cpus=7 umi_touch=1 umi_keys=1
memory 6144 MB, page size 16384, 7 cpu
... launchd: powerd / lsd / analyticsd / datamigrator 正常拉起
... (user/501) SpringBoard[7] 在请求服务
```

## 本次成功的确切配置（可复现）

| 环节 | 内容 |
|---|---|
| UEFI | `casPkg` = **umiPkg 模板**（MemoryMapLib/ConfigurationMapLib/BootManagerLib）+ **cas 自己 XBL 提取的 90 个驱动** + **TzDxeLA 补丁(0xd469=1)**；FD 0x9FC00000/0x300000 |
| boot.img | `tools/cas-ios-boot.img`（cas UEFI + **umi prebuilt loader** + cas.dtb）→ loader 上报 board=umi |
| 内核 | umi prebuilt `mach.development.bcm2837`（15.2MB） |
| root.apfs | 4,194,304 B（`PREBUILT=1 REST=1` 构建，软件渲染 GL 不设） |
| system.apfs | 2,492,198,912 B（608,447 × 4096） |
| 启动参数 | `debug=0x8 rd=md0 loader_return_after=600 sb_delay=8 bootcpu=7 cpus=7 umi_touch=1 umi_keys=1` |
| 传输 | WinUsb 常驻 daemon：`ramdisk` → `stage system.apfs`(2.5GB, ~90s) → `stage run-payload.bin`(19.4MB) → `run` |
| 状态 | `getvar result` → `OKAYrunning 0 0` |

## 关键坑与解法（本轮新增）

1. **TzDxeLA 必须打补丁**：cas XBL 的该驱动与 umi 补丁前哈希一致；不打则 `fastboot boot` 后 2 秒设备消失、6 秒回 fastboot。
2. **包结构必须与配置映射匹配**：alioth 结构的 ConfigurationMapLib（EnableUfsIOC=1 等）配 cas 驱动会失败；必须用取自 cas `uefiplat.cfg` 的 umi 模板配置值。
3. **PowerShell 5.1 脚本禁止中文**：UTF-8 无 BOM 会被当 ANSI 解析导致语法错误（已两次踩坑）。脚本一律纯 ASCII。
4. **命令通道竞态**：客户端不能用"匹配历史输出"来判断上一条命令完成（会提前发送导致命令被 daemon 删除）。应记录 out 文件行数差或让 daemon 回显序号。
5. **loader 的 `run` 必须在 payload 暂存完成后发送**；sent 后 `getvar result` 返回 `running` 即为内核在跑。

## 当前未做（下一步）

| 项 | 说明 |
|---|---|
| **触摸** | umi 驱动门控 `/chosen/hackios-board == "umi"`；本次 board=cas 故未启用。cas 触摸与 umi 同为 ST FTS、地址 0x49、复位 GPIO38 / IRQ GPIO39、SDA36/SCL37（cas DTB 同一组引脚）→ **改用 umi loader 启动（board=umi）或给内核加 cas 门控即可** |
| **GPU 硬件渲染** | 需 `fw-gpu`（a650 固件）+ `gpu=1`；本次软件渲染（与 umi 首次成功一致） |
| **Wi-Fi/音频/相机** | 需对应 `fw-*` 固件；本次 `fw/: 0 files` |
| **cas 一等公民化** | 在 umi 树里加 `platform_cas`（loader）+ 内核 cas 门控 + `cas_touch.c`（可选） |

## 运行窗口说明

本次 `loader_return_after=600`：内核运行 10 分钟后自动返回 loader（`oem run` 于 15:13:12，约 15:23 返回），返回后可再取一次完整的内核日志，或直接再次 `run` 重启 iOS。
