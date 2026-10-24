#!/bin/bash
# sync_and_rebuild_kernel.sh — 把仓库源码同步进 xnu-src 并重编内核
set -u
K=/root/hackios/hackios-xnu-kernel-umi
XS=$K/xnu-src/xnu-8796.121.2
export THEOS=${THEOS:-$HOME/theos}
export PATH="/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"
cd $K
echo "########## 1. 现状对比（仓库 vs 构建树） ##########"
for f in bsd/dev/umi_wifi.c bsd/dev/umi_mhi.c bsd/dev/hk_wifi.c bsd/dev/hk_lx.c; do
  a=$(stat -c %Y xnu/files/$f 2>/dev/null || echo 0); b=$(stat -c %Y $XS/$f 2>/dev/null || echo 0)
  echo "  $f: repo=$(date -d @$a +%H:%M) buildtree=$(date -d @$b +%H:%M)"
done
echo
echo "########## 2. 把 Apple 文件还原到 tag，再重新 apply 我们的工作 ##########"
git -C $XS checkout -- . 2>&1 | tail -2
git -C $XS apply --whitespace=nowarn "$K/xnu/hackios.patch" 2>&1 | tail -3
cp -a $K/xnu/files/. $XS/ 2>&1 | tail -2
echo "--- 同步后 mtime ---"
ls -la $XS/bsd/dev/umi_wifi.c $XS/bsd/dev/hk_lx.c 2>/dev/null
echo
echo "########## 3. 重编内核（完整日志） ##########"
date -u +%FT%TZ
THEOS=$THEOS JOBS=$(nproc) ./xnu-build.sh -j"$(nproc)" -k > /root/hackios/logs/kernel-v3.log 2>&1
echo "rc=$?"
grep -n -E 'error:|Error 1|fatal error' /root/hackios/logs/kernel-v3.log | head -12
echo "--- 内核时间戳 ---"
KM=$XS/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837
ls -la "$KM"
echo "--- 新符号 ---"
strings "$KM" 2>/dev/null | grep -i -E 'ath11k station|QCA6390 firmware|hk_lx_net|adreno: THE GPU STAYS' | head -8
echo SYNC_REBUILD_DONE