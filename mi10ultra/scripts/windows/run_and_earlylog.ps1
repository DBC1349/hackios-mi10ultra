# run_and_earlylog.ps1 - run payload, then fetch the LIVE log early (uncapped) before any early return
param(
  [string]$Img='cas-touch2.img',
  [string]$SysImg='C:\Users\17125\DoubaoWork\chats\2026-10-23\new-chat-1\system.apfs',
  [string]$Payload='C:\Users\17125\DoubaoWork\chats\2026-10-23\new-chat-1\tools\run-payload-dump3.bin',
  [int]$LogDelay=18,
  [int]$StageTimeout=3600
)
$d='C:\Users\17125\DoubaoWork\chats\2026-10-23\new-chat-1'
$me=$PID
$adb=Join-Path $d 'tools\platform-tools\adb.exe'
$fb=Join-Path $d 'tools\platform-tools\fastboot.exe'
$cmd=Join-Path $d 'winfb_cmd.txt'; $out=Join-Path $d 'winfb_out.txt'
function W([string]$m){ $t=Get-Date -Format HH:mm:ss; Write-Output "[$t] $m" }
function DevIds { (Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -like 'USB\VID_18D1*' }).InstanceId -join ',' }
function LoaderUp { (DevIds) -match 'XNULOADER' }
function SendWait([string]$c,[string]$pat,[int]$timeout){
  $before=(Get-Content $out -ErrorAction SilentlyContinue).Count
  Set-Content $cmd $c
  for ($i=0;$i -lt $timeout;$i++){
    Start-Sleep -Seconds 1
    $lines=Get-Content $out -ErrorAction SilentlyContinue
    if ($lines.Count -gt $before){
      $new=($lines[$before..($lines.Count-1)]) -join "`n"
      if ($new -match $pat) { return "OK" }
      if ($new -match 'ERR:') { return ("ERR " + $new) }
    }
  }
  return "TIMEOUT"
}
function SendStage([string]$file,[int]$stallSec,[int]$maxSec){
  $before=(Get-Content $out -ErrorAction SilentlyContinue).Count
  Set-Content $cmd ("stage " + $file)
  $last=(Get-Date); $start=(Get-Date)
  while ($true){
    Start-Sleep -Seconds 2
    $lines=Get-Content $out -ErrorAction SilentlyContinue
    if ($lines.Count -gt $before){
      $new=($lines[$before..($lines.Count-1)])
      if ($new | Where-Object { $_ -match 'staged \d+ MB' }) { $last=(Get-Date) }
      $t=($new -join "`n")
      if ($t -match 'stage sent \d+ bytes; ack ->') { return "OK" }
      if ($t -match 'ERR:') { return "ERR" }
    }
    if (((Get-Date)-$last).TotalSeconds -gt $stallSec) { return "STALL" }
    if (((Get-Date)-$start).TotalSeconds -gt $maxSec) { return "TIMEOUT" }
  }
}
W ("state: pnp=" + (DevIds))
if (-not (LoaderUp)) {
  & $adb reboot bootloader 2>&1 | Out-Null
  for ($i=0;$i -lt 40;$i++){ if (((& $fb devices 2>$null) -join ' ') -match 'fastboot'){ break }; Start-Sleep -Seconds 2 }
  & $fb boot (Join-Path $d ("tools\"+$Img)) 2>&1 | ForEach-Object { W $_ }
  $up=$false
  for ($i=0;$i -lt 45;$i++){ if (LoaderUp) { $up=$true; break }; Start-Sleep -Seconds 2 }
  W "loader up = $up"
  if (-not $up) { W "loader missing"; exit 1 }
}
W "start daemon v3"
Get-CimInstance Win32_Process -Filter "Name='powershell.exe'" | Where-Object { $_.ProcessId -ne $me -and $_.CommandLine -like '*winfbd*.ps1*' } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
Start-Sleep -Seconds 2
Remove-Item $out,(Join-Path $d 'winfb_ready.txt') -ErrorAction SilentlyContinue
Start-Process powershell -ArgumentList ('-NoProfile -ExecutionPolicy Bypass -File "' + $d + '\tools\winfbd3.ps1"') | Out-Null
for ($i=0;$i -lt 25;$i++){ if (Test-Path (Join-Path $d 'winfb_ready.txt')){ break }; Start-Sleep -Seconds 1 }
W ("getvar: " + (SendWait 'getvar version' 'version ->' 20))
W ("ramdisk: " + (SendWait 'ramdisk' 'oem ramdisk ->' 30))
W ("system.apfs: " + (SendStage $SysImg 120 $StageTimeout))
W ("payload: " + (SendStage $Payload 90 600))
W ("run: " + (SendWait 'run' 'oem run ->' 30))
# --- the important part: fetch the LIVE log early, before any early return ---
foreach ($n in 1..4) {
  Start-Sleep -Seconds $LogDelay
  $before=(Get-Content $out -ErrorAction SilentlyContinue).Count
  Set-Content $cmd 'log'
  Start-Sleep -Seconds 12
  $sz=(Get-Item (Join-Path $d 'win_loader.log') -ErrorAction SilentlyContinue).Length
  Copy-Item (Join-Path $d 'win_loader.log') (Join-Path $d ("early_log_" + $n + ".log")) -Force -ErrorAction SilentlyContinue
  W ("live log attempt $n -> size $sz")
  Set-Content $cmd 'getvar result'; Start-Sleep -Seconds 4
  $st=Get-Content $out | Where-Object { $_ -match 'result ->' } | Select-Object -Last 1
  W ("status: " + $st)
}
W "EARLYLOG_DONE"