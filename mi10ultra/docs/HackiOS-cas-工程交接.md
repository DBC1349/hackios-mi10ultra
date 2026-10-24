# HackiOS → 小米10至尊纪念版（cas / SM8250）移植工程 · 完整交接文档

> 生成时间：2026-10-23 22:30（UTC+8）
> 用途：给 DeepSeek（或任何后续执行者）完整接管本工程的上下文。所有路径、命令、地址均为实测值，可直接复现。
> 主线结论：这是一个**设备移植工程**（不是刷机包安装）。当前已完成全部准备 + QEMU 校准 + casPkg UEFI 骨架搭建，**尚未产出首个 cas boot.img**，也未上真机。

---

## 0. 一句话现状

官方项目 hackios-xnu-kernel 只支持 POCO X5 Pro 5G（redwood，SM7325）与 QEMU。用户手机是小米10至尊纪念版（cas，M2007J1SC，骁龙865/SM8250/kona），目标："能开机进 iOS 16 桌面即可，不追求驱动"。
当前：环境 ✅、真机素材 ✅、QEMU 校准（模拟器跑起 iOS 16.5 苹果 Logo）✅、cas 设备档案 ✅、casPkg UEFI 骨架 ✅、**Binaries/cas 驱动为 redwood 占位（待从 cas xbl 提取/替换）**、首个真机镜像 ❌。

---

## 1. 项目来源与定性

- 仓库：https://github.com/oosexgang/hackios-xnu-kernel（@8e61187）+ hackios-xnu-userland（@a79ff07）
- 原理：苹果开源 XNU 内核（xnu-8796.121.2，iOS 16.5 内核）+ 未修改的 iOS 16.5 用户态（IPSW 素材打包成 APFS）跑在安卓硬件上；引导链 = Mu-Silicium UEFI（Project Silicium）→ hackios loader（xnuloader）→ XNU → iOS 用户态。
- 官方设备：redwood（SM7325/lito，KodiakPkg/lisa 同 SoC 包）、qemu-virt。
- 官方移植流程文档：`WSL:/root/hackios/docs/PORTING.md`（74 行，已通读，见 §8）。

## 2. 关键事实核查（此前 DeepSeek 论断的核实结果）

| DeepSeek 说法 | 核实结果 |
|---|---|
| "cas 无现成包，以天计的移植工程" | ✅ 方向对（无现成产物），时间过于乐观——实为数周量级研究工程 |
| "同 SoC 先例 venusPkg 存在，门槛更低" | ❌ 不成立。venus=小米11/SM8350≠SM8250；全平台无 venusPkg 仓库；Mu-Silicium 生态无 kona 平台包先例 |
| "xl screenshot 无显示驱动也能截屏" | ⚠️ 半对。确实从 framebuffer 经 loader USB 通道取图、不依赖屏幕点亮；但前提是内核 framebuffer 平台配置已通，早期只能截到 -v 启动文本 |
| "群里看到小米10成功开机" | 无公开证据。已知开机机型仅 redwood（SM7325）与 QEMU |

## 3. 已完成工作（全部真实执行）

### 3.1 环境（Windows 11 + WSL2）
- WSL：Ubuntu 26.04.1，36 核 / 15GB RAM / 955GB 空闲，root 用户。
- 依赖已装：git、python3、7z、lz4、gcc、clang、lld、qemu-system-arm、qemu-efi-aarch64、adb、fastboot、dtc（device-tree-compiler）等。
- `C:\Users\17125\.wslconfig` 已写 `[wsl2] vmIdleTimeout=-1`（禁空闲回收；此前 WSL 默认 60s 无连接即杀 VM 导致构建进程被杀，已解决）。
- 两个仓库克隆于 `WSL:/root/hackios/`，`common/setup.sh` 通过（kernel/loader 用 prebuilt/）。

### 3.2 真机素材采集（手机当时在 OrangeFox recovery，adb root 运行）
设备确认：product=cas / model=M2007J1SC / Android(实际 LineageOS 24) / verifiedbootstate=orange / adb 序列号 9c4a9224。
- 拉取 `/sys/firmware/fdt` → `cas.dtb`（947,376 B，真机设备树）✅
- 拉取分区 xbl_4、xbl_5（各 7,340,032 B = 手机 XBL UEFI 固件镜像，A/B 槽位，内容基本相同）✅
- 音频固件：modem/dsp 分区 → `adsp.mdt + adsp.b00~b18`（20 文件）✅
- WiFi 固件：qca6390 目录 54 文件/8.1MB（amss20.bin、bdwlan.elf、bd_*.elf、regdb.bin）✅
- 马达固件：aw8697_haptic.bin ✅
- GPU（Adreno 650/a650）固件：**当前系统缺失**（仅 sysfs 节点 3d6a000.qcom,gmu、subsys_a650_zap；文件需从官方线刷包或开源 vendor 提取）。首期 framebuffer 里程碑不依赖，不阻塞。

### 3.3 IPSW 下载（iOS 16.5 / 20F66，iPhone X）
- URL：`https://updates.cdn-apple.com/2023SpringFCS/fullrestores/032-85130/6AB0D10A-8AF9-4F6C-8CF5-A1B315CABE88/iPhone10,3,iPhone10,6_16.5_20F66_Restore.ipsw`（总 6,165,698,198 B）
- 已全量下载并解包于 `WSL:/root/hackios/ios165/`：rootfs.dmg（4,728,546,953 B）、cryptex-os、cryptex-app、dyld shared cache；`.done-rootfs/.done-cryptex/.done-cryptex-app` 三个完成标记齐全。
- 下载中断问题已解决：用 `setsid nohup` 脱离会话重跑；网络实测正常（Apple CDN 5.7MB/s）。

### 3.4 QEMU 校准（重要里程碑：整条编译链路验证通过）
- Mu-Silicium 克隆：`uefi/apply.sh` → `/root/hackios/Mu-Silicium/`（BASE commit e51cd877e31c8fbb322032dc4cb6715e83d25ad3，约 3.6GB 含子模块），子模块断链后手动补跑 `git apply /root/hackios/uefi/mu-silicium.patch` + `cp -r files/.`。
- 构建 `common/build.sh qemu-virt` 修复两处：① userland/build.sh 366 行附近 drawable_test 的 `cp` 加 `[ -f ]` 防护（prebuilt 模式缺该文件）；② 被 WSL 空闲回收杀进程（.wslconfig 修复）。
- 产物：`userland/root.apfs`、`userland/system.apfs`、`qemu/ram.img`。
- 启动：QEMU 需要 64MB pflash，Ubuntu 自带 QEMU_EFI.fd 仅 2MB → 改用 `/usr/share/AAVMF/AAVMF_CODE.fd`（软链 no-secboot）+ `AAVMF_VARS.fd`。
- 结果：iOS 16.5 在模拟器里完整启动——launchd 拉起 33+ 进程（wifi/fseventsd/analyticsd/lsd 活跃）、SpringBoard 日志 68+ 行、无 panic、截屏 1125x2436 显示**黑底白苹果 Logo**（两轮运行一致）。证据：`WSL:/root/hackios/logs/qemu-cal.log`（412KB 完整启动日志）、`logs/qemu-screen*.png`。
- 复跑入口：`WSL:/root/hackios/start_qemu.sh`（600s 窗口）。

### 3.5 cas 设备档案（common/new-device.sh 生成）
- `WSL:/root/hackios/common/devices/cas.conf`（已修正 NAME="Xiaomi 10 Ultra (cas)"、SOC="Qualcomm SM8250"、DTB=/root/hackios/cas.dtb）
- `WSL:/root/hackios/common/devices/cas.platform.c`（GIC dist 0x17a00000 / redist 0x17a60000、8 核、MPIDR shift 8、small_cores 0xf —— 参数正确）
- 待填：io_base/io_size（需一个覆盖 SoC 外设的窗口，不能从 0 开始）、fb_base（等 UEFI 报出，loader 启动时打印 "display W x H at ..."）。

### 3.6 casPkg UEFI 移植骨架（本轮新增）
- 创建 `WSL:/root/hackios/Mu-Silicium/Platforms/Xiaomi/casPkg/`（从 redwoodPkg 复制全量改名替换，redwood→cas / redwoodPkg→casPkg，验证无残留）：
  - cas.dsc：PLATFORM_NAME=cas、PLATFORM_GUID=`FD69FE4A-0C42-4423-BFD7-40569B2741AB`、SMBIOS "Xiaomi 10 Ultra" / RetailModel "cas" / RetailSku "Xiaomi_10_Ultra_cas"、framebuffer **1080x2340x32**（小米10至尊分辨率）、UEFI Stack 0x9FF90000/0x40000（redwood 值暂留）、`!include KodiakPkg/KodiakPkg.dsc.inc`（**关键：Mu-Silicium 无 kona SoC 包，暂借 lito 的 KodiakPkg，按 PORTING.md "copy closest device" 策略**）。
  - cas.fdf、cas.dec（空）、DeviceBuild.py（casPkg/cas/dsc 均已替换）、Include/{APRIORI,DXE,RAW}.inc（驱动路径已改 Binaries/cas）。
  - Library/{MemoryMapLib, ConfigurationMapLib}：**内容仍为 redwood 值，kona 布局待改**（见 §5 差异表）。
- `WSL:/root/hackios/Mu-Silicium/Binaries/cas/`：从 Binaries/redwood 复制全量（67 个 Qcom 驱动 + MdeModulePkg/ArmPkg/EmbeddedPkg/FatPkg/MiPkg/MiToken + XBLCore + RawFiles）作为**占位**。平台无关部分（MdeModulePkg/ArmPkg/FatPkg/MiToken/XBLCore）可直接复用；Qcom 专有驱动（TzDxe/ClockDxe/UFSDxe/DisplayDxe 等）需从 cas xbl 提取后替换。
- `WSL:/root/hackios/uefi/files/Resources/Configs/cas.toml`（从 redwood.toml 复制；uefi_fd base=0x9FD00000/size=0x200000 为 redwood 值待校准）。
- `WSL:/root/hackios/uefi/files/Resources/DTBs/cas.dtb` = 真机 cas.dtb（947,376 B）✅
- build_uefi.py 是**动态发现**平台（`Platforms/**/{device}Pkg` glob + `Resources/Configs/{device}.toml`），无需注册，`-d cas` 直接可用。

### 3.7 cas xbl 逆向分析（本轮，结论重要）
- xbl_4/xbl_5 均为 ELF（XBL.elf，13 个 PT_LOAD 段），内含：
  - FV 头 `_FVH`：xbl_4 @0x828E0、xbl_5 @0x88900，FvLength=0x270000（2.4MB）；FileSystemGuid=78E58C8C-3D8A-1C4F-9935-896185C32DD3（非标准 FFS GUID，uefi_firmware 库无法解析文件）。
  - 第二嵌套 ELF @0x2F28E0（4.2MB，段 vaddr 0x1469B000 附近——kona IMEM 区域）。
  - **明文驱动字符串**：DisplayDxe×73、DALSYS×4、SPMI×3（在 0x344xxx，属嵌套 ELF 尾部数据区）；**未见** TzDxe/ClockDxe/UFSDxe 等 40+ 个 redwood 驱动名单字符串。
  - 0x3048E0 有 XBL 段表（含 0xaddeadde 魔数、0x146A9xxx 运行地址指针）。
- **结论**：cas 的 xbl_4/5 只含 boot 子集驱动（DisplayDxe 等），完整 DXE 驱动集（UFS/TZ/Clock…）需另取——候选：官方线刷包（V816.0.4.0.TJJCNXM，5.4GB）的 xbl.img，或 abl 分区（当前 LineageOS 无 root，adb 读不了 /dev/block/by-name）。驱动提取工具：WOA-Project/UEFIReader（Windows GUI，redwood 官方流程即用它从 xbl.img 提取 + 5 个 Silicium 标准 patch，见 `WSL:/root/hackios/Mu-Silicium/Binaries/redwood/README.md`）。

## 4. 产物位置清单

### Windows 主项目目录 `C:\Users\17125\DoubaoWork\chats\2026-10-23\new-chat-1\`
- `tools\cas.dtb`（947,376 B，真机 FDT）
- `tools\xbl_4.img`、`tools\xbl_5.img`（各 7,340,032 B）
- `tools\fw-audio\`（adsp.mdt + b00~b18，20 文件）
- `tools\fw-wifi\qca6390\`（54 文件/8.1MB）
- `tools\fw-haptic\aw8697_haptic.bin`
- `tools\platform-tools\`（adb/fastboot 37.0.1，Windows 侧 adb 才能连真机）
- `tools\*.sh`：start_qemu.sh（AAVMF 固件）、check_screen.sh、final_check.sh、prep_mu_build.sh、monitor_qemu.sh、sample_qemu.sh 等（Write 工具写的是 LF，直接 cp 到 WSL 用，**不要再跑 sed/tr 行尾处理**，见 §7）
- `tools\*.py`：extract_xbl.py / dump_fvh.py / scan_elf.py / probe_te.py / scan_xbl5.py / make_caspkg.py / fix_caspkg.py / fix_casconf.py / check_*.py（均为解析/生成脚本）
- `qemu-screen.png`、`qemu-screen-t1.png`（QEMU 截屏，黑底白苹果 Logo，1125x2436）
- 各 `*check*.txt`、`*scan*.txt`、`*probe*.txt` 为过程记录（供回查）
- `.wslconfig`（C:\Users\17125\.wslconfig）

### WSL `/root/hackios/`
- `hackios-xnu-kernel/`、`hackios-xnu-userland/`（已 setup）
- `Mu-Silicium/`（已 apply + casPkg + Binaries/cas）
- `common/devices/cas.conf`、`cas.platform.c`
- `ios165/`（IPSW 素材齐全）
- `userland/root.apfs`、`userland/system.apfs`、`qemu/ram.img`（QEMU 校准产物）
- `logs/qemu-cal.log`（412KB 启动日志）、`logs/qemu-screen*.png`
- `xbl_4.img`、`xbl_5.img`、`cas.dtb`（副本）
- `uefi/files/Resources/Configs/cas.toml`、`uefi/files/Resources/DTBs/cas.dtb`、`uefi/files/Binaries/cas/`（占位）

## 5. 关键参数表

| 参数 | redwood（参照） | cas（当前状态） |
|---|---|---|
| SoC | SM7325 (lito) | SM8250 (kona) |
| DDR 基址 | 0x80000000 | 0x80000000（kona 同惯例，待 xbl 核实） |
| FD_BASE | 0x9FD00000 | cas.conf/toml 暂沿用，**待校准** |
| FD_SIZE | 0x200000 | 暂沿用 |
| PAYLOAD_BASE | 0xF0000000 | 暂沿用（XNU 容器地址，Mu-Silicium patch 的 BootShim.S 拷 XNUP 到 0xF0000000） |
| FB_BASE | 0xE1000000 | 待 UEFI 报出（loader 打印） |
| framebuffer | 1080x2400 | **1080x2340**（已改） |
| IMEM | 0x14680000 / SharedIMEM 0x146AA000 | 与 xbl ELF2 段 vaddr **一致**（可复用 redwood 值） |
| GIC | dist 0x17A00000 / redist 0x17A60000 | 相同（kona/lito 同代，已从 DTB 确认） |
| 核心数 | 8 | 8（cas.platform.c 正确） |
| UFS | PcdStorageIsUfs3 FALSE | 待核实（kona UFS3） |
| WiFi | WCN6750（ath11k, WIFI_ATH11K=1） | **qca6390（WCN3990 系），WIFI_ATH11K=0**（cas.conf 已置 0） |
| 触摸 | Goodix GT9916（SPI） | **FocalTech FTS（I2C-3, 0x49）**，需另写内核驱动 feeding /dev/redwood-touch |
| GPU | Adreno 642L | **Adreno 650**（a650 固件缺失，不阻塞首期 framebuffer 里程碑） |
| 面板 | 多面板 XML | cas 面板见 xbl 字符串 "J3S_37_02_0a_lcd_dsc_vid_xmldata" 等（RawFiles 待提取） |
| 平台 GUID | 62B40F33-4011-4ACB-8348-1A6BB928C8A6 | **FD69FE4A-0C42-4423-BFD7-40569B2741AB** |

redwood MemoryMapLib 内存描述 56 项（HYP/AOP/SMEM/WLAN FW/PIL Reserved/FV/FD/SEC Heap/DXE Heap/TZApps/QTEE/XNU Payload/IMEM/外设寄存器窗口…）：**kona 大部分区域地址不同（如 PIL Reserved、DXE Heap、FD 位置、Display Reserved），需按 kona 的 XBL 内存表重写**（可从 xbl 段表 0x3048E0 区域 + 官方 xbl.img 提取）。

## 6. 未完成项与阻塞点（按优先级）

1. **Binaries/cas 真驱动**（最大硬块）：从 cas xbl.img 用 UEFIReader 提取 67 个 Qcom DXE 驱动 + 面板 XML RawFiles + 5 个标准 patch。当前占位为 redwood 驱动，kona 上大概率不匹配（Clock/Display/UFS 配置不同）。需要完整 xbl.img（官方线刷包 5.4GB 或 abl 分区）。
2. **casPkg 内存映射**：MemoryMapLib.c 的 kona 布局（FD_BASE/FD_SIZE/Display/DXE Heap 等）；ConfigurationMapLib.c 校验（NumCpus=8 已对；DloadCookieAddr/SecurityFlag 等 kona 值需查 xbl）。
3. **kernel 平台代码**（cas.platform.c 剩余 + kernel 驱动）：io_base/io_size 窗口；fb_base（首次 UEFI 起来后读取）；触摸 FocalTech 驱动；buttons/cpufreq GPIO 数字（redwood_input.c / redwood_cpufreq.c / loader_return.c 改）；GPU a650 固件获取。
4. **首次真机构建与 bring-up**：`common/build.sh cas` → `fastboot boot`（用户手机已解锁、有 OrangeFox recovery、当前系统 LineageOS 24 USB 调试在线、Windows 侧 adb 可用）。

## 7. 已知技术坑（执行者必读）

- **WSL 后台进程**：`wsl.exe bash -lc "setsid nohup ... &"` 的嵌套后台会在外层退出时被杀。可靠做法：前台执行 + 长 timeout；或写 wrapper 脚本（内部 nohup）由前台调起。长任务记得 `.wslconfig` 已禁空闲回收。
- **行尾**：Windows Write 工具产出的脚本是 LF，直接 `cp` 到 WSL 即用。**禁止再跑 `sed -i "s/\r$//"` 或 `tr -d "\r"`**（bash 双引号里 `\r` 是字面反斜杠+r，会把脚本里所有字母 r 删光：source→souce、mkdir→mkdi——已踩坑）。
- **PowerShell 引号**：`$()`、`\"`、`$(ps...)` 会被 PowerShell 展开/破坏传给 WSL 的命令。复杂脚本一律：Write 写 Windows 侧 → `cp` 进 WSL → `bash 脚本`；或 Python 代码用 base64（`echo $b64 | base64 -d > x.py && python3 x.py`）。
- **venv 坑**：WSL 的 Ubuntu 26.04 默认 python3.14 无 ensurepip，`python3 -m venv` 报错。需要 `apt install python3.14-venv`；或 pip 加 `--break-system-packages`（系统 rich 13.9.4 与 requirements 冲突会报 "Cannot uninstall rich"，此时优先 venv 方案）。当前 venv 构建**未确认完成**，跑 build_uefi.py 前先验证 `venv/bin/python -c "import edk2toollib"`。
- **mu_nasm**：Mu_Basecore/BaseTools/Bin/mu_nasm_extdep 需系统 nasm 替代（避免 Mono）：`mkdir -p $D/Linux-x86-64 && echo 'version: 20016.1.1' > $D/extdep_state.yaml && ln -sf /usr/bin/nasm /usr/bin/ndisasm $D/Linux-x86-64/`（**已完成**）。
- **adb**：WSL 的 adb 看不到 Windows USB 设备；真机操作必须用 Windows 侧 `C:\Users\17125\DoubaoWork\chats\2026-10-23\new-chat-1\tools\platform-tools\adb.exe -s 9c4a9224`。emulator-5554 是无关的离线模拟器。手机重启类操作（adb reboot）会被系统策略拦截，只能让用户手动重启。
- **QEMU 固件**：virt 机器要 64MB pflash，用 AAVMF_CODE.fd + AAVMF_VARS.fd，不能用 2MB 的 QEMU_EFI.fd。
- **fastboot/线刷**：未执行过；手机已解锁，首次 `fastboot boot <img>` 前建议先做好 OrangeFox 备份（用户已在 recovery 用过）。

## 8. 下一步建议（供 DeepSeek 指导路线）

按官方 PORTING.md 的顺序（每步可验证）：
1. **搞定 venv/构建环境** → `cd /root/hackios/Mu-Silicium && source ../venv/bin/activate && python3 build_uefi.py -d cas -r RELEASE`，目标：先让 casPkg 骨架**编译通过**（当前用 redwood 占位驱动，能编过即证明结构正确；编不过先修结构问题）。
2. **提取 cas 真驱动**：获取完整 xbl.img（官方线刷包 V816.0.4.0.TJJCNXM 5.4GB 内，或让用户回 OrangeFox recovery 后 `adb root` 拉 abl/其它分区对比），用 UEFIReader 提取到 Binaries/cas，替换占位驱动 + RawFiles 面板 XML，应用 5 个标准 patch（ButtonsDxe/DALSYSDxe/TzDxeLA/UsbConfigDxe/UsbMsdDxe）。
3. **校准 kona 内存映射**：从 xbl 段表/官方 xbl 提取 MemoryMap 值，重写 MemoryMapLib.c + 更新 cas.toml/cas.conf 的 FD_BASE/FD_SIZE/PAYLOAD_BASE（kona 与 lito 的差异集中在 FD 位置、PIL Reserved、DXE Heap 等）。
4. **构建 loader/kernel**：`common/build.sh cas`（产出 boot.img）；loader 启动打印 "display W x H at ..." → 回填 fb_base 到 cas.conf/platform.c。
5. **真机 bring-up**：`fastboot boot boot.img` 迭代；先看 loader 的 USB 日志（`./xl log`），再 -v 内核启动文本，然后 SpringBoard（锁屏）。首期里程碑 = framebuffer + touch + CPU 的最小启动（无 GPU 固件也可达锁屏）。

## 9. 快速命令集

```sh
# Windows 侧真机
adb='C:\Users\17125\DoubaoWork\chats\2026-10-23\new-chat-1\tools\platform-tools\adb.exe'
& $adb -s 9c4a9224 shell id

# WSL
wsl.exe -d Ubuntu -e bash -lc '...'   # 复杂命令走脚本文件

# 复跑 QEMU 校准
bash /root/hackios/start_qemu.sh      # AAVMF 固件，600s 窗口
bash qemu/screendump.sh logs/qemu-screen.png

# 构建 cas UEFI（第一步目标）
cd /root/hackios/Mu-Silicium && source ../venv/bin/activate && python3 build_uefi.py -d cas -r RELEASE
```

## 10. 已完成任务的真实性说明

QEMU 校准是**官方流程在模拟器的完整验证**（IPSW→userland 打包→loader→XNU→用户态→苹果 Logo），不是仿真造假；日志 412KB 可查。xbl 分析、casPkg 骨架、DTB/固件采集均为真实执行，产物路径见 §4。尚未进行：cas 首次编译、真机刷机。
