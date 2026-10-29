# HackiOS on 小米 10 至尊纪念版

**在骁龙 865 安卓手机上，跑起 Apple 原封未动的 iOS 16.5 用户态。**

本项目将开源的 **XNU 内核**（`xnu-8796.121.2`，即 iOS 16.5 所搭载的内核）移植到
**小米 10 至尊纪念版（`cas`，SM8250 / 骁龙 865）**，并在其上运行**未经任何改动的 iOS 16.5 用户态**——
`launchd → logd → backboardd → SpringBoard → 应用`，与 Apple 出厂时逐字节一致。

<p align="center">
  <img src="docs/cas-homescreen.gif" width="250" alt="iOS 16.5 主屏在小米10至尊纪念版上运行（动画）">
  <img src="docs/cas-about-128g.jpg" width="250" alt="iOS 16.5 关于本机：129 GB / 120.37 GB 可用">
  <img src="docs/cas-bluetooth.jpg" width="250" alt="iOS 蓝牙设置在骁龙 865 上正常可用">
</p>

<p align="center">
  <em>从左到右：小米 10 至尊纪念版上带手势的 iOS 16.5 主屏（可看
  <a href="docs/cas-homescreen.mp4">完整视频</a>）· “关于本机”显示 129&nbsp;GB UFS 数据卷 ·
  蓝牙设置，可被发现且功能正常。</em>
</p>

---

## 这是什么

这是一个**系统级的移植项目**：把 Apple 整套板级支持层——启动固件、平台驱动、GPU、Wi-Fi、存储、电源——
全部用自研实现替换掉，而 iOS 的程序、框架和 `dyld` 共享缓存**一个字节都不改**。全程不刷机，
手机通过 USB 从内存启动，重启后回到安卓。

| | |
|---|---|
| **目标机型** | 小米 10 至尊纪念版（`cas`），高通 SM8250（骁龙 865，8 核），6 GB 内存，1080×2340 AMOLED |
| **客户机 OS** | iOS 16.5（未改动用户态），XNU `xnu-8796.121.2` |
| **GPU** | Adreno 650，走 Mesa 25.0.7（`freedreno` FD650），CoreAnimation 用 OpenGL ES 渲染 |
| **存储** | UFS——实验性的 **128 GiB 数据卷**，由自研块分配器支撑 |
| **网络** | QCA6390 Wi-Fi / 蓝牙（`ath11k` 经 LinuxKPI） |
| **启动链** | `fastboot boot` → UEFI（Project Silicium）→ 自研 EFI loader → XNU |

## 亮点

- **在异构芯片上跑未改动的 iOS。** iOS 的程序、框架和 `dyld` 共享缓存未改一个字节。Apple 闭源驱动原本
  做的事，要么在内核里重新实现，要么由内核在加载时注入的库来提供（`DYLD_INSERT_LIBRARIES`）。
- **Adreno 650 完整 bring-up。** GMU / zap / SMMU 固件加载、587 MHz 时钟，加上 Mesa `freedreno` FD650 驱动，
  让 SpringBoard 的 CoreAnimation 真正走 GPU 的 OpenGL ES 渲染——不是软光栅。
- **中文端到端支持。** 完整语言包 + 未裁剪的全量（78 MB）苹方字体注入系统镜像，设置、主屏、自带应用
  均正确显示中文。
- **真实的数据持久化。** `/private/var` 关机时打包到 UFS 承载的 Darwin 卷、开机恢复
  （`var_store=1 ufs_write=1`）——照片和设置重启不丢。
- **128 GiB UFS 数据卷。** 自研块分配器（位图式、跨会话持久）向内核呈现 128 GiB 卷，“关于本机”如实显示
  完整容量（如上图）。
- **不分发任何厂商固件。** 所有二进制 blob（GPU、Wi-Fi、音频 DSP、马达）都在装机时从手机自带的安卓里
  提取；iOS 系统镜像在你的机器上从 Apple 官方 IPSW 生成。

## 真机已验证

| 功能 | 状态 |
|---|---|
| iOS 16.5 → SpringBoard 主屏、小组件、Dock | ✅ |
| 触摸屏（ST FTS521）、多点触控、手势、电源/音量键 | ✅ 全程 0 错误 |
| GPU 硬件渲染（Adreno 650，Mesa FD650） | ✅ |
| 系统 UI 中文 + 完整苹方字体 | ✅ |
| Wi-Fi / 蓝牙（QCA6390） | ✅ |
| 音频 DSP（ADSP）、马达（AW8697）、电池与充电（BQ28Z610 + BQ25790） | ✅ |
| 重启后数据持久化 | ✅ |
| 128 GiB UFS 数据卷 | ⚠️ 实验性（见 [mi10ultra](mi10ultra/README.md)） |

## 架构

```
fastboot boot ──► UEFI (Project Silicium) ──► xnuloader（自研 EFI 应用）
                                                │  常驻：USB 链接（fastboot 协议）、
                                                │  看门狗、接收内核日志
                                                ▼
        XNU 内核（BCM2837 参考配置 + 我们的平台层）
           ├─ GICv3、定时器、视频控制台、返回 loader 的交接
           ├─ APFS（镜像在内存、iOS 根卷在 UFS）、ramfs（/private/var）
           ├─ minicrypto 替代 corecrypto；AMFI & Sandbox 桩（MAC policy）
           ├─ 驱动替换：IOSurface、IOMobileFramebuffer、AppleKeyStore、网络栈
           ├─ GPU：Adreno 650 · Wi-Fi：QCA6390（ath11k / LinuxKPI）· 声音：ADSP · 输入：FTS521
           ▼
        iOS 16.5 launchd → backboardd → SpringBoard → 应用
```

## 我的贡献（小米 10 至尊版移植）

本仓库基于 **HackiOS / Project Silicium** 生态（见[致谢](#致谢)）。其中 `cas`（小米 10 至尊版）移植为原创工作：

- **设备配置与平台层**——`mi10ultra/cas-device/`、`common/devices/cas.conf`：DTB 解析、UEFI 内存布局
  （`FD 0x9FC00000`、`PAYLOAD 0xF0000000`），以及 `platform_cas` loader 目标。
- **自研 EFI loader**——`mi10ultra/loader/`，含 `platform_cas`、大 extent 的 `darwin_file` 传输，以及
  `hackios-board` 设备属性。
- **内核驱动**——`hk_*.c` / `umi_*.c`：Adreno 650 GPU、QCA6390、ADSP 音频、`hk_speaker.c`（CS35L41）、
  触摸、电源，以及一套 **UFS 块分配器**（`hk_ufsvar.c`，128 GiB 几何，16 KiB 页）。
- **工具链与构建**——在 Linux 上交叉编译 XNU（cctools、LLVM `ld64.lld`、Mesa `freedreno`），外加 Windows
  的 WinUSB loader 常驻守护进程与一键启动脚本。
- **逆向工程**——`TzDxeLA.efi` 补丁、RPMh `cmd-db` 电源轨映射，以及从手机自带厂商镜像提取固件。

完整可复现的记录见 [`mi10ultra/README.md`](mi10ultra/README.md) 与
[`mi10ultra/RELEASE-NOTES.md`](mi10ultra/RELEASE-NOTES.md)。

## 目录结构

| 路径 | 内容 |
|---|---|
| `mi10ultra/` | `cas` 移植：设备配置、loader、内核驱动、补丁、脚本、文档 |
| `xnuloader/` | EFI loader（`loader.c`、`mkpayload.py`） |
| `xnu/` | XNU 补丁与新增文件（`apply.sh` / `export.sh`） |
| `xnu-deps/`、`xnu-build.sh`、`xnu-env.sh` | 在 Linux 上构建 XNU |
| `uefi/`、`mkxnuimg.py` | UEFI 移植与 `fastboot boot` 镜像打包 |
| `wifi/lx/` | 让 Linux Wi-Fi 驱动能针对本内核编译的兼容层 |
| `qemu/` | QEMU 运行、触摸输入、截图、栈符号化 |
| `tools/` | 录制、UFS/闪存工具、固件提取、主机测试 |
| `common/` | 工作区配置、按设备的构建/运行、设备配置 |
| `prebuilt/`、`docs/` | 构建产物与文档 |

## 快速开始

最快的方式是在 **QEMU** 里跑（无需真机、无需编译内核）——见 [`common/run.sh`](common/run.sh) 的上游流程。
对于**小米 10 至尊纪念版**，请按 [`mi10ultra/README.md`](mi10ultra/README.md) 操作；所需的 `system.apfs`
（2.4 GB，中文版）在你的机器上从 Apple IPSW 生成，因此不在此提交（见
[releases](https://github.com/DBC1349/hackios-mi10ultra/releases)）。

## Releases

预编译内核、loader payload 与 root 镜像已附在
[releases](https://github.com/DBC1349/hackios-mi10ultra/releases) 页面。系统镜像需从 Apple 官方 CDN 本地生成。

## 致谢

基于 **HackiOS / Project Silicium** 生态：上游
[hackios-xnu-kernel](https://github.com/oosexgang/hackios-xnu-kernel) 与
[hackios-xnu-userland](https://github.com/oosexgang/hackios-xnu-userland) 仓库，以及 umi 线移植
[`JopangJovan/hackios-xnu-kernel-umi`](https://github.com/JopangJovan/hackios-xnu-kernel-umi) 与
`hackios-xnu-userland-umi`（小米 10 真机验证）。`mi10ultra/` 中的 `cas` 专属工作为原创。

## 许可证

仓库内不包含任何 Apple 或厂商文件。`xnu/` 是 XNU 的衍生作品，按 APSL 2.0 分发；本仓库以
[Apple Public Source License 2.0](LICENSE) 发布。第三方组件（ath11k、FreeBSD LinuxKPI、libzbitmap、
libdeflate、UEFI 驱动）保留各自许可证——见 [prebuilt/NOTICES.md](prebuilt/NOTICES.md)。

> iOS、iPhone、Apple 是 Apple Inc. 的商标。本项目为独立研究项目，与 Apple 无隶属、授权或背书关系。
