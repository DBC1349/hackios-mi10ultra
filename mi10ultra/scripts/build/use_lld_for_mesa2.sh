#!/bin/bash
# use_lld_for_mesa2.sh — 包装改用 -fuse-ld=lld，然后构建 Mesa
set -u
export K=/root/hackios/hackios-xnu-kernel-umi
HB=$K/xnu-deps/host/bin
echo "########## 1. 重写包装（-fuse-ld=lld） ##########"
for d in clang clang++; do
  out=/root/hackios/lldbin/xnu-$d
  cat > "$out" <<EOF
#!/bin/bash
# xnu-clang variant: links with LLVM lld (ld64.lld) which reads the SDK's .tbd stubs.
# The cctools ld64 built here has no TAPI_SUPPORT, so it cannot read .tbd at all.
HB=$HB
case "\$(basename "\$0")" in *++) drv=clang++; extra=(-fno-use-cxa-atexit);; *) drv=clang; extra=();; esac
args=()
for a in "\$@"; do
  case "\$a" in
    -enable-trivial-auto-var-init-zero-knowing-it-will-be-removed-from-clang) ;;
    -fapple-kext-vtable-verify|-Wno-vla-cxx-extension) ;;
    -Werror|-Werror=*|-Weverything|-pedantic-errors) ;;
    *) args+=("\$a");;
  esac
done
exec /usr/bin/\$drv -include "\$HB/../../hostinc/xnu_clang_compat.h" -target arm64-apple-ios\${XNU_SDK_VERSION:-16.5} \\
  -fuse-ld=lld -w "\${extra[@]}" "\${args[@]}"
EOF
  chmod +x "$out"
done
echo "--- 测试 ---"
SDK=/root/theos/sdks/iPhoneOS16.5.sdk
printf '#include <stdio.h>\nint main(){printf("ok\\n");return 0;}\n' > /tmp/m.c
/root/hackios/lldbin/xnu-clang -arch arm64 -isysroot $SDK /tmp/m.c -o /tmp/mwrap 2>&1 | head -6
[ -f /tmp/mwrap ] && { echo "  ✓ 包装可用"; file /tmp/mwrap | head -1; } || { echo "  ✗ 仍失败"; exit 1; }
echo
echo "########## 2. 构建 Mesa（后台会继续） ##########"
cd $K
date -u +%FT%TZ
bash tools/mk-mesa.sh > /root/hackios/logs/mesa-build9.log 2>&1
echo "rc=$?"
grep -n -E 'ERROR|error:|FAILED|Problem encountered' /root/hackios/logs/mesa-build9.log | head -12
tail -15 /root/hackios/logs/mesa-build9.log
ls -la $K/xnu-deps/build/mesa/out/ 2>/dev/null | head -8
echo MESA_LLD2_DONE