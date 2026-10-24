#!/bin/bash
# setup_toolchain.sh — 准备 XNU 内核构建链：theos + iPhoneOS16.5 SDK + cctools(ld64) + XNU 源码
set -u
export THEOS=${THEOS:-$HOME/theos}
echo "########## 0. 环境检查 ##########"
for t in git curl tar xz make clang flex bison m4 perl python3 autoconf automake; do
  printf "%-12s %s\n" "$t" "$(command -v $t || echo MISSING)"
done
nproc; df -h /root | tail -1

echo "########## 1. 克隆 theos（shallow + 子模块） ##########"
if [ -d "$THEOS" ]; then echo "  $THEOS 已存在"; else
  git clone --depth 1 --recursive https://github.com/theos/theos.git "$THEOS" 2>&1 | tail -3
fi
ls "$THEOS" 2>/dev/null | head -8

echo "########## 2. 获取 iPhoneOS16.5 SDK（theos/sdks，稀疏检出单个 SDK） ##########"
mkdir -p "$THEOS/sdks"
if [ -f "$THEOS/sdks/iPhoneOS16.5.sdk.tar.xz" ]; then
  echo "  已有 tarball"
elif [ -d "$THEOS/sdks/iPhoneOS16.5.sdk" ]; then
  echo "  已有解包 SDK"
else
  cd /tmp
  rm -rf theos-sdks
  GIT_LFS_SKIP_SMUDGE=1 git clone --depth 1 --filter=blob:none --sparse https://github.com/theos/sdks.git theos-sdks 2>&1 | tail -2
  cd theos-sdks && git sparse-checkout set iPhoneOS16.5.sdk.tar.xz 2>&1 | tail -2
  ls -la iPhoneOS16.5.sdk.tar.xz 2>/dev/null || echo "  !! tarball 未取到（可能是 LFS）"
  if [ -s iPhoneOS16.5.sdk.tar.xz ]; then
    cp iPhoneOS16.5.sdk.tar.xz "$THEOS/sdks/"
    cd "$THEOS/sdks" && tar xf iPhoneOS16.5.sdk.tar.xz && echo "  解包完成"
  fi
fi
ls -d "$THEOS"/sdks/*.sdk 2>/dev/null | head -5

echo "########## 3. 构建 Linux cctools（ld64 链接器） ##########"
if [ -x "$THEOS/toolchain/linux/iphone/bin/ld64" ]; then
  echo "  已存在 ld64"
else
  cd /root/hackios
  if [ ! -d cctools-port ]; then
    git clone --depth 1 https://github.com/tpoechtrager/cctools-port.git cctools-port 2>&1 | tail -2
  fi
  cd cctools-port/cctools
  ./configure --prefix=/root/hackios/cctools-build --target=aarch64-apple-darwin 2>&1 | tail -5
  make -j"$(nproc)" 2>&1 | tail -5
  make install 2>&1 | tail -3
  mkdir -p "$THEOS/toolchain/linux/iphone/bin"
  cp -a /root/hackios/cctools-build/bin/* "$THEOS/toolchain/linux/iphone/bin/" 2>/dev/null
  ls "$THEOS/toolchain/linux/iphone/bin" | head -10
fi

echo "########## 4. 拉取 XNU 源码并应用 hackios 补丁 ##########"
K=/root/hackios/hackios-xnu-kernel-umi
if [ -d "$K/xnu-src/xnu-8796.121.2" ]; then
  echo "  xnu-src 已存在"
else
  bash "$K/xnu/apply.sh" 2>&1 | tail -4
fi
ls -d $K/xnu-src/xnu-8796.121.2 2>/dev/null && echo "  XNU 源码就绪"
echo TOOLCHAIN_SETUP_DONE
