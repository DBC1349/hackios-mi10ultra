#!/bin/bash
# build_gpu_payload.sh — 重建 userland（GL+WiFi）并打包 GPU payload
set -u
export K=/root/hackios/hackios-xnu-kernel-umi
export THEOS=${THEOS:-$HOME/theos}
export XNUDEPS=$K/xnu-deps
export PATH="/root/hackios/lldbin:$XNUDEPS/host/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"
OUT=/mnt/c/Users/17125/DoubaoWork/chats/2026-10-23/new-chat-1
cd $K/userland
echo "########## 1. 重建 userland（GL=1 WIFID=1，含 fw-gpu + fw-wifi） ##########"
date -u +%FT%TZ
env GL=1 WIFID=1 REST=1 ADSP= RC_JOBS=rc.jobs.umi-boot bash build.sh > /root/hackios/logs/userland-gl3.log 2>&1
echo "rc=$?"
grep -n -E 'error:|cannot|No such' /root/hackios/logs/userland-gl3.log | head -8
grep -n 'fw/' /root/hackios/logs/userland-gl3.log | tail -2
ls -la root.apfs system.apfs
echo "--- root/fw 树 ---"
find root/fw -maxdepth 2 -type f 2>/dev/null | head -12
echo
echo "########## 2. 打包 payload（内核 + root.apfs；gpu=1 wifi=1 触摸） ##########"
cd $K
KM=xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837
ls -la $KM
CMDLINE="loader_return_after=900 epoch=$(date +%s) sb_delay=8 bootcpu=7 cpus=7 umi_touch=1 umi_keys=1 gpu=1 wifi=1 fb_scan=1 cpufreq=1"
python3 xnuloader/mkpayload.py -o out/run-payload-gpu.bin kernel=$KM ramdisk=userland/root.apfs --cmdline "$CMDLINE"
echo "mkpayload rc=$?"
ls -la out/run-payload-gpu.bin
echo "cmdline: $CMDLINE"
echo
echo "########## 3. 拷贝到 Windows（供手机暂存） ##########"
cp -v out/run-payload-gpu.bin $OUT/tools/run-payload-gpu.bin
cp -v userland/system.apfs $OUT/system.apfs
ls -la $OUT/tools/run-payload-gpu.bin $OUT/system.apfs
echo GPU_PAYLOAD_DONE