#!/bin/bash
# Test the installed-device boot in QEMU, with no phone: a disk laid out like one (boot_a, boot_b with
# Qualcomm A/B attributes, Android active on _b, a darwin partition with a boot bundle in its boot area),
# the loader started with no payload, as the UEFI in boot_a would start it.
#
# usage: common/flash/qemu-test.sh [auto|android|developer]
#   auto       no key: the menu's default after its countdown -- the bundle from darwin boots
#   android    down + enter: the loader makes slot _b active again (the GPT is checked afterwards)
#
# The bundle runs the prebuilt kernel with userland's `hello` as its root (a 20 s session), so the test
# needs no files of iOS.
set -e
cd "$(dirname "$(readlink -f "$0")")/../.."
MODE=${1:-auto}
W=out/qemu-flash; mkdir -p $W/esp/EFI/BOOT
EFI=${QEMU_EFI:-/usr/share/AAVMF/AAVMF_CODE.fd}; [ -f "$EFI" ] || EFI=/usr/share/edk2/aarch64/QEMU_EFI.fd
VARS=${QEMU_VARS:-/usr/share/AAVMF/AAVMF_VARS.fd}; [ -f "$VARS" ] || VARS=/usr/share/edk2/aarch64/QEMU_VARS.fd
L=xnuloader/loader.efi; [ -f $L ] || L=prebuilt/loader.efi
K=xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837; [ -f $K ] || K=prebuilt/mach.development.bcm2837
HELLO=${HELLO:-userland/hello}; [ -f $HELLO ] || HELLO=prebuilt/userland/hello
cp $L $W/esp/EFI/BOOT/BOOTAA64.EFI; cp "$VARS" $W/vars.fd

# the "phone": a sparse 9 GiB disk
rm -f $W/disk.img; truncate -s 9G $W/disk.img
sgdisk -o -n 1:2048:+64M -c 1:boot_a -n 2:0:+64M -c 2:boot_b -n 3:0:0 -c 3:darwin $W/disk.img > /dev/null
# Qualcomm A/B byte (attribute bits 48..55) as `bootctl set-active-boot-slot 0` leaves it before HackiOS boots:
# _a active (bit 50), priority 3 (48, 49), retries left (52, 53); _b, Android's slot, inactive but successful (54)
for bit in 48 49 50 52 53; do sgdisk -A 1:set:$bit $W/disk.img > /dev/null; done
sgdisk -A 2:set:54 $W/disk.img > /dev/null
python3 common/flash/mkbundle.py -o $W/bundle.img --kernel $K --ramdisk $HELLO --cmdline "-v loader_return_after=15" --generation 1
DARWIN_START=$(sgdisk -i 3 $W/disk.img | awk '/First sector/ {print $3}')
dd if=$W/bundle.img of=$W/disk.img bs=512 seek=$((DARWIN_START + 2042368 * 8)) conv=notrunc status=none
echo "qemu-test: before: $(sgdisk -A 1:show $W/disk.img | tr '\n' ' ') / $(sgdisk -A 2:show $W/disk.img | tr '\n' ' ')"

case $MODE in
auto) KEYS="" ;;
android) KEYS=$'\e[B\r' ;;
developer) KEYS=$'\e[B\e[B\r' ;;
esac
rm -f $W/log.txt; touch $W/log.txt
# the keys go in once the menu is on the screen (the firmware takes a while to get there under TCG)
( until grep -qa "HACKiOS boot menu" $W/log.txt; do sleep .5; done; sleep 1; printf '%s' "$KEYS"; sleep 400 ) | timeout ${QEMU_TIMEOUT:-300} qemu-system-aarch64 -M virt,gic-version=3 -cpu cortex-a76 -smp 1 -m 6G \
	-drive if=pflash,format=raw,readonly=on,file="$EFI" -drive if=pflash,format=raw,file=$W/vars.fd \
	-drive file=fat:rw:$W/esp,format=raw,if=virtio -drive file=$W/disk.img,format=raw,if=virtio \
	-display none -serial stdio -monitor none -no-reboot 2>&1 | stdbuf -o0 tr -d '\r' > $W/log.txt || true
grep -aE "menu|flash:|ab:|hello|loader_return_after expired|Darwin Kernel" $W/log.txt | head -40
echo "qemu-test: after: $(sgdisk -A 1:show $W/disk.img | tr '\n' ' ') / $(sgdisk -A 2:show $W/disk.img | tr '\n' ' ')"
sgdisk -v $W/disk.img | grep -iE "problem|no problems" | head -3
