'use strict';
// セッション(規格 6-9 章): ハンドシェイク、ケイパビリティの通知、ペアリングの確認、メッセージの分割と組み立て、操作の要求と応答。
// 機器(initiator)とホスト(responder)の両方をこのクラスで扱う。トランスポート(TcpLink・UartLink)の上に載せる。

const { EventEmitter } = require('node:events');
const DEF = require('../../spec/messages.json');
const { Handshake, fingerprint } = require('./noise.js');
const { buildFrame, parseFrame, TYPES, VERSION } = require('./frame.js');
const R = require('./records.js');

const OPS = DEF.message.ops;
const OP_NAME = Object.fromEntries(Object.entries(OPS).map(([k, v]) => [v, k]));
const FLAG = DEF.message.flags;
const HEADER = DEF.message.headerSize;
const MIN_FRAME = DEF.frame.minSize;
const PROLOGUE = Buffer.from(DEF.noise.prologue, 'ascii');
const TAG = 16;
const MAX_BODY = DEF.message.maxBodySize;
const PARTIAL_LIMIT = 2 * MAX_BODY;   // 組み立て途中のメッセージにためる量の、セッションごとの上限
const MAJOR = DEF.version.split('.')[0];
const REUSE_MS = DEF.timeouts.responseMs;   // 時間切れにしたリクエスト ID を使わない時間(規格 7 章)
const fail = (code, message, extra) => Object.assign(new Error(message), { code }, extra);

/** ケイパビリティの通知([ラベル, 値...] の並び)を、扱いやすい形にする */
function readCaps(records) {
  const v = l => { const r = records.find(x => x[0] === l); return r ? r.slice(1) : null; };
  const all = l => records.filter(x => x[0] === l).flatMap(x => x.slice(1)).filter(Boolean);
  const trace = v('TRACE');
  return {
    version: (v('VER') || [''])[0], dev: (v('DEV') || [''])[0], ven: (v('VEN') || [''])[0], model: (v('MODEL') || [''])[0], sn: (v('SN') || [''])[0],
    maxFrame: Math.min(DEF.frame.maxSize, Math.max(MIN_FRAME, Number((v('MAXFRAME') || [MIN_FRAME])[0]) || MIN_FRAME)),
    // ホストが申告する、機器が応答を待たずに出してよい要求の数(規格 9 章。無ければ 1)
    maxReq: Math.min(255, Math.max(1, Number((v('MAXREQ') || ['1'])[0]) || 1)),
    labels: all('LABELS'), ext: all('EXT'), require: all('REQUIRE'),
    trace: trace ? { n: Number(trace[0]) || 0, mode: trace[1] || 'E' } : null,
    records,
  };
}

/**
 * @param {object} o
 *   link: TcpLink | UartLink
 *   role: 'device' | 'host'
 *   keys: 自分の静的鍵の組 { privateKey, publicKey }
 *   caps: 自分のケイパビリティの通知([ラベル, 値...] の並び)
 *   trust(peer): 相手を信じるか(peer は { publicKey, fingerprint, caps })。Promise でもよい
 *   handlers: 相手からの要求を扱う関数 { PUT, GET, RESULT, AVAIL, INFO, TIME, PING, VENDOR }。({ session, records, reqId, op }) → 応答の { op?, records } か status の番号
 *             VENDOR は独自の操作(0x80 以降)。中身の最初の行(EXT=名前空間)を除いて records に渡し、namespace も渡す
 *   timeoutMs: 応答を待つ時間
 *   handshakeTimeoutMs: ハンドシェイクを終えるまで待つ時間
 *   e: テストベクタのためだけに、一時鍵を決めて渡す
 */
class Session extends EventEmitter {
  constructor({ link, role, keys, caps = [], trust = () => false, handlers = {}, timeoutMs = DEF.timeouts.responseMs, handshakeTimeoutMs = DEF.timeouts.handshakeMs, firstMessageMs = DEF.timeouts.firstMessageMs, keepAliveMs = DEF.timeouts.keepAliveMs, e = null }) {
    super();
    if (!['device', 'host'].includes(role)) throw new Error('role は device か host');
    this.link = link;
    this.role = role;
    this.keys = keys;
    this.caps = caps;
    this.trust = trust;
    this.handlers = handlers;
    this.timeoutMs = timeoutMs;
    this.handshakeTimeoutMs = handshakeTimeoutMs;
    this.firstMessageMs = firstMessageMs;
    this.keepAliveMs = keepAliveMs;
    this.fixedE = e;
    this.hs = new Handshake({ initiator: role === 'device', s: keys, prologue: PROLOGUE, e: this.takeE() });
    this.peer = null;
    this.candidate = null;  // UART: 前のセッションと並べて進めている新しいハンドシェイク
    this.ready = false;     // 操作を送れる
    this.secured = false;   // ハンドシェイクを終え、データのフレームを解ける
    this.closed = false;
    this.nextReq = 1;
    this.pending = new Map();   // reqId → { resolve, reject, timer, op }
    this.recent = new Map();    // 時間切れにしたリクエスト ID → 使ってよくなる時刻
    this.timeouts = 0;          // 続けて起きた応答の時間切れの数
    this.inflight = 0;          // 機器: 応答を待っている要求の数
    this.waiters = [];          // 機器: 出す順を待っている要求
    this.partial = new Map();   // 'q:reqId' / 'r:reqId' → { op, chunks, size, kept, over }
    this.partialBytes = 0;      // 組み立て途中のメッセージにためているバイト数
    this.sendQueue = Promise.resolve();
    this.opened = new Promise((resolve, reject) => { this._open = resolve; this._fail = reject; });
    this.opened.catch(() => {});
    link.on('frame', f => this.onFrame(f).catch(err => this.abort(err)));
    link.on('close', () => this.onClose());
    link.on('error', err => this.emit('link-error', err));
  }

  /** 機器: ハンドシェイクを始める。ホスト: 機器の 1 つめのメッセージを待つ。どちらも、使えるようになったら解決する */
  start() {
    this.armHandshakeTimer();
    if (this.role === 'device') this.sendFrame(TYPES.hs1, this.hs.write(Buffer.alloc(0))).catch(err => this.abort(err));
    // TCP のホスト: 接続から決めた時間のうちに 1 つ目のメッセージが来なければ閉じる(規格 4 章)
    if (this.role === 'host' && !this.link.isUart && this.firstMessageMs) {
      this.firstTimer = setTimeout(() => this.abort(fail('first-message-timeout', 'ハンドシェイクの 1 つ目のメッセージが届きませんでした')), this.firstMessageMs);
      if (this.firstTimer.unref) this.firstTimer.unref();
    }
    return this.opened;
  }

  /** 機器(UART): ホストから決めた時間何も届かなければ PING を送る(規格 4 章。その時間切れも、やり直しの 2 度に数える) */
  armKeepAlive() {
    clearTimeout(this.keepTimer);
    if (this.role !== 'device' || !this.link.isUart || !this.keepAliveMs || this.closed) return;
    this.keepTimer = setTimeout(() => {
      if (this.closed) return;
      if (this.ready) this.request('PING').catch(() => {});
      this.armKeepAlive();
    }, this.keepAliveMs);
    if (this.keepTimer.unref) this.keepTimer.unref();
  }

  /** 機器: UART で応答の時間切れが続いたとき、新しいハンドシェイクを始める(規格 4 章) */
  restartDevice({ e = null } = {}) {
    const err = fail('restarted', '新しいハンドシェイクを始めました');
    for (const w of this.pending.values()) { clearTimeout(w.timer); w.reject(err); }
    this.pending.clear();
    this.clearParts();
    this.hs = new Handshake({ initiator: true, s: this.keys, prologue: PROLOGUE, e });
    this.peer = null;
    this.ready = false;
    this.secured = false;
    this.nextReq = 1;
    this.timeouts = 0;
    this.emit('restart');
    if (this.link.resetSeq) this.link.resetSeq();
    this.armHandshakeTimer();
    return this.sendFrame(TYPES.hs1, this.hs.write(Buffer.alloc(0))).catch(err2 => this.abort(err2));
  }

  /** ハンドシェイクが決めた時間に終わらなければ閉じる(途中で止まった相手のために接続を持ち続けない) */
  armHandshakeTimer() {
    clearTimeout(this.handshakeTimer);
    this.handshakeTimer = setTimeout(() => this.abort(fail('handshake-timeout', 'ハンドシェイクが時間内に終わりませんでした')), this.handshakeTimeoutMs);
    if (this.handshakeTimer.unref) this.handshakeTimer.unref();
  }

  /** UART では接続の切れ目が無いので、機器のハンドシェイクの 1 つめが来たら、前のセッションを捨ててやり直す(規格 4 章) */
  restart() {
    const err = fail('restarted', '機器がセッションをやり直しました');
    for (const w of this.pending.values()) { clearTimeout(w.timer); w.reject(err); }
    this.pending.clear();
    this.clearParts();
    this.hs = new Handshake({ initiator: false, s: this.keys, prologue: PROLOGUE, e: this.takeE() });
    this.peer = null;
    this.ready = false;
    this.secured = false;
    this.nextReq = 1;
    this.armHandshakeTimer();
    this.emit('restart');
  }

  /** テストベクタのためだけに決めた一時鍵(関数なら、呼ぶたびに次の鍵を返す)。決めていなければ null(乱数で作る) */
  takeE() { return typeof this.fixedE === 'function' ? this.fixedE() : this.fixedE; }

  get peerMax() { return this.peer ? this.peer.caps.maxFrame : MIN_FRAME; }

  sendFrame(type, payload) {
    const frame = buildFrame(type, payload);
    // ハンドシェイクのフレームは 512 バイトまで(規格 5 章)。ケイパビリティが長すぎる
    if (type !== TYPES.data && type !== TYPES.fail && frame.length > MIN_FRAME) {
      return Promise.reject(fail('handshake-too-big', `ハンドシェイクのフレームが ${frame.length} バイトで、${MIN_FRAME} バイトを超えます`));
    }
    const p = this.sendQueue.then(() => this.link.send(frame));
    this.sendQueue = p.catch(() => {});
    return p;
  }

  /** ハンドシェイクの失敗を知らせて閉じる */
  refuse(code, text) {
    const payload = Buffer.concat([Buffer.from([code]), Buffer.from(text || '', 'utf8')]);
    // 失敗のフレームを送り終えてから閉じる(先に閉じると、相手に理由が届かない)
    this.sendFrame(TYPES.fail, payload).catch(() => {}).finally(() => setTimeout(() => this.link.close(), 50));
    this.abort(fail('refused', `${DEF.fail[code] || code}${text ? ': ' + text : ''}`, { failCode: code }), { keepLink: true });
  }

  async onFrame(buf) {
    if (this.closed) return;
    clearTimeout(this.firstTimer);
    this.armKeepAlive();
    // UART の使用中は、暗号で確かめられないものでセッションを終えない(規格 4 章)
    const guarded = this.ready && this.link.isUart;
    let f;
    try { f = parseFrame(buf); } catch (e) { if (guarded) return; throw e; }
    if (guarded) return this.onGuardedFrame(f);
    if (f.version !== VERSION) return this.refuse(1, `supported=${VERSION}`);
    if (f.type === TYPES.fail) {
      return this.abort(fail('refused-by-peer', `相手が断りました: ${DEF.fail[f.payload[0]] || f.payload[0]} ${f.payload.subarray(1).toString('utf8')}`.trim(), { failCode: f.payload[0] }));
    }
    if (f.type === TYPES.hs1 && this.role === 'host' && this.hs.step !== 0 && this.link.isUart) this.restart();
    if (f.type === TYPES.hs1 && this.role === 'host' && this.hs.step === 0) {
      if (!this.readHandshake(f.payload)) return;
      const hs2 = this.writeHandshake(Buffer.from(R.format(this.caps), 'ascii'));
      if (!hs2) return;
      if (this.link.resetSeq) this.link.resetSeq();
      return this.sendFrame(TYPES.hs2, hs2);
    }
    if (f.type === TYPES.hs2 && this.role === 'device' && this.hs.step === 1) {
      const plain = this.readHandshake(f.payload);
      if (!plain) return;
      if (!await this.admit(plain)) return;
      const hs3 = this.writeHandshake(Buffer.from(R.format(this.caps), 'ascii'));
      if (!hs3) return;
      // 送る前に、データのフレームを解けるようにする(ACK と受け入れの通知が続けて届くことがあるため)。
      // 機器は、ホストの「受け入れた」(リクエスト ID 0 の RESULT)を受けてから使い始める(規格 6 章)
      this.secured = true;
      await this.sendFrame(TYPES.hs3, hs3);
      return;
    }
    if (f.type === TYPES.hs3 && this.role === 'host' && this.hs.step === 2) {
      const plain = this.readHandshake(f.payload);
      if (!plain) return;
      if (!await this.admit(plain)) return;
      this.secured = true;
      this.opened_();
      // 受け入れたことを、最初のデータで知らせる
      return this.respond(OPS.RESULT, 0, [['STATUS', '0']]);
    }
    if (f.type === TYPES.data && this.role === 'device' && !this.ready) {
      // 機器が受け入れの通知を待つ間(規格 4・6 章): UART では、解けないものと、まだ受けられないものを捨てて待つ
      if (!this.secured) { if (this.link.isUart) return; return this.refuse(4, 'ハンドシェイクの途中にデータが来ました'); }
      let msg;
      try { msg = this.hs.recv.decrypt(f.header, f.payload); } catch (e) { if (this.link.isUart) return; throw e; }
      return this.onMessage(msg);
    }
    if (f.type === TYPES.data && this.secured) {
      const msg = this.hs.recv.decrypt(f.header, f.payload);
      return this.onMessage(msg);
    }
    return this.refuse(4, `種類 ${f.type} のフレームをいまは受けられません`);
  }

  async onGuardedFrame(f) {
    if (f.version !== VERSION) return;
    if (f.type === TYPES.data) {
      let msg;
      try { msg = this.hs.recv.decrypt(f.header, f.payload); } catch { return; }
      return this.onMessage(msg);
    }
    if (this.role !== 'host') return;
    if (f.type === TYPES.hs1) return this.beginCandidate(f.payload);
    if (f.type === TYPES.hs3 && this.candidate) return this.finishCandidate(f.payload);
  }

  /** 前のセッションを続けたまま、新しいハンドシェイクを始める */
  async beginCandidate(payload) {
    const hs = new Handshake({ initiator: false, s: this.keys, prologue: PROLOGUE, e: this.takeE() });
    let hs2;
    // 読めない・計算できない(一時鍵が位数の小さい点など)ものは捨て、前のセッションを続ける(規格 4 章)
    try { hs.read(payload); hs2 = hs.write(Buffer.from(R.format(this.caps), 'ascii')); } catch { return; }
    this.candidate = hs;
    clearTimeout(this.candidateTimer);
    this.candidateTimer = setTimeout(() => { this.candidate = null; }, this.handshakeTimeoutMs);
    if (this.candidateTimer.unref) this.candidateTimer.unref();
    if (this.link.resetSeq) this.link.resetSeq();
    await this.sendFrame(TYPES.hs2, hs2).catch(() => {});
  }

  /** 新しいハンドシェイクを終える。相手を受け入れたときだけ、前のセッションと入れ替える */
  async finishCandidate(payload) {
    const hs = this.candidate;
    this.candidate = null;
    clearTimeout(this.candidateTimer);
    const reject = (code, text) => this.sendFrame(TYPES.fail, Buffer.concat([Buffer.from([code]), Buffer.from(text || '', 'utf8')])).catch(() => {});
    let plain;
    try { plain = hs.read(payload); } catch { return reject(2); }
    const r = await this.judge(plain, hs);
    if (r.code) return reject(r.code, r.text);
    const err = fail('restarted', '機器がセッションをやり直しました');
    for (const w of this.pending.values()) { clearTimeout(w.timer); w.reject(err); }
    this.pending.clear();
    this.clearParts();
    this.hs = hs;
    this.peer = r.peer;
    this.nextReq = 1;
    this.emit('restart');
    this.emit('open', this.peer);
    return this.respond(OPS.RESULT, 0, [['STATUS', '0']]).catch(() => {});
  }

  /** ハンドシェイクのメッセージを読む。解けなければ失敗のフレームを送って null を返す */
  readHandshake(payload) {
    try { return this.hs.read(payload); } catch { this.refuse(2); return null; }
  }

  /** ハンドシェイクのメッセージを書く。計算できなければ(X25519 の結果が全て 0 など)失敗のフレームを送って null を返す */
  writeHandshake(payload) {
    try { return this.hs.write(payload); } catch { this.refuse(2); return null; }
  }

  /** ハンドシェイクで知った相手の鍵を、バージョン・拡張・ペアリングで確かめる。よければ { peer }、断るなら { code, text } */
  async judge(plain, hs) {
    let caps;
    // latin1 で読む(ascii は最上位のビットを落とすので、0x80 以上のバイトが別の文字に化け、形の確かめをすり抜ける)
    try { caps = readCaps(R.parse(plain.toString('latin1'))); } catch { return { code: 4, text: 'ケイパビリティの通知の形が正しくありません' }; }
    const bad = capsError(caps);
    if (bad) return { code: 4, text: bad };
    for (const l of DEF.capabilities.required) if (!caps.records.some(r => r[0] === l)) return { code: 4, text: `ケイパビリティの通知に ${l} がありません` };
    if (caps.version.split('.')[0] !== MAJOR) return { code: 1, text: `相手のバージョン ${caps.version}` };
    const mine = readCaps(this.caps);
    // 相手の REQUIRE が自分の EXT に無い、自分の REQUIRE が相手の EXT に無い、相手が自分の EXT に無いものを REQUIRE に書いた(規格 9 章)
    const missing = [...caps.require.filter(n => !mine.ext.includes(n)), ...mine.require.filter(n => !caps.ext.includes(n)), ...caps.require.filter(n => !caps.ext.includes(n))];
    if (missing.length) return { code: 5, text: missing.join(',') };
    const peer = { publicKey: Buffer.from(hs.rs), fingerprint: fingerprint(hs.rs), caps };
    let ok = false;
    try { ok = await this.trust(peer); } catch { ok = false; }
    return ok ? { peer } : { code: 3 };
  }

  async admit(plain) {
    const r = await this.judge(plain, this.hs);
    if (r.code) { this.refuse(r.code, r.text); return false; }
    this.peer = r.peer;
    return true;
  }

  opened_() {
    this.ready = true;
    this.armKeepAlive();
    clearTimeout(this.handshakeTimer);
    this.link.maxIn = Math.max(MIN_FRAME, readCaps(this.caps).maxFrame);
    this._open(this.peer);
    this.emit('open', this.peer);
  }

  // ---- メッセージ ----

  /** メッセージを、相手のフレームの上限に合わせて行の区切りで分けて送る */
  async sendMessage(op, reqId, flags, body) {
    const room = this.peerMax - 2 - HEADER - TAG;
    const lines = body.length ? body.toString('latin1').split(/(?<=\r\n)/) : [];
    const chunks = [];
    let cur = '';
    for (const l of lines) {
      if (cur.length + l.length > room) { chunks.push(cur); cur = ''; }
      cur += l;
    }
    chunks.push(cur);
    for (const [i, c] of chunks.entries()) {
      const head = Buffer.alloc(HEADER);
      head[0] = op; head.writeUInt16BE(reqId, 1); head[3] = flags | (i < chunks.length - 1 ? FLAG.more : 0);
      const plain = Buffer.concat([head, Buffer.from(c, 'latin1')]);
      await this.sendQueue;
      const frameHead = Buffer.from([VERSION, TYPES.data]);
      // 暗号化は送る順に行う(シーケンス番号が順に進むため)
      const p = this.sendQueue.then(() => this.link.send(Buffer.concat([frameHead, this.hs.send.encrypt(frameHead, plain)])));
      this.sendQueue = p.catch(() => {});
      await p;
    }
  }

  onMessage(msg) {
    // 4 バイトに満たない中身は、形の正しくないメッセージとして捨てる(規格 7 章)
    if (msg.length < HEADER) return;
    const op = msg[0], reqId = msg.readUInt16BE(1), flags = msg[3];
    if (!this.ready) {
      // 機器: 受け入れの通知(リクエスト ID 0 の RESULT)を待つ。ほかのデータは捨てる(規格 6 章)
      if (this.role === 'device' && op === OPS.RESULT && reqId === 0 && (flags & FLAG.response)) return this.opened_();
      return;
    }
    const isResp = !!(flags & FLAG.response);
    if (reqId === 0) return;   // リクエスト ID 0 は受け入れの通知だけに使う
    const key = `${isResp ? 'r' : 'q'}:${reqId}`;
    if (isResp) {
      // 分けた応答の続きが届いたら、次のフレームまでの待ちを数え直す(規格 8 章)
      const w = this.pending.get(reqId);
      if (!w) return;
      this.armResponseTimer(reqId, w);
    }
    const part = this.partial.get(key) || { op, chunks: [], size: 0, kept: 0, over: false, bad: false, refused: false };
    if (part.op !== op) part.bad = true;   // 続きのフレームで操作の番号が変わった
    if (!isResp && this.role === 'device' && (flags & FLAG.more) && !part.refused) {
      // 機器への要求は 1 つのフレームに収める。続きのフラグが付いたものには STATUS=2(規格 7 章)
      part.refused = true;
      this.respond(OPS.RESULT, reqId, [['STATUS', '2', 'request in one frame']]).catch(() => {});
    }
    const len = msg.length - HEADER;
    part.size += len;
    // 1 つのメッセージは MAX_BODY まで(規格 7 章)。組み立て途中のメッセージが多くのリクエスト ID にまたがっても、
    // ためる量はセッションごとに PARTIAL_LIMIT までにする(超えた分のメッセージは、大きすぎるものとして扱う)
    if (part.size > MAX_BODY || this.partialBytes + len > PARTIAL_LIMIT) this.dropPart(part);
    if (!part.over) { part.chunks.push(msg.subarray(HEADER)); part.kept += len; this.partialBytes += len; }
    if (flags & FLAG.more) { this.partial.set(key, part); return; }
    this.partial.delete(key);
    this.partialBytes -= part.kept;
    if (part.refused) return;
    if (part.bad) {
      if (!isResp) return this.respond(OPS.RESULT, reqId, [['STATUS', '2', 'op changed']]);
      return;
    }
    if (part.over) {
      if (!isResp) return this.respond(OPS.RESULT, reqId, [['STATUS', '5']]);
      const w = this.pending.get(reqId);
      if (w) { this.pending.delete(reqId); clearTimeout(w.timer); w.reject(fail('too-big', '応答が大きすぎます')); }
      return;
    }
    let records;
    try { records = R.parse(Buffer.concat(part.chunks).toString('latin1')); } catch (e) {
      if (!isResp) return this.respond(OPS.RESULT, reqId, [['STATUS', '2', e.message.replace(/[=;]/g, ' ').slice(0, 60)]]);
      records = [['STATUS', '2', e.message.slice(0, 60)]];
    }
    if (isResp) {
      const w = this.pending.get(reqId);
      if (!w) return;
      this.pending.delete(reqId);
      clearTimeout(w.timer);
      this.timeouts = 0;
      return w.resolve({ op, opName: OP_NAME[op] || op, records, status: statusOf(records) });
    }
    return this.handleRequest(op, reqId, records);
  }

  /** 組み立て途中のメッセージを、大きすぎるものとして捨てる(ためた分を返す) */
  dropPart(part) {
    this.partialBytes -= part.kept;
    part.kept = 0;
    part.chunks = [];
    part.over = true;
  }

  /** 組み立て途中のメッセージを全て捨てる(閉じたとき) */
  clearParts() {
    this.partial.clear();
    this.partialBytes = 0;
  }

  async handleRequest(op, reqId, records) {
    const name = OP_NAME[op];
    const h = name && this.handlers[name];
    const vendor = op >= DEF.message.vendorOpsFrom ? this.vendorRequest(records) : null;
    const fn = vendor ? this.handlers.VENDOR : h;
    let res;
    if (name === 'PING' && !h) res = { records: [] };
    else if (!fn) res = 3;
    else {
      const given = { session: this, records: vendor ? vendor.records : records, reqId, op, peer: this.peer, namespace: vendor ? vendor.namespace : undefined };
      try { res = await fn(given); } catch (e) {
        this.emit('handler-error', e);
        res = { records: [['STATUS', '7', String(e.message).replace(/[=;\r\n]/g, ' ').slice(0, 60)]] };
      }
    }
    if (typeof res === 'number') res = { records: [['STATUS', String(res)]] };
    if (!res || !Array.isArray(res.records)) res = { records: [['STATUS', '0']] };
    // 応答の操作: GET には DATA、TIME・PING には同じ操作、ほかは RESULT(GET で断るときも RESULT)
    const refused = statusOf(res.records) !== null;
    let rop = res.op !== undefined ? OPS[res.op] ?? res.op : !refused && (op === OPS.TIME || op === OPS.PING) ? op : OPS.RESULT;
    if (op === OPS.GET && res.op === undefined) rop = statusOf(res.records) === null ? OPS.DATA : OPS.RESULT;
    return this.respond(rop, reqId, res.records);
  }

  /** 独自の操作の中身を分ける。最初の行が EXT=名前空間 で、その名前空間を両方が宣言していなければ null(知らない操作として答える) */
  vendorRequest(records) {
    const first = records[0];
    if (!first || first[0] !== 'EXT' || !first[1]) return null;
    const mine = this.caps.filter(c => c[0] === 'EXT').flatMap(c => c.slice(1));
    if (!mine.includes(first[1]) || !this.peer.caps.ext.includes(first[1])) return null;
    return { namespace: first[1], records: records.slice(1) };
  }

  respond(op, reqId, records) {
    return this.sendMessage(op, reqId, FLAG.response, Buffer.from(R.format(records), 'ascii'));
  }

  /**
   * 要求を送り、応答を待つ。
   * @returns {Promise<{op, opName, records, status}>} status は STATUS の番号(無ければ null)
   */
  request(opName, records = []) {
    if (this.role !== 'device') return this.request_(opName, records);
    // 機器は、応答を待っている要求がホストの MAXREQ(無ければ 1)に達しているうちは、次の要求を出さない(規格 7 章)
    return new Promise(resolve => {
      const go = () => { this.inflight++; resolve(this.request_(opName, records).finally(() => this.releaseReq())); };
      if (this.inflight < this.maxReqOut()) go(); else this.waiters.push(go);
    });
  }

  maxReqOut() { return this.peer ? this.peer.caps.maxReq : 1; }

  releaseReq() {
    this.inflight--;
    while (this.waiters.length && this.inflight < this.maxReqOut()) this.waiters.shift()();
  }

  /** 使ってよいリクエスト ID(応答を待っているものと、時間切れから決めた時間たっていないものを除く。規格 7 章) */
  takeReqId() {
    const now = Date.now();
    for (const [id, until] of this.recent) if (until <= now) this.recent.delete(id);
    for (let i = 0; i < 0xffff; i++) {
      const id = this.nextReq;
      this.nextReq = this.nextReq === 0xffff ? 1 : this.nextReq + 1;
      if (!this.pending.has(id) && !this.recent.has(id)) return id;
    }
    return null;
  }

  /** 応答の待ち: 要求を送り終えてから、又は応答の前のフレームから数える(規格 8 章) */
  armResponseTimer(reqId, w) {
    clearTimeout(w.timer);
    w.timer = setTimeout(() => this.onResponseTimeout(reqId), this.timeoutMs);
  }

  onResponseTimeout(reqId) {
    const w = this.pending.get(reqId);
    if (!w) return;
    this.pending.delete(reqId);
    const part = this.partial.get(`r:${reqId}`);
    if (part) { this.partialBytes -= part.kept; this.partial.delete(`r:${reqId}`); }
    this.recent.set(reqId, Date.now() + REUSE_MS);
    w.reject(fail('timeout', `${w.opName} の応答がありません`));
    this.timeouts++;
    // 機器: UART で応答の時間切れが 2 度続いたら、新しいハンドシェイクを始める(規格 4 章)
    if (this.role === 'device' && this.link.isUart && this.timeouts >= 2 && !this.closed) this.restartDevice();
  }

  request_(opName, records = []) {
    if (this.closed) return Promise.reject(fail('closed', '接続が切れました'));
    if (!this.ready) return Promise.reject(fail('not-ready', 'まだ使えません'));
    const op = OPS[opName] ?? opName;
    const reqId = this.takeReqId();
    if (reqId === null) return Promise.reject(fail('busy', '使えるリクエスト ID がありません'));
    let w;
    const p = new Promise((resolve, reject) => {
      w = { resolve, reject, timer: null, op, opName };
      this.pending.set(reqId, w);
    });
    this.sendMessage(op, reqId, 0, Buffer.from(R.format(records), 'ascii')).then(() => {
      if (this.pending.get(reqId) === w) this.armResponseTimer(reqId, w);
    }, err => {
      if (this.pending.get(reqId) === w) { this.pending.delete(reqId); clearTimeout(w.timer); w.reject(err); }
    });
    return p;
  }

  abort(err, { keepLink = false } = {}) {
    if (this.closed) return;
    this.closed = true;
    clearTimeout(this.handshakeTimer);
    clearTimeout(this.firstTimer);
    clearTimeout(this.keepTimer);
    this._fail(err);
    for (const w of this.pending.values()) { clearTimeout(w.timer); w.reject(err); }
    this.pending.clear();
    this.clearParts();
    if (this.listenerCount('error')) this.emit('error', err);
    if (!keepLink) try { this.link.close(); } catch { /* 閉じていてもよい */ }
  }

  onClose() {
    if (this.closed) return;
    this.closed = true;
    clearTimeout(this.handshakeTimer);
    const err = fail('closed', '接続が切れました');
    this._fail(err);
    for (const w of this.pending.values()) { clearTimeout(w.timer); w.reject(err); }
    this.pending.clear();
    this.clearParts();
    this.emit('close');
  }

  close() {
    this.closed = true;
    clearTimeout(this.handshakeTimer);
    clearTimeout(this.firstTimer);
    clearTimeout(this.keepTimer);
    clearTimeout(this.candidateTimer);
    const err = fail('closed', '接続を閉じました');
    this._fail(err);
    for (const w of this.pending.values()) { clearTimeout(w.timer); w.reject(err); }
    this.pending.clear();
    this.clearParts();
    this.link.close();
  }
}

/** ケイパビリティの形の誤り(規格 9 章)。よければ null */
function capsError(caps) {
  const repeatable = new Set(DEF.capabilities.repeatable || []);
  const seen = new Set();
  for (const r of caps.records) {
    if (seen.has(r[0]) && !repeatable.has(r[0])) return `${r[0]} が 2 行あります`;
    seen.add(r[0]);
  }
  const v = l => (caps.records.find(r => r[0] === l) || [])[1];
  if (v('VER') !== undefined && !/^\d+\.\d+$/.test(v('VER'))) return `VER の形が正しくありません: ${v('VER')}`;
  if (v('MAXFRAME') !== undefined && !/^\d+$/.test(v('MAXFRAME'))) return `MAXFRAME が数ではありません: ${v('MAXFRAME')}`;
  if (v('MAXREQ') !== undefined && !/^\d+$/.test(v('MAXREQ'))) return `MAXREQ が数ではありません: ${v('MAXREQ')}`;
  return null;
}

/** 応答の STATUS の番号(無ければ null) */
function statusOf(records) {
  const r = records.find(x => x[0] === 'STATUS');
  return r ? Number(r[1]) : null;
}

module.exports = { Session, readCaps, statusOf, OPS };
