'use strict';
// Noise Protocol Framework(第 34 版)の Noise_XX_25519_ChaChaPoly_SHA256(規格 6 章)。
// 暗号の部品(X25519・ChaCha20-Poly1305・SHA-256・HMAC)は Node.js の crypto を使い、手順だけをここで書く。

const crypto = require('node:crypto');

const PROTOCOL = 'Noise_XX_25519_ChaChaPoly_SHA256';
const HASHLEN = 32, DHLEN = 32, TAGLEN = 16;
const MAX_NONCE = 2n ** 64n - 1n;

// ---- X25519(鍵は 32 バイトの生のまま扱う) ----
const PKCS8 = Buffer.from('302e020100300506032b656e04220420', 'hex');
const SPKI = Buffer.from('302a300506032b656e032100', 'hex');
const privKey = raw => crypto.createPrivateKey({ key: Buffer.concat([PKCS8, raw]), format: 'der', type: 'pkcs8' });
const pubKey = raw => crypto.createPublicKey({ key: Buffer.concat([SPKI, raw]), format: 'der', type: 'spki' });

/** 鍵の組を作る。privateKey を渡せば、その秘密鍵から公開鍵を求める */
function keyPair(privateKey) {
  const priv = privateKey ? privKey(Buffer.from(privateKey)) : crypto.generateKeyPairSync('x25519').privateKey;
  const rawPriv = priv.export({ format: 'der', type: 'pkcs8' }).subarray(-32);
  const rawPub = crypto.createPublicKey(priv).export({ format: 'der', type: 'spki' }).subarray(-32);
  return { privateKey: Buffer.from(rawPriv), publicKey: Buffer.from(rawPub) };
}

function dh(kp, publicKey) {
  return crypto.diffieHellman({ privateKey: privKey(kp.privateKey), publicKey: pubKey(publicKey) });
}

// ---- ChaCha20-Poly1305(nonce は 32 ビットの 0 と、64 ビットの小さい順のシーケンス番号) ----
function nonceOf(n) {
  const b = Buffer.alloc(12);
  b.writeBigUInt64LE(n, 4);
  return b;
}
function aeadEncrypt(k, n, ad, plaintext) {
  const c = crypto.createCipheriv('chacha20-poly1305', k, nonceOf(n), { authTagLength: TAGLEN });
  c.setAAD(ad, { plaintextLength: plaintext.length });
  return Buffer.concat([c.update(plaintext), c.final(), c.getAuthTag()]);
}
function aeadDecrypt(k, n, ad, ciphertext) {
  if (ciphertext.length < TAGLEN) throw Object.assign(new Error('暗号文が短すぎます'), { code: 'decrypt' });
  const d = crypto.createDecipheriv('chacha20-poly1305', k, nonceOf(n), { authTagLength: TAGLEN });
  d.setAAD(ad, { plaintextLength: ciphertext.length - TAGLEN });
  d.setAuthTag(ciphertext.subarray(ciphertext.length - TAGLEN));
  try { return Buffer.concat([d.update(ciphertext.subarray(0, ciphertext.length - TAGLEN)), d.final()]); } catch {
    throw Object.assign(new Error('暗号を解けません(改ざん、または鍵の違い)'), { code: 'decrypt' });
  }
}

const hash = data => crypto.createHash('sha256').update(data).digest();
const hmac = (key, data) => crypto.createHmac('sha256', key).update(data).digest();
function hkdf2(ck, ikm) {
  const temp = hmac(ck, ikm);
  const o1 = hmac(temp, Buffer.from([1]));
  const o2 = hmac(temp, Buffer.concat([o1, Buffer.from([2])]));
  return [o1, o2];
}

// ---- CipherState ----
class CipherState {
  constructor(k = null) { this.k = k; this.n = 0n; }
  hasKey() { return this.k !== null; }
  encrypt(ad, plaintext) {
    if (!this.k) return Buffer.from(plaintext);
    if (this.n === MAX_NONCE) throw Object.assign(new Error('シーケンス番号を使い切りました'), { code: 'nonce' });
    const out = aeadEncrypt(this.k, this.n, ad, plaintext);
    this.n++;
    return out;
  }
  decrypt(ad, ciphertext) {
    if (!this.k) return Buffer.from(ciphertext);
    if (this.n === MAX_NONCE) throw Object.assign(new Error('シーケンス番号を使い切りました'), { code: 'nonce' });
    const out = aeadDecrypt(this.k, this.n, ad, ciphertext);
    this.n++;
    return out;
  }
}

// ---- SymmetricState ----
class SymmetricState {
  constructor(name) {
    const n = Buffer.from(name, 'ascii');
    this.h = n.length <= HASHLEN ? Buffer.concat([n, Buffer.alloc(HASHLEN - n.length)]) : hash(n);
    this.ck = Buffer.from(this.h);
    this.cs = new CipherState();
  }
  mixKey(ikm) { const [ck, k] = hkdf2(this.ck, ikm); this.ck = ck; this.cs = new CipherState(k); }
  mixHash(data) { this.h = hash(Buffer.concat([this.h, data])); }
  encryptAndHash(plaintext) { const c = this.cs.encrypt(this.h, plaintext); this.mixHash(c); return c; }
  decryptAndHash(ciphertext) { const p = this.cs.decrypt(this.h, ciphertext); this.mixHash(ciphertext); return p; }
  split() { const [k1, k2] = hkdf2(this.ck, Buffer.alloc(0)); return [new CipherState(k1), new CipherState(k2)]; }
}

/**
 * XX のハンドシェイク。initiator(機器)が 1・3 番目、responder(ホスト)が 2 番目のメッセージを書く。
 *   -> e
 *   <- e, ee, s, es
 *   -> s, se
 * @param {{initiator: boolean, s: {privateKey, publicKey}, prologue?: Buffer, e?: {privateKey, publicKey}}} opts
 *   e はテストベクタのためだけに渡す(ふだんは毎回作る)
 */
class Handshake {
  constructor({ initiator, s, prologue = Buffer.alloc(0), e = null }) {
    this.initiator = initiator;
    this.s = s;
    this.e = null;
    this.fixedE = e;
    this.rs = null;
    this.re = null;
    this.ss = new SymmetricState(PROTOCOL);
    this.ss.mixHash(Buffer.from(prologue));
    this.step = 0;      // 次に扱うメッセージ(0・1・2)
    this.done = false;
  }

  /** 自分の番のメッセージを書く */
  write(payload = Buffer.alloc(0)) {
    const mine = this.initiator ? [0, 2] : [1];
    if (!mine.includes(this.step)) throw new Error('自分の番ではありません');
    const parts = [];
    const ss = this.ss;
    if (this.step === 0) {                       // -> e
      this.e = this.fixedE || keyPair();
      parts.push(this.e.publicKey); ss.mixHash(this.e.publicKey);
    } else if (this.step === 1) {                // <- e, ee, s, es
      this.e = this.fixedE || keyPair();
      parts.push(this.e.publicKey); ss.mixHash(this.e.publicKey);
      ss.mixKey(dh(this.e, this.re));
      parts.push(ss.encryptAndHash(this.s.publicKey));
      ss.mixKey(dh(this.s, this.re));
    } else {                                     // -> s, se
      parts.push(ss.encryptAndHash(this.s.publicKey));
      ss.mixKey(dh(this.s, this.re));
    }
    parts.push(ss.encryptAndHash(Buffer.from(payload)));
    return this.advance(Buffer.concat(parts));
  }

  /** 相手のメッセージを読む。中に入っていた申告(payload)を返す */
  read(message) {
    const theirs = this.initiator ? [1] : [0, 2];
    if (!theirs.includes(this.step)) throw new Error('相手の番ではありません');
    const ss = this.ss;
    let i = 0;
    const take = n => {
      if (message.length < i + n) throw Object.assign(new Error('ハンドシェイクのメッセージが短すぎます'), { code: 'handshake' });
      const b = message.subarray(i, i + n); i += n; return b;
    };
    if (this.step === 0) {
      this.re = Buffer.from(take(DHLEN)); ss.mixHash(this.re);
    } else if (this.step === 1) {
      this.re = Buffer.from(take(DHLEN)); ss.mixHash(this.re);
      ss.mixKey(dh(this.e, this.re));
      this.rs = ss.decryptAndHash(take(DHLEN + TAGLEN));
      ss.mixKey(dh(this.e, this.rs));
    } else {
      this.rs = ss.decryptAndHash(take(DHLEN + TAGLEN));
      ss.mixKey(dh(this.e, this.rs));
    }
    const payload = ss.decryptAndHash(message.subarray(i));
    this.advance(null);
    return payload;
  }

  advance(out) {
    this.step++;
    if (this.step === 3) {
      const [c1, c2] = this.ss.split();
      // c1 は initiator から responder へ、c2 はその逆
      this.send = this.initiator ? c1 : c2;
      this.recv = this.initiator ? c2 : c1;
      this.handshakeHash = this.ss.h;
      this.done = true;
    }
    return out;
  }
}

/** 公開鍵のフィンガープリント: SHA-256 の先頭 10 バイトを Crockford の base32 にし、4 文字ずつ区切る(規格 6 章) */
const CROCKFORD = '0123456789ABCDEFGHJKMNPQRSTVWXYZ';
function fingerprint(publicKey) {
  const b = hash(Buffer.from(publicKey)).subarray(0, 10);
  let bits = 0, value = 0, out = '';
  for (const x of b) {
    value = ((value << 8) | x) & 0xffff; bits += 8;
    while (bits >= 5) { out += CROCKFORD[(value >>> (bits - 5)) & 31]; bits -= 5; }
  }
  return out.match(/.{1,4}/g).join('-');
}

module.exports = { PROTOCOL, Handshake, CipherState, keyPair, dh, fingerprint, aeadEncrypt, aeadDecrypt, hkdf2, TAGLEN, DHLEN };
