'use strict';
// セッション: ハンドシェイク、ペアリング、操作、分割、UART の再送(規格 4-9 章)。
const test = require('node:test');
const assert = require('node:assert/strict');
const { EventEmitter } = require('node:events');
const P = require('../src/index.js');

const hostKeys = P.noise.keyPair();
const devKeys = P.noise.keyPair();
const hostCaps = [['VER', '1.0'], ['DEV', 'HST'], ['VEN', 'TEST'], ['MODEL', 'HOST'], ['MAXFRAME', '4096']];
const edgerCaps = [['VER', '1.0'], ['DEV', 'EDG'], ['VEN', 'TEST'], ['MODEL', 'E-1'], ['SN', '0001'], ['MAXFRAME', '512'],
  ['LABELS', 'JOB', 'DO', 'DBL', 'HBOX', 'VBOX', 'TRCFMT', 'R', 'ETYP', 'BEVP', 'BEVM', 'BEVC'], ['TRACE', '1440', 'E'], ['EXT', 'ACME']];
const trust = list => peer => list.some(k => k.equals(peer.publicKey));
const radii = n => Array.from({ length: n }, (_, i) => String(2400 + (i % 300)));

async function pair({ hostTrusts = [devKeys.publicKey], devTrusts = [hostKeys.publicKey], handlers = {}, devHandlers = {} } = {}) {
  let hostSession;
  const got = new Promise(r => { hostSession = r; });
  const server = await P.listen(0, { keys: hostKeys, caps: hostCaps, trust: trust(hostTrusts), handlers }, s => hostSession(s));
  const port = server.address().port;
  const dev = await P.connect('127.0.0.1', port, { keys: devKeys, caps: edgerCaps, trust: trust(devTrusts), handlers: devHandlers });
  const host = await got;
  await host.opened;
  return { server, dev, host, close() { dev.close(); host.close(); server.close(); } };
}

test('ハンドシェイク: 互いの鍵とケイパビリティの通知を知り、同じハンドシェイクのハッシュになる', async () => {
  const p = await pair();
  try {
    assert.deepEqual(p.dev.peer.publicKey, hostKeys.publicKey);
    assert.deepEqual(p.host.peer.publicKey, devKeys.publicKey);
    assert.equal(p.host.peer.caps.dev, 'EDG');
    assert.equal(p.host.peer.caps.maxFrame, 512);
    assert.deepEqual(p.host.peer.caps.trace, { n: 1440, mode: 'E' });
    assert.ok(p.host.peer.caps.labels.includes('BEVC'));
    assert.equal(p.dev.peer.caps.maxFrame, 4096);
    assert.deepEqual(p.dev.hs.handshakeHash, p.host.hs.handshakeHash);
    assert.match(p.host.peer.fingerprint, /^[0-9A-HJKMNP-TV-Z]{4}(-[0-9A-HJKMNP-TV-Z]{4}){3}$/);
  } finally { p.close(); }
});

test('ペアリングしていない機器は、ホストが断る(機器には断られたことが分かる)。フィンガープリントはホストの側で分かる', async () => {
  let seen = null;
  const server = await P.listen(0, { keys: hostKeys, caps: hostCaps, trust: peer => { seen = peer.fingerprint; return false; } });
  try {
    await assert.rejects(P.connect('127.0.0.1', server.address().port, { keys: devKeys, caps: edgerCaps, trust: trust([hostKeys.publicKey]) }),
      e => e.code === 'refused-by-peer' && e.failCode === 3);
    assert.equal(seen, P.noise.fingerprint(devKeys.publicKey));
  } finally { server.close(); }
});

test('機器は、覚えていないホストとは話さない', async () => {
  const other = P.noise.keyPair();
  const server = await P.listen(0, { keys: other, caps: hostCaps, trust: () => true });
  try {
    await assert.rejects(P.connect('127.0.0.1', server.address().port, { keys: devKeys, caps: edgerCaps, trust: trust([hostKeys.publicKey]) }),
      e => e.code === 'refused' && e.failCode === 3);
  } finally { server.close(); }
});

test('PUT: 機器がジョブを送り、ホストが RESULT で答える', async () => {
  const got = [];
  const p = await pair({ handlers: { PUT: ({ records }) => { got.push(records); return 0; } } });
  try {
    const job = [['JOB', 'T-1'], ['DBL', '18.00'], ['TRCFMT', '1', '360', 'E', 'R', 'F'], ['R', ...radii(360)]];
    const r = await p.dev.request('PUT', job);
    assert.equal(r.opName, 'RESULT');
    assert.equal(r.status, 0);
    assert.deepEqual(got[0].filter(x => x[0] === 'R').flatMap(x => x.slice(1)), radii(360));
  } finally { p.close(); }
});

test('GET: 1440 点の玉型を、機器のフレームの上限(512 バイト)に合わせて分けて渡し、機器は RESULT で項目ごとの扱いを返す', async () => {
  const results = [];
  const job = [['JOB', 'E-1'], ['DO', 'B'], ['DBL', '18.00'], ['ETYP', '1'], ['BEVP', '3'], ['BEVM', '1.00', '1.00'], ['BEVC', '4.00', '4.00'], ['ACME:MODE', '2'], ['FOO', '1'],
    ['TRCFMT', '1', '1440', 'E', 'R', 'F'], ['R', ...radii(1440)], ['TRCFMT', '1', '1440', 'E', 'L', 'F'], ['R', ...radii(1440)]];
  const p = await pair({
    handlers: {
      GET: ({ records }) => (P.records.parse(P.records.format(records))[0][1] === 'E-1' ? { records: job } : 1),
      RESULT: ({ records }) => { results.push(records); return 0; },
    },
  });
  const sizes = [];
  const send = p.host.link.send.bind(p.host.link);
  p.host.link.send = f => { sizes.push(f.length); return send(f); };
  try {
    const r = await p.dev.request('GET', [['JOB', 'E-1']]);
    assert.equal(r.opName, 'DATA');
    assert.equal(r.status, null);
    assert.ok(sizes.length > 10 && sizes.every(n => n <= 512), `フレームの数 ${sizes.length}、最大 ${Math.max(...sizes)}`);
    assert.deepEqual(P.records.checkTraces(r.records), []);
    const c = P.records.classify(r.records, { accepts: new Set(p.dev.caps.find(x => x[0] === 'LABELS').slice(1)), ext: new Set(['ACME']) });
    assert.deepEqual(c.ignored.sort(), ['ACME:MODE', 'FOO'], 'ACME の名前空間は宣言したが、LABELS に無い');
    await p.dev.request('RESULT', [['JOB', 'E-1'], ['STATUS', '0'], ['IGNORED', ...c.ignored]]);
    assert.deepEqual(results[0].find(x => x[0] === 'IGNORED').slice(1).sort(), ['ACME:MODE', 'FOO']);
    // 無いジョブは RESULT の STATUS=1
    const nf = await p.dev.request('GET', [['JOB', 'NONE']]);
    assert.equal(nf.opName, 'RESULT');
    assert.equal(nf.status, 1);
  } finally { p.close(); }
});

test('AVAIL はホストから機器へ。TIME・PING は両方向。知らない操作には STATUS=3', async () => {
  const avail = [];
  const p = await pair({ devHandlers: { AVAIL: ({ records }) => { avail.push(records[0][1]); return 0; } }, handlers: { TIME: () => ({ records: [['TIME', '20261108120000']] }) } });
  try {
    assert.equal((await p.host.request('AVAIL', [['JOB', 'E-9']])).status, 0);
    assert.deepEqual(avail, ['E-9']);
    const t = await p.dev.request('TIME');
    assert.equal(t.opName, 'TIME');
    assert.deepEqual(t.records, [['TIME', '20261108120000']]);
    assert.equal((await p.host.request('PING')).opName, 'PING');
    assert.equal((await p.dev.request(0x90)).status, 3);
  } finally { p.close(); }
});

test('暗号文を書き換えると、受けた側は解けずに接続を閉じる', async () => {
  const p = await pair({ handlers: { PUT: () => 0 } });
  try {
    const send = p.dev.link.send.bind(p.dev.link);
    p.dev.link.send = f => { const g = Buffer.from(f); g[g.length - 1] ^= 1; return send(g); };
    const closed = new Promise(r => p.host.once('error', r));
    p.dev.request('PUT', [['JOB', 'X']]).catch(() => {});
    const err = await closed;
    assert.equal(err.code, 'decrypt');
  } finally { p.close(); }
});

// ---- UART(線の上で化ける・落ちる) ----
class Wire extends EventEmitter {
  constructor() { super(); this.peer = null; this.filter = b => b; }
  write(buf) { const b = this.filter(Buffer.from(buf)); if (b) setImmediate(() => this.peer.emit('data', b)); return true; }
  destroy() { this.emit('close'); }
}
function wires() { const a = new Wire(), b = new Wire(); a.peer = b; b.peer = a; return [a, b]; }

test('UART: 化けた・落ちたフレームは再送し、2 度届いたフレームは 1 度だけ使う', async () => {
  const [a, b] = wires();
  let n = 0;
  // 機器から送るものの 3 つめを落とし、5 つめを化けさせ、ACK の 1 つを落とす
  a.filter = buf => { n++; if (n === 3) return null; if (n === 5) { buf[2] ^= 0x40; } return buf; };
  let m = 0;
  b.filter = buf => { m++; return m === 2 ? null : buf; };
  const got = [];
  const hostLink = new P.UartLink(b, { ackTimeoutMs: 50 });
  const devLink = new P.UartLink(a, { ackTimeoutMs: 50 });
  const host = new P.Session({ link: hostLink, role: 'host', keys: hostKeys, caps: hostCaps, trust: trust([devKeys.publicKey]), handlers: { PUT: ({ records }) => { got.push(records); return 0; } } });
  const dev = new P.Session({ link: devLink, role: 'device', keys: devKeys, caps: edgerCaps, trust: trust([hostKeys.publicKey]) });
  host.start().catch(() => {});
  await dev.start();
  const job = [['JOB', 'U-1'], ['TRCFMT', '1', '360', 'E', 'R', 'F'], ['R', ...radii(360)]];
  for (let i = 0; i < 3; i++) assert.equal((await dev.request('PUT', job)).status, 0);
  assert.equal(got.length, 3, '同じフレームを 2 度使っていない');
  assert.deepEqual(got[2].filter(x => x[0] === 'R').flatMap(x => x.slice(1)), radii(360));
});

test('UART: ACK がまったく来なければ、決めた回数だけ再送して失敗する', async () => {
  const [a] = wires();
  a.filter = () => null;
  const link = new P.UartLink(a, { ackTimeoutMs: 20, retries: 2 });
  let sent = 0;
  const write = a.write.bind(a);
  a.write = buf => { sent++; return write(buf); };
  await assert.rejects(link.send(Buffer.from([1, 16, 0])), e => e.code === 'no-ack');
  assert.equal(sent, 3, '最初の 1 回と再送 2 回');
});
