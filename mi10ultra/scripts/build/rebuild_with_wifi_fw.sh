#!/bin/bash
# rebuild_with_wifi_fw.sh — 加 DEVICE=umi 重建 userland，让 WiFi 固件进镜像
set -u
export K=/root/hackios/hackios-xnu-kernel-umi
export THEOS=${THEOS:-$HOME/theos}
export XNUDEPS=$K/xnu-deps
export PATH="/root/hackios/lldbin:$XNUDEPS/host/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"
OUT=/mnt/c/Users/17125/DoubaoWork/chats/2026-10-23/new-chat-1
cd $K/userland
echo "########## 0. 前置确认 ##########"
for d in fw-gpu fw-wifi fw-haptic; do
  if [ -d ../fw-$d ]; then echo "  ../fw-$d: $(find ../fw-$d -type f | wc -l) 文件"; else echo "  ../fw-$d: 无"; fi
done
ls ../fw-wifi/qca6390/amss20.bin ../fw-wifi/qca6390/m3.bin ../fw-wifi/qca6390/bd_j1gl.elf ../fw-wifi/qca6390/regdb.bin 2>&1 | head -5
echo
echo "########## 1. 重建（DEVICE=umi GL=1 WIFID=1） ##########"
date -u +%FT%TZ
env DEVICE=umi GL=1 WIFID=1 REST=1 ADSP= RC_JOBS=rc.jobs.umi-boot bash build.sh > /root/hackios/logs/userland-gl4.log 2>&1
echo "rc=$?"
grep -n 'fw/' /root/hackios/logs/userland-gl4.log | tail -2
grep -n -E 'error:|cannot|No such' /root/hackios/logs/userland-gl4.log | head -6
ls -la root.apfs system.apfs
echo "--- root/fw 树（应有 qca6390） ---"
find root/fw -maxdepth 2 | head -20
echo
echo "########## 2. 打包 payload（WiFi 固件版） ##########"
cd $K
KM=xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837
CMDLINE="loader_return_after=900 epoch=$(date +%s) sb_delay=8 bootcpu=7 cpus=7 umi_touch=1 umi_keys=1 gpu=1 wifi=1 fb_scan=1 cpufreq=1"
python3 xnuloader/mkpayload.py -o out/run-payload-gpu-wifi.bin kernel=$KM ramdisk=userland/root.apfs --cmdline "$CMDLINE"
echo "mkpayload rc=$?"
ls -la out/run-payload-gpu-wifi.bin
cp -v out/run-payload-gpu-wifi.bin $OUT/tools/run-payload-gpu-wifi.bin
cp -v userland/system.apfs $OUT/system.apfs
echo WIFI_FW_PAYLOAD_DONE