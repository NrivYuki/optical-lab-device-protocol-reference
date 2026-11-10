#Requires -Version 7
<#
.SYNOPSIS
  H8 の基板(c/h8 のファームウェア)にコマンドを送り、セッションの間は基板とホスト(vca-hub の待ち受け)の間を中継する。

.DESCRIPTION
  put・get・ops・wait: 基板が "start" を返したらホストへ TCP でつなぎ、基板が "OLDP-END" を返すまで、バイトをそのまま中継する。
            ホストの待ち受けは UART のトランスポートにしておく(vca-hub の listeners の settings に { "transport": "uart" })。
  bench:    基板が "bench-start" と "bench-end" を返すあいだの時間(X25519 の 1 回分)を測る。
  -Lan:     put・get・ops・wait を基板の LAN で行う。基板がホストへ直接 TCP でつなぐので、中継も -Hub も要らない
            (基板の ip コマンドでホストのアドレスとポートを設定しておく)。線は結果の表示にだけ使う。
  そのほか(id・log)は、返事をそのまま表示する。

.EXAMPLE
  pwsh -File tools/h8-session.ps1 -Port COM11 -Command id
  pwsh -File tools/h8-session.ps1 -Port COM11 -Command bench
  pwsh -File tools/h8-session.ps1 -Port COM11 -Command 'put H8-T1' -Hub 127.0.0.1:33520 -Pair
  pwsh -File tools/h8-session.ps1 -Port COM11 -Command 'get H8-E1' -Hub 127.0.0.1:33520
#>
param(
    [string]$Port = 'COM11',
    [int]$BaudRate = 9600,
    [Parameter(Mandatory)][string]$Command,
    [string]$Hub,
    [switch]$Pair,
    [switch]$Lan,
    [int]$TimeoutSeconds = 900
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

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

$serial = [System.IO.Ports.SerialPort]::new($Port, $BaudRate, 'None', 8, 'One')
$serial.Open()
try {
    $serial.DiscardInBuffer()
    # 基板が応えるかを確かめる(空行は読み飛ばされて何も返らないので、id を送る)
    $serial.Write("id`r")
    [void](Read-Until $serial 'oldp> ' 5)
    if ($Pair) { $serial.Write("pair`r"); [void](Read-Until $serial 'oldp> ' 5); Write-Host 'ペアリングの受付にしました(次のセッションで、知らないホストの鍵を覚える)' }

    if ($Command -eq 'bench') {
        $serial.Write("bench`r")
        [void](Read-Until $serial "bench-start`r`n" 10)
        $t0 = Get-Date
        [void](Read-Until $serial 'bench-end' $TimeoutSeconds)
        Write-Host ('X25519 の 1 回(公開鍵を求める計算): {0:F1} 秒' -f ((Get-Date) - $t0).TotalSeconds)
        return
    }
    if ($Command -notmatch '^(put|get|ops|wait|rekey) ') {
        $serial.Write("$Command`r")
        Write-Host ((Read-Until $serial 'oldp> ' 10) -replace "`r`noldp> $", '')
        return
    }
    $serial.Write($(if ($Lan) { "via lan`r" } else { "via uart`r" }))
    [void](Read-Until $serial 'oldp> ' 5)
    if ($Lan) {
        $serial.Write("$Command`r")
        [void](Read-Until $serial "start`r`n" 10)
        $start = Get-Date
        $tail = Read-Until $serial 'oldp> ' $TimeoutSeconds
        $end = [regex]::Match($tail, 'OLDP-END (-?\d+) (-?\d+)')
        Write-Host ('セッション: {0:F1} 秒、LAN、戻り値 {1}、STATUS {2}' -f ((Get-Date) - $start).TotalSeconds, $end.Groups[1].Value, $end.Groups[2].Value)
        $serial.Write("log`r")
        Write-Host ((Read-Until $serial 'oldp> ' 10) -replace "`r`noldp> $", '')
        $serial.Write("lan`r")
        Write-Host ((Read-Until $serial 'oldp> ' 10) -replace "`r`noldp> $", '')
        return
    }
    if (-not $Hub) { throw 'put・get・ops・wait には -Hub(ホストの待ち受け)が要ります' }

    $serial.Write("$Command`r")
    [void](Read-Until $serial "start`r`n" 10)
    $h, $p = $Hub.Split(':')
    $client = [System.Net.Sockets.TcpClient]::new()
    $client.Connect($h, [int]$p)
    $stream = $client.GetStream()
    $buf = [byte[]]::new(4096)
    $tail = ''
    $start = Get-Date
    $toHost = 0; $toBoard = 0
    Write-Host "中継を始めました($Port ⇔ $Hub)"
    while ($true) {
        if (((Get-Date) - $start).TotalSeconds -gt $TimeoutSeconds) { throw "$TimeoutSeconds 秒たっても終わりません" }
        $moved = $false
        $n = $serial.BytesToRead
        if ($n -gt 0) {
            $n = $serial.Read($buf, 0, [Math]::Min($n, $buf.Length))
            # ホストが断って接続を切ったあとも、基板の "OLDP-END" までは読み続ける
            if ($client.Connected) { try { $stream.Write($buf, 0, $n) } catch { $client.Close() } }
            $toHost += $n; $moved = $true
            $tail = ($tail + [Text.Encoding]::Latin1.GetString($buf, 0, $n))
            if ($tail.Length -gt 200) { $tail = $tail.Substring($tail.Length - 200) }
            if ($tail -match "OLDP-END (-?\d+) (-?\d+)\r\noldp> ") { break }      # 入力待ちの表示まで受けてから終える
        }
        if ($client.Connected -and $stream.DataAvailable) {
            $n = $stream.Read($buf, 0, $buf.Length)
            if ($n -gt 0) { $serial.Write($buf, 0, $n); $toBoard += $n; $moved = $true }
        }
        if (-not $moved) { Start-Sleep -Milliseconds 2 }
    }
    $client.Close()
    $end = [regex]::Match($tail, 'OLDP-END (-?\d+) (-?\d+)')
    Write-Host ('セッション: {0:F1} 秒、基板→ホスト {1} バイト、ホスト→基板 {2} バイト、戻り値 {3}、STATUS {4}' -f ((Get-Date) - $start).TotalSeconds, $toHost, $toBoard, $end.Groups[1].Value, $end.Groups[2].Value)
    $serial.Write("log`r")
    Write-Host ((Read-Until $serial 'oldp> ' 10) -replace "`r`noldp> $", '')
} finally {
    $serial.Close()
}
