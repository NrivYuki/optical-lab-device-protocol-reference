#Requires -Version 7
<#
.SYNOPSIS
  H8 の基板の試験用の組み立て(c/h8 で make trace)で、乱数に混ぜる文字の到着の時刻(TCNT0 の下位 8 ビット)を集める。

.DESCRIPTION
  基板の rngtrace コマンドへ、長さがばらばらな文字の並び(1-255 のバイトを 8-300 個、0x00 で区切る)を送り、
  区切りごとに基板の返事('A')を待ってから次を送る。UART のトランスポートで、フレームを送って ACK を待つのと同じ形である。
  基板は、文字を受けるたびに TCNT0 を記録し、4096 個たまったら 'D' と記録を返す。記録は -OutFile に足していく。
  集めた値は tools/rng-estimate.c で見積もる。

  -Restarts: SP 800-90B の再起動のテスト(3.1.4)。基板を reset コマンドで起動し直し、起動した直後からの値を 1000 個ずつ、
             指定した回数だけ集める。-OutFile に 1 回 1000 バイトずつ足し、最初の文字の TCNT0(16 ビット)を -FirstFile に足す。
  -Dump:     基板の rngdump コマンドで、直前のセッションの間に記録した値を受け取り、-OutFile に足す(tools/h8-rngsession.js が使う)。
  -Lan:      LAN で受けたパケットで混ぜる値を集める。基板の rnglan コマンドの後、基板のアドレス(-LanHost)へ UDP のパケットを、
             1-20 個のまとまりで、まとまりの間を 0-5 ms のばらばらな長さにして送り続ける。基板は記録が 4096 個たまるたびに返す。

.EXAMPLE
  pwsh -File tools/h8-rngtrace.ps1 -Port COM11 -Blocks 245
  pwsh -File tools/h8-rngtrace.ps1 -Port COM11 -Restarts 1000 -OutFile tools/.h8-restart.bin
  pwsh -File tools/h8-rngtrace.ps1 -Port COM11 -Lan -Blocks 245 -OutFile tools/.h8-rnglan.bin
#>
param(
    [string]$Port = 'COM11',
    [int]$BaudRate = 9600,
    [int]$Blocks = 245,
    [int]$Restarts = 0,
    [switch]$Dump,
    [switch]$Lan,
    [string]$LanHost = '169.254.74.50',
    [string]$OutFile = (Join-Path $PSScriptRoot '.h8-rngtrace.bin'),
    [string]$FirstFile = (Join-Path $PSScriptRoot '.h8-restart-first.bin')
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$TR_MAX = 4096

function Read-Until($Serial, [string]$Text, [int]$Seconds) {
    $sb = [Text.StringBuilder]::new()
    $end = (Get-Date).AddSeconds($Seconds)
    $Serial.ReadTimeout = 100
    while ((Get-Date) -lt $end) {
        try { [void]$sb.Append([char]$Serial.ReadByte()) } catch [TimeoutException] { continue }
        if ($sb.ToString().EndsWith($Text)) { return $sb.ToString() }
    }
    throw "基板から「$Text」が届きません(届いたもの: $($sb.ToString()))"
}

function Send-Burst($Serial) {
    $n = $rand.Next(8, 301)
    $burst = [byte[]]::new($n + 1)
    for ($i = 0; $i -lt $n; $i++) { $burst[$i] = [byte]$rand.Next(1, 256) }
    $Serial.Write($burst, 0, $burst.Length)
}

function Read-Exact($Serial, [int]$Count) {
    $buf = [byte[]]::new($Count)
    $got = 0
    while ($got -lt $Count) { $got += $Serial.Read($buf, $got, $Count - $got) }
    return $buf
}

$rand = [Random]::new()
$serial = [System.IO.Ports.SerialPort]::new($Port, $BaudRate, 'None', 8, 'One')
$serial.Open()
$out = [IO.File]::Open($OutFile, 'Append', 'Write')
if ($Dump) {
    try {
        $serial.DiscardInBuffer()
        $serial.Write("id`r")
        [void](Read-Until $serial 'oldp> ' 5)
        $serial.Write("rngdump`r")
        $serial.ReadTimeout = 5000
        while ($serial.ReadByte() -ne [byte][char]'D') { }
        $h = Read-Exact $serial 2
        $n = $h[0] * 256 + $h[1]
        if ($n) { $d = Read-Exact $serial $n; $out.Write($d, 0, $n) }
        [void](Read-Until $serial 'oldp> ' 5)
        Write-Host "記録 $n 個"
    } finally { $out.Close(); $serial.Close() }
    return
}
if ($Lan) {
    $udp = [System.Net.Sockets.UdpClient]::new()
    $dest = [System.Net.IPEndPoint]::new([System.Net.IPAddress]::Parse($LanHost), 9)
    $sw = [Diagnostics.Stopwatch]::new()
    try {
        $serial.DiscardInBuffer()
        $serial.Write("id`r")
        [void](Read-Until $serial 'oldp> ' 5)
        $serial.Write("rnglan $Blocks`r")
        [void](Read-Until $serial "ready`r`n" 5)
        $start = Get-Date
        $done = 0
        $pkt = [byte[]]::new(64)
        while ($done -lt $Blocks) {
            for ($k = $rand.Next(1, 21); $k -gt 0; $k--) { $rand.NextBytes($pkt); [void]$udp.Send($pkt, $rand.Next(18, 64), $dest) }
            $sw.Restart(); $gap = $rand.NextDouble() * 5
            while ($sw.Elapsed.TotalMilliseconds -lt $gap) { }
            if ($serial.BytesToRead -gt 0) {
                $serial.ReadTimeout = 5000
                if ($serial.ReadByte() -ne [byte][char]'D') { throw '基板から思わない返事' }
                $buf = Read-Exact $serial $TR_MAX
                $out.Write($buf, 0, $TR_MAX); $out.Flush()
                $done++
                if ($done % 10 -eq 0 -or $done -eq $Blocks) { Write-Host ("{0}/{1} 回({2:N0} 秒)" -f $done, $Blocks, ((Get-Date) - $start).TotalSeconds) }
            }
        }
        [void](Read-Until $serial 'oldp> ' 10)
    } finally { $udp.Close(); $out.Close(); $serial.Close() }
    return
}
if ($Restarts) {
    $first = [IO.File]::Open($FirstFile, 'Append', 'Write')
    try {
        $start = Get-Date
        for ($r = 1; $r -le $Restarts; $r++) {
            $serial.DiscardInBuffer()
            $serial.Write("id`r")
            [void](Read-Until $serial 'oldp> ' 5)
            $serial.Write("reset`r")
            [void](Read-Until $serial 'oldp> ' 10)   # 起動した後の表示
            $serial.Write("rngboot 1000`r")
            [void](Read-Until $serial "ready`r`n" 5)
            $serial.ReadTimeout = 5000
            for (;;) {
                Send-Burst $serial
                $c = $serial.ReadByte()
                if ($c -eq [byte][char]'A') { continue }
                if ($c -ne [byte][char]'D') { throw "基板から思わない返事: $c" }
                break
            }
            $f = Read-Exact $serial 2
            $d = Read-Exact $serial 1000
            $first.Write($f, 0, 2); $first.Flush()
            $out.Write($d, 0, 1000); $out.Flush()
            [void](Read-Until $serial 'oldp> ' 5)
            if ($r % 50 -eq 0 -or $r -eq $Restarts) { Write-Host ("{0}/{1} 回({2:N0} 秒)" -f $r, $Restarts, ((Get-Date) - $start).TotalSeconds) }
        }
    } finally {
        $first.Close(); $out.Close(); $serial.Close()
    }
    return
}
try {
    $serial.DiscardInBuffer()
    $serial.Write("id`r")
    [void](Read-Until $serial 'oldp> ' 5)
    $serial.Write("rngtrace $Blocks`r")
    [void](Read-Until $serial "ready`r`n" 5)
    $start = Get-Date
    $serial.ReadTimeout = 5000
    $done = 0
    while ($done -lt $Blocks) {
        Send-Burst $serial
        $c = $serial.ReadByte()
        if ($c -eq [byte][char]'A') { continue }
        if ($c -ne [byte][char]'D') { throw "基板から思わない返事: $c" }
        $buf = Read-Exact $serial $TR_MAX
        $out.Write($buf, 0, $TR_MAX)
        $out.Flush()
        $done++
        if ($done % 10 -eq 0 -or $done -eq $Blocks) {
            Write-Host ("{0}/{1} 回({2:N0} 秒)" -f $done, $Blocks, ((Get-Date) - $start).TotalSeconds)
        }
    }
    [void](Read-Until $serial 'oldp> ' 10)
    Write-Host "集めた値: $($out.Length) 個($OutFile)"
} finally {
    $out.Close()
    $serial.Close()
}
