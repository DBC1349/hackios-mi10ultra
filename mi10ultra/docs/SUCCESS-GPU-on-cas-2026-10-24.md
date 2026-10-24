# 🎉 cas（小米10至尊）GPU 硬件渲染打通 —— 2026-10-24 21:00

## 结论

**Adreno 650 GPU 在 cas 上完整启动，CoreAnimation 通过 Mesa 的 FD650 (freedreno) 驱动在 GPU 上硬件渲染。**
触摸同时健康工作（零错误）。iOS 16.5 主屏由 GPU 绘制。

### 内核侧证据（`win_loader.log`）

```
adreno: GPU profile: UMI Adreno 650 / Kona
adreno: 2: qfprom corrected speed bin (0x78419b) = 0x10 → UMI Adreno 650 speed bin 0
adreno: 5: in Kona GPUCC: set the GMU clock to the stock 200 MHz rate
adreno: 5: Kona's GMU clock accepted wake/sleep delays (0x80004ff0)
adreno: 6: gmu cm3_boot_config / fw_init_result 读取正常
adreno: 10: the GMU's memory TAKES WHAT IS WRITTEN: its firmware can be loaded
adreno: 11: TrustZone 鉴权调用全部返回 answer 0x1（zap/A650 可用）
adreno: step 12: 找到 zap shader header；step 13/14: 加载 GMU 固件（前 16K 代码+数据）
adreno: step 16..20: PDC、GMU 表、3D 核心 ring、核心寄存器
adreno: step 21: command the 3D core out of secure mode (zap shader)
adreno: THE GPU STAYS ON for the system to draw with: the service HKGPU
adreno: the GPU's sensors read 46.7 and 53.3 degrees; its clock is set to 587 MHz
```

### userland 侧证据

```
hk-gl: 8 of 8 methods of EAGLContext are ours; OpenGL ES by Mesa, off-screen (OSMesa)
hk-gl: an OpenGL ES 3 context for CoreAnimation, by Mesa: OpenGL ES 3.2 Mesa 25.0.7; FD650; GLSL ES 3.20
hk-gl: asked for string 0x1f01: FD650
redwood: the server has an accelerated renderer
/hackios/lib/libOSMesa.8.dylib  14809956 B
```

### 触摸

```
UMI FTS: poll=21875 events=7 errors=0 irq=1 active=0
UMI FTS: event 43 01 a0 00 01 00 00 05 ...
```

## 启动参数（本次）

```
loader_return_after=900 sb_delay=8 bootcpu=7 cpus=7 umi_touch=1 umi_keys=1 gpu=1 wifi=1 fb_scan=1 cpufreq=1
```

## 本次新增/修复的关键环节

| 环节 | 内容 |
|---|---|
| **内核** | 拉到仓库最新提交（`6264ebf` WiFi/PCIe/MHI + 之前的 A650/587MHz/Metal）并**真正重编**。⚠️关键坑：`xnu-src/` 不在 git 里，`git reset` 不会更新它——必须先 `git checkout -- .` 还原 Apple 文件、重新 `git apply hackios.patch`、再 `cp -a xnu/files/. xnu-src/`，否则编出来的还是旧内核 |
| **GPU 固件** | 从手机 `/vendor/firmware/` 拉取：`a650_gmu.bin`(41548) `a650_sqe.fw`(31968) `a650_zap.mdt`(6860)+`b00/b01/b02`+`elf` → `fw-gpu/` |
| **Mesa** | 编出 `libOSMesa.8.dylib` 14,809,956 B（1296 个 gl* 函数），`DRIVERS=softpipe,freedreno` |
| **链接器（大坑）** | 自建的 cctools ld64 编译时**没有 `TAPI_SUPPORT`**，完全读不了 iOS SDK 的 `.tbd` 文本桩库（报 `library not found for -lSystem`）。解法：**用 LLVM 的 `ld64.lld`**（原生支持 tbd），做成 `xnu-clang` 包装的变体（`/root/hackios/lldbin/xnu-clang`，`-fuse-ld=lld`），静态程序仍用 cctools、动态库走 lld |
| **userland** | 必须**全量编译**（`PREBUILT` 分支不含 GL 产物）；`GL=1 WIFID=1` |
| **构建依赖** | theos + iPhoneOS16.5.sdk(theos/sdks master 稀疏检出) + libdispatch(源码) + libyaml-dev + uuid-dev + libcrypt-dev + meson/ninja/mako + XNU 源码 |
| **USB 新发现** | 运行中 `err=203` 不是 USB 死了，而是**daemon 句柄失效**——重启 daemon（新句柄）即可继续通信，可实时取内核日志 |

## 当前未通：WiFi（QCA6390）

```
UMI Wi-Fi: PCIe enumeration diagnostic; DMA stays disabled
UMI PCIe: enumeration failed errno = 0x13
```
两个原因：
1. **固件没进镜像**：`build.sh` 的 `umi_firmware()` 有门控 `[ "${DEVICE:-}" = umi ] || return 0` → 重建时要加 `DEVICE=umi`（固件已在 `fw-wifi/qca6390/`：amss20.bin 4.35MB、m3.bin、bd_j1gl.elf、regdb.bin）
2. **PCIe 枚举失败 0x13**：仓库自带诊断（`umi_wifi_step=1`/`2` + `umi_wifi_return=1`，需用 umi loader/DTB 的临时 RAM 启动）可定位到具体寄存器

## 复现命令（本轮）

```sh
# 1) 内核（在仓库树上）
cd /root/hackios/hackios-xnu-kernel-umi
git fetch --depth=30 origin main && git reset --hard origin/main
XS=xnu-src/xnu-8796.121.2
git -C $XS checkout -- . && git -C $XS apply --whitespace=nowarn xnu/hackios.patch && cp -a xnu/files/. $XS/
THEOS=$HOME/theos ./xnu-build.sh -j$(nproc) -k

# 2) Mesa（用 lld 包装）
sed -i 's|^CC=.*xnu-clang$|CC=/root/hackios/lldbin/xnu-clang|' tools/mk-mesa.sh
bash tools/mk-mesa.sh

# 3) userland（动态库走 lld + GL + WiFi）
cd userland
sed -i 's|^DYN="xnu-clang |DYN="/root/hackios/lldbin/xnu-clang |' build.sh
env DEVICE=umi GL=1 WIFID=1 REST=1 ADSP= RC_JOBS=rc.jobs.umi-boot bash build.sh

# 4) payload + 真机
python3 xnuloader/mkpayload.py -o out/run-payload-gpu.bin \
  kernel=$XS/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837 ramdisk=userland/root.apfs \
  --cmdline "loader_return_after=900 sb_delay=8 bootcpu=7 cpus=7 umi_touch=1 umi_keys=1 gpu=1 wifi=1 fb_scan=1 cpufreq=1"
```