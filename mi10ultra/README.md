# HackiOS on Xiaomi Mi 10 Ultra (cas) — 项目交付包

**目标**：把 Apple XNU 内核 + iOS 16.5 userland 移植到小米10至尊纪念版（cas / SM8250 / kona）。
**当前状态**：✅ **iOS 16.5 已能在真机跑到桌面，触摸可用，GPU（Adreno 650）硬件渲染已打通**。WiFi 未通（已定位到具体原因）。

---

## 1. 交付物清单

### 包 A：`HackiOS-cas-源码与补丁.zip`
| 路径 | 内容 |
|---|---|
| `sources/kernel-umi.bundle` | umi 内核仓库的完整 git bundle（含最新提交 `6264ebf`，WiFi/PCIe/MHI/GPU/触摸） |
| `sources/userland-umi.bundle` | umi userland 仓库 bundle（`ee787f6`） |
| `sources/our-changes/` | 我们相对仓库的全部改动（diff + 新增文件） |
| `sources/loader/` | **我们自己的 loader**（老 hackios 树，已加 `platform_umi` 别名 + `hackios-board` 属性）与打包脚本 |
| `sources/cas-device/cas.conf` | cas 设备配置（DTB/FD_BASE/PAYLOAD_BASE/FB_BASE） |
| `patches/` | 本轮所有补丁脚本与说明（loader 别名、hackios-board、RPMH dump/镜像、GPU terse、build.sh 的 lld 链接器） |
| `scripts/windows/` | WinUsb 直连 loader 的常驻 daemon 与一键运行脚本（不依赖 fastboot.exe/usbipd） |
| `scripts/build/` | 工具链、Mesa、userland、内核的完整构建脚本 |
| `docs/` | 交接文档 + 本轮 4 份进度/成功报告 |

### 包 B：`HackiOS-cas-产物与固件.zip`
| 路径 | 内容 |
|---|---|
| `build/mach.development.bcm2837` | **已编译内核**（15.2MB；含 触摸 + Adreno 650 GPU + WiFi/PCIe 驱动 + 日志镜像） |
| `build/boot-images/*.img` | 可直接 `fastboot boot` 的镜像（cas UEFI + loader） |
| `build/payloads/run-payload-mirror.bin` | **可直接送内核的 payload**（kernel + root.apfs，启动参数含 `gpu=1 wifi=1 umi_touch=1`） |
| `build/mesa/` | `libOSMesa.8.dylib`（14.8MB，Mesa 25.0.7 `FD650` freedreno）+ `libhackios-gl.dylib` |
| `build/root.apfs` | 小镜像（19MB；含 GL 库 + `fw/` 固件） |
| `firmware/fw-gpu/` | 从手机提取的 A650 固件：`a650_gmu.bin`、`a650_sqe.fw`、`a650_zap.mdt`+`b00/b01/b02` |
| `firmware/fw-wifi/qca6390/` | QCA6390 WiFi 固件（`amss20.bin` 4.35MB、`m3.bin`、`bd_j1gl.elf`、`regdb.bin` 等 54 个） |
| `firmware/fw-haptic/` | `aw8697_haptic.bin`、`aw8697_rtp_1.bin` |
| `firmware/adsp/` | ADSP 音频 DSP 固件（`adsp.mdt` + `b00..b18`，约 19MB） |

> `system.apfs`（2.4GB 系统镜像）未包含；用 `userland/build.sh` 可重建（见 §5）。

---

## 2. 硬件事实（实测，非推测）

| 项 | cas（小米10至尊） |
|---|---|
| SoC | SM8250 / kona，8 核 |
| 内存 | 6GB，DDR 空洞 `0xBBB00000..0xC0000000`（69MB） |
| 显示 | 1080×2340；framebuffer `0x9C000000`；**当前 TE 60.316Hz**（命令模式，`INTF1 @0xAE6B800`） |
| 触摸 | ST FTS521 `3639`，I2C 地址 `0x49`，SDA GPIO36 / SCL GPIO37 / RESET GPIO38 / IRQ GPIO39；供电 PM8150A L1 1800mV + PM8150B GPIO5 |
| WiFi | QCA6390（`17cb:1101`）PCIe0 |
| 触觉 | AW8697（`aw8697_haptic.bin`） |
| UEFI 内存布局 | FD `0x9FC00000`/`0x300000`；PAYLOAD `0xF0000000`/64MB；Display Reserved `0x9C000000`/36MB |

**关键结论**：cas 与 umi（小米10）**同 SoC、同面板分辨率、同触摸控制器与引脚、同 framebuffer/内存布局**——因此 umi 的驱动与配置可直接用于 cas。

---

## 3. 怎么跑起来（真机）

前提：手机已解锁 bootloader（`verifiedbootstate=orange`），USB 接在 **USB 3.0 口**（USB 2.0 口会导致 loader 枚举失败）。

```powershell
# 1) 进 bootloader 并启动 cas 的 UEFI+loader（内存启动，不写任何分区）
adb reboot bootloader
fastboot boot boot-images/cas-touch2.img

# 2) 常驻 daemon（WinUsb 直连，保持句柄不关；关闭句柄会让 loader 变聋）
powershell -File scripts/windows/winfbd3.ps1

# 3) 依次：准备常驻系统镜像 → 送 2.4GB 系统镜像 → 送内核 payload → 启动
#    这些通过 winfb_cmd.txt 单行指令驱动 daemon：
#    ramdisk
#    stage <system.apfs 路径>
#    stage <payload 路径>
#    run
#    然后 getvar result 应回 "OKAYrunning 0 0"

# 或直接用一键脚本：
powershell -File scripts/windows/boot_gpu_wifi.ps1 `
  -Payload <payload 路径> -SysImg <system.apfs 路径>
```

要点：
- **payload 里的 cmdline**：`loader_return_after=900 sb_delay=8 bootcpu=7 cpus=7 umi_touch=1 umi_keys=1 gpu=1 wifi=1 fb_scan=1 cpufreq=1`
  （`bootcpu=7 cpus=7` 让 loader 自己保留一个核跑 USB，运行中可继续取日志/截图；`loader_return_after` 到点自动回到 loader）
- **运行中如果 `err=203`**：不是 USB 坏了，而是 daemon 句柄失效——重启 daemon 即可继续通信
- **GPU 跑过一轮后**：自建 loader 缺少 umi 新 loader 的"归还 GPU"复位逻辑，第二次 bring-up 会卡在 GPU step 20 → 需要冷启动（长按电源键）或改用 `cas-umiloader.img`

---

## 4. 已验证的关键修复（踩坑记录，都可复现）

| # | 问题 | 修复 |
|---|---|---|
| 1 | cas 的 `TzDxeLA.efi` 未打补丁 → UEFI 启动 2 秒后设备消失、6 秒回 fastboot | 打补丁：`0xd469` 处 0→1（`patches/patch_tz.py`） |
| 2 | 包结构与配置映射不匹配（用 alioth 的 ConfigurationMapLib 配 cas 驱动必失败） | 采用 umi 模板包结构 + cas 自己的驱动 + cas 的 `uefiplat.cfg` 配置值 |
| 3 | **内核编译后不生效**（`git reset` 后编出来还是旧内核） | `xnu-src/` 不在 git 内：须 `git checkout -- .` → 重新 `git apply hackios.patch` → `cp -a xnu/files/. xnu-src/` |
| 4 | 触摸不工作 | ① 内核必须含 umi 触摸驱动（仓库 prebuilt **不含**，必须自行编译）② loader 必须传 `/chosen/hackios-board = "umi"`（自建 loader 原无此属性，已加） |
| 5 | Mesa 编译失败（`library not found for -lSystem`） | 自建 cctools ld64 未开 `TAPI_SUPPORT`，读不了 iOS SDK 的 `.tbd`；改用 **LLVM `ld64.lld`**（`patches/lld-clang-wrapper`） |
| 6 | WiFi 固件不进镜像 | `build.sh` 的 `umi_firmware()` 门控 `DEVICE=umi` —— 构建时必须设 `DEVICE=umi` |
| 7 | 动态库链接失败（userland 全量编译） | `build.sh` 的 `DYN` 改用 lld 包装（静态程序仍用 cctools） |
| 8 | 日志被截断在 32KB（看不到后段驱动的输出） | loader 的 `log_buf` 仅 32KB 且只收"镜像写入"的行；在驱动里照 `hk_adreno.c` 的 `say()` 模式逐字符 `loader_log_putc`（`patches/patch_rpmh_mirror.py`） |

---

## 5. 怎么重建（完整构建链）

```sh
# --- 工具链（一次性）---
git clone --depth 1 --recursive https://github.com/theos/theos.git ~/theos
# iPhoneOS16.5.sdk：从 github.com/theos/sdks 稀疏检出（master 分支）+ 解包到 ~/theos/sdks/
# cctools(ld64)：git clone https://github.com/tpoechtrager/cctools-port && configure/make/install
#   → 安装到 ~/theos/toolchain/linux/iphone/bin（并建 ld64/ld 无前缀符号链接）
# 依赖：git curl tar xz make clang flex bison m4 perl python3 autoconf automake
#        uuid-dev libcrypt-dev libblocksruntime-dev libyaml-dev libdispatch（源码编译）
#        meson ninja python3-mako cmake

# --- XNU 源码 + 我们的工作 ---
cd hackios-xnu-kernel-umi
bash xnu/apply.sh                     # 取 Apple xnu-8796.121.2 + 打 hackios.patch + 铺 files/
JOBS=$(nproc) bash xnu-deps/setup.sh  # host 工具(mig/…) + xnu SDK 组装（含 availability.pl）
THEOS=$HOME/theos ./xnu-build.sh -j$(nproc) -k
# 产出：xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837

# --- Mesa（GL=1 的 OpenGL ES 实现，FD650/freedreno + softpipe）---
sed -i 's|^CC=.*xnu-clang$|CC=/root/hackios/lldbin/xnu-clang|' tools/mk-mesa.sh
bash tools/mk-mesa.sh                 # 产出 xnu-deps/build/mesa/out/libOSMesa.8.dylib

# --- userland（GL=1 + WiFi 固件）---
cd userland
sed -i 's|^DYN="xnu-clang |DYN="/root/hackios/lldbin/xnu-clang |' build.sh
env DEVICE=umi GL=1 WIFID=1 REST=1 ADSP= RC_JOBS=rc.jobs.umi-boot bash build.sh
# 产出 root.apfs（小镜像，含 GL + fw/）与 system.apfs（2.4GB 系统镜像）

# --- payload ---
python3 xnuloader/mkpayload.py -o out/run-payload.bin \
  kernel=<内核> ramdisk=userland/root.apfs \
  --cmdline "loader_return_after=900 sb_delay=8 bootcpu=7 cpus=7 umi_touch=1 umi_keys=1 gpu=1 wifi=1 fb_scan=1 cpufreq=1"
```

---

## 6. 当前进展与未完成项

| 项 | 状态 | 证据 |
|---|---|---|
| iOS 16.5 到桌面 | ✅ | `docs/SUCCESS-iOS-on-cas-2026-10-24.md` |
| 触摸 | ✅ 零错误 | `UMI FTS: poll=11875 events=2758 errors=0` |
| **GPU 硬件渲染** | ✅ | `adreno: THE GPU STAYS ON…`、`Mesa 25.0.7; FD650`、`accelerated renderer`、587MHz |
| 显示流畅度 | ⚠️ 60.3Hz TE + 偶发长帧（93ms 一次） | `display: UMI command TE: 60.316 Hz` |
| WiFi | ❌ | `UMI RPMH: smpf2 lookup matches=0; refusing` → `UMI PCIe: enumeration failed errno=0x13`；**根因：cas 的 RPMh cmd-db 里没有 `smpf2` 电源轨**（smpa4/smpa6 有），需按 cas 实际轨名改映射 |
| 音频 ADSP / 触觉 AW8697 / Metal | ⏳ 固件与驱动均已就位，未启用 | `firmware/` |

**WiFi 的下一步**：用 `patches/patch_rpmh_mirror.py` 编译的内核（日志镜像，能把 cmd-db 全部轨名写进 32KB 窗口）跑一轮，读出 cas 的轨名列表，然后修改 `xnu/files/bsd/dev/umi_rpmh.c` 里的 `rails[]` 表（当前为 umi 的 `smpa4/smpa6/smpf2/smpa5/smpc8/ldoa9/ldoa5`）。

---

## 7. 来源与致谢

- 上游移植方法、驱动与工具链：**hackios / Project Silicium** 生态
- 本机所用仓库（用户提供）：
  - `https://github.com/JopangJovan/hackios-xnu-kernel-umi`
  - `https://github.com/JopangJovan/hackios-xnu-userland-umi`
  - `https://github.com/JopangJovan/ios-sm8250`（另一条路线的参照）
- cas 专属资料：本机通过 `fastboot`/`adb`/recovery 从手机自身提取（DTB、XBL 驱动、UEFI 固件、A650/QCA6390/ADSP/AW8697 固件）

> 本包仅供研究自用；iOS 系统文件与厂商固件版权归各自所有者，请勿再分发。