#!/bin/bash
# userland_gl_full.sh — 全量编译 userland（GL=1 + WIFID=1），动态库用 lld 链接
set -u
export K=/root/hackios/hackios-xnu-kernel-umi
export THEOS=${THEOS:-$HOME/theos}
export XNUDEPS=$K/xnu-deps
export PATH="/root/hackios/lldbin:$XNUDEPS/host/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"
cd $K/userland

echo "########## 1. 让动态库链接走 lld（cctools ld64 缺 TAPI/tbd） ##########"
cp -f build.sh build.sh.orig-$(date +%H%M) 2>/dev/null
grep -n '^DYN=' build.sh
sed -i 's|^DYN="xnu-clang -arch arm64 -isysroot ${THEOS:-$HOME/theos}/sdks/iPhoneOS16.5.sdk -O1"|DYN="/root/hackios/lldbin/xnu-clang -arch arm64 -isysroot ${THEOS:-$HOME/theos}/sdks/iPhoneOS16.5.sdk -O1"|' build.sh
grep -n '^DYN=\|^CC=' build.sh
echo "--- ld64 是否在 PATH（决定是否 PREBUILT） ---"
command -v ld64 || echo "  无 ld64"
echo
echo "########## 2. 确认 Mesa 库可被找到 ##########"
ls -la $K/xnu-deps/build/mesa/out/libOSMesa.8.dylib
echo
echo "########## 3. 全量编译（GL=1 WIFID=1，无 PREBUILT） ##########"
date -u +%FT%TZ
env GL=1 WIFID=1 REST=1 ADSP= RC_JOBS=rc.jobs.umi-boot bash build.sh > /root/hackios/logs/userland-gl2.log 2>&1
echo "rc=$?"
echo "--- 错误行 ---"
grep -n -E 'error:|Error|cannot|No such|failed' /root/hackios/logs/userland-gl2.log | head -20
echo "--- 尾部 ---"
tail -20 /root/hackios/logs/userland-gl2.log
echo "--- 产物 ---"
ls -la $K/userland/root.apfs $K/userland/system.apfs 2>/dev/null
echo "--- root.apfs 里的 GL 库 ---"
ls -la $K/userland/root/lib/ 2>/dev/null | grep -i -E 'osmesa|gl' | head -5
echo "--- fw/ ---"
ls -la $K/userland/root/fw/ 2>/dev/null | head -8
echo USERLAND_GL_FULL_DONE