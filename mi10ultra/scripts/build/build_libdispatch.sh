#!/bin/bash
set -u
echo "=== 1. 安装 cmake + 依赖 ==="
sudo apt-get install -y cmake 2>&1 | tail -2
command -v cmake && cmake --version | head -1
echo
echo "=== 2. theos/sdks 的 master 树里到底有什么 ==="
rm -rf /tmp/theos-sdks2
GIT_LFS_SKIP_SMUDGE=1 git clone --depth 1 --branch master --filter=blob:none --no-checkout https://github.com/theos/sdks.git /tmp/theos-sdks2 2>&1 | tail -2
git -C /tmp/theos-sdks2 ls-tree --name-only HEAD 2>&1 | head -20
echo "--- 含 16.5 的条目 ---"
git -C /tmp/theos-sdks2 ls-tree --name-only HEAD 2>&1 | grep -i '16\.5' | head -5
echo
echo "=== 3. 构建 libdispatch（后台会继续） ==="
cd /root/hackios/swift-corelibs-libdispatch 2>/dev/null || exit 1
rm -rf build && mkdir -p build && cd build
cmake .. -DCMAKE_INSTALL_PREFIX=/usr/local -DBUILD_TESTING=OFF -DCMAKE_BUILD_TYPE=Release 2>&1 | tail -8
make -j"$(nproc)" 2>&1 | tail -6
sudo make install 2>&1 | tail -3
sudo ldconfig
ls -la /usr/local/lib/libdispatch* /usr/local/include/dispatch/dispatch.h 2>/dev/null | head -5
echo LIBDISPATCH2_DONE
