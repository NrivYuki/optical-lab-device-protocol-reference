'use strict';
// H8 の基板を起動し直すたびに、ハンドシェイクの一時鍵が変わるかを確かめる(規格 2 章の乱数)。
//   node tools/h8-ephemeral.js [回数] [COM11] [速度]
// 1 回ごとに、基板の reset コマンドで起動し直し、put でセッションを始めさせる。ホストの代わりにこの道具が受けて、
// 最初のハンドシェイクのフレーム(hs1)から機器の一時公開鍵を取り出し、ハンドシェイクの途中の FAIL で終わらせる。
// 集めた一時公開鍵に同じものがないかを見る。結果は tools/.h8-ephemeral.json に残す(続きから集められる)。

const { spawn } = require('node:child_process');
const fs = require('node:fs');
const net = require('node:net');
const path = require('node:path');
const { uartEncode, uartDecode, TYPES, VERSION } = require('../js/src/frame.js');

const ROOT = path.join(__dirname, '..');
const count = Number(process.argv[2] || 100);
const comPort = process.argv[3] || 'COM11';
const baud = process.argv[4] || '9600';
const OUT = path.join(__dirname, '.h8-ephemeral.json');

function pwsh(args) {
  return new Promise((resolve, reject) => {
    const child = spawn('pwsh', ['-NoProfile', '-File', 'tools/h8-session.ps1', '-Port', comPort, '-BaudRate', baud, ...args], { cwd: ROOT });
    let out = '';
    child.stdout.on('data', d => { out += d; });
    child.stderr.on('data', d => { out += d; });
    child.on('error', reject);
    child.on('close', code => resolve({ code, out }));
  });
}

// ホストの代わり: hs1 を受けたら ACK を返し、FAIL(ハンドシェイクに失敗した)を送って終わらせる
function fakeHost() {
  let pending = null;
  const server = net.createServer(sock => {
    let buf = [];
    let done = false;
    sock.on('error', () => {});
    sock.on('data', chunk => {
      for (const b of chunk) {
        if (b !== 0) { buf.push(b); continue; }
        const u = buf.length ? uartDecode(Buffer.from(buf)) : null;
        buf = [];
        if (!u || u.kind !== 0) continue;
        sock.write(uartEncode(1, u.lseq));
        if (done || u.body.length < 34 || u.body[0] !== VERSION || u.body[1] !== TYPES.hs1) continue;
        done = true;
        if (pending) { pending(u.body.subarray(2, 34).toString('hex')); pending = null; }
        sock.write(uartEncode(0, 0, Buffer.from([VERSION, TYPES.fail, 2])));
      }
    });
  });
  return new Promise(resolve => server.listen(0, '127.0.0.1', () => resolve({
    port: server.address().port,
    next: () => new Promise(r => { pending = r; }),
    close: () => server.close(),
  })));
}

async function main() {
  const seen = fs.existsSync(OUT) ? JSON.parse(fs.readFileSync(OUT, 'utf8')) : [];
  const host = await fakeHost();
  console.log(`基板 ${comPort}(${baud} bps)。これまでに ${seen.length} 回分`);
  for (let i = 0; i < count; i++) {
    const r = await pwsh(['-Command', 'reset']);
    if (r.code) throw new Error(`reset に応えません: ${r.out}`);
    const got = host.next();
    const s = await pwsh(['-Command', 'put R-1', '-Hub', `127.0.0.1:${host.port}`]);
    const e = await Promise.race([got, new Promise(res => setTimeout(() => res(null), 1000))]);
    if (!e) throw new Error(`一時公開鍵を受けられません: ${s.out}`);
    seen.push(e);
    fs.writeFileSync(OUT, JSON.stringify(seen));
    if ((i + 1) % 10 === 0) console.log(`${seen.length} 回: 異なる一時公開鍵 ${new Set(seen).size}`);
  }
  host.close();
  const uniq = new Set(seen).size;
  console.log(`合わせて ${seen.length} 回、異なる一時公開鍵 ${uniq}${uniq === seen.length ? '(重なりなし)' : '(重なりあり)'}`);
  process.exit(uniq === seen.length ? 0 : 1);
}

main().catch(e => { console.error(e.message); process.exit(2); });
