# HackiOS → 小米10至尊纪念版（cas / SM8250）· 接管后进展交接（2026-10-24 凌晨）

> 本文件是 `HackiOS-cas-工程交接.md` 的**增量更新**，记录接管后的关键转向与成果。原交接文档的 §2/§3.6/§6 关于"无 kona 先例、必须从 xbl 提取驱动"的判断已被推翻，见下。

## A. 一句话现状

环境已全修复；casPkg 已从 **alioth（同 SoC kona/SM8250）** 重建并编译通过；**首个 cas boot.img 已产出并严格校验**；**真机首里程碑达成**——UEFI 在 cas 上完整启动、loader 驻留、`platform cas` 匹配、framebuffer `0x9C000000` 与预测完全吻合，并通过 WinUsb 直连取到首份真机 loader 日志。剩余：常驻 USB daemon 稳定后送内核首跑。

## B. 推翻原交接文档的三个判断

| 原判断 | 实测结论 |
|---|---|
| "Mu-Silicium 无 kona 平台包先例，只能借 lito 的 KodiakPkg" | **错**。`Silicon/Qualcomm/KonaPkg` 存在；`aliothPkg`(POCO F3, SM8250) 用 `!include KonaPkg/KonaPkg.dsc.inc`；`Binaries/alioth` 有 64 个完整 kona 驱动；`Silicium-ACPI/.../alioth` 有 ACPI 表 |
| "最大硬块=从 cas xbl 提取 67 个驱动（数周量级）" | **可绕过**。alioth 与 cas 同 SoC，其驱动集+面板 XML（含 `Panel_J3S_37_02_0a_lcd_dsc_vid.xml`，正是 cas 面板）直接可用 |
| "venusPkg 同 SoC 先例不存在" | 无关紧要；alioth 就是同 SoC 先例 |

## C. 已完成（接管后）

1. **环境修复**（死机造成的损坏）：venv 重建（python3.14-venv+nasm+llvm 补装）；`Conf/tools_def.txt|build_rule.txt|target.txt` 从模板恢复（曾全 0 字节）；`Binaries` 子模块 `git reset --hard` 恢复 21616 文件；`Mu_Basecore` 补丁复位。
2. **casPkg 重建**：`Platforms/Xiaomi/casPkg` 从 aliothPkg 复制（旧 redwood 版备份为 `casPkg.redwood.bak`）；`Binaries/cas`←`Binaries/alioth`；SMBIOS 改 cas；SOC_TYPE=0(SM8250)；framebuffer 1080x2340；FD base/size=0x9FC00000/0x300000（对齐 alioth 内存图）；MemoryMapLib 加 `XNU Payload 0xF0000000/0x4000000` 条目；DeviceBuild.py 修正 dsc 引用。
3. **编译通过**：`build_uefi.py -d cas -r RELEASE` → `Mu-cas.img`(1988608B) + `SILICIUM_UEFI.fd`(3145728B=0x300000)，48 images verified。
4. **loader 平台**：`xnuloader/loader.c` 注入 `platform_cas`（fb_base=0x9C000000，已用 cas.dtb `cont_splash_region@9c000000` 核实）并注册进 `platforms[]`；loader.efi 编译通过。
5. **首个 boot.img**：`common/build.sh cas`（SKIP=images）→ `out/cas-boot.img`(2019328B)。严格校验：gzip blob=3214096(=shim176+FD3145728+XNUP68192)、DTB 拼接(d00dfeed,947376)、shim FD_BASE@offset8=0x9fc00000。
6. **真机首里程碑**：`fastboot boot cas-boot.img` → 屏幕显示 `XNULOADER v3 / display 1080x2340 at 9c000000 / platform cas / usb: started`。
7. **WinUsb 直连**：给 loader 设备注册表加 Android fastboot 接口 GUID `{F72FE0D4-...}` 后，用自写 WinUsb 主机工具（`tools/winfb.ps1`/`WU2.cs`）直连成功（`getvar:version`→`OKAY0.5`），取到首份真机 loader 日志（`win_loader.log`）。

## D. USB 通路的坑与正解（重要）

- **loader 的 USB 是软件外设栈**：主机端**关闭 WinUsb 句柄会取消 loader 已排队的接收**，loader 视同拔线进入 `usb_wait` 永久等重连 → 设备"聋掉"（err=203 ERROR_OPERATION_ABORTED）。一次性脚本"用完即关"必聋。
- **正解 = 常驻 daemon**：`tools/winfbd.ps1`（句柄全程不关 + 每 15s keepalive + 命令文件 `winfb_cmd.txt`/结果 `winfb_out.txt`）。**绝不要再用一次性 winfb.ps1 反复开关句柄。**
- usbipd/vhci 路线**放弃**（两次把手机 USB 搞挂）；已 `usbipd unbind` 清除持久化。
- 复活聋掉的 loader：物理拔插 USB 线；彻底死掉：长按电源 10-15s 强启。
- 本机 USB3 链路不稳 + PCIe AER 纠错错误频发（也是电脑反复死机的根因），尽量用 USB2 口/换线可减复发。

## E. 产物位置

- WSL：`/root/hackios/Mu-Silicium/Mu-cas.img`、`Build/casPkg/RELEASE_CLANGPDB/FV/SILICIUM_UEFI.fd`、`/root/hackios/out/cas-boot.img`、`logs/cas-memmap.txt`(待取)
- Windows：`tools/cas-boot.img`、`tools/Mu-cas.img`、`tools/SILICIUM_UEFI_cas.fd`、`tools/run-payload.bin`(19.8MB 内核+root.apfs)、`tools/winfb.ps1|winfbd.ps1|WU2.cs|send_cmd.ps1|start_daemon.ps1|raw2png.py`
- 首份真机日志：`win_loader.log`

## F. 下一步（按序）

1. 手机强启回 Android → adb → bootloader → `fastboot boot cas-boot.img`。
2. **立即** `start_daemon.ps1` 起常驻 daemon（不关句柄）。
3. `send_cmd.ps1 memmap` 取真机固件内存表 → 校准 `casPkg/Library/MemoryMapLib/MemoryMapLib.c`（对照 alioth 值）。
4. `send_cmd.ps1 screenshot` → `raw2png.py` 转 PNG 验证显示。
5. `send_cmd.ps1 "stage tools/run-payload.bin"` → `send_cmd.ps1 run` → `send_cmd.ps1 result` → `send_cmd.ps1 log` 取内核日志 = **真机首次 XNU 运行**。
6. 内核跑通后：`common/build.sh cas` 全量（含 userland images）→ 完整 boot.img → 迭代到锁屏。
