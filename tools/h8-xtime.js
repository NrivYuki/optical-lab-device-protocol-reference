'use strict';
// H8 の基板の試験用の組み立て(c/h8 で make trace)で、秘密の値を使う計算の時間が値によって変わらないかを確かめる。
//   node tools/h8-xtime.js [COM11] [乱数の鍵の数] [計算の種類(x25519,aead,aeadlong,hmac をカンマで)]
// X25519(秘密の値の 1 の並び方を変えた鍵と、乱数の鍵)、ChaCha20-Poly1305(64 バイトと 480 バイト)、HMAC-SHA-256 を、鍵だけを変えて計算させ、
// 基板の xtime コマンドが返す時間(TCNT0 の数。8 サイクルごと)を比べる。同じ鍵で 2 回ずつ測り、刻みによるずれ(1 数)の大きさも見る。

const { spawn } = require('node:child_process');
const crypto = require('node:crypto');
const path = require('node:path');

const ROOT = path.join(__dirname, '..');
const comPort = process.argv[2] || 'COM11';
const randoms = Number(process.argv[3] || 6);

function xtime(kind, key) {
  return new Promise((resolve, reject) => {
    const child = spawn('pwsh', ['-NoProfile', '-File', 'tools/h8-session.ps1', '-Port', comPort, '-Command', `xtime ${kind} ${key}`, '-TimeoutSeconds', '30'], { cwd: ROOT });
    let out = '';
    child.stdout.on('data', d => { out += d; });
    child.stderr.on('data', d => { out += d; });
    child.on('error', reject);
    child.on('close', () => {
      const m = /ticks=(\d+)/.exec(out);
      if (!m) return reject(new Error(`基板の返事がありません: ${out}`));
      resolve(Number(m[1]));
    });
  });
}

async function main() {
  const fixed = [
    ['全て 0', '00'.repeat(32)],
    ['全て 1', 'ff'.repeat(32)],
    ['01 の繰り返し', '55'.repeat(32)],
    ['10 の繰り返し', 'aa'.repeat(32)],
    ['前半 1・後半 0', 'ff'.repeat(16) + '00'.repeat(16)],
  ];
  const keys = [...fixed, ...Array.from({ length: randoms }, (_, i) => [`乱数 ${i + 1}`, crypto.randomBytes(32).toString('hex')])];
  let differ = 0;
  for (const kind of (process.argv[4] ? process.argv[4].split(',') : ['x25519', 'aead', 'aeadlong', 'hmac'])) {
    const seen = new Map();
    console.log(`== ${kind}`);
    for (const [name, key] of keys) {
      const a = await xtime(kind, key), b = await xtime(kind, key);
      console.log(`  ${name.padEnd(12)} ${a} ${b}${a !== b ? '(同じ鍵で違う)' : ''}`);
      seen.set(a, (seen.get(a) || 0) + 1);
      if (a !== b) seen.set(b, (seen.get(b) || 0) + 1);
    }
    const values = [...seen.keys()].sort((x, y) => x - y);
    const span = values[values.length - 1] - values[0];
    console.log(`  → 異なる時間 ${values.length} 通り(最小と最大の差 ${span} 数 = ${span * 8} サイクル)`);
    // 読み始めがタイマの 8 サイクルの刻みのどこに当たるかで、同じ計算でも 1 数ずれる。それより大きい差を、鍵による違いとする
    if (span > 1) differ++;
  }
  console.log(differ ? `鍵によって時間が変わる計算が ${differ} 種類あった` : 'どの計算も、鍵による違いは測れる幅(8 サイクル)より小さかった');
}

main().catch(e => { console.error(e.message); process.exit(2); });
