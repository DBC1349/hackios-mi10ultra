# pull_wifi_adsp_fw.ps1 - pull QCA6390 wifi firmware and ADSP firmware from /firmware/image
$d='C:\Users\17125\DoubaoWork\chats\2026-10-23\new-chat-1'
$adb=Join-Path $d 'tools\platform-tools\adb.exe'
$dest=Join-Path $d 'fw-extra'
New-Item -ItemType Directory -Force -Path $dest | Out-Null
"=== 1. list /firmware/image/qca6390 ==="
& $adb shell "ls -la /firmware/image/qca6390/" 2>&1 | Select-Object -First 40
"=== 2. pull the qca6390 dir ==="
& $adb pull /firmware/image/qca6390 (Join-Path $dest 'qca6390') 2>&1 | Select-Object -Last 2
"=== 3. pull adsp/cdsp firmware (at least mdt + a few b*) ==="
$adsp = @('adsp.mdt','adsp.b00','adsp.b01','adsp.b02','adsp.b03','adsp.b04','adsp.b05','adsp.b06','adsp.b07','adsp.b08','adsp.b09','adsp.b10','adsp.b11','adsp.b12','adsp.b13','adsp.b14','adsp.b15','adsp.b16','adsp.b17','adsp.b18','adspr.jsn','adspua.jsn','cdsp.mdt')
New-Item -ItemType Directory -Force -Path (Join-Path $dest 'adsp') | Out-Null
foreach ($f in $adsp) {
  $r = & $adb shell "ls /firmware/image/$f" 2>$null
  if ($r) { & $adb pull "/firmware/image/$f" (Join-Path $dest "adsp\$f") 2>&1 | Out-Null }
}
"=== pulled ==="
Get-ChildItem $dest -Recurse -File | Select-Object FullName,Length | Format-Table -AutoSize | Out-String -Width 160
"PULL_WIFI_ADSP_DONE"