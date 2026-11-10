'use strict';
// 基本仕様の決まり(規格 4・5・9 章): UART の使用中は暗号で確かめられないものでセッションを終えない、拡張の必須の宣言、接続数の上限。
const test = require('node:test');
const assert = require('node:assert/strict');
const net = require('node:net');
const { EventEmitter } = require('node:events');
const crypto = require('node:crypto');
const P = require('../src/index.js');
const { TYPES, buildFrame, uartEncode } = P;

const CAPS = d => [['VER', '1.0'], ['DEV', d], ['VEN', 'T'], ['MODEL', 'M'], ['MAXFRAME', '512']];

/** 1 つのホストの線に、機器の側の口をいくつでもつなげる(線に割り込む相手を表す) */
function wire() {
  const host = new EventEmitter(), taps = [];
  host.write = d => setImmediate(() => { for (const t of taps) t.emit('data', Buffer.from(d)); });
  host.destroy = () => {};
  const tap = () => {
    const t = new EventEmitter();
    t.write = d => setImmediate(() => host.emit('data', Buffer.from(d)));
    t.destroy = () => {};
    taps.push(t);
    return t;
  };
  return { host, tap };
}
const link = s => new P.UartLink(s, { ackTimeoutMs: 30, retries: 1 });

async function pair({ hostOpts = {}, devOpts = {} } = {}) {
  const hk = P.noise.keyPair(), dk = P.noise.keyPair();
  const w = wire();
  const hostFp = P.noise.fingerprint(hk.publicKey), devFp = P.noise.fingerprint(dk.publicKey);
  const host = new P.Session({ link: link(w.host), role: 'host', keys: hk, caps: CAPS('HST'), trust: p => p.fingerprint === devFp, ...hostOpts });
  const dev = new P.Session({ link: link(w.tap()), role: 'device', keys: dk, caps: CAPS('EDG'), trust: p => p.fingerprint === hostFp, ...devOpts });
  host.on('error', () => {}); dev.on('error', () => {});
  host.start().catch(() => {});
  await dev.start();
  await host.opened;
  return { w, host, dev, hk, dk, hostFp, devFp };
}
const inject = (t, frame, seq) => t.write(uartEncode(0, seq, frame));
const tick = (ms = 80) => new Promise(r => setTimeout(r, ms));
const done = (...s) => s.forEach(x => x.close());

test('UART の使用中: 偽のハンドシェイクの 1 つめでは前のセッションを捨てない', async () => {
  const { w, host, dev } = await pair();
  let restarted = 0;
  host.on('restart', () => restarted++);
  inject(w.tap(), buildFrame(TYPES.hs1, crypto.randomBytes(32)), 200);
  await tick();
  assert.equal(restarted, 0);
  assert.equal((await dev.request('PING')).opName, 'PING');
  done(host, dev);
});

test('UART の使用中: 失敗のフレームでは両側ともセッションを終えない', async () => {
  const { w, host, dev } = await pair();
  const t = w.tap();
  inject(t, buildFrame(TYPES.fail, Buffer.from([2])), 201);
  w.host.write(uartEncode(0, 202, buildFrame(TYPES.fail, Buffer.from([2]))));
  await tick();
  assert.ok(!host.closed && !dev.closed);
  assert.equal((await dev.request('PING')).opName, 'PING');
  done(host, dev);
});

test('UART の使用中: 解けないデータ・バージョン違い・短いフレームは捨てて、シーケンス番号も進めない', async () => {
  const { w, host, dev } = await pair();
  const t = w.tap();
  inject(t, buildFrame(TYPES.data, crypto.randomBytes(40)), 203);
  inject(t, Buffer.concat([Buffer.from([9, TYPES.data]), crypto.randomBytes(20)]), 204);
  inject(t, Buffer.from([1]), 205);
  await tick();
  assert.ok(!host.closed);
  assert.equal((await dev.request('PING')).opName, 'PING');
  done(host, dev);
});

test('UART の使用中: ペアリングしていない相手がハンドシェイクを終えても、前のセッションは続き、相手は理由 3 で断られる', async () => {
  const { w, host, dev, hostFp } = await pair();
  const evil = new P.Session({ link: link(w.tap()), role: 'device', keys: P.noise.keyPair(), caps: CAPS('EDG'), trust: p => p.fingerprint === hostFp });
  evil.on('error', () => {});
  await assert.rejects(evil.start(), e => e.failCode === 3);
  assert.ok(!host.closed && !dev.closed);
  assert.equal((await dev.request('PING')).opName, 'PING');
  done(host, dev);
});

test('UART の使用中: 同じ機器がやり直してハンドシェイクを終えたら入れ替わり、待っていた要求は失敗する', async () => {
  const { w, host, dev, dk, hostFp } = await pair({ devOpts: { handlers: { AVAIL: () => new Promise(() => {}) } } });
  const events = [];
  host.on('restart', () => events.push('restart'));
  host.on('open', () => events.push('open'));
  const waiting = host.request('AVAIL', [['JOB', 'A']]).catch(e => e.code);
  await tick(20);
  const again = new P.Session({ link: link(w.tap()), role: 'device', keys: dk, caps: CAPS('EDG'), trust: p => p.fingerprint === hostFp });
  again.on('error', () => {});
  await again.start();
  assert.deepEqual(events, ['restart', 'open']);
  assert.equal(await waiting, 'restarted');
  assert.equal((await again.request('PING')).opName, 'PING');
  done(host, dev, again);
});

test('REQUIRE: 相手の EXT に無ければ、ホストも機器も理由 5 で断る。あれば話す', async () => {
  const ext = ['EXT', 'ACME'];
  for (const [hostExtra, devExtra, ok] of [
    [[['REQUIRE', 'ACME']], [], false],
    [[], [['REQUIRE', 'ACME']], false],
    [[['REQUIRE', 'ACME']], [ext], true],
    [[ext], [['REQUIRE', 'ACME'], ext], true],
  ]) {
    const hk = P.noise.keyPair(), dk = P.noise.keyPair();
    const w = wire();
    const host = new P.Session({ link: link(w.host), role: 'host', keys: hk, caps: [...CAPS('HST'), ...hostExtra], trust: () => true });
    const dev = new P.Session({ link: link(w.tap()), role: 'device', keys: dk, caps: [...CAPS('EDG'), ...devExtra], trust: () => true });
    host.on('error', () => {}); dev.on('error', () => {});
    host.start().catch(() => {});
    if (ok) { await dev.start(); await host.opened; done(host, dev); }
    else await assert.rejects(Promise.all([dev.start(), host.opened]), e => e.failCode === 5);
  }
});

test('TCP: ハンドシェイクの途中の接続は上限まで。上限を超えた接続は閉じ、終われば空く', async () => {
  const server = await P.listen(0, () => ({ keys: P.noise.keyPair(), caps: CAPS('HST'), handshakeTimeoutMs: 150 }), null, { maxHandshakes: 2 });
  const { port } = server.address();
  const open = () => new Promise(r => { const c = net.connect(port, '127.0.0.1'); c.closedAt = null; c.on('close', () => { c.closedAt = Date.now(); }); c.on('error', () => {}); c.once('connect', () => r(c)); });
  try {
    const a = await open(), b = await open();
    const c = await open();
    await tick(50);
    assert.ok(!a.closedAt && !b.closedAt, '上限までは閉じない');
    assert.ok(c.closedAt, '上限を超えた接続は閉じる');
    await tick(250);
    const d = await open();
    await tick(30);
    assert.ok(!d.closedAt, 'ハンドシェイクが時間切れで終われば空く');
    d.destroy(); a.destroy(); b.destroy();
  } finally { await new Promise(r => server.close(r)); }
});

test('定義: 標準の拡張の予約と理由 5 が定義にある', () => {
  const D = require('../../spec/messages.json');
  assert.equal(D.message.standardExtPrefix, 'OLDP');
  assert.ok(D.fail['5']);
  assert.equal(D.tcp.maxHandshakes, 8);
  assert.ok(D.capabilities.labels.includes('REQUIRE'));
});
