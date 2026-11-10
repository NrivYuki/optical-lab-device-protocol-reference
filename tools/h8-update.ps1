#Requires -Version 7
<#
.SYNOPSIS
  H8 の基板の本体(c/h8/oldp-h8.mot)を、DIP スイッチを切り替えずにシリアルで書き換える。

.DESCRIPTION
  基板のスイッチは ON・OFF・ON・ON(FWE=1 のモード 5)にしておく。本体の update コマンドで更新の仕組みへ移り、
  本体の範囲を消して 128 バイトずつ書き、読み直して CRC-32 を比べてから、本体を起動する。

  -Recover: 本体が壊れて update コマンドが使えないとき。合図を送り続けるので、そのあいだに基板のリセットボタンを押す
            (本体が動いていれば reset コマンドで戻るので、ボタンは要らない)。

.EXAMPLE
  pwsh -File tools/h8-update.ps1 -Port COM11
  pwsh -File tools/h8-update.ps1 -Port COM11 -Recover
#>
param(
    [string]$Port = 'COM11',
    [string]$MotFile = (Join-Path $PSScriptRoot '..\c\h8\oldp-h8.mot'),
    [switch]$Recover
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$APP_BASE = 0x1000
$CRC_TABLE = [uint32[]]::new(256)
for ($n = 0; $n -lt 256; $n++) {
    [uint32]$c = $n
    for ($k = 0; $k -lt 8; $k++) { if ($c -band 1) { $c = 0xEDB88320u -bxor ($c -shr 1) } else { $c = $c -shr 1 } }
    $CRC_TABLE[$n] = $c
}
function Get-Crc32([byte[]]$Data, [int]$Offset, [int]$Count) {
    [uint32]$c = 0xFFFFFFFFu
    for ($i = $Offset; $i -lt $Offset + $Count; $i++) { $c = $CRC_TABLE[($c -bxor $Data[$i]) -band 0xFF] -bxor ($c -shr 8) }
    return [uint32]($c -bxor 0xFFFFFFFFu)
}
function ConvertTo-Be32([uint32]$V) { return [byte[]]@((($V -shr 24) -band 255), (($V -shr 16) -band 255), (($V -shr 8) -band 255), ($V -band 255)) }

# ---- S レコードを読み、本体の範囲を 1 枚の並びにする(すき間は 0xFF) ----
$records = foreach ($line in Get-Content -LiteralPath $MotFile) {
    # 番地が 2 バイトなら S1、3 バイトなら S2(本体が 64 KB に収まると S1 になる)
    if ($line -notmatch '^S[12]') { continue }
    $addrBytes = if ($line[1] -eq '1') { 2 } else { 3 }
    $count = [Convert]::ToInt32($line.Substring(2, 2), 16)
    $addr = [Convert]::ToInt32($line.Substring(4, $addrBytes * 2), 16)
    [pscustomobject]@{ Addr = $addr; Hex = $line.Substring(4 + $addrBytes * 2, ($count - $addrBytes - 1) * 2) }
}
if (-not $records) { throw "データのレコードがありません: $MotFile" }
$first = ($records | Measure-Object Addr -Minimum).Minimum
if ($first -lt $APP_BASE) { throw "0x$('{0:X}' -f $first) 番地のデータがあります。本体だけのファイル(0x1000 番地から)を渡してください" }
$end = ($records | ForEach-Object { $_.Addr + $_.Hex.Length / 2 } | Measure-Object -Maximum).Maximum
$len = [int]((($end - $APP_BASE + 127) -band (-bnot 127)))
$image = [byte[]]::new($len)
[Array]::Fill($image, [byte]0xFF)
foreach ($r in $records) {
    $at = $r.Addr - $APP_BASE
    for ($i = 0; $i -lt $r.Hex.Length; $i += 2) { $image[$at + $i / 2] = [Convert]::ToByte($r.Hex.Substring($i, 2), 16) }
}
$imageCrc = Get-Crc32 $image 0 $len
Write-Host ('本体: {0} バイト、CRC-32 0x{1:X8}' -f $len, $imageCrc)

$serial = [System.IO.Ports.SerialPort]::new($Port, 9600, 'None', 8, 'One')
$serial.Open()

function Read-Until([string]$Text, [int]$Seconds) {
    $sb = [Text.StringBuilder]::new()
    $end = (Get-Date).AddSeconds($Seconds)
    $serial.ReadTimeout = 100
    while ((Get-Date) -lt $end) {
        try { [void]$sb.Append([char]$serial.ReadByte()) } catch [TimeoutException] { continue }
        if ($sb.ToString().EndsWith($Text)) { return $sb.ToString() }
    }
    throw "基板から「$Text」が届きません(届いたもの: $($sb.ToString()))"
}
function Wait-Ack([int]$Seconds, [string]$What) {
    $serial.ReadTimeout = $Seconds * 1000
    $b = $serial.ReadByte()
    if ($b -eq [byte][char]'K') { return }
    if ($b -eq [byte][char]'N') {
        $code = $serial.ReadByte()
        if ($code -eq 6) {
            $at = $serial.ReadByte(); $got = $serial.ReadByte(); $want = $serial.ReadByte()
            throw ('{0}: 書いた中身が読み直しと違います(区切りの {1} バイト目、読めた値 0x{2:X2}、書いた値 0x{3:X2}、番号 0x06)' -f $What, $at, $got, $want)
        }
        throw ('{0}: 基板が失敗を返しました(番号 0x{1:X2})' -f $What, $code)
    }
    throw ('{0}: 思っていない返事 0x{1:X2}' -f $What, $b)
}

try {
    $serial.DiscardInBuffer()
    if ($Recover) {
        # 本体が動いていれば reset コマンドで起動の入口へ戻る。動いていなければ、リセットボタンを押す
        $serial.Write("`rreset`r")
        Write-Host '起動の入口へ戻しました。応えなければ基板のリセットボタンを押してください(合図を送り続けます)'
        $until = (Get-Date).AddSeconds(60)
        $sb = [Text.StringBuilder]::new()
        while ($true) {
            if ((Get-Date) -gt $until) { throw '60 秒待っても、更新の仕組みが応えません' }
            $serial.Write('OLDPLOAD')
            Start-Sleep -Milliseconds 30
            while ($serial.BytesToRead -gt 0) { [void]$sb.Append([char]$serial.ReadByte()) }
            if ($sb.ToString().Contains("LOAD`r`n")) { break }
        }
    } else {
        $serial.Write("update`r")
        [void](Read-Until "LOAD`r`n" 5)
    }
    Write-Host '更新の仕組みに移りました。消去します'

    $msg = [byte[]](@([byte][char]'E') + (ConvertTo-Be32 $len) + (ConvertTo-Be32 $imageCrc))
    $serial.Write($msg, 0, $msg.Length)
    Wait-Ack 120 '消去'

    $start = Get-Date
    $written = 0
    $total = [int]($len / 128)
    for ($offset = 0; $offset -lt $len; $offset += 128) {
        $blank = $true
        for ($i = 0; $i -lt 128; $i++) { if ($image[$offset + $i] -ne 0xFF) { $blank = $false; break } }
        if ($blank) { continue }          # 消してあるので書かない
        $body = [byte[]]::new(132)
        [Array]::Copy([byte[]](ConvertTo-Be32 ([uint32]($APP_BASE + $offset))), 0, $body, 0, 4)
        [Array]::Copy($image, $offset, $body, 4, 128)
        $crc = [byte[]](ConvertTo-Be32 (Get-Crc32 $body 0 132))
        for ($try = 1; ; $try++) {
            $serial.Write('W')
            $serial.Write($body, 0, 132)
            $serial.Write($crc, 0, 4)
            try { Wait-Ack 10 ('書き込み 0x{0:X}' -f ($APP_BASE + $offset)); break }
            catch { if ($try -ge 3 -or $_.Exception.Message -notmatch '番号 0x03') { throw } }    # CRC の誤りだけ送り直す
        }
        $written++
        if ($written % 64 -eq 0) { Write-Host ('  {0} / {1} 区切り({2:F0} 秒)' -f $written, $total, ((Get-Date) - $start).TotalSeconds) }
    }
    Write-Host ('書き込み: {0} 区切り({1:F0} 秒)' -f $written, ((Get-Date) - $start).TotalSeconds)

    $serial.Write('V')
    Wait-Ack 60 '読み直し'
    Write-Host '読み直して CRC-32 が一致しました。本体を起動します'
    $serial.Write('G')
    $banner = Read-Until 'oldp> ' 10
    Write-Host ($banner.Trim())
} finally {
    $serial.Close()
}
