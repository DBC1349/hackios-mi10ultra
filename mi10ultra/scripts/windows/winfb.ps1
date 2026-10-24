# winfb.ps1 — WinUsb 直连 xnuloader 的 fastboot 主机工具（不依赖 fastboot.exe / usbipd）
# 用法: winfb.ps1 log | memmap | screenshot | stage <file> | run [N] | result | reboot | getvar <name>
param([string]$cmd='log', [string]$arg1='')
$ErrorActionPreference='Stop'
Add-Type -Path 'C:\Users\17125\DoubaoWork\chats\2026-10-23\new-chat-1\tools\WU2.cs'
$out='C:\Users\17125\DoubaoWork\chats\2026-10-23\new-chat-1'
function L32 { [System.Runtime.InteropServices.Marshal]::GetLastWin32Error() }

$path='\\?\USB#VID_18D1&PID_D00D#xnuloader#{f72fe0d4-cbcb-407d-8814-9ed673d0dd6e}'
$h=[WU2]::CreateFileW($path, [uint32]::Parse('C0000000','HexNumber'), 3, [IntPtr]::Zero, 3, 0x40000000, [IntPtr]::Zero)
if ($h -eq [IntPtr]::new(-1)) { throw "CreateFile failed err=$(L32)" }
$wh=[IntPtr]::Zero
if (-not [WU2]::WinUsb_Initialize($h, [ref]$wh)) { throw "WinUsb_Initialize failed err=$(L32)" }
$EPIN=[byte]0x81; $EPOUT=[byte]0x01
$tm=[uint32]15000
[WU2]::WinUsb_SetPipePolicy($wh, $EPIN, 0x03, 4, [ref]$tm) | Out-Null
[WU2]::WinUsb_SetPipePolicy($wh, $EPOUT, 0x03, 4, [ref]$tm) | Out-Null
# 上次关闭句柄取消的 pending 传输可能让端点 halt：先 abort+reset+flush 两端点
[WU2]::WinUsb_AbortPipe($wh, $EPIN) | Out-Null
[WU2]::WinUsb_AbortPipe($wh, $EPOUT) | Out-Null
[WU2]::WinUsb_ResetPipe($wh, $EPIN) | Out-Null
[WU2]::WinUsb_ResetPipe($wh, $EPOUT) | Out-Null
[WU2]::WinUsb_FlushPipe($wh, $EPIN) | Out-Null

function SendCmd([string]$c) {
  $b=[System.Text.Encoding]::ASCII.GetBytes($c)
  $w=[uint32]0
  if (-not [WU2]::WinUsb_WritePipe($script:wh, $script:EPOUT, $b, [uint32]$b.Length, [ref]$w, [IntPtr]::Zero)) { throw "write '$c' failed err=$(L32)" }
}
function ReadResp([int]$max=64) {
  $b=New-Object byte[] $max
  $r=[uint32]0
  if (-not [WU2]::WinUsb_ReadPipe($script:wh, $script:EPIN, $b, [uint32]$max, [ref]$r, [IntPtr]::Zero)) { throw "read failed err=$(L32)" }
  [System.Text.Encoding]::ASCII.GetString($b,0,[int]$r)
}
function Cmd([string]$c) {
  SendCmd $c
  $resp=ReadResp
  $resp
}
function GetStaged([string]$file) {
  SendCmd 'upload'
  $resp=ReadResp
  if (-not $resp.StartsWith('DATA')) { throw "get_staged: unexpected '$resp'" }
  $len=[Convert]::ToInt64($resp.Substring(4),16)
  $fs=[System.IO.File]::Create($file)
  $got=0
  $buf=New-Object byte[] 1048576
  while ($got -lt $len) {
    $want=[uint32][Math]::Min(1048576, $len-$got)
    $r=[uint32]0
    if (-not [WU2]::WinUsb_ReadPipe($script:wh, $script:EPIN, $buf, $want, [ref]$r, [IntPtr]::Zero)) { throw "staged read failed at $got err=$(L32)" }
    $fs.Write($buf,0,[int]$r)
    $got+=[int]$r
  }
  $fs.Close()
  # upload 分支在 raw chunk 之后还回一个 OKAY，读掉丢弃
  try { ReadResp 64 | Out-Null } catch { }
  $len
}

switch ($cmd) {
  'log' {
    $r=Cmd 'oem log'; Write-Output "oem log -> $r"
    $n=GetStaged "$out\win_loader.log"
    Write-Output "staged $n bytes -> win_loader.log"
    Get-Content "$out\win_loader.log" | Select-Object -Last 50
  }
  'memmap' {
    $r=Cmd 'oem memmap'; Write-Output "oem memmap -> $r"
    $r=Cmd 'oem log'; Write-Output "oem log -> $r"
    $n=GetStaged "$out\win_memmap.log"
    Write-Output "staged $n bytes -> win_memmap.log"
    $m=Get-Content "$out\win_memmap.log"
    $start=-1
    for ($i=0;$i -lt $m.Count;$i++) { if ($m[$i] -match 'memory map') { $start=$i; break } }
    if ($start -ge 0) { $m[$start..([Math]::Min($start+80,$m.Count-1))] } else { $m | Select-Object -Last 80 }
  }
  'screenshot' {
    $r=Cmd 'oem screenshot'; Write-Output "oem screenshot -> $r"
    $n=GetStaged "$out\win_screen.raw"
    Write-Output "framebuffer $n bytes -> win_screen.raw"
  }
  'stage' {
    $data=[System.IO.File]::ReadAllBytes($arg1)
    $hex=$data.Length.ToString('x8')
    $r=Cmd "download:$hex"; Write-Output "download: -> $r"
    $off=0
    while ($off -lt $data.Length) {
      $chunk=[Math]::Min(1048576, $data.Length-$off)
      $w=[uint32]0
      $seg=New-Object byte[] $chunk
      [Array]::Copy($data,$off,$seg,0,$chunk)
      if (-not [WU2]::WinUsb_WritePipe($wh,$EPOUT,$seg,[uint32]$chunk,[ref]$w,[IntPtr]::Zero)) { throw "payload write failed at $off err=$(L32)" }
      $off+=$chunk
    }
    $resp=ReadResp
    Write-Output "payload ack -> $resp"
  }
  'run' {
    $c='oem run'; if ($arg1) { $c="oem run $arg1" }
    $tm2=[uint32]600000
    [WU2]::WinUsb_SetPipePolicy($wh,$EPIN,0x03,4,[ref]$tm2) | Out-Null
    $r=Cmd $c; Write-Output "$c -> $r"
  }
  'result' {
    $tm2=[uint32]600000
    [WU2]::WinUsb_SetPipePolicy($wh,$EPIN,0x03,4,[ref]$tm2) | Out-Null
    $r=Cmd 'getvar result'; Write-Output "result -> $r"
  }
  'reboot' { $r=Cmd 'reboot'; Write-Output "reboot -> $r" }
  'getvar' { $r=Cmd "getvar:$arg1"; Write-Output "$arg1 -> $r" }
  default { throw "unknown cmd $cmd" }
}
[WU2]::WinUsb_Free($wh) | Out-Null
[WU2]::CloseHandle($h) | Out-Null
Write-Output "WINFB_DONE"
