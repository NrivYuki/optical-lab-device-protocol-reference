'use strict';
// PC で動く模擬機器。TCP でホストへつなぎ、トレーサー(PUT)か加工機(GET と RESULT)として動く。
//
// 使い方:
//   node tools/sim-device.js tracer <ホスト> <ポート> <VCA ファイル> <ジョブ> [--state <フォルダ>] [--pair]
//   node tools/sim-device.js edger  <ホスト> <ポート> <名前>            [--state <フォルダ>] [--pair]
//   node tools/sim-device.js whoami                                     [--state <フォルダ>]
// --state: 機器の鍵と、ペアリングしたホストの鍵を置くフォルダ(既定は tools/.sim-device)
// --pair:  ペアリングの受付(知らないホストの鍵を覚える。規格 6 章)。付けなければ、覚えたホストとしか話さない
// 鍵のフィンガープリントは、機器のラベルの代わりに画面に出す。ホストの画面に出るフィンガープリントと見比べて、ホストの側で「信じる」を選ぶ。

const fs = require('node:fs');
const path = require('node:path');
const P = require('../js/src/index.js');

const args = process.argv.slice(2);
const flag = name => { const i = args.indexOf(name); if (i < 0) return null; const v = args[i + 1]; args.splice(i, 2); return v; };
const has = name => { const i = args.indexOf(name); if (i < 0) return false; args.splice(i, 1); return true; };
const stateDir = flag('--state') || path.join(__dirname, '.sim-device');
const pairing = has('--pair');
const [mode, ...rest] = args;

fs.mkdirSync(stateDir, { recursive: true });
const keyFile = path.join(stateDir, 'device-key.json');
const hostsFile = path.join(stateDir, 'hosts.json');
const keys = fs.existsSync(keyFile)
  ? P.noise.keyPair(Buffer.from(JSON.parse(fs.readFileSync(keyFile, 'utf8')).privateKey, 'hex'))
  : (() => { const k = P.noise.keyPair(); fs.writeFileSync(keyFile, JSON.stringify({ privateKey: k.privateKey.toString('hex') })); return k; })();
const hosts = fs.existsSync(hostsFile) ? JSON.parse(fs.readFileSync(hostsFile, 'utf8')) : [];

console.log(`この機器の鍵のフィンガープリント: ${P.noise.fingerprint(keys.publicKey)}`);

/** ペアリングしたホストか。受付中なら、知らないホストを覚える */
function trust(peer) {
  const fp = peer.fingerprint;
  if (hosts.includes(fp)) return true;
  if (!pairing) { console.log(`知らないホスト(${fp})なので話しません。ペアリングするときは --pair を付けてください`); return false; }
  hosts.push(fp);
  fs.writeFileSync(hostsFile, JSON.stringify(hosts, null, 1));
  console.log(`ホストを覚えました: ${fp}(ホストの画面のフィンガープリントと同じか確かめてください)`);
  return true;
}

/** DCS の VCA ファイルから、この規格で送るレコードを取り出す(通信用の行を除く。玉型は形式 1 のものだけ) */
function fromVcaFile(file) {
  const skip = new Set(['REQ', 'ANS', 'JOB', 'STATUS', 'CRC', 'DEV', 'VEN', 'MODEL', 'SN', 'DO']);
  const recs = P.records.parse(fs.readFileSync(file, 'latin1')).filter(r => !skip.has(r[0]));
  const bad = recs.find(r => r[0] === 'TRCFMT' && r[1] !== '1');
  if (bad) throw new Error(`TRCFMT=${bad.slice(1).join(';')}: この規格の玉型は形式 1 だけです`);
  return recs;
}

const CAPS = {
  tracer: [['VER', '1.0'], ['DEV', 'TRC'], ['VEN', 'SIM'], ['MODEL', 'TRACER'], ['SN', 'SIM-T1'], ['MAXFRAME', '1024']],
  edger: [['VER', '1.0'], ['DEV', 'EDG'], ['VEN', 'SIM'], ['MODEL', 'EDGER'], ['SN', 'SIM-E1'], ['MAXFRAME', '512'],
    ['LABELS', 'JOB', 'DO', 'DBL', 'HBOX', 'VBOX', 'CIRC', 'FCRV', 'TRCFMT', 'R', 'A'],
    ['LABELS', 'ETYP', 'BEVP', 'BEVM', 'BEVC', 'POLISH', 'LIND', 'SPH', 'CYL', 'AX', 'IPD', 'OCHT'],
    ['TRACE', '360', 'E']],
};

async function main() {
  if (mode === 'whoami') return;
  const [host, port] = rest;
  if (!CAPS[mode] || !host || !port) throw new Error('使い方は、このファイルの先頭を見てください');
  const caps = CAPS[mode];
  const s = await P.connect(host, Number(port), {
    keys, caps, trust,
    handlers: { AVAIL: ({ records }) => { console.log(`ホストからの知らせ: ジョブ ${P.records.parse(P.records.format(records)).find(r => r[0] === 'JOB')?.[1]} が用意できました`); return 0; } },
  });
  console.log(`つながりました: ホスト ${s.peer.caps.ven} ${s.peer.caps.model}(${s.peer.fingerprint})`);
  try {
    if (mode === 'tracer') {
      const [, , file, job] = rest;
      const r = await s.request('PUT', [['JOB', job], ...fromVcaFile(file)]);
      console.log(`PUT ${job}: STATUS=${r.status}`);
      process.exitCode = r.status === 0 ? 0 : 1;
      return;
    }
    const [, , name] = rest;
    const r = await s.request('GET', [['JOB', name]]);
    if (r.opName !== 'DATA') { console.log(`GET ${name}: STATUS=${r.status}`); process.exitCode = 1; return; }
    const accepts = new Set(caps.filter(x => x[0] === 'LABELS').flatMap(x => x.slice(1)));
    const c = P.records.classify(r.records, { accepts });
    const traces = P.records.checkTraces(r.records);
    const status = traces.length ? 4 : 0;
    console.log(`GET ${name}: ${r.records.length} レコード、玉型 ${r.records.filter(x => x[0] === 'TRCFMT').map(x => x.slice(1).join(';')).join(' / ')}`);
    console.log(`  使った: ${c.used.join(',') || 'なし'}`);
    if (c.ignored.length) console.log(`  捨てた: ${c.ignored.join(',')}`);
    if (c.rejected.length) console.log(`  受けられない: ${c.rejected.join(',')}(${c.reasons.join(' / ')})`);
    for (const t of traces) console.log(`  玉型の誤り: ${t}`);
    const rep = [['JOB', name], ['STATUS', String(status)]];
    if (c.ignored.length) rep.push(['IGNORED', ...c.ignored]);
    if (c.rejected.length) rep.push(['REJECTED', ...c.rejected]);
    const ack = await s.request('RESULT', rep);
    console.log(`RESULT を返しました: ホストの STATUS=${ack.status}`);
    process.exitCode = status;
  } finally { s.close(); }
}

main().catch(e => { console.error(`失敗: ${e.message}`); process.exitCode = 1; });
