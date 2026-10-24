#!/bin/bash
# build_cctools2.sh — 装好 libdispatch 后重建 cctools（ld64），装进 theos 工具链目录
set -u
export THEOS=${THEOS:-$HOME/theos}
echo "=== 0. 依赖检查 ==="
ls /usr/local/lib/libdispatch.so /usr/local/include/dispatch/dispatch.h 2>/dev/null || { echo "!! 缺 libdispatch"; exit 1; }
ls /usr/lib/x86_64-linux-gnu/libBlocksRuntime.so 2>/dev/null || { echo "!! 缺 BlocksRuntime"; exit 1; }
echo "=== 1. configure（显式指定两个库） ==="
cd /root/hackios/cctools-port/cctools || exit 1
export LD_LIBRARY_PATH=/usr/local/lib:${LD_LIBRARY_PATH:-}
./configure --prefix=/root/hackios/cctools-build --target=aarch64-apple-darwin \
  --with-libdispatch=/usr/local \
  --with-libblocksruntime=/usr/lib/x86_64-linux-gnu 2>&1 | tail -8
echo "=== 2. make ==="
make -j"$(nproc)" 2>&1 | tail -8
echo "=== 3. install 到 theos 工具链 ==="
make install 2>&1 | tail -3
mkdir -p "$THEOS/toolchain/linux/iphone/bin"
cp -a /root/hackios/cctools-build/bin/* "$THEOS/toolchain/linux/iphone/bin/" 2>/dev/null
ls "$THEOS/toolchain/linux/iphone/bin" 2>/dev/null | head -15
echo "=== 4. ld64 可用性 ==="
"$THEOS/toolchain/linux/iphone/bin/ld64" -v 2>&1 | head -3 || echo "  ld64 调用失败"
echo CCTOOLS2_DONE
