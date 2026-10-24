# HackiOS → 小米10至尊纪念版（cas）· 接管进展（2026-10-24 下午）

> 增量更新。承接《HackiOS-cas-接管进展-2026-10-24.md》（凌晨版）。

## A. 重大转向：改用用户自己的 umi（小米10）移植仓库作蓝本

用户提供了两个已验证的移植仓库（**小米10 / umi，SM8250/kona**）：
- `hackios-xnu-kernel-umi`（main @512173b2）
- `hackios-xnu-userland-umi`（main @5b70456b）

README 实证：umi 已能从 RAM 引导 iOS 16.5 到 SpringBoard（锁屏/桌面/App/触摸/Wi-Fi/GPU/声音/相机均已验证）。**umi 与 cas 同 SoC、同代、面板同为 1080x2340、触摸同为 ST FTS（地址 0x49，复位 GPIO38 / IRQ GPIO39）、framebuffer 同在 0x9C000000、FD/PAYLOAD 地址全同。**

### 推论（已逐一验证）
1. **cas 自己 XBL 的 `uefiplat.cfg` 内存映射与 umi 逐条相同** → umi 的 MemoryMapLib/ConfigurationMapLib 可直接用于 cas。
2. **cas DTB 的 DDR 布局与 umi 相同**（含 `0xBBB00000..0xC0000000` 69MB 空洞）→ umi 的 `UMI DDR hole` 条目同样适用。
3. **cas 自己的 XBL 里包含全部 91 个 Kona 驱动模块**（用 `uefi_firmware 1.16` 解析，91/91 命中）——**推翻凌晨文档"cas xbl 只含 boot 子集、需另取驱动"的结论**（那是解析器 bug）。

## B. 本轮成果（全部真机验证）

### B1. cas 自己的 UEFI 构建并成功启动 ✅
- 用 `uefi/umi/` 的**方法**（apply/build/extract 三件套思路）+ **cas 自己 XBL 提取的驱动** + umi 模板的 MemoryMapLib/ConfigurationMapLib/BootManagerLib 生成 `Platforms/Xiaomi/casPkg`。
- 真机 `fastboot boot` 后 loader 正常驻留，屏幕显示：
  `HackiOS CAS UEFI: looking for RAM loader` / `UMI LoadImage: Success` / `== XNULOADER v3 ==` / `display 1080 x 2340 at 9c000000` / `platform cas` / `usb: started`
- **截图证据**：`loader_screen.png`（Project Silicium logo + 上述文本）。

### B2. 关键坑：TzDxeLA 补丁（否则 UEFI 启动即崩）
- cas XBL 提取出的 `TzDxeLA.patched.efi` 与 umi 期望的补丁前哈希**完全一致**（`a87cb38b...`），需把 `0xd469` 由 0 置 1（TZ app-loaded 标志）。
- **不打此补丁的表现：`fastboot boot` 后 2 秒设备消失、6 秒回到 fastboot。** 这是最典型的"UEFI 早期崩溃"信号。

### B3. 另一个坑：包结构必须与配置映射匹配
- 只把 alioth 结构包的 `Binaries/cas` 换成 cas 驱动（保留 alioth 的 ConfigurationMapLib/EnableUfsIOC=1 等）→ **仍失败**。
- 正确组合 = **umi 模板包（配置值取自 cas 的 uefiplat.cfg）+ cas 自己的驱动 + TzDxeLA 补丁** → 成功。

### B4. 真机固件内存表（透过 loader 的 `oem memmap` 取到，验证映射正确）
```
9c000000 - 9e400000  type 0  36 MB    Display Reserved ✓
a0000000 - b0000000  type 0 256 MB    Kernel
bbb00000 - c0000000  type 0  69 MB    DDR hole ✓
f0000000 - f4000000  type 0  64 MB    XNUP Payload（我们的预留生效）✓
type 7（空闲）合计 7329 MB
```

## C. USB 通路的最终正解：WinUsb 常驻 daemon（不经 usbipd）
- **一次性脚本"用完即关句柄"会把 loader 搞聋**（关闭句柄会取消 loader 已排队的接收，它视同拔线进入 `usb_wait`）。
- **正解**：`tools/winfbd3.ps1` —— 常驻进程，句柄全程不关；命令走 `winfb_cmd.txt`，结果落 `winfb_out.txt`；支持 `log/memmap/screenshot/ramdisk/stage/run/result/stop/reboot`。
  - 一次性脚本 `tools/winfb.ps1` 仅用于单条探测（有聋掉风险）。
- 前置条件：给 loader 设备节点加 Android fastboot 接口 GUID `{F72FE0D4-CBCB-407D-8814-9ED673D0DD6E}`（注册表 `...\USB\VID_18D1&PID_D00D\XNULOADER\Device Parameters`），并禁用 USB 选择性暂停。
- usbipd/vhci 路线**弃用**（两次搞挂手机 USB）。

## D. 产物与脚本位置

**WSL**
- `/root/hackios/Mu-Silicium/Platforms/Xiaomi/casPkg`（当前=**可启动的 umi 模板包**；`casPkg.alioth2.bak`=alioth 结构包；`casPkg.umipkg.failed` 已并入前者）
- `/root/hackios/Mu-Silicium/Binaries/cas`（cas 自己 XBL 提取的 90 个 .efi + 59 个 RawFiles；TzDxeLA 已打补丁）
- `/root/hackios/cas-fw/{Drivers,RawFiles,index.json}`（原始提取产物）
- `/root/hackios/Mu-Silicium/Build/casPkg/RELEASE_CLANGPDB/FV/SILICIUM_UEFI.fd`（FD，0x300000）
- `/root/hackios/hackios-xnu-kernel-umi/`（umi 移植树，已建 `common/devices/cas.conf`、ios165 数据 5.7G）
- `/root/hackios/hackios-xnu-userland-umi/`（userland）

**Windows（`...\new-chat-1\`）**
- `tools/cas-ios-boot.img`（cas UEFI + umi loader + cas.dtb；loader 上报 board=umi）
- `tools/cas-boot-C.img`（cas UEFI + 自家 loader，board=cas）
- `tools/winfbd3.ps1`（常驻 daemon，含 ramdisk/stage/run）、`tools/send_cmd.ps1`、`tools/start_daemon.ps1`
- `loader_screen.png`（真机截屏）、`win_loader.log`、`win_memmap.log`

## E. 复现步骤（cas）

```bash
# 1) 提取 cas 自己的驱动（用 umi 的方法）
/root/hackios/venv/bin/python tools/extract_cas.py         # -> /root/hackios/cas-fw
/root/hackios/venv/bin/python tools/patch_tz.py            # TzDxeLA 0xd469=1

# 2) 生成 casPkg（umi 模板 + cas 驱动）并编译
/root/hackios/venv/bin/python tools/make_caspkg_from_umipkg.py
cd /root/hackios/Mu-Silicium && source ../venv/bin/activate
python3 build_uefi.py -d cas -r RELEASE -c                 # -> SILICIUM_UEFI.fd (0x300000)

# 3) 打包 iOS 启动镜像（cas UEFI + umi loader）
bash tools/make_ios_img.sh                                 # -> tools/cas-ios-boot.img

# 4) 真机：fastboot boot cas-ios-boot.img
# 5) Windows 侧常驻 daemon 接管，再走 ramdisk/stage/run
powershell -File tools/start_daemon.ps1
powershell -File tools/ios_run.ps1
```

## F. 下一步

1. 等 userland 构建出 `root.apfs`/`system.apfs`（`PREBUILT=1 SKIP=kernel common/build.sh umi`）。
2. 用 daemon：`ramdisk` → `stage system.apfs`（2.5GB）→ `stage run-payload.bin`（内核+root.apfs）→ `run` → 观察屏幕/取日志。
3. 首次建议 `bootcpu=7 cpus=7 umi_touch=1 umi_keys=1`（USB 保活，可实时截图看 iOS 启动过程），成功后再切 8 核。
4. 稳定后把 cas 作为一等设备加入 umi 树（`cas` 板卡门控、cas 触摸参数），并考虑内核侧 `cas_touch.c`。
