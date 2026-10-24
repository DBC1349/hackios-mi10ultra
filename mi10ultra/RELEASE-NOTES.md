# HackiOS on Xiaomi Mi 10 Ultra (cas) — 已验证可用版本

把 Apple **XNU 内核 + iOS 16.5 userland** 移植到**小米10至尊纪念版（cas / SM8250 / kona）**。
本 Release 是**真机验证跑通**的那一套（不是实验版）。

## 已验证（真机，2026-10-24）

| 项 | 结果 |
|---|---|
| iOS 16.5 启动到桌面 | ✅ SpringBoard 主屏、小组件、Dock 全部正常 |
| **触摸** | ✅ ST FTS521，`poll=11875 events=2758 errors=0`（零错误） |
| **GPU 硬件渲染** | ✅ Adreno 650 完整 bring-up（GMU/zap/SMMU/587MHz），CoreAnimation 走 `Mesa 25.0.7 FD650` |
| 全程不写分区 | ✅ 纯 `fastboot boot` 内存启动 |

## 资产

| 文件 | 说明 |
|---|---|
| `cas-touch2.img` | **启动镜像**：cas UEFI（自建，含 cas 自身 XBL 提取的驱动）+ 我们的 loader（已把板卡标识设为 `umi`，触摸门控放行） |
| `cas-umiloader.img` | 备用启动镜像：cas UEFI + umi 官方 loader |
| `run-payload-gpu-wifi.bin` | **内核 payload**（本版本验证对象：kernel + root.apfs + 启动参数 `gpu=1 wifi=1 umi_touch=1 umi_keys=1`） |
| `mach.development.bcm2837.verified` | 从上面 payload 中提取的**内核本体**（15.2MB） |
| `root.apfs` | 小镜像（含 Mesa GL 库与 `fw/` 固件） |
| `libOSMesa.8.dylib` / `libhackios-gl.dylib` | Mesa 25.0.7（freedreno FD650 + softpipe）GL 实现 |
| `firmware/fw-gpu/` | A650 固件（`a650_gmu.bin`/`a650_sqe.fw`/`a650_zap.mdt`+`b00..b02`） |
| `firmware/fw-wifi/qca6390/` | QCA6390 WiFi 固件（`amss20.bin` 等 54 个） |
| `firmware/fw-haptic/` | AW8697 触觉固件 |

> 需要 2.4GB 的 `system.apfs`（系统镜像）：用 userland 仓库 `build.sh` 重建，或使用对应工作区的产物。

## 怎么用

```sh
# 1) 进 bootloader，内存启动 UEFI+loader（不写任何分区）
adb reboot bootloader
fastboot boot cas-touch2.img

# 2) 用 WinUsb 常驻 daemon（见源码包 scripts/windows/）
#    依次发送：ramdisk → stage <system.apfs> → stage run-payload-gpu-wifi.bin → run

# 3) 检查
#    getvar result 应回 OKAYrunning 0 0；触摸可用；界面由 GPU 渲染
```

## 关键前提（缺一不可）

1. 手机的 `TzDxeLA.efi` 必须已打补丁（`0xd469`: 0→1），否则 UEFI 启动 2 秒即崩回 fastboot
2. 内核必须**自行编译**（仓库里的 prebuilt 不含 umi 触摸驱动）
3. loader 必须传 `/chosen/hackios-board = "umi"`（触摸/按键等驱动的门控条件）
4. USB 接 **USB 3.0 口**（USB 2.0 口会导致 loader 枚举失败）
5. 构建 userland 时必须 `DEVICE=umi`，否则 WiFi 固件不会进镜像

## 已知未完成

- **WiFi**：QCA6390 PCIe 枚举失败（`errno=0x13`）。根因已定位：cas 的 RPMh cmd-db 中不存在驱动所需的 `smpf2` 电源轨（`smpa4`/`smpa6` 存在），需按 cas 实际轨名修改 `xnu/files/bsd/dev/umi_rpmh.c` 的 `rails[]`
- 显示：当前 TE 60.3Hz + 偶发长帧
- 音频/触觉/Metal：驱动与固件就位，未启用

## 来源与致谢

- 移植方法与驱动：**hackios / Project Silicium** 生态，以及用户提供的
  `JopangJovan/hackios-xnu-kernel-umi`、`hackios-xnu-userland-umi`（真机验证：小米10）
- cas 专属资料：全部从本机手机自身提取（DTB、XBL 驱动、UEFI 固件、A650/QCA6390/ADSP/AW8697 固件）

> 仅供研究自用。iOS 系统文件与厂商固件版权归各自所有者，请勿再分发。