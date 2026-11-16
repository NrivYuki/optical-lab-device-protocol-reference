'use strict';
// 第 1.1 版で決めた、テストベクタで表しにくい決まり(規格 4・7・8・9 章)。
const test = require('node:test');
const assert = require('node:assert/strict');
const net = require('node:net');
const { EventEmitter } = require('node:events');
const P = require('../src/index.js');
const { TYPES, buildFrame } = P;

const CAPS = d => [['VER', '1.1'], ['DEV', d], ['VEN', 'T'], ['MODEL', 'M'], ['MAXFRAME', '512']];

/** 2 つの口をつなぐ線 */
function linePair() {
  const a = new EventEmitter(), b = new EventEmitter();
  a.write = d => { setImmediate(() => b.emit('data', Buffer.from(d))); return true; };
  b.write = d => { setImmediate(() => a.emit('data', Buffer.from(d))); return true; };
  a.destroy = b.destroy = () => {};
  return [a, b];
}

async function pair({ hostOpts = {}, devOpts = {}, devCaps = CAPS('EDG'), hostCaps = CAPS('HST') } = {}) {
  const [a, b] = linePair();
  const host = new P.Session({ link: new P.UartLink(b, { ackTimeoutMs: 50 }), role: 'host', keys: P.noise.keyPair(), caps: hostCaps, trust: () => true, ...hostOpts });
  const dev = new P.Session({ link: new P.UartLink(a, { ackTimeoutMs: 50 }), role: 'device', keys: P.noise.keyPair(), caps: devCaps, trust: () => true, ...devOpts });
  host.on('error', () => {}); dev.on('error', () => {});
  host.start().catch(() => {});
  await dev.start();
  await host.opened;
  return { host, dev };
}
const done = (...ss) => { for (const s of ss) s.close(); };

test('TCP: 接続から決めた時間のうちに 1 つ目のメッセージが来なければ、ホストは閉じる(規格 4 章)', async () => {
  const server = await P.listen(0, () => ({ keys: P.noise.keyPair(), caps: CAPS('HST'), firstMessageMs: 100 }), null, {});
  const sock = net.connect(server.address().port, '127.0.0.1');
  const closed = new Promise(r => sock.on('close', r));
  sock.on('error', () => {});
  const t0 = Date.now();
  await closed;
  assert.ok(Date.now() - t0 < 2000, '待ち時間の後に閉じられた');
  server.close();
});

test('ケイパビリティの形が正しくなければ、失敗(理由 4)で断る(規格 9 章)', async () => {
  for (const bad of [
    [['VER', '1'], ['DEV', 'EDG'], ['VEN', 'T'], ['MODEL', 'M'], ['MAXFRAME', '512']],
    [['VER', '1.1'], ['DEV', 'EDG'], ['VEN', 'T'], ['MODEL', 'M'], ['MAXFRAME', 'x']],
    [...CAPS('EDG'), ['DEV', 'TRC']],
  ]) {
    await assert.rejects(pair({ devCaps: bad }), e => e.failCode === 4, JSON.stringify(bad));
  }
});

test('応答の時間切れにしたリクエスト ID は、決めた時間のうちは使わない。待っていない ID の応答は捨てる(規格 7 章)', async () => {
  let n = 0;
  const { host, dev } = await pair({
    devOpts: { timeoutMs: 80 },
    hostOpts: { handlers: { PING: () => (n++ === 0 ? new Promise(r => setTimeout(() => r({ records: [] }), 200)) : { records: [] }) } },
  });
  await assert.rejects(dev.request('PING'), e => e.code === 'timeout');
  const first = [...dev.recent.keys()];
  assert.equal(first.length, 1);
  dev.nextReq = first[0];   // 同じ ID を次に選ぶ並びにしても、使わない
  const sent = [];
  const orig = dev.sendMessage.bind(dev);
  dev.sendMessage = (op, id, ...rest) => { sent.push(id); return orig(op, id, ...rest); };
  const r = await dev.request('PING');
  assert.equal(r.opName, 'PING');
  assert.notEqual(sent[0], first[0], '時間切れにした ID を使わない');
  await new Promise(res => setTimeout(res, 250));   // 遅れて届いた 1 つ目の応答は、待っていない ID なので捨てる
  assert.equal(dev.pending.size, 0);
  done(host, dev);
});

test('機器は、応答を待っている要求がホストの MAXREQ(無ければ 1)に達しているうちは、次の要求を出さない(規格 7 章)', async () => {
  for (const [extra, want] of [[[], 1], [[['MAXREQ', '3']], 3]]) {
    let inFlight = 0, most = 0;
    const { host, dev } = await pair({
      hostCaps: [...CAPS('HST'), ...extra],
      hostOpts: { handlers: { PING: async () => { inFlight++; most = Math.max(most, inFlight); await new Promise(r => setTimeout(r, 60)); inFlight--; return { records: [] }; } } },
    });
    await Promise.all(Array.from({ length: 6 }, () => dev.request('PING')));
    assert.equal(most, want, `MAXREQ ${want}`);
    done(host, dev);
  }
});

test('続きのフレームで操作の番号が変わった要求、続きのフラグが付いた機器への要求には STATUS=2 で答える(規格 7 章)', async () => {
  const { host, dev } = await pair();
  const sent = [];
  const orig = host.respond.bind(host);
  // ホストへ: 1 つ目は PUT、続きは INFO
  const seen = new Promise(r => { host.respond = (op, id, recs) => { sent.push(recs); r(); return orig(op, id, recs); }; });
  await dev.sendMessage(P.OPS.PUT, 7, 1, Buffer.from('JOB=A\r\n'));
  await dev.sendMessage(P.OPS.INFO, 7, 0, Buffer.from('EVENT=DONE\r\n'));
  await seen;
  assert.equal(sent[0][0][1], '2');
  // 機器へ: 続きのフラグが付いた AVAIL
  const got = [];
  const orig2 = dev.respond.bind(dev);
  const seen2 = new Promise(r => { dev.respond = (op, id, recs) => { got.push(recs); r(); return orig2(op, id, recs); }; });
  await host.sendMessage(P.OPS.AVAIL, 9, 1, Buffer.from('JOB=A\r\n'));
  await seen2;
  assert.equal(got[0][0][1], '2');
  done(host, dev);
});

test('UART: 機器は、応答の時間切れが 2 度続いたら、新しいハンドシェイクを始める(規格 4 章)', async () => {
  const { host, dev } = await pair({ devOpts: { timeoutMs: 60 }, hostOpts: { handlers: { PING: () => new Promise(() => {}) } } });
  const restarted = new Promise(r => dev.once('restart', r));
  await assert.rejects(dev.request('PING'), e => e.code === 'timeout');
  await assert.rejects(dev.request('PING'), e => e.code === 'timeout');
  await restarted;
  await new Promise(r => dev.once('open', r));
  assert.ok(dev.ready, '新しいセッションが使える');
  done(host, dev);
});

test('UART: ホストは、ハンドシェイクの 2 つ目のメッセージから、シーケンス番号を 0 から付ける(規格 4 章)', async () => {
  const [a, b] = linePair();
  const link = new P.UartLink(b, { ackTimeoutMs: 50 });
  link.nextSeq = 9;
  const seqs = [];
  const orig = b.write;
  b.write = d => { const u = P.uartDecode(Buffer.from(d).subarray(0, -1)); if (u && u.kind === 0) seqs.push(u.lseq); return orig(d); };
  const host = new P.Session({ link, role: 'host', keys: P.noise.keyPair(), caps: CAPS('HST'), trust: () => true });
  const dev = new P.Session({ link: new P.UartLink(a, { ackTimeoutMs: 50 }), role: 'device', keys: P.noise.keyPair(), caps: CAPS('EDG'), trust: () => true });
  host.on('error', () => {}); dev.on('error', () => {});
  host.start().catch(() => {});
  await dev.start();
  assert.equal(seqs[0], 0);
  done(host, dev);
});

test('閉じたら、待っている要求は失敗する。時間切れにした応答の、ためた量は戻す(規格 7 章)', async () => {
  const { host, dev } = await pair({ devOpts: { timeoutMs: 80 }, hostOpts: { handlers: { PING: () => new Promise(() => {}) } } });
  // 分けた応答の 1 つ目だけを送って止める
  const p = dev.request('PING').catch(e => e);
  await new Promise(r => setTimeout(r, 20));
  const id = [...dev.pending.keys()][0];
  await host.sendMessage(P.OPS.PING, id, 2 | 1, Buffer.from('X=1\r\n'));
  const err = await p;
  assert.equal(err.code, 'timeout');
  assert.equal(dev.partialBytes, 0, '時間切れの後に、ためた量が残らない');
  const q = dev.request('PING');
  dev.close();
  await assert.rejects(q, e => e.code === 'closed');
  host.close();
});

test('UART: 機器は、ホストから決めた時間何も届かなければ PING を送る(規格 4 章)', async () => {
  let pings = 0;
  const { host, dev } = await pair({ devOpts: { keepAliveMs: 60 }, hostOpts: { handlers: { PING: () => { pings++; return { records: [] }; } } } });
  await new Promise(r => setTimeout(r, 400));
  assert.ok(pings >= 2, `PING ${pings} 回`);
  done(host, dev);
});

test('ASCII の 0x20-0x7E 以外のバイトを含む要求には、STATUS=2 で答える(規格 8 章)', async () => {
  let got = null;
  const { host, dev } = await pair({ hostOpts: { handlers: { PUT: ({ records }) => { got = records; return 0; } } } });
  const id = 41;
  const wait = new Promise(r => { const o = host.respond.bind(host); host.respond = (op, i, recs) => { r(recs); return o(op, i, recs); }; });
  await dev.sendMessage(P.OPS.PUT, id, 0, Buffer.from([...Buffer.from('JOB=A\r\nDBL=18.0'), 0xb0, 0x0d, 0x0a]));
  const recs = await wait;
  assert.equal(recs[0][1], '2');
  assert.equal(got, null, '保存しない');
  done(host, dev);
});

test('ハンドシェイクのフレームが 512 バイトを超えるケイパビリティは送らない(規格 5 章)', async () => {
  const big = [...CAPS('EDG'), ...Array.from({ length: 12 }, (_, i) => ['LABELS', ...Array.from({ length: 6 }, (_, k) => `L${i}${k}XXXXXX`)])];
  await assert.rejects(pair({ devCaps: big }), e => e.code === 'handshake-too-big' || e.code === 'handshake-timeout' || e.failCode);
});

test('VER の無いケイパビリティは、理由 1 ではなく理由 4 で断る(規格 9 章)', async () => {
  await assert.rejects(pair({ devCaps: [['DEV', 'EDG'], ['VEN', 'T'], ['MODEL', 'M'], ['MAXFRAME', '512']] }), e => e.failCode === 4);
});

test('受け入れの通知を待つ間に解けたほかのデータのフレームは、機器が捨てる(規格 6 章)', async () => {
  const link = new EventEmitter();
  link.isUart = true;
  link.send = () => Promise.resolve();
  link.close = () => {};
  const s = new P.Session({ link, role: 'device', keys: P.noise.keyPair(), caps: CAPS('EDG') });
  s.ready = false; s.secured = true;
  // onMessage に直接、受け入れの通知でないものを渡す
  s.onMessage(Buffer.from([P.OPS.PING, 0, 5, 0]));
  assert.ok(!s.closed && !s.ready);
  s.onMessage(Buffer.from([P.OPS.RESULT, 0, 0, 2]));
  assert.ok(s.ready);
  s.close();
  void TYPES; void buildFrame;
});

test('応答の待ちは、分けた応答の続きが届くたびに数え直す(規格 8 章)', async () => {
  const { host, dev } = await pair({ devOpts: { timeoutMs: 400 }, hostOpts: { handlers: { PING: () => new Promise(() => {}) } } });
  const p = dev.request('PING');
  await new Promise(r => setTimeout(r, 20));
  const id = [...dev.pending.keys()][0];
  // 続きを 250 ms ごとに 3 つ送る。全体では 400 ms を超えるが、フレームの間は超えない
  for (let i = 0; i < 3; i++) {
    await new Promise(r => setTimeout(r, 250));
    await host.sendMessage(P.OPS.PING, id, 2 | (i < 2 ? 1 : 0), Buffer.from(`X=${i}\r\n`));
  }
  const r = await p;
  assert.equal(r.records.length, 3);
  done(host, dev);
});
