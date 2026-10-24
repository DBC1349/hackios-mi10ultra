# winfbd3.ps1 - resident daemon with ramdisk support (oem ramdisk + big staging)
$ErrorActionPreference='Stop'
Add-Type -Path 'C:\Users\17125\DoubaoWork\chats\2026-10-23\new-chat-1\tools\WU2.cs'
$dir='C:\Users\17125\DoubaoWork\chats\2026-10-23\new-chat-1'
$cmdfile=Join-Path $dir 'winfb_cmd.txt'; $outfile=Join-Path $dir 'winfb_out.txt'
function L32 { [System.Runtime.InteropServices.Marshal]::GetLastWin32Error() }
function Log([string]$m) { "$(Get-Date -Format HH:mm:ss) $m" | Add-Content $outfile }
$path='\\?\USB#VID_18D1&PID_D00D#xnuloader#{f72fe0d4-cbcb-407d-8814-9ed673d0dd6e}'
$h=[IntPtr]::new(-1)
for ($try=0; $try -lt 30; $try++) {
  $h=[WU2]::CreateFileW($path, [uint32]::Parse('C0000000','HexNumber'), 3, [IntPtr]::Zero, 3, 0x40000000, [IntPtr]::Zero)
  if ($h -ne [IntPtr]::new(-1)) { break }
  Log "CreateFile try $try failed err=$(L32)"; Start-Sleep -Seconds 1
}
if ($h -eq [IntPtr]::new(-1)) { Log "CreateFile failed permanently"; exit 1 }
$wh=[IntPtr]::Zero
if (-not [WU2]::WinUsb_Initialize($h, [ref]$wh)) { Log "init failed err=$(L32)"; exit 1 }
$EPIN=[byte]0x81; $EPOUT=[byte]0x01
$tm=[uint32]8000
[WU2]::WinUsb_SetPipePolicy($wh,$EPIN,0x03,4,[ref]$tm) | Out-Null
[WU2]::WinUsb_SetPipePolicy($wh,$EPOUT,0x03,4,[ref]$tm) | Out-Null
Log "daemon3 up (ramdisk+stage+run)"
'up' | Set-Content (Join-Path $dir 'winfb_ready.txt')

function SendCmd([string]$c) {
  $b=[System.Text.Encoding]::ASCII.GetBytes($c); $w=[uint32]0
  if (-not [WU2]::WinUsb_WritePipe($wh,$EPOUT,$b,[uint32]$b.Length,[ref]$w,[IntPtr]::Zero)) { throw "write '$c' err=$(L32)" }
}
function ReadResp([int]$max=64) {
  $b=New-Object byte[] $max; $r=[uint32]0
  if (-not [WU2]::WinUsb_ReadPipe($wh,$EPIN,$b,[uint32]$max,[ref]$r,[IntPtr]::Zero)) { throw "read err=$(L32)" }
  [System.Text.Encoding]::ASCII.GetString($b,0,[int]$r)
}
function Cmd([string]$c) { SendCmd $c; ReadResp }
function GetStaged([string]$file) {
  SendCmd 'upload'
  $resp=ReadResp
  if (-not $resp.StartsWith('DATA')) { throw "upload resp '$resp'" }
  $len=[Convert]::ToInt64($resp.Substring(4),16)
  $fs=[System.IO.File]::Create($file); $got=0; $buf=New-Object byte[] 1048576
  while ($got -lt $len) {
    $want=[uint32][Math]::Min(1048576,$len-$got); $r=[uint32]0
    if (-not [WU2]::WinUsb_ReadPipe($wh,$EPIN,$buf,$want,[ref]$r,[IntPtr]::Zero)) { throw "staged read err=$(L32)" }
    $fs.Write($buf,0,[int]$r); $got+=[int]$r
  }
  $fs.Close()
  try { ReadResp | Out-Null } catch { }
  $len
}
function Stage([string]$file) {
  $len=(Get-Item $file).Length
  $r=Cmd ("download:"+$len.ToString('x8')); Log "download:$($len.ToString('x8')) -> $r"
  $fs=[System.IO.File]::OpenRead($file)
  $buf=New-Object byte[] 1048576; $sent=0
  while ($true) {
    $n=$fs.Read($buf,0,$buf.Length); if ($n -le 0) { break }
    $seg=New-Object byte[] $n; [Array]::Copy($buf,0,$seg,0,$n); $w=[uint32]0
    if (-not [WU2]::WinUsb_WritePipe($wh,$EPOUT,$seg,[uint32]$n,[ref]$w,[IntPtr]::Zero)) { $fs.Close(); throw "stage write err=$(L32) at $sent" }
    $sent+=$n
    if (($sent % (64*1048576)) -lt 1048576) { Log "  staged $([math]::Round($sent/1MB)) MB" }
  }
  $fs.Close()
  Log "stage sent $sent bytes; ack -> $(ReadResp)"
}

Remove-Item $cmdfile -ErrorAction SilentlyContinue
while ($true) {
  if (-not (Test-Path $cmdfile)) { Start-Sleep -Milliseconds 300; continue }
  $lines=Get-Content $cmdfile
  Remove-Item $cmdfile -ErrorAction SilentlyContinue
  foreach ($line in $lines) {
    $line=$line.Trim(); if (-not $line) { continue }
    Log "cmd: $line"
    $parts=$line -split '\s+',2; $c=$parts[0]; $a=if ($parts.Count -gt 1) { $parts[1] } else { '' }
    try {
      switch ($c) {
        'log'        { $r=Cmd 'oem log'; Log "oem log -> $r"; $n=GetStaged (Join-Path $dir 'win_loader.log'); Log "staged $n bytes" }
        'memmap'     { $r=Cmd 'oem memmap'; Log "memmap -> $r"; $r=Cmd 'oem log'; $n=GetStaged (Join-Path $dir 'win_memmap.log'); Log "staged $n bytes" }
        'screenshot' { $r=Cmd 'oem screenshot'; Log "screenshot -> $r"; $n=GetStaged (Join-Path $dir 'win_screen.raw'); Log "screen $n bytes" }
        'getvar'     { Log "$a -> $(Cmd "getvar:$a")" }
        'ramdisk'    { if ($a -eq 'none') { Log "ramdisk none -> $(Cmd 'oem ramdisk none')" } else { Log "oem ramdisk -> $(Cmd 'oem ramdisk')" } }
        'stage'      { Stage $a }
        'run'        { $cc='oem run'; if ($a) { $cc="oem run $a" }; $tm2=[uint32]600000; [WU2]::WinUsb_SetPipePolicy($wh,$EPIN,0x03,4,[ref]$tm2)|Out-Null; Log "$cc -> $(Cmd $cc)" }
        'result'     { $tm2=[uint32]600000; [WU2]::WinUsb_SetPipePolicy($wh,$EPIN,0x03,4,[ref]$tm2)|Out-Null; Log "result -> $(Cmd 'getvar result')" }
        'stop'       { Log "stop -> $(Cmd 'oem stop')" }
        'reboot'     { Log "reboot -> $(Cmd 'reboot')" }
        'quit'       { Log "quit"; exit 0 }
        default      { Log "unknown cmd: $c" }
      }
    } catch { Log "ERR: $_" }
  }
}
