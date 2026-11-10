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
const MAJOR = DEF.version.split('.')[0];
const fail = (code, message, extra) => Object.assign(new Error(message), { code }, extra);

/** ケイパビリティの通知([ラベル, 値...] の並び)を、扱いやすい形にする */
function readCaps(records) {
  const v = l => { const r = records.find(x => x[0] === l); return r ? r.slice(1) : null; };
  const all = l => records.filter(x => x[0] === l).flatMap(x => x.slice(1)).filter(Boolean);
  const trace = v('TRACE');
  return {
    version: (v('VER') || [''])[0], dev: (v('DEV') || [''])[0], ven: (v('VEN') || [''])[0], model: (v('MODEL') || [''])[0], sn: (v('SN') || [''])[0],
    maxFrame: Math.min(DEF.frame.maxSize, Math.max(MIN_FRAME, Number((v('MAXFRAME') || [MIN_FRAME])[0]) || MIN_FRAME)),
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
  constructor({ link, role, keys, caps = [], trust = () => false, handlers = {}, timeoutMs = DEF.timeouts.responseMs, handshakeTimeoutMs = DEF.timeouts.handshakeMs, e = null }) {
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
    this.fixedE = e;
    this.hs = new Handshake({ initiator: role === 'device', s: keys, prologue: PROLOGUE, e });
    this.peer = null;
    this.candidate = null;  // UART: 前のセッションと並べて進めている新しいハンドシェイク
    this.ready = false;     // 操作を送れる
    this.secured = false;   // ハンドシェイクを終え、データのフレームを解ける
    this.closed = false;
    this.nextReq = 1;
    this.pending = new Map();   // reqId → { resolve, reject, timer, op }
    this.partial = new Map();   // 'q:reqId' / 'r:reqId' → { op, chunks }
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
    return this.opened;
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
    this.partial.clear();
    this.hs = new Handshake({ initiator: false, s: this.keys, prologue: PROLOGUE, e: this.fixedE });
    this.peer = null;
    this.ready = false;
    this.secured = false;
    this.nextReq = 1;
    this.armHandshakeTimer();
    this.emit('restart');
  }

  get peerMax() { return this.peer ? this.peer.caps.maxFrame : MIN_FRAME; }

  sendFrame(type, payload) {
    const frame = buildFrame(type, payload);
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
      return this.sendFrame(TYPES.hs2, this.hs.write(Buffer.from(R.format(this.caps), 'ascii')));
    }
    if (f.type === TYPES.hs2 && this.role === 'device' && this.hs.step === 1) {
      const plain = this.readHandshake(f.payload);
      if (!plain) return;
      if (!await this.admit(plain)) return;
      await this.sendFrame(TYPES.hs3, this.hs.write(Buffer.from(R.format(this.caps), 'ascii')));
      // 機器は、ホストの「受け入れた」(リクエスト ID 0 の RESULT)を受けてから使い始める(規格 6 章)
      this.secured = true;
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
    const hs = new Handshake({ initiator: false, s: this.keys, prologue: PROLOGUE, e: this.fixedE });
    try { hs.read(payload); } catch { return; }
    this.candidate = hs;
    clearTimeout(this.candidateTimer);
    this.candidateTimer = setTimeout(() => { this.candidate = null; }, this.handshakeTimeoutMs);
    if (this.candidateTimer.unref) this.candidateTimer.unref();
    await this.sendFrame(TYPES.hs2, hs.write(Buffer.from(R.format(this.caps), 'ascii'))).catch(() => {});
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
    this.partial.clear();
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

  /** ハンドシェイクで知った相手の鍵を、バージョン・拡張・ペアリングで確かめる。よければ { peer }、断るなら { code, text } */
  async judge(plain, hs) {
    let caps;
    try { caps = readCaps(R.parse(plain.toString('ascii'))); } catch { return { code: 4, text: 'ケイパビリティの通知の形が正しくありません' }; }
    if (caps.version.split('.')[0] !== MAJOR) return { code: 1, text: `相手のバージョン ${caps.version}` };
    for (const l of DEF.capabilities.required) if (!caps.records.some(r => r[0] === l)) return { code: 4, text: `ケイパビリティの通知に ${l} がありません` };
    const mine = readCaps(this.caps);
    const missing = [...caps.require.filter(n => !mine.ext.includes(n)), ...mine.require.filter(n => !caps.ext.includes(n))];
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
    clearTimeout(this.handshakeTimer);
    this.link.maxIn = Math.max(MIN_FRAME, readCaps(this.caps).maxFrame);
    this._open(this.peer);
    this.emit('open', this.peer);
  }

  // ---- メッセージ ----

  /** メッセージを、相手のフレームの上限に合わせて行の区切りで分けて送る */
  async sendMessage(op, reqId, flags, body) {
    const room = this.peerMax - 2 - HEADER - TAG;
    const lines = body.length ? body.toString('ascii').split(/(?<=\r\n)/) : [];
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
      const plain = Buffer.concat([head, Buffer.from(c, 'ascii')]);
      await this.sendQueue;
      const frameHead = Buffer.from([VERSION, TYPES.data]);
      // 暗号化は送る順に行う(シーケンス番号が順に進むため)
      const p = this.sendQueue.then(() => this.link.send(Buffer.concat([frameHead, this.hs.send.encrypt(frameHead, plain)])));
      this.sendQueue = p.catch(() => {});
      await p;
    }
  }

  onMessage(msg) {
    if (msg.length < HEADER) return this.refuse(4, 'メッセージが短すぎます');
    const op = msg[0], reqId = msg.readUInt16BE(1), flags = msg[3];
    if (!this.ready) {
      // 機器: ハンドシェイクのあとの最初のデータは、ホストの「受け入れた」でなければならない
      if (this.role === 'device' && op === OPS.RESULT && reqId === 0 && (flags & FLAG.response)) return this.opened_();
      return this.refuse(4, '受け入れの知らせの前にデータが来ました');
    }
    const isResp = !!(flags & FLAG.response);
    const key = `${isResp ? 'r' : 'q'}:${reqId}`;
    const part = this.partial.get(key) || { op, chunks: [], size: 0, over: false };
    part.size += msg.length - HEADER;
    if (part.size > MAX_BODY) { part.over = true; part.chunks = []; } else part.chunks.push(msg.subarray(HEADER));
    if (flags & FLAG.more) { this.partial.set(key, part); return; }
    this.partial.delete(key);
    if (part.over) {
      if (!isResp) return this.respond(OPS.RESULT, reqId, [['STATUS', '5']]);
      const w = this.pending.get(reqId);
      if (w) { this.pending.delete(reqId); clearTimeout(w.timer); w.reject(fail('too-big', '応答が大きすぎます')); }
      return;
    }
    let records;
    try { records = R.parse(Buffer.concat(part.chunks).toString('ascii')); } catch (e) {
      if (!isResp) return this.respond(OPS.RESULT, reqId, [['STATUS', '2', e.message.replace(/[=;]/g, ' ').slice(0, 60)]]);
      records = [['STATUS', '2', e.message.slice(0, 60)]];
    }
    if (isResp) {
      const w = this.pending.get(reqId);
      if (!w) return;
      this.pending.delete(reqId);
      clearTimeout(w.timer);
      return w.resolve({ op, opName: OP_NAME[op] || op, records, status: statusOf(records) });
    }
    return this.handleRequest(op, reqId, records);
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
    if (this.closed) return Promise.reject(fail('closed', '接続が切れました'));
    if (!this.ready) return Promise.reject(fail('not-ready', 'まだ使えません'));
    const op = OPS[opName] ?? opName;
    const reqId = this.nextReq;
    this.nextReq = this.nextReq === 0xffff ? 1 : this.nextReq + 1;
    const p = new Promise((resolve, reject) => {
      const timer = setTimeout(() => { this.pending.delete(reqId); reject(fail('timeout', `${opName} の応答がありません`)); }, this.timeoutMs);
      this.pending.set(reqId, { resolve, reject, timer, op });
    });
    this.sendMessage(op, reqId, 0, Buffer.from(R.format(records), 'ascii')).catch(err => {
      const w = this.pending.get(reqId);
      if (w) { this.pending.delete(reqId); clearTimeout(w.timer); w.reject(err); }
    });
    return p;
  }

  abort(err, { keepLink = false } = {}) {
    if (this.closed) return;
    this.closed = true;
    clearTimeout(this.handshakeTimer);
    this._fail(err);
    for (const w of this.pending.values()) { clearTimeout(w.timer); w.reject(err); }
    this.pending.clear();
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
    this.emit('close');
  }

  close() {
    this.closed = true;
    clearTimeout(this.handshakeTimer);
    clearTimeout(this.candidateTimer);
    this._fail(fail('closed', '接続を閉じました'));
    for (const w of this.pending.values()) clearTimeout(w.timer);
    this.pending.clear();
    this.link.close();
  }
}

/** 応答の STATUS の番号(無ければ null) */
function statusOf(records) {
  const r = records.find(x => x[0] === 'STATUS');
  return r ? Number(r[1]) : null;
}

module.exports = { Session, readCaps, statusOf, OPS };
