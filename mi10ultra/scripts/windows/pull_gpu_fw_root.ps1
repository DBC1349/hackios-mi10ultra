# pull_gpu_fw_root.ps1 - with root: list and pull the A650 firmware (ASCII)
$d='C:\Users\17125\DoubaoWork\chats\2026-10-23\new-chat-1'
$adb=Join-Path $d 'tools\platform-tools\adb.exe'
$dest=Join-Path $d 'gpu-fw'
New-Item -ItemType Directory -Force -Path $dest | Out-Null
function Su([string]$c){ & $adb shell "su -c '$c'" 2>&1 }

"=== 1. root available? ==="
Su 'id'
"=== 2. firmware dirs (as root) ==="
Su 'ls -la /vendor/firmware_mnt/image/ | head -50'
Su 'ls -la /vendor/firmware/ | head -30'
"=== 3. find a650 / gmu / sqe / zap (as root) ==="
Su 'find /vendor /system /odm /firmware -iname "a650*" 2>/dev/null | head -40'
Su 'find /vendor /firmware -iname "*gmu*" -o -iname "*sqe*" -o -iname "*zap*" 2>/dev/null | head -40'
"=== 4. try to pull the usual suspects ==="
$files = @('a650_gmu.bin','a650_sqe.fw','a650_zap.mdt','a650_zap.b00','a650_zap.b01','a650_zap.b02','a650_zap.b03','a650_zap.b04','a650_zap.b05','a650_zap.b06','a650_zap.b07','a650_zap.b08','a650_zap.b09','a650_zap.b10','a650_zap.b11','a650_zap.b12','a650_zap.b13','a650_zap.b14','a650_zap.b15','a650_zap.b16','a650_zap.b17','a650_zap.b18')
$bases = @('/vendor/firmware_mnt/image','/vendor/firmware','/firmware/image','/system/etc/firmware')
foreach ($b in $bases) {
  foreach ($f in $files) {
    $r = Su "ls -la $b/$f 2>/dev/null"
    if ($r -match '^-' -or $r -match ' rw') {
      Write-Output ("FOUND: $b/$f")
      & $adb shell "su -c 'cat $b/$f'" > (Join-Path $dest $f) 2>$null
      $sz = (Get-Item (Join-Path $dest $f) -ErrorAction SilentlyContinue).Length
      Write-Output ("  pulled " + $sz + " bytes")
    }
  }
}
"=== 5. pulled files ==="
Get-ChildItem $dest -ErrorAction SilentlyContinue | Select-Object Name,Length | Format-Table -AutoSize
"PULL_ROOT_DONE"