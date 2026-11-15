'use strict';
// H8 の基板の試験用の組み立て(c/h8 で make trace)で、実際のセッション(暗号の計算をしている間を含む)に混ぜる値を集める。
//   node tools/h8-rngsession.js [COM11] [集める数] [記録のファイル]
// この道具がホストになり(UART のトランスポート。基板とは tools/h8-session.ps1 の中継でつなぐ)、基板に GET をさせて、
// 両眼 1440 点の玉型を DATA で返す。基板は rngsess on のあいだ、セッションを始めるたびに記録を空にし、受けた文字ごとに
// TCNT0 の下位 8 ビットを記録する(1 セッションで 4096 個まで)。セッションの後に rngdump で受け取り、ファイルに足す。
// 集めた値は tools/rng-estimate.c で見積もる。

const { spawn } = require('node:child_process');
const fs = require('node:fs');
const path = require('node:path');
const P = require('../js/src/index.js');

const ROOT = path.join(__dirname, '..');
const comPort = process.argv[2] || 'COM11';
const target = Number(process.argv[3] || 1000000);
const OUT = path.resolve(process.argv[4] || path.join(__dirname, '.h8-rngsession.bin'));

function pwsh(args) {
  return new Promise((resolve, reject) => {
    const child = spawn('pwsh', ['-NoProfile', '-File', ...args], { cwd: ROOT });
    let out = '';
    child.stdout.on('data', d => { out += d; });
    child.stderr.on('data', d => { out += d; });
    child.on('error', reject);
    child.on('close', code => resolve({ code, out }));
  });
}

async function main() {
  const keys = P.noise.keyPair();
  const caps = [['VER', '1.0'], ['DEV', 'HST'], ['VEN', 'RNG'], ['MODEL', 'H'], ['MAXFRAME', '4096']];
  const radii = Array.from({ length: 1440 }, (_, i) => String(2400 + (i % 300)));
  const handlers = {
    GET: ({ records }) => ({ op: 'DATA', records: [['JOB', (records.find(r => r[0] === 'JOB') || [, 'X'])[1]], ['DBL', '18.00'], ['TRCFMT', '1', '1440', 'E', 'R', 'F'], ['R', ...radii], ['TRCFMT', '1', '1440', 'E', 'L', 'F'], ['R', ...radii]] }),
    RESULT: () => 0, PUT: () => 0, INFO: () => 0,
  };
  const server = await P.listen(0, { keys, caps, trust: () => true, transport: 'uart', handlers });
  const port = server.address().port;
  for (const c of ['trace 1440', 'rngsess on']) {
    const r = await pwsh(['tools/h8-session.ps1', '-Port', comPort, '-Command', c]);
    if (!/ok/.test(r.out)) throw new Error(`基板が応えません(${c}): ${r.out}`);
  }
  const have = () => (fs.existsSync(OUT) ? fs.statSync(OUT).size : 0);
  console.log(`基板 ${comPort}、これまでに ${have()} 個、${target} 個まで集める`);
  const t0 = Date.now();
  for (let i = 0; have() < target; i++) {
    const s = await pwsh(['tools/h8-session.ps1', '-Port', comPort, '-Command', 'get R-1', '-Hub', `127.0.0.1:${port}`, '-TimeoutSeconds', '240', ...(i === 0 ? ['-Pair'] : [])]);
    const rc = (/(?:^|\s)rc=(-?\d+)/m.exec(s.out) || [, 'なし'])[1];
    const d = await pwsh(['tools/h8-rngtrace.ps1', '-Port', comPort, '-Dump', '-OutFile', OUT]);
    if (d.code) throw new Error(`記録を受け取れません: ${d.out}`);
    if ((i + 1) % 10 === 0) console.log(`${i + 1} セッション(最後の戻り値 ${rc}): ${have()} 個、${((Date.now() - t0) / 60000).toFixed(0)} 分`);
  }
  await pwsh(['tools/h8-session.ps1', '-Port', comPort, '-Command', 'rngsess off']);
  server.close();
  console.log(`集めた値: ${have()} 個(${OUT})`);
}

main().catch(e => { console.error(e.message); process.exit(2); });
