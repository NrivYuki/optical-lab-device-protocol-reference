'use strict';
// spec/vectors のテストベクタ(規格の文章から別に作ったもの)を通す。
const test = require('node:test');
const assert = require('node:assert/strict');
const { EventEmitter } = require('node:events');
const P = require('../src/index.js');
const V = require('../../spec/vectors/protocol.json');
const REC = require('../../spec/vectors/records.json');

const hex = s => Buffer.from(s, 'hex');
const R = P.records;

test('COBS', () => {
  for (const c of V.cobs) {
    assert.equal(P.cobs.encode(hex(c.raw)).toString('hex'), c.encoded);
    assert.equal(P.cobs.decode(hex(c.encoded)).toString('hex'), c.raw);
  }
});

test('CRC-32', () => { for (const c of V.crc32) assert.equal(P.crc32(Buffer.from(c.text)).toString(16).padStart(8, '0'), c.crc); });

test('UART の 1 つ', () => {
  for (const c of V.uart) {
    assert.equal(P.uartEncode(c.kind, c.seq, hex(c.body)).toString('hex'), c.wire);
    const d = P.uartDecode(hex(c.wire).subarray(0, -1));
    assert.equal(d.kind, c.kind);
    assert.equal(d.lseq, c.seq);
    assert.equal(d.body.toString('hex'), c.body);
  }
});

test('フィンガープリント', () => { for (const c of V.fingerprint) assert.equal(P.noise.fingerprint(hex(c.public)), c.fingerprint); });

// 決めた相手の代わりに、ベクタのバイト列を順に流すトランスポート
class ScriptLink extends EventEmitter {
  constructor(me, steps) { super(); this.me = me; this.steps = steps; this.i = 0; this.errors = []; }
  run() { this.feed(); }
  feed() {
    while (this.i < this.steps.length && this.steps[this.i].from !== this.me) {
      const s = this.steps[this.i++];
      this.emit('frame', hex(s.frame));
    }
  }
  send(frame) {
    const s = this.steps[this.i++];
    if (!s || s.from !== this.me) this.errors.push(`余分なフレーム ${frame.toString('hex').slice(0, 40)}`);
    else if (frame.toString('hex') !== s.frame) this.errors.push(`${s.note}: 食い違い`);
    setImmediate(() => this.feed());
    return Promise.resolve();
  }
  close() {}
}

const S = V.session;
const keys = side => P.noise.keyPair(hex(S[side].static));
const caps = side => R.parse(S[side].caps);
const eph = side => P.noise.keyPair(hex(S[side].ephemeral));

test('セッションのテストベクタ: 機器の側のフレームがバイト列まで一致する', async () => {
  const link = new ScriptLink('device', S.steps);
  const s = new P.Session({ link, role: 'device', keys: keys('device'), caps: caps('device'), e: eph('device'), trust: p => p.fingerprint === S.host.fingerprint });
  await Promise.all([s.start(), Promise.resolve().then(() => link.run())]);
  assert.deepEqual([...s.hs.handshakeHash].map(b => b.toString(16).padStart(2, '0')).join(''), S.handshakeHash);
  assert.equal((await s.request('PUT', R.parse(S.put))).status, 0);
  const got = await s.request('GET', R.parse(S.get));
  assert.equal(got.opName, 'DATA');
  assert.equal(R.format(got.records), S.data);
  assert.equal((await s.request('RESULT', R.parse(S.result))).status, 0);
  assert.deepEqual(link.errors, []);
  assert.equal(link.i, S.steps.length);
});

test('セッションのテストベクタ: ホストの側のフレームがバイト列まで一致する', async () => {
  const link = new ScriptLink('host', S.steps);
  const put = [];
  const s = new P.Session({
    link, role: 'host', keys: keys('host'), caps: caps('host'), e: eph('host'), trust: p => p.fingerprint === S.device.fingerprint,
    handlers: { PUT: ({ records }) => { put.push(records); return 0; }, GET: () => ({ records: R.parse(S.data) }), RESULT: () => 0 },
  });
  const opened = s.start();
  link.run();
  await opened;
  await new Promise(r => setTimeout(r, 100));
  assert.deepEqual(link.errors, []);
  assert.equal(link.i, S.steps.length);
  assert.equal(R.format(put[0]), S.put);
});

test('項目の扱いのテストベクタ', () => {
  for (const c of REC.cases) {
    const recs = R.parse(c.lines.join('\r\n'));
    const out = R.classify(recs, { accepts: c.accept ? new Set(c.accept) : null, ext: new Set(c.ext) });
    assert.deepEqual(out.used.sort(), [...c.used].sort(), `${c.name}: used`);
    assert.deepEqual(out.ignored.sort(), [...c.ignored].sort(), `${c.name}: ignored`);
    assert.deepEqual(out.rejected.sort(), [...c.rejected].sort(), `${c.name}: rejected ${out.reasons}`);
    assert.equal(R.checkTraces(recs).length > 0, c.badTrace, `${c.name}: badTrace`);
  }
  for (const line of REC.malformed) assert.throws(() => R.parse(line), { code: /bad-line|line-too-long/ }, line);
});

test('UART: 同じ番号でも中身が違えば新しいフレームとして受ける(機器がやり直したとき)', () => {
  const e = new EventEmitter();
  e.write = () => true;
  const link = new P.UartLink(e);
  const got = [];
  link.on('frame', f => got.push(f.toString('hex')));
  e.emit('data', P.uartEncode(0, 0, hex('010101')));
  e.emit('data', P.uartEncode(0, 0, hex('010101')));
  e.emit('data', P.uartEncode(0, 0, hex('010102')));
  assert.deepEqual(got, ['010101', '010102']);
});

function linePair() {
  const a = new EventEmitter(), b = new EventEmitter();
  a.write = x => { setImmediate(() => b.emit('data', Buffer.from(x))); return true; };
  b.write = x => { setImmediate(() => a.emit('data', Buffer.from(x))); return true; };
  a.destroy = b.destroy = () => {};
  return [a, b];
}

test('UART: ホストは、機器がセッションをやり直したら前のセッションを捨てて受け直す', async () => {
  const [a, b] = linePair();
  const hk = keys('host'), dk = keys('device');
  const host = new P.Session({ link: new P.UartLink(b, { ackTimeoutMs: 50 }), role: 'host', keys: hk, caps: caps('host'), trust: () => true, handlers: { PUT: () => 0 } });
  host.start().catch(() => {});
  const devOpts = { role: 'device', keys: dk, caps: caps('device'), trust: () => true };
  const d1 = new P.Session({ ...devOpts, link: new P.UartLink(a, { ackTimeoutMs: 50 }) });
  await d1.start();
  assert.equal((await d1.request('PUT', [['JOB', 'A']])).status, 0);
  d1.closed = true;
  a.removeAllListeners('data');
  const d2 = new P.Session({ ...devOpts, link: new P.UartLink(a, { ackTimeoutMs: 50 }) });
  await d2.start();
  assert.equal((await d2.request('PUT', [['JOB', 'B']])).status, 0);
  d2.close(); host.close();
});

test('ハンドシェイクが止まったら、決めた時間で閉じる', async () => {
  const [, b] = linePair();
  const host = new P.Session({ link: new P.UartLink(b), role: 'host', keys: keys('host'), caps: caps('host'), handshakeTimeoutMs: 30 });
  await assert.rejects(host.start(), { code: 'handshake-timeout' });
});

test('相手のメジャーバージョンが違えば、ペアリングを見る前に断る', async () => {
  const [a, b] = linePair();
  let asked = false;
  const host = new P.Session({ link: new P.UartLink(b, { ackTimeoutMs: 50 }), role: 'host', keys: keys('host'), caps: caps('host'), trust: () => { asked = true; return true; } });
  host.on('error', () => {});
  host.start().catch(() => {});
  const old = R.parse(S.device.caps).map(r => (r[0] === 'VER' ? ['VER', '2.0'] : r));
  const dev = new P.Session({ link: new P.UartLink(a, { ackTimeoutMs: 50 }), role: 'device', keys: keys('device'), caps: old, trust: () => true });
  dev.on('error', () => {});
  await assert.rejects(dev.start(), e => e.failCode === 1);
  assert.equal(asked, false);
});

test('ケイパビリティの通知に必須の項目が無ければ断る', async () => {
  const [a, b] = linePair();
  const host = new P.Session({ link: new P.UartLink(b, { ackTimeoutMs: 50 }), role: 'host', keys: keys('host'), caps: caps('host'), trust: () => true });
  host.on('error', () => {});
  host.start().catch(() => {});
  const dev = new P.Session({ link: new P.UartLink(a, { ackTimeoutMs: 50 }), role: 'device', keys: keys('device'), caps: [['VER', '1.0'], ['DEV', 'EDG']], trust: () => true });
  dev.on('error', () => {});
  await assert.rejects(dev.start(), e => e.failCode === 4);
});

async function openPair({ hostCaps, devCaps, handlers, devHandlers }) {
  const [a, b] = linePair();
  const host = new P.Session({ link: new P.UartLink(b, { ackTimeoutMs: 50 }), role: 'host', keys: keys('host'), caps: hostCaps, trust: () => true, handlers });
  const hostOpen = host.start();
  const dev = new P.Session({ link: new P.UartLink(a, { ackTimeoutMs: 50 }), role: 'device', keys: keys('device'), caps: devCaps, trust: () => true, handlers: devHandlers });
  await dev.start();
  await hostOpen;
  return { host, dev };
}

test('独自の操作: 両方が宣言した名前空間なら使え、宣言していなければ STATUS=3', async () => {
  const seen = [];
  const hostCaps = [...caps('host'), ['EXT', 'ACME']], devCaps = [...caps('device'), ['EXT', 'ACME', 'BETA']];
  const { host, dev } = await openPair({ hostCaps, devCaps, handlers: { VENDOR: ({ op, namespace, records }) => { seen.push([op, namespace, records]); return 0; } } });
  assert.equal((await dev.request(0x81, [['EXT', 'ACME'], ['ACME:MODE', '2']])).status, 0);
  assert.deepEqual(seen, [[0x81, 'ACME', [['ACME:MODE', '2']]]]);
  assert.equal((await dev.request(0x81, [['EXT', 'BETA'], ['BETA:X', '1']])).status, 3, 'ホストは BETA を宣言していない');
  assert.equal((await dev.request(0x81, [['DBL', '1']])).status, 3, '名前空間の行が無い');
  dev.close(); host.close();
});

test('80 文字を超える行の要求には STATUS=2、大きすぎるメッセージには STATUS=5 で答える', async () => {
  const { host, dev } = await openPair({ hostCaps: caps('host'), devCaps: caps('device'), handlers: { PUT: () => 0 } });
  const send = lines => dev.sendMessage(1, 77, 0, Buffer.from(lines, 'ascii'));
  const reply = () => new Promise(resolve => {
    const orig = dev.onMessage.bind(dev);
    dev.onMessage = m => { dev.onMessage = orig; resolve(m); };
  });
  let w = reply();
  await send(`A=${'1'.repeat(79)}\r\n`);
  let m = await w;
  assert.equal(m[0], 4);
  assert.match(m.subarray(4).toString(), /^STATUS=2/);
  const big = `R=${'1'.repeat(70)}\r\n`.repeat(4000);
  w = reply();
  await send(big);
  m = await w;
  assert.match(m.subarray(4).toString(), /^STATUS=5/);
  dev.close(); host.close();
});

test('UART の使用中のテストベクタ: 機器は確かめられないフレームを捨て、PING の応答がバイト列まで一致する', async () => {
  const steps = [...S.steps, ...S.uartGuard.device];
  const link = new ScriptLink('device', steps);
  link.isUart = true;
  const s = new P.Session({ link, role: 'device', keys: keys('device'), caps: caps('device'), e: eph('device'), trust: p => p.fingerprint === S.host.fingerprint });
  await Promise.all([s.start(), Promise.resolve().then(() => link.run())]);
  await s.request('PUT', R.parse(S.put));
  await s.request('GET', R.parse(S.get));
  await s.request('RESULT', R.parse(S.result));
  await new Promise(r => setTimeout(r, 50));
  assert.deepEqual(link.errors, []);
  assert.equal(link.i, steps.length);
  assert.ok(!s.closed);
});

test('UART の使用中のテストベクタ: ホストは確かめられないフレームを捨て、PING の応答がバイト列まで一致する', async () => {
  const steps = [...S.steps, ...S.uartGuard.host];
  const link = new ScriptLink('host', steps);
  link.isUart = true;
  const s = new P.Session({
    link, role: 'host', keys: keys('host'), caps: caps('host'), e: eph('host'), trust: p => p.fingerprint === S.device.fingerprint,
    handlers: { PUT: () => 0, GET: () => ({ records: R.parse(S.data) }), RESULT: () => 0 },
  });
  const opened = s.start();
  link.run();
  await opened;
  await new Promise(r => setTimeout(r, 100));
  assert.deepEqual(link.errors, []);
  assert.equal(link.i, steps.length);
  assert.ok(!s.closed);
});

test('REQUIRE= の判定のテストベクタ', async () => {
  for (const c of V.capabilities) {
    const [a, b] = linePair();
    const host = new P.Session({ link: new P.UartLink(b, { ackTimeoutMs: 50 }), role: 'host', keys: keys('host'), caps: [...caps('host'), ...R.parse(c.host.join('\r\n'))], trust: () => true });
    const dev = new P.Session({ link: new P.UartLink(a, { ackTimeoutMs: 50 }), role: 'device', keys: keys('device'), caps: [...caps('device'), ...R.parse(c.device.join('\r\n'))], trust: () => true });
    host.on('error', () => {}); dev.on('error', () => {});
    host.start().catch(() => {});
    if (c.fail) await assert.rejects(dev.start(), e => e.failCode === c.fail, c.name);
    else { await dev.start(); await host.opened; }
    host.close(); dev.close();
  }
});
