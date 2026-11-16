# Optical Lab Device Protocol 参照実装

OLDP(Optical Lab Device Protocol)第 1.1 版の参照実装である。機器の側を C で、ホストの側を JavaScript で書いた。機器の側には、H8/3069F の基板で動くファームウェアも含む。

規格の文章・定義・テストベクタは、規格のリポジトリ [optical-lab-device-protocol](https://github.com/NrivYuki/optical-lab-device-protocol) に置いている。テストの内容、基板で測った値、実装の制限、確かめていないことは、[docs/results.md](docs/results.md) にまとめた。

## フォルダ

| フォルダ | 中身 |
|---|---|
| `spec/` | 規格のリポジトリの `spec/`(第 1.1 版)の写し。このリポジトリでは直さない |
| `js/` | ホストの側の実装(Node.js 22 以上。外部のパッケージは使わない)とテスト |
| `c/` | 機器の側の実装(C。動的なメモリ確保をしない)、そのテスト、C の模擬機器(`c/sim/sim_device.c`) |
| `c/h8/` | H8/3069F の基板のファームウェア |
| `tools/sim-device.js` | PC で動く模擬機器(トレーサー・加工機。JavaScript) |
| `tools/bridge.js` | 2 つの TCP の接続をつなぐ中継(WSL の C の模擬機器と、Windows のホストをつなぐため) |
| `tools/import-spec.js` | 規格のリポジトリの `spec/` を写し直し、C の表を作り直す |
| `tools/h8-ephemeral.js` | H8 の基板を起動し直すたびに、ハンドシェイクの一時鍵が変わるかを確かめる |
| `tools/h8-rngtrace.ps1` | H8 の基板の試験用の組み立てから、乱数に混ぜる文字の到着の時刻を集める |
| `tools/rng-estimate.c` | 集めた値の最小エントロピーを、NIST SP 800-90B の 6.3 節の方法で見積もる(3.1.4 の再起動のテストも) |
| `tools/crosscheck.py` | 機器の側の C の暗号の部品を、OpenSSL(pyca/cryptography)の結果と比べる |
| `tools/h8-xtime.js` | H8 の基板の試験用の組み立てで、秘密の値を使う計算の時間が値によって変わらないかを確かめる |
| `tools/h8-ubsan.js` | 未定義の動作の手前で止まる H8 の組み立てを、ペアリングしたホストの代わりになって、形の崩れたメッセージを送る |
| `tools/h8-rngsession.js` | H8 の基板の試験用の組み立てから、実際のセッションの間に混ぜる値を集める |
| `samples/` | 見本のジョブ。`tools/gen-samples.js` が計算で作る(角を丸めた楕円の玉型) |
| `third_party/` | 外部から取り込んだもの。Monocypher 4.0.3(機器の側の C の暗号の部品。BSD 2 条項か CC0) |

C の表のうち、`c/src/oldp_labels.h` は `tools/gen-c-labels.js` が `spec/labels.json` から、`c/tests/vectors.h` は `tools/gen-c-vectors.js` が `spec/vectors` から作る。手では直さない。

## 規格の取り込み

`spec/` は規格のリポジトリの `spec/` を写したものである。規格が変わったときは、次のコマンドで写し直す。規格のリポジトリは、隣のフォルダ(`../optical-lab-device-protocol`)か、環境変数 `OLDP_SPEC` で指定した場所のものを使う。

```
node tools/import-spec.js
```

JavaScript のテストは、規格のリポジトリが見つかれば、`spec/` がそれと同じかどうかも確かめる。見つからなければ、この確認だけを飛ばす。

## テスト

JavaScript:

```
cd js
npm test
```

C(WSL の gcc で組み立てる。`c/Makefile`):

```
cd c
make test
```

機器の側の C の実装は、次の 3 つでも確かめる(WSL の中で動かす)。`crosscheck.py` は Python の pyca/cryptography を使う。

```
make -C c crosscheck fuzz_device timing
python3 tools/crosscheck.py c/crosscheck 50000
c/fuzz_device 10000 1
c/timing 20000 200000
```

- `crosscheck`: X25519・SHA-256・HMAC-SHA-256・ChaCha20-Poly1305(Noise の nonce の形)を、乱数の入力と境界の値(位数の小さい点、p 以上の u、nonce の上限など)で OpenSSL と比べる。書き換えた暗号文・タグ・付加データを受け入れないことも確かめる。
- `fuzz_device`: AddressSanitizer と UndefinedBehaviorSanitizer を付けて組み立てる。ホストの代わりをする部分が、ハンドシェイクの前のフレーム、ペアリングしたホストのメッセージ(正しく暗号化した上で中身を崩したもの)、線の上のバイト列を、乱数で壊して機器へ渡す(`c/tests/fuzz_device.c` の先頭に詳しく書いた)。2 つ目の引数は乱数の種である。
- `timing`: 決まった鍵と乱数の鍵で X25519・ChaCha20-Poly1305・HMAC-SHA-256 の処理時間を集め、Welch の t 検定で比べる(dudect の方法)。3 つ目の引数に `null` を付けると、両方の組を乱数の鍵にして、測り方が偽の差を出さないかを確かめる。

## 模擬機器

JavaScript の模擬機器は次のように動かす。

```
node tools/sim-device.js tracer 127.0.0.1 33520 samples/frame-both.vca T-1 --pair
node tools/sim-device.js edger 127.0.0.1 33520 E-1 --pair
```

`--pair` を付けると、ペアリングの受付の状態で動き、知らないホストの鍵を覚える。鍵と覚えたホストは `tools/.sim-device` に保存する(`--state` で場所を変えられる)。

C の模擬機器は WSL の中で接続を待ち受け、Windows の側から `tools/bridge.js` でホストへつなぐ。WSL から Windows で待ち受けているホストへ直接つなぐには、ファイアウォールの設定を変える必要があるためである。コマンドは基板と同じである(`put <JOB>`・`get <名前>`・`ops <JOB> <名前>`・`wait <秒>`)。

```
wsl -e c/sim_device "get E-1" --listen 35001 --pair --state /tmp/sim-c
node tools/bridge.js 127.0.0.1:35001 127.0.0.1:33520
```

LAN(TCP)が第一の方式である。シリアルは LAN を持たない旧機器をつなぐための方式である。シリアルの速度の既定値は 9600 bps で、ほかの速度を使うときは、規格の 4 章の一覧から選び、線の両端に同じ値を設定する。C の模擬機器をシリアルの機器として動かすときは、`--uart` を付ける。

## H8/3069F の基板(c/h8)

`c/h8` は、機器の側の実装を、内蔵フラッシュから起動するファームウェアにしたものである。WSL の中で、`make -C c/h8/loader` を実行した後に `make -C c/h8 all` を実行して作る。

検証を繰り返しやすくするため、基板の本来の書き込み方(DIP スイッチでブートモードにして書く)に加えて、スイッチを切り替えずに本体を書き換える更新の仕組みと、ソフトウェアのリセットを加えている。この改造は検証のためのもので、OLDP の規格には含まれず、製品に使ってはならない(署名を確かめない)。改造の中身、フラッシュの配置、本来の使い方への戻し方は [docs/h8-board.md](docs/h8-board.md) にまとめた。

| ファイル | 中身 |
|---|---|
| `c/h8/loader/` | 割り込みのベクタ表と更新の仕組み(検証のための改造。フラッシュの先頭 4 KB に置き、RAM に写して動かす) |
| `c/h8/oldp-h8.mot` | 本体だけ(0x1000 番地から) |
| `c/h8/oldp-h8-all.mot` | 更新の仕組みと本体を合わせたもの |

基板の DIP スイッチ(1-4 番)は次のように使う。

| 位置 | 動作 | 使う場面 |
|---|---|---|
| ON・OFF・ON・ON | 動作の位置(モード 5、FWE=1)。電源を入れると、内蔵フラッシュの本体が起動する。FWE=1 なので、スイッチを切り替えずにシリアルから本体を書き換えられる(本来の使い方では 4 番を OFF にして、動作中の書き換えを防ぐ) | 基板を使うとき、本体を書き換えるとき |
| ON・ON・OFF・ON | ブートモード。内蔵のブートプログラムが動き、シリアルからフラッシュ全体を書き込める | `tools/kz_h8write.ps1` で `oldp-h8-all.mot` を書き込むとき(初めて書き込むときと、更新の仕組みが壊れたときだけ) |

本体の書き換えは、スイッチを動作の位置にしたまま、次のコマンドで行う(`COM3` は、基板をつないだシリアルポートの例)。

```
pwsh -File tools/h8-update.ps1 -Port COM3
```

本体が壊れて `update` コマンドが使えないときは、`-Recover` を付けて実行し、基板のリセットボタンを押す。本体が動いていれば `reset` コマンドで戻せるので、ボタンは要らない。更新の仕組み(`c/h8/loader/`)を変えたときは、`-Loader` を付けて実行する。動いている本体が、フラッシュの先頭のブロックを書き換える。

シリアルの速度は、既定の 9600 bps のほかに、19200・38400・57600 bps を使える(規格の 4 章の一覧のうち、20 MHz の H8 で作れるもの)。`baud` コマンドで変え、フラッシュに保存される。速度を変えた後は、PC の道具に `-BaudRate` で同じ値を渡す。更新の仕組みはいつも 9600 bps で動く(`h8-update.ps1` が切り替える)。

```
pwsh -File tools/h8-session.ps1 -Port COM3 -Command 'baud 57600'
pwsh -File tools/h8-session.ps1 -Port COM3 -BaudRate 57600 -Command 'get H8-E1' -Hub 127.0.0.1:33622
```

LAN でつなぐときは、`ip` コマンドで基板のアドレスを設定する。設定はフラッシュに保存される。基板の LAN コントローラは RTL8019AS(10BASE-T)である。次の例は、基板を 169.254.74.50、ホストの PC を 169.254.74.208 にしたものである。

```
pwsh -File tools/h8-session.ps1 -Port COM3 -Command 'ip 169.254.74.50 255.255.0.0 0.0.0.0 169.254.74.208 33520'
pwsh -File tools/h8-session.ps1 -Port COM3 -Command 'put H8-T1' -Lan -Pair
```

シリアルでホストとつなぐときは、ホストの待ち受けを、シリアルの機器を中継する設定にしておく。初めてつなぐときは `-Pair` を付ける。

```
pwsh -File tools/h8-session.ps1 -Port COM3 -Command 'put H8-T1' -Hub 127.0.0.1:33622 -Pair
pwsh -File tools/h8-session.ps1 -Port COM3 -Command 'get H8-E1' -Hub 127.0.0.1:33622
```

基板のコマンドの一覧は、`c/h8/main.c` の先頭にある。覚えたホストはフラッシュの最後のブロック(EB15)に保存される。`hosts` で一覧を表示し、`unpair` で消去できる。

機器の鍵と乱数の種は、`c/h8/devkey.h` に置く。このファイルがなければ、`tools/gen-h8-assets.js` が機器ごとに作る。鍵と種は外部に出さない。組み立てた `.elf`・`.mot` と `asm/` にも含まれるので、これらは `.gitignore` で除いている。

組み立てるときの注意:

- 更新の仕組みと本体の S レコードを合わせるときは、S1 と S2 の両方を取り出す。本体が 64 KB に収まると、本体も S1 になるためである。
- FTDAR の値とダウンロード先は、H'00 のとき H'FFEF20、H'03 のとき H'FFBF20 である(ハードウェアマニュアル 18.4.2)。
- 内蔵の書き込みプログラムに渡すデータは、偶数番地に置く。奇数番地に置くと、1 バイトずれて書き込まれる(マニュアルには書かれていない)。RAM に写して動かす手順も、偶数番地に置く(`rom.ld`)。
- SCI1 の受信は割り込みで受け、リングバッファにためる。受信を読みに行く作りでは、フレームを処理する間(復号など)に届いた文字を取りこぼす(オーバーランエラー)。割り込みの飛び先の仕組みは [docs/h8-board.md](docs/h8-board.md) にある。
- 変数は、内蔵の書き込みプログラムの取り出し先(0xFFDF20)より下に置く(`rom.ld` で確かめる)。残りは少なく、リングバッファの大きさ(`main.c` の `RXQ`)はこれで決めている。
- 32 ビット・64 ビットの掛け算は、libgcc のものではなく `c/h8/ctmul.c` のものを使う。libgcc のものは値によって処理時間が変わり、Poly1305 が秘密の値で呼ぶためである。

乱数に混ぜる値と、秘密の値を使う計算の時間を調べるときは、試験用の組み立て(`make -C c/h8 trace`。`rngtrace`・`rngboot`・`rngsess`・`rngdump`・`rnglan`・`xtime` のコマンドを加えた `oldp-h8-trace.mot`)を基板に書く。見積もりの道具は、WSL の gcc で組み立てて動かす。終わったら、製品用の組み立てに書き戻す。

```
pwsh -File tools/h8-update.ps1 -Port COM3 -MotFile c/h8/oldp-h8-trace.mot
pwsh -File tools/h8-rngtrace.ps1 -Port COM3 -Blocks 245
pwsh -File tools/h8-rngtrace.ps1 -Port COM3 -Restarts 1000 -OutFile tools/.h8-restart.bin
node tools/h8-xtime.js COM3
node tools/h8-rngsession.js COM3 1000000
pwsh -File tools/h8-rngtrace.ps1 -Port COM3 -Lan -Blocks 245 -OutFile tools/.h8-rnglan.bin
gcc -O2 -o rng-estimate tools/rng-estimate.c -lm
./rng-estimate -d tools/.h8-rngtrace.bin
./rng-estimate -r 0.378 tools/.h8-restart.bin
pwsh -File tools/h8-update.ps1 -Port COM3
```

int が 16 ビットのこの CPU でだけ起きる未定義の動作を確かめるときは、未定義の動作の手前で番地を返して止まる組み立て(`make -C c/h8 ubsan` で `oldp-h8-ub.mot`)を書き、`tools/h8-ubsan.js` や vca-hub-extension の確かめの道具を動かす。

```
pwsh -File tools/h8-update.ps1 -Port COM3 -MotFile c/h8/oldp-h8-ub.mot
node tools/h8-ubsan.js COM3 60
```

## 確かめた環境

文書は日本語だけで書いている。道具は Windows を前提にしている(H8 の基板の道具は PowerShell、C の組み立ては WSL)。次の環境で、テストを全て通した。基板では、第 1.1 版のファームウェアで `verify-device.js`(UART 9600 bps・LAN)を通した。ほかの基板での確認は、第 1.0 版のファームウェアで通した(docs/results.md の「確かめていないこと」)。

| 対象 | 版 |
|---|---|
| OS | Windows 10 Pro(10.0.19045) |
| Node.js | 24.20.0 |
| PowerShell | 7.6.9 |
| WSL | WSL 3.0.5、Ubuntu 26.04.1 LTS(gcc 15.2.0、GNU Make 4.4.1、Python 3.14.4) |
| H8 の組み立て | h8300-elf-gcc 16.2.0、GNU Binutils 2.47 |
| 基板 | H8/3069F(20 MHz)、LAN コントローラ RTL8019AS(10BASE-T)。PC とは、USB シリアル変換器(Prolific のチップ)と LAN の直結でつなぐ |

## ライセンス

| 対象 | ライセンス |
|---|---|
| このリポジトリの実装・道具・見本、`spec/` の写し | MIT(`LICENSE`) |
| `spec/vectors/noise_xx_25519_chachapoly_sha256.json` | cacophony のテストベクタ: Unlicense |
| `third_party/` と、そこから写したもの | Monocypher: BSD 2 条項か CC0 |
| `tools/kz_h8write.ps1` | KOZOS H8/3069F Flash Writer(Shinichiro Nakamura、MIT)を PowerShell に移したもの。元の著作権表示はファイルの中にある |
