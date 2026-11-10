'use strict';
// フレーム(規格 5 章)とトランスポート(規格 4 章)。
//   フレーム: [版][種類][中身]
//   TCP: [長さ 2 バイト(上位から)][フレーム]
//   UART: COBS([kind][lseq][フレーム][CRC-32 4 バイト(上位から)]) 0x00。kind 0=フレーム、1=ACK。1 フレームずつ ACK を待つ

const { EventEmitter } = require('node:events');
const DEF = require('../../spec/messages.json');
const COBS = require('./cobs.js');
const { crc32 } = require('./crc32.js');

const VERSION = DEF.frame.version;
const TYPES = DEF.frame.types;
const fail = (code, message) => Object.assign(new Error(message), { code });

function buildFrame(type, payload) { return Buffer.concat([Buffer.from([VERSION, type]), payload]); }
function parseFrame(buf) {
  if (buf.length < 2) throw fail('frame', 'フレームが短すぎます');
  return { version: buf[0], type: buf[1], header: buf.subarray(0, 2), payload: buf.subarray(2) };
}

/**
 * TCP のトランスポート。socket の上でフレームを送り、受けたフレームを 'frame' で知らせる。
 * maxIn: 受けるフレームの上限(これを超える長さが来たら切る)
 */
class TcpLink extends EventEmitter {
  constructor(socket, { maxIn = 65535 } = {}) {
    super();
    this.socket = socket;
    this.maxIn = maxIn;
    let buf = Buffer.alloc(0);
    socket.on('data', chunk => {
      buf = Buffer.concat([buf, chunk]);
      while (buf.length >= 2) {
        const len = buf.readUInt16BE(0);
        if (len > this.maxIn || len < 2) { this.emit('error', fail('frame', `フレームの長さ ${len} は受けられません`)); socket.destroy(); return; }
        if (buf.length < 2 + len) break;
        const frame = buf.subarray(2, 2 + len);
        buf = buf.subarray(2 + len);
        this.emit('frame', Buffer.from(frame));
      }
    });
    socket.on('close', () => this.emit('close'));
    socket.on('error', e => this.emit('error', e));
  }
  send(frame) {
    if (frame.length > 65535) throw fail('frame', 'フレームが大きすぎます');
    const head = Buffer.alloc(2);
    head.writeUInt16BE(frame.length);
    this.socket.write(Buffer.concat([head, frame]));
    return Promise.resolve();
  }
  close() { this.socket.destroy(); }
}

/** UART の 1 つの詰め物: COBS([kind][lseq][body][CRC-32]) と 0x00 */
function uartEncode(kind, lseq, body = Buffer.alloc(0)) {
  const inner = Buffer.concat([Buffer.from([kind, lseq]), body]);
  const crc = Buffer.alloc(4);
  crc.writeUInt32BE(crc32(inner));
  return Buffer.concat([COBS.encode(Buffer.concat([inner, crc])), Buffer.from([0])]);
}
/** 区切りの 0x00 を除いた 1 つを戻す。壊れていれば null */
function uartDecode(chunk) {
  const raw = COBS.decode(chunk);
  if (!raw || raw.length < 6) return null;
  const inner = raw.subarray(0, raw.length - 4);
  if (crc32(inner) !== raw.readUInt32BE(raw.length - 4)) return null;
  return { kind: inner[0], lseq: inner[1], body: Buffer.from(inner.subarray(2)), crc: raw.readUInt32BE(raw.length - 4) };
}

/**
 * UART のトランスポート。stream はバイトの流れ(シリアルの口、またはそれを中継した TCP)。
 * 送る側は 1 フレームずつ ACK を待ち、時間内に来なければ再送する。受ける側は同じ番号の 2 度目を捨てて ACK だけ返す。
 */
class UartLink extends EventEmitter {
  constructor(stream, { ackTimeoutMs = DEF.uart.ackTimeoutMs, retries = DEF.uart.retries, maxIn = 65535, baud = DEF.uart.default.baud } = {}) {
    super();
    this.isUart = true;
    this.stream = stream;
    this.ackTimeoutMs = ackTimeoutMs;
    this.baud = baud;
    this.retries = retries;
    this.maxIn = maxIn;
    this.nextSeq = 0;
    this.lastIn = null;     // 最後に受けたフレームの { lseq, crc }
    this.queue = Promise.resolve();
    this.waiting = null;   // { lseq, resolve }
    let buf = [];
    stream.on('data', chunk => {
      for (const b of chunk) {
        if (b !== 0) { buf.push(b); if (buf.length > this.maxIn * 2 + 16) buf = []; continue; }
        const got = buf.length ? uartDecode(Buffer.from(buf)) : null;
        buf = [];
        if (got) this.onUnit(got);
      }
    });
    stream.on('close', () => this.emit('close'));
    stream.on('error', e => this.emit('error', e));
  }
  onUnit({ kind, lseq, body, crc }) {
    if (kind === DEF.uart.kinds.ack) {
      if (this.waiting && this.waiting.lseq === lseq) { const w = this.waiting; this.waiting = null; w.resolve(); }
      return;
    }
    if (kind !== DEF.uart.kinds.frame) return;
    this.stream.write(uartEncode(DEF.uart.kinds.ack, lseq));
    // 再送された 2 度目。番号だけでなく中身も比べる(機器がやり直すと番号は 0 に戻るが、ハンドシェイクの中身は毎回違う)
    if (this.lastIn && this.lastIn.lseq === lseq && this.lastIn.crc === crc) return;
    this.lastIn = { lseq, crc };
    this.emit('frame', body);
  }
  /** フレームを送る。ACK を受けたら解決する。再送しても届かなければ失敗する */
  send(frame) {
    const run = async () => {
      const lseq = this.nextSeq;
      this.nextSeq = (this.nextSeq + 1) & 0xff;
      const unit = uartEncode(DEF.uart.kinds.frame, lseq, frame);
      for (let i = 0; i <= this.retries; i++) {
        const acked = new Promise(resolve => { this.waiting = { lseq, resolve }; });
        this.stream.write(unit);
        // 待ち時間は送り終えてから数える(ここでは送り終わりが分からないので、線の上にかかる時間を足す)
        const onWire = Math.ceil(unit.length * 10 * 1000 / this.baud);
        let timer;
        const ok = await Promise.race([acked.then(() => true), new Promise(r => { timer = setTimeout(() => r(false), this.ackTimeoutMs + onWire); })]);
        clearTimeout(timer);
        if (ok) return;
      }
      this.waiting = null;
      throw fail('no-ack', `フレームに ACK がありません(${this.retries} 回再送した)`);
    };
    const p = this.queue.then(run);
    this.queue = p.catch(() => {});
    return p;
  }
  close() { if (this.stream.destroy) this.stream.destroy(); }
}

module.exports = { buildFrame, parseFrame, TcpLink, UartLink, uartEncode, uartDecode, TYPES, VERSION };
