#Requires -Version 7
<#
.SYNOPSIS
  H8/3069F の内蔵フラッシュに、S レコード形式(.mot)のプログラムを書き込む。

.DESCRIPTION
  KOZOS H8/3069F Flash Writer(kz_h8write 0.2.1)を PowerShell に移したもの。
  基板の DIP スイッチを書き込みの位置にして電源を入れ(またはリセットし)、実行する。

  書き込みの手順(チップの書き込みモードのプログラムとのやり取り):
    1. 速さ合わせ: 0x00 を送り、0x00 が返ったら 0x55 を送る。0xE6 なら成功、0xFF なら失敗
    2. デバイスの問い合わせ(0x20)と選択(0x10)
    3. クロックモードの問い合わせ(0x21)と選択(0x11)
    4. 新しい速さの選択(0x3F)。ACK のあと、互いに ACK を送り合って確かめる
    5. 書き込み・消去の状態へ移る(0x40)。このときフラッシュ全体が消去される
    6. ユーザー領域への書き込みを選ぶ(0x43)
    7. 128 バイトずつ書き込む(0x50 とアドレス 4 バイト)。最後にアドレス 0xFFFFFFFF で終える

.PARAMETER MotFile
  書き込む S レコード形式のファイル。

.PARAMETER Port
  シリアルポート名(COM3 など)。

.PARAMETER ClockMHz
  基板のクロック(20 または 25)。

.PARAMETER BaudRate
  通信の速さ。速さ合わせと書き込みの両方をこの速さで行う。

.EXAMPLE
  pwsh -File kz_h8write.ps1 -MotFile kzload.mot -Port COM3

.NOTES
  元のプログラムの著作権表示と許諾の文:

  Copyright (c) 2010-2012 Shinichiro Nakamura

  Permission is hereby granted, free of charge, to any person
  obtaining a copy of this software and associated documentation
  files (the "Software"), to deal in the Software without
  restriction, including without limitation the rights to use,
  copy, modify, merge, publish, distribute, sublicense, and/or
  sell copies of the Software, and to permit persons to whom the
  Software is furnished to do so, subject to the following
  conditions:

  The above copyright notice and this permission notice shall be
  included in all copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
  EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
  OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
  NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
  HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
  WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
  FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
  OTHER DEALINGS IN THE SOFTWARE.
#>
[CmdletBinding()]
param(
    [string]$MotFile,
    [string]$Port,
    [ValidateSet(20, 25)][int]$ClockMHz = 20,
    [int]$BaudRate = 19200
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$ACK = 0x06
$FLASH_SIZE = 0x80000       # H8/3069F の内蔵フラッシュは 512 KB
$BLOCK = 128                # 1 回の書き込みの大きさ
$TIMEOUT_MS = 500           # 1 文字を待つ時間
$ERASE_MS = 40000           # 消去にかかる最長の時間(元のプログラムの値)
$WRITE_MS = 3000            # 128 バイトの書き込みにかかる最長の時間(元のプログラムの値)

# ---- 通信に依存しない部分(テストから読み込んで使う) ----------------------------

<#
  S レコード形式の行の並びを読み、フラッシュの像(消去後と同じ 0xFF で埋めたバイト列)と、
  データのある最後のアドレスの次を返す。S1・S2・S3 だけをデータとして扱い、ほかの種類は読み飛ばす。
#>
function Read-MotImage {
    param([string[]]$Lines, [int]$Size = $FLASH_SIZE)
    $image = [byte[]]::new($Size)
    [Array]::Fill($image, [byte]0xFF)
    $last = 0
    $n = 0
    foreach ($raw in $Lines) {
        $n++
        $line = $raw.Trim()
        if ($line -eq '') { continue }
        if ($line -notmatch '^S([0-9])((?:[0-9A-Fa-f]{2})+)$') { throw "${n} 行目: S レコードの形式ではありません" }
        $type = [int]$Matches[1]
        $bytes = [Convert]::FromHexString($Matches[2])
        $count = $bytes[0]
        if ($count -ne $bytes.Length - 1) { throw "${n} 行目: バイト数 $count と行の長さが合いません" }
        $sum = 0
        for ($i = 0; $i -lt $bytes.Length - 1; $i++) { $sum += $bytes[$i] }
        if (((-bnot $sum) -band 0xFF) -ne $bytes[-1]) { throw "${n} 行目: チェックサムが合いません" }
        $alen = @{ 1 = 2; 2 = 3; 3 = 4 }[$type]
        if (-not $alen) { continue }
        $addr = 0
        for ($i = 1; $i -le $alen; $i++) { $addr = ($addr -shl 8) -bor $bytes[$i] }
        $dlen = $count - $alen - 1
        if ($addr + $dlen -gt $Size) {
            throw ('{0} 行目: アドレス 0x{1:X} から {2} バイトはフラッシュの範囲(0x{3:X} まで)を超えます' -f $n, $addr, $dlen, $Size)
        }
        [Array]::Copy($bytes, 1 + $alen, $image, $addr, $dlen)
        if ($addr + $dlen -gt $last) { $last = $addr + $dlen }
    }
    [pscustomobject]@{ Image = $image; End = $last }
}

<# 合計が 0 になるように付ける 1 バイトのチェックサム。 #>
function Get-Checksum {
    param([byte[]]$Bytes)
    $sum = 0
    foreach ($b in $Bytes) { $sum += $b }
    [byte]((0x100 - ($sum -band 0xFF)) -band 0xFF)
}

<# コマンド・データの大きさ・データ・チェックサムの順に並べた要求。 #>
function New-Request {
    param([byte]$Command, [byte[]]$Data = @())
    $body = [byte[]](@($Command, $Data.Length) + $Data)
    [byte[]]($body + (Get-Checksum $body))
}

<# 128 バイトの書き込み(0x50)。Data を省くと書き込みの終わり(アドレス 0xFFFFFFFF)になる。 #>
function New-ProgramPacket {
    param([uint32]$Address, [byte[]]$Data = @())
    $body = [byte[]](@(0x50, (($Address -shr 24) -band 0xFF), (($Address -shr 16) -band 0xFF), (($Address -shr 8) -band 0xFF), ($Address -band 0xFF)) + $Data)
    [byte[]]($body + (Get-Checksum $body))
}

<# 0x3F で送る新しい速さ: 速さ / 100 と、クロック(MHz)× 100 を 2 バイトずつ。続く 3 バイトは逓倍比(元のプログラムの値)。 #>
function Get-BitrateData {
    param([int]$BaudRate, [int]$ClockMHz)
    $b = [int]($BaudRate / 100); $c = $ClockMHz * 100
    [byte[]]@((($b -shr 8) -band 0xFF), ($b -band 0xFF), (($c -shr 8) -band 0xFF), ($c -band 0xFF), 1, 1, 1)
}

<# 書き込む 128 バイトの区切りの先頭アドレス。0 から End を切り上げたところまで。 #>
function Get-BlockAddresses {
    param([int]$End)
    $top = [int]([Math]::Ceiling($End / $BLOCK)) * $BLOCK
    for ($a = 0; $a -lt $top; $a += $BLOCK) { $a }
}

# ---- 通信 --------------------------------------------------------------------------

function Read-Byte {
    param($Serial, [int]$Ms = $TIMEOUT_MS)
    $Serial.ReadTimeout = $Ms
    try { $Serial.ReadByte() } catch [TimeoutException] { -1 }
}

function Send-Bytes {
    param($Serial, [byte[]]$Bytes)
    $Serial.Write($Bytes, 0, $Bytes.Length)
}

<# ACK を待つ。ACK でなければ、続いて届くエラーコードを添えて失敗にする。 #>
function Wait-Ack {
    param($Serial, [string]$Step, [int]$Ms = $TIMEOUT_MS)
    $c = Read-Byte $Serial $Ms
    if ($c -eq $ACK) { return }
    if ($c -lt 0) { throw "${Step}: 応答がありません" }
    $code = Read-Byte $Serial
    throw ('{0}: 0x{1:X2} が返りました(エラーコード {2})' -f $Step, $c, $(if ($code -lt 0) { 'なし' } else { '0x{0:X2}' -f $code }))
}

<# 応答(コマンド・大きさ・データ・チェックサム)を受け取り、データを返す。 #>
function Read-Response {
    param($Serial, [string]$Step)
    $head = @((Read-Byte $Serial), (Read-Byte $Serial))
    if ($head -contains -1) { throw "${Step}: 応答がありません" }
    $all = [System.Collections.Generic.List[byte]]::new()
    $all.Add($head[0]); $all.Add($head[1])
    for ($i = 0; $i -le $head[1]; $i++) {
        $c = Read-Byte $Serial
        if ($c -lt 0) { throw "${Step}: 応答が途中で切れました" }
        $all.Add($c)
    }
    $sum = 0
    foreach ($b in $all) { $sum += $b }
    if (($sum -band 0xFF) -ne 0) { throw "${Step}: 応答のチェックサムが合いません" }
    [byte[]]$all.GetRange(2, $head[1]).ToArray()
}

<# 速さ合わせ。電源を入れた直後は余計な信号で失敗することがあるので、2 回まで試す(元のプログラムと同じ)。 #>
function Sync-Bitrate {
    param($Serial)
    for ($try = 0; $try -lt 2; $try++) {
        $Serial.DiscardInBuffer()
        for ($i = 0; $i -lt 30; $i++) {
            Send-Bytes $Serial @(0x00)
            $c = Read-Byte $Serial
            if ($c -lt 0) { continue }
            if ($c -ne 0x00) { break }
            Send-Bytes $Serial @(0x55)
            $r = Read-Byte $Serial
            if ($r -eq 0xE6) { return }
            break
        }
    }
    throw '速さ合わせ: 基板から正しい応答がありません。スイッチが書き込みの位置か、電源が入っているかを確かめ、リセットしてからやり直してください'
}

function Invoke-Write {
    param([string]$MotFile, [string]$Port, [int]$ClockMHz, [int]$BaudRate)
    $mot = Read-MotImage (Get-Content -LiteralPath $MotFile)
    if ($mot.End -eq 0) { throw "$MotFile に書き込むデータがありません" }
    Write-Host ('{0}: 0x{1:X} バイト' -f $MotFile, $mot.End)

    $serial = [System.IO.Ports.SerialPort]::new($Port, $BaudRate, 'None', 8, 'One')
    $serial.Open()
    try {
        Sync-Bitrate $serial
        Write-Host '速さ合わせ: 済み'

        Send-Bytes $serial @(0x20)
        $dev = Read-Response $serial 'デバイスの問い合わせ'
        if ($dev.Length -lt 6) { throw 'デバイスの問い合わせ: 応答が短すぎます' }
        Send-Bytes $serial (New-Request 0x10 $dev[2..5])
        Wait-Ack $serial 'デバイスの選択'
        Write-Host ('デバイスコード: {0}' -f [Convert]::ToHexString([byte[]]$dev[2..5]))

        Send-Bytes $serial @(0x21)
        $null = Read-Response $serial 'クロックモードの問い合わせ'
        Send-Bytes $serial (New-Request 0x11 @(0))
        Wait-Ack $serial 'クロックモードの選択'

        # 速さ合わせと同じ速さを選ぶので、こちらの速さは変えない
        Send-Bytes $serial (New-Request 0x3F (Get-BitrateData $BaudRate $ClockMHz))
        Wait-Ack $serial '速さの選択'
        Send-Bytes $serial @($ACK)
        Wait-Ack $serial '速さの確認'
        Write-Host ('速さ: {0} bps、クロック {1} MHz' -f $BaudRate, $ClockMHz)

        Write-Host -NoNewline '消去: '
        Send-Bytes $serial @(0x40)
        Wait-Ack $serial '消去' $ERASE_MS
        Write-Host '済み'

        Send-Bytes $serial @(0x43)
        Wait-Ack $serial 'ユーザー領域の選択'

        $blocks = @(Get-BlockAddresses $mot.End)
        $k = 0
        foreach ($a in $blocks) {
            $data = [byte[]]::new($BLOCK)
            [Array]::Copy($mot.Image, $a, $data, 0, $BLOCK)
            Send-Bytes $serial (New-ProgramPacket $a $data)
            Wait-Ack $serial ('書き込み(0x{0:X})' -f $a) $WRITE_MS
            $k++
            Write-Progress -Activity '書き込み' -Status ('{0} / {1}' -f $k, $blocks.Count) -PercentComplete (100 * $k / $blocks.Count)
        }
        Write-Progress -Activity '書き込み' -Completed
        Send-Bytes $serial (New-ProgramPacket ([uint32]::MaxValue))
        Wait-Ack $serial '書き込みの終わり'
        Write-Host ('書き込み: {0} 区切り(128 バイトずつ)' -f $blocks.Count)
        Write-Host '完了しました。スイッチを実行の位置に戻して電源を入れ直してください'
    } finally {
        $serial.Close()
    }
}

# テストから読み込んだとき(. で読み込んだとき)は、書き込みを始めない
if ($MyInvocation.InvocationName -ne '.') {
    if (-not $MotFile -or -not $Port) {
        Write-Error '使い方: kz_h8write.ps1 -MotFile <ファイル> -Port <COM3 など> [-ClockMHz 20|25] [-BaudRate 19200]'
        exit 2
    }
    try { Invoke-Write $MotFile $Port $ClockMHz $BaudRate; exit 0 }
    catch { Write-Error $_.Exception.Message -ErrorAction Continue; exit 1 }
}
