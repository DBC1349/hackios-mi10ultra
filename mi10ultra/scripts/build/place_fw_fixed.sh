#!/bin/bash
set -u
export K=/root/hackios/hackios-xnu-kernel-umi
EXT=/mnt/c/Users/17125/DoubaoWork/chats/2026-10-23/new-chat-1/fw-extra
echo "=== 1. fw-* 的拷贝逻辑（build.sh 里 cp 的地方） ==="
grep -n -B4 -A10 'fw-\$d\|fw-\*' $K/userland/build.sh | head -40
echo
echo "=== 2. 内核里 haptic 固件的文件名 ==="
grep -n -i 'aw8697\|haptic.bin\|fw/' $K/xnu/files/bsd/dev/hk_haptic.c 2>/dev/null | head -12
echo
echo "=== 3. 放置固件（正确布局） ==="
mkdir -p $K/fw-wifi/qca6390 $K/fw-haptic
cp -a $EXT/qca6390/amss20.bin $EXT/qca6390/m3.bin $EXT/qca6390/bd_j1gl.elf $EXT/qca6390/regdb.bin $K/fw-wifi/qca6390/ 2>&1
cp -a $EXT/qca6390/. $K/fw-wifi/qca6390/ 2>/dev/null
ls -la $K/fw-wifi/qca6390/ | head -8
# haptic: 从手机 /vendor/firmware 拉过的 aw8697
for f in aw8697_haptic.bin aw8697_rtp_1.bin; do
  src=/mnt/c/Users/17125/DoubaoWork/chats/2026-10-23/new-chat-1/gpu-fw/$f
  [ -f "$src" ] && cp -a "$src" $K/fw-haptic/ && echo "  haptic: $f"
done
echo "--- fw-* 目录总览 ---"
for d in gpu wifi haptic camera audio; do
  if [ -d $K/fw-$d ]; then echo "  fw-$d: $(find $K/fw-$d -type f | wc -l) 个文件"; else echo "  fw-$d: 无"; fi
done
echo PLACE_FW_FIXED_DONE