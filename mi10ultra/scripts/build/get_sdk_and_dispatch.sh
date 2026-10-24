#!/bin/bash
set -u
echo "########## 1. 取 theos SDK（master 分支，稀疏检出 iPhoneOS16.5.sdk） ##########"
rm -rf /tmp/theos-sdks3
GIT_LFS_SKIP_SMUDGE=1 git clone --depth 1 --branch master --filter=blob:none --sparse https://github.com/theos/sdks.git /tmp/theos-sdks3 2>&1 | tail -2
cd /tmp/theos-sdks3
git sparse-checkout set iPhoneOS16.5.sdk 2>&1 | tail -2
echo "--- SDK 目录内容 ---"
ls iPhoneOS16.5.sdk 2>/dev/null | head -10
du -sh iPhoneOS16.5.sdk 2>/dev/null
echo "--- 判断是否为 LFS 指针（文件应远大于 200 字节） ---"
sz=$(stat -c %s iPhoneOS16.5.sdk/usr/include/stdio.h 2>/dev/null || echo 0)
echo "  stdio.h = $sz B"
if [ "$sz" -gt 1000 ]; then
  mkdir -p "$HOME/theos/sdks"
  rm -rf "$HOME/theos/sdks/iPhoneOS16.5.sdk"
  cp -a iPhoneOS16.5.sdk "$HOME/theos/sdks/" && echo "  SDK 已安装到 theos"
  ls "$HOME/theos/sdks/" | head -3
else
  echo "  !! 可能是 LFS 指针，需要 git-lfs"
fi
echo
echo "########## 2. 用 clang 重建 libdispatch ##########"
cd /root/hackios/swift-corelibs-libdispatch || exit 1
rm -rf build && mkdir -p build && cd build
CC=clang CXX=clang++ cmake .. -DCMAKE_INSTALL_PREFIX=/usr/local -DBUILD_TESTING=OFF -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ 2>&1 | tail -5
make -j"$(nproc)" 2>&1 | tail -6
sudo make install 2>&1 | tail -3
sudo ldconfig
ls -la /usr/local/lib/libdispatch* /usr/local/include/dispatch/dispatch.h 2>/dev/null | head -5
echo SDK_AND_DISPATCH_DONE
