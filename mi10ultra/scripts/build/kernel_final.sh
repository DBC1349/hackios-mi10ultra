#!/bin/bash
# kernel_final.sh — 用系统 tcsh 绕过 tcsh 编译 → setup.sh 完整跑完 → 编内核
set -u
K=/root/hackios/hackios-xnu-kernel-umi
export THEOS=${THEOS:-$HOME/theos}
export PATH="/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"
mkdir -p /root/hackios/logs
cd $K

echo "########## A. 放入系统 tcsh，让 setup.sh 跳过编译 ##########"
mkdir -p xnu-deps/host/bin
cp -f /usr/bin/tcsh xnu-deps/host/bin/tcsh 2>/dev/null || echo "  !! 无 /usr/bin/tcsh"
chmod +x xnu-deps/host/bin/tcsh 2>/dev/null
ln -sfn tcsh xnu-deps/host/bin/csh
ls -la xnu-deps/host/bin/tcsh xnu-deps/host/bin/csh 2>/dev/null
xnu-deps/host/bin/tcsh -c 'echo tcsh_ok' 2>&1 | head -2

echo "########## B. 重跑 xnu-deps/setup.sh（完整） ##########"
JOBS=$(nproc) bash xnu-deps/setup.sh > /root/hackios/logs/xnudeps-setup.log 2>&1
echo "setup rc=$?"
tail -20 /root/hackios/logs/xnudeps-setup.log

echo "########## C. 验证 availability.pl 是否装入 SDK ##########"
ls -la "$K/xnu-deps/sdk/iPhoneOS16.5.xnu.sdk/usr/local/libexec/" 2>/dev/null | head -8
find "$K/xnu-deps/sdk" -name 'availability.pl' 2>/dev/null | head -3
ls "$K/xnu-deps/sdk/iPhoneOS16.5.xnu.sdk/usr/include" 2>/dev/null | head -5

echo "########## D. 编译内核（完整日志） ##########"
THEOS=$THEOS JOBS=$(nproc) ./xnu-build.sh -j"$(nproc)" -k > /root/hackios/logs/kernel-full2.log 2>&1
echo "kernel build rc=$?"
echo "--- 错误行 ---"
grep -n -E 'error:|Error 1|fatal error|Unable to locate' /root/hackios/logs/kernel-full2.log | head -20
echo "--- 尾部 ---"
tail -12 /root/hackios/logs/kernel-full2.log

echo "########## E. 验证产物与触摸符号 ##########"
KM=$K/xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837
ls -la "$KM" 2>/dev/null || echo "  !! 内核未产出"
[ -f "$KM" ] && strings "$KM" | grep -i -E 'UMI FTS|FTS521|hackios-board' | head -8
echo KERNEL_FINAL_DONE